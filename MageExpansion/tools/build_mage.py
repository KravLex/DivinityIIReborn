r"""Builds the Mage Skills Expansion overlay archive from config/skills.json (stage 2a: data only, no exe changes).

  py -3 -I build_mage.py <game dir> <out dir>

Output: <out dir>\MageExpansion.dv2 (install into Data\Win32\Packed\Reborn\, Div2Fix overlay) + manifest.txt.
Every changed game file starts from the copy the game uses now (Reborn overlay without our own archive, then the patch
slots, then the main archives - skills_dump.archive_order), so the skill balance module in FOV_Patch_2.dv2 is kept.
New XML nodes are copies of existing ones (Magic Missile, a tree entry, a localisation entry) with new values, so the
hashed names and the attribute order are exactly what the game wrote itself. Refuses to build when a new skill's uuid
already has a prototype, its tree cell is taken or its name is already localised.
"""
import hashlib
import json
import os
import copy
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'reborn'))
import d2xml                                  # noqa: E402
import skills_dump                            # noqa: E402
from dv2_inspect import Dv2                   # noqa: E402
from dv2_pack import nif_pixel_range, pack, verify   # noqa: E402
from fx_select import decode_dxt              # noqa: E402
import nif_fx                                 # noqa: E402
import icon_art                               # noqa: E402
import fx_frostnova                           # noqa: E402
import fx_chain                               # noqa: E402
import fx_icebolt                             # noqa: E402
import anim_frostnova                         # noqa: E402
import anim_arcanepush                        # noqa: E402
import kfm                                    # noqa: E402
import nif_write                              # noqa: E402
import skeleton as skel                       # noqa: E402
import sfx_frostnova                          # noqa: E402
import sfx_chain                              # noqa: E402
import anim_holysmite                         # noqa: E402
import anim_live                              # noqa: E402
import sfx_push                               # noqa: E402
import sfx_shield                             # noqa: E402
import sfx_smite                              # noqa: E402
import sfx_bless                              # noqa: E402
import sfx_drain                              # noqa: E402
import sfx_cry                                # noqa: E402
import sfx_icebolt                            # noqa: E402
from dv2_inspect import parse_texture_nif     # noqa: E402

ARCHIVE = 'MageExpansion.dv2'
DATA_ARCHIVE = 'MageData.dv2'       # 07.11: the XML files alone, small enough for the game to read whole at the start
EPISODES = ('Episode_1_Extended', 'Episode_2')
KNOWN = ('root Skills Skill Level Parameters SkillTree Entry DefaultSkills main Dialog original translation UUID Name '
         'CodePath Description LyricalDescription EnduranceCost CooldownTime RequiredLevel X Y SkillUUID '
         'effectprototypes effect KFMFileName SizeEntry m_ID m_Size m_TotalSize ItemDescriptors itemdescriptor name size '
         'Entries Size FileName Trainers Trainer Skill NewMaxLevel GoldCost Available').split()
NAMES = {d2xml.name_hash(s): s for s in KNOWN}
TEMPLATE_SKILL = 'Magic Missile'
TEXTURE_INDEX = r'Win32\Textures\Textures.bin'
TREE_SHEET = r'GUI\Win32\characterCreation_I26.dds'
FX_PROTOS = 'rpgstats_effectprototypes.xml'               # effect name -> KFM path (MainDataStartup)
FX_ASSETS = 'effectassetdatadescriptors.xml'              # the same list with asset attributes (MainDataStartup)
FX_SIZES = r'Win32\Effects\Sizes.xml'                     # .item path -> size (MainDataPlatform)
FX_DESCRIPTORS = r'Win32\Effects\ItemDescriptors.xml'     # .item path (lower case) -> bounds (MainDataPlatform)
FX_ANIMS = r'Win32\Effects\effectanimationdata.xml'       # KFM path -> animation (MainDataPlatform)
TRAINERS = r'Episodes\%s\rpgstats_skilltrainerlists.xml'
FX_DIR = 'MageExpansion'                                  # Win32\Effects\MageExpansion\<name>.item
OWN_TEX_CONTAINER = r'Win32\Textures\skillbutton_MagicMissile_normal.nif'   # 512x512 DXT5, 10 mips (only the format)
# 03.11: 1024 for Chain Lightning's bolts (512 looked soft): a texture of the Reborn FX pack, 1024x1024 DXT5, 11 mips
OWN_TEX_CONTAINERS = {512: OWN_TEX_CONTAINER, 1024: r'Win32\Textures\FX_Circle_D.nif'}


def clone(n):
    c = d2xml.Node(n.name, list(n.attrs), n.text, n.h)
    c.children = [clone(k) for k in n.children]
    return c


def num(v):
    return ('%g' % v) if isinstance(v, float) else str(v)


def load(arcs, name):
    hit = skills_dump.effective(arcs, name)
    if not hit:
        raise SystemExit('missing in the archives: ' + name)
    a, e = hit
    return d2xml.Doc(a.read(e), NAMES), os.path.basename(a.path)


def add_prototype(doc, s):
    skills = doc.root.find('Skills')
    have = {k.get('UUID') for k in skills.children}
    if s['uuid'] in have:
        raise SystemExit('%s: skill %r already has a prototype' % (s['id'], s['uuid']))
    tpl = next(k for k in skills.children if k.get('UUID') == TEMPLATE_SKILL)
    lvl = tpl.children[0]
    node = d2xml.Node('Skill', [('UUID', s['uuid']), ('Name', s['name']), ('CodePath', s['code_path'])], h=tpl.h)
    for i, r in enumerate(s['ranks'], 1):
        L = clone(lvl)
        for k, v in (('Level', str(i)), ('Description', s['description']), ('LyricalDescription', s['lyrical']),
                     ('EnduranceCost', num(r['mana'])), ('CooldownTime', num(r['cooldown'])),
                     ('RequiredLevel', num(r['level']))):
            L.set(k, v)
        P = L.find('Parameters')
        P.attrs = [(k, num(v)) for k, v in r['params'].items()]
        node.children.append(L)
    # 07.11: spare ranks (copies of the last) so an item's +N (Skill Boosters, config/boosters.json) on a maxed skill
    # finds a rank; the rank cap (5 / trainers) keeps them out of reach of skill points
    for i in range(len(s['ranks']) + 1, len(s['ranks']) + 1 + EXTRA_RANKS):
        L = clone(node.children[-1])
        L.set('Level', str(i))
        node.children.append(L)
    skills.children.append(node)


EXTRA_RANKS = 0                                    # set from config/boosters.json in main()


def add_boosters(doc, cfg):
    """07.11: our skills as Skill Boosters of random items (docs/LOOT_SKILL_BOOSTERS.md): one effect per skill, cloned
    from the template skill's entry (attribute meanings in that doc)."""
    group = next((g for g in doc.root.children[0].children if g.get('Name') == 'Skill Boosters'), None)
    if group is None:
        raise SystemExit('itemmagicaleffects: no Skill Boosters group')
    tpl = next(c for c in group.children if c.get('Name') == cfg['template'])
    have_ids = set()
    for g in doc.root.children[0].children:
        for c in g.children:
            for e in c.children:
                have_ids.add(e.get('H_00017342'))
    added = 0
    for b in cfg['boosters']:
        if str(b['id']) in have_ids:
            raise SystemExit('booster id %d already used' % b['id'])
        c = clone(tpl)
        c.set('Name', b['skill'])
        e = c.children[0]
        for k, v in (('H_0006810C', b['skill']), ('H_BB46E9F2', str(b['min'])), ('H_C2E4BDEE', b['prefix']),
                     ('H_282F6C23', str(b['value'])), ('H_7A512C4C', b['class']), ('H_00017342', str(b['id']))):
            if e.get(k) is None:
                raise SystemExit('booster template has no %s' % k)
            e.set(k, v)
        group.children.append(c)
        added += 1
    # 07.11 the user: more than +1. Attribute names from the exe (hash -> string): MinEffect H_AC57C8D1 / MaxEffect
    # H_4C0F8153 = the random bonus range, MinLvl / MaxLvl = item level, Frequency = weight, UID = the saved number.
    # Every skill (the game's and ours) gets the higher tiers of boosters.json 'tiers', like the stats' tiers.
    uid = int(cfg['tier_first_id'])
    for c in list(group.children):
        base = c.children[0]
        for tier in cfg.get('tiers', []):
            e = clone(base)
            lo = max(int(base.get('H_BB46E9F2')), tier['min_lvl'])
            for k, v in (('H_BB46E9F2', str(lo)), ('H_AC57C8D1', str(tier['min'])), ('H_4C0F8153', str(tier['max'])),
                         ('H_7E271012', str(tier['freq'])), ('H_00017342', str(uid)),
                         ('H_282F6C23', str(int(round(int(base.get('H_282F6C23')) * tier['gold_mul']))))):
                e.set(k, v)
            if str(uid) in have_ids:
                raise SystemExit('booster tier id %d already used' % uid)
            c.children.append(e)
            uid += 1
    # 08.11: armour and weapon slots take skill boosters too (Div2Fix SkillBoostersEverywhere); the user asked for ~20 %
    # of those slots: the base +1 weight of every skill booster (the game's 3) becomes boosters.json 'base_freq'
    if cfg.get('base_freq'):
        for c in group.children:
            c.children[0].set('H_7E271012', str(cfg['base_freq']))
    # 08.11 the user: boosters on armour and weapons too. AllowUse (H_7A512C4C) = the MagicEffectCategory letters of the
    # items an effect may land on (item prototypes): A armour/shields, B belts/rings, X swords/axes, Y bows, Z hammers;
    # the game's skill boosters had only W/M/D/R (a few class jewels). boosters.json 'allow_extra' adds letters per class.
    extra = cfg.get('allow_extra', {})
    for c in group.children:
        for e in c.children:
            cls = e.get('H_7A512C4C') or ''
            add = ''.join(ch for ch in sorted(set(''.join(extra.get(ch, '') for ch in cls))) if ch not in cls)
            if add:                                    # no AllowUse (Meteor Strike): left as it is
                e.set('H_7A512C4C', cls + add)
    return added


