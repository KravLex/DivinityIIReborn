r"""Ice Bolt's own effects, made from code (03.11: before, the Fireball's effects recoloured blue).

  py -3 -I fx_icebolt.py check      sizes of the three files

The projectile itself stays the game's Fireball code (flight, collision, damage); Div2Fix [MageSkills]
Fx14=IceBolt_Travel,IceBolt_Impact,IceBolt_Cast swaps the three effects it shows. Made like Frost Nova's and Chain
Lightning's effects (fx_frostnova helpers: additive or opaque quads, emissive keys in a NiControllerSequence) and with
Frost Nova's textures (fx_frostnova_*, already in the archive). 03.11 (the user: volume, particles): each file is the
game's own Fireball effect with its particle systems (the trail, the burst, the sparks) - coloured cold (nif_fx.tint) and
with our ice sprites in place of fire and smoke (fx_icebolt_flake, fx_icebolt_mist) - and our parts added to it:
  Travel - on the flying projectile, the same from every side (its flight axis is not known inside the effect): a spiky
           crystal of opaque ice blades pointing every way, a pulsing cold core, frost sparks circling it, a soft haze;
           the keys repeat every PULSE s for the 16.7 s of the game's own travel effect (clamped, as the game's)
  Impact - where it lands: a white-blue flash, a frost ring, ice splinters thrown out and falling, frost puffs
  Cast   - in the casting hand: a cold glow gathering and a few sparks
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fx_frostnova as fx           # noqa: E402
import nif_write
import psys                    # noqa: E402

TEX = fx.TEXTURES                   # Frost Nova's: ring, frost, shard, flash, haze, rime
COLD = (0.45, 0.8, 1.0)
PULSE = 0.5


SOURCES = {'Travel': r'Win32\Effects\Human_Fireball\Human_Fireball.item',
           'Impact': r'Win32\Effects\Fireball\Fireball_Impact_B.item',
           'Cast': None}            # 03.11: the cast's particles summed into a white ball at the hand: ours only
# the Fireball's particle textures -> ours (fire and smoke become frost, the rest cold versions)
RETEXTURE = {'fx_fire_a': 'fx_icebolt_flake', 'fx_alphasmoke_a': 'fx_icebolt_mist', 'fx_alphasmoke_c': 'fx_icebolt_mist',
             'fx_alphasmoke_f': 'fx_icebolt_mist', 'exp_fx_explosion_a': 'fx_icebolt_flake',
             # their pixels are warm too (the tint changes only the colours in the file): all of them ours
             'fx_halo_a': 'fx_frostnova_flash', 'fx_sparks_e': 'fx_icebolt_flake', 'fx_sparks_l': 'fx_icebolt_flake',
             'fx_8star_b': 'fx_frostnova_flash', 'fx_ring_c': 'fx_icebolt_ring', 'fx_hit_i': 'fx_frostnova_flash'}
# 03.11: Frost Nova's ring texture is a band along u for an annulus; on the Fireball's flat ground quad it filled the
# whole square (a cyan flood with straight edges): a round ring of our own
SAT, GREEN = 0.9, 0.95              # nif_fx.tint matrix: red <-> blue (03.11: 0.55 left the sparks golden)
LIGHT = 0.45                        # and darker (the floor light)


def tex_flake(size=512):
    """an ice sparkle: a bright six-pointed crystal star with a cold glow (for the trail and burst particles)"""
    u, v, r, a = fx._grid(size)
    star = sum(np.exp(-(np.abs(np.sin(a - k * np.pi / 3)) * r / 0.02) ** 2) * np.exp(-(r / 0.75) ** 2) for k in range(3))
    core = np.exp(-(r / 0.12) ** 2)
    glow = np.exp(-(r / 0.45) ** 2) * 0.5
    # 03.11: dim and blue (white sprites in dozens of additive particles burned into a white blot)
    rgb = (core * 0.7 + star * 0.5)[..., None] * (0.5 * fx.WHITE + 0.5 * fx.CYAN) + glow[..., None] * fx.DEEP * 0.4
    rgb *= (r < 1)[..., None]
    return fx._rgba(rgb)


def tex_mist(size=512, seed=17):
    """a soft frost cloud: noisy, round, nothing at the edge"""
    import icon_art
    rng = np.random.default_rng(seed)
    u, v, r, a = fx._grid(size)
    n = icon_art._noise(size, size // 8, rng) * 0.6 + icon_art._noise(size, size // 24, rng) * 0.4
    cloud = np.clip((1 - r) / 0.6, 0, 1) ** 1.5 * (0.45 + 0.55 * n)
    rgb = cloud[..., None] * (0.6 * fx.DEEP + 0.4 * fx.CYAN) * 0.35
    return fx._rgba(rgb, alpha_gain=1.1)


def tex_ring(size=512):
    """a round shock ring in the square, transparent inside and out"""
    u, v, r, a = fx._grid(size)
    ring = np.exp(-((r - 0.78) / 0.07) ** 2) + 0.35 * np.exp(-((r - 0.68) / 0.12) ** 2)
    rgb = ring[..., None] * (0.5 * fx.CYAN + 0.5 * fx.DEEP) * 0.6 * (r < 1)[..., None]
    return fx._rgba(rgb, alpha_gain=1.2)


def textures():
    tex = {'fx_icebolt_flake': tex_flake(), 'fx_icebolt_mist': tex_mist(), 'fx_icebolt_ring': tex_ring()}
    return {k: (np.clip(v, 0, 1) * 255 + 0.5).astype(np.uint8) for k, v in tex.items()}


def _tint_matrix():
    lum = np.array([0.299, 0.587, 0.114])
    m = SAT * np.array([[0, 0, 1], [0, 1, 0], [1, 0, 0]], float) + (1 - SAT) * np.tile(lum, (3, 1))
    m[1] *= GREEN
    # 03.11: the floor light of the game's Fireball (its lighting, not a block of the file: HasEmittance off did not
    # remove it) follows these colours - a cyan flood; darker colours, a soft cold glow
    return m * LIGHT



def _particle_base(source):
    """the game's effect, coloured cold, ice sprites in place of fire/smoke: (nif_write.Nif, retextured names)"""
    import nif_fx
    nf = nif_fx.Nif(source)
    nf.tint(_tint_matrix())
    nif = nif_write.Nif(bytes(nf.data))
    done = []
    for b in nif.blocks:
        if b.type == 'NiSourceTexture':
            name = nif.strings[b['file']]
            new = RETEXTURE.get(name.rsplit('.', 1)[0].lower())
            if new:
                b['file'] = nif.string(new + '.tga')
                done.append('%s -> %s' % (name, new))
    # 03.11: the Fireball's shock ring grows over its blast area, many metres (the user: as if the bolt had an area
    # attack); hidden (APP_CULLED), our own small ring stays
    for b in nif.blocks:
        # and its flat 'Flash' quad: a big square on the ground at the impact (03.11 screenshot); ours faces the camera
        if b.type in ('NiNode', 'NiTriShape', 'NiBillboardNode') and b['name'] >= 0 and nif.strings[b['name']] in ('Ring', 'Flash'):
            b['flags'] |= 1
            done.append(nif.strings[b['name']] + ' hidden')
    return nif, done


