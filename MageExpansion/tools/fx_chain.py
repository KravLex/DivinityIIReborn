r"""Chain Lightning's beam, made from code (geometry, animation and textures; nothing of the game's is copied).

  py -3 -I fx_chain.py preview <out.png>     the textures side by side

03.11: the first version cut the flying tower's lightning (Win32\Effects\D_LTower_Beam) to length. In the game it drew
huge pink planes over the view: the tower's beam is made for the shader div2_FF_LaserBeam, which the exe gives only to
an effect named D_LTower_Beam (0xC6FC70). So the beam is our own now, the way Frost Nova's effects are made
(fx_frostnova: additive textured quads, emissive keys in a NiControllerSequence).

Div2Fix places it with attach mode 7 (position + 3x3 rotation) at the point the bolt comes from. The attach adds a turn
of pi about Z to every effect (0x982715: 0x501C70(pi), 0x501DC0), so the bolt runs along -X here and ends up pointing
at the enemy. One file per length (the effect cannot be stretched at run time):
  Bolt0..2 - the bolt, two crossed strips each (seen from every side), three jagged frames flickering in turn
  Spark    - a camera facing burst where the bolt strikes
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fx_frostnova as fx           # noqa: E402
import nif_write                    # noqa: E402

DURATION = 0.7                      # s: one flash per leap
WIDTH = 75.0                        # cm, half width of a strip (03.11: 120 made a wide cloud, not a strike)
TEXTURES = {'bolt0': 'fx_chainlight_bolt0', 'bolt1': 'fx_chainlight_bolt1', 'bolt2': 'fx_chainlight_bolt2',
            'bolt3': 'fx_chainlight_bolt3',
            'spark': 'fx_chainlight_spark', 'glow': 'fx_chainlight_glow0', 'mark': 'fx_huntersmark_target'}
GLOW_TINT = (0.85, 0.30, 1.0)      # 03.11: more colour - violet-magenta for the sparks and flashes
HAZE_TINT = (0.45, 0.40, 1.0)      # 03.11: the haze around the bolt: blue-violet, soft (the magenta slab looked odd)
HAZE_PUFFS = 9                     # camera facing glow puffs along the bolt
TINT = (0.55, 0.62, 1.0)            # violet-blue
# the bound (centre, radius, min, max; cm) of the longest beam, 15 m along -X
BOUND = (-750.0, 0.0, 0.0, 820.0, -1560.0, -200.0, -200.0, 60.0, 200.0, 200.0)
SIZE = 512
BOLT_SIZE = 1024                    # 03.11: the bolts at 512 looked soft over a 15 m leap


# ---------------------------------------------------------------- textures
def _crack(rng, p0, p1, depth, rough):
    """midpoint displacement: the polyline of a jagged bolt from p0 to p1"""
    pts = [np.array(p0, float), np.array(p1, float)]
    for _ in range(depth):
        out = [pts[0]]
        for a, b in zip(pts[:-1], pts[1:]):
            d = b - a
            n = np.array([-d[1], d[0]])
            m = (a + b) / 2 + n * rng.normal(0, rough)
            out += [m, b]
        pts = out
    return pts


def tex_bolt(seed, size=SIZE):
    """03.11 (the reference the user sent): a thin bright trunk along u, sharply jagged, with long forks spreading far
    to both sides of v, each forking again; only a narrow glow (the colour stays ours)"""
    rng = np.random.default_rng(seed)
    k = size / 512.0                                          # the shapes are made for 512
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    core = np.zeros((size, size), np.float32)

    def draw(pts, w, gain):
        nonlocal core
        for a, b in zip(pts[:-1], pts[1:]):
            x0, y0 = min(a[0], b[0]) - 4 * w, min(a[1], b[1]) - 4 * w
            x1, y1 = max(a[0], b[0]) + 4 * w, max(a[1], b[1]) + 4 * w
            i0, i1 = int(max(0, y0)), int(min(size, y1 + 1))
            j0, j1 = int(max(0, x0)), int(min(size, x1 + 1))
            if i0 >= i1 or j0 >= j1:
                continue
            X, Y = xx[i0:i1, j0:j1], yy[i0:i1, j0:j1]
            d = b - a
            t = np.clip(((X - a[0]) * d[0] + (Y - a[1]) * d[1]) / max(d @ d, 1e-6), 0, 1)
            dist = np.hypot(X - (a[0] + t * d[0]), Y - (a[1] + t * d[1]))
            core[i0:i1, j0:j1] = np.maximum(core[i0:i1, j0:j1], np.exp(-(dist / w) ** 2) * gain)

    c = size / 2
    trunk = _crack(rng, (0, c + rng.normal(0, 10) * k), (size, c + rng.normal(0, 10) * k), 8, 0.16)
    draw(trunk, 2.0 * k, 1.0)
    for f in range(rng.integers(6, 9)):                       # long forks, each forking again
        p = trunk[rng.integers(8, len(trunk) - 20)]
        side = rng.choice([-1, 1])
        q = p + np.array([rng.uniform(110, 240), side * rng.uniform(35, 120)]) * k   # forward, along the strike
        q[1] = np.clip(q[1], 10 * k, size - 10 * k)
        fork = _crack(rng, p, q, 6, 0.22)
        draw(fork, 1.3 * k, 0.85)
        for g in range(rng.integers(1, 3)):
            r = fork[rng.integers(4, len(fork) - 4)]
            e = r + np.array([rng.uniform(40, 100), side * rng.uniform(10, 45)]) * k
            e[1] = np.clip(e[1], 5 * k, size - 5 * k)
            draw(_crack(rng, r, e, 5, 0.25), 0.9 * k, 0.6)
    white = np.array([0.97, 0.97, 1.0])
    blue = np.array([0.45, 0.65, 1.0])
    violet = np.array([0.50, 0.32, 1.0])
    rgb = core[..., None] * white * 1.7
    rgb = rgb + fx_glow(core, int(4 * k))[..., None] * blue * 1.3 + fx_glow(core, int(12 * k))[..., None] * violet * 0.6
    u = (xx + 0.5) / size
    rgb *= np.clip(np.minimum(u, 1 - u) / 0.03, 0, 1)[..., None]
    return fx._rgba(rgb)


def fx_glow(a, r):
    import icon_art
    return icon_art._blur(a, r)


def tex_spark(size=SIZE):
    u, v, r, a = fx._grid(size)
    core = np.exp(-(r / 0.12) ** 2)
    halo = np.exp(-(r / 0.45) ** 2) * 0.6
    rays = sum(np.clip(np.cos(a * n + n), 0, 1) ** 40 for n in (3, 5, 7)) * np.exp(-(r / 0.8) ** 2)
    rgb = core[..., None] * np.array([1.0, 1.0, 1.0]) * 1.6 + halo[..., None] * np.array([0.5, 0.4, 1.0]) + \
        rays[..., None] * np.array([0.7, 0.75, 1.0])
    rgb *= (r < 1)[..., None]
    return fx._rgba(rgb)


def tex_glow(size=SIZE):
    """a soft round puff, nothing at the square's edge (03.11: a band on flat strips read as a slab)"""
    u, v, r, a = fx._grid(size)
    puff = np.exp(-(r / 0.42) ** 2) * np.clip((1 - r) / 0.25, 0, 1)
    rgb = puff[..., None] * np.array([0.6, 0.55, 1.0]) * 0.8
    return fx._rgba(rgb, alpha_gain=1.2)


