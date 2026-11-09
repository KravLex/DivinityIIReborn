r"""08.11.2026: recolour a DXT1 texture NIF to another texture's hue (the user: the Dragon Knight set's gloves stand out).

The game's own data: the arms of the DragonWizard T2 set have a cold blue specular map (*_Arms_*_SM), the torso a gold
one, so the gloves shine silver. Every DXT1 colour endpoint keeps its brightness and takes the mean hue of the target
texture. The block mode is kept: c0 > c1 (4 colours) stays c0 > c1, c0 <= c1 (3 colours + transparent) stays so;
when the new endpoints come out in the other order they are swapped and the indices remapped.

  py -3 -I tex_retint.py <src.nif> <hue.nif> <out.nif>
"""
import struct
import sys
import numpy as np


def _rgb565(v):
    return np.array([(v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31])


def _pack565(c):
    c = np.clip(c, 0, 255)
    return (int(round(c[0] * 31 / 255)) << 11) | (int(round(c[1] * 63 / 255)) << 5) | int(round(c[2] * 31 / 255))


LUM = np.array([0.299, 0.587, 0.114])


def mean_hue(nif):
    """mean colour of a DXT1 NIF's top mip, divided by its brightness (from the block endpoints)"""
    from dv2_pack import nif_pixel_range
    fmt, start, end = nif_pixel_range(nif)
    if fmt != 'DXT1':
        raise SystemExit('hue source is %s, expected DXT1' % fmt)
    acc = np.zeros(3)
    n = 0
    for i in range(start, end - 7, 8 * 7):                   # a sample of the blocks is enough
        c0, c1 = struct.unpack_from('<HH', nif, i)
        acc += _rgb565(c0) + _rgb565(c1)
        n += 2
    m = acc / n
    return m / max(1e-6, float(m @ LUM))


def retint(nif, hue, gain=1.0):
    from dv2_pack import nif_pixel_range
    fmt, start, end = nif_pixel_range(nif)
    if fmt != 'DXT1':
        raise SystemExit('texture is %s, expected DXT1' % fmt)
    out = bytearray(nif)
    swap4 = bytes(((b ^ 0x55) for b in range(256)))           # 0<->1, 2<->3 in every 2-bit index
    swap3 = bytes(((sum(((((b >> s) & 3) ^ 1) if ((b >> s) & 3) < 2 else ((b >> s) & 3)) << s for s in (0, 2, 4, 6)))
                   for b in range(256)))                      # 0<->1, 2 and 3 stay
    for i in range(start, end - 7, 8):
        c0, c1 = struct.unpack_from('<HH', out, i)
        four = c0 > c1
        n0 = _pack565(hue * float(_rgb565(c0) @ LUM) * gain)
        n1 = _pack565(hue * float(_rgb565(c1) @ LUM) * gain)
        idx = out[i + 4:i + 8]
        if four:
            if n0 == n1:                                       # would turn into the 3-colour mode: one solid colour
                idx = b'\0\0\0\0'
                n0 = max(n0, 1); n1 = n0 - 1
            elif n0 < n1:
                n0, n1 = n1, n0
                idx = idx.translate(swap4)
        else:
            if n0 > n1:
                n0, n1 = n1, n0
                idx = idx.translate(swap3)
        struct.pack_into('<HH', out, i, n0, n1)
        out[i + 4:i + 8] = idx
    return bytes(out)


if __name__ == '__main__':
    sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.dirname(
        __import__('os').path.dirname(__import__('os').path.abspath(__file__)))), 'tools', 'reborn'))
    src, hue = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
    open(sys.argv[3], 'wb').write(retint(src, mean_hue(hue)))
