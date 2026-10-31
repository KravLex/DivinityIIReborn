"""Divinity II .dv2 archive / texture-NIF inspector (stdlib only, read-only).

Formats (cross-checked against kilazz/D2DCTool src/dv2_archive.rs and src/dds/converter.rs, and against real files):
  .dv2  : u32 version (4|5); v5: 8 bytes pad; u8,u8 flags; u32 data_start; u32 names_size; names (NUL separated,
          backslashes); u32 count; count x {u32 offset (rel. data_start), u32 stored_size, u32 raw_size (0 = stored raw)}.
          Each compressed entry is one complete zlib stream.
  .nif  : "Gamebryo File Format, Version 20.3.0.9\\n", standalone texture wrapper with one
          NiPersistentSrcTextureRendererData block (pixel format, mip table, pixel data).

Commands:
  py -3 -I dv2_inspect.py list    <archive.dv2>                 entries of one archive
  py -3 -I dv2_inspect.py scan    <Packed dir> <out.csv>        every texture NIF in every archive -> CSV + summary
  py -3 -I dv2_inspect.py extract <archive.dv2> <name> <out>    one entry, decompressed
Nothing is ever written into the game folder.
"""
import csv
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

PIXEL_FORMATS = {0: "RGB", 1: "RGBA", 2: "PAL", 3: "PALA", 4: "DXT1", 5: "DXT3", 6: "DXT5", 7: "RGB24NONINT",
                 8: "BUMP", 9: "BUMPLUMA", 10: "RENDERSPEC", 11: "1CH", 12: "2CH", 13: "3CH", 14: "4CH",
                 15: "DEPTH_STENCIL", 16: "UNKNOWN"}
BLOCK_BYTES = {"DXT1": 8, "DXT3": 16, "DXT5": 16}


class Entry:
    __slots__ = ("name", "offset", "stored", "raw")

    def __init__(self, name, offset, stored, raw):
        self.name, self.offset, self.stored, self.raw = name, offset, stored, raw

    @property
    def size(self):
        return self.raw or self.stored


class Dv2:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        f = self.f
        ver = struct.unpack("<I", f.read(4))[0]
        if ver not in (4, 5):
            raise ValueError(f"{path}: unsupported dv2 version {ver}")
        self.version = ver
        if ver == 5:
            f.read(8)
        f.read(2)
        self.data_start, names_size = struct.unpack("<II", f.read(8))
        names = [n for n in f.read(names_size).split(b"\0") if n]
        count = struct.unpack("<I", f.read(4))[0]
        table = f.read(12 * count)
        self.entries = []
        for i in range(min(count, len(names))):
            off, stored, raw = struct.unpack_from("<III", table, 12 * i)
            self.entries.append(Entry(names[i].decode("utf-8", "replace"), off, stored, raw))
        self.declared = count

    def read(self, e, limit=None):
        """Return the entry's bytes; with limit, only the first `limit` decompressed bytes (cheap header peek)."""
        self.f.seek(self.data_start + e.offset)
        if not e.raw:
            return self.f.read(e.stored if limit is None else min(limit, e.stored))
        if limit is None:
            return zlib.decompress(self.f.read(e.stored))
        d = zlib.decompressobj()
        out = b""
        left = e.stored
        while len(out) < limit and left > 0:
            chunk = self.f.read(min(16384, left))
            left -= len(chunk)
            out += d.decompress(chunk, limit - len(out))
            if d.eof:
                break
        return out


def parse_texture_nif(buf):
    """Parse the header of a standalone texture NIF. Returns dict or raises ValueError."""
    nl = buf.find(b"\n")
    if nl < 0 or not buf.startswith(b"Gamebryo File Format"):
        raise ValueError("not a Gamebryo NIF")
    hdr = buf[:nl].decode("ascii", "replace")
    p = nl + 1
    version, = struct.unpack_from("<I", buf, p); p += 4
    p += 1  # endian
    p += 4  # user version
    nblocks, = struct.unpack_from("<I", buf, p); p += 4
    ntypes, = struct.unpack_from("<H", buf, p); p += 2
    types = []
    for _ in range(ntypes):
        ln, = struct.unpack_from("<I", buf, p); p += 4
        types.append(buf[p:p + ln].decode("ascii", "replace")); p += ln
    idx = struct.unpack_from(f"<{nblocks}H", buf, p); p += 2 * nblocks
    sizes = struct.unpack_from(f"<{nblocks}I", buf, p); p += 4 * nblocks
    nstr, _maxlen = struct.unpack_from("<II", buf, p); p += 8
    for _ in range(nstr):
        ln, = struct.unpack_from("<I", buf, p); p += 4 + ln
    ngroups, = struct.unpack_from("<I", buf, p); p += 4 + 4 * ngroups
    blk = p
    for i in range(nblocks):
        tname = types[idx[i] & 0x7FFF]
        if "SrcTextureRendererData" in tname or tname == "NiPixelData":
            break
        blk += sizes[i]
    else:
        return {"header": hdr, "version": version, "types": "|".join(types), "pixel_format": None}
    # D2DCTool's writer emits "three u32 zeros" before the block: those are the header's num_strings,
    # max_string_len and num_groups, already consumed above - the block starts with the pixel format.
    q = blk
    pf, = struct.unpack_from("<I", buf, q)
    q += 4 + 59
    nmips, bpp = struct.unpack_from("<II", buf, q); q += 8
    if nmips > 24:
        raise ValueError(f"bad mip count {nmips}")
    mips = [struct.unpack_from("<III", buf, q + 12 * i) for i in range(nmips)]
    q += 12 * nmips
    npix, = struct.unpack_from("<I", buf, q)
    faces = 1
    if tname == "NiPersistentSrcTextureRendererData":
        _pad, faces, _plat = struct.unpack_from("<III", buf, q + 4)
    return {"header": hdr, "version": version, "types": "|".join(types), "block": tname,
            "pixel_format": PIXEL_FORMATS.get(pf, str(pf)), "mips": mips, "bytes_per_pixel": bpp,
            "pixel_bytes": npix, "faces": faces}