def tex_mark(size=SIZE):
    """05.11: Hunter's Mark: a target: two rings, four notches and a dot, red on nothing"""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    u, v = (xx + 0.5) / size * 2 - 1, (yy + 0.5) / size * 2 - 1
    r = np.hypot(u, v)
    shape = np.exp(-((r - 0.80) / 0.035) ** 2) + 0.8 * np.exp(-((r - 0.45) / 0.025) ** 2) + np.exp(-(r / 0.09) ** 2)
    notch = (np.exp(-(u / 0.03) ** 2) + np.exp(-(v / 0.03) ** 2)) * (r > 0.52) * (r < 0.98)
    shape = np.clip(shape + notch, 0, 1) * (r < 1)
    glow = np.exp(-((r - 0.8) / 0.14) ** 2) * 0.35 * (r < 1)
    rgb = shape[..., None] * np.array([1.0, 0.25, 0.1]) * 1.2 + glow[..., None] * np.array([1.0, 0.1, 0.05]) + (shape ** 3)[..., None] * np.array([0.3, 0.3, 0.2])
    return np.concatenate([np.clip(rgb, 0, 1), np.clip(shape + glow, 0, 1)[..., None]], -1)


def textures():
    tex = {TEXTURES['mark']: tex_mark(), TEXTURES['bolt0']: tex_bolt(101, BOLT_SIZE), TEXTURES['bolt1']: tex_bolt(202, BOLT_SIZE),
           TEXTURES['bolt2']: tex_bolt(303, BOLT_SIZE), TEXTURES['bolt3']: tex_bolt(404, BOLT_SIZE),
           TEXTURES['spark']: tex_spark(), TEXTURES['glow']: tex_glow()}
    return {k: (np.clip(v, 0, 1) * 255 + 0.5).astype(np.uint8) for k, v in tex.items()}


# ---------------------------------------------------------------- the NIF
def _strips(length):
    """two crossed quads along -X (0 .. -length): flat (XY) and upright (XZ)"""
    L, w = -length, WIDTH
    verts = [(0, -w, 0), (L, -w, 0), (L, w, 0), (0, w, 0),
             (0, 0, -w), (L, 0, -w), (L, 0, w), (0, 0, w)]
    uvs = [[0, 1], [1, 1], [1, 0], [0, 0]] * 2
    return verts, uvs, (0, 0, 1)


def _particles(nif, length, T):
    """04.11: volumetric particles (psys.py): sparks crackling off the whole bolt, ions glowing along it, and a burst of
    sparks where it strikes. The bolt runs along -X (0 .. -length); emitted once, kept in the world."""
    import os
    import psys
    if os.environ.get('MAGE_NO_PSYS') is not None:
        return []
    mid = (-length / 2, 0, 0)
    # 04.11 second try (in game hardly seen): bigger, living longer, more of them; third (too bright): dimmer, ions halved
    return [
        psys.add(nif, 'Crackle', TEXTURES['spark'], emitter=('box', length, 30, 30), rate=min(1000, 1.6 * length),
                 stop=0.25, duration=T, life=0.5, life_var=0.15, speed=220, speed_var=120, declination=0.0,
                 declination_var=3.1416, radius=10, radius_var=3, grow=0.02, fade=0.25, rotation=(4.0, 3.0),
                 gravity=((0, 0, 1), -500), translation=mid, world_space=True,
                 colours=((0.0, (0.8, 0.95, 1.0, 0.7)), (0.5, (0.45, 0.85, 1.0, 0.55)), (1.0, (0.2, 0.7, 1.0, 0.0)))),
        psys.add(nif, 'Ions', TEXTURES['glow'], emitter=('box', length, 25, 25), rate=min(300, 0.45 * length), stop=0.3,
                 duration=T, life=0.6, life_var=0.15, speed=25, speed_var=20, radius=26, radius_var=6, grow=0.05,
                 fade=0.35, rotation=(0.0, 0.5), translation=mid, world_space=True,
                 colours=((0.0, (0.35, 0.8, 1.0, 0.4)), (1.0, (0.1, 0.45, 1.0, 0.0)))),
        psys.add(nif, 'StrikeSparks', TEXTURES['spark'], emitter=('sphere', 15), rate=900, stop=0.08, duration=T,
                 life=0.6, life_var=0.2, speed=450, speed_var=180, declination=0.0, declination_var=3.1416, radius=12,
                 radius_var=4, grow=0.02, fade=0.3, rotation=(4.0, 3.0), gravity=((0, 0, 1), -800),
                 translation=(-length, 0, 0), world_space=True,
                 colours=((0.0, (0.9, 1.0, 1.0, 0.85)), (0.4, (0.5, 0.9, 1.0, 0.7)), (1.0, (0.2, 0.6, 1.0, 0.0)))),
    ]


