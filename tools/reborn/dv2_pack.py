r"""Builds Divinity II .dv2 archives (v5) for the Reborn overlay (Data\Win32\Packed\Reborn\*.dv2, docs/OVERLAY.md).

  py -3 -I dv2_pack.py pack <source dir> <out.dv2>              every file under <source dir>; entry names are the
                                                                relative paths with backslashes (e.g. GUI\Win32\x.dds)
  py -3 -I dv2_pack.py verify <archive.dv2> <source dir>        reads the archive back and compares with the files
  py -3 -I dv2_pack.py menutest <Packed dir> <out.dv2>          overlay test: the main menu textures (GUI\Win32\
                                                                mainmenu_*.dds) as the game shows them now (patch slot
                                                                first), red and blue swapped
  py -3 -I dv2_pack.py terraintest <Packed dir> <out.dv2>       overlay test for the region archives (World\...): every
                                                                terrain colour MegaTexture, red and blue swapped

Layout as in the game's and the HD pack's archives (checked on GUI.dv2, DKS_Patch.dv2, FOV_Patch_2.dv2):
  u32 5, u32 1, u32 4, u8 0, u8 1 | u32 data_start | u32 names_size | names (each NUL-terminated) | u32 count |
  count x {u32 offset from data_start, u32 stored size, u32 raw size} | data
Entries are single zlib streams (level 9, 78 DA), each starting on a 2048-byte boundary; data_start is 32768 when the
table fits (as in the game's archives), else the next 2048 boundary. Nothing is written into the game folder by
pack/verify; menutest only reads it.
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'asset_inspector'))

ALIGN = 2048
PATCH_SLOTS = ['DKS_Patch', 'DKS_Patch_2', 'DKS_Patch_3', 'DKS_Patch_4', 'FOV_Patch', 'FOV_Patch_2', 'FOV_Patch_3',
               'FOV_Patch_4', 'Patch', 'Patch_2', 'Patch_3', 'Patch_4']


def align(n, a):
    return (n + a - 1) // a * a


def pack(entries, out):
    """entries: list of (name with backslashes, bytes)."""
    names = b''.join(n.encode('latin1') + b'\0' for n, _ in entries)
    table_end = 22 + len(names) + 4 + 12 * len(entries)
    data_start = 32768 if table_end <= 32768 else align(table_end, ALIGN)
    blobs, table, off = [], [], 0
    for _, raw in entries:
        z = zlib.compress(raw, 9)
        table.append(struct.pack('<III', off, len(z), len(raw)))
        blobs.append(z)
        off = align(off + len(z), ALIGN)
    with open(out, 'wb') as f:
        f.write(struct.pack('<III', 5, 1, 4) + bytes([0, 1]))
        f.write(struct.pack('<II', data_start, len(names)) + names)
        f.write(struct.pack('<I', len(entries)) + b''.join(table))
        f.write(b'\0' * (data_start - f.tell()))
        for z in blobs:
            f.write(z)
            f.write(b'\0' * (align(f.tell() - data_start, ALIGN) - (f.tell() - data_start)))
    return data_start


def read_back(path):
    from dv2_inspect import Dv2
    a = Dv2(path)
    if a.version != 5 or a.declared != len(a.entries):
        raise SystemExit('%s: version %d, %d names for %d entries' % (path, a.version, len(a.entries), a.declared))
    return {e.name: a.read(e) for e in a.entries}


def files_under(src):
    out = []
    for root, _, files in os.walk(src):
        for fn in files:
            p = os.path.join(root, fn)
            out.append((os.path.relpath(p, src).replace('/', '\\'), open(p, 'rb').read()))
    return sorted(out, key=lambda e: e[0].lower())


def verify(entries, path):
    back = read_back(path)
    bad = [n for n, raw in entries if back.get(n) != raw]
    if bad or len(back) != len(entries):
        raise SystemExit('verify FAILED: %d of %d entries differ, %d in the archive' % (len(bad), len(entries), len(back)))
    print('verify OK: %d entries, %d bytes of data' % (len(entries), sum(len(r) for _, r in entries)))


# --- overlay test: red and blue swapped in a DDS without recompressing ---------------------------------------------

def _swap565(c):
    return ((c & 0x1F) << 11) | (c & 0x07E0) | (c >> 11)


def _swap_color_block(b, i, dxt1):
    c0, c1, idx = struct.unpack_from('<HHI', b, i)
    n0, n1 = _swap565(c0), _swap565(c1)
    if dxt1 and (c0 > c1) != (n0 > n1) and n0 != n1:
        # keep the block's mode (4 colours or 3 + transparent): swap the endpoints and remap the indices
        n0, n1 = n1, n0
        if c0 > c1:
            idx ^= 0x55555555                                  # 0<->1, 2<->3
        else:
            idx = sum(((v ^ 1) if v < 2 else v) << (2 * k) for k in range(16) for v in [(idx >> (2 * k)) & 3])
    struct.pack_into('<HHI', b, i, n0, n1, idx)


def swap_red_blue(dds):
    b = bytearray(dds)
    if b[:4] != b'DDS ':
        raise ValueError('not a DDS')
    flags, four = struct.unpack_from('<I4s', b, 80)
    if flags & 4:
        if four not in (b'DXT1', b'DXT3', b'DXT5'):
            raise ValueError('unsupported DDS format %r' % four)
        dxt1 = four == b'DXT1'
        size = 8 if dxt1 else 16
        for i in range(128, len(b) - size + 1, size):
            _swap_color_block(b, i + (0 if dxt1 else 8), dxt1)
    else:
        bits, rm, gm, bm = struct.unpack_from('<IIII', b, 88)
        if bits != 32 or rm != 0x00FF0000 or bm != 0x000000FF:
            raise ValueError('unsupported DDS pixel format (%d bits, masks %08X %08X)' % (bits, rm, bm))
        for i in range(128, len(b) - 3, 4):
            b[i], b[i + 2] = b[i + 2], b[i]
    return bytes(b)


def nif_pixel_range(b):
    """(format, start, end) of the pixel data of a standalone texture NIF (NiPersistentSrcTextureRendererData or
    NiPixelData): the pixels are the last field of the block, the 8-byte footer follows."""
    from dv2_inspect import parse_texture_nif
    t = parse_texture_nif(b)
    nl = b.find(b'\n')
    p = nl + 1 + 4 + 1 + 4
    nblocks, = struct.unpack_from('<I', b, p); p += 4
    ntypes, = struct.unpack_from('<H', b, p); p += 2
    for _ in range(ntypes):
        ln, = struct.unpack_from('<I', b, p); p += 4 + ln
    p += 2 * nblocks
    sizes = struct.unpack_from('<%dI' % nblocks, b, p); p += 4 * nblocks
    nstr, _ = struct.unpack_from('<II', b, p); p += 8
    for _ in range(nstr):
        ln, = struct.unpack_from('<I', b, p); p += 4 + ln
    ng, = struct.unpack_from('<I', b, p); p += 4 + 4 * ng
    if nblocks != 1 or p + sizes[0] + 8 != len(b):
        raise ValueError('unexpected NIF layout (%d blocks)' % nblocks)
    end = p + sizes[0]
    return t['pixel_format'], end - t['pixel_bytes'] * t['faces'], end


def nif_swap_red_blue(b):
    fmt, start, end = nif_pixel_range(b)
    if fmt not in ('DXT1', 'DXT5'):
        raise ValueError('unsupported pixel format %s' % fmt)
    out = bytearray(b)
    dxt1 = fmt == 'DXT1'
    size = 8 if dxt1 else 16
    for i in range(start, end - size + 1, size):
        _swap_color_block(out, i + (0 if dxt1 else 8), dxt1)
    return bytes(out)


def terraintest(packed, out):
    """Overlay test for the region archives: every terrain colour MegaTexture (not _NM) with red and blue swapped."""
    from dv2_inspect import Dv2
    picked = {}
    for root, _, files in os.walk(packed):
        for fn in sorted(files):
            if not fn.lower().endswith('.dv2') or 'patch' in fn.lower():
                continue
            a = Dv2(os.path.join(root, fn))
            for e in a.entries:
                low = e.name.lower()
                base = low.rsplit('\\', 1)[-1]
                if base.startswith('megatexture_') and not base.startswith('megatexture_nm') and low not in picked:
                    raw = a.read(e)
                    picked[low] = (e.name, swap_red_blue(raw) if raw[:4] == b'DDS ' else nif_swap_red_blue(raw))
    entries = sorted(picked.values(), key=lambda e: e[0].lower())
    regions = sorted({n.split('\\')[1] for n, _ in entries if n.lower().startswith('world\\')})
    print('%d terrain colour textures, regions: %s' % (len(entries), ', '.join(regions)))
    pack(entries, out)
    verify(entries, out)


def menutest(packed, out):
    from dv2_inspect import Dv2
    picked = {}
    for slot in PATCH_SLOTS + ['GUI']:                         # what the game shows now: the patch slot copy first
        a = Dv2(os.path.join(packed, slot + '.dv2'))
        for e in a.entries:
            low = e.name.lower()
            if low.startswith('gui\\win32\\mainmenu_') and low.endswith('.dds') and low not in picked:
                picked[low] = (e.name, swap_red_blue(a.read(e)), slot)
    entries = sorted(((n, raw) for n, raw, _ in picked.values()), key=lambda e: e[0].lower())
    for n, _, slot in sorted(picked.values()):
        print('  %-40s from %s' % (n, slot))
    pack(entries, out)
    verify(entries, out)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'pack':
        ents = files_under(sys.argv[2])
        pack(ents, sys.argv[3])
        verify(ents, sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'verify':
        verify(files_under(sys.argv[3]), sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == 'menutest':
        menutest(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'terraintest':
        terraintest(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