DDS_FOURCC = {b"DXT1": "DXT1", b"DXT3": "DXT3", b"DXT5": "DXT5", b"ATI1": "BC4", b"ATI2": "BC5", b"DX10": "DX10"}


def parse_dds(buf):
    """Header of a raw .dds file, returned in the same shape as parse_texture_nif."""
    if buf[:4] != b"DDS " or len(buf) < 128:
        raise ValueError("not a DDS")
    h, w, pitch, _depth, nmips = struct.unpack_from("<IIIII", buf, 12)
    pf_flags, = struct.unpack_from("<I", buf, 80)
    fourcc = buf[84:88]
    bits, = struct.unpack_from("<I", buf, 88)
    caps2, = struct.unpack_from("<I", buf, 112)
    fmt = DDS_FOURCC.get(fourcc, fourcc.decode("latin1")) if pf_flags & 4 else f"RGB{bits}"
    nmips = max(1, nmips)
    mips, mw, mh, off = [], w, h, 0
    for _ in range(nmips):
        mips.append((mw, mh, off))
        off += level_bytes(fmt, mw, mh) or mw * mh * max(1, bits // 8)
        mw, mh = max(1, mw // 2), max(1, mh // 2)
    faces = 6 if caps2 & 0x200 else 1
    return {"pixel_format": fmt, "mips": mips, "pixel_bytes": off, "faces": faces, "block": "DDS"}


def expected_mips(w, h):
    n = 1
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        n += 1
    return n


def level_bytes(fmt, w, h):
    if fmt in BLOCK_BYTES:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK_BYTES[fmt]
    return None


def check(t):
    """Validation issues for a parsed texture."""
    issues = []
    mips = t["mips"]
    if not mips:
        return ["no mip table"]
    w, h, _ = mips[0]
    if w & (w - 1) or h & (h - 1):
        issues.append("non-power-of-two")
    if len(mips) == 1 and (w > 64 or h > 64):
        issues.append("no-mipmaps")
    elif len(mips) < expected_mips(w, h):
        issues.append(f"partial-mips({len(mips)}/{expected_mips(w, h)})")
    pw, ph = w, h
    for i, (mw, mh, off) in enumerate(mips[1:], 1):
        if (mw, mh) != (max(1, pw // 2), max(1, ph // 2)):
            issues.append(f"bad-mip-dims@{i}")
            break
        pw, ph = mw, mh
    fmt = t["pixel_format"]
    calc = sum(level_bytes(fmt, mw, mh) or 0 for mw, mh, _ in mips)
    if fmt in BLOCK_BYTES and calc != t["pixel_bytes"]:
        issues.append(f"size-mismatch(calc {calc} vs {t['pixel_bytes']})")
    if max(w, h) > 8192:
        issues.append("over-8192")
    return issues


def tex_kind(name):
    """Texture role from Larian's file-name suffix convention (DM diffuse, NM normal, ... - heuristic)."""
    stem = os.path.splitext(os.path.basename(name))[0].upper()
    if stem.startswith("ICON_"):
        return "icon"
    suf = stem.rsplit("_", 1)[-1]
    return {"DM": "diffuse", "NM": "normal", "SM": "specular", "GM": "glow", "HM": "height", "CM": "color",
            "DTLM": "detail", "DRKM": "dark", "LOD": "lod", "GLOW": "glow"}.get(suf, "other")


def cmd_list(path):
    a = Dv2(path)
    print(f"{path}: v{a.version}, {len(a.entries)} entries (declared {a.declared}), data at {a.data_start:#x}")
    for e in a.entries:
        print(f"{e.size:>12} {e.stored:>12} {e.name}")


def cmd_extract(path, name, out):
    a = Dv2(path)
    key = name.replace("/", "\\").lower()
    for e in a.entries:
        if e.name.lower() == key:
            with open(out, "wb") as f:
                f.write(a.read(e))
            print(f"wrote {out} ({e.size} bytes)")
            return
    print("not found:", name)


def archive_order(names):
    """Override order used for the 'effective' view: base archives first, then Patch*, DKS_Patch*, FOV_Patch*;
    the later archive wins on duplicate paths.
    Verified: Divinity2.exe v1.4.700.56 has a hard-coded archive table (.rdata, file+0xf615d8) listing DKS_Patch[_2.._4],
    FOV_Patch[_2.._4], Patch[_2.._4] before the base archives. NOT verified: whether the first or the last match wins
    between two patch archives - only 9 paths (icons) are duplicated across patch archives, so the view barely depends
    on it."""
    def key(n):
        b = os.path.basename(n).lower()
        rank = 3 if b.startswith("fov_patch") else 2 if b.startswith("dks_patch") else 1 if b.startswith("patch") else 0
        return (rank, b)
    return sorted(names, key=key)


def cmd_scan(packed, out_csv):
    archives = []
    for root, _, files in os.walk(packed):
        archives += [os.path.join(root, f) for f in files if f.lower().endswith(".dv2")]
    archives = archive_order(archives)
    rows = []
    for ap in archives:
        try:
            a = Dv2(ap)
        except Exception as ex:  # noqa: BLE001 - report and continue
            print(f"!! {ap}: {ex}")
            continue
        rel = os.path.relpath(ap, packed)
        ntex = 0
        for e in a.entries:
            low = e.name.lower()
            if not low.endswith((".nif", ".dds")):
                continue
            try:
                # by magic, not extension: terrain MegaTexture_*.dds are NIF texture wrappers
                head = a.read(e, 65536)
                t = parse_dds(head) if head[:4] == b"DDS " else parse_texture_nif(head)
            except Exception as ex:  # noqa: BLE001
                rows.append({"archive": rel, "name": e.name, "file_bytes": e.size, "error": str(ex)[:80]})
                continue
            if t["pixel_format"] is None:
                continue  # a model/scene NIF, not a texture wrapper
            w, h, _ = t["mips"][0] if t["mips"] else (0, 0, 0)
            rows.append({"archive": rel, "name": e.name, "file_bytes": e.size, "stored_bytes": e.stored,
                         "format": t["pixel_format"], "width": w, "height": h, "mips": len(t["mips"]),
                         "faces": t["faces"], "pixel_bytes": t["pixel_bytes"], "kind": tex_kind(e.name),
                         "issues": ";".join(check(t)), "error": ""})
            ntex += 1
        print(f"{rel}: {len(a.entries)} entries, {ntex} texture NIFs")
    cols = ["archive", "name", "kind", "format", "width", "height", "mips", "faces", "pixel_bytes", "file_bytes",
            "stored_bytes", "issues", "error"]
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, cols)
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, "") for c in cols})
    summarize(rows)


