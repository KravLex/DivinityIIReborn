r"""Gamebryo 20.3.0.9 (Divinity II) NIF reader/writer for the block types an effect (.item) of our own needs.

  py -3 -I nif_write.py roundtrip <file.item>...   parse every known block and write it again: must be byte equal
  py -3 -I nif_write.py dump <file.item>           blocks with their fields

Layouts follow the public NIF format description for 20.3 and are checked here against the game's own files: a block
type is "known" only when every block of that type in the test files is consumed exactly and written back byte for
byte. Unknown types are kept as raw bytes (enough for the round trip, never written by the effect builder).
Strings are indexes into the header string table (-1 = none); refs are block indexes (-1 = none).
"""
import struct
import sys

# field kinds: i (i32), I (u32), H (u16), B (u8), f (f32), s (string index i32), r (ref i32),
# 'v3' 3 f32, 'm33' 9 f32, ('n', kind) u32 count then items, ('nb', kind) u8 count ... (not used), callables
NIOBJECTNET = [('name', 's'), ('extra', ('n', 'r')), ('controller', 'r')]
NIAVOBJECT = NIOBJECTNET + [('flags', 'H'), ('translation', 'v3'), ('rotation', 'm33'), ('scale', 'f'),
                            ('properties', ('n', 'r')), ('collision', 'r')]
NINODE = NIAVOBJECT + [('children', ('n', 'r')), ('effects', ('n', 'r'))]
NIPROPERTY = NIOBJECTNET


def _key_group(kind):
    """u32 count, then (when count > 0) u32 interpolation and keys of time + value [+ tangents]."""
    width = {'f': 1, 'v3': 3, 'c4': 4, 'q': 4}[kind]

    def rd(r):
        n = r.u32()
        if not n:
            return {'interp': None, 'keys': []}
        it = r.u32()
        per = {1: width, 2: 3 * width, 3: width + 3, 5: width}[it] if kind != 'q' else {1: 4, 2: 4, 3: 7, 5: 4}[it]
        keys = [r.floats(1 + per) for _ in range(n)]
        return {'interp': it, 'keys': keys}

    def wr(w, v):
        w.u32(len(v['keys']))
        if v['keys']:
            w.u32(v['interp'])
            for k in v['keys']:
                w.floats(k)
    return rd, wr


def _key_bool():
    """u32 count, then (when count > 0) u32 interpolation and keys of f32 time + u8 value (bool keys have no tangents)"""
    def rd(r):
        n = r.u32()
        if not n:
            return {'interp': None, 'keys': []}
        it = r.u32()
        return {'interp': it, 'keys': [[r.f32(), r.u8()] for _ in range(n)]}

    def wr(w, v):
        w.u32(len(v['keys']))
        if v['keys']:
            w.u32(v['interp'])
            for t, x in v['keys']:
                w.f32(t); w.u8(x)
    return rd, wr