def build(name='ChainLightning_Beam', length=600.0):
    """length in cm"""
    nif = nif_write.Nif()
    aroot = 'AROOT_' + name
    nif.add('CStreamableAssetData', raw=b'')
    root = fx._node(nif, 'NiNode', 'Scene Root', scale=0.01, flags=0x102)
    maxb = nif.add('NiFloatsExtraData', name=nif.string('MaxBound'), values=[0.0] * 10)
    zroot = nif.add('NiZBufferProperty', name=-1, extra=[], controller=-1, flags=15)
    vroot = nif.add('NiVertexColorProperty', name=-1, extra=[], controller=-1, flags=8)
    arootn = fx._node(nif, 'NiNode', aroot, flags=0x102)
    upb = nif.add('NiStringExtraData', name=nif.string('UserPropBuffer'), value=nif.string('KFAccumRoot = \r\n'))
    nonaccum = fx._node(nif, 'NiNode', aroot + ' NonAccum', flags=0x186)
    nif.blocks[root].f.update(children=[arootn], extra=[maxb], properties=[vroot, zroot])
    nif.blocks[arootn].f.update(children=[nonaccum], extra=[upb])
    T = DURATION
    parts, names, plan = [], [], {}
    # three frames, each bright for a third of the flicker cycle, all fading out together
    cycle = 0.08
    for k in range(4):
        nm = 'Bolt%d' % k
        parts.append(fx._part(nif, nm, TEXTURES['bolt%d' % k], *_strips(length)))
        names.append(nm)
        glow = [(0.0, 0.0)]
        t = 0.0
        while t < T:
            on = t + k * cycle / 4
            fade = max(0.0, 1.0 - on / T) ** 0.6
            glow += [(min(on, T), 0.2 * fade), (min(on + 0.01, T), 3.0 * fade), (min(on + cycle / 4, T), 0.2 * fade)]
            t += cycle
        glow.append((T, 0.0))
        glow = sorted({round(a, 4): b for a, b in glow}.items())
        plan[nm] = ([(0.0, 1.0), (T, 1.0)], glow, (0, 0, 0))
    # the haze: soft camera facing puffs along the bolt, overlapping, a slow pulse (03.11: wide flat strips read as a
    # magenta slab cutting into the ground)
    pv = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
    step = length / HAZE_PUFFS
    for k in range(HAZE_PUFFS):
        nm = 'Haze%d' % k
        parts.append(fx._part(nif, nm, TEXTURES['glow'], pv, [[0, 1], [1, 1], [1, 0], [0, 0]], (0, -1, 0), billboard=True))
        names.append(nm)
        r = min(max(step * 0.9, 45.0), 110.0)
        plan[nm] = ([(0.0, r * 0.8), (0.12, r), (T, r * 1.1)],
                    [(0.0, 0.35), (0.1, 0.2), (0.3, 0.25), (T, 0.0)], (-(k + 0.5) * step, 0, 0))
    # 03.11 (the tower's look): the game's own electric textures - two more flickering arcs (FX_ElectricBolt*, the
    # frames run along V as in the tower's beam), a crackle where the bolt leaves and sparks thrown from the struck enemy
    sv, _, _ = _strips(length)
    sv = [(x, y * 0.6, z * 0.6) for x, y, z in sv]
    along_v = [[0, 0], [0, 1], [1, 1], [1, 0]] * 2
    for k, tex in enumerate(('FX_ElectricBolt_001', 'FX_ElectricBolt_B_002')):
        nm = 'Arc%d' % k
        parts.append(fx._part(nif, nm, tex, sv, along_v, (0, 0, 1)))
        names.append(nm)
        glow = [(0.0, 0.0)]
        t = 0.02 + 0.03 * k
        while t < T - 0.05:
            f = max(0.0, 1.0 - t / T)
            glow += [(t, 0.0), (t + 0.01, 2.2 * f), (t + 0.05, 0.0)]
            t += 0.07 + 0.02 * k
        glow.append((T, 0.0))
        plan[nm] = ([(0.0, 1.0), (T, 1.0)], sorted({round(a, 4): b for a, b in glow}.items()), (0, 0, 0))
    fv = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
    quad_uv = [[0, 1], [1, 1], [1, 0], [0, 0]]
    parts.append(fx._part(nif, 'Crackle', 'FX_Electricity_C', fv, quad_uv, (0, -1, 0), billboard=True))
    names.append('Crackle')
    plan['Crackle'] = ([(0.0, 45), (0.15, 70), (T, 50)],
                       [(0.0, 2.0), (0.05, 0.6), (0.1, 1.8), (0.16, 0.5), (0.24, 1.2), (T, 0.0)], (0, 0, 0))
    rng = np.random.default_rng(int(length))
    moving = {}
    for k in range(10):
        nm = 'Spark%02d' % k
        parts.append(fx._part(nif, nm, 'FX_Sparks_C', fv, quad_uv, (0, -1, 0), billboard=True))
        names.append(nm)
        ang = rng.uniform(0, 2 * np.pi)
        up = rng.uniform(-0.2, 1.0)
        speed = rng.uniform(150, 320)
        life = rng.uniform(0.25, 0.45)
        path = []
        for j in range(6):
            t = life * j / 5
            path.append((t, (-length + np.cos(ang) * speed * t, np.sin(ang) * speed * t, up * speed * t - 300 * t * t)))
        path.append((T, path[-1][1]))
        size = rng.uniform(12, 24)
        moving[nm] = ([(0.0, size), (life, size * 0.4), (life + 0.01, fx.MIN_SCALE), (T, fx.MIN_SCALE)], path,
                      [(0.0, 2.2), (life, 0.3), (life + 0.01, 0.0), (T, 0.0)])
    fv = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
    parts.append(fx._part(nif, 'Source', TEXTURES['spark'], fv, [[0, 1], [1, 1], [1, 0], [0, 0]], (0, -1, 0), billboard=True))
    names.append('Source')
    plan['Source'] = ([(0.0, 30), (0.06, 55), (T, 30)], [(0.0, 1.6), (0.1, 0.9), (T, 0.0)], (0, 0, 0))
    parts.append(fx._part(nif, 'Spark', TEXTURES['spark'], fv, [[0, 1], [1, 1], [1, 0], [0, 0]], (0, -1, 0), billboard=True))
    names.append('Spark')
    # 03.11: smaller and short: the flat star read as a sticker; the game's MagicMissile_Impact is the strike now (Div2Fix)
    plan['Spark'] = ([(0.0, 35), (0.05, 60), (0.25, 40), (T, 40)], [(0.0, 2.0), (0.08, 1.0), (0.25, 0.0), (T, 0.0)], (-length, 0, 0))
    nif.blocks[nonaccum]['children'] = [p[0] for p in parts] + _particles(nif, length, T)
    for node, shape_name, mat in parts:
        blend = nif.add('NiBlendPoint3Interpolator', raw=b'\x05\x02\x00\x00\x00\x00' + struct.pack('<3f', *[-3.4028235e38] * 3))
        ctrl = nif.add('NiMaterialColorController', next=-1, flags=0x6C, frequency=1.0, phase=0.0, start=0.0,
                       stop=T, target=mat, interpolator=blend, color=3)
        nif.blocks[mat]['controller'] = ctrl
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
        if part in moving:
            scales, path, glow = moving[part]
            nif.blocks[node]['translation'] = list(path[0][1])
            ti = fx._keys_ts(nif, scales, path)
        else:
            scales, glow, pos = plan[part]
            nif.blocks[node]['translation'] = list(pos)
            ti = fx._keys_scale(nif, scales, pos)
        items.append({'interpolator': ti, 'controller': -1, 'node': nif.string(part), 'property': -1,
                      'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1})
        ei = fx._keys_emissive(nif, glow, HAZE_TINT if part.startswith('Haze') else GLOW_TINT if part in ('Spark', 'Source') else
                               (0.8, 0.85, 1.0) if part.startswith(('Arc', 'Crackle', 'Spark')) else TINT)
        items.append({'interpolator': ei, 'controller': -1, 'node': nif.string(shape_name),
                      'property': nif.string('NiMaterialProperty'), 'ctrl_type': nif.string('NiMaterialColorController'),
                      'ctrl_id': nif.string('SELF_ILLUM'), 'interp_id': -1})
    tk = nif.add('NiTextKeyExtraData', name=-1, keys=[[0.0, nif.string('start')], [T, nif.string('end')]])
    nif.blocks[seq]['blocks'] = {'grow': 1, 'items': items + getattr(nif, 'psys_items', [])}
    nif.blocks[seq]['text_keys'] = tk
    nif.blocks[maxb]['values'] = list(BOUND)

    def st(s):
        b = s.encode('latin1')
        return struct.pack('<I', len(b)) + b
    body = (st('.\\%s.nif' % name) + st(aroot) + struct.pack('<II2f2I', 1, 2, 0.1, 0.1, 1, 0) + st('%s.kf' % name)
            + b'\0' * 12 + struct.pack('<II', 1, seq_index))
    nif.blocks[0] = nif_write.Block('CStreamableAssetData', raw=struct.pack('<IBI', 1, 1, len(body) - 8) + body)
    nif.roots = [0]
    return nif.encode()


class Cast:
    """04.11: the cast on the caster (Div2Fix OwnFx28): Div2Fix plays OwnAnim only together with an own effect, so
    with OwnFx28 empty (the Fireball's golden flash was taken away, 03.11) there was no animation at all. Turquoise
    sparks crackling round the hands, ions gathering and a burst at the release (OwnDelay28 = 450 ms)."""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 0.0, 80.0, -70.0, -70.0, -70.0, 70.0, 70.0, 70.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ChainLightning_Cast', source=None, **kw):
        import fx_icebolt
        T = Cast.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Crackle', TEXTURES['spark'], emitter=('sphere', 30), rate=220, stop=0.5, duration=T, life=0.2,
                    life_var=0.08, speed=120, speed_var=70, radius=5, radius_var=2, grow=0.02, fade=0.1,
                    rotation=(4.0, 3.0), world_space=False,
                    colours=((0.0, (0.85, 1.0, 1.0, 1.0)), (1.0, (0.2, 0.7, 1.0, 0.0))))
        e.particles('Gather', TEXTURES['glow'], emitter=('sphere', 45), rate=50, stop=0.42, duration=T, life=0.35,
                    life_var=0.08, speed=-110, speed_var=20, radius=18, radius_var=5, grow=0.1, fade=0.15,
                    rotation=(0.0, 0.5), world_space=False,
                    colours=((0.0, (0.3, 0.8, 1.0, 0.0)), (0.5, (0.5, 0.95, 1.0, 0.8)), (1.0, (0.7, 1.0, 1.0, 0.0))))
        e.particles('Release', TEXTURES['spark'], emitter=('sphere', 10), rate=900, start=0.43, stop=0.5, duration=T,
                    life=0.35, life_var=0.1, speed=320, speed_var=120, radius=6, radius_var=2, grow=0.02, fade=0.2,
                    rotation=(4.0, 3.0), gravity=((0, 0, 1), -500), world_space=True,
                    colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.2, 0.6, 1.0, 0.0))))
        return e.encode(T, Cast.BOUND)