def summarize(rows):
    ok = [r for r in rows if not r.get("error")]
    eff = {}
    for r in ok:  # rows are in archive override order: later wins
        eff[r["name"].lower()] = r
    print(f"\nTexture NIFs: {len(ok)} parsed, {len(rows) - len(ok)} unparsed; unique paths (effective): {len(eff)}")
    for label, data in (("all archives", ok), ("effective set", list(eff.values()))):
        print(f"\n== {label}")
        print("  formats:", dict(Counter(r["format"] for r in data)))
        sizes = Counter(max(r["width"], r["height"]) for r in data)
        print("  max side:", dict(sorted(sizes.items())))
        mb = sum(r["pixel_bytes"] * r["faces"] for r in data) / 2**20
        print(f"  total pixel data: {mb:,.0f} MB")
        iss = Counter(i.split("(")[0].split("@")[0] for r in data for i in r["issues"].split(";") if i)
        print("  issues:", dict(iss))
    by_arch = defaultdict(lambda: [0, 0, 0])
    for r in ok:
        s = by_arch[r["archive"]]
        s[0] += 1
        s[1] += r["pixel_bytes"]
        s[2] = max(s[2], r["width"], r["height"])
    print("\n== per archive: textures / pixel MB / max side")
    for k, (n, b, m) in by_arch.items():
        print(f"  {k:45} {n:6} {b / 2**20:9,.0f} {m:6}")
    eff_v = list(eff.values())
    big = sorted(eff_v, key=lambda r: -r["pixel_bytes"])[:10]
    print("\n== largest effective textures")
    for r in big:
        print(f"  {r['pixel_bytes'] / 2**20:7.1f} MB {r['width']}x{r['height']} {r['format']} {r['name']} [{r['archive']}]")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "list":
        cmd_list(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "scan":
        cmd_scan(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 5 and sys.argv[1] == "extract":
        cmd_extract(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
