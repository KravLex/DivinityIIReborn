r"""Frost Nova's own effect, made from nothing but code: geometry, animation and textures (no game effect is copied).

  py -3 -I fx_frostnova.py preview <out.png>     the four textures side by side

The effect (.item, Gamebryo 20.3.0.9, written by nif_write) follows the layout of the game's effect files:
CStreamableAssetData, "Scene Root" (scale 0.01, MaxBound, z-buffer + vertex colour properties) -> AROOT_<name>
(UserPropBuffer "KFAccumRoot") -> "<AROOT> NonAccum" -> parts; the animation is a NiControllerSequence in the same file
(transform keys per part node; emissive colour keys through a NiMaterialColorController on each material, as the
game's own effects fade). Blending is additive, so a part disappears when its emissive colour goes to black.
Parts (units: centimetres under the 0.01 root; the effect sits at the caster's feet, Z up, -Y forward):
  HandL/R - cold glows in the palms, following the hands of the cast animation until the slam (03.11)
  MistL/R - a soft frost mist trailing the palms (03.11)
  Flash  - billboard glow, fast bloom and fade at the slam
  Ring   - flat shock ring on the ground, grows to the skill radius and fades
  Frost  - frost pattern on the ground, grows behind the ring, lingers and melts slowly (03.11: LINGER)
  Haze   - cold haze under the cracks, lingers with them
  Shards - ice blades standing in a circle, grow out of the ground with the ring
  Debris - ice splinters thrown outwards in arcs by the slam (03.11)
  Dust   - snow dust clouds rising behind the ring (03.11)
Everything of the burst starts at CAST_AT, the "Cast" text key of the hero's animation (the effect itself starts with
the animation: Div2Fix sets both at the same moment).
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_frostnova               # noqa: E402
import icon_art                     # noqa: E402
import nif_write
import psys                    # noqa: E402

NAME = 'FrostNova_Burst'
CAST_AT = anim_frostnova.CAST_AT     # the slam: the burst starts here
DURATION = anim_frostnova.DURATION  # 03.11: the effect is stopped with the cast animation (Div2Fix), so it fits in it
LINGER = DURATION - CAST_AT - 0.05  # the frost on the ground melts away within this after the slam
# centre of the left palm in the cast animation (anim_frostnova.sample, mean of the female and male skeletons, cm,
# +X the hero's left, -Y forward, Z from the ground); the right palm is the mirror (X negated)
PALM = [(0.00, 26, -3, 92), (0.10, 24, -18, 97), (0.20, 13, -34, 113), (0.30, 7, -34, 123), (0.40, 28, -40, 129),
        (0.50, 54, -14, 181), (0.60, 37, 20, 199), (0.70, 36, 32, 196), (0.80, 47, 5, 189), (0.86, 32, -40, 76),
        (0.90, 30, -44, 71)]
RADIUS_CM = 800.0                   # the blade size is made for this radius; build(radius=...) spreads the effect wider
MAX_RADIUS_CM = 1200.0              # 03.11: the radius grows with the rank (6/9/12 m, skills.json Param3)
TEXTURES = {'ring': 'fx_frostnova_ring', 'frost': 'fx_frostnova_frost', 'shard': 'fx_frostnova_shard',
            'flash': 'fx_frostnova_flash', 'haze': 'fx_frostnova_haze', 'rime': 'fx_frostnova_rime', 'flake': 'fx_frostnova_flake'}
RING_TILES = 16                     # the ring band texture repeats this many times around the circle
SIZE = 512
# 03.11: a part at scale 0 (hidden until the slam) made the whole effect vanish (a zero scale breaks the inverse
# transforms and bounds); hidden parts are tiny, dark and moved under the ground instead
MIN_SCALE = 0.01
# 03.11: the ring and the cracks lie this high over the caster's feet (at 2-3 cm small bumps and grass hid them) and
# glow in the blades' blue (nearly white, they turned yellow-green over the red ground)
LIFT = 14.0
GROUND_TINT = (0.12, 0.42, 1.0)
# bound of the whole burst (centre, radius, min, max), in the file's units: the ring reaches 1.05 R, flash 1.2 m up
BOUND = (0.0, 0.0, 130.0, MAX_RADIUS_CM * 1.12, -MAX_RADIUS_CM * 1.05, -MAX_RADIUS_CM * 1.05, 0.0,
         MAX_RADIUS_CM * 1.05, MAX_RADIUS_CM * 1.05, 260.0)          # 03.11: a 1.5 m bound did not change the bright disc


# ---------------------------------------------------------------- textures (RGBA float 0..1, alpha = brightness)
def _grid(size):
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    u = (xx + 0.5) / size * 2 - 1
    v = (yy + 0.5) / size * 2 - 1
    return u, v, np.hypot(u, v), np.arctan2(v, u)


def _rgba(rgb, alpha_gain=1.6):
    rgb = 1 - np.exp(-2.2 * np.clip(rgb, 0, None))          # 03.11: 1.3 looked pale over the bright red scenes
    alpha = np.clip(rgb.max(-1) * alpha_gain, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1).astype(np.float32)


CYAN = np.array([0.55, 0.92, 1.00])
TEAL = np.array([0.10, 0.62, 0.70])
WHITE = np.array([0.96, 1.00, 1.00])
DEEP = np.array([0.10, 0.38, 1.00])


def tex_ring(size=SIZE, seed=11):
    """one tile of the ring band (02.11: a geometry ring, crisp at any size): x runs along the ring (tiles repeat
    around it), y across the band from the outer edge (top) to the inner edge (bottom); a bright frost edge with ice
    spikes pointing outwards and a cold glow trailing inwards"""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    x = (xx + 0.5) / size                                 # 0..1 along (wraps)
    y = (yy + 0.5) / size                                 # 0 outer .. 1 inner
    edge_y = 0.32
    crisp = np.exp(-((y - edge_y) / 0.035) ** 2)
    rough = 0.75 + 0.25 * np.sin(x * 2 * np.pi * 11 + 1.3) * np.sin(x * 2 * np.pi * 5)
    glow = np.exp(-np.clip(y - edge_y, 0, None) / 0.22) * (y > edge_y) * 0.55
    sp = np.zeros_like(x)                                 # spikes: 4 per tile, pointing to the outer edge (y = 0)
    for k in range(4):
        cx = (k + 0.5) / 4 + rng.uniform(-0.04, 0.04)
        hgt = rng.uniform(0.18, 0.3)
        w = 0.035 * np.clip((y - (edge_y - hgt)) / hgt, 0, 1)
        dx = np.minimum(np.abs(x - cx), 1 - np.abs(x - cx))
        sp = np.maximum(sp, ((dx < w) & (y < edge_y + 0.02) & (y > edge_y - hgt)).astype(np.float32)
                        * (0.6 + 0.4 * (dx < 0.4 * w)))
    sp = icon_art._blur(sp, 1)
    rgb = ((crisp * rough)[..., None] * (0.55 * WHITE + 0.45 * CYAN) * 1.5 + glow[..., None] * TEAL
           + sp[..., None] * (0.5 * WHITE + 0.5 * CYAN) * 1.3)
    rgb *= (np.clip(1 - y, 0, 1) ** 0.3)[..., None]
    return _rgba(rgb)


def tex_frost(size=SIZE, seed=12):
    """the profile of one frost crack (02.11: cracks are ribbons on the ground, crisp at any size): y across the
    ribbon, x along it; a sharp bright core, a cold glow around it and small frost needles"""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    x = (xx + 0.5) / size
    y = (yy + 0.5) / size * 2 - 1                         # -1 .. 1 across
    core = np.exp(-(y / 0.08) ** 2)
    glow = np.exp(-(y / 0.45) ** 2) * 0.45
    needles = np.zeros_like(x)
    for k in range(14):                                   # short needles growing off the crack
        cx = rng.uniform(0.05, 0.95)
        side = rng.choice([-1, 1])
        t = np.clip(side * y, 0, 1)
        off = np.abs(x - (cx + 0.15 * t))
        needles += np.exp(-(off / 0.006) ** 2) * (t < rng.uniform(0.4, 0.9)) * (side * y > 0)
    rgb = (core[..., None] * (0.5 * WHITE + 0.5 * CYAN) * 1.6 + glow[..., None] * TEAL
           + np.clip(needles, 0, 1)[..., None] * CYAN * 0.8)
    return _rgba(rgb, 1.4)


def tex_rime(size=SIZE, seed=15):
    """hoarfrost on the ground (03.11, instead of the additive teal haze that looked like a soapy square): a white and
    pale blue crystal crust with a ragged edge, drawn alpha blended so it covers the ground instead of tinting it"""
    rng = np.random.default_rng(seed)
    u, v, r, a = _grid(size)
    s = size / 512.0
    n_low = icon_art._blur(icon_art._noise(size, int(60 * s), rng), int(20 * s))
    n_mid = icon_art._noise(size, int(14 * s), rng)
    n_fine = icon_art._noise(size, max(1, int(2 * s)), rng)
    n_col = icon_art._blur(icon_art._noise(size, int(30 * s), rng), int(10 * s))
    n_edge = icon_art._noise(size, int(6 * s), rng)
    edge = 0.70 + 0.18 * (n_low - n_low.min()) / (np.ptp(n_low) + 1e-6) + 0.05 * n_edge
    mask = np.clip((edge - r) / 0.015, 0, 1)
    # feathery crystals growing out of the edge (rays in polar coordinates, bent by the noise)
    rays = np.clip(np.cos(a * 160 + 25 * n_low + 6 * n_mid), 0, 1) ** 10
    fringe = rays * np.clip(1 - np.abs(r - edge) / 0.07, 0, 1)
    crust = np.clip(0.35 + 0.5 * n_mid + 0.35 * (n_fine > 0.62), 0, 1)
    sparkle = (n_fine > 0.9).astype(np.float32)
    pale = np.array([0.62, 0.82, 1.0])
    rgb = WHITE[None, None, :] * (0.7 + 0.3 * n_col[..., None]) + pale[None, None, :] * (0.3 - 0.3 * n_col[..., None])
    rgb = np.clip(rgb * (0.8 + 0.2 * crust[..., None]) + sparkle[..., None] * 0.3, 0, 1)
    alpha = np.clip(mask * (0.5 + 0.35 * crust) + fringe * 0.85, 0, 0.92)
    return np.concatenate([rgb, alpha[..., None]], -1).astype(np.float32)


def tex_haze(size=SIZE, seed=14):
    """a soft cold haze on the ground under the cracks (soft on purpose)"""
    rng = np.random.default_rng(seed)
    u, v, r, a = _grid(size)
    s = size / 512.0
    haze = icon_art._blur((icon_art._noise(size, int(40 * s), rng) > 0.5).astype(np.float32), int(18 * s))
    rgb = (0.35 + 0.65 * haze)[..., None] * TEAL * 0.6 * (np.clip(1 - r, 0, 1) ** 1.2)[..., None]
    return _rgba(rgb, 1.2)


def tex_shard(size=SIZE, seed=13):
    """one ice blade, point up (the quad is 1 wide x 2 high, base at the bottom): wide frosted body with lit and shaded
    facets, rime crust along the edges, cracks and snow specks inside, frost mist at the base (02.11: thicker, frostier)"""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    x = (xx + 0.5) / size * 2 - 1                         # -1 .. 1 across
    h = 1 - (yy + 0.5) / size                             # 0 at the base, 1 at the tip
    # the ice fills exactly the triangle of a pyramid face (UV base corners (0,1),(1,1), tip (0.5,0)); 02.11: a
    # narrower shape left the lower corners of every face empty (a notch in each blade)
    half = np.clip(1 - h, 0, 1)
    inside = (np.abs(x) < half).astype(np.float32)
    lit = np.where(x < 0, 1.0, 0.6)
    grain = 0.75 + 0.25 * icon_art._noise(size, int(10 * s), rng)
    body = inside * lit * grain * (0.6 + 0.4 * h)
    # rime: a rough bright crust along both edges
    rough = 0.5 + 0.5 * icon_art._noise(size, int(5 * s), rng)
    rim = np.exp(-((np.abs(x) - half) / (0.035 * rough + 0.01)) ** 2) * (h < 0.99)
    ridge = np.exp(-(x / 0.012) ** 2) * inside
    # cracks inside the ice
    cracks = np.zeros((size, size), np.float32)
    for k in range(9):
        y0 = rng.uniform(0.1, 0.8); x0 = rng.uniform(-0.3, 0.3) * (1 - y0)
        ang = rng.uniform(-1.2, 1.2) + np.pi / 2
        x1 = x0 + 0.25 * np.cos(ang); y1 = y0 + 0.25 * np.sin(ang)
        p0 = ((x0 + 1) / 2 * size, (1 - y0) * size); p1 = ((x1 + 1) / 2 * size, (1 - y1) * size)
        d, t = icon_art._segment(xx, yy, p0[0], p0[1], p1[0], p1[1])
        cracks += np.exp(-(d / (1.2 * s)) ** 2) * (1 - t)
    cracks *= inside
    # snow specks frozen in the ice
    specks = (icon_art._noise(size, int(3 * s), rng) > 0.86).astype(np.float32) * inside
    mist = icon_art._blur(((h < 0.18) * (np.abs(x) < 0.9)).astype(np.float32), int(26 * s)) * 0.5
    mist *= np.clip(1 - np.abs(x), 0, 1) ** 1.5 * np.clip(h / 0.04, 0, 1)      # no hard edge at the quad's sides/base
    # 03.11: solid ice (alpha blended, opaque): deep blue at the base, saturated cyan towards the tip, an inner glow
    # along the ridge, white only on the rime, cracks and snow specks; the alpha is the blade's shape
    body_rgb = DEEP[None, None, :] * (1 - h[..., None]) + CYAN[None, None, :] * h[..., None]
    glow_in = icon_art._blur(ridge, int(10 * s)) * 1.5
    rgb = (body[..., None] * body_rgb * 1.25
           + glow_in[..., None] * CYAN * 0.6
           + (1.0 * rim + 0.5 * ridge)[..., None] * (0.7 * WHITE + 0.3 * CYAN)
           + (0.7 * cracks + 0.8 * specks)[..., None] * WHITE)
    rgb = np.clip(rgb, 0, 1)
    alpha = np.clip(inside * 0.97 + rim * 0.9, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1).astype(np.float32)


def tex_flash(size=SIZE):
    u, v, r, a = _grid(size)
    core = np.exp(-(r / 0.16) ** 2)
    halo = np.exp(-(r / 0.5) ** 2) * 0.5
    rays = (np.clip(np.cos(a * 4), 0, 1) ** 30 + 0.6 * np.clip(np.cos(a * 4 + np.pi / 4), 0, 1) ** 40) * np.exp(-(r / 0.75) ** 2)
    rgb = core[..., None] * WHITE * 1.6 + halo[..., None] * CYAN + rays[..., None] * (0.6 * WHITE + 0.4 * CYAN)
    rgb *= (r < 1)[..., None]
    return _rgba(rgb)


def tex_flake(size=SIZE):
    """05.11: a snowflake: six arms with side branches, a small hexagon in the middle; particles use it instead of the star"""
    u, v, r, a = _grid(size)
    seg = np.mod(a + np.pi / 6, np.pi / 3) - np.pi / 6      # fold into one arm, along +x
    x, y = r * np.cos(seg), np.abs(r * np.sin(seg))
    w = 0.028
    arm = np.exp(-(y / w) ** 2) * (x < 0.92)
    for at, ln in ((0.34, 0.2), (0.56, 0.17), (0.76, 0.11)):  # branches leaning outwards at 60 degrees
        bx, by = x - at, y
        along = bx * 0.5 + by * 0.866
        across = -bx * 0.866 + by * 0.5
        arm = np.maximum(arm, np.exp(-(across / (w * 0.85)) ** 2) * (along > 0) * (along < ln))
    hexa = np.exp(-((x - 0.17) / 0.024) ** 2) * (y < 0.11)
    shape = np.maximum(arm, hexa)
    rgb = shape[..., None] * (0.85 * WHITE + 0.15 * CYAN) * 1.7 + np.exp(-(r / 0.55) ** 2)[..., None] * CYAN * 0.12
    rgb *= (r < 1)[..., None]
    return _rgba(rgb)


def textures():
    """uint8 RGBA, as icon_art.mip_chain/dxt5 take them (floats 0..1 came out black, 02.11)"""
    tex = {TEXTURES['ring']: tex_ring(), TEXTURES['frost']: tex_frost(), TEXTURES['shard']: tex_shard(),
           TEXTURES['flash']: tex_flash(), TEXTURES['haze']: tex_haze(), TEXTURES['rime']: tex_rime(), TEXTURES['flake']: tex_flake()}
    return {k: (np.clip(v, 0, 1) * 255 + 0.5).astype(np.uint8) for k, v in tex.items()}


# ---------------------------------------------------------------- the NIF
IDENT = [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0]


def _quad(nif, verts, uvs, normal):
    xs, ys, zs = zip(*verts)
    box = [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]
    centre = [(box[0] + box[3]) / 2, (box[1] + box[4]) / 2, (box[2] + box[5]) / 2]
    radius = max(((x - centre[0]) ** 2 + (y - centre[1]) ** 2 + (z - centre[2]) ** 2) ** 0.5 for x, y, z in verts)
    d = {'group': 0, 'keep': 0, 'compress': 0, 'has_verts': 1, 'verts': [list(p) for p in verts], 'vflags': 1,
         'has_normals': 1, 'normals': [list(normal)] * len(verts), 'tangents': [], 'unk8': 0, 'centre': centre,
         'radius': radius, 'bound_kind': 2, 'box': box, 'has_colours': 0, 'colours': [], 'uvs': [uvs],
         'consistency': 0x4000, 'additional': -1, 'tri_points': 6 * (len(verts) // 4), 'has_tris': 1,
         'tris': [t for q in range(len(verts) // 4) for t in ([4 * q, 4 * q + 1, 4 * q + 2], [4 * q + 2, 4 * q + 3, 4 * q])],
         'match': []}
    d['nt'] = len(d['tris'])
    return nif.add('NiTriShapeData', d=d)


def _node(nif, type_, name, children=(), extra=(), props=(), translation=(0, 0, 0), rotation=None, scale=1.0,
          flags=0x116, **more):
    f = dict(name=nif.string(name), extra=list(extra), controller=-1, flags=flags, translation=list(translation),
             rotation=list(rotation or IDENT), scale=scale, properties=list(props), collision=-1,
             children=list(children), effects=[])
    f.update(more)
    return nif.add(type_, **f)


def _part(nif, name, texture, verts, uvs, normal, billboard=False, opaque=False, blend=False):
    """a node with a textured additive shape; returns (node index, shape name, material index)"""
    tex = nif.add('NiSourceTexture', name=-1, extra=[], controller=-1, external=1, file=nif.string(texture + '.tga'),
                  unknown_link=-1, pixel_layout=6, mipmap=1, alpha_format=3, static=1, direct=0, persist=0)
    slots = [{'has': 1, 'desc': {'source': tex, 'flags': 0x3200, 'has_tf': 0}}] + [{'has': 0}] * 8
    texp = nif.add('NiTexturingProperty', name=-1, extra=[], controller=-1, t={'flags': 4, 'count': 9, 'slots': slots,
                                                                                'shader': 0})
    # ONE + ONE (glows); opaque (03.11, the ice blades): SRC_ALPHA + INV_SRC_ALPHA, alpha test, z written
    alpha = nif.add('NiAlphaProperty', name=-1, extra=[], controller=-1, flags=0x12ED if opaque else 0x00ED if blend else 0x3001,
                    threshold=96 if opaque else 0)
    vcol = nif.add('NiVertexColorProperty', name=-1, extra=[], controller=-1, flags=0)
    mat = nif.add('NiMaterialProperty', name=nif.string(name + '_Mat'), extra=[], controller=-1, ambient=[0, 0, 0],
                  diffuse=[0, 0, 0], specular=[0, 0, 0], emissive=[1, 1, 1], glossiness=10.0, alpha=1.0)
    data = _quad(nif, verts, uvs, normal)
    # 03.11: HasEmittance made the engine light a bright disc on the ground around the caster (gone in a test with
    # no part marked); the game marks its effect quads, ours are much bigger, so none is marked
    emit = nif.add('NiBooleanExtraData', name=nif.string('HasEmittance'), value=1) if False else None   # 03.11: on no part at all (test: the disc went with it)
    shape_name = name + '_Shape'
    # both sides drawn, as the game's effect quads do (NiStencilProperty draw mode 3 = both; 02.11: the blades were
    # only visible from one side)
    stencil = nif.add('NiStencilProperty', name=-1, extra=[], controller=-1, flags=0x4D80, ref=0, mask=0xFFFFFFFF)
    shape = _node(nif, 'NiTriShape', shape_name, extra=[emit] if emit is not None else [], props=[mat, vcol, alpha, texp, stencil], flags=0x16,
                  data=data, skin=-1, materials=[], active_material=-1, dirty=1)
    for k in ('children', 'effects'):
        del nif.blocks[shape].f[k]
    zbuf = nif.add('NiZBufferProperty', name=-1, extra=[], controller=-1, flags=15 if opaque else 13)  # test (+write)
    upb = nif.add('NiStringExtraData', name=nif.string('UserPropBuffer'), value=nif.string('\r\nzMode10\r\n'))
    if billboard:
        node = _node(nif, 'NiBillboardNode', name, children=[shape], extra=[upb], props=[zbuf], flags=0x196,
                     billboard_mode=8)
    else:
        node = _node(nif, 'NiNode', name, children=[shape], extra=[upb], props=[zbuf])
    return node, shape_name, mat


def _ground_quad(half):
    return ([(-half, -half, 0), (half, -half, 0), (half, half, 0), (-half, half, 0)],
            [[0, 1], [1, 1], [1, 0], [0, 0]], (0, 0, 1))


def _shards(count=30, radius=1.0, width=0.09, height=0.19, seed=5):       # 02.11: 30, no gaps in the circle
    """ice blades standing on a circle (02.11: solid four-sided pyramids leaning outwards, not crossed planes);
    every face is a quad whose two top corners meet at the tip"""
    rng = np.random.default_rng(seed)
    verts, uvs = [], []
    for k in range(count):
        ang = 2 * np.pi * (k + rng.uniform(-0.3, 0.3)) / count
        r = radius * rng.uniform(0.7, 1.0)
        cx, cy = r * np.cos(ang), r * np.sin(ang)
        w = width * rng.uniform(0.7, 1.2) / 2
        hgt = height * rng.uniform(0.45, 1.25)
        lean = 0.35 * hgt
        tip = (cx + lean * np.cos(ang), cy + lean * np.sin(ang), hgt)
        rot = rng.uniform(0, np.pi / 2)
        base = [(cx + w * np.cos(rot + q * np.pi / 2), cy + w * np.sin(rot + q * np.pi / 2), 0.0) for q in range(4)]
        for q in range(4):
            verts += [base[q], base[(q + 1) % 4], tip, tip]
            uvs += [[0, 1], [1, 1], [0.5, 0], [0.5, 0]]
    return verts, uvs, (0, 0, 1)


def _blade(ang, rng, width=0.09, height=0.19):
    """one ice blade at the origin (unit size like _shards), leaning outwards along ang: a four-sided pyramid"""
    w = width * rng.uniform(0.7, 1.2) / 2
    hgt = height * rng.uniform(0.7, 1.15)
    lean = 0.35 * hgt
    tip = (lean * np.cos(ang), lean * np.sin(ang), hgt)
    rot = rng.uniform(0, np.pi / 2)
    base = [(w * np.cos(rot + q * np.pi / 2), w * np.sin(rot + q * np.pi / 2), 0.0) for q in range(4)]
    verts, uvs = [], []
    for q in range(4):
        verts += [base[q], base[(q + 1) % 4], tip, tip]
        uvs += [[0, 1], [1, 1], [0.5, 0], [0.5, 0]]
    return verts, uvs, (0, 0, 1)


def _annulus(inner=0.86, outer=1.04, segments=64, tiles=RING_TILES):
    """a flat ring band: x of the texture runs around (repeating), y from the outer (0) to the inner edge (1)"""
    verts, uvs = [], []
    for k in range(segments):
        a0, a1 = 2 * np.pi * k / segments, 2 * np.pi * (k + 1) / segments
        u0, u1 = tiles * k / segments, tiles * (k + 1) / segments
        verts += [(inner * np.cos(a0), inner * np.sin(a0), 0), (outer * np.cos(a0), outer * np.sin(a0), 0),
                  (outer * np.cos(a1), outer * np.sin(a1), 0), (inner * np.cos(a1), inner * np.sin(a1), 0)]
        uvs += [[u0, 1], [u0, 0], [u1, 0], [u1, 1]]
    return verts, uvs, (0, 0, 1)


def _cracks(count=22, seed=12, width=0.012):
    """frost cracks as flat ribbons from the centre outwards, with side branches; each ribbon narrows to its end
    (texture y across the ribbon, x along it)"""
    rng = np.random.default_rng(seed)
    verts, uvs = [], []

    def ribbon(p0, ang, length, w0, steps):
        p = np.array(p0, float)
        for i in range(steps):
            ang += rng.uniform(-0.25, 0.25)
            q = p + length / steps * np.array([np.cos(ang), np.sin(ang)])
            wa = w0 * (1 - i / steps) + 0.15 * w0
            wb = w0 * (1 - (i + 1) / steps) + 0.15 * w0
            n0 = np.array([-np.sin(ang), np.cos(ang)])
            for a in ((p - n0 * wa), (q - n0 * wb), (q + n0 * wb), (p + n0 * wa)):
                verts.append((float(a[0]), float(a[1]), 0.0))
            uvs.extend([[0, 1], [1, 1], [1, 0], [0, 0]])
            p = q

    for k in range(count):
        ang = 2 * np.pi * (k + rng.uniform(-0.3, 0.3)) / count
        start = rng.uniform(0.02, 0.08) * np.array([np.cos(ang), np.sin(ang)])
        length = rng.uniform(0.55, 0.95)
        ribbon(start, ang, length, width * rng.uniform(0.8, 1.3), 6)
        for b in range(2):
            t = rng.uniform(0.25, 0.7)
            bp = start + t * length * np.array([np.cos(ang), np.sin(ang)])
            ribbon(bp, ang + rng.choice([-1, 1]) * rng.uniform(0.4, 0.9), rng.uniform(0.1, 0.25), width * 0.6, 3)
    return verts, uvs, (0, 0, 1)


def _keys_scale(nif, times_scales, translation=(0, 0, 0)):
    data = nif.add('NiTransformData', keys={'rot': {'n': 0, 'type': None, 'keys': []},
                                            'trans': {'interp': None, 'keys': []},
                                            'scale': {'interp': 1, 'keys': [[t, max(s, MIN_SCALE)] for t, s in times_scales]}})
    return nif.add('NiTransformInterpolator', translation=list(translation), rotation=[1.0, 0, 0, 0],
                   scale=max(times_scales[0][1], MIN_SCALE), data=data)


def _keys_ts(nif, times_scales, times_pos):
    """scale and translation keys (linear)"""
    data = nif.add('NiTransformData', keys={'rot': {'n': 0, 'type': None, 'keys': []},
                                            'trans': {'interp': 1, 'keys': [[t] + list(p) for t, p in times_pos]},
                                            'scale': {'interp': 1, 'keys': [[t, max(s, MIN_SCALE)] for t, s in times_scales]}})
    return nif.add('NiTransformInterpolator', translation=list(times_pos[0][1]), rotation=[1.0, 0, 0, 0],
                   scale=max(times_scales[0][1], MIN_SCALE), data=data)


def _keys_emissive(nif, times_values, tint=(0.45, 0.8, 1.0)):
    # a cold glow: red and green kept below blue (02.11; 03.11 more saturated blue, the scenes are red)
    data = nif.add('NiPosData', keys={'interp': 1, 'keys': [[t, tint[0] * v, tint[1] * v, tint[2] * v]
                                                             for t, v in times_values]})
    return nif.add('NiPoint3Interpolator', value=[-3.4028235e38] * 3, data=data)



def _particles(nif, chill, C, R, tx):
    """04.11: volumetric particles (psys.py) on top of the flat parts: at the slam a wave of frost mist rolls out over
    the ground to the radius, ice crystals are thrown up and fall, and a low fog lingers over the whole area; on a
    target (chill) a puff of frost and a few flakes. Emitted once (CLAMP), kept in the world (left behind if one moves)."""
    if os.environ.get('MAGE_NO_PSYS') is not None:
        return []
    out = []
    if chill:
        out.append(psys.add(nif, 'ChillMist', tx('haze'), emitter=('sphere', 30), rate=60, stop=0.2, duration=Chill.DURATION,
                            life=0.8, life_var=0.15, speed=30, speed_var=15, radius=35, radius_var=10, grow=0.1, fade=0.5,
                            rotation=(0.5, 0.5), translation=(0, 0, 60), world_space=True,
                            colours=((0.0, (0.6, 0.85, 1.0, 0.8)), (1.0, (0.25, 0.45, 1.0, 0.0)))))
        out.append(psys.add(nif, 'ChillFlakes', tx('flake'), emitter=('sphere', 40), rate=80, stop=0.2,
                            duration=Chill.DURATION, life=0.9, life_var=0.2, speed=60, speed_var=30, radius=7, radius_var=2,
                            grow=0.05, fade=0.4, rotation=(2.0, 2.0), gravity=((0, 0, 1), -250), translation=(0, 0, 90),
                            world_space=True, colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.5, 0.8, 1.0, 0.0)))))
        return out
    # 04.11 second try (in game the wave stayed small around the hero and came late; only the crystals read): no drag
    # (it stopped the mist at once), faster and bigger, from a ring already around the hero; the fog starts with it
    wave = R / 0.55                                         # cm/s: the mist reaches the edge within its life
    out.append(psys.add(nif, 'FrostWave', tx('haze'), emitter=('cylinder', R * 0.12, 10), rate=900, start=max(0.0, C - 0.03),
                        stop=C + 0.1, duration=DURATION, life=0.7, life_var=0.1, speed=wave, speed_var=wave * 0.1,
                        declination=1.5708, declination_var=0.1, planar=0.0, planar_var=3.1416, radius=170,
                        radius_var=40, grow=0.04, fade=0.35, rotation=(0.4, 0.4), translation=(0, 0, 40),
                        world_space=True,
                        colours=((0.0, (0.8, 0.95, 1.0, 1.0)), (0.6, (0.5, 0.75, 1.0, 0.8)), (1.0, (0.25, 0.45, 1.0, 0.0)))))
    out.append(psys.add(nif, 'FrostCrystals', tx('flake'), emitter=('cylinder', R * 0.5, 20), rate=400, start=C,
                        stop=C + 0.15, duration=DURATION, life=1.0, life_var=0.25, speed=420, speed_var=160,
                        declination=0.0, declination_var=0.6, radius=8, radius_var=3, grow=0.04, fade=0.4,
                        rotation=(1.5, 1.5), gravity=((0, 0, 1), -900), translation=(0, 0, 20), world_space=True,
                        colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (0.7, (0.7, 0.9, 1.0, 0.8)), (1.0, (0.4, 0.7, 1.0, 0.0)))))
    # 05.11: instead of the solid blades: a wave of ice bursting out of the ground, ring after ring as the shock passes: white crystals thrown
    # up and falling back, a thin cold plume with them
    for i, far in enumerate((0.3, 0.5, 0.7, 0.92)):
        t0 = C + 0.55 * far
        out.append(psys.add(nif, 'IceBurst%d' % i, tx('flake'), emitter=('cylinder', R * far, 15), rate=1000, start=t0, stop=t0 + 0.09,
                            duration=DURATION, life=0.75, life_var=0.2, speed=620 + 180 * far, speed_var=220, declination=0.0, declination_var=0.3,
                            radius=9, radius_var=4, grow=0.03, fade=0.35, rotation=(1.5, 1.5), gravity=((0, 0, 1), -1300), translation=(0, 0, 10),
                            world_space=True, colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (0.5, (0.8, 0.95, 1.0, 0.95)), (1.0, (0.35, 0.65, 1.0, 0.0)))))
        out.append(psys.add(nif, 'IcePlume%d' % i, tx('haze'), emitter=('cylinder', R * far, 15), rate=500, start=t0, stop=t0 + 0.09,
                            duration=DURATION, life=0.6, life_var=0.15, speed=420, speed_var=120, declination=0.0, declination_var=0.2,
                            radius=38, radius_var=14, grow=0.08, fade=0.5, rotation=(0.5, 0.5), translation=(0, 0, 10), world_space=True,
                            colours=((0.0, (0.75, 0.92, 1.0, 0.9)), (1.0, (0.3, 0.55, 1.0, 0.0)))))
    out.append(psys.add(nif, 'FrostFog', tx('haze'), emitter=('cylinder', R * 0.85, 10), rate=350, start=C,
                        stop=C + 0.3, duration=DURATION, life=min(1.3, DURATION - C - 0.3), life_var=0.1, speed=20,
                        speed_var=10, declination=0.0, declination_var=1.2, radius=150, radius_var=40, grow=0.12,
                        fade=0.5, rotation=(0.2, 0.2), translation=(0, 0, 30), world_space=True,
                        colours=((0.0, (0.55, 0.8, 1.0, 0.8)), (0.4, (0.45, 0.7, 1.0, 0.65)), (1.0, (0.25, 0.45, 1.0, 0.0)))))
    return out


def build(name=NAME, texture=None, static_scale=None, chill=False, dark=(), radius=None):
    """texture: one texture name for every part instead of ours; static_scale: no growth and no fade, every part at this
    scale with full glow (test variants: is the file drawn at all, at which size); chill: the small frost on a target
    (Chill below: no palms, no splinters, starts at once)"""
    nif = nif_write.Nif()
    aroot = 'AROOT_' + name
    # block order as in the game's files: 0 asset data, 1 Scene Root, 2 MaxBound, 3 z-buffer, 4 vertex colour,
    # 5 AROOT, 6 its UserPropBuffer, 7 NonAccum, then the parts; the footer root is block 0
    nif.add('CStreamableAssetData', raw=b'')                           # filled at the end (needs the sequence index)
    root = _node(nif, 'NiNode', 'Scene Root', scale=0.01, flags=0x102)
    # The caster effect component is attached at the hand.  Frost Nova is a
    # ground burst, so move the asset root to the character's feet (centimetres
    # before the scene-root scale converts them to metres).
    nif.blocks[root]['translation'] = [0.0, 0.0, -150.0]
    maxb = nif.add('NiFloatsExtraData', name=nif.string('MaxBound'), values=[0.0] * 10)
    zroot = nif.add('NiZBufferProperty', name=-1, extra=[], controller=-1, flags=15)
    vroot = nif.add('NiVertexColorProperty', name=-1, extra=[], controller=-1, flags=8)
    arootn = _node(nif, 'NiNode', aroot, flags=0x102)
    upb = nif.add('NiStringExtraData', name=nif.string('UserPropBuffer'), value=nif.string('KFAccumRoot = \r\n'))
    nonaccum = _node(nif, 'NiNode', aroot + ' NonAccum', flags=0x186)
    nif.blocks[root].f.update(children=[arootn], extra=[maxb], properties=[vroot, zroot])
    nif.blocks[arootn].f.update(children=[nonaccum], extra=[upb])
    # flash: a camera facing quad (billboard node) 1.2 m above the ground
    fv = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
    tx = (lambda k: texture) if texture else (lambda k: TEXTURES[k])
    flash = _part(nif, 'Flash', tx('flash'), fv, [[0, 1], [1, 1], [1, 0], [0, 0]], (0, -1, 0), billboard=True)
    ring = _part(nif, 'Ring', tx('ring'), *_annulus())
    frost = _part(nif, 'Frost', tx('frost'), *_cracks())
    # 03.11: no flat hoarfrost layer (a plane at the caster's feet cut into steps and slopes); the ground is the cracks
    # (no Haze part is made at all: its blocks would stay in the file without a parent)
    parts = [flash, ring, frost]
    names = ['Flash', 'Ring', 'Frost']
    moving = {}                                            # part -> (scale keys, translation keys, glow keys)
    C, R, T = (0.0, Chill.RADIUS_CM, Chill.DURATION) if chill else (CAST_AT, radius or RADIUS_CM, DURATION)
    E = Chill.LINGER if chill else LINGER
    quad_uv = [[0, 1], [1, 1], [1, 0], [0, 0]]
    rng = np.random.default_rng(31)
    for side, sx in () if chill else (('L', 1), ('R', -1)):
        path = [(t, (sx * x, y, z)) for t, x, y, z in PALM]
        lag = [(min(t + 0.07, C + 0.04), p) for t, p in path]
        parts.append(_part(nif, 'Hand' + side, tx('flash'), fv, quad_uv, (0, -1, 0), billboard=True))
        names.append('Hand' + side)
        moving['Hand' + side] = ([(0.0, 6), (0.3, 12), (0.7, 16), (C - 0.02, 22), (C + 0.06, 4), (T, 4)],
                                 path + [(T, path[-1][1])],
                                 [(0.0, 0.0), (0.15, 0.7), (0.6, 1.0), (C - 0.02, 1.5), (C + 0.06, 0.0), (T, 0.0)])
        if True:                                          # 03.11: the soft mist hung in the sky (low camera): off
            continue
        moving['Mist' + side] = ([(0.0, 20), (0.5, 34), (C, 44), (C + 0.3, 60), (T, 60)],
                                 lag + [(T, lag[-1][1])],
                                 [(0.0, 0.0), (0.2, 0.5), (C, 0.7), (C + 0.3, 0.0), (T, 0.0)])
    for k in range(0):                                     # 05.11: no solid splinters, the particles of _particles() do it
        ang = 2 * np.pi * (k + rng.uniform(-0.35, 0.35)) / 14
        dist = R * rng.uniform(0.35, 0.8)
        peak = rng.uniform(90, 180)
        fly = rng.uniform(0.45, 0.7)
        nm = 'Debris%02d' % k
        parts.append(_part(nif, nm, tx('shard'), *_shards(count=1, radius=0.0, seed=40 + k), opaque=True))
        names.append(nm)
        path = [(0.0, (0, 0, -500)), (C - 0.01, (0, 0, -500))]
        for i in range(7):
            f = i / 6
            path.append((C + f * fly, (np.cos(ang) * dist * f, np.sin(ang) * dist * f,
                                       30 + 4 * peak * f * (1 - f) - 30 * f)))
        path.append((T, path[-1][1]))
        size = rng.uniform(55, 95)
        moving[nm] = ([(0.0, 0.0), (C - 0.01, 0.0), (C, size), (C + fly, size), (C + fly + 0.6, 0.0), (T, 0.0)], path,
                      [(0.0, 0.0), (C - 0.01, 0.0), (C, 1.3), (C + fly, 1.0), (C + fly + 0.6, 0.0), (T, 0.0)])
    dust = 0                                               # 03.11: the clouds read as a blue veil in the sky: off
    big = 0.45 if chill else 1.0                           # cloud size and rise on a target
    # 03.11: bigger and brighter clouds (5 m, glow 0.55) joined into a blue veil over the whole view; now 2.6 m, 0.25
    for k in range(dust):                                  # snow dust rising behind the ring
        ang = 2 * np.pi * (k + rng.uniform(-0.3, 0.3)) / dust
        far = rng.uniform(0.75, 0.95)
        nm = 'Dust%02d' % k
        parts.append(_part(nif, nm, tx('haze'), fv, quad_uv, (0, -1, 0), billboard=True))
        names.append(nm)
        d0, d1 = R * 0.15, R * far
        hidden = [(0.0, (0, 0, -500)), (C - 0.01, (0, 0, -500))] if C > 0 else []
        path = hidden + [(C, (np.cos(ang) * d0, np.sin(ang) * d0, 20)),
                         (C + 0.55, (np.cos(ang) * d1, np.sin(ang) * d1, 70 * big)),
                         (C + E * 0.9, (np.cos(ang) * d1 * 1.05, np.sin(ang) * d1 * 1.05, 140 * big)), (T, (0, 0, -500))]
        moving[nm] = (([(0.0, 0.0), (C - 0.01, 0.0)] if C > 0 else []) +
                      [(C, 35 * big), (C + 0.55, 90 * big), (C + E * 0.9, 130 * big), (C + E * 0.9 + 0.01, 0.0), (T, 0.0)], path,
                      [(0.0, 0.0), (C + 0.3, 0.25), (C + E * 0.9, 0.0), (T, 0.0)])
    # ice blades (03.11: one node each, opaque): they burst out of the ground as the ring passes, overshoot and settle,
    # flash white when they appear and glint once more, and sink back into the ground one after another at the end
    # ice blades (03.11: one node each, opaque) in a wave from the caster to the edge: rings of blades burst out of the
    # ground as the shock ring passes them, overshoot and settle; the small inner ones sink back at once, the big outer
    # ring stands, glints and sinks one after another at the end
    # the unit blade keeps its 8 m size; a wider radius only spreads the rings (spread) and adds inner blades
    S = (R if chill else RADIUS_CM) * 0.92
    spread = 1.0 if chill else R / RADIUS_CM
    rings = ([] if chill else
             [])                                       # 05.11 the user: the solid blades looked foreign, particles instead
    # 03.11: one node per blade for every ring (94 parts) left only the glows: too many parts for one effect (the
    # working files had about 50); the inner rings are one part each, only the outer blades are separate
    k = 0
    for ri, (far0, count, size0) in enumerate(rings[:-1]):
        count = int(round(count * spread))
        verts, uvs = [], []
        for j in range(count):
            ang = 2 * np.pi * (j + rng.uniform(-0.3, 0.3)) / count + ri * 0.4
            far = far0 + rng.uniform(-0.04, 0.04)
            size = size0 * rng.uniform(0.75, 1.2)
            v, u, _ = _blade(ang, rng)
            verts += [(px * size + np.cos(ang) * far * spread, py * size + np.sin(ang) * far * spread, pz * size)
                      for px, py, pz in v]
            uvs += u
        nm = 'Wave%d' % ri
        parts.append(_part(nif, nm, tx('shard'), verts, uvs, (0, 0, 1), opaque=True))
        names.append(nm)
        hgt = 0.19 * S * size0
        t0 = C + 0.55 * far0
        t_end = t0 + 0.22 + 0.15 * far0
        path = [(0.0, (0, 0, -500)), (t0 - 0.01, (0, 0, -500)), (t0, (0, 0, -0.8 * hgt)), (t0 + 0.08, (0, 0, 0.1 * hgt)),
                (t0 + 0.16, (0, 0, 0.0)), (t_end, (0, 0, 0.0)), (t_end + 0.2, (0, 0, -1.0 * hgt)),
                (t_end + 0.21, (0, 0, -500)), (T, (0, 0, -500))]
        scale = [(0.0, 0.0), (t0 - 0.01, 0.0), (t0, S), (t_end + 0.2, S), (t_end + 0.21, 0.0), (T, 0.0)]
        glow = [(0.0, 0.0), (t0 - 0.01, 0.0), (t0, 2.0), (t0 + 0.15, 1.0), (T, 1.0)]
        moving[nm] = (scale, path, glow)
    for ri, (far0, count, size0) in ([(len(rings) - 1, rings[-1])] if rings else []):
        outer = True
        for j in range(count):
            ang = 2 * np.pi * (j + rng.uniform(-0.3, 0.3)) / count + ri * 0.4
            far = far0 + rng.uniform(-0.04, 0.04)
            size = size0 * rng.uniform(0.75, 1.2)
            v, u, n = _blade(ang, rng)
            nm = 'Blade%02d' % k
            k += 1
            parts.append(_part(nif, nm, tx('shard'), v, u, n, opaque=True))
            names.append(nm)
            x, y = np.cos(ang) * S * far * spread, np.sin(ang) * S * far * spread
            hgt = 0.19 * S * size
            t0 = C + 0.55 * far                            # the shock ring passes
            t_end = (C + E * 0.62 + rng.uniform(0.0, 0.12)) if outer else t0 + 0.22 + 0.15 * far
            hidden = [(0.0, (x, y, -500)), (t0 - 0.01, (x, y, -500))]
            path = hidden + [(t0, (x, y, -0.7 * hgt)), (t0 + 0.08, (x, y, 0.1 * hgt)), (t0 + 0.16, (x, y, 0.0)),
                             (t_end, (x, y, 0.0)), (t_end + 0.2, (x, y, -0.9 * hgt)), (t_end + 0.21, (x, y, -500)),
                             (T, (x, y, -500))]
            scale = [(0.0, 0.0), (t0 - 0.01, 0.0), (t0, S * size * 0.6), (t0 + 0.08, S * size * 1.15),
                     (t0 + 0.16, S * size), (t_end, S * size), (t_end + 0.2, S * size * 0.8), (t_end + 0.21, 0.0), (T, 0.0)]
            glow = [(0.0, 0.0), (t0 - 0.01, 0.0), (t0, 2.0), (t0 + 0.15, 1.0)]
            if outer:
                glint = (t0 + 0.2 + t_end) / 2 + rng.uniform(-0.02, 0.02)
                glow += [(glint - 0.05, 1.0), (glint, 1.8), (glint + 0.08, 1.0)]
            moving[nm] = (scale, path, glow + [(T, 1.0)])
    nif.blocks[nonaccum]['children'] = [p[0] for p in parts] + _particles(nif, chill, C, R, tx)
    # emissive controllers on the materials (the sequence drives them; blend interpolators as in the game's files)
    for node, shape_name, mat in parts:
        blend = nif.add('NiBlendPoint3Interpolator', raw=b'\x05\x02\x00\x00\x00\x00' + struct.pack('<3f', *[-3.4028235e38] * 3))
        ctrl = nif.add('NiMaterialColorController', next=-1, flags=0x6C, frequency=1.0, phase=0.0, start=0.0,
                       stop=DURATION, target=mat, interpolator=blend, color=3)
        nif.blocks[mat]['controller'] = ctrl
    # the animation
    # the burst of 02.11 with every key moved to the slam (CAST_AT); before it every part is at scale 0 and dark
    burst = {
        # 03.11: a short compact flash at the slam (the 5 m glow lay on the ground long before the blade wave arrived)
        # 03.11: a big camera facing flash read as a lit disc on the ground (top-down camera); now a small burst where
        # the palms hit the ground (PALM at the slam: 40 cm in front, low)
        # a cold burst where the palms hit the ground, up to 1.4 m (03.11: the lit disc on the ground was not this flash
        # but the attach call in Div2Fix; the 5 m flash of 02.11 covered the view from a top-down camera)
        'Flash': ([(0.0, 30), (0.06, 70), (0.3, 40)], [(0.0, 1.3), (0.06, 1.0), (0.3, 0.0)], (0, -40, 40)),
        'Ring': ([(0.0, 30), (0.55, R), (E, R * 1.05)], [(0.0, 1.8), (0.35, 1.5), (E * 0.7, 0.8), (E, 0.0)], (0, 0, LIFT)),
        'Frost': ([(0.0, 20), (0.6, R * 0.95), (E, R)], [(0.0, 0.0), (0.2, 1.5), (0.9, 1.3), (E * 0.7, 0.9), (E, 0.0)],
                  (0, 0, LIFT - 2)),
        # the hoarfrost spreads with the ring and melts back towards the centre (alpha blended: it fades by size)
        'Haze': ([(0.0, 20), (0.6, R * 0.95), (E * 0.7, R * 0.95), (E, 0.0)], [(0.0, 1.0), (E, 1.0)],
                 (0, 0, 1)),
    }
    if chill:                                       # on a target: a soft flash at the chest, frost at the feet
        burst['Flash'] = ([(0.0, 30), (0.1, 140), (0.4, 60)], [(0.0, 1.0), (0.1, 0.9), (0.4, 0.0)], (0, 0, 100))
    plan = {}
    for k, (sc, gl, pos) in burst.items():
        lead = [(0.0, 0.0), (C - 0.01, 0.0)] if C > 0 else []
        sc = lead + [(C + t, v) for t, v in sc]
        gl = lead + [(C + t, v) for t, v in gl]
        plan[k] = (sc + [(T, sc[-1][1])], gl + [(T, 0.0)], pos)
    if static_scale:
        plan = {k: ([(0.0, static_scale), (T, static_scale)], [(0.0, 1.0), (T, 1.0)], v[2]) for k, v in plan.items()}
    items = [{'interpolator': -1, 'controller': -1, 'node': nif.string(aroot), 'property': -1,
              'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1},
             {'interpolator': -1, 'controller': -1, 'node': nif.string(aroot + ' NonAccum'), 'property': -1,
              'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1}]
    seq_index = len(nif.blocks)
    seq = nif.add('NiControllerSequence', name=nif.string(name), extra=[], controller=-1, blocks=None, weight=1.0,
                  text_keys=-1, cycle=2, frequency=1.0, start=0.0, stop=T, manager=-1, accum_root=nif.string(aroot),
                  tail=64)
    assert seq == seq_index
    for (node, shape_name, mat), part in zip(parts, names):
        if part in moving and not static_scale:
            scales, path, glow = moving[part]
            nif.blocks[node]['translation'] = list(path[0][1])
            ti = _keys_ts(nif, scales, path)
        else:
            scales, glow, pos = plan[part] if part in plan else ([(0.0, 50), (T, 50)], [(0.0, 1.0), (T, 1.0)], (0, 0, 0))
            nif.blocks[node]['translation'] = list(pos)
            ti = _keys_scale(nif, scales, pos)
        if part.startswith(tuple(dark)):                  # test variants (03.11): this part tiny, dark and underground
            ti = _keys_ts(nif, [(0.0, MIN_SCALE), (T, MIN_SCALE)], [(0.0, (0, 0, -500)), (T, (0, 0, -500))])
            glow = [(0.0, 0.0), (T, 0.0)]
        items.append({'interpolator': ti, 'controller': -1, 'node': nif.string(part), 'property': -1,
                      'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1})
        ei = _keys_emissive(nif, glow, GROUND_TINT if part in ('Ring', 'Frost') else (0.45, 0.8, 1.0))
        items.append({'interpolator': ei, 'controller': -1, 'node': nif.string(shape_name),
                      'property': nif.string('NiMaterialProperty'), 'ctrl_type': nif.string('NiMaterialColorController'),
                      'ctrl_id': nif.string('SELF_ILLUM'), 'interp_id': -1})
    tk = nif.add('NiTextKeyExtraData', name=-1, keys=[[0.0, nif.string('start')], [T, nif.string('end')]])
    nif.blocks[seq]['blocks'] = {'grow': 1, 'items': items + getattr(nif, 'psys_items', [])}
    nif.blocks[seq]['text_keys'] = tk
    # MaxBound as in the game's files (and ItemDescriptors.xml): centre xyz, radius, min xyz, max xyz
    nif.blocks[maxb]['values'] = list(Chill.BOUND if chill else BOUND)
    # the asset header
    def st(s):
        b = s.encode('latin1')
        return struct.pack('<I', len(b)) + b
    body = (st('.\\%s.nif' % name) + st(aroot) + struct.pack('<II2f2I', 1, 2, 0.1, 0.1, 1, 0) + st('%s.kf' % name)
            + b'\0' * 12 + struct.pack('<II', 1, seq_index))
    # the length field counts the body without its last 8 bytes (u32 1, u32 sequence index): checked on all the game's
    # effects (asset_header_check); one off and the engine reads the next block from the wrong place (crash 02.11)
    nif.blocks[0] = nif_write.Block('CStreamableAssetData', raw=struct.pack('<IBI', 1, 1, len(body) - 8) + body)
    nif.roots = [0]
    return nif.encode()


class Chill:
    """the frost on every target of Frost Nova (Div2Fix OwnHitFx): the burst in small, no palms and no splinters"""
    RADIUS_CM = 140.0
    LINGER = 2.2
    DURATION = LINGER + 0.3
    BOUND = (0.0, 0.0, 100.0, 200.0, -150.0, -150.0, 0.0, 150.0, 150.0, 200.0)
    textures = staticmethod(textures)

    @staticmethod
    def build(name='FrostNova_Chill', **kw):
        return build(name=name, chill=True, **kw)


def main(argv):
    if len(argv) == 3 and argv[1] == 'preview':
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                        'tools', 'reborn'))
        from fx_select import sheet
        items = []
        for k, v in textures().items():
            f = v.astype(np.float32) / 255
            img = np.clip(f[..., :3] * f[..., 3:4], 0, 1)
            items.append((k, (np.concatenate([img, np.ones_like(img[..., :1])], -1) * 255).astype(np.uint8)))
        sheet(argv[2], items, tile=512, cols=4)
        print('preview ->', argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