def _psys_data():
    """NiPSysData 20.3 (03.11): NiGeometryData as the game writes it (see _tri_shape_data), then NiParticlesData
    (radii, active count, sizes, rotations, rotation angles, rotation axes; each 'has' u8 + one value per particle) and
    NiPSysData (per particle: velocity v3, age, life span, last update f32, spawn generation u16, code u16; rotation
    speeds; added particles count + base u16)."""
    def rd(r):
        v = {'group': r.i32()}
        nv = r.u16(); v['keep'] = r.u8(); v['compress'] = r.u8()
        v['has_verts'] = r.u8()
        v['verts'] = [r.floats(3) for _ in range(nv)] if v['has_verts'] else []
        v['vflags'] = r.u16()
        v['has_normals'] = r.u8()
        v['normals'] = [r.floats(3) for _ in range(nv)] if v['has_normals'] else []
        v['tangents'] = [r.floats(3) for _ in range(2 * nv)] if v['has_normals'] and v['vflags'] & 0x1000 else []
        v['unk8'] = r.u8()
        v['centre'] = r.floats(3); v['radius'] = r.f32()
        v['bound_kind'] = r.u16()
        if v['bound_kind'] not in (0, 2):
            raise ValueError('bound kind %d' % v['bound_kind'])
        v['box'] = r.floats(6) if v['bound_kind'] == 2 else []
        v['has_colours'] = r.u8()
        v['colours'] = [r.floats(4) for _ in range(nv)] if v['has_colours'] else []
        v['uvs'] = [[r.floats(2) for _ in range(nv)] for _ in range(v['vflags'] & 0x3F)]
        v['consistency'] = r.u16()
        v['additional'] = r.i32()
        v['nv'] = nv
        v['has_radii'] = r.u8(); v['radii'] = r.floats(nv) if v['has_radii'] else []
        v['active'] = r.u16()
        v['has_sizes'] = r.u8(); v['sizes'] = r.floats(nv) if v['has_sizes'] else []
        v['has_rotations'] = r.u8(); v['rotations'] = [r.floats(4) for _ in range(nv)] if v['has_rotations'] else []
        v['has_angles'] = r.u8(); v['angles'] = r.floats(nv) if v['has_angles'] else []
        v['has_axes'] = r.u8(); v['axes'] = [r.floats(3) for _ in range(nv)] if v['has_axes'] else []
        v['info'] = [[r.floats(3), r.f32(), r.f32(), r.f32(), r.u16(), r.u16()] for _ in range(nv)]
        v['has_speeds'] = r.u8(); v['speeds'] = r.floats(nv) if v['has_speeds'] else []
        v['added'] = r.u16(); v['added_base'] = r.u16()
        return v

    def wr(w, v):
        nv = v['nv']
        w.i32(v['group']); w.u16(nv); w.u8(v['keep']); w.u8(v['compress'])
        w.u8(v['has_verts'])
        for p in v['verts']: w.floats(p)
        w.u16(v['vflags'])
        w.u8(v['has_normals'])
        for p in v['normals']: w.floats(p)
        for p in v['tangents']: w.floats(p)
        w.u8(v['unk8'])
        w.floats(v['centre']); w.f32(v['radius'])
        w.u16(v['bound_kind'])
        if v['bound_kind'] == 2: w.floats(v['box'])
        w.u8(v['has_colours'])
        for c in v['colours']: w.floats(c)
        for uvs in v['uvs']:
            for uv in uvs: w.floats(uv)
        w.u16(v['consistency']); w.i32(v['additional'])
        w.u8(v['has_radii']); w.floats(v['radii'])
        w.u16(v['active'])
        w.u8(v['has_sizes']); w.floats(v['sizes'])
        w.u8(v['has_rotations'])
        for q in v['rotations']: w.floats(q)
        w.u8(v['has_angles']); w.floats(v['angles'])
        w.u8(v['has_axes'])
        for x in v['axes']: w.floats(x)
        for vel, age, life, last, gen, code in v['info']:
            w.floats(vel); w.f32(age); w.f32(life); w.f32(last); w.u16(gen); w.u16(code)
        w.u8(v['has_speeds']); w.floats(v['speeds'])
        w.u16(v['added']); w.u16(v['added_base'])
    return rd, wr


def _transform_data():
    """NiKeyframeData: u32 rotation key count, rotation type, keys (quaternion: time + wxyz), translations, scales."""
    qrd, qwr = _key_group('q')
    vrd, vwr = _key_group('v3')
    frd, fwr = _key_group('f')

    def rd(r):
        n = r.u32()
        rot = {'n': n, 'type': None, 'keys': []}
        if n:
            rot['type'] = r.u32()
            if rot['type'] == 4:                      # XYZ: three float key groups instead of quaternion keys
                rot['xyz'] = [frd(r) for _ in range(3)]
            else:
                per = {1: 4, 2: 4, 3: 7, 5: 4}[rot['type']]
                rot['keys'] = [r.floats(1 + per) for _ in range(n)]
        return {'rot': rot, 'trans': vrd(r), 'scale': frd(r)}

    def wr(w, v):
        rot = v['rot']
        w.u32(rot['n'] if rot.get('xyz') else len(rot['keys']))
        if rot.get('xyz'):
            w.u32(4)
            for g in rot['xyz']:
                fwr(w, g)
        elif rot['keys']:
            w.u32(rot['type'])
            for k in rot['keys']:
                w.floats(k)
        vwr(w, v['trans'])
        fwr(w, v['scale'])
    return rd, wr