def _no_emittance(nif):
    """03.11: HasEmittance makes the engine light the ground around those parts - a cold flood over the whole
    floor on the cast, the flight and the impact; set to false"""
    for b in nif.blocks:
        if b.type == 'NiBooleanExtraData' and b['name'] >= 0 and nif.strings[b['name']] == 'HasEmittance':
            b['value'] = 0


# ---------------------------------------------------------------- geometry
def _spikes(count, length, width, seed):
    """opaque ice blades from the centre in every direction (four-sided pyramids)"""
    rng = np.random.default_rng(seed)
    verts, uvs = [], []
    for k in range(count):
        # spread over the sphere (golden spiral) with a little jitter
        z = 1 - 2 * (k + 0.5) / count
        r = np.sqrt(max(0.0, 1 - z * z))
        a = k * 2.399963 + rng.uniform(-0.2, 0.2)
        d = np.array([r * np.cos(a), r * np.sin(a), z])
        d /= np.linalg.norm(d)
        up = np.array([0, 0, 1.0]) if abs(d[2]) < 0.9 else np.array([1.0, 0, 0])
        u = np.cross(d, up); u /= np.linalg.norm(u)
        v = np.cross(d, u)
        L = length * rng.uniform(0.6, 1.15)
        w = width * rng.uniform(0.7, 1.2) / 2
        tip = d * L
        base = [d * L * 0.08 + w * (np.cos(q * np.pi / 2) * u + np.sin(q * np.pi / 2) * v) for q in range(4)]
        for q in range(4):
            for p in (base[q], base[(q + 1) % 4], tip, tip):
                verts.append(tuple(float(c) for c in p))
            uvs += [[0, 1], [1, 1], [0.5, 0], [0.5, 0]]
    return verts, uvs, (0, 0, 1)


