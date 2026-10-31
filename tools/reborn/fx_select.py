r"""Picks the effect textures (FX_*.nif in Textures.dv2, left alone by the HD pack) that gain from an AI upscale.

  py -3 -I fx_select.py <Packed dir> <out dir> [threshold]

Most effect sprites are soft glows, smoke and rings: an upscaled gradient is the same gradient. Worth it are the ones
with sharp detail (magic circles, lightning, lava sparks, debris). Every FX texture is decoded (mip 0) and scored by
its sharpness: the share of pixels with a strong luminance step (|Sobel| / 255 > 0.25) among the pixels that are not
empty. Writes <out dir>\fx_scores.csv (all), fx_list.txt (archive names with score >= threshold, default 0.04, plus the rock textures; no masks,
material maps (_NM, _SM, _GM) or textures under 32 pixels) and
fx_top.png / fx_bottom.png (contact sheets of the sharpest and the softest, to check the threshold by eye).
"""
import csv
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'asset_inspector'))

from dv2_inspect import Dv2, parse_texture_nif   # noqa: E402
from dv2_pack import nif_pixel_range              # noqa: E402


def _rgb565(c):
    c = c.astype(np.uint32)
    return np.stack([(c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.int32)


def decode_dxt(data, w, h, fmt):
    """RGBA uint8 (h, w, 4) of mip 0 of a DXT1/DXT5 surface."""
    bw, bh = max(1, w // 4), max(1, h // 4)
    size = 8 if fmt == 'DXT1' else 16
    blocks = np.frombuffer(data, np.uint8, bw * bh * size).reshape(bh * bw, size)
    col = blocks[:, -8:]
    c0 = col[:, 0].astype(np.uint16) | (col[:, 1].astype(np.uint16) << 8)
    c1 = col[:, 2].astype(np.uint16) | (col[:, 3].astype(np.uint16) << 8)
    idx = col[:, 4:8].copy().view('<u4').reshape(-1)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    four = (c0 > c1) | (fmt != 'DXT1')
    pal = np.empty((len(blocks), 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3] = p0, p1
    pal[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal[:, :, 3] = 255
    if fmt == 'DXT1':
        pal[~four, 3, 3] = 0
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    px = np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(4, 2), 1)       # (n, 16, 4)
    if fmt == 'DXT5':
        a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
        ab = np.zeros(len(blocks), np.uint64)
        for k in range(6):
            ab |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
        asel = ((ab[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
        apal = np.empty((len(blocks), 8), np.int32)
        apal[:, 0], apal[:, 1] = a0, a1
        eight = a0 > a1
        for k in range(1, 7):
            apal[:, 1 + k] = np.where(eight, (a0 * (7 - k) + a1 * k) // 7, 0)
        for k in range(1, 5):
            apal[:, 1 + k] = np.where(eight, apal[:, 1 + k], (a0 * (5 - k) + a1 * k) // 5)
        apal[:, 6] = np.where(eight, apal[:, 6], 0)
        apal[:, 7] = np.where(eight, apal[:, 7], 255)
        px[:, :, 3] = np.take_along_axis(apal, asel, 1)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)


def sharpness(img):
    rgb = img[..., :3].astype(np.float32)
    a = img[..., 3].astype(np.float32) / 255.0
    lum = (0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]) * a
    if lum.shape[0] < 3 or lum.shape[1] < 3:
        return 0.0
    gx = (lum[1:-1, 2:] - lum[1:-1, :-2]) * 2 + lum[:-2, 2:] - lum[:-2, :-2] + lum[2:, 2:] - lum[2:, :-2]
    gy = (lum[2:, 1:-1] - lum[:-2, 1:-1]) * 2 + lum[2:, :-2] - lum[:-2, :-2] + lum[2:, 2:] - lum[:-2, 2:]
    g = np.hypot(gx, gy) / (4 * 255.0)
    content = lum[1:-1, 1:-1] > 12
    n = int(content.sum())
    return float(((g > 0.25) & content).sum()) / n if n > 64 else 0.0


def sheet(path, items, tile=128, cols=8):
    rows = (len(items) + cols - 1) // cols
    out = np.zeros((rows * tile, cols * tile, 3), np.uint8)
    for k, (_, img) in enumerate(items):
        h, w = img.shape[:2]
        ys = np.arange(tile) * h // tile
        xs = np.arange(tile) * w // tile
        t = img[ys][:, xs, :3].astype(np.float32) * (img[ys][:, xs, 3:4].astype(np.float32) / 255.0)
        out[(k // cols) * tile:(k // cols + 1) * tile, (k % cols) * tile:(k % cols + 1) * tile] = t.astype(np.uint8)
    raw = b''.join(b'\0' + out[y].tobytes() for y in range(out.shape[0]))

    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', out.shape[1], out.shape[0], 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


SURFACES = ('fx_debries', 'fx_lavastone')     # textures of rock meshes: detailed but low-contrast, the metric misses them


def wanted(r, threshold):
    base = r['name'].replace('\\', '/').rsplit('/', 1)[-1].lower()
    stem = base[:-4] if base.endswith('.nif') else base
    if 'mask' in stem or stem.endswith(('_nm', '_sm', '_gm')):       # masks and material maps: no AI
        return False
    if min(r['width'], r['height']) < 32 or max(r['width'], r['height']) < 64:
        return False
    return r['score'] >= threshold or stem.startswith(SURFACES)


def main(packed, out_dir, threshold=0.04):
    os.makedirs(out_dir, exist_ok=True)
    a = Dv2(os.path.join(packed, 'Textures.dv2'))
    rows, imgs = [], {}
    for e in a.entries:
        base = e.name.replace('\\', '/').rsplit('/', 1)[-1]
        if not base.lower().startswith('fx_') or not base.lower().endswith('.nif'):
            continue
        raw = a.read(e)
        try:
            fmt, start, _ = nif_pixel_range(raw)
            t = parse_texture_nif(raw)
        except ValueError as ex:
            rows.append({'name': e.name, 'error': str(ex)})
            continue
        w, h, _ = t['mips'][0]
        if fmt not in ('DXT1', 'DXT5') or w < 16 or h < 16:
            rows.append({'name': e.name, 'format': fmt, 'width': w, 'height': h, 'error': 'skipped'})
            continue
        img = decode_dxt(raw[start:], w, h, fmt)
        s = sharpness(img)
        imgs[e.name] = img
        rows.append({'name': e.name, 'format': fmt, 'width': w, 'height': h, 'mips': len(t['mips']), 'score': round(s, 4)})
    scored = sorted((r for r in rows if 'score' in r), key=lambda r: -r['score'])
    with open(os.path.join(out_dir, 'fx_scores.csv'), 'w', newline='', encoding='utf-8') as f:
        wr = csv.DictWriter(f, ['name', 'format', 'width', 'height', 'mips', 'score', 'error'])
        wr.writeheader()
        for r in scored + [r for r in rows if 'score' not in r]:
            wr.writerow(r)
    picked = [r for r in scored if wanted(r, threshold)]
    open(os.path.join(out_dir, 'fx_list.txt'), 'w', encoding='utf-8').write(''.join(r['name'] + '\n' for r in picked))
    sheet(os.path.join(out_dir, 'fx_top.png'), [(r['name'], imgs[r['name']]) for r in scored[:48]])
    sheet(os.path.join(out_dir, 'fx_bottom.png'), [(r['name'], imgs[r['name']]) for r in scored[-48:]])
    border = [r for r in scored if threshold * 0.6 <= r['score'] < threshold * 1.6]
    sheet(os.path.join(out_dir, 'fx_border.png'), [(r['name'], imgs[r['name']]) for r in border[:48]])
    print('%d effect textures scored, %d skipped; %d at or above %.3f' % (len(scored), len(rows) - len(scored), len(picked), threshold))
    for q in (0.9, 0.75, 0.5, 0.25):
        print('  score at the %d%% point: %.3f' % (q * 100, scored[int(len(scored) * (1 - q))]['score']))
    print('  near the threshold (fx_border.png): %d' % len(border))


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) == 4 else 0.04)