def _tri_shape_data():
    """NiTriShapeData 20.3: group id, vertex count, keep/compress flags, has vertices + vertices, vector flags
    (bit 12 = tangents/bitangents with normals; low 6 bits = UV set count), material crc u32 (20.3), has normals +
    normals [+ tangents + bitangents], centre + radius, has colours + colours, UV sets, consistency u16, additional
    data ref, triangle count u16, triangle points u32, has triangles + triangles, match group count u16 (+ groups)."""
    def rd(r):
        v = {'group': r.i32()}
        nv = r.u16(); v['keep'] = r.u8(); v['compress'] = r.u8()
        v['has_verts'] = r.u8()
        v['verts'] = [r.floats(3) for _ in range(nv)] if v['has_verts'] else []
        v['vflags'] = r.u16()
        v['has_normals'] = r.u8()
        v['normals'] = [r.floats(3) for _ in range(nv)] if v['has_normals'] else []
        v['tangents'] = []
        if v['has_normals'] and v['vflags'] & 0x1000:
            v['tangents'] = [r.floats(3) for _ in range(2 * nv)]
        v['unk8'] = r.u8()                               # 20.3 (game files): one byte before the bound
        v['centre'] = r.floats(3); v['radius'] = r.f32()
        v['bound_kind'] = r.u16()                        # 2 in the game's effects: + 6 f32 (an axis aligned box?)
        if v['bound_kind'] not in (0, 2):
            raise ValueError('bound kind %d' % v['bound_kind'])
        v['box'] = r.floats(6) if v['bound_kind'] == 2 else []
        v['has_colours'] = r.u8()
        v['colours'] = [r.floats(4) for _ in range(nv)] if v['has_colours'] else []
        v['uvs'] = [[r.floats(2) for _ in range(nv)] for _ in range(v['vflags'] & 0x3F)]
        v['consistency'] = r.u16()
        v['additional'] = r.i32()
        nt = r.u16(); v['tri_points'] = r.u32()
        v['has_tris'] = r.u8()
        v['tris'] = [r.u16s(3) for _ in range(nt)] if v['has_tris'] else []
        v['nt'] = nt
        ng = r.u16()
        v['match'] = [r.u16s(r.u16()) for _ in range(ng)]
        return v

    def wr(w, v):
        nv = len(v['verts']) if v['has_verts'] else len(v['normals'])
        w.i32(v['group']); w.u16(nv); w.u8(v['keep']); w.u8(v['compress'])
        w.u8(v['has_verts'])
        for p in v['verts']:
            w.floats(p)
        w.u16(v['vflags'])
        w.u8(v['has_normals'])
        for p in v['normals'] + v['tangents']:
            w.floats(p)
        w.u8(v['unk8'])
        w.floats(v['centre']); w.f32(v['radius'])
        w.u16(v['bound_kind'])
        if v['bound_kind'] == 2:
            w.floats(v['box'])
        w.u8(v['has_colours'])
        for c in v['colours']:
            w.floats(c)
        for uv in v['uvs']:
            for p in uv:
                w.floats(p)
        w.u16(v['consistency']); w.i32(v['additional'])
        w.u16(v.get('nt', len(v['tris']))); w.u32(v['tri_points'])
        w.u8(v['has_tris'])
        for t in v['tris']:
            w.u16s(t)
        w.u16(len(v['match']))
        for g in v['match']:
            w.u16(len(g)); w.u16s(g)
    return rd, wr


def _texturing():
    """NiTexturingProperty 20.3: flags u16, texture count u32, then per slot (base, dark, detail, gloss, glow, bump,
    normal, unknown2, decal0...): u8 has + TexDesc {source ref, flags u16, has transform u8 [+ translation 2f,
    scale 2f, rotation f, method u32, centre 2f]}; bump slot adds luma scale, offset, matrix 4f; shader maps none."""
    def texdesc(r):
        d = {'source': r.i32(), 'flags': r.u16(), 'has_tf': r.u8()}
        if d['has_tf']:
            d['tf'] = r.raw(4 * 5 + 4 + 8)
        return d

    def wtexdesc(w, d):
        w.i32(d['source']); w.u16(d['flags']); w.u8(d['has_tf'])
        if d['has_tf']:
            w.raw(d['tf'])

    def rd(r):
        v = {'flags': r.u16(), 'count': r.u32(), 'slots': []}
        for i in range(min(v['count'], 6) if v['count'] <= 6 else v['count']):
            has = r.u8()
            s = {'has': has}
            if has:
                s['desc'] = texdesc(r)
                if i == 5:
                    s['bump'] = r.floats(6)
            v['slots'].append(s)
            if len(v['slots']) == v['count']:
                break
        v['shader'] = r.u32()
        if v['shader']:
            raise ValueError('shader maps not supported')
        return v

    def wr(w, v):
        w.u16(v['flags']); w.u32(v['count'])
        for i, s in enumerate(v['slots']):
            w.u8(s['has'])
            if s['has']:
                wtexdesc(w, s['desc'])
                if i == 5:
                    w.floats(s['bump'])
        w.u32(v['shader'])
    return rd, wr