QUAD = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
QUAD_UV = [[0, 1], [1, 1], [1, 0], [0, 0]]


# ---------------------------------------------------------------- the NIF
class _Effect:
    def __init__(self, name, base=None):
        self.name = name
        self.base = base
        self.systems = []                   # 03.11: particle system nodes (psys.add)
        self.parts = []                     # (part tuple, name, scale keys, translation keys, glow keys, tint)
        if base is not None:                # our parts go into the game's effect (its NonAccum and its sequence)
            self.nif = base
            self.nonaccum = next(i for i, x in enumerate(base.blocks)
                                 if x.type == 'NiNode' and x['name'] >= 0 and base.strings[x['name']].endswith('NonAccum'))
            return
        self.nif = nif = nif_write.Nif()
        self.aroot = 'AROOT_' + name
        nif.add('CStreamableAssetData', raw=b'')
        self.root = fx._node(nif, 'NiNode', 'Scene Root', scale=0.01, flags=0x102)
        self.maxb = nif.add('NiFloatsExtraData', name=nif.string('MaxBound'), values=[0.0] * 10)
        z = nif.add('NiZBufferProperty', name=-1, extra=[], controller=-1, flags=15)
        vc = nif.add('NiVertexColorProperty', name=-1, extra=[], controller=-1, flags=8)
        arootn = fx._node(nif, 'NiNode', self.aroot, flags=0x102)
        upb = nif.add('NiStringExtraData', name=nif.string('UserPropBuffer'), value=nif.string('KFAccumRoot = \r\n'))
        self.nonaccum = fx._node(nif, 'NiNode', self.aroot + ' NonAccum', flags=0x186)
        nif.blocks[self.root].f.update(children=[arootn], extra=[self.maxb], properties=[vc, z])
        nif.blocks[arootn].f.update(children=[self.nonaccum], extra=[upb])

    def add(self, name, tex, geo, scales, path, glow, tint=COLD, billboard=False, opaque=False):
        p = fx._part(self.nif, name, tex, *geo, billboard=billboard, opaque=opaque)
        self.parts.append((p, name, scales, path, glow, tint))

    def particles(self, name, tex, **kw):
        self.systems.append(psys.add(self.nif, name, tex, **kw))

    def encode(self, T, bound):
        if self.base is not None:
            return self._encode_into(T)
        nif = self.nif
        nif.blocks[self.nonaccum]['children'] = [p[0][0] for p in self.parts] + self.systems
        for (node, shape_name, mat), *_ in self.parts:
            blend = nif.add('NiBlendPoint3Interpolator', raw=b'\x05\x02\x00\x00\x00\x00' + struct.pack('<3f', *[-3.4028235e38] * 3))
            ctrl = nif.add('NiMaterialColorController', next=-1, flags=0x6C, frequency=1.0, phase=0.0, start=0.0,
                           stop=T, target=mat, interpolator=blend, color=3)
            nif.blocks[mat]['controller'] = ctrl
        items = [{'interpolator': -1, 'controller': -1, 'node': nif.string(self.aroot), 'property': -1,
                  'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1},
                 {'interpolator': -1, 'controller': -1, 'node': nif.string(self.aroot + ' NonAccum'), 'property': -1,
                  'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1}]
        seq_index = len(nif.blocks)
        seq = nif.add('NiControllerSequence', name=nif.string(self.name), extra=[], controller=-1, blocks=None,
                      weight=1.0, text_keys=-1, cycle=2, frequency=1.0, start=0.0, stop=T, manager=-1,
                      accum_root=nif.string(self.aroot), tail=64)
        assert seq == seq_index
        for (node, shape_name, mat), name, scales, path, glow, tint in self.parts:
            nif.blocks[node]['translation'] = list(path[0][1])
            ti = fx._keys_ts(nif, scales, path)
            items.append({'interpolator': ti, 'controller': -1, 'node': nif.string(name), 'property': -1,
                          'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1})
            ei = fx._keys_emissive(nif, glow, tint)
            items.append({'interpolator': ei, 'controller': -1, 'node': nif.string(shape_name),
                          'property': nif.string('NiMaterialProperty'), 'ctrl_type': nif.string('NiMaterialColorController'),
                          'ctrl_id': nif.string('SELF_ILLUM'), 'interp_id': -1})
        tk = nif.add('NiTextKeyExtraData', name=-1, keys=[[0.0, nif.string('start')], [T, nif.string('end')]])
        nif.blocks[seq]['blocks'] = {'grow': 1, 'items': items + getattr(nif, 'psys_items', [])}
        nif.blocks[seq]['text_keys'] = tk
        nif.blocks[self.maxb]['values'] = list(bound)

        def st(s):
            b = s.encode('latin1')
            return struct.pack('<I', len(b)) + b
        body = (st('.\\%s.nif' % self.name) + st(self.aroot) + struct.pack('<II2f2I', 1, 2, 0.1, 0.1, 1, 0)
                + st('%s.kf' % self.name) + b'\0' * 12 + struct.pack('<II', 1, seq_index))
        nif.blocks[0] = nif_write.Block('CStreamableAssetData', raw=struct.pack('<IBI', 1, 1, len(body) - 8) + body)
        nif.roots = [0]
        return nif.encode()


def _encode_into(self, T):
    """our parts appended to the game's effect: children of its NonAccum, items of its own sequence (its length)"""
    nif = self.nif
    nif.blocks[self.nonaccum]['children'] = list(nif.blocks[self.nonaccum]['children']) + [p[0][0] for p in self.parts] + self.systems
    seq = next(x for x in nif.blocks if x.type == 'NiControllerSequence')
    T = seq['stop']
    for (node, shape_name, mat), *_ in self.parts:
        blend = nif.add('NiBlendPoint3Interpolator', raw=b'\x05\x02\x00\x00\x00\x00' + struct.pack('<3f', *[-3.4028235e38] * 3))
        ctrl = nif.add('NiMaterialColorController', next=-1, flags=0x6C, frequency=1.0, phase=0.0, start=0.0,
                       stop=T, target=mat, interpolator=blend, color=3)
        nif.blocks[mat]['controller'] = ctrl
    items = seq['blocks']['items']
    items.extend(getattr(nif, 'psys_items', []))
    for (node, shape_name, mat), name, scales, path, glow, tint in self.parts:
        clip = lambda keys: [(t, v) for t, v in keys if t <= T] or keys[:1]
        scales, path, glow = clip(scales), clip(path), clip(glow)
        nif.blocks[node]['translation'] = list(path[0][1])
        ti = fx._keys_ts(nif, scales, path)
        items.append({'interpolator': ti, 'controller': -1, 'node': nif.string(name), 'property': -1,
                      'ctrl_type': nif.string('NiTransformController'), 'ctrl_id': -1, 'interp_id': -1})
        ei = fx._keys_emissive(nif, glow, tint)
        items.append({'interpolator': ei, 'controller': -1, 'node': nif.string(shape_name),
                      'property': nif.string('NiMaterialProperty'), 'ctrl_type': nif.string('NiMaterialColorController'),
                      'ctrl_id': nif.string('SELF_ILLUM'), 'interp_id': -1})
    return nif.encode()


_Effect._encode_into = _encode_into


def _still(T, at=(0, 0, 0)):
    return [(0.0, at), (T, at)]


# ---------------------------------------------------------------- Travel
class Travel:
    SOURCE = SOURCES['Travel']
    DURATION = 16.6666679
    BOUND = (0.0, 0.0, 0.0, 70.0, -60.0, -60.0, -60.0, 60.0, 60.0, 60.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceBolt_Travel', source=None, **kw):
        T = Travel.DURATION
        e = _Effect(name, _particle_base(source)[0] if source else None)
        if e.base is not None:          # 03.11: its light flooded the floor cyan around the bolt as well
            _no_emittance(e.nif)
        n = int(T / PULSE)
        beat = lambda lo, hi: [(k * PULSE + f * PULSE, v) for k in range(n) for f, v in ((0.0, lo), (0.5, hi))] + [(T, lo)]
        real_add = e.add
        if kw.get('trail_only'):        # 06.11 Ice Arrow: no projectile of its own, only the frost it leaves behind
            e.add = lambda *a, **k: None
        # the crystal: opaque ice blades, breathing a little
        e.add('Crystal', TEX['shard'], _spikes(14, 1.0, 0.32, 7), beat(30, 34), _still(T), beat(1.0, 1.5), opaque=True)
        e.add('Inner', TEX['shard'], _spikes(9, 1.0, 0.45, 9), beat(17, 15), _still(T), beat(1.4, 1.1), opaque=True)
        # the core and the haze (camera facing)
        e.add('Core', TEX['flash'], (QUAD, QUAD_UV, (0, -1, 0)), beat(26, 36), _still(T), beat(1.6, 2.2), billboard=True)
        e.add('Haze', TEX['haze'], (QUAD, QUAD_UV, (0, -1, 0)), beat(55, 62), _still(T), beat(0.5, 0.65),
              tint=(0.25, 0.55, 1.0), billboard=True)
        # frost sparks circling the crystal in tilted orbits
        rng = np.random.default_rng(3)
        for k in range(6):
            tilt = rng.uniform(-0.8, 0.8)
            ph = 2 * np.pi * k / 6
            R = rng.uniform(38, 52)
            period = rng.uniform(0.45, 0.7)
            path = []
            t = 0.0
            while t < T:
                a = ph + 2 * np.pi * t / period
                path.append((t, (R * np.cos(a), R * np.sin(a) * np.cos(tilt), R * np.sin(a) * np.sin(tilt))))
                t += period / 8
            path.append((T, path[-1][1]))
            e.add('Spark%d' % k, TEX['flash'], (QUAD, QUAD_UV, (0, -1, 0)), [(0.0, 7), (T, 7)], path,
                  beat(1.8, 0.8), billboard=True)
        e.add = real_add
        # 03.11, our first particle systems: a frost trail left in the world behind the bolt, and flakes falling off it
        if os.environ.get('MAGE_NO_PSYS') is None:
            # 03.11 second try (the first was hardly visible): bigger, denser, longer lived, full alpha, a glowing layer
            e.particles('FrostTrail', TEX['haze'], emitter=('sphere', 30), rate=90, stop=T, life=1.3, life_var=0.3,
                        speed=10, speed_var=8, radius=48, radius_var=12, grow=0.1, fade=0.7, rotation=(0.6, 0.6),
                        colours=((0.0, (0.7, 0.9, 1.0, 1.0)), (0.4, (0.45, 0.7, 1.0, 0.8)), (1.0, (0.2, 0.4, 1.0, 0.0))),
                        world_space=True)
            e.particles('FrostGlow', TEX['flash'], emitter=('sphere', 15), rate=60, stop=T, life=0.6, life_var=0.15,
                        speed=5, speed_var=5, radius=30, radius_var=8, grow=0.05, fade=0.4, rotation=(0.0, 0.5),
                        colours=((0.0, (0.8, 0.95, 1.0, 1.0)), (1.0, (0.3, 0.6, 1.0, 0.0))), world_space=True)
            e.particles('FrostFlakes', TEX['flash'], emitter=('sphere', 35), rate=70, stop=T, life=1.4, life_var=0.4,
                        speed=40, speed_var=25, radius=9, radius_var=3, grow=0.05, fade=0.5, rotation=(2.0, 2.0),
                        gravity=((0, 0, 1), -300), colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.5, 0.8, 1.0, 0.0))),
                        world_space=True)
        return e.encode(T, Travel.BOUND)


# ---------------------------------------------------------------- Impact
class Impact:
    SOURCE = SOURCES['Impact']
    DURATION = 1.4
    BOUND = (0.0, 0.0, 50.0, 260.0, -220.0, -220.0, -60.0, 220.0, 220.0, 240.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceBolt_Impact', source=None, **kw):
        T = Impact.DURATION
        e = _Effect(name, _particle_base(source)[0] if source else None)
        if e.base is not None:
            _no_emittance(e.nif)
        if kw.get('no_shards'):         # 06.11 Ice Arrow: no crystal star on the target
            real_add = e.add
            e.add = lambda n, *a, **k: None if n.startswith(('Shatter', 'Splinter')) else real_add(n, *a, **k)
        e.add('Flash', TEX['flash'], (QUAD, QUAD_UV, (0, -1, 0)), [(0.0, 30), (0.06, 80), (0.35, 50), (T, 50)],
              _still(T), [(0.0, 2.4), (0.08, 1.8), (0.35, 0.0), (T, 0.0)], billboard=True)
        e.add('Burst', TEX['flash'], (QUAD, QUAD_UV, (0, -1, 0)), [(0.0, 40), (0.15, 100), (T, 110)],
              _still(T), [(0.0, 0.6), (0.2, 0.25), (0.5, 0.0), (T, 0.0)], tint=(0.3, 0.6, 1.0), billboard=True)
        e.add('Ring', TEX['ring'], fx._annulus(), [(0.0, 20), (0.35, 150), (T, 170)], _still(T),
              [(0.0, 1.8), (0.3, 1.2), (0.9, 0.0), (T, 0.0)])
        # a crystal bursting: big shards standing out, then sinking away
        e.add('Shatter', TEX['shard'], _spikes(12, 1.0, 0.3, 21), [(0.0, 20), (0.08, 75), (0.6, 70), (0.95, 0.0), (T, 0.0)],
              _still(T), [(0.0, 2.0), (0.2, 1.2), (0.7, 1.0), (0.95, 0.0), (T, 0.0)], opaque=True)
        rng = np.random.default_rng(11)
        for k in range(12):                                  # splinters thrown out and falling
            ang = 2 * np.pi * (k + rng.uniform(-0.3, 0.3)) / 12
            up = rng.uniform(0.2, 1.0)
            sp = rng.uniform(180, 330)
            fly = rng.uniform(0.5, 0.8)
            path = [(fly * j / 6, (np.cos(ang) * sp * fly * j / 6, np.sin(ang) * sp * fly * j / 6,
                                   up * sp * fly * j / 6 - 420 * (fly * j / 6) ** 2)) for j in range(7)]
            path.append((T, path[-1][1]))
            size = rng.uniform(14, 26)
            e.add('Splinter%02d' % k, TEX['shard'], fx._shards(count=1, radius=0.0, seed=60 + k), [(0.0, size), (fly, size * 0.7),
                  (fly + 0.01, fx.MIN_SCALE), (T, fx.MIN_SCALE)], path,
                  [(0.0, 1.6), (fly, 0.8), (fly + 0.01, 0.0), (T, 0.0)], opaque=True)
        for k in range(5):                                   # frost puffs
            ang = 2 * np.pi * k / 5
            path = [(0.0, (0, 0, 0)), (0.6, (np.cos(ang) * 90, np.sin(ang) * 90, 40)), (T, (np.cos(ang) * 110, np.sin(ang) * 110, 70))]
            e.add('Puff%d' % k, TEX['haze'], (QUAD, QUAD_UV, (0, -1, 0)), [(0.0, 25), (0.6, 45), (T, 55)], path,
                  [(0.0, 0.35), (0.4, 0.2), (T, 0.0)], tint=(0.35, 0.65, 1.0), billboard=True)
        # 03.11: the bolt's trail dies with the projectile at the hit, so the frost lingers here instead: a cloud
        # settling where it struck and flakes thrown out and falling (emitted for 0.3 s, gone by the end)
        if os.environ.get('MAGE_NO_PSYS') is None:
            e.particles('FrostCloud', TEX['haze'], emitter=('sphere', 40), rate=120, stop=0.3, duration=T, life=1.0,
                        life_var=0.2, speed=60, speed_var=30, radius=55, radius_var=15, grow=0.15, fade=0.6,
                        rotation=(0.5, 0.5), drag=(0.6, 300.0),
                        colours=((0.0, (0.7, 0.9, 1.0, 1.0)), (0.5, (0.45, 0.7, 1.0, 0.7)), (1.0, (0.2, 0.4, 1.0, 0.0))),
                        world_space=True)
            e.particles('FrostShower', TEX['flash'], emitter=('sphere', 25), rate=150, stop=0.25, duration=T, life=1.0,
                        life_var=0.25, speed=220, speed_var=90, declination=0.0, declination_var=1.2, radius=10,
                        radius_var=3, grow=0.03, fade=0.4, rotation=(2.0, 2.0), gravity=((0, 0, 1), -600),
                        colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.5, 0.8, 1.0, 0.0))), world_space=True)
        return e.encode(T, Impact.BOUND)


# ---------------------------------------------------------------- Cast
class Cast:
    SOURCE = SOURCES['Cast']
    DURATION = 1.0
    BOUND = (0.0, 0.0, 0.0, 60.0, -50.0, -50.0, -50.0, 50.0, 50.0, 50.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceBolt_Cast', source=None, **kw):
        T = Cast.DURATION
        e = _Effect(name, _particle_base(source)[0] if source else None)
        if e.base is not None:
            _no_emittance(e.nif)
        e.add('Glow', TEX['flash'], (QUAD, QUAD_UV, (0, -1, 0)), [(0.0, 6), (0.3, 14), (0.45, 20), (0.6, 8), (T, 6)],
              _still(T), [(0.0, 0.5), (0.4, 1.8), (0.55, 2.2), (0.75, 0.0), (T, 0.0)], billboard=True)
        e.add('Frost', TEX['shard'], _spikes(8, 1.0, 0.35, 5), [(0.0, 4), (0.45, 16), (0.6, 0.0), (T, 0.0)], _still(T),
              [(0.0, 1.0), (0.45, 1.6), (0.6, 0.0), (T, 0.0)], opaque=True)
        rng = np.random.default_rng(2)
        for k in range(5):                                   # sparks drawn into the hand
            d = rng.normal(0, 1, 3); d = d / np.linalg.norm(d) * rng.uniform(35, 50)
            path = [(0.0, tuple(d)), (0.45, (0, 0, 0)), (T, (0, 0, 0))]
            e.add('Mote%d' % k, TEX['flash'], (QUAD, QUAD_UV, (0, -1, 0)), [(0.0, 6), (0.45, 3), (0.5, fx.MIN_SCALE), (T, fx.MIN_SCALE)],
                  path, [(0.0, 1.5), (0.45, 1.0), (0.5, 0.0), (T, 0.0)], billboard=True)
        return e.encode(T, Cast.BOUND)


# ---------------------------------------------------------------- the marks on a slowed / frozen enemy
class Slowed:
    """04.11: one puff of the mark on an enemy Ice Bolt slows (Div2Fix SlowFx14 starts one at its feet every 250 ms
    while it lasts; a free effect moved every frame was never seen): frost mist at the feet, flakes falling off the
    body. One burst (0.1 s), gone within 0.95 s (Div2Fix releases it then; the game replays a free effect after ~1 s)."""
    SOURCE = None
    DURATION = 0.95
    BOUND = (0.0, 0.0, 100.0, 140.0, -80.0, -80.0, 0.0, 80.0, 80.0, 220.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceBolt_Slowed', source=None, **kw):
        # 04.11 second try (at the feet only was "nonsense"): over the whole body, head to feet
        T = Slowed.DURATION
        e = _Effect(name)
        e.particles('Frost', TEX['haze'], emitter=('cylinder', 30, 170), rate=200, stop=0.1, duration=T, life=0.5,
                    life_var=0.1, speed=6, speed_var=4, radius=24, radius_var=6, grow=0.12, fade=0.3,
                    rotation=(0.5, 0.5), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.5, 0.8, 1.0, 0.0)), (0.3, (0.45, 0.75, 1.0, 0.6)), (1.0, (0.25, 0.5, 1.0, 0.0))))
        e.particles('Flakes', TEX['flash'], emitter=('cylinder', 35, 170), rate=120, stop=0.1, duration=T, life=0.55,
                    life_var=0.1, speed=0, speed_var=10, radius=6, radius_var=2, grow=0.1, fade=0.25, rotation=(2.0, 2.0),
                    gravity=((0, 0, 1), -160), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (1.0, 1.0, 1.0, 0.9)), (1.0, (0.5, 0.8, 1.0, 0.0))))
        return e.encode(T, Slowed.BOUND)


