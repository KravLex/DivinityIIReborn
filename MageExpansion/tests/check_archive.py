r"""Offline check of the built overlay archive against the files the game uses now.

  py -3 -I check_archive.py <game dir> <MageExpansion.dv2> [<png out>]

For every XML in the archive: decode ours and the game's effective copy (our own archive excluded) and diff them as
text. Passes only when every difference is an added line and the original lines all remain in order. Icons: same
size/format as their source; with <png out> the three states are written side by side for a visual check.
"""
import difflib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'reborn'))
import d2xml                                   # noqa: E402 - first: puts tools/reborn/mindread on the path
import d2dom                                   # noqa: E402
import skills_dump                             # noqa: E402
import build_mage                              # noqa: E402
from dv2_inspect import Dv2, parse_texture_nif  # noqa: E402


COUNTERS = re.compile(r' (Size|size|m_Size|m_TotalSize|Animations)="\d+"')


def text(data, names):
    return d2dom.to_xml(d2xml.Doc(data, names).root)


def counters_hold(data, names):
    """Every counter of the effect lists matches its children: Size/size = number of children, m_TotalSize = sum of the
    children's m_TotalSize. Returns (number of counters checked, list of wrong ones)."""
    wrong, n = [], 0
    stack = [d2xml.Doc(data, names).root]
    while stack:
        node = stack.pop()
        stack.extend(node.children)
        for k, v in node.attrs:
            if k in ('Size', 'size') and node.children:
                n += 1
                if int(v) != len(node.children):
                    wrong.append('%s %s=%s, %d children' % (node.name, k, v, len(node.children)))
            elif k == 'm_TotalSize' and node.children:
                n += 1
                total = sum(int(c.get('m_TotalSize') or 0) for c in node.children) + int(node.get('m_Size') or 0)
                if int(v) != total:
                    wrong.append('%s m_TotalSize=%s, children %d' % (node.get('m_ID'), v, total))
    return n, wrong


