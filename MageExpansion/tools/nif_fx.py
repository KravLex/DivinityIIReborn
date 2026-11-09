r"""Gamebryo 20.3.0.9 effect files (.item = one NIF with the scene and the .kf sequence): read, recolour, rename textures.

  py -3 -I nif_fx.py dump <file.item>        header, blocks, texture names and every colour this module can change

Header (Gamebryo 20.3.0.9, little endian, as in the game's files): text line, u32 version, u8 endian, u32 user version,
u32 block count, u16 type count + types (u32 length + text), u16 type index per block, u32 size per block, u32 string
count, u32 longest string, strings (u32 length + text), u32 group count + groups; then the blocks. Texture file names
(NiSourceTexture) are header strings, so a rename to a name of the same length changes no offset.
Colours that are changed (layouts from the public NIF format description for 20.3, checked by the sizes of the
blocks in the game's files - every parse is checked against the block size):
  NiColorData            key group: u32 count, u32 interpolation (1 linear, 2 quadratic, 3 TBC, 5 constant), keys
                         {f32 time, rgba; quadratic: + forward rgba + backward rgba; TBC: + 3 f32}
  NiMaterialProperty     NiObjectNET (string, u32 extra count + refs, ref) then ambient, diffuse, specular, emissive
                         rgb, glossiness, alpha
  NiPSysBoxEmitter etc.  NiPSysModifier (string, u32 order, ref, u8 active), 6 f32, initial colour rgba, ...
  NiPosData              key group of xyz (u32 count, u32 interpolation, {f32 time, xyz[, +fwd +bwd | +3 f32]});
                         only when every NiPosData of the file is the data of a NiPoint3Interpolator and the file has
                         as many NiMaterialColorControllers (animated material colours, as in the game's fire effects)
  NiPoint3Interpolator   xyz value + data ref (same condition; the "no value" marker -FLT_MAX is left alone)
Texture recolouring works on the DXT colour endpoints (no recompression): DXT5 colour blocks are always 4-colour;
for DXT1 the block mode (c0 > c1) is kept by swapping the endpoints and remapping the indices.
"""
import struct
import sys

EMITTERS = ('NiPSysBoxEmitter', 'NiPSysSphereEmitter', 'NiPSysCylinderEmitter', 'NiPSysMeshEmitter')


class Nif:
    def __init__(self, data):
        self.data = bytearray(data)
        d = self.data
        p = d.index(b'\n') + 1
        self.version, = struct.unpack_from('<I', d, p)
        if self.version != 0x14030009:
            raise ValueError('not a 20.3.0.9 file: %08X' % self.version)
        p += 4 + 1 + 4
        nblocks, = struct.unpack_from('<I', d, p); p += 4
        ntypes, = struct.unpack_from('<H', d, p); p += 2
        self.types = []
        for _ in range(ntypes):
            ln, = struct.unpack_from('<I', d, p); self.types.append(d[p + 4:p + 4 + ln].decode('latin1')); p += 4 + ln
        tidx = struct.unpack_from('<%dH' % nblocks, d, p); p += 2 * nblocks
        sizes = struct.unpack_from('<%dI' % nblocks, d, p); p += 4 * nblocks
        nstr, _ = struct.unpack_from('<II', d, p); p += 8
        self.strings = []                    # (offset of the text, length, text)
        for _ in range(nstr):
            ln, = struct.unpack_from('<I', d, p)
            self.strings.append((p + 4, ln, d[p + 4:p + 4 + ln].decode('latin1')))
            p += 4 + ln
        ngroups, = struct.unpack_from('<I', d, p); p += 4 + 4 * ngroups
        self.blocks = []                     # (type, offset, size)
        for i in range(nblocks):
            self.blocks.append((self.types[tidx[i] & 0x7FFF], p, sizes[i]))
            p += sizes[i]
        self.end = p

    def texture_names(self):
        names = set()
        for t, off, size in self.blocks:
            if t == 'NiSourceTexture':
                # NiObjectNET: name, extra count + refs, controller; then u8 use external, string index file name
                q = off + 4
                n, = struct.unpack_from('<I', self.data, q); q += 4 + 4 * n + 4
                if self.data[q]:
                    si, = struct.unpack_from('<i', self.data, q + 1)
                    if 0 <= si < len(self.strings):
                        names.add(self.strings[si][2])
        return sorted(names)

    def rename_string(self, old, new):
        if len(old) != len(new):
            raise ValueError('rename must keep the length: %r -> %r' % (old, new))
        n = 0
        for off, ln, text in self.strings:
            if text == old:
                self.data[off:off + ln] = new.encode('latin1')
                n += 1
        return n

    def colours(self):
        """[(block type, block index, offset of an rgb triple)] for every colour this module changes."""
        out = []
        d = self.data
        for bi, (t, off, size) in enumerate(self.blocks):
            if t == 'NiColorData':
                count, = struct.unpack_from('<I', d, off)
                q = off + 4
                if count:
                    interp, = struct.unpack_from('<I', d, q); q += 4
                    per = {1: 20, 5: 20, 2: 52, 3: 32}.get(interp)
                    if per is None or 8 + count * per != size:
                        raise ValueError('NiColorData %d: interpolation %d, %d keys, size %d' % (bi, interp, count, size))
                    for k in range(count):
                        out.append((t, bi, q + k * per + 4))
                        if interp == 2:
                            out.append((t, bi, q + k * per + 20))
                            out.append((t, bi, q + k * per + 36))
            elif t == 'NiMaterialProperty':
                q = off + 4
                n, = struct.unpack_from('<I', d, q); q += 4 + 4 * n + 4
                if q + 56 != off + size:
                    raise ValueError('NiMaterialProperty %d: size %d' % (bi, size))
                for k in range(4):
                    out.append((t, bi, q + 12 * k))
            elif t in EMITTERS:
                out.append((t, bi, off + 4 + 4 + 4 + 1 + 24))
        out += self._material_animation()
        return out

    def _material_animation(self):
        types = [t for t, _, _ in self.blocks]
        npos, nint, nctl = types.count('NiPosData'), types.count('NiPoint3Interpolator'), types.count('NiMaterialColorController')
        if not npos or not (npos == nint == nctl):
            return []
        d, out = self.data, []
        for bi, (t, off, size) in enumerate(self.blocks):
            if t == 'NiPoint3Interpolator':
                if size != 16:
                    raise ValueError('NiPoint3Interpolator %d: size %d' % (bi, size))
                if max(abs(v) for v in struct.unpack_from('<3f', d, off)) < 1e30:
                    out.append((t, bi, off))
            elif t == 'NiPosData':
                count, = struct.unpack_from('<I', d, off)
                if count:
                    interp, = struct.unpack_from('<I', d, off + 4)
                    per = {1: 16, 5: 16, 2: 40, 3: 28}.get(interp)
                    if per is None or 8 + count * per != size:
                        raise ValueError('NiPosData %d: interpolation %d, %d keys, size %d' % (bi, interp, count, size))
                    for k in range(count):
                        out.append((t, bi, off + 8 + k * per + 4))
        return out

    def tint(self, matrix):
        """Apply a 3x3 matrix to every coloured value (clamped to 0..1). Neutral values (grey, white, black and the
        -1 / 1.004 of the material animations) are left exactly as they are. Returns the number of changed values."""
        d, n = self.data, 0
        for _, _, q in self.colours():
            rgb = struct.unpack_from('<3f', d, q)
            if max(rgb) - min(rgb) < 0.02:
                continue
            new = [min(1.0, max(0.0, sum(matrix[r][c] * rgb[c] for c in range(3)))) for r in range(3)]
            struct.pack_into('<3f', d, q, *new)
            n += 1
        return n