SPECS = {
    'NiNode': NINODE,
    'NiBillboardNode': NINODE + [('billboard_mode', 'H')],
    'NiTriShape': NIAVOBJECT + [('data', 'r'), ('skin', 'r'), ('materials', ('n', ('pair', 's', 'i'))),
                                ('active_material', 'i'), ('dirty', 'B')],
    'NiZBufferProperty': NIPROPERTY + [('flags', 'H')],
    'NiVertexColorProperty': NIPROPERTY + [('flags', 'H')],
    'NiAlphaProperty': NIPROPERTY + [('flags', 'H'), ('threshold', 'B')],
    'NiStencilProperty': NIPROPERTY + [('flags', 'H'), ('ref', 'I'), ('mask', 'I')],
    'NiMaterialProperty': NIPROPERTY + [('ambient', 'v3'), ('diffuse', 'v3'), ('specular', 'v3'), ('emissive', 'v3'),
                                        ('glossiness', 'f'), ('alpha', 'f')],
    'NiSourceTexture': NIOBJECTNET + [('external', 'B'), ('file', 's'), ('unknown_link', 'r'), ('pixel_layout', 'I'),
                                      ('mipmap', 'I'), ('alpha_format', 'I'), ('static', 'B'), ('direct', 'B'),
                                      ('persist', 'B')],
    'NiStringExtraData': [('name', 's'), ('value', 's')],
    'NiBooleanExtraData': [('name', 's'), ('value', 'B')],
    'NiFloatsExtraData': [('name', 's'), ('values', ('n', 'f'))],
    'NiTextKeyExtraData': [('name', 's'), ('keys', ('n', ('pair', 'f', 's')))],
    'NiTransformInterpolator': [('translation', 'v3'), ('rotation', 'q'), ('scale', 'f'), ('data', 'r')],
    'NiFloatInterpolator': [('value', 'f'), ('data', 'r')],
    'NiTransformData': [('keys', 'transform_data')],
    'NiFloatData': [('keys', 'key_f')],
    'NiPosData': [('keys', 'key_v3')],
    'NiPoint3Interpolator': [('value', 'v3'), ('data', 'r')],
    'NiMaterialColorController': [('next', 'r'), ('flags', 'H'), ('frequency', 'f'), ('phase', 'f'), ('start', 'f'),
                                  ('stop', 'f'), ('target', 'r'), ('interpolator', 'r'), ('color', 'H')],
    'NiTriShapeData': [('d', 'tri_shape_data')],
    'NiTexturingProperty': NIPROPERTY + [('t', 'texturing')],
    'NiAlphaController': [('next', 'r'), ('flags', 'H'), ('frequency', 'f'), ('phase', 'f'), ('start', 'f'),
                          ('stop', 'f'), ('target', 'r'), ('interpolator', 'r')],
    'NiTransformController': [('next', 'r'), ('flags', 'H'), ('frequency', 'f'), ('phase', 'f'), ('start', 'f'),
                              ('stop', 'f'), ('target', 'r'), ('interpolator', 'r')],
    # 20.3 as in the game's files: NiObjectNET head, controlled blocks of 7 i32 (no priority byte), one u32 at the end
    'NiControllerSequence': NIOBJECTNET + [('blocks', 'cblocks'), ('weight', 'f'),
                                           ('text_keys', 'r'), ('cycle', 'I'), ('frequency', 'f'), ('start', 'f'),
                                           ('stop', 'f'), ('manager', 'r'), ('accum_root', 's'), ('tail', 'I')],
}
# 03.11: the particle systems (as the game's 743 systems in 411 effects are written; base sizes checked on Human_Fireball)
TIMECTRL = [('next', 'r'), ('flags', 'H'), ('frequency', 'f'), ('phase', 'f'), ('start', 'f'), ('stop', 'f'), ('target', 'r')]
PSYSMOD = [('name', 's'), ('order', 'I'), ('target', 'r'), ('active', 'B')]
PSYSEMIT = PSYSMOD + [('speed', 'f'), ('speed_var', 'f'), ('declination', 'f'), ('declination_var', 'f'),
                      ('planar', 'f'), ('planar_var', 'f'), ('colour', 'c4'), ('radius', 'f'), ('radius_var', 'f'),
                      ('life', 'f'), ('life_var', 'f')]