class Frozen:
    """04.11: one puff of the mark on a frozen enemy (Div2Fix FreezeFx14, as Slowed): bright frost round the body and
    a cold mist at the feet"""
    SOURCE = None
    DURATION = 0.95
    BOUND = (0.0, 0.0, 100.0, 150.0, -90.0, -90.0, 0.0, 90.0, 90.0, 230.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceBolt_Frozen', source=None, **kw):
        # 04.11 second try: a dense bright shell of ice over the whole body and sparkles in it
        T = Frozen.DURATION
        e = _Effect(name)
        e.particles('Shell', TEX['flash'], emitter=('cylinder', 32, 175), rate=400, stop=0.1, duration=T, life=0.5,
                    life_var=0.1, speed=3, speed_var=2, radius=22, radius_var=6, grow=0.1, fade=0.3,
                    rotation=(0.5, 0.5), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.7, 0.9, 1.0, 0.0)), (0.3, (0.75, 0.92, 1.0, 0.85)), (1.0, (0.4, 0.7, 1.0, 0.0))))
        e.particles('Haze', TEX['haze'], emitter=('cylinder', 30, 170), rate=200, stop=0.1, duration=T, life=0.5,
                    life_var=0.1, speed=4, speed_var=3, radius=30, radius_var=8, grow=0.12, fade=0.3,
                    rotation=(0.4, 0.4), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (0.55, 0.85, 1.0, 0.0)), (0.3, (0.5, 0.8, 1.0, 0.7)), (1.0, (0.25, 0.5, 1.0, 0.0))))
        e.particles('Sparkle', TEX['flash'], emitter=('cylinder', 35, 175), rate=150, stop=0.1, duration=T, life=0.4,
                    life_var=0.1, speed=0, speed_var=5, radius=5, radius_var=2, grow=0.05, fade=0.2,
                    rotation=(2.0, 2.0), translation=(0, 0, 95), world_space=True,
                    colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.6, 0.9, 1.0, 0.0))))
        return e.encode(T, Frozen.BOUND)