def balance_check(name, orig, mine, names):
    """Differences allowed by config/balance.json: in the prototypes, every original skill is still there in order;
    a skill not in balance.json is unchanged, one in it keeps its structure and differs only in Description, RequiredLevel,
    EnduranceCost, CooldownTime and parameter values of ranks 1-15. In the localisation, every original line is
    still there in order and only the translations of the balanced skills' old descriptions differ."""
    import json
    bal = json.load(open(os.path.join(os.path.dirname(HERE), 'config', 'balance.json'), encoding='utf-8'))['skills']
    a, b = d2xml.Doc(orig, names).root, d2xml.Doc(mine, names).root
    problems = []
    if name.lower().endswith('skills_localisation.xml'):
        ru_only = {x.get('desc_ru') for x in bal.values() if 'desc_ru' in x and 'desc_en' not in x} | {x['name_ru'] for x in bal.values() if 'name_ru' in x}
        for x, y in zip(a.children, b.children):
            xs, ys = [c.text for c in x.children], [c.text for c in y.children]
            if xs != ys and not (xs[0] == ys[0] and xs[2] == ys[2] and ys[1] in ru_only):
                problems.append('line %r changed' % xs[0][:40])
        return problems
    sa, sb = a.find('Skills').children, b.find('Skills').children
    for x, y in zip(sa, sb):
        uuid = x.get('UUID')
        if y.get('UUID') != uuid:
            problems.append('order changed at %r' % uuid)
            continue
        if uuid not in bal:
            if d2dom.to_xml(x) != d2dom.to_xml(y):
                problems.append('%r changed but is not in balance.json' % uuid)
            continue
        if len(x.children) != len(y.children):
            problems.append('%r: rank count changed' % uuid)
            continue
        for lx, ly in zip(x.children, y.children):
            ka = [k for k, _ in lx.attrs]
            if ka != [k for k, _ in ly.attrs] or len(lx.children) != len(ly.children):
                problems.append('%r rank %s: structure changed' % (uuid, lx.get('Level')))
                continue
            free = 1 <= int(lx.get('Level')) <= 15
            for (k, v), (_, w) in zip(lx.attrs, ly.attrs):
                if v != w and not (free and str(k) in ('Description', 'EnduranceCost', 'CooldownTime', 'RequiredLevel')):
                    problems.append('%r rank %s: %s changed' % (uuid, lx.get('Level'), k))
            for px, py in zip(lx.children, ly.children):
                if [k for k, _ in px.attrs] != [k for k, _ in py.attrs] or (not free and px.attrs != py.attrs):
                    problems.append('%r rank %s: parameters changed' % (uuid, lx.get('Level')))
    if len(sb) < len(sa):
        problems.append('skills removed')
    return problems


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__)
        return 2
    game, arc = argv[1], argv[2]
    names = skills_dump.name_table(os.path.join(game, 'bin'))
    ours = Dv2(arc)
    base = [Dv2(p) for p in skills_dump.archive_order(os.path.join(game, 'Data', 'Win32', 'Packed'),
                                                       skip=(os.path.basename(arc).lower(), 'magedata.dv2', 'mageexpansion.dv2'))]
    bad, pngs, dds_areas = 0, [], []
    for e in ours.entries:
        mine = ours.read(e)
        if e.name.lower().endswith('.xml'):
            hit = skills_dump.effective(base, e.name)
            orig = hit[0].read(hit[1])
            a, b = text(orig, names), text(mine, names)
            added, removed = [], []
            for line in difflib.ndiff(a, b):
                if line.startswith('+ '):
                    added.append(line[2:].strip())
                elif line.startswith('- '):
                    removed.append(line[2:].strip())
            # a removed line is a changed counter when an added line differs from it only in Size/size/m_TotalSize
            plain_added = {COUNTERS.sub('', x) for x in added}
            counters = [x for x in removed if COUNTERS.search(x) and COUNTERS.sub('', x) in plain_added]
            other = [x for x in removed if x not in counters]
            note = ''
            if counters:
                # the counters must still match their children, if they did in the original
                n0, wrong0 = counters_hold(orig, names)
                n1, wrong1 = counters_hold(mine, names)
                if not wrong0 and wrong1:
                    other += wrong1
                note = ', %d counter(s) updated, %s' % (len(counters), 'consistent' if not wrong1 else
                                                       'inconsistent as in the original' if wrong0 else 'WRONG')
            if other and e.name.lower().endswith('rpgstats_skilltrainerlists.xml'):
                # config/balance.json trainer_remove: exactly those purchases may go
                import json
                drop = {(s, str(c)) for s, c in json.load(open(os.path.join(os.path.dirname(HERE), 'config', 'balance.json'),
                                                              encoding='utf-8')).get('trainer_remove', {}).get('rows', [])}
                gone = [x for x in other if (re.search(r' Skill="([^"]*)"', x) and
                        (re.search(r' Skill="([^"]*)"', x).group(1), (re.search(r' NewMaxLevel="(\d+)"', x) or [0, ''])[1]) in drop)]
                note += ', %d purchase(s) removed by balance.json' % len(gone)
                other = [x for x in other if x not in gone]
            if other and e.name.lower().endswith('rpgstats_itemprototypes.xml'):
                # 07.11 config/balance.json limbs: only the Value of the creature parts' stat modifiers may change
                val = re.compile(r' Value="[^"]*"')
                mod = re.compile(r'^<Modifier Type="(HP|MeleeDamage|MagicDamage)PercentileMods" ')
                ok_lines = (all(mod.search(x) for x in other) and all(mod.search(x) for x in added) and
                            len(added) == len(other) and
                            sorted(val.sub('', x) for x in other) == sorted(val.sub('', x) for x in added))
                note += ', %d creature part modifier(s) changed by balance.json limbs%s' % (len(other), '' if ok_lines else ' - WRONG')
                if ok_lines:
                    other = []
            if other and e.name.lower().endswith('rpgstats_itemmagicaleffects.xml'):
                # 08.11 config/boosters.json allow_extra: the game's skill boosters may only get more AllowUse letters
                allow = re.compile(r' AllowUse="([^"]*)"')
                freq = re.compile(r' Frequency="[^"]*"')     # 08.11 boosters.json base_freq
                strip = lambda x: freq.sub('', allow.sub('', x))
                pool = {}
                for x in added:
                    pool.setdefault(strip(x), []).append(x)
                matched = []
                for x in other:
                    cand = pool.get(strip(x)) or []
                    a0 = (allow.search(x) or [0, ''])[1]
                    found = next((y for y in cand if set(a0) <= set((allow.search(y) or [0, ''])[1])), None)
                    if found is not None:
                        cand.remove(found)
                        matched.append(x)
                note += ', %d skill booster(s) allowed on more item kinds / reweighted (boosters.json allow_extra, base_freq)' % len(matched)
                other = [x for x in other if x not in matched]
            if other and (e.name.lower().endswith('rpgstats_skillprototypes.xml') or
                          e.name.lower().endswith('skills_localisation.xml')):
                # config/balance.json changes the game's own skills: the changed lines are checked node by node
                problems = balance_check(e.name, orig, mine, names)
                note += ', balance.json changes %s' % ('only where allowed' if not problems else 'WRONG: ' + '; '.join(problems[:5]))
                if not problems:
                    other = []
            if e.name.lower().endswith('rpgstats_skilltree.xml') and other:   # 09.11: only the game skills of config tree_moves may change cell
                import json, re as _re
                cfgp = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'config', 'skills.json')
                moves = {m['uuid']: m['tree'] for m in json.load(open(cfgp, encoding='utf-8')).get('tree_moves', [])}
                def uuid_of(x):
                    m = _re.search(r'SkillUUID="([^"]+)"', x)
                    return m.group(1) if m else None
                moved = [x for x in other if uuid_of(x) in moves]
                good = all(any(uuid_of(a) == uuid_of(x) and 'X="%d"' % moves[uuid_of(x)][0] in a and 'Y="%d"' % moves[uuid_of(x)][1] in a for a in added) for x in moved)
                if len(moved) == len(other) and len(moved) == len(moves) and good:
                    other = []
            ok = not other and added
            bad += not ok
            print('%-4s %s: +%d lines, -%d lines%s (from %s)' % ('OK' if ok else 'FAIL', e.name, len(added), len(removed),
                                                               note, os.path.basename(hit[0].path)))
            for line in added:
                print('       + ' + line[:150])
            for line in removed:
                print('       - ' + line[:150])
        elif e.name.lower().endswith('textures.bin'):
            hit = skills_dump.effective(base, e.name)
            orig = hit[0].read(hit[1])
            n0, n1 = struct.unpack_from('<I', orig)[0], struct.unpack_from('<I', mine)[0]
            ok = mine[4:len(orig)] == orig[4:] and n1 >= n0
            added, p = [], len(orig)
            while ok and p < len(mine):
                ln = struct.unpack_from('<I', mine, p)[0]
                added.append(mine[p + 4:p + 4 + ln].decode('latin1'))
                p += 4 + ln
            ok = ok and p == len(mine) and len(added) == n1 - n0
            bad += not ok
            print('%-4s %s: %d -> %d entries, original kept as a prefix (from %s)' % ('OK' if ok else 'FAIL', e.name, n0, n1,
                                                                                 os.path.basename(hit[0].path)))
            for n in added:
                print('       + ' + n)
        elif e.name.lower().endswith('.item'):
            import nif_fx
            n = nif_fx.Nif(mine)
            refs = [t.rsplit('.', 1)[0].lower() + '.nif' for t in n.texture_names()]
            in_archive = {x.name.lower().rsplit('\\', 1)[-1] for x in ours.entries}
            # a texture is ours (renamed copies) or the game's own (03.11: Chain Lightning's beam keeps the tower's frames)
            missing = [r for r in refs if r not in in_archive and not skills_dump.effective(base, r'Win32\Textures' + '\\' + r)]
            ok = n.end + 8 == len(mine) and not missing
            bad += not ok
            print('%-4s %s: %d blocks, textures %s%s' % ('OK' if ok else 'FAIL', e.name, len(n.blocks), ', '.join(refs),
                                                      ' MISSING ' + ', '.join(missing) if missing else ''))
        elif e.name.lower().endswith('.dds'):
            hit = skills_dump.effective(base, e.name)
            orig = hit[0].read(hit[1])
            h, w = struct.unpack_from('<II', orig, 12)
            changed = [i for i in range(128, len(orig), 16) if orig[i:i + 16] != mine[i:i + 16]]
            ok = len(mine) == len(orig) and mine[:128] == orig[:128] and changed
            bad += not ok
            bx = [(i - 128) // 16 % (w // 4) for i in changed]
            by = [(i - 128) // 16 // (w // 4) for i in changed]
            box = (min(bx) * 4, min(by) * 4, max(bx) * 4 + 4, max(by) * 4 + 4) if changed else None
            area = 'x %d..%d y %d..%d' % (box[0], box[2], box[1], box[3]) if box else 'none'
            print('%-4s %s: %dx%d, %d of %d blocks changed, area %s (from %s)' % (
                'OK' if ok else 'FAIL', e.name, w, h, len(changed), (len(orig) - 128) // 16, area,
                os.path.basename(hit[0].path)))
            if box:
                dds_areas.append((e.name, orig, mine, w, h, box))
        elif e.name.lower().endswith('.kfm'):
            # the game's KFM plus our sequences only: every old sequence kept (transitions may only gain entries)
            import kfm
            hit = skills_dump.effective(base, e.name)
            old, new = kfm.Kfm(hit[0].read(hit[1])), kfm.Kfm(mine)
            olds = {q['id']: q for q in old.seqs}
            added = [q for q in new.seqs if q['id'] not in olds]
            kept = all(q['id'] in olds and q['file'] == olds[q['id']]['file'] and q['index'] == olds[q['id']]['index']
                       and q['trans'][:len(olds[q['id']]['trans'])] == olds[q['id']]['trans']
                       for q in new.seqs if q['id'] in olds)
            ok = kept and len(new.seqs) == len(old.seqs) + len(added) and new.tail == old.tail and added
            bad += not ok
            print('%-4s %s: %d -> %d sequences, added %s (from %s)' % ('OK' if ok else 'FAIL', e.name, len(old.seqs),
                  len(new.seqs), ', '.join('%d %s' % (q['id'], q['file']) for q in added), os.path.basename(hit[0].path)))
        elif e.name.lower().endswith('.kf'):
            import nif_write
            n = nif_write.Nif(mine)
            seqs = [b for b in n.blocks if b.type == 'NiControllerSequence']
            # Standalone effect KF files contain one sequence.  Player base KFs
            # may contain the game's existing sequences plus our appended one.
            # Validate both forms without rejecting the latter as a malformed KF.
            ok = n.encode() == mine and bool(seqs)
            if '\\Characters\\HumanFemale\\Player_F_Base.kf'.lower() in e.name.lower() or \
                    '\\Characters\\HumanMale\\Player_M_Base.kf'.lower() in e.name.lower():
                hit = skills_dump.effective(base, e.name)
                old = nif_write.Nif(hit[0].read(hit[1])) if hit else None
                old_names = [old.strings[b['name']] for b in old.blocks if b.type == 'NiControllerSequence'] if old else []
                new_names = [n.strings[b['name']] for b in seqs]
                ok = ok and all(x in new_names for x in old_names) and new_names.count('Skill_FrostNova') == 1
            bad += not ok
            q = seqs[0] if seqs else None
            print('%-4s %s: sequence %s, %.2f s, %d bones, text keys %s' % (
                'OK' if ok else 'FAIL', e.name, n.strings[q['name']] if q else '-', q['stop'] if q else 0,
                len(q['blocks']['items']) if q else 0,
                [n.strings[v] for _, v in n.blocks[q['text_keys']]['keys']] if q and q['text_keys'] >= 0 else '-'))
        elif e.name.lower().endswith('.gfx') and e.name.split('\\')[-1][:-4].lower() in [w.lower() for w in
                                                                                      __import__('gfx_fade').WINDOWS]:
            # 08.11 the windows' fade-in (tools/gfx_fade.py), the skill tree's hover zoom (tools/gfx_hoverzoom.py) under it:
            # unpacked, exactly what those make of the game's movie
            import zlib
            import gfx_hoverzoom
            import gfx_fade
            hit = skills_dump.effective(base, e.name)
            g = hit[0].read(hit[1])
            un = lambda b: b[:8] + zlib.decompress(b[8:])[:struct.unpack_from('<I', b, 4)[0] - 8]
            want = gfx_hoverzoom.patch(g) if e.name.lower().endswith('skilltree.gfx') else g
            ok = un(gfx_fade.patch(want)) == un(mine)
            bad += not ok
            print('%-4s %s: %d -> %d bytes, the game movie + fade-in / hover zoom scripts' % (
                'OK' if ok else 'FAIL', e.name, len(un(g)), len(un(mine))))
        elif e.name.lower().endswith('.gfx'):
            # a movie: unpacked, it differs from the game's only at the listed bytes (build_mage.SUMMONS_LEFT)
            import zlib
            hit = skills_dump.effective(base, e.name)
            g = hit[0].read(hit[1])
            un = lambda b: b[:8] + zlib.decompress(b[8:])[:struct.unpack_from('<I', b, 4)[0] - 8]
            a_, b_ = un(g), un(mine)
            diff = [i for i in range(len(a_)) if i >= len(b_) or a_[i] != b_[i]]
            want = sorted(x[0] for x in build_mage.SUMMONS_LEFT)
            ok = len(a_) == len(b_) and diff == want
            bad += not ok
            print('%-4s %s: %d bytes differ from the game (%s)' % ('OK' if ok else 'FAIL', e.name, len(diff), diff[:8]))
        else:
            t = parse_texture_nif(mine)
            print('OK   %s: %s %dx%d, %d mips' % (e.name, t['pixel_format'], t['mips'][0][0], t['mips'][0][1],
                                                  len(t['mips'])))
            pngs.append((e.name, mine))
    if len(argv) == 4 and pngs:
        from dv2_pack import nif_pixel_range
        from fx_select import decode_dxt, sheet
        items = []
        for n, b in sorted(pngs):
            fmt, s, _ = nif_pixel_range(b)
            w, h, _ = parse_texture_nif(b)['mips'][0]
            items.append((n.rsplit('_', 1)[-1], decode_dxt(b[s:], w, h, fmt)))
        for n, orig, mine, w, h, (x0, y0, x1, y1) in dds_areas:
            # the changed cell and, for comparison, the same area of the original (empty) and its column's top cell
            o = decode_dxt(orig[128:], w, h, 'DXT5')
            m = decode_dxt(mine[128:], w, h, 'DXT5')
            items += [('before', o[y0:y1, x0:x1]), ('after', m[y0:y1, x0:x1]), ('top of column', o[0:y1 - y0, x0:x1])]
        sheet(argv[3], items, tile=256, cols=len(items))
        print('icons ->', argv[3])
    print('FAILED: %d file(s)' % bad if bad else 'all files OK')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