SPECS.update({
    'NiParticleSystem': SPECS['NiTriShape'] + [('world_space', 'B'), ('modifiers', ('n', 'r'))],
    'NiPSysData': [('d', 'psys_data')],
    'NiPSysAgeDeathModifier': PSYSMOD + [('spawn_on_death', 'B'), ('spawn_modifier', 'r')],
    'NiPSysSpawnModifier': PSYSMOD + [('generations', 'H'), ('percent', 'f'), ('min', 'H'), ('max', 'H'),
                                      ('speed_var', 'f'), ('dir_var', 'f'), ('life', 'f'), ('life_var', 'f')],
    'NiPSysPositionModifier': PSYSMOD,
    'NiPSysBoundUpdateModifier': PSYSMOD + [('skip', 'H')],
    'NiPSysColorModifier': PSYSMOD + [('data', 'r')],
    'NiPSysGrowFadeModifier': PSYSMOD + [('grow', 'f'), ('grow_gen', 'H'), ('fade', 'f'), ('fade_gen', 'H')],
    'NiPSysRotationModifier': PSYSMOD + [('speed', 'f'), ('speed_var', 'f'), ('angle', 'f'), ('angle_var', 'f'),
                                         ('random_sign', 'B'), ('random_axis', 'B'), ('axis', 'v3')],
    'NiPSysGravityModifier': PSYSMOD + [('object', 'r'), ('axis', 'v3'), ('decay', 'f'), ('strength', 'f'),
                                        ('force', 'I'), ('turbulence', 'f'), ('turbulence_scale', 'f')],
    'NiPSysDragModifier': PSYSMOD + [('parent', 'r'), ('axis', 'v3'), ('percent', 'f'), ('range', 'f'),
                                     ('falloff', 'f')],
    'NiPSysBoxEmitter': PSYSEMIT + [('object', 'r'), ('width', 'f'), ('height', 'f'), ('depth', 'f')],
    'NiPSysSphereEmitter': PSYSEMIT + [('object', 'r'), ('sphere_radius', 'f')],
    'NiPSysCylinderEmitter': PSYSEMIT + [('object', 'r'), ('cyl_radius', 'f'), ('cyl_height', 'f')],
    'NiPSysMeshEmitter': PSYSEMIT + [('meshes', ('n', 'r')), ('velocity_type', 'I'), ('emission_type', 'I'),
                                     ('emission_axis', 'v3')],
    'NiPSysEmitterCtlr': TIMECTRL + [('interpolator', 'r'), ('modifier', 's'), ('visibility', 'r')],
    'NiPSysUpdateCtlr': TIMECTRL,
    'NiPSysResetOnLoopCtlr': TIMECTRL,
    'NiPSysModifierActiveCtlr': TIMECTRL + [('interpolator', 'r'), ('modifier', 's')],
    'NiPSysGravityStrengthCtlr': TIMECTRL + [('interpolator', 'r'), ('modifier', 's')],
    'NiPSysEmitterInitialRadiusCtlr': TIMECTRL + [('interpolator', 'r'), ('modifier', 's')],
    'NiPSysEmitterSpeedCtlr': TIMECTRL + [('interpolator', 'r'), ('modifier', 's')],
    'NiPSysEmitterLifeSpanCtlr': TIMECTRL + [('interpolator', 'r'), ('modifier', 's')],
    'NiBoolInterpolator': [('value', 'B'), ('data', 'r')],
    'NiBoolTimelineInterpolator': [('value', 'B'), ('data', 'r')],
    'NiBoolData': [('keys', 'key_bool')],
    'NiColorData': [('keys', 'key_c4')],
})
COMPLEX = {'transform_data': _transform_data(), 'key_f': _key_group('f'), 'key_v3': _key_group('v3'),
           'tri_shape_data': _tri_shape_data(),
           'key_bool': _key_bool(), 'key_c4': _key_group('c4'), 'psys_data': _psys_data(),
           'texturing': _texturing()}