class PushWave:
    """04.11: Arcane Push on the caster (Div2Fix OwnFx54, plays OwnAnim54 = Skill_ArcanePush): force gathers at the chest,
    at the release (anim_arcanepush.CAST_AT) a core flash in front, two rolling waves over the ground, sparks thrown
    with them and dust rising. Radial speeds for about the 5-8 m of Param3."""
    SOURCE = None
    DURATION = 1.3
    BOUND = (0.0, 0.0, 0.0, 800.0, -700.0, -700.0, -150.0, 700.0, 700.0, 150.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ArcanePush_Wave', source=None, **kw):
        import fx_icebolt
        T, C = PushWave.DURATION, 0.45
        e = fx_icebolt._Effect(name)
        ground = (0, 0, -125)       # the effect is attached at about the hands; the waves run at the shins
        violet = ((0.0, (0.75, 0.65, 1.0, 0.95)), (0.6, (0.45, 0.35, 1.0, 0.6)), (1.0, (0.25, 0.2, 0.9, 0.0)))
        e.particles('Gather', TEXTURES['glow'], emitter=('sphere', 55), rate=70, stop=C - 0.05, duration=T, life=0.28, life_var=0.05,
                    speed=-170, speed_var=25, radius=16, radius_var=4, grow=0.1, fade=0.1, rotation=(0.0, 0.5), world_space=False,
                    colours=((0.0, (0.45, 0.35, 1.0, 0.0)), (0.5, (0.65, 0.55, 1.0, 0.8)), (1.0, (0.85, 0.8, 1.0, 0.0))))
        e.particles('Crackle', TEXTURES['spark'], emitter=('sphere', 30), rate=120, stop=C, duration=T, life=0.18, life_var=0.05,
                    speed=110, speed_var=60, radius=5, radius_var=2, grow=0.02, fade=0.08, rotation=(4.0, 3.0), world_space=False,
                    colours=((0.0, (0.95, 0.9, 1.0, 1.0)), (1.0, (0.4, 0.3, 1.0, 0.0))))
        e.particles('Core', TEXTURES['glow'], emitter=('sphere', 15), rate=500, start=C, stop=C + 0.05, duration=T, life=0.22,
                    life_var=0.05, speed=90, speed_var=40, radius=70, radius_var=15, grow=0.04, fade=0.15, rotation=(0.0, 0.5),
                    world_space=True, colours=((0.0, (1.0, 0.95, 1.0, 1.0)), (1.0, (0.45, 0.35, 1.0, 0.0))))
        e.particles('Wave', TEXTURES['glow'], emitter=('cylinder', 60, 10), rate=1600, start=C, stop=C + 0.06, duration=T,
                    life=0.45, life_var=0.05, speed=1500, speed_var=120, declination=1.5708, declination_var=0.08,
                    planar=0.0, planar_var=3.1416, radius=80, radius_var=20, grow=0.03, fade=0.25, rotation=(0.4, 0.4),
                    translation=ground, world_space=True, colours=violet)
        e.particles('Wave2', TEXTURES['glow'], emitter=('cylinder', 40, 10), rate=1000, start=C + 0.1, stop=C + 0.15, duration=T,
                    life=0.5, life_var=0.05, speed=950, speed_var=100, declination=1.5708, declination_var=0.08,
                    planar=0.0, planar_var=3.1416, radius=60, radius_var=15, grow=0.03, fade=0.3, rotation=(0.4, 0.4),
                    translation=ground, world_space=True,
                    colours=((0.0, (0.55, 0.45, 1.0, 0.7)), (1.0, (0.2, 0.15, 0.8, 0.0))))
        e.particles('Sparks', TEXTURES['spark'], emitter=('cylinder', 50, 80), rate=2000, start=C, stop=C + 0.07, duration=T,
                    life=0.55, life_var=0.12, speed=1150, speed_var=350, declination=1.5708, declination_var=0.3,
                    planar=0.0, planar_var=3.1416, radius=9, radius_var=3, grow=0.02, fade=0.2, rotation=(4.0, 3.0),
                    gravity=((0, 0, 1), -500), translation=(0, 0, -80), world_space=True,
                    colours=((0.0, (0.95, 0.95, 1.0, 1.0)), (1.0, (0.4, 0.35, 1.0, 0.0))))
        e.particles('Motes', TEXTURES['glow'], emitter=('cylinder', 300, 10), rate=350, start=C + 0.05, stop=C + 0.25, duration=T,
                    life=0.55, life_var=0.1, speed=60, speed_var=30, declination=0.0, declination_var=0.5, radius=22,
                    radius_var=6, grow=0.1, fade=0.3, rotation=(0.3, 0.3), translation=ground, world_space=True,
                    colours=((0.0, (0.45, 0.35, 1.0, 0.0)), (0.3, (0.5, 0.4, 1.0, 0.4)), (1.0, (0.25, 0.2, 0.9, 0.0))))
        return e.encode(T, PushWave.BOUND)


class PushBurst:
    """04.11: the wave itself, a free effect at the hero's feet at the release (Div2Fix OwnPushFx54; on the caster's
    action it was not seen in game): a flat ring running out over the ground, a core flash and particles with it"""
    SOURCE = None
    DURATION = 0.95
    BOUND = (0.0, 0.0, 50.0, 850.0, -800.0, -800.0, 0.0, 800.0, 800.0, 200.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ArcanePush_Burst', source=None, **kw):
        import fx_frostnova as fx
        import fx_icebolt
        T = PushBurst.DURATION
        e = fx_icebolt._Effect(name)
        V = (0.55, 0.40, 1.0)
        here = fx_icebolt._still(T, (0, 0, 25))
        e.add('Ring', fx_icebolt.TEX['ring'], fx._annulus(), [(0.0, 40), (0.45, 720), (T, 760)], here,
              [(0.0, 2.4), (0.25, 1.8), (0.6, 0.0), (T, 0.0)], tint=V)
        e.add('Ring2', fx_icebolt.TEX['ring'], fx._annulus(), [(0.0, 20), (0.1, 20), (0.6, 480), (T, 520)], here,
              [(0.0, 0.0), (0.1, 1.6), (0.7, 0.0), (T, 0.0)], tint=V)
        e.add('Core', fx_icebolt.TEX['flash'], (fx_icebolt.QUAD, fx_icebolt.QUAD_UV, (0, -1, 0)), [(0.0, 60), (0.08, 190), (0.3, 110), (T, 110)],
              fx_icebolt._still(T, (0, 0, 110)), [(0.0, 2.2), (0.1, 1.6), (0.3, 0.0), (T, 0.0)], tint=V, billboard=True)
        ground = (0, 0, 30)
        e.particles('Wave', TEXTURES['glow'], emitter=('cylinder', 60, 10), rate=1600, stop=0.06, duration=T,
                    life=0.45, life_var=0.05, speed=1500, speed_var=120, declination=1.5708, declination_var=0.08,
                    planar=0.0, planar_var=3.1416, radius=80, radius_var=20, grow=0.03, fade=0.25, rotation=(0.4, 0.4),
                    translation=ground, world_space=True,
                    colours=((0.0, (0.75, 0.65, 1.0, 0.95)), (0.6, (0.45, 0.35, 1.0, 0.6)), (1.0, (0.25, 0.2, 0.9, 0.0))))
        e.particles('Sparks', TEXTURES['spark'], emitter=('cylinder', 50, 120), rate=2200, stop=0.07, duration=T,
                    life=0.55, life_var=0.12, speed=1150, speed_var=350, declination=1.5708, declination_var=0.35,
                    planar=0.0, planar_var=3.1416, radius=10, radius_var=3, grow=0.02, fade=0.2, rotation=(4.0, 3.0),
                    gravity=((0, 0, 1), -500), translation=(0, 0, 70), world_space=True,
                    colours=((0.0, (0.95, 0.95, 1.0, 1.0)), (1.0, (0.4, 0.35, 1.0, 0.0))))
        e.particles('Motes', TEXTURES['glow'], emitter=('cylinder', 300, 10), rate=400, stop=0.2, duration=T,
                    life=0.55, life_var=0.1, speed=70, speed_var=30, declination=0.0, declination_var=0.5, radius=26,
                    radius_var=6, grow=0.1, fade=0.3, rotation=(0.3, 0.3), translation=ground, world_space=True,
                    colours=((0.0, (0.45, 0.35, 1.0, 0.0)), (0.3, (0.5, 0.4, 1.0, 0.45)), (1.0, (0.25, 0.2, 0.9, 0.0))))
        return e.encode(T, PushBurst.BOUND)


class SummonBoom:
    """07.11: Summon Explosion on the summon (Div2Fix OwnBoomFx113, a free effect at its feet; the user: a proper explosion).
    A white-hot flash, a fireball of orange glow rising and spreading, a shock ring over the ground, burning sparks and
    debris thrown out and falling, green soul-fire licks for the dead flesh, and dark smoke lingering after."""
    SOURCE = None
    DURATION = 1.8
    BOUND = (0.0, 0.0, 120.0, 900.0, -850.0, -850.0, 0.0, 850.0, 850.0, 500.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='SummonExplosion_Boom', source=None, **kw):
        import fx_frostnova as fx
        import fx_icebolt
        T = SummonBoom.DURATION
        e = fx_icebolt._Effect(name)
        FIRE = (1.0, 0.55, 0.15)
        e.add('Flash', fx_icebolt.TEX['flash'], (fx_icebolt.QUAD, fx_icebolt.QUAD_UV, (0, -1, 0)),
              [(0.0, 120), (0.06, 420), (0.25, 260), (T, 260)], fx_icebolt._still(T, (0, 0, 110)),
              [(0.0, 3.0), (0.08, 2.4), (0.3, 0.0), (T, 0.0)], tint=(1.0, 0.85, 0.6), billboard=True)
        e.add('Ring', fx_icebolt.TEX['ring'], fx._annulus(), [(0.0, 40), (0.4, 820), (T, 860)], fx_icebolt._still(T, (0, 0, 20)),
              [(0.0, 2.6), (0.2, 1.8), (0.55, 0.0), (T, 0.0)], tint=FIRE)
        e.particles('Fireball', TEXTURES['glow'], emitter=('sphere', 40), rate=1800, stop=0.08, duration=T, life=0.6,
                    life_var=0.15, speed=520, speed_var=200, radius=95, radius_var=30, grow=0.05, fade=0.4,
                    rotation=(0.5, 0.5), gravity=((0, 0, 1), 260), translation=(0, 0, 90), world_space=True,
                    colours=((0.0, (1.0, 0.95, 0.75, 1.0)), (0.25, (1.0, 0.6, 0.15, 0.9)), (0.7, (0.75, 0.2, 0.05, 0.5)),
                             (1.0, (0.2, 0.05, 0.02, 0.0))))
        e.particles('Sparks', TEXTURES['spark'], emitter=('sphere', 30), rate=2600, stop=0.07, duration=T, life=0.9,
                    life_var=0.25, speed=1300, speed_var=450, radius=9, radius_var=3, grow=0.02, fade=0.3,
                    rotation=(4.0, 3.0), gravity=((0, 0, 1), -900), translation=(0, 0, 90), world_space=True,
                    colours=((0.0, (1.0, 0.95, 0.7, 1.0)), (0.5, (1.0, 0.5, 0.1, 0.9)), (1.0, (0.6, 0.15, 0.02, 0.0))))
        e.particles('Debris', TEXTURES['spark'], emitter=('sphere', 25), rate=500, stop=0.06, duration=T, life=1.2,
                    life_var=0.3, speed=800, speed_var=300, radius=14, radius_var=6, grow=0.0, fade=0.15,
                    rotation=(6.0, 4.0), gravity=((0, 0, 1), -1400), translation=(0, 0, 80), world_space=True,
                    colours=((0.0, (0.55, 0.45, 0.35, 1.0)), (0.8, (0.3, 0.22, 0.15, 1.0)), (1.0, (0.2, 0.15, 0.1, 0.0))))
        e.particles('Soul', TEXTURES['glow'], emitter=('sphere', 60), rate=300, start=0.05, stop=0.4, duration=T, life=0.7,
                    life_var=0.2, speed=120, speed_var=60, radius=30, radius_var=10, grow=0.1, fade=0.3,
                    rotation=(0.3, 0.3), gravity=((0, 0, 1), 350), translation=(0, 0, 70), world_space=True,
                    colours=((0.0, (0.4, 1.0, 0.45, 0.0)), (0.3, (0.35, 0.95, 0.4, 0.7)), (1.0, (0.1, 0.4, 0.15, 0.0))))
        e.particles('Smoke', TEXTURES['glow'], emitter=('cylinder', 200, 60), rate=260, start=0.1, stop=0.7, duration=T,
                    life=1.0, life_var=0.25, speed=90, speed_var=40, declination=0.0, declination_var=0.6, radius=110,
                    radius_var=30, grow=0.25, fade=0.4, rotation=(0.2, 0.2), gravity=((0, 0, 1), 120),
                    translation=(0, 0, 60), world_space=True,
                    colours=((0.0, (0.25, 0.18, 0.12, 0.0)), (0.3, (0.18, 0.14, 0.12, 0.55)), (1.0, (0.08, 0.07, 0.07, 0.0))))
        return e.encode(T, SummonBoom.BOUND)


class PushHit:
    """04.11: on an enemy the push struck (Div2Fix OwnHitFx54, a free effect at its feet): violet sparks over the body"""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 100.0, 150.0, -100.0, -100.0, 0.0, 100.0, 100.0, 230.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ArcanePush_Hit', source=None, **kw):
        import fx_icebolt
        T = PushHit.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Flash', TEXTURES['glow'], emitter=('cylinder', 25, 150), rate=300, stop=0.08, duration=T, life=0.3, life_var=0.08,
                    speed=30, speed_var=15, radius=32, radius_var=8, grow=0.05, fade=0.2, rotation=(0.3, 0.3),
                    translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.75, 0.65, 1.0, 0.8)), (1.0, (0.3, 0.25, 1.0, 0.0))))
        e.particles('Sparks', TEXTURES['spark'], emitter=('cylinder', 30, 150), rate=700, stop=0.08, duration=T, life=0.5, life_var=0.12,
                    speed=280, speed_var=120, declination=0.0, declination_var=3.1416, radius=7, radius_var=2, grow=0.02, fade=0.2,
                    rotation=(4.0, 3.0), gravity=((0, 0, 1), -700), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (1.0, 0.95, 1.0, 1.0)), (1.0, (0.4, 0.35, 1.0, 0.0))))
        return e.encode(T, PushHit.BOUND)