def add_booster_names(doc, cfg):
    """the prefixes in ItemsGenerators_Localisation.xml: a Dialog like the template's, 6 forms (positions 2-5, 10, 11)"""
    have = {d.children[0].text for d in doc.root.children}
    src = next(d for d in doc.root.children if d.children[0].text == 'Striking')
    for b in cfg['boosters']:
        if b['prefix'] in have:
            continue
        d = clone(src)
        d.children[0].text = b['prefix']
        for pos, txt in zip((2, 3, 4, 5, 10, 11), b['ru']):
            d.children[pos].text = txt
        doc.root.children.append(d)
        have.add(b['prefix'])


def move_tree_entries(doc, moves):
    """Move game skills to other cells; the cells a move leaves or takes are checked when the entries are added."""
    tree = doc.root.find('SkillTree')
    for m in moves:
        e = next((x for x in tree.children if x.get('SkillUUID') == m['uuid']), None)
        if e is None:
            raise SystemExit('tree_moves: no skill %r in the tree' % m['uuid'])
        e.attrs = [('X', str(m['tree'][0])), ('Y', str(m['tree'][1])), ('SkillUUID', m['uuid'])]
    seen = set()
    for e in tree.children:
        c = (e.get('X'), e.get('Y'))
        if c in seen:
            raise SystemExit('tree_moves: two game skills in cell %s,%s' % c)
        seen.add(c)


def add_tree_entry(doc, s):
    tree = doc.root.find('SkillTree')
    x, y = (str(v) for v in s['tree'])
    for e in tree.children:
        if (e.get('X'), e.get('Y')) == (x, y) or e.get('SkillUUID') == s['uuid']:
            raise SystemExit('%s: tree cell %s,%s or uuid already used by %r' % (s['id'], x, y, e.get('SkillUUID')))
    e = clone(tree.children[0])
    e.attrs = [('X', x), ('Y', y), ('SkillUUID', s['uuid'])]
    tree.children.append(e)


def add_default(doc, s):
    root = doc.root
    if any(k.get('UUID') == s['uuid'] for k in root.children):
        raise SystemExit('%s: %r already in DefaultSkills' % (s['id'], s['uuid']))
    e = clone(root.children[0])
    e.attrs = [('UUID', s['uuid']), ('Level', str(s['default_level']))]
    root.children.append(e)


def add_localisation(doc, s):
    """Dialog = original (English text), translation, category (hash only: 'Skill Name' / 'Description' / ...)."""
    root = doc.root
    by_cat, have = {}, set()
    for d in root.children:
        o, t, c = d.children
        have.add(o.text)
        by_cat.setdefault(c.text, d)
    entries = [(s['name'], s['ru']['name'], 'Skill Name'),
               (s['description'], s['ru']['description'], 'Description'),
               (s['lyrical'], s['ru']['lyrical'], 'LyricalDescription')]
    # the trainer window translates the record's Skill string, which is the UUID (0x89EE80 -> 0x8E5920)
    if s['uuid'] != s['name']:
        entries.append((s['uuid'], s['ru']['name'], 'Skill Name'))
    for key, text, cat in entries:
        if key in have:
            raise SystemExit('%s: %r is already localised' % (s['id'], key))
        d = clone(by_cat[cat])
        d.children[0].text, d.children[1].text = key, text
        root.children.append(d)


def add_trainers(doc, s):
    """Add the two rank-cap purchases to every mage trainer that already teaches Magic Missile."""
    trainers = doc.root.find('Trainers')
    added = 0
    for trainer in trainers.children:
        missile = [x for x in trainer.children if x.get('Skill') == TEMPLATE_SKILL]
        # Every trainer list gets the purchases. The trainer window (0xB1B200) turns the record's Skill string into
        # the skill id through the prototype UUID map (0x89F0D0 -> 0x6FEEB0) when it opens, so the key is the UUID.
        for purchase in s.get('trainer', []):
            for skill_key in (s['uuid'],):
                if any(x.get('Skill') == skill_key and x.get('NewMaxLevel') == str(purchase['max_level'])
                   for x in trainer.children):
                    continue
                source = next((x for x in missile if x.get('NewMaxLevel') == str(purchase['max_level'])),
                              missile[-1] if missile else trainer.children[-1])
                x = clone(source)
                x.set('Skill', skill_key)
                x.set('NewMaxLevel', str(purchase['max_level']))
                x.set('Description', purchase['description'])
                x.set('GoldCost', str(purchase['gold']))
                x.set('Available', 'True' if purchase.get('available', True) else 'False')
                x.set('RequiredLevel', str(purchase['required_level']))
                trainer.children.append(x)
                added += 1
    return added


def tint_matrix(sat, green):
    lum = np.array([0.299, 0.587, 0.114])
    m = sat * np.array([[0, 0, 1], [0, 1, 0], [1, 0, 0]], float) + (1 - sat) * np.tile(lum, (3, 1))
    m[1] *= green
    return m