# ---------------------------------------------------------------- Trail
class Trail:
    """03.11: one puff of the bolt's trail. The bolt's own particles die with it at the hit (the engine drops the
    projectile and its effect), so Div2Fix (OwnTrail14) starts this free effect in the world where the bolt is, every
    few tens of ms while it flies: each puff lives on its own and melts away after the hit as well."""
    SOURCE = None
    DURATION = 0.95
    BOUND = (0.0, 0.0, 0.0, 120.0, -100.0, -100.0, -100.0, 100.0, 100.0, 100.0)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceBolt_Trail', source=None, **kw):
        T = Trail.DURATION
        e = _Effect(name)
        # 03.11 second try: one puff every 50 cm (Div2Fix interpolates), so fewer and bigger particles in each
        e.particles('Mist', TEX['haze'], emitter=('sphere', 10), rate=40, stop=0.05, duration=T, life=0.7,
                    life_var=0.12, speed=10, speed_var=8, radius=60, radius_var=15, grow=0.08, fade=0.45,
                    rotation=(0.6, 0.6),
                    colours=((0.0, (0.6, 0.85, 1.0, 0.8)), (0.4, (0.4, 0.65, 1.0, 0.55)), (1.0, (0.2, 0.4, 1.0, 0.0))),
                    world_space=True)
        e.particles('Glow', TEX['flash'], emitter=('sphere', 5), rate=20, stop=0.05, duration=T, life=0.4,
                    life_var=0.08, speed=3, speed_var=2, radius=35, radius_var=6, grow=0.03, fade=0.35,
                    rotation=(0.0, 0.5), colours=((0.0, (0.7, 0.9, 1.0, 0.9)), (1.0, (0.3, 0.6, 1.0, 0.0))),
                    world_space=True)
        e.particles('Flakes', TEX['flash'], emitter=('sphere', 20), rate=40, stop=0.05, duration=T, life=0.75,
                    life_var=0.12, speed=35, speed_var=20, radius=7, radius_var=3, grow=0.05, fade=0.5,
                    rotation=(2.0, 2.0), gravity=((0, 0, 1), -300),
                    colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.5, 0.8, 1.0, 0.0))), world_space=True)
        return e.encode(T, Trail.BOUND)