class ShieldCast:
    """04.11: Arcane Shield on the caster (Div2Fix OwnFx96, plays OwnAnim96): motes rising and closing round the body"""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 0.0, 120.0, -100.0, -100.0, -150.0, 100.0, 100.0, 100.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ArcaneShield_Cast', source=None, **kw):
        import fx_icebolt
        T = ShieldCast.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Rise', TEXTURES['glow'], emitter=('cylinder', 50, 10), rate=160, stop=0.45, duration=T, life=0.4, life_var=0.08,
                    speed=280, speed_var=50, declination=0.0, declination_var=0.1, radius=16, radius_var=5, grow=0.05, fade=0.2,
                    rotation=(0.3, 0.3), translation=(0, 0, -130), world_space=False,
                    colours=((0.0, (0.5, 0.75, 1.0, 0.8)), (1.0, (0.3, 0.3, 1.0, 0.0))))
        e.particles('Motes', TEXTURES['spark'], emitter=('cylinder', 55, 170), rate=350, start=0.2, stop=0.5, duration=T, life=0.3,
                    life_var=0.08, speed=-60, speed_var=30, radius=6, radius_var=2, grow=0.03, fade=0.15,
                    rotation=(3.0, 2.0), translation=(0, 0, -50), world_space=False,
                    colours=((0.0, (0.9, 0.95, 1.0, 1.0)), (1.0, (0.4, 0.5, 1.0, 0.0))))
        return e.encode(T, ShieldCast.BOUND)


