r"""Particle systems from code for our effects (03.11): one call builds a whole Gamebryo 20.3 particle system the way the
game's 743 systems are written (nif_write parses them all byte for byte since 03.11).

    node = psys.add(nif, 'Frost', 'fx_icebolt_mist', emitter=('sphere', 30), rate=60, life=0.8, ...)
    # node: an NiNode to put among the effect's parts (its children: the NiParticleSystem)

What one system is in the game's files (Human_Fireball, 03.11):
  NiNode (the emitter object and the parent) -> NiParticleSystem: NiTriShape fields + world space + modifiers, properties
  material (diffuse 1, emissive 0), vertex colour (16: the colours of the colour modifier), alpha, texturing, z-buffer;
  its controller chain NiPSysEmitterCtlr (birth rate = an NiFloatInterpolator, emission on/off = NiBoolInterpolator +
  NiBoolData keys) -> NiPSysUpdateCtlr; both run on their own (not in the NiControllerSequence).
  NiPSysData sized to the most particles alive (its contents are a snapshot of the export: zeros do).
  Modifiers in order: age/death 0, emitter 1000, spawn 1000, grow/fade 3000, colour 3000, rotation 3000, gravity 4000,
  drag 4000, position 6000, bound update 7000. Units: centimetres (the effect's Scene Root scales by 0.01), seconds.
Colours: (age fraction 0..1, (r, g, b, a)) keys; sizes: radius in cm, grow/fade in seconds.
"""
import math

import nif_write

IDENT = [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0]
BLEND = {'add': 0x320D,          # SRC_ALPHA + ONE (glows; the game's 0x32ED with the destination set to ONE)
         'alpha': 0x32ED}        # SRC_ALPHA + INV_SRC_ALPHA (smoke, the game's)


def _modname(nif, sysname, what):
    return nif.string('%s:%s' % (sysname, what))