def _rgb565(v):
    return (v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31


def _tint565(v, matrix):
    """Tinted RGB565 endpoint; near-grey endpoints (smoke, white cores) stay as they are."""
    rgb = _rgb565(v)
    if max(rgb) - min(rgb) < 24:
        return v
    r, g, b = (min(255.0, max(0.0, sum(matrix[i][c] * rgb[c] for c in range(3)))) for i in range(3))
    return (round(r * 31 / 255) << 11) | (round(g * 63 / 255) << 5) | round(b * 31 / 255)


def tint_dxt(data, start, end, fmt, matrix):
    """Tint the colour endpoints of the DXT blocks in data[start:end] (a bytearray) in place."""
    if fmt == 'DXT5':
        for i in range(start + 8, end, 16):
            c0, c1 = struct.unpack_from('<HH', data, i)
            struct.pack_into('<HH', data, i, _tint565(c0, matrix), _tint565(c1, matrix))
        return
    if fmt != 'DXT1':
        raise ValueError('unsupported texture format %s' % fmt)
    for i in range(start, end, 8):
        c0, c1, idx = struct.unpack_from('<HHI', data, i)
        n0, n1 = _tint565(c0, matrix), _tint565(c1, matrix)
        four = c0 > c1
        if four:
            if n0 == n1:                                   # keep 4-colour mode: endpoints must differ
                if n0:
                    n1 = n0 - 1
                else:
                    n0 = 1
            if n0 < n1:
                n0, n1 = n1, n0
                idx ^= 0x55555555                          # 0<->1, 2<->3
        elif n0 > n1:                                      # 3 colours + transparent: swap 0<->1 only
            n0, n1 = n1, n0
            idx = sum((((v ^ 1) if v < 2 else v) << (2 * k)) for k in range(16) for v in [(idx >> (2 * k)) & 3])
        struct.pack_into('<HHI', data, i, n0, n1, idx)


def main(argv):
    if len(argv) == 3 and argv[1] == 'dump':
        n = Nif(open(argv[2], 'rb').read())
        print('blocks %d, end %d of %d bytes' % (len(n.blocks), n.end, len(n.data)))
        counts = {}
        for t, _, _ in n.blocks:
            counts[t] = counts.get(t, 0) + 1
        print('types:', ', '.join('%s x%d' % kv for kv in sorted(counts.items())))
        print('textures:', n.texture_names())
        for t, bi, q in n.colours():
            print('  %-20s block %3d @%06X rgb %s' % (t, bi, q, ' '.join('%.3f' % v for v in struct.unpack_from('<3f', n.data, q))))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