def tint_nif(b, m):
    """Linear colour transform of the DXT5 colour endpoints (BC3 colour blocks are always 4-colour: no remapping)."""
    fmt, start, end = nif_pixel_range(b)
    if fmt != 'DXT5':
        raise SystemExit('icon format %s not supported' % fmt)
    out = bytearray(b)
    for i in range(start + 8, end, 16):
        ends = []
        for v in struct.unpack_from('<HH', out, i):
            rgb = np.array([(v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31])
            r, g, bl = np.clip(m @ rgb, 0, 255)
            ends.append((round(r * 31 / 255) << 11) | (round(g * 63 / 255) << 5) | round(bl * 31 / 255))
        struct.pack_into('<HH', out, i, *ends)
    return bytes(out)


def icons(arcs, s):
    ic = s['icon']
    m = tint_matrix(ic['saturation'], ic['green'])
    dst = s['uuid'].replace(' ', '')
    art = icon_art.button_states(ic['art']) if ic.get('art') else None
    out = []
    for state in ('normal', 'hilite', 'pressed'):
        src = r'Win32\Textures\skillbutton_%s_%s.nif' % (ic['from'], state)
        hit = skills_dump.effective(arcs, src)
        if not hit:
            raise SystemExit('missing icon ' + src)
        a, e = hit
        b = a.read(e)
        if art is None:
            data = tint_nif(b, m)
        else:
            # our own art in the same container: same size, DXT5 and mip count as the source button
            fmt, start, end = nif_pixel_range(b)
            mips = parse_texture_nif(b)['mips']
            if fmt != 'DXT5' or mips[0][0] != art[state].shape[1] or mips[0][1] != art[state].shape[0]:
                raise SystemExit('icon template %s: %s %dx%d, art %dx%d' % (src, fmt, mips[0][0], mips[0][1],
                                                                          art[state].shape[1], art[state].shape[0]))
            pixels = icon_art.mip_chain(art[state], len(mips))
            if len(pixels) != end - start:
                raise SystemExit('icon template %s: %d pixel bytes, art %d' % (src, end - start, len(pixels)))
            data = b[:start] + pixels + b[end:]
        out.append((r'Win32\Textures\skillbutton_%s_%s.nif' % (dst, state), data, os.path.basename(a.path)))
    return out


def buff_icon(arcs, src=r'Win32\Textures\Buff_SkillUp.nif', art='arcane_shield'):
    """05.11: the icon over the bar under the shield. Status 0xF asks for "buf/shielded" (exe 0x9833E0), the file is Buff_Shielded.nif:
    outside the dragon form the status asks for "buf/skillup" (Div2Fix buff_name): no status of the game reads that name (exe: 0x14F1F84 only
    made and freed), so our art goes into Buff_SkillUp.nif, in
    that file's own container; the dragon keeps Buff_Shielded.nif."""
    a, e = skills_dump.effective(arcs, src)
    b = a.read(e)
    fmt, start, end = nif_pixel_range(b)
    mips = parse_texture_nif(b)['mips']
    if fmt != 'DXT5' or mips[0][0] != mips[0][1]:
        raise SystemExit('%s: %s %dx%d' % (src, fmt, mips[0][0], mips[0][1]))
    img = (icon_art.ARTS[art](mips[0][0]) * 255 + 0.5).astype(np.uint8)
    pixels = icon_art.mip_chain(img, len(mips))
    if len(pixels) != end - start:
        raise SystemExit('%s: %d pixel bytes, art %d' % (src, end - start, len(pixels)))
    print('  %s: %dx%d, %d mips, from %s' % (src, mips[0][0], mips[0][1], len(mips), os.path.basename(a.path)))
    return (src, b[:start] + pixels + b[end:], os.path.basename(a.path))


def texture_names(arcs):
    """Texture file names listed in the game's Textures.bin."""
    a, e = skills_dump.effective(arcs, TEXTURE_INDEX)
    data = a.read(e)
    count, p, names = struct.unpack_from('<I', data, 0)[0], 4, []
    for _ in range(count):
        ln = struct.unpack_from('<I', data, p)[0]
        names.append(data[p + 4:p + 4 + ln].decode('latin1'))
        p += 4 + ln
    return names


def texture_index(arcs, names):
    """Win32\\Textures\\Textures.bin: u32 count, count x {u32 length, name}. The game only finds texture files that are
    listed there (a new skillbutton_*.nif without an entry stayed blank in game, 01.11). The list is not strictly sorted
    and already ends with later additions, so new names are appended."""
    hit = skills_dump.effective(arcs, TEXTURE_INDEX)
    if not hit:
        raise SystemExit('missing ' + TEXTURE_INDEX)
    a, e = hit
    data = a.read(e)
    count = struct.unpack_from('<I', data, 0)[0]
    p, have = 4, set()
    for _ in range(count):
        ln = struct.unpack_from('<I', data, p)[0]
        have.add(data[p + 4:p + 4 + ln].decode('latin1').lower())
        p += 4 + ln
    if p != len(data):
        raise SystemExit('%s: %d bytes after %d entries' % (TEXTURE_INDEX, len(data) - p, count))
    add = [n for n in names if n.lower() not in have]
    out = struct.pack('<I', count + len(add)) + data[4:]
    for n in add:
        out += struct.pack('<I', len(n)) + n.encode('latin1')
    return out, os.path.basename(a.path)


def _runs(mask):
    idx = np.where(mask)[0]
    runs, start = [], idx[0]
    for a, b in zip(idx, idx[1:]):
        if b != a + 1:
            runs.append((start, a))
            start = b
    runs.append((start, idx[-1]))
    return runs


def tree_sheet(arcs, cfg, tree):
    """The skill tree window of the hero does not load icons: showPlayerSkillIcons(true) (exe 0xA54360) shows the layer
    skillIcons_mc of skillTree.gfx, a pre-drawn picture of all cells (GUI\\Win32\\characterCreation_I26.dds, 10 columns
    x 6 rows; column = Y, row = X of rpgstats_skilltree.xml), and addSkill() skips addIcon() (MageExpansion/tools/
    gfx_avm1.py). A new cell is drawn there: the cell of tree_icon_from is copied block for block (DXT5, no
    recompression; both offsets must be multiples of 4) and its coloured endpoints are tinted like the icon."""
    hit = skills_dump.effective(arcs, TREE_SHEET)
    if not hit:
        raise SystemExit('missing ' + TREE_SHEET)
    a, e = hit
    data = bytearray(a.read(e))
    h, w = struct.unpack_from('<II', data, 12)
    if data[:4] != b'DDS ' or data[84:88] != b'DXT5' or struct.unpack_from('<I', data, 28)[0] > 1:
        raise SystemExit('%s: expected DXT5 without mips' % TREE_SHEET)
    alpha = decode_dxt(bytes(data[128:]), w, h, 'DXT5')[..., 3] > 16
    cols, rows = _runs(alpha.any(0)), _runs(alpha.any(1))
    bw = w // 4
    by_uuid = {en.get('SkillUUID'): (int(en.get('X')), int(en.get('Y'))) for en in tree.find('SkillTree').children}
    for s in cfg['skills']:
        src_uuid = s.get('tree_icon_from')
        if not src_uuid:
            continue
        sx, sy = by_uuid[src_uuid]                  # row = X, column = Y
        dx_, dy_ = s['tree']
        if dx_ >= len(rows):
            # a row below the sheet (X >= 6): no picture there; Div2Fix [MageSkills] TreeSheet=0 hides the sheet and
            # the movie loads every cell's own button icon (addSkill -> addIcon)
            print('  tree cell %s (%d,%d): below the %d rows of the sheet, needs TreeSheet=0' % (s['id'], dx_, dy_, len(rows)))
            continue
        if max(sy, dy_) >= len(cols) or max(sx, dx_) >= len(rows):
            raise SystemExit('%s: cell outside the %dx%d grid of %s' % (s['id'], len(cols), len(rows), TREE_SHEET))
        (c0, c1), (r0, r1) = cols[sy], rows[sx]
        ox, oy = cols[dy_][0] - c0, rows[dx_][0] - r0
        if ox % 4 or oy % 4:
            raise SystemExit('%s: cell offset %d,%d is not block aligned' % (s['id'], ox, oy))
        bx0, bx1 = max(0, (c0 - 2) // 4), min(bw, (c1 + 6) // 4)
        by0, by1 = max(0, (r0 - 2) // 4), min(h // 4, (r1 + 6) // 4)
        tx0, ty0 = (c0 - 2 + ox) // 4 * 4, (r0 - 2 + oy) // 4 * 4
        if alpha[max(0, ty0):ty0 + (by1 - by0) * 4, max(0, tx0):tx0 + (bx1 - bx0) * 4].any():
            raise SystemExit('%s: the target cell of %s is not empty' % (s['id'], TREE_SHEET))
        if s['icon'].get('art'):
            _tree_cell_art(data, w, h, cols, rows, alpha, (sx, sy), (dx_, dy_), s['icon']['art'])
            continue
        m = tint_matrix(s['icon']['saturation'], s['icon']['green'])
        for by in range(by0, by1):
            for bx in range(bx0, bx1):
                so = 128 + (by * bw + bx) * 16
                do = 128 + ((by + oy // 4) * bw + bx + ox // 4) * 16
                block = bytearray(data[so:so + 16])
                ends = []
                for v in struct.unpack_from('<HH', block, 8):
                    rgb = np.array([(v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31])
                    if rgb.max() - rgb.min() < 24:          # the grey frame and the level badge stay as they are
                        ends.append(v)
                        continue
                    r, g, bl = np.clip(m @ rgb, 0, 255)
                    ends.append((round(r * 31 / 255) << 11) | (round(g * 63 / 255) << 5) | round(bl * 31 / 255))
                struct.pack_into('<HH', block, 8, *ends)
                data[do:do + 16] = block
    return bytes(data), os.path.basename(a.path)


def _resize(img, w, h):
    """Bilinear resize of a float image (h, w, c)."""
    sh, sw = img.shape[:2]
    ys = np.clip((np.arange(h) + 0.5) * sh / h - 0.5, 0, sh - 1)
    xs = np.clip((np.arange(w) + 0.5) * sw / w - 0.5, 0, sw - 1)
    y0, x0 = ys.astype(int), xs.astype(int)
    y1, x1 = np.minimum(y0 + 1, sh - 1), np.minimum(x0 + 1, sw - 1)
    fy, fx = (ys - y0)[:, None, None], (xs - x0)[None, :, None]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bot = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy


def _tree_cell_art(data, w, h, cols, rows, alpha, src, dst, art):
    """Draw our own art into an empty cell of the tree sheet: the frame and the level badge are the same in every
    cell, the art is what differs, so the art window is where the filled cells differ. The source cell gives the frame
    and badge, the art is fitted into the window (composited over black, the cell's alpha is kept), and the cell's
    block-aligned rectangle is encoded again (icon_art.dxt5)."""
    img = decode_dxt(bytes(data[128:]), w, h, 'DXT5')
    (c0, c1), (r0, r1) = cols[src[1]], rows[src[0]]
    W, H = c1 - c0 + 5, r1 - r0 + 5

    def window(r, c):
        y, x = rows[r][0] - 2, cols[c][0] - 2
        return img[max(0, y):y + H, max(0, x):x + W]
    cells = [(r, c) for r in range(len(rows)) for c in range(len(cols)) if (r, c) != tuple(dst)
             and alpha[rows[r][0]:rows[r][1] + 1, cols[c][0]:cols[c][1] + 1].any()]
    stack = [window(r, c) for r, c in cells]
    stack = np.stack([x for x in stack if x.shape == (H, W, 4)]).astype(np.float32)
    var = stack[..., :3].std(0).mean(-1)
    artm = icon_art._blur((var > 20).astype(np.float32), 2) > 0.5          # close small holes
    ys, xs = np.where(artm)
    ay0, ay1, ax0, ax1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    # the variance also catches the inner edge of the ring (other cells' art shows through it) and the gap under the
    # badge: keep only the disc inside the ring (the window is round at the top: centre = half its width below the top)
    rad = (ax1 - ax0) / 2.0
    gy, gx = np.mgrid[0:H, 0:W]
    disc = np.hypot(gx - (ax0 + ax1) / 2.0, gy - (ay0 + rad)) < rad - 4
    artm = artm & disc
    soft = np.clip(icon_art._blur(artm.astype(np.float32), 1) * 1.2, 0, 1)[..., None]
    base = window(*src).astype(np.float32)
    pic = icon_art.button_states(art)['normal'].astype(np.float32)
    k = pic.shape[0] // 10                                 # the central 80 %: the art fills the round window
    pic = pic[k:pic.shape[0] - k, k:pic.shape[1] - k]
    pic = _resize(pic, ax1 - ax0, ay1 - ay0)
    over = np.zeros_like(base)
    over[ay0:ay1, ax0:ax1, :3] = pic[..., :3] * (pic[..., 3:] / 255.0)
    out = base.copy()
    out[..., :3] = base[..., :3] * (1 - soft) + over[..., :3] * soft
    y, x = rows[dst[0]][0] - 2, cols[dst[1]][0] - 2
    img[y:y + H, x:x + W] = np.clip(out + 0.5, 0, 255).astype(np.uint8)
    X0, Y0 = x // 4 * 4, y // 4 * 4
    X1, Y1 = min(w, -(-(x + W) // 4) * 4), min(h, -(-(y + H) // 4) * 4)
    enc = icon_art.dxt5(img[Y0:Y1, X0:X1])
    row = (X1 - X0) // 4 * 16
    for j in range((Y1 - Y0) // 4):
        at = 128 + ((Y0 // 4 + j) * (w // 4) + X0 // 4) * 16
        data[at:at + row] = enc[j * row:(j + 1) * row]
    print('  tree cell art %s: window %dx%d of the %dx%d cell (from %d filled cells), blocks x %d..%d y %d..%d' % (
        art, ax1 - ax0, ay1 - ay0, W, H, len(stack), X0, X1, Y0, Y1))


def _find_parent(node, pred):
    """(parent, child) of the first child that matches pred, depth first."""
    for c in node.children:
        if pred(c):
            return node, c
        hit = _find_parent(c, pred)
        if hit:
            return hit
    return None


def effects(arcs, cfg, taken_textures):
    """New effects for skills with "effects": copies of game effects (.item = NIF) with recoloured particles and
    textures, registered in the five lists the game reads for effects. Texture file names are header strings of the
    NIF: the copy gets names of the same length (first letter -> I/i, nif_fx.Nif.rename_string), so no offset moves.
    Returns [(archive entry, bytes, source)] and the new texture file names (for Textures.bin)."""
    specs = [(s, e) for s in cfg['skills'] for e in s.get('effects', {}).get('list', [])]
    own = [(s, e) for s in cfg['skills'] for e in s.get('own_effects', [])]
    if not specs and not own:
        return [], []
    docs = {k: load(arcs, k) for k in (FX_PROTOS, FX_ASSETS, FX_SIZES, FX_DESCRIPTORS, FX_ANIMS)}
    protos = docs[FX_PROTOS][0].root.find('effectprototypes')
    assets = docs[FX_ASSETS][0].root.find('effectprototypes')
    out, textures = [], {}                                    # textures: old base name -> new base name
    own_tex = {}                                              # textures of our own effects: name -> RGBA
    for s, e in specs + own:
        is_own = 'builder' in e
        if any(x.get('Name') == e['name'] for x in protos.children):
            raise SystemExit('%s: effect %s already exists' % (s['id'], e['name']))
        # an own effect takes only the format of the five list rows from rows_from, never its file
        src_name = e['rows_from'] if is_own else e['from']
        src = next((x for x in protos.children if x.get('Name') == src_name), None)
        src_asset = next((x for x in assets.children if x.get('Name') == src_name), None)
        if src is None or src_asset is None:
            raise SystemExit('%s: no effect %s to copy' % (s['id'], src_name))
        kfm = src.get('KFMFileName')
        rel_item = kfm[:-4] + '.item'
        new_kfm = '%s\\%s.kfm' % (FX_DIR, e['name'])
        new_rel = '%s\\%s.item' % (FX_DIR, e['name'])
        if is_own:
            mod = {'frost_nova': fx_frostnova, 'frost_nova_chill': fx_frostnova.Chill, 'chain_beam': fx_chain, 'chain_cast': fx_chain.Cast, 'push_wave': fx_chain.PushWave, 'push_hit': fx_chain.PushHit, 'shield_cast': fx_chain.ShieldCast, 'shield_hit': fx_chain.ShieldHit, 'mark_target': fx_chain.MarkTarget, 'smite_hit': fx_chain.SmiteHit, 'drain_hit': fx_chain.DrainHit, 'curse_aura': fx_chain.CurseAura, 'drain_cast': fx_chain.DrainCast,  'smite_cast': fx_chain.SmiteCast, 'wind_burst': fx_chain.WindBurst, 'push_burst': fx_chain.PushBurst, 'summon_boom': fx_chain.SummonBoom, 'blink_out': fx_chain.BlinkOut, 'blink_in': fx_chain.BlinkIn,
                   'cry_wave': fx_chain.CryWave, 'armor_crack': fx_chain.ArmorCrack, 'ice_arrow_cast': fx_icebolt.ArrowCast, 'ice_travel': fx_icebolt.Travel, 'ice_impact': fx_icebolt.Impact, 'ice_cast': fx_icebolt.Cast, 'ice_trail': fx_icebolt.Trail, 'ice_slowed': fx_icebolt.Slowed, 'ice_frozen': fx_icebolt.Frozen}[e['builder']]
            args = dict(e.get('args', {}))
            if getattr(mod, 'SOURCE', None) and not os.environ.get('MAGE_NO_BASE_' + e['name'].upper()):  # built on a game effect (its particle systems); env MAGE_NO_BASE_<NAME>: test without
                hit = skills_dump.effective(arcs, mod.SOURCE)
                if not hit:
                    raise SystemExit('%s: missing %s' % (s['id'], mod.SOURCE))
                args['source'] = hit[0].read(hit[1])
            data = mod.build(name=e['name'], **args)
            own_tex.update(mod.textures())
            out.append(('Win32\\Effects\\' + new_rel, data, 'own (%s)' % e['builder']))
            print('  effect %-16s own (%s): %d bytes; list rows formatted like %s' % (
                e['name'], e['builder'], len(data), src_name))
        else:
            m = tint_matrix(s['effects']['tint']['saturation'], s['effects']['tint']['green'])
            hit = skills_dump.effective(arcs, 'Win32\\Effects\\' + rel_item)
            if not hit:
                raise SystemExit('%s: missing Win32\\Effects\\%s' % (s['id'], rel_item))
            nif = nif_fx.Nif(hit[0].read(hit[1]))
            for tex in nif.texture_names():
                base = tex.rsplit('.', 1)[0]
                new = textures.setdefault(base, ('i' if base[0].islower() else 'I') + base[1:])
                nif.rename_string(tex, new + tex[len(base):])
            changed = nif.tint(m)
            data = bytes(nif.data)
            out.append(('Win32\\Effects\\' + new_rel, data, os.path.basename(hit[0].path)))
            print('  effect %-16s from %-16s (%s): %d colours tinted, textures %s' % (
                e['name'], e['from'], rel_item, changed, ', '.join(nif.texture_names())))
        # the five lists: copies of the source effect's entries
        for parent, node in ((protos, src), (assets, src_asset)):
            c = clone(node)
            c.set('Name', e['name'])
            c.set('KFMFileName', new_kfm)
            parent.children.append(c)
        hit = _find_parent(docs[FX_SIZES][0].root, lambda n: (n.get('m_ID') or '').lower() == ('win32\\effects\\' + rel_item).lower())
        if not hit:
            raise SystemExit('%s: %s not in %s' % (s['id'], rel_item, FX_SIZES))
        parent, node = hit
        c = clone(node)
        c.set('m_ID', 'Win32\\Effects\\' + new_rel)
        c.set('m_Size', str(len(data)))
        c.set('m_TotalSize', str(len(data)))
        parent.children.append(c)
        parent.set('m_TotalSize', str(int(parent.get('m_TotalSize')) + len(data)))
        root = docs[FX_DESCRIPTORS][0].root
        node = next((x for x in root.children if (x.get('name') or '').lower() == rel_item.lower()), None)
        if node is None:
            raise SystemExit('%s: %s not in %s' % (s['id'], rel_item, FX_DESCRIPTORS))
        c = clone(node)
        c.set('name', new_rel.lower())
        if is_own:                     # the bound (radius; centre, min, max) is the own effect's, not the row source's
            b = mod.BOUND
            box = c.children[0]
            box.attrs = [(box.attrs[0][0], num(b[3]))]
            for child, xyz in zip(box.children, (b[0:3], b[4:7], b[7:10])):
                child.attrs = [(k, num(v)) for (k, _), v in zip(child.attrs, xyz)]
        root.children.append(c)
        root.set('size', str(int(root.get('size')) + 1))
        entries_node = docs[FX_ANIMS][0].root.find('Entries')
        node = next((x for x in entries_node.children if (x.get('FileName') or '').lower() == kfm.lower()), None)
        if node is None:
            raise SystemExit('%s: %s not in %s' % (s['id'], kfm, FX_ANIMS))
        c = clone(node)
        c.set('Name', e['name'])
        c.set('FileName', new_kfm)
        if is_own:                     # its one animation: our sequence name and length (cycle and the rest as the row)
            for anim in c.children:
                anim.set('Name', e['name'])
                anim.attrs = [(k, num(mod.DURATION) if str(k) == 'H_00E319E6' else v) for k, v in anim.attrs]
        entries_node.children.append(c)
        entries_node.set('Size', str(int(entries_node.get('Size')) + 1))
    for k, (doc, src) in docs.items():
        out.append((k, doc.encode(), src))
    lower = {n.lower() for n in taken_textures}
    names = []
    # textures of our own effects: 512x512 DXT5 with mips, put into the container of a game texture of that format
    # test aid: byte copies of game textures under new names (is a new texture name found at all?)
    for s_, e_ in own:
        for new, src in e_.get('copy_textures', {}).items():
            hit = skills_dump.effective(arcs, r'Win32\Textures\%s.nif' % src)
            if not hit:
                raise SystemExit('missing texture ' + src)
            out.append((r'Win32\Textures\%s.nif' % new, hit[0].read(hit[1]), os.path.basename(hit[0].path)))
            names.append(new + '.nif')
    for s_, e_ in own:      # 06.11 a game texture recoloured in place (DXT colour endpoints), under a new name
        for new, t in e_.get('tint_textures', {}).items():
            hit = skills_dump.effective(arcs, r'Win32\Textures\%s.nif' % t['from'])
            if not hit:
                raise SystemExit('missing texture ' + t['from'])
            if (new + '.nif').lower() in lower:
                raise SystemExit('texture name %s.nif is already taken' % new)
            b = bytearray(hit[0].read(hit[1]))
            fmt, start, end = nif_pixel_range(bytes(b))
            nif_fx.tint_dxt(b, start, end, fmt, np.array(t['matrix'], float))
            out.append((r'Win32\Textures\%s.nif' % new, bytes(b), os.path.basename(hit[0].path)))
            names.append(new + '.nif')
    containers = {}
    for size, cname in OWN_TEX_CONTAINERS.items() if own_tex else ():
        hit = skills_dump.effective(arcs, cname)
        if hit:
            cb = hit[0].read(hit[1])
            containers[size] = (cname, cb, nif_pixel_range(cb), parse_texture_nif(cb)['mips'])
    if own_tex:
        for name, img in sorted(own_tex.items()):
            if img.shape[1] not in containers:
                raise SystemExit('no texture container for %s (%dx%d)' % (name, img.shape[1], img.shape[0]))
            cname, b, (fmt, start, end), mips = containers[img.shape[1]]
            if (name + '.nif').lower() in lower:
                raise SystemExit('texture name %s.nif is already taken' % name)
            if fmt != 'DXT5' or (mips[0][0], mips[0][1]) != (img.shape[1], img.shape[0]):
                raise SystemExit('texture container %s does not fit %s' % (cname, name))
            pixels = icon_art.mip_chain(img, len(mips))
            if len(pixels) != end - start:
                raise SystemExit('texture %s: %d pixel bytes, container %d' % (name, len(pixels), end - start))
            out.append(('Win32\\Textures\\%s.nif' % name, b[:start] + pixels + b[end:], 'own'))
            names.append(name + '.nif')
    if not specs:
        return out, names
    # recoloured textures under the new names (same tint as the skill's effects; one skill uses them so far)
    m = tint_matrix(specs[0][0]['effects']['tint']['saturation'], specs[0][0]['effects']['tint']['green'])
    for old, new in sorted(textures.items()):
        if (new + '.nif').lower() in lower:
            raise SystemExit('texture name %s.nif is already taken' % new)
        hit = None
        for a in arcs:
            hit = next(((a, x) for x in a.entries if x.name.lower() == ('win32\\textures\\%s.nif' % old).lower()), None)
            if hit:
                break
        if not hit:
            raise SystemExit('missing texture Win32\\Textures\\%s.nif' % old)
        b = bytearray(hit[0].read(hit[1]))
        fmt, start, end = nif_pixel_range(bytes(b))
        nif_fx.tint_dxt(b, start, end, fmt, m)
        out.append(('Win32\\Textures\\%s.nif' % new, bytes(b), os.path.basename(hit[0].path)))
        names.append(new + '.nif')
    return out, names


ANIM_GENDERS = (('HumanFemale', 'F'), ('HumanMale', 'M'))
# 06.11 Weapon Expertise Bow: a copy of the shot per speed; Div2Fix adds the suffix to the sequence's name by the skill's rank (BowSkill). The copy shares the game's key data,
# only its frequency differs, and its length and text key times in CharacterAnimationData are divided by the speed: the shot's action reads the arrow's moment from there
BOW_SHOTS = ('Ranged_0_BOW', 'Ranged_45_BOW', 'Ranged_Min45_BOW')
BOW_TIERS = (('_S1', 1.10), ('_S2', 1.25), ('_S3', 1.40), ('_S4', 1.50))
ANIM_WEAPONS = ('', '_1H', '_1HSH', '_2HSW', '_BOW', '_DW', '_UA')
CHAR_ANIMS = r'Win32\Characters\CharacterAnimationData.xml'   # KFM -> its sequences (MainDataPlatform)
CHAR_SIZES = r'Win32\Characters\Sizes.xml'                    # .kf path -> size (MainDataPlatform)
ANIM_SEQ_ID = 900                     # sequence id of our own animations in the player's KFMs (game ids end below 0x60)


def _merge_anim_nif(base, extra):
    """Append the generated controller sequence and its blocks to a player's base .kf NIF.

    The game accepts the sequence only when it lives in the same KF as the actor's existing
    sequences. References and header-string indexes are relocated explicitly; the original
    base blocks remain byte-for-byte represented by the writer's normal round-trip.
    """
    block0 = len(base.blocks)
    string_map = [base.string(s) for s in extra.strings]

    def relocate(kind, value):
        if isinstance(kind, tuple):
            if kind[0] == 'n':
                return [relocate(kind[1], x) for x in value]
            if kind[0] == 'pair':
                return [relocate(kind[1], value[0]), relocate(kind[2], value[1])]
        if kind == 's':
            return -1 if value < 0 else string_map[value]
        if kind == 'r':
            return -1 if value < 0 else value + block0
        if kind == 'cblocks':
            out = {'grow': value['grow'], 'items': []}
            for x in value['items']:
                y = dict(x)
                # Controlled blocks mix block references and string-table indexes:
                # interpolator/controller/property are refs; node and the three
                # controller identifiers are strings.  Keeping these namespaces
                # separate is required when appending a sequence to an existing KF.
                for key in ('interpolator', 'controller', 'property'):
                    if y[key] >= 0:
                        y[key] += block0
                for key in ('node', 'ctrl_type', 'ctrl_id', 'interp_id'):
                    if y[key] >= 0:
                        y[key] = string_map[y[key]]
                out['items'].append(y)
            return out
        return copy.deepcopy(value)

    for b in extra.blocks:
        if b.raw is not None:
            raise SystemExit('generated animation contains an unsupported raw NIF block')
        fields = {name: relocate(kind, b.f[name]) for name, kind in nif_write.SPECS[b.type]}
        base.blocks.append(nif_write.Block(b.type, fields))
    base.roots.append(block0 + extra.roots[0])
    return block0


def animations(arcs, cfg):
    """Own cast animations (tools/anim_frostnova.py): one .kf per skeleton and a sequence entry in each of the hero's
    14 KFMs (2 skeletons x 7 weapon sets), default transitions to and from every other sequence."""
    specs = [(s, a) for s in cfg['skills'] for a in s.get('own_animations', [])]
    out = []
    if not specs:
        return out
    # the game also lists every KFM's sequences (name, length, cycle, text keys) and the size of every .kf;
    # 02.11: without these entries the hero vanished
    anims_doc, anims_src = load(arcs, CHAR_ANIMS)
    sizes_doc, sizes_src = load(arcs, CHAR_SIZES)

    def walk(n):
        yield n
        for c in n.children:
            yield from walk(c)
    # 04.11: several animations go into the same base .kf and KFMs, so one copy of each is kept across them
    bases, kfms, files = {}, {}, {}
    for n, (s, a) in enumerate(specs):
        mod = {'frost_nova': anim_frostnova, 'arcane_push': anim_arcanepush, 'holy_smite': anim_holysmite}[a['builder']]
        for folder, g in ANIM_GENDERS:
            hit = skills_dump.effective(arcs, r'Win32\Characters\%s\Skeleton.nif' % folder)
            base = skills_dump.effective(arcs, r'Win32\Characters\%s\Player_%s_Base.kf' % (folder, g))
            if not hit or not base:
                raise SystemExit('missing skeleton or base animation of ' + folder)
            sk = skel.Skeleton(hit[0].read(hit[1]))
            # the bones the game's own cast animation drives (names only)
            base_name = r'Win32\Characters\%s\Player_%s_Base.kf' % (folder, g)
            gk = bases.setdefault(base_name, nif_write.Nif(base[0].read(base[1])))
            seq = next(b for b in gk.blocks if b.type == 'NiControllerSequence' and gk.strings[b['name']] == 'Skill_Cast_Target')
            bones = [gk.strings[c['node']] for c in seq['blocks']['items']]
            # 05.11: the body from the game's own cast, only the arms ours (anim_live)
            real_sample = anim_frostnova.sample
            if a['builder'] == 'arcane_push':            # Frost Nova's own raised arms on the game's body looked broken (05.11)
                anim_frostnova.sample = anim_live.live(real_sample, gk, mod.DURATION, mod.CAST_AT)
            else:                                        # 05.11: its own pose, a part of the game's upper body on top, nothing moving as one piece
                anim_frostnova.sample = anim_arcanepush._lagged(anim_live.subtle(real_sample, gk, mod.DURATION, mod.CAST_AT, bones=(('Spine1', 'Spine2') if a['builder'] == 'holy_smite' else ('Spine1', 'Spine2', 'Neck', 'Head'))))
            try:
                data, moving = mod.build_kf(sk, bones, still=a.get('still', False))
            finally:
                anim_frostnova.sample = real_sample
            print('  animation %s (%s): %d bones, %d keyed, %d bytes' % (a['sequence'], folder, len(bones), len(moving), len(data)))
            for w in ANIM_WEAPONS:
                name = r'Win32\Characters\%s\Player_%s_Base%s.kfm' % (folder, g, w)
                k_hit = skills_dump.effective(arcs, name)
                if not k_hit:
                    raise SystemExit('missing ' + name)
                k = kfms.setdefault(name, kfm.Kfm(k_hit[0].read(k_hit[1])))
                # 05.11: a KFM only finds sequences in its own .kf (all of a KFM's rows name one file: a second file left the hero standing still with
                # every weapon, and a number past the end of the bow's .kf did the same), so each weapon's .kf gets the sequence, at its own number
                kf_name = r'Win32\Characters\%s\Player_%s_Base%s.kf' % (folder, g, w)
                kf_hit = skills_dump.effective(arcs, kf_name)
                if not kf_hit:
                    raise SystemExit('missing ' + kf_name)
                gw = bases.setdefault(kf_name, nif_write.Nif(kf_hit[0].read(kf_hit[1])))
                seq_count = sum(b.type == 'NiControllerSequence' for b in gw.blocks)
                _merge_anim_nif(gw, nif_write.Nif(data))
                kf_bytes = gw.encode()
                files[kf_name] = (kf_name, kf_bytes, 'own sequences appended')
                # the .kf grew: its row in Characters\Sizes.xml must give the new size (the loader reads that many bytes)
                hit = _find_parent(sizes_doc.root, lambda x: (x.get('m_ID') or '').lower() == kf_name.lower())
                if not hit:
                    raise SystemExit('%s not in %s' % (kf_name, CHAR_SIZES))
                parent, node = hit
                grow = len(kf_bytes) - int(node.get('m_Size'))
                node.set('m_Size', str(len(kf_bytes)))
                node.set('m_TotalSize', str(int(node.get('m_TotalSize')) + grow))
                if parent.get('m_TotalSize') is not None:
                    parent.set('m_TotalSize', str(int(parent.get('m_TotalSize')) + grow))
                k.add_sequence(None, k.seqs[0]['file'], index=seq_count)
                files[name] = (name, k.encode(), os.path.basename(k_hit[0].path))
                # Keep CharacterAnimationData aligned with the KFM.
                kfm_name = '%s/Player_%s_Base%s.kfm' % (folder, g, w)
                entry = next(x for x in walk(anims_doc.root) if (x.get('Name') or '').lower() == kfm_name.lower())
                count_key = next(k_ for k_, v in entry.attrs if str(k_) == 'H_AC669053')
                tpl = entry.children[-1]
                anim = clone(tpl)
                keys = [('start', 0.0)] + ([('Cast', mod.CAST_AT)] if mod.CAST_KEY else []) + [('end', mod.DURATION)]
                anim.attrs = [(k_, a['sequence'] if str(k_) == 'Name' else num(mod.DURATION) if str(k_) == 'H_00E319E6'
                               else '2' if str(k_) == 'H_EFB5A072' else str(len(keys)) if str(k_) == 'H_7E264760' else v)
                              for k_, v in tpl.attrs]
                tk_tpl = tpl.children[0]
                anim.children = []
                for tname, ttime in keys:
                    t_ = clone(tk_tpl)
                    t_.attrs = [(k_, tname if str(k_) == 'Name' else num(ttime)) for k_, _ in tk_tpl.attrs]
                    anim.children.append(t_)
                entry.children.append(anim)
                entry.attrs = [(k_, str(int(v) + 1) if k_ == count_key else v) for k_, v in entry.attrs]
    for folder, g in ANIM_GENDERS:
        kf_name = r'Win32\Characters\%s\Player_%s_Base_BOW.kf' % (folder, g)
        name = r'Win32\Characters\%s\Player_%s_Base_BOW.kfm' % (folder, g)
        gw, k = bases[kf_name], kfms[name]
        entry = next(x for x in walk(anims_doc.root) if (x.get('Name') or '').lower() == ('%s/Player_%s_Base_BOW.kfm' % (folder, g)).lower())
        count_key = next(k_ for k_, v in entry.attrs if str(k_) == 'H_AC669053')
        for shot in BOW_SHOTS:
            src = next(b for b in gw.blocks if b.type == 'NiControllerSequence' and gw.strings[b['name']] == shot)
            row = next(x for x in entry.children if x.get('Name') == shot)
            for suffix, speed in BOW_TIERS:
                index = sum(b.type == 'NiControllerSequence' for b in gw.blocks)
                f = copy.deepcopy(src.f)
                f['name'] = gw.string(shot + suffix)
                f['frequency'] = speed
                gw.roots.append(gw.add('NiControllerSequence', **f))
                k.add_sequence(None, k.seqs[0]['file'], index=index)
                r2 = clone(row)
                r2.attrs = [(k_, shot + suffix if str(k_) == 'Name' else num(round(float(v) / speed, 3)) if str(k_) == 'H_00E319E6' else v) for k_, v in row.attrs]
                for t_ in r2.children:
                    t_.attrs = [(k_, v if str(k_) == 'Name' else num(round(float(v) / speed, 3))) for k_, v in t_.attrs]
                entry.children.append(r2)
                entry.attrs = [(k_, str(int(v) + 1) if k_ == count_key else v) for k_, v in entry.attrs]
        kf_bytes = gw.encode()
        files[kf_name] = (kf_name, kf_bytes, 'own sequences appended')
        parent, node = _find_parent(sizes_doc.root, lambda x: (x.get('m_ID') or '').lower() == kf_name.lower())
        grow = len(kf_bytes) - int(node.get('m_Size'))
        node.set('m_Size', str(len(kf_bytes)))
        node.set('m_TotalSize', str(int(node.get('m_TotalSize')) + grow))
        if parent.get('m_TotalSize') is not None:
            parent.set('m_TotalSize', str(int(parent.get('m_TotalSize')) + grow))
        files[name] = (name, k.encode(), 'own sequences appended')
    out.extend(files.values())
    out.append((CHAR_ANIMS, anims_doc.encode(), anims_src))
    out.append((CHAR_SIZES, sizes_doc.encode(), sizes_src))
    return out


def balance(doc, bal):
    """Apply config/balance.json to the game's own skills in one rpgstats_skillprototypes.xml (ranks 1-15).
    Returns {UUID: old English description} for the localisation."""
    skills = doc.root.find('Skills')
    old = {}
    for uuid, b in bal['skills'].items():
        sk = next((k for k in skills.children if k.get('UUID') == uuid), None)
        if sk is None:
            raise SystemExit('balance: no skill %r' % uuid)
        levels = [L for L in sk.children if 1 <= int(L.get('Level')) <= 15]
        old[uuid] = levels[0].get('Description')
        for op in b.get('ops', []):
            kind = op[0]
            for L in levels:
                lv = int(L.get('Level'))
                if kind == 'level_linear':                # the hero level each rank needs, op[1] at rank 1 .. op[2] at 15
                    L.set('RequiredLevel', str(int(round(op[1] + (op[2] - op[1]) * (lv - 1) / 14.0))))
                    continue
                if kind == 'cooldown_linear':             # 07.11: the cooldown, op[1] s at rank 1 .. op[2] s at 15 (Curse)
                    L.set('CooldownTime', num(int(round(op[1] + (op[2] - op[1]) * (lv - 1) / 14.0))))
                    continue
                if kind == 'cost_set':                    # 05.11: a passive pays nothing (Jump Attack)
                    L.set('EnduranceCost', num(op[1]))
                    continue
                if kind in ('cooldown_mul', 'cooldown_set', 'cost_mul'):
                    key = 'EnduranceCost' if kind == 'cost_mul' else 'CooldownTime'
                    if L.get(key) is None:
                        continue
                    v = float(L.get(key))
                    v = v * op[1] if kind.endswith('_mul') else op[1]
                    L.set(key, num(max(1, round(v))))
                    continue
                p = L.children[0]
                k, v = p.attrs[op[1] - 1]
                whole = '.' not in v                      # the game puts the value into the description as it is
                v = float(v)
                if kind == 'mul':
                    v = v * op[2]
                elif kind == 'add':
                    v = v + op[2]
                elif kind == 'linear':
                    v = op[2] + (op[3] - op[2]) * (lv - 1) / 14.0
                elif kind == 'set':
                    if str(lv) not in op[2]:
                        continue
                    v = op[2][str(lv)]
                else:
                    raise SystemExit('balance: unknown op %r' % kind)
                p.attrs[op[1] - 1] = (k, num(round(v) if whole else round(v, 1)))
        if 'desc_en' in b:
            for L in sk.children:
                if L.get('Description') == old[uuid]:
                    L.set('Description', b['desc_en'])
    return old


LIMB_TYPE, LIMB_KIND = 'H_33A6D371', 'H_690D188F'          # item attributes: 'Limb', 'Severed Arms' (names unknown, hashes)
MOD_NAME, MOD_VALUE = 'H_00302162', 'H_064B46FD'           # stat mod: 'MeleeDamagePercentileMods', value


def balance_limbs(doc, rules):
    """Multiplies the stat mods of the creature parts whose kind is in rules ({'Severed Arms': 2.0})."""
    n = 0

    def mods(node, f):
        nonlocal n
        for i, (a, v) in enumerate(node.attrs):
            if a == MOD_VALUE and node.get(MOD_NAME):
                node.attrs[i] = (a, num(max(1, round(float(v) * f))))   # the hashed name object is kept
                n += 1
        for k in node.children:
            mods(k, f)

    def walk(node):
        if node.get(LIMB_TYPE) == 'Limb' and node.get(LIMB_KIND) in rules:
            mods(node, rules[node.get(LIMB_KIND)])
            return
        for k in node.children:
            walk(k)
    walk(doc.root)
    if not n:
        raise SystemExit('balance: no creature part mods found (attribute hashes changed?)')
    return n


def balance_trainers(doc, bal):
    """config/balance.json purchases at the trainers. The game's scripts open its own closed purchases (13/15,
    Available="False") by the record's number in the trainer's list, so no game row may move: new purchases go to
    the end of the list (the trainer offers a skill's purchases in list order, and ours are the higher caps), and a
    game purchase to take away (trainer_remove: the expansion's 7/8) is turned in place into the skill's first ladder
    step. Returns the number of rows added or turned."""
    added = 0
    trainers = doc.root.find('Trainers').children
    lad = bal.get('trainer_ladders', {})
    steps_of = lad.get('skills', {})
    turn = {(sk, c) for sk, c in bal.get('trainer_remove', {}).get('rows', [])}

    def put(trainer, src, skill, cap, gold, level):
        x = clone(src)
        for k, v in (('Skill', skill), ('NewMaxLevel', str(cap)), ('GoldCost', str(gold)),
                     ('RequiredLevel', str(level)), ('Available', 'True'),
                     ('Description', 'You will be able to study %s in more detail.' % skill)):
            x.set(k, v)
        trainer.children.append(x)

    for trainer in trainers:
        rows = trainer.children
        # the odd caps become the first step, in place
        for x in rows:
            key = (x.get('Skill'), int(x.get('NewMaxLevel') or 0))
            if key in turn and key[0] in steps_of:
                cap, gold, level = steps_of[key[0]][0]
                if not any(y.get('Skill') == key[0] and int(y.get('NewMaxLevel')) == cap for y in rows):
                    for k, v in (('NewMaxLevel', str(cap)), ('GoldCost', str(gold)), ('RequiredLevel', str(level)),
                                 ('Available', 'True')):
                        x.set(k, v)
                    added += 1
        # higher caps of game skills that stopped at 13 (weapon styles)
        for t in bal.get('trainer_extra', {}).get('groups', []):
            for skill in t['skills']:
                have = [x for x in trainer.children if x.get('Skill') == skill]
                if any(x.get('NewMaxLevel') == str(t['cap']) for x in have):
                    continue
                src = next((x for x in have if x.get('NewMaxLevel') == str(t['after_cap'])), None)
                if src is not None:
                    put(trainer, src, skill, t['cap'], t['gold'], t['required_level'])
                    added += 1
        # ladders: every listed trainer sells each step
        if trainer.get('UUID') in lad.get('trainers', []):
            for skill, steps in steps_of.items():
                for cap, gold, level in steps:
                    if not any(x.get('Skill') == skill and int(x.get('NewMaxLevel')) == cap for x in trainer.children):
                        put(trainer, trainer.children[-1], skill, cap, gold, level)
                        added += 1
    return added


def balance_localisation(doc, bal, old):
    """Russian lines for the changed descriptions: a new line for a new English text, or a new translation"""
    root = doc.root
    by_orig = {d.children[0].text: d for d in root.children}
    tpl = next(d for d in root.children if d.children[2].text == 'Description')
    for uuid, b in bal['skills'].items():
        if 'name_ru' in b:                                 # the skill's shown name (keyed by its English Name)
            name = by_orig.get(b.get('name_en', uuid))
            if name is None:
                raise SystemExit('balance: no Russian name line for %s' % uuid)
            name.children[1].text = b['name_ru']
        line = by_orig.get(old[uuid])
        if 'desc_en' in b:
            d = clone(tpl)
            d.children[0].text = b['desc_en']
            d.children[1].text = b.get('desc_ru') or (line.children[1].text if line is not None else b['desc_en'])
            root.children.append(d)
        elif 'desc_ru' in b:
            if line is None:
                raise SystemExit('balance: no Russian line for %s' % uuid)
            line.children[1].text = b['desc_ru']


SUMMONS_GFX = r'GUI\Win32\summons.gfx'
# (offset in the unpacked movie, game byte, ours, what): the summon portraits row of the HUD grows to the right and
# runs into the target's health bar with more than one summon (Div2Fix SummonLimit); make it grow to the left.
# AVM1 of DefineSprite 50 (gfx_avm1.py): addSummon "xpos = xpos + icon.sizer_mc._width", removeSummon
# "xpos = xpos - ..." and the re-layout "icon[i]._x = icon[i-1]._x + icon[i-1].sizer_mc._width".
SUMMONS_LEFT = [(7392, 0x47, 0x0B, 'addSummon: next x = x - width'),
                (8110, 0x0B, 0x47, 'removeSummon: x = x + width'),
                (8360, 0x47, 0x0B, 'removeSummon: icon[i].x = icon[i-1].x - width')]


FROST_TRIM = 0.2      # 08.11 the user: the sound lagged a little; its impact is at ~1.0 s, the wave goes out at OwnDelay57 860 ms


def frost_cast_sound(path):
    """Frost Nova's cast WAV: the first FROST_TRIM seconds cut, a 20 ms fade-in so the cut does not click"""
    import io
    import wave
    w = wave.open(path)
    rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
    if sw != 2:
        raise SystemExit(path + ': expected 16-bit PCM')
    a = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, ch).astype(np.float32)
    a = a[int(FROST_TRIM * rate):]
    fade = int(0.02 * rate)
    a[:fade] *= np.linspace(0, 1, fade)[:, None]
    out = io.BytesIO()
    o = wave.open(out, 'wb')
    o.setnchannels(ch); o.setsampwidth(2); o.setframerate(rate)
    o.writeframes(np.clip(a, -32768, 32767).astype(np.int16).tobytes())
    o.close()
    return out.getvalue()


def summons_layout(arcs):
    """summons.gfx with the portraits row growing to the left (CFX: zlib after the 8-byte header, which holds the
    unpacked length). Refuses when any byte differs from the game's."""
    hit = skills_dump.effective(arcs, SUMMONS_GFX)
    if not hit:
        sys.exit('no ' + SUMMONS_GFX)
    d = hit[0].read(hit[1])
    if d[:3] != b'CFX':
        sys.exit(SUMMONS_GFX + ': not a compressed GFX')
    n = struct.unpack_from('<I', d, 4)[0]
    u = bytearray(d[:8] + zlib.decompress(d[8:])[:n - 8])
    for at, game, ours, what in SUMMONS_LEFT:
        if u[at] != game:
            sys.exit('%s: byte %d is %02X, expected %02X (%s)' % (SUMMONS_GFX, at, u[at], game, what))
        u[at] = ours
    return [(SUMMONS_GFX, bytes(u[:8]) + zlib.compress(bytes(u[8:]), 9), os.path.basename(hit[0].path))]


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    game, out_dir = argv[1], argv[2]
    cfg = json.load(open(os.path.join(os.path.dirname(HERE), 'config', 'skills.json'), encoding='utf-8'))
    bal = json.load(open(os.path.join(os.path.dirname(HERE), 'config', 'balance.json'), encoding='utf-8'))
    boost = json.load(open(os.path.join(os.path.dirname(HERE), 'config', 'boosters.json'), encoding='utf-8'))
    global EXTRA_RANKS
    EXTRA_RANKS = int(boost.get('extra_ranks', 0))
    packed = os.path.join(game, 'Data', 'Win32', 'Packed')
    arcs = [Dv2(p) for p in skills_dump.archive_order(packed, skip=(ARCHIVE.lower(), DATA_ARCHIVE.lower()))]
    entries = []                                   # (name, bytes, source archive)

    for ep in EPISODES:
        for fname, fn in (('rpgstats_skillprototypes.xml', add_prototype), ('rpgstats_skilltree.xml', add_tree_entry),
                          ('DefaultSkills.xml', add_default)):
            name = r'Episodes\%s\%s' % (ep, fname)
            doc, src = load(arcs, name)
            if fname == 'rpgstats_skilltree.xml':       # 09.11: game skills moved (config tree_moves), before ours are added
                move_tree_entries(doc, cfg.get('tree_moves', []))
            for s in cfg['skills']:
                fn(doc, s)
            if fname == 'rpgstats_skillprototypes.xml':
                old_desc = balance(doc, bal)
            entries.append((name, doc.encode(), src))
            if fname == 'rpgstats_skilltree.xml' and ep == EPISODES[0]:
                tree = doc.root
    # 07.11: the necromancer's creature parts (items of type Limb): balance.json "limbs" = {subtype: factor} on every
    # percent/flat mod of those items (Severed Arms: melee/magic damage and health %)
    if bal.get('limbs'):
        for ep in EPISODES:
            name = r'Episodes\%s\rpgstats_itemprototypes.xml' % ep
            doc, src = load(arcs, name)
            print('  %s: %d creature part mods changed (balance.json limbs)' % (name, balance_limbs(doc, bal['limbs'])))
            entries.append((name, doc.encode(), src))
    for ep in EPISODES:
        name = r'Episodes\%s\rpgstats_itemmagicaleffects.xml' % ep
        doc, src = load(arcs, name)
        print('  %s: %d skill boosters' % (name, add_boosters(doc, boost)))
        entries.append((name, doc.encode(), src))
    name = r'Localisation\Russian\ItemsGenerators_Localisation.xml'
    doc, src = load(arcs, name)
    add_booster_names(doc, boost)
    entries.append((name, doc.encode(), src))
    name = r'Localisation\Russian\Skills_Localisation.xml'
    doc, src = load(arcs, name)
    for s in cfg['skills']:
        add_localisation(doc, s)
    balance_localisation(doc, bal, old_desc)
    entries.append((name, doc.encode(), src))
    for ep in EPISODES:
        name = TRAINERS % ep
        doc, src = load(arcs, name)
        for s in cfg['skills']:
            add_trainers(doc, s)
        print('  %s: %d higher caps of game skills (balance.json)' % (name, balance_trainers(doc, bal)))
        entries.append((name, doc.encode(), src))
    for s in cfg['skills']:
        entries += icons(arcs, s)
    entries.append(buff_icon(arcs))
    entries.append(buff_icon(arcs, r'Win32\Textures\Buff_LevelUp.nif', 'blessing'))     # 06.11: Div2Fix bless_name
    icon_files = [n.rsplit('\\', 1)[-1] for n, _, _ in entries if n.lower().endswith('.nif')]
    fx, fx_textures = effects(arcs, cfg, texture_names(arcs))
    entries += animations(arcs, cfg)
    entries += summons_layout(arcs)
    # 08.11 the user: the DragonWizard T2 set's gloves shine silver (the game's arms specular map is blue, the rest gold):
    # the arms' specular map takes the gold hue of the arms' colour map, 30 % towards grey (tools/tex_retint.py)
    import tex_retint
    # 08.11: a scan of the 50 armour sets (torso vs parts): also NewOrder T2/T3 arms have a grey specular on gold armour
    for sex, kit in ((s, k) for s in ('F', 'M') for k in ('DragonWizard_T2', 'NewOrder_T2', 'NewOrder_T3')):
        sm, dm = (r'Win32\Textures\%s_Arms_%s_%s.nif' % (sex, kit, k) for k in ('SM', 'DM'))
        hs, hd = skills_dump.effective(arcs, sm), skills_dump.effective(arcs, dm)
        if not hs or not hd:
            print('  %s: not in the game data, skipped' % sm)
            continue
        hue = 0.7 * tex_retint.mean_hue(hd[0].read(hd[1])) + 0.3
        entries.append((sm, tex_retint.retint(hs[0].read(hs[1]), hue), os.path.basename(hs[0].path)))
        print('  %s: gold specular (hue %.2f %.2f %.2f)' % (sm, hue[0], hue[1], hue[2]))
    # 08.11 the user: a skill cell grows and comes to the front under the mouse (tools/gfx_hoverzoom.py)
    import gfx_hoverzoom
    tree_gfx = r'GUI\Win32\skillTree.gfx'
    hit = skills_dump.effective(arcs, tree_gfx)
    if not hit:
        sys.exit('no ' + tree_gfx)
    import gfx_fade
    entries.append((tree_gfx, gfx_fade.patch(gfx_hoverzoom.patch(hit[0].read(hit[1]))), os.path.basename(hit[0].path)))
    print('  %s: hover zoom %d %% + fade-in' % (tree_gfx, gfx_hoverzoom.ZOOM))
    # 08.11 the user's UI idea 5: the windows fade in (tools/gfx_fade.py)
    for w in gfx_fade.WINDOWS:
        if w == 'skillTree':
            continue
        name = 'GUI\\Win32\\%s.gfx' % w
        hit = skills_dump.effective(arcs, name)
        if not hit:
            print('  %s: not in the game data, no fade' % name)
            continue
        entries.append((name, gfx_fade.patch(hit[0].read(hit[1])), os.path.basename(hit[0].path)))
    print('  %d window movies fade in (%d ms)' % (len(gfx_fade.WINDOWS), gfx_fade.FADE_MS))
    entries += fx
    index, src = texture_index(arcs, icon_files + fx_textures)
    entries.append((TEXTURE_INDEX, index, src))
    sheet, src = tree_sheet(arcs, cfg, tree)
    if sheet != skills_dump.effective(arcs, TREE_SHEET)[0].read(skills_dump.effective(arcs, TREE_SHEET)[1]):
        entries.append((TREE_SHEET, sheet, src))          # unchanged when every new cell is below the sheet

    # every XML must decode again and contain what we added
    for name, data, _ in entries:
        if name.endswith('.xml'):
            d2xml.Doc(data, NAMES)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, ARCHIVE)
    plain = sorted(((n, d) for n, d, _ in entries), key=lambda e: e[0].lower())
    pack(plain, out)
    verify(plain, out)
    # 07.11: the XML data again in a small archive of its own. The game reads small archives whole into memory when it
    # registers them (FOV_Patch_2, MainDataStartup ... MainDataStreaming 4.3 MB) and only the index of the 22 MB one; the
    # skill prototypes were then taken from a copy in memory (78 original entries, a crash at 0x89F9B0 on loading a save
    # that has our skills). MageData.dv2 comes before MageExpansion.dv2 (alphabetical) in the overlay.
    # not Win32\Characters\*.xml: they go with the hero's KF/KFM of MageExpansion.dv2, which lose to Characters.dv2/KFMs.dv2
    data = [(n, d) for n, d in plain if n.lower().endswith('.xml') and not n.lower().startswith('win32\\characters\\')]
    pack(data, os.path.join(out_dir, DATA_ARCHIVE))
    verify(data, os.path.join(out_dir, DATA_ARCHIVE))
    print('%s: %d XML files, %d bytes' % (DATA_ARCHIVE, len(data), os.path.getsize(os.path.join(out_dir, DATA_ARCHIVE))))
    # the own sound is not in the archive: Div2Fix plays it from bin\MageExpansion (install.cmd copies it)
    # 08.11 the user: Frost Nova's cast is Pixabay DRAGON-STUDIO 'ice spell impact' (assets/sounds/ice-spell-impact.wav),
    # the synthesised one (sfx_frostnova) only when that file is missing
    ice = os.path.join(os.path.dirname(HERE), 'assets', 'sounds', 'ice-spell-impact.wav')
    with open(os.path.join(out_dir, 'FrostNova.wav'), 'wb') as f:
        f.write(frost_cast_sound(ice) if os.path.isfile(ice) else sfx_frostnova.wav(sfx_frostnova.synth()))
    with open(os.path.join(out_dir, 'ChainLightning.wav'), 'wb') as f:
        f.write(sfx_chain.wav(sfx_chain.best()))
    with open(os.path.join(out_dir, 'Drain.wav'), 'wb') as f:
        f.write(sfx_drain.wav(sfx_drain.best()))
    with open(os.path.join(out_dir, 'Blessing.wav'), 'wb') as f:
        f.write(sfx_bless.wav(sfx_bless.best()))
    with open(os.path.join(out_dir, 'BattleCry_M.wav'), 'wb') as f:
        f.write(sfx_cry.wav(sfx_cry.male()))
    with open(os.path.join(out_dir, 'BattleCry_F.wav'), 'wb') as f:
        f.write(sfx_cry.wav(sfx_cry.female()))
    with open(os.path.join(out_dir, 'HolySmite.wav'), 'wb') as f:
        f.write(sfx_smite.wav(sfx_smite.best()))
    with open(os.path.join(out_dir, 'ArcaneShield.wav'), 'wb') as f:
        f.write(sfx_shield.wav(sfx_shield.best()))
    with open(os.path.join(out_dir, 'ArcanePush.wav'), 'wb') as f:
        f.write(sfx_push.wav(sfx_push.best()))
    with open(os.path.join(out_dir, 'IceBolt.wav'), 'wb') as f:
        f.write(sfx_icebolt.wav(sfx_icebolt.best()))
    with open(os.path.join(out_dir, 'manifest.txt'), 'w', encoding='utf-8') as f:
        f.write('%s  sha256 %s\n' % (ARCHIVE, hashlib.sha256(open(out, 'rb').read()).hexdigest()))
        for n, d, src in sorted(entries, key=lambda e: e[0].lower()):
            f.write('  %-64s %8d  from %s\n' % (n, len(d), src))
    print(open(os.path.join(out_dir, 'manifest.txt'), encoding='utf-8').read())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