class MarkTarget:
    """05.11: Hunter's Mark: a red target over the marked enemy. Div2Fix starts one every 40 ms where the enemy is now, each lives 90 ms (a free effect cannot
    follow a character), so it holds no rotation or fade, and about two overlap: the colour is half bright"""
    SOURCE = None
    DURATION = 0.2
    BOUND = (0.0, 0.0, 110.0, 120.0, -100.0, -100.0, 0.0, 100.0, 100.0, 220.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='HuntersMark_Target', source=None, **kw):
        import fx_icebolt
        T = MarkTarget.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Target', TEXTURES['mark'], emitter=('sphere', 1), rate=200, stop=0.01, duration=T, life=0.11, life_var=0.0, speed=0, speed_var=0,
                    radius=62, radius_var=0, grow=0.0, fade=0.0, rotation=(0.0, 0.0), translation=(0, 0, 110),
                    colours=((0.0, (0.5, 0.5, 0.5, 0.5)), (1.0, (0.5, 0.5, 0.5, 0.5))))
        return e.encode(T, MarkTarget.BOUND)


class WindBurst:
    """05.11: Second Wind: golden sparks rising around the hero (Div2Fix WindFx)"""
    SOURCE = None
    DURATION = 1.4
    BOUND = (0.0, 0.0, 100.0, 170.0, -100.0, -100.0, 0.0, 100.0, 100.0, 240.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='SecondWind_Burst', source=None, **kw):
        import fx_icebolt
        T = WindBurst.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Rise', TEXTURES['spark'], emitter=('cylinder', 55, 30), rate=450, stop=0.25, duration=T, life=0.9, life_var=0.2,
                    speed=210, speed_var=70, declination=0.0, declination_var=0.25, radius=8, radius_var=3, grow=0.03, fade=0.35,
                    rotation=(3.0, 2.0), translation=(0, 0, 10), world_space=True,
                    colours=((0.0, (1.0, 0.95, 0.65, 1.0)), (0.5, (1.0, 0.75, 0.25, 0.9)), (1.0, (0.8, 0.35, 0.05, 0.0))))
        e.particles('Glow', TEXTURES['glow'], emitter=('sphere', 10), rate=40, stop=0.12, duration=T, life=0.7, life_var=0.1, speed=20, speed_var=10,
                    radius=95, radius_var=15, grow=0.2, fade=0.5, rotation=(0.3, 0.3), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.9, 0.65, 0.25, 0.6)), (1.0, (0.6, 0.25, 0.05, 0.0))))
        return e.encode(T, WindBurst.BOUND)


class ArmorCrack:
    """06.11 Armor Breaker (second version, the first was hardly visible): a big hot flash at the chest, a fan of large sparks,
    heavy fragments falling, red embers rising and lingering (Div2Fix BreakFx)"""
    SOURCE = None
    DURATION = 1.8
    BOUND = (0.0, 0.0, 120.0, 300.0, -260.0, -260.0, -20.0, 260.0, 260.0, 320.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ArmorBreaker_Crack', source=None, **kw):
        import fx_icebolt
        T = ArmorCrack.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Flash', TEXTURES['glow'], emitter=('sphere', 10), rate=120, stop=0.08, duration=T, life=0.45, life_var=0.05, speed=15, speed_var=5,
                    radius=170, radius_var=20, grow=0.25, fade=0.4, rotation=(0.3, 0.3), translation=(0, 0, 120), world_space=True,
                    colours=((0.0, (1.0, 0.85, 0.5, 1.0)), (0.4, (1.0, 0.45, 0.1, 0.8)), (1.0, (0.6, 0.1, 0.02, 0.0))))
        e.particles('Sparks', TEXTURES['spark'], emitter=('sphere', 30), rate=750, stop=0.12, duration=T, life=0.8, life_var=0.2,
                    speed=430, speed_var=150, radius=11, radius_var=3, grow=0.03, fade=0.35, rotation=(3.0, 2.0),
                    translation=(0, 0, 120), gravity=((0, 0, 1), -700), world_space=True,
                    colours=((0.0, (1.0, 0.95, 0.7, 1.0)), (0.5, (1.0, 0.55, 0.12, 1.0)), (1.0, (0.7, 0.15, 0.03, 0.0))))
        e.particles('Fragments', TEXTURES['spark'], emitter=('sphere', 40), rate=220, stop=0.15, duration=T, life=1.2, life_var=0.25,
                    speed=240, speed_var=90, radius=30, radius_var=8, grow=0.02, fade=0.6, rotation=(4.0, 3.0),
                    translation=(0, 0, 130), gravity=((0, 0, 1), -900), world_space=True,
                    colours=((0.0, (1.0, 0.8, 0.5, 1.0)), (1.0, (0.35, 0.2, 0.15, 0.0))))
        e.particles('Embers', TEXTURES['spark'], emitter=('cylinder', 60, 90), rate=140, stop=0.9, duration=T, life=0.9, life_var=0.2,
                    speed=90, speed_var=40, declination=0.0, declination_var=0.3, radius=10, radius_var=3, grow=0.03, fade=0.5,
                    rotation=(2.0, 2.0), translation=(0, 0, 60), world_space=True,
                    colours=((0.0, (1.0, 0.4, 0.1, 1.0)), (1.0, (0.7, 0.05, 0.02, 0.0))))
        return e.encode(T, ArmorCrack.BOUND)


class CryWave:
    """06.11 Battle Cry: a red shock ring running over the ground to the radius of the cry, a hot flash on the hero, dust thrown out (Div2Fix OwnFx)"""
    SOURCE = None
    DURATION = 1.1
    BOUND = (0.0, 0.0, 100.0, 900.0, -850.0, -850.0, -20.0, 850.0, 850.0, 320.0)

    @staticmethod
    def textures():
        import fx_icebolt
        return {**textures(), **fx_icebolt.textures()}

    @staticmethod
    def build(name='BattleCry_Wave', source=None, **kw):
        import fx_icebolt
        T = CryWave.DURATION
        e = fx_icebolt._Effect(name)
        still = [(0.0, (0, 0, 15)), (T, (0, 0, 15))]
        e.add('Ring', fx_icebolt.TEX['ring'], fx._annulus(), [(0.0, 60), (0.55, 800), (T, 850)], still,
              [(0.0, 2.2), (0.35, 1.6), (0.8, 0.0), (T, 0.0)], tint=(1.0, 0.3, 0.08))
        e.add('Ring2', fx_icebolt.TEX['ring'], fx._annulus(), [(0.0, 30), (0.12, 30), (0.7, 620), (T, 660)], still,
              [(0.0, 0.0), (0.12, 1.4), (0.45, 1.0), (0.9, 0.0), (T, 0.0)], tint=(1.0, 0.55, 0.15))
        e.particles('Flash', TEXTURES['glow'], emitter=('sphere', 15), rate=80, stop=0.1, duration=T, life=0.45, life_var=0.08, speed=20, speed_var=10,
                    radius=150, radius_var=20, grow=0.25, fade=0.45, rotation=(0.3, 0.3), translation=(0, 0, 130), world_space=True,
                    colours=((0.0, (1.0, 0.6, 0.25, 0.9)), (1.0, (0.7, 0.1, 0.03, 0.0))))
        e.particles('Dust', TEXTURES['spark'], emitter=('cylinder', 40, 10), rate=500, stop=0.2, duration=T, life=0.7, life_var=0.15,
                    speed=420, speed_var=120, declination=1.45, declination_var=0.1, radius=9, radius_var=3, grow=0.03, fade=0.4,
                    rotation=(2.0, 2.0), translation=(0, 0, 25), world_space=True,
                    colours=((0.0, (1.0, 0.75, 0.4, 1.0)), (1.0, (0.6, 0.15, 0.04, 0.0))))
        return e.encode(T, CryWave.BOUND)


