r"""Build the post-processing passes that DXVK patch 0011 (d3d9.postAA) runs after the engine's tonemap pass.

Usage: py -3 -I build_post.py <sources dir: native\shaders> <output dir: native\post-out>

Writes to the output dir:
  smaa_edge.vs.dxso / smaa_edge.ps.dxso, smaa_edge_geo.ps.dxso, smaa_weight.*, smaa_blend.*   from post/smaa.hlsl
  taa.vs.dxso / taa.ps.dxso (DXVK patch 0012, d3d9.postTAA)                                     from post/taa.hlsl
  AreaTex.bin  (160 x 560 A8R8G8B8: area in r and g)                    from third_party/smaa/AreaTex.png
  SearchTex.bin (64 x 16 A8R8G8B8: search value in r)                   from third_party/smaa/SearchTex.png
#include "x" in post/*.hlsl is resolved from post/, third_party/smaa/ and common/.
"""
import os, re, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import d3dsm3  # noqa: E402

PASSES = [('smaa.hlsl', 'smaa_edge', 'EdgeVS', 'EdgePS'), ('smaa.hlsl', 'smaa_edge_geo', None, 'EdgeGeoPS'),
          ('smaa.hlsl', 'smaa_weight', 'WeightVS', 'WeightPS'), ('smaa.hlsl', 'smaa_blend', 'BlendVS', 'BlendPS'),
          ('taa.hlsl', 'taa', 'TaaVS', 'TaaPS')]   # smaa_edge_geo uses the vertex shader of smaa_edge


def read_png(path):
    """8-bit RGB/RGBA/grey or palette (1/2/4/8 bit) PNG without interlacing -> (w, h, rows of (r, g, b, a))."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG: %s' % path)
    pos, idat, palette, trns = 8, b'', None, None
    while pos < len(data):
        n, tag = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif tag == b'PLTE':
            palette = [tuple(body[i:i + 3]) for i in range(0, n, 3)]
        elif tag == b'tRNS':
            trns = body
        elif tag == b'IDAT':
            idat += body
        pos += 12 + n
    if interlace:
        raise ValueError('interlaced PNG not supported')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)
    stride = (w * channels * depth + 7) // 8
    raw = zlib.decompress(idat)
    prev, rows, o = bytearray(stride), [], 0
    for _ in range(h):
        f, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        px = []
        for x in range(w):
            if depth == 8:
                s = line[x * channels:(x + 1) * channels]
            else:
                bit = x * depth
                s = [(line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)]
            if ctype == 3:
                r, g, b_ = palette[s[0]]
                px.append((r, g, b_, trns[s[0]] if trns and s[0] < len(trns) else 255))
            elif ctype == 2:
                px.append((s[0], s[1], s[2], 255))
            elif ctype == 6:
                px.append(tuple(s))
            elif ctype == 0:
                px.append((s[0], s[0], s[0], 255))
            else:
                px.append((s[0], s[0], s[0], s[1]))
        rows.append(px)
    return w, h, rows


def to_bgra(rows):
    return b''.join(bytes((b, g, r, a)) for row in rows for (r, g, b, a) in row)


def resolve(text, dirs):
    def repl(m):
        for d in dirs:
            p = os.path.join(d, m.group(1))
            if os.path.exists(p):
                return open(p, encoding='utf-8').read()
        raise FileNotFoundError(m.group(1))
    for _ in range(8):
        text, n = re.subn(r'^#include "([^"]+)"\s*$', repl, text, flags=re.M)
        if not n:
            break
    return text


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    src, out = argv[1], argv[2]
    os.makedirs(out, exist_ok=True)
    dirs = [os.path.join(src, d) for d in ('post', os.path.join('third_party', 'smaa'), 'common')]
    for source, name, vs, ps in PASSES:
        text = resolve(open(os.path.join(src, 'post', source), encoding='utf-8').read(), dirs)
        for entry, target, kind in ((vs, 'vs_3_0', 'vs'), (ps, 'ps_3_0', 'ps')):
            if entry is None:
                continue
            code, log = d3dsm3.compile(text, entry, target, name=source)
            open(os.path.join(out, '%s.%s.dxso' % (name, kind)), 'wb').write(code)
            slots = re.search(r'approximately (\d+) instruction slots', d3dsm3.disassemble(code))
            print('OK   %-18s %-8s %5d bytes, %s slots' % (name + '.' + kind, entry, len(code), slots.group(1) if slots else '?'))
    for name, size in (('AreaTex', (160, 560)), ('SearchTex', (64, 16))):
        w, h, rows = read_png(os.path.join(src, 'third_party', 'smaa', name + '.png'))
        if (w, h) != size:
            raise ValueError('%s is %dx%d, expected %dx%d' % (name, w, h, size[0], size[1]))
        open(os.path.join(out, name + '.bin'), 'wb').write(to_bgra(rows))
        used = sorted({p[0] for row in rows for p in row})
        print('OK   %-18s %dx%d, red values %d..%d (%d distinct)' % (name + '.bin', w, h, used[0], used[-1], len(used)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
