r"""Dump Divinity II skill data as readable XML + the exe's skill-name -> skill-id table (read-only).

Usage: py -3 -I skills_dump.py <game dir> <out dir>

What it does
  1. Finds the *effective* copy of every skill-related file the way the game resolves it: Packed\Reborn\*.dv2
     (Div2Fix overlay, registered first), then the 12 patch slots (DKS_Patch, FOV_Patch, Patch, each _2.._4),
     then the main archives. First registered wins (docs/OVERLAY.md, checked in game 31.10).
  2. Decodes the Gamebryo binary XML (tools/reborn/mindread/d2dom.py). Element/attribute names are stored as
     hashes only; names are recovered by hashing every string of Divinity2.exe and Divinity2-debug.exe plus
     Param0..Param39 (the generic skill parameters).
  3. Parses the skill-name lookup at 0x89D560 (CodePath/UUID string -> skill id) into skill_ids.txt.
Nothing is written into the game folder.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'asset_inspector'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'reborn', 'mindread'))
import d2dom          # noqa: E402
import dv2_inspect    # noqa: E402

FILES = [
    r'Episodes\Episode_1_Extended\rpgstats_skillprototypes.xml',
    r'Episodes\Episode_2\rpgstats_skillprototypes.xml',
    r'Episodes\Episode_1_Extended\rpgstats_skilltree.xml',
    r'Episodes\Episode_2\rpgstats_skilltree.xml',
    r'Episodes\Episode_1_Extended\DefaultSkills.xml',
    r'Episodes\Episode_2\DefaultSkills.xml',
    r'Episodes\Episode_1_Extended\rpgstats_skilltrainerlists.xml',
    r'Episodes\Episode_1_Extended\rpgstats_formulas.xml',
    r'rpgstats_effectprototypes.xml',
    r'Localisation\Russian\Skills_Localisation.xml',
]
SLOTS = ['DKS_Patch', 'FOV_Patch', 'Patch']
LOOKUP_VA = 0x89D560
LOOKUP_HEAD = bytes.fromhex('568B742408')        # push esi; mov esi,[esp+8]


def archive_order(packed, skip=()):
    """Archives in the order the game registers them (first wins); file names in `skip` are left out."""
    order = []
    reborn = os.path.join(packed, 'Reborn')
    if os.path.isdir(reborn):
        order += sorted(os.path.join(reborn, f) for f in os.listdir(reborn)
                        if f.lower().endswith('.dv2') and f.lower() not in skip)
    for s in SLOTS:
        for suf in ('', '_2', '_3', '_4'):
            order.append(os.path.join(packed, s + suf + '.dv2'))
    order += sorted(os.path.join(packed, f) for f in os.listdir(packed)
                    if f.lower().endswith('.dv2') and os.path.join(packed, f) not in order)
    return [p for p in order if os.path.isfile(p)]


def effective(arcs, want):
    """(archive, entry) of the copy of `want` the game uses, or None."""
    for a in arcs:
        for e in a.entries:
            if e.name.lower() == want.lower():
                return a, e
    return None


def name_table(bindir):
    names = {}
    for exe in ('Divinity2.exe', 'Divinity2-debug.exe'):
        p = os.path.join(bindir, exe)
        if not os.path.isfile(p):
            continue
        d = open(p, 'rb').read()
        words = set()
        for m in re.finditer(rb'[\x20-\x7e]{2,80}', d):
            s = m.group().decode('latin1')
            words.add(s)
            words.update(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', s))
        for w in words:
            names[d2dom.name_hash(w)] = w
    for i in range(40):
        names[d2dom.name_hash('Param%d' % i)] = 'Param%d' % i
    return names


def pe_sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec, optsz = struct.unpack_from('<HH', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 52)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
        secs.append((base + va, max(vsz, rsz), raw))
    return base, secs


def va2off(secs, va):
    for sva, size, raw in secs:
        if sva <= va < sva + size:
            return raw + va - sva
    raise ValueError(hex(va))


def skill_ids(exe):
    """push str; mov ecx,esi; call cmp; test al,al; je; mov eax,imm32 -> (id, name). Last entry uses sbb."""
    d = open(exe, 'rb').read()
    _, secs = pe_sections(d)
    o = va2off(secs, LOOKUP_VA)
    if d[o:o + 5] != LOOKUP_HEAD:
        raise SystemExit('0x89D560: unexpected bytes %s (wrong exe version?)' % d[o:o + 5].hex())
    out = []
    pat = re.compile(rb'\x68(.{4})\x8B\xCE\xE8.{4}(?:\x84\xC0\x74.\xB8(.{4})|\x0F\xB6\xC0\xF7\xD8\x1B\xC0\x83\xE0(.))', re.S)
    for m in pat.finditer(d, o, o + 0x900):
        sva = struct.unpack('<I', m.group(1))[0]
        so = va2off(secs, sva)
        name = d[so:d.index(b'\0', so)].decode('latin1')
        # last entry: neg; sbb; and eax,X; dec eax -> X-1 on match, -1 otherwise
        sid = struct.unpack('<I', m.group(2))[0] if m.group(2) else m.group(3)[0] - 1
        out.append((sid, name))
        if m.group(3):
            break
    return out


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    game, out = argv[1], argv[2]
    packed = os.path.join(game, 'Data', 'Win32', 'Packed')
    os.makedirs(out, exist_ok=True)
    names = name_table(os.path.join(game, 'bin'))
    arcs = [dv2_inspect.Dv2(p) for p in archive_order(packed)]
    for want in FILES:
        hit = effective(arcs, want)
        if not hit:
            print('missing:', want)
            continue
        a, e = hit
        root = d2dom.parse(a.read(e), names)
        text = '\n'.join(d2dom.to_xml(root))
        dst = os.path.join(out, want.replace('\\', '__').replace('Episodes__', ''))
        open(dst, 'w', encoding='utf-8').write(text)
        print('%-62s <- %-18s unknown names: %d' % (want, os.path.basename(a.path),
                                                   len(set(re.findall(r'H_[0-9A-F]{8}', text)))))
    ids = skill_ids(os.path.join(game, 'bin', 'Divinity2.exe'))
    with open(os.path.join(out, 'skill_ids.txt'), 'w', encoding='utf-8') as f:
        f.write('# Divinity2.exe 0x89D560: skill name -> skill id (-1 = unknown name)\n')
        for sid, name in sorted(ids):
            f.write('0x%02X  %s\n' % (sid, name))
    used = {s for s, _ in ids}
    print('skill ids: %d names, max 0x%X, unused below max: %s' % (
        len(ids), max(used), ' '.join('0x%X' % i for i in range(1, max(used)) if i not in used)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