class SmiteHit:
    """05.11, second version: light falls from the sky on the target: a white flash, a dense tall column, streaks falling fast down it, a golden wave running out over the
    ground, sparks thrown up, motes rising after it (Div2Fix OwnHitFx101)"""
    SOURCE = None
    DURATION = 1.6
    BOUND = (0.0, 0.0, 300.0, 520.0, -300.0, -300.0, 0.0, 300.0, 300.0, 800.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='HolySmite_Hit', source=None, **kw):
        import fx_icebolt
        T = SmiteHit.DURATION
        gold = ((0.0, (1.0, 0.97, 0.8, 1.0)), (0.5, (1.0, 0.8, 0.35, 0.8)), (1.0, (0.85, 0.4, 0.08, 0.0)))
        e = fx_icebolt._Effect(name)
        e.particles('Flash', TEXTURES['glow'], emitter=('sphere', 5), rate=120, stop=0.05, duration=T, life=0.28, life_var=0.05, speed=0, speed_var=0,
                    radius=210, radius_var=20, grow=0.1, fade=0.7, rotation=(0.0, 0.0), translation=(0, 0, 110), world_space=True,
                    colours=((0.0, (1.0, 1.0, 0.92, 1.0)), (1.0, (1.0, 0.75, 0.3, 0.0))))
        e.particles('Column', TEXTURES['glow'], emitter=('cylinder', 10, 760), rate=1500, stop=0.22, duration=T, life=0.45, life_var=0.1, speed=15, speed_var=10,
                    declination=0.0, declination_var=0.2, radius=52, radius_var=10, grow=0.04, fade=0.6, rotation=(0.2, 0.2), translation=(0, 0, 390), world_space=True, colours=gold)
        e.particles('Core', TEXTURES['glow'], emitter=('cylinder', 3, 760), rate=1000, stop=0.2, duration=T, life=0.3, life_var=0.05, speed=0, speed_var=0,
                    radius=20, radius_var=4, grow=0.02, fade=0.5, rotation=(0.0, 0.0), translation=(0, 0, 390), world_space=True,
                    colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 0.9, 0.6, 0.0))))
        e.particles('Fall', TEXTURES['spark'], emitter=('cylinder', 45, 200), rate=420, stop=0.28, duration=T, life=0.42, life_var=0.08, speed=1500, speed_var=300,
                    declination=3.1416, declination_var=0.05, radius=9, radius_var=3, grow=0.0, fade=0.2, rotation=(0.0, 0.0), translation=(0, 0, 720), world_space=True, colours=gold)
        e.particles('Wave', TEXTURES['glow'], emitter=('cylinder', 30, 6), rate=900, start=0.03, stop=0.12, duration=T, life=0.5, life_var=0.08, speed=720, speed_var=60,
                    declination=1.5708, declination_var=0.04, planar=0.0, planar_var=3.1416, radius=42, radius_var=10, grow=0.12, fade=0.5, rotation=(0.4, 0.4), translation=(0, 0, 14), world_space=True, colours=gold)
        e.particles('Sparks', TEXTURES['spark'], emitter=('cylinder', 40, 12), rate=700, start=0.02, stop=0.14, duration=T, life=0.8, life_var=0.25, speed=520, speed_var=220,
                    declination=0.45, declination_var=0.4, radius=8, radius_var=3, grow=0.02, fade=0.3, rotation=(4.0, 3.0), gravity=((0, 0, 1), -800), translation=(0, 0, 15), world_space=True, colours=gold)
        e.particles('Motes', TEXTURES['spark'], emitter=('cylinder', 80, 30), rate=90, start=0.15, stop=0.8, duration=T, life=0.8, life_var=0.2, speed=130, speed_var=50,
                    declination=0.0, declination_var=0.25, radius=6, radius_var=2, grow=0.02, fade=0.4, rotation=(1.0, 1.0), translation=(0, 0, 20), world_space=True, colours=gold)
        return e.encode(T, SmiteHit.BOUND)


class SmiteCast:
    """05.11: Holy Smite on the caster: golden light gathers and rises around the raised hands (Div2Fix OwnFx101)"""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 120.0, 190.0, -120.0, -120.0, 0.0, 120.0, 120.0, 260.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='HolySmite_Cast', source=None, **kw):
        import fx_icebolt
        T = SmiteCast.DURATION
        gold = ((0.0, (1.0, 0.95, 0.7, 0.0)), (0.3, (1.0, 0.85, 0.45, 1.0)), (1.0, (0.9, 0.45, 0.1, 0.0)))
        e = fx_icebolt._Effect(name)
        e.particles('Rise', TEXTURES['spark'], emitter=('cylinder', 60, 40), rate=260, stop=0.5, duration=T, life=0.6, life_var=0.15, speed=230, speed_var=70,
                    declination=0.0, declination_var=0.15, radius=7, radius_var=3, grow=0.02, fade=0.3, rotation=(2.0, 2.0), translation=(0, 0, 25), colours=gold)
        e.particles('Halo', TEXTURES['glow'], emitter=('sphere', 8), rate=40, start=0.25, stop=0.5, duration=T, life=0.35, life_var=0.05, speed=5, speed_var=5,
                    radius=75, radius_var=10, grow=0.15, fade=0.5, rotation=(0.2, 0.2), translation=(0, 0, 215), colours=gold)
        return e.encode(T, SmiteCast.BOUND)


class BlessAura:
    """06.11: Blessing on the hero: soft golden motes rising, a faint glow; started again about every second (Div2Fix OwnHitFx102)"""
    SOURCE = None
    DURATION = 1.3
    BOUND = (0.0, 0.0, 100.0, 170.0, -100.0, -100.0, 0.0, 100.0, 100.0, 240.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='Blessing_Aura', source=None, **kw):
        import fx_icebolt
        T = BlessAura.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Rise', TEXTURES['spark'], emitter=('cylinder', 55, 20), rate=40, stop=0.6, duration=T, life=0.7, life_var=0.15, speed=170, speed_var=50,
                    declination=0.0, declination_var=0.15, radius=7, radius_var=2, grow=0.02, fade=0.4, rotation=(1.0, 1.0), translation=(0, 0, 15), world_space=True,
                    colours=((0.0, (1.0, 0.95, 0.75, 0.0)), (0.3, (1.0, 0.88, 0.55, 1.0)), (1.0, (0.95, 0.6, 0.2, 0.0))))
        return e.encode(T, BlessAura.BOUND)