def add(nif, name, texture, *, emitter=('sphere', 20.0), rate=40.0, start=0.0, stop=1.0, max_count=None,
        life=1.0, life_var=0.2, speed=50.0, speed_var=20.0, declination=0.0, declination_var=math.pi, planar=0.0,
        planar_var=math.pi, radius=10.0, radius_var=3.0, colours=((0.0, (1, 1, 1, 1)), (1.0, (1, 1, 1, 0))),
        grow=0.1, fade=0.3, rotation=(0.0, 1.0), gravity=None, drag=None, world_space=False, blend='add',
        translation=(0, 0, 0), duration=None):
    """returns the index of the NiNode holding the system. emitter: ('sphere', r) | ('box', w, h, d) |
    ('cylinder', r, h); gravity: (axis v3, strength cm/s^2) or None; drag: (percent 0..1, range) or None;
    rotation: (speed rad/s, speed variation); duration: the stop of the update controller's range (default stop)"""
    T = duration if duration is not None else stop
    if max_count is None:
        max_count = int(math.ceil(rate * (life + life_var) * 1.2)) + 2
    max_count = max(2, min(max_count, 500))

    node = nif.add('NiNode', name=nif.string(name), extra=[], controller=-1, flags=0x310, translation=list(translation),
                   rotation=list(IDENT), scale=1.0, properties=[], collision=-1, children=[], effects=[])
    tex = nif.add('NiSourceTexture', name=-1, extra=[], controller=-1, external=1, file=nif.string(texture + '.tga'),
                  unknown_link=-1, pixel_layout=6, mipmap=1, alpha_format=3, static=1, direct=0, persist=0)
    slots = [{'has': 1, 'desc': {'source': tex, 'flags': 0x3200, 'has_tf': 0}}] + [{'has': 0}] * 8
    props = [
        nif.add('NiMaterialProperty', name=nif.string(name + '_Mat'), extra=[], controller=-1, ambient=[0, 0, 0],
                diffuse=[1, 1, 1], specular=[1, 1, 1], emissive=[0, 0, 0], glossiness=10.0, alpha=1.0),
        nif.add('NiVertexColorProperty', name=-1, extra=[], controller=-1, flags=16),
        nif.add('NiAlphaProperty', name=-1, extra=[], controller=-1, flags=BLEND[blend], threshold=0),
        nif.add('NiTexturingProperty', name=-1, extra=[], controller=-1,
                t={'flags': 4, 'count': 9, 'slots': slots, 'shader': 0}),
        nif.add('NiZBufferProperty', name=-1, extra=[], controller=-1, flags=13),
    ]
    n = max_count
    data = nif.add('NiPSysData', d={
        'group': 0, 'nv': n, 'keep': 0, 'compress': 0, 'has_verts': 1, 'verts': [[0.0, 0.0, 0.0]] * n, 'vflags': 0,
        'has_normals': 0, 'normals': [], 'tangents': [], 'unk8': 0, 'centre': [0.0, 0.0, 0.0], 'radius': 0.0,
        'bound_kind': 2, 'box': [0.0] * 6, 'has_colours': 1, 'colours': [[1.0, 1.0, 1.0, 1.0]] * n, 'uvs': [],
        'consistency': 0x8000, 'additional': -1,
        'has_radii': 1, 'radii': [float(radius)] * n, 'active': 0, 'has_sizes': 1, 'sizes': [1.0] * n,
        'has_rotations': 0, 'rotations': [], 'has_angles': 1, 'angles': [0.0] * n, 'has_axes': 0, 'axes': [],
        'info': [[[0.0, 0.0, 0.0], 0.0, 0.0, 0.0, 0, 0] for _ in range(n)], 'has_speeds': 1, 'speeds': [0.0] * n,
        'added': 0, 'added_base': 0})
    psys = len(nif.blocks)                               # its index (the modifiers point at it)
    mods = []

    def mod(type_, what, order, **f):
        i = nif.add(type_, name=_modname(nif, name, what), order=order, target=psys + 0, active=1, **f)
        mods.append(i)
        return i

    nif.add('NiParticleSystem', name=nif.string(name + '_PSys'), extra=[], controller=-1, flags=14,
            translation=[0.0, 0.0, 0.0], rotation=list(IDENT), scale=1.0, properties=props, collision=-1, data=data,
            skin=-1, materials=[], active_material=-1, dirty=1, world_space=1 if world_space else 0, modifiers=[])
    assert len(nif.blocks) - 1 == psys
    spawn = len(nif.blocks) + 2                          # added right after age/death and the emitter
    mod('NiPSysAgeDeathModifier', 'AgeDeath', 0, spawn_on_death=0, spawn_modifier=spawn)
    emit = dict(speed=float(speed), speed_var=float(speed_var), declination=float(declination),
                declination_var=float(declination_var), planar=float(planar), planar_var=float(planar_var),
                colour=[1.0, 1.0, 1.0, 1.0], radius=float(radius), radius_var=float(radius_var), life=float(life),
                life_var=float(life_var), object=node)
    kind = emitter[0]
    if kind == 'sphere':
        emitter_i = mod('NiPSysSphereEmitter', 'Emitter', 1000, sphere_radius=float(emitter[1]), **emit)
    elif kind == 'box':
        emitter_i = mod('NiPSysBoxEmitter', 'Emitter', 1000, width=float(emitter[1]), height=float(emitter[2]),
                        depth=float(emitter[3]), **emit)
    elif kind == 'cylinder':
        emitter_i = mod('NiPSysCylinderEmitter', 'Emitter', 1000, cyl_radius=float(emitter[1]),
                        cyl_height=float(emitter[2]), **emit)
    else:
        raise ValueError('emitter ' + kind)
    assert mod('NiPSysSpawnModifier', 'Spawn', 1000, generations=0, percent=1.0, min=1, max=1, speed_var=0.0,
               dir_var=0.0, life=float(life), life_var=float(life_var)) == spawn
    mod('NiPSysGrowFadeModifier', 'GrowFade', 3000, grow=float(grow), grow_gen=0, fade=float(fade), fade_gen=0)
    cdata = nif.add('NiColorData', keys={'interp': 1, 'keys': [[t] + [float(c) for c in rgba] for t, rgba in colours]})
    mod('NiPSysColorModifier', 'Colour', 3000, data=cdata)
    mod('NiPSysRotationModifier', 'Rotation', 3000, speed=float(rotation[0]), speed_var=float(rotation[1]),
        angle=0.0, angle_var=math.pi, random_sign=1, random_axis=1, axis=[1.0, 0.0, 0.0])
    if gravity:
        axis, strength = gravity
        mod('NiPSysGravityModifier', 'Gravity', 4000, object=node, axis=[float(a) for a in axis], decay=0.0,
            strength=float(strength), force=0, turbulence=0.0, turbulence_scale=1.0)
    if drag:
        pct, rng = drag
        mod('NiPSysDragModifier', 'Drag', 4000, parent=node, axis=[1.0, 0.0, 0.0], percent=float(pct),
            range=float(rng), falloff=float(rng))
    mod('NiPSysPositionModifier', 'Position', 6000)
    mod('NiPSysBoundUpdateModifier', 'Bound', 7000, skip=4)
    nif.blocks[psys]['modifiers'] = mods

    # the controllers: the emitter's birth rate and on/off window, then the update.
    # 04.11: driven by the effect's NiControllerSequence as 619 of the game's 738 emitters are (GenericCast_A): flags
    # 104 (manager controlled), blend interpolators on the controller, and two sequence items (node = the system,
    # NiPSysEmitterCtlr, id = the emitter's name, BirthRate = NiFloatInterpolator, EmitterActive = NiBoolInterpolator
    # on NiBoolData). Free running (03.11) the burst came late when the game played an effect again: its clock was not
    # the effect's. The encoders add nif.psys_items to their sequence.
    keys = ([[0.0, 0], [float(start), 1], [float(stop), 0]] if start > 0 else [[0.0, 1], [float(stop), 0]]) + [[float(T), 0]]
    vis_data = nif.add('NiBoolData', keys={'interp': 5, 'keys': keys})
    rate_s = nif.add('NiFloatInterpolator', value=float(rate), data=-1)
    vis_s = nif.add('NiBoolInterpolator', value=1, data=vis_data)
    rate_i = nif.add('NiBlendFloatInterpolator', raw=bytes.fromhex('010200000000ffff7fff'))
    vis_i = nif.add('NiBlendBoolInterpolator', raw=bytes.fromhex('01020000000002'))
    upd = nif.add('NiPSysUpdateCtlr', next=-1, flags=76, frequency=1.0, phase=0.0, start=0.0, stop=0.0, target=psys)
    ectl = nif.add('NiPSysEmitterCtlr', next=upd, flags=104, frequency=1.0, phase=0.0, start=0.0, stop=float(T),
                   target=psys, interpolator=rate_i, modifier=nif.blocks[emitter_i]['name'], visibility=vis_i)
    items = nif.__dict__.setdefault('psys_items', [])
    for interp, what in ((rate_s, 'BirthRate'), (vis_s, 'EmitterActive')):
        items.append({'interpolator': interp, 'controller': -1, 'node': nif.blocks[psys]['name'], 'property': -1,
                      'ctrl_type': nif.string('NiPSysEmitterCtlr'), 'ctrl_id': nif.blocks[emitter_i]['name'],
                      'interp_id': nif.string(what)})
    nif.blocks[psys]['controller'] = ectl
    nif.blocks[node]['children'] = [psys]
    return node