class ArrowCast:
    """06.11 Ice Arrow's cast: the game's Arrow_Poison_Cast (three rings falling to the feet; only its texture is green,
    build_mage recolours a copy: tint_textures) with small snowflakes added"""
    SOURCE = r'Win32\Effects\Arrow_Poison\Arrow_Poison_Cast.item'
    DURATION = 1.0416667461395264
    BOUND = (0, 0, 100, 320, -320, -320, -20, 320, 320, 320)
    textures = staticmethod(lambda: textures())

    @staticmethod
    def build(name='IceArrow_Cast', source=None, **kw):
        T = ArrowCast.DURATION
        nif = nif_write.Nif(source)
        for b in nif.blocks:
            if b.type == 'NiSourceTexture' and nif.strings[b['file']].lower().startswith('fx_bow_poison_b'):
                b['file'] = nif.string('fx_bow_ice_b.tga')
        e = _Effect(name, nif)
        _no_emittance(nif)
        e.particles('Snowflakes', TEX['flash'], emitter=('sphere', 70), rate=70, stop=0.6, duration=T, life=0.8,
                    life_var=0.2, speed=45, speed_var=25, radius=4, radius_var=1.5, grow=0.05, fade=0.5,
                    rotation=(2.0, 2.0), gravity=((0, 0, 1), -200),
                    colours=((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.55, 0.8, 1.0, 0.0))), world_space=True)
        return e.encode(T, ArrowCast.BOUND)


def main(argv):
    if len(argv) >= 2 and argv[1] == 'check':
        for c in (Travel, Impact, Cast, Trail):
            print(c.__name__, len(c.build()))
        return 0
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