class DrainHit:
    """06.11: Drain on its target at every tick: red motes torn out of the body and rising, a sickly green haze (Div2Fix OwnHitFx104)"""
    SOURCE = None
    DURATION = 1.0
    BOUND = (0.0, 0.0, 110.0, 170.0, -100.0, -100.0, 0.0, 100.0, 100.0, 260.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='Drain_Hit', source=None, **kw):
        import fx_icebolt
        T = DrainHit.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Haze', TEXTURES['glow'], emitter=('cylinder', 30, 90), rate=45, stop=0.25, duration=T, life=0.6, life_var=0.1, speed=30, speed_var=15,
                    radius=48, radius_var=10, grow=0.15, fade=0.5, rotation=(0.3, 0.3), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.35, 0.8, 0.15, 0.0)), (0.3, (0.3, 0.7, 0.12, 0.7)), (1.0, (0.1, 0.3, 0.05, 0.0))))
        e.particles('Blood', TEXTURES['spark'], emitter=('cylinder', 26, 110), rate=170, stop=0.3, duration=T, life=0.65, life_var=0.15, speed=190, speed_var=70,
                    declination=0.0, declination_var=0.3, radius=8, radius_var=3, grow=0.02, fade=0.35, rotation=(2.0, 2.0), translation=(0, 0, 90), world_space=True,
                    colours=((0.0, (1.0, 0.25, 0.1, 1.0)), (1.0, (0.5, 0.02, 0.02, 0.0))))
        return e.encode(T, DrainHit.BOUND)


class CurseAura:
    """09.11: on an enemy infected by Contagious Curse. Div2Fix starts one every 120 ms where the enemy is now and each lives 950 ms, so about eight overlap: every
    puff is only a few dim particles. A violet haze around the body and violet motes rising from it (the game's Curse_Active was created but not seen)"""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 110.0, 170.0, -100.0, -100.0, 0.0, 100.0, 100.0, 260.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ContagiousCurse_Aura', source=None, **kw):
        import fx_icebolt
        T = CurseAura.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Haze', TEXTURES['glow'], emitter=('cylinder', 30, 90), rate=30, stop=0.1, duration=T, life=0.7, life_var=0.1, speed=25, speed_var=10,
                    radius=46, radius_var=10, grow=0.15, fade=0.5, rotation=(0.3, 0.3), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.55, 0.15, 0.9, 0.0)), (0.3, (0.5, 0.12, 0.85, 0.55)), (1.0, (0.2, 0.03, 0.4, 0.0))))
        e.particles('Motes', TEXTURES['spark'], emitter=('cylinder', 28, 110), rate=40, stop=0.1, duration=T, life=0.75, life_var=0.1, speed=90, speed_var=30,
                    declination=0.0, declination_var=0.3, radius=7, radius_var=2, grow=0.02, fade=0.35, rotation=(2.0, 2.0), translation=(0, 0, 90), world_space=True,
                    colours=((0.0, (0.8, 0.4, 1.0, 1.0)), (1.0, (0.35, 0.05, 0.6, 0.0))))
        return e.encode(T, CurseAura.BOUND)


class DrainCast:
    """06.11: Drain on the caster: sickly green wisps and red sparks really fly in to the hands: an emitter per point on a circle around him, each shooting straight at the hands
    (-Y is forward), at the speed that brings a particle there at the end of its life (Div2Fix OwnFx104)"""
    SOURCE = None
    DURATION = 1.0
    BOUND = (0.0, 0.0, 110.0, 200.0, -170.0, -170.0, 0.0, 170.0, 170.0, 260.0)
    HANDS = (0.0, -45.0, 135.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='Drain_Cast', source=None, **kw):
        import math
        import fx_icebolt
        T = DrainCast.DURATION
        hx, hy, hz = DrainCast.HANDS
        e = fx_icebolt._Effect(name)
        green = ((0.0, (0.3, 0.8, 0.1, 0.0)), (0.25, (0.4, 0.95, 0.15, 0.9)), (0.8, (0.6, 1.0, 0.3, 0.8)), (1.0, (0.9, 1.0, 0.6, 0.0)))
        red = ((0.0, (1.0, 0.25, 0.1, 0.0)), (0.3, (1.0, 0.2, 0.06, 1.0)), (1.0, (1.0, 0.5, 0.2, 0.0)))
        n = 10
        for k in range(n):
            a = 2 * math.pi * k / n
            r = 150.0 if k % 2 else 115.0
            z = 45.0 + 150.0 * ((k * 3) % n) / (n - 1)
            x, y = r * math.cos(a), r * math.sin(a)
            vx, vy, vz = hx - x, hy - y, hz - z
            dist = math.sqrt(vx * vx + vy * vy + vz * vz)
            decl, plan = math.acos(vz / dist), math.atan2(vy, vx)
            life = 0.42
            e.particles('Wisp%d' % k, TEXTURES['glow'], emitter=('sphere', 10), rate=26, start=0.02 * k, stop=0.5, duration=T, life=life, life_var=0.0, speed=dist / life, speed_var=0,
                        declination=decl, declination_var=0.05, planar=plan, planar_var=0.05, radius=20, radius_var=5, grow=0.1, fade=0.3, rotation=(0.5, 0.5), translation=(x, y, z), colours=green)
            e.particles('Spark%d' % k, TEXTURES['spark'], emitter=('sphere', 10), rate=18, start=0.02 * k, stop=0.5, duration=T, life=life, life_var=0.0, speed=dist / life, speed_var=0,
                        declination=decl, declination_var=0.08, planar=plan, planar_var=0.08, radius=6, radius_var=2, grow=0.02, fade=0.3, rotation=(2.0, 2.0), translation=(x, y, z), colours=red)
        return e.encode(T, DrainCast.BOUND)


class ShieldHit:
    """04.11: a free effect on the hero when the shield takes a hit and when it falls (Div2Fix OwnShield96)"""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 100.0, 150.0, -100.0, -100.0, 0.0, 100.0, 100.0, 230.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='ArcaneShield_Hit', source=None, **kw):
        import fx_icebolt
        T = ShieldHit.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Sparks', TEXTURES['spark'], emitter=('cylinder', 35, 160), rate=500, stop=0.08, duration=T, life=0.4, life_var=0.1,
                    speed=160, speed_var=80, declination=0.0, declination_var=3.1416, radius=7, radius_var=2, grow=0.02, fade=0.2,
                    rotation=(4.0, 3.0), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.9, 0.95, 1.0, 1.0)), (1.0, (0.3, 0.45, 1.0, 0.0))))
        return e.encode(T, ShieldHit.BOUND)


class _Blink:
    """04.11: Arcane Blink, Div2Fix OwnBlink54: a free effect where the hero leaves (motes rush in) and where he lands
    (motes burst out), violet-blue over his whole height"""
    SOURCE = None
    DURATION = 0.9
    BOUND = (0.0, 0.0, 100.0, 170.0, -100.0, -100.0, 0.0, 100.0, 100.0, 240.0)
    textures = staticmethod(lambda: textures())
    SPEED = 90

    @classmethod
    def build(cls, name='ArcaneBlink', source=None, **kw):
        import fx_icebolt
        T = cls.DURATION
        e = fx_icebolt._Effect(name)
        e.particles('Veil', TEXTURES['glow'], emitter=('cylinder', 30, 170), rate=400, stop=0.08, duration=T, life=0.55,
                    life_var=0.1, speed=cls.SPEED * 0.3, speed_var=10, radius=30, radius_var=8, grow=0.05, fade=0.3,
                    rotation=(0.3, 0.3), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.5, 0.45, 1.0, 0.75)), (1.0, (0.25, 0.2, 0.9, 0.0))))
        e.particles('Motes', TEXTURES['spark'], emitter=('cylinder', 45, 170), rate=900, stop=0.08, duration=T, life=0.6,
                    life_var=0.15, speed=cls.SPEED, speed_var=40, radius=8, radius_var=3, grow=0.03, fade=0.25,
                    rotation=(3.0, 2.0), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.9, 0.9, 1.0, 0.9)), (0.5, (0.6, 0.6, 1.0, 0.7)), (1.0, (0.35, 0.3, 1.0, 0.0))))
        return e.encode(T, cls.BOUND)


class BlinkOut(_Blink):
    SPEED = -90


class BlinkIn(_Blink):
    SPEED = 200


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
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