class Reader:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def take(self, fmt):
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += struct.calcsize(fmt)
        return v

    def i32(self): return self.take('<i')[0]
    def u32(self): return self.take('<I')[0]
    def u16(self): return self.take('<H')[0]
    def u8(self): return self.take('<B')[0]
    def f32(self): return self.take('<f')[0]
    def floats(self, n): return list(self.take('<%df' % n))
    def u16s(self, n): return list(self.take('<%dH' % n))

    def raw(self, n):
        v = bytes(self.d[self.p:self.p + n]); self.p += n
        return v


class Writer:
    def __init__(self):
        self.b = bytearray()

    def i32(self, v): self.b += struct.pack('<i', v)
    def u32(self, v): self.b += struct.pack('<I', v)
    def u16(self, v): self.b += struct.pack('<H', v)
    def u8(self, v): self.b += struct.pack('<B', v)
    def f32(self, v): self.b += struct.pack('<f', v)
    def floats(self, v): self.b += struct.pack('<%df' % len(v), *v)
    def u16s(self, v): self.b += struct.pack('<%dH' % len(v), *v)
    def raw(self, v): self.b += v


def _read(r, kind):
    if isinstance(kind, tuple):
        if kind[0] == 'n':
            return [_read(r, kind[1]) for _ in range(r.u32())]
        if kind[0] == 'pair':
            return [_read(r, kind[1]), _read(r, kind[2])]
    if kind in ('i', 's', 'r'):
        return r.i32()
    if kind == 'I':
        return r.u32()
    if kind == 'H':
        return r.u16()
    if kind == 'B':
        return r.u8()
    if kind == 'f':
        return r.f32()
    if kind == 'v3':
        return r.floats(3)
    if kind in ('q', 'c4'):
        return r.floats(4)
    if kind == 'm33':
        return r.floats(9)
    if kind == 'cblocks':                      # u32 count, u32 array grow, then the blocks
        n = r.u32()
        return {'grow': r.u32(), 'items': [_read(r, 'cblock') for _ in range(n)]}
    if kind == 'cblock':
        return {'interpolator': r.i32(), 'controller': r.i32(), 'node': r.i32(), 'property': r.i32(),
                'ctrl_type': r.i32(), 'ctrl_id': r.i32(), 'interp_id': r.i32()}
    return COMPLEX[kind][0](r)


def _write(w, kind, v):
    if isinstance(kind, tuple):
        if kind[0] == 'n':
            w.u32(len(v))
            for x in v:
                _write(w, kind[1], x)
            return
        if kind[0] == 'pair':
            _write(w, kind[1], v[0]); _write(w, kind[2], v[1])
            return
    if kind in ('i', 's', 'r'):
        w.i32(v)
    elif kind == 'I':
        w.u32(v)
    elif kind == 'H':
        w.u16(v)
    elif kind == 'B':
        w.u8(v)
    elif kind == 'f':
        w.f32(v)
    elif kind in ('v3', 'q', 'm33', 'c4'):
        w.floats(v)
    elif kind == 'cblocks':
        w.u32(len(v['items'])); w.u32(v['grow'])
        for x in v['items']:
            _write(w, 'cblock', x)
    elif kind == 'cblock':
        for k in ('interpolator', 'controller', 'node', 'property', 'ctrl_type', 'ctrl_id', 'interp_id'):
            w.i32(v[k])
    else:
        COMPLEX[kind][1](w, v)


class Block:
    def __init__(self, type_, fields=None, raw=None):
        self.type, self.f, self.raw = type_, fields or {}, raw

    def __getitem__(self, k): return self.f[k]
    def __setitem__(self, k, v): self.f[k] = v

    def encode(self):
        if self.raw is not None:
            return self.raw
        w = Writer()
        for name, kind in SPECS[self.type]:
            _write(w, kind, self.f[name])
        return bytes(w.b)


class Nif:
    """header (version line, user version, endian) + strings + blocks; .roots = footer roots"""

    def __init__(self, data=None):
        self.head_line = b'Gamebryo File Format, Version 20.3.0.9\n'
        self.version, self.endian, self.user_version = 0x14030009, 1, 0x00030000
        self.strings, self.groups, self.blocks, self.roots = [], [], [], [0]
        if data is not None:
            self._parse(data)

    def _parse(self, d):
        nl = d.index(b'\n') + 1
        self.head_line = bytes(d[:nl])
        r = Reader(d, nl)
        self.version = r.u32(); self.endian = r.u8(); self.user_version = r.u32()
        n = r.u32()
        types = [r.raw(r.u32()).decode('latin1') for _ in range(r.u16())]
        tidx = [r.u16() for _ in range(n)]
        sizes = [r.u32() for _ in range(n)]
        ns = r.u32(); r.u32()
        self.strings = [r.raw(r.u32()).decode('latin1') for _ in range(ns)]
        self.groups = [r.u32() for _ in range(r.u32())]
        for i in range(n):
            t = types[tidx[i] & 0x7FFF]
            start = r.p
            if t in SPECS:
                br = Reader(d, start)
                f = {}
                for name, kind in SPECS[t]:
                    f[name] = _read(br, kind)
                if br.p != start + sizes[i]:
                    raise ValueError('block %d %s: read %d of %d bytes' % (i, t, br.p - start, sizes[i]))
                self.blocks.append(Block(t, f))
            else:
                self.blocks.append(Block(t, raw=bytes(d[start:start + sizes[i]])))
            r.p = start + sizes[i]
        self.roots = [r.i32() for _ in range(r.u32())]

    def string(self, text):
        """index of a header string, added when missing"""
        if text not in self.strings:
            self.strings.append(text)
        return self.strings.index(text)

    def add(self, type_, raw=None, **fields):
        self.blocks.append(Block(type_, fields, raw))
        return len(self.blocks) - 1

    def encode(self):
        w = Writer()
        w.raw(self.head_line)
        w.u32(self.version); w.u8(self.endian); w.u32(self.user_version)
        w.u32(len(self.blocks))
        types = []
        for b in self.blocks:
            if b.type not in types:
                types.append(b.type)
        w.u16(len(types))
        for t in types:
            w.u32(len(t)); w.raw(t.encode('latin1'))
        bodies = [b.encode() for b in self.blocks]
        for b in self.blocks:
            w.u16(types.index(b.type))
        for body in bodies:
            w.u32(len(body))
        w.u32(len(self.strings)); w.u32(max([len(s) for s in self.strings] + [0]))
        for s in self.strings:
            e = s.encode('latin1'); w.u32(len(e)); w.raw(e)
        w.u32(len(self.groups))
        for g in self.groups:
            w.u32(g)
        for body in bodies:
            w.raw(body)
        w.u32(len(self.roots))
        for x in self.roots:
            w.i32(x)
        return bytes(w.b)


def main(argv):
    if len(argv) >= 3 and argv[1] == 'roundtrip':
        bad = 0
        for path in argv[2:]:
            d = open(path, 'rb').read()
            try:
                n = Nif(d)
                out = n.encode()
                ok = out == d
            except Exception as e:
                ok, out = False, b''
                print('ERROR', path, e)
            known = sorted({b.type for b in n.blocks if b.raw is None}) if ok or out else []
            print('%-4s %s (%d blocks; parsed types: %s)' % ('OK' if ok else 'FAIL', path, len(n.blocks) if out else 0,
                                                             ', '.join(known)))
            bad += not ok
        return 1 if bad else 0
    if len(argv) == 3 and argv[1] == 'dump':
        n = Nif(open(argv[2], 'rb').read())
        print('user version %08X, strings %s' % (n.user_version, n.strings))
        for i, b in enumerate(n.blocks):
            print(i, b.type, 'RAW %d' % len(b.raw) if b.raw is not None else b.f)
        print('roots', n.roots)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
