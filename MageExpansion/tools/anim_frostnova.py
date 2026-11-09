r"""Frost Nova's own cast animation, made from code on the hero's skeleton (no game animation is copied).

  py -3 -I anim_frostnova.py frames <Skeleton.nif> <out.png>     stick figures of the key poses (front + side)

The mage cups the cold between the hands in front of the belly (head down, fingers curled), rises and throws both arms
up and back (fingers spread, chest open), then crouches hard and slams both palms down to the ground in front (the
"Cast" text key: the game releases the spell there), recoils a little on the impact, holds low and stands up again.
Poses (key poses below) are world directions of the upper arm, forearm and hand (so the male skeleton, whose rest pose
holds the arms out, and the female one, with the arms down, reach the same pose), bends of the spine and the head
about the hero's side axis, a drop of the hips (the legs keep the feet planted: two-bone IK, knees forward and a
little outward) and a finger curl. World axes of the skeleton: +X the hero's left, -Y forward, +Z up.
Each segment between key poses has its own easing: the windup eases in and out, the slam accelerates into the ground
(no slow-down before the hit), the recoil and the return ease out. Keys are sampled at 30 frames per second and written
as absolute local quaternions (plus the hips translation) in a NiControllerSequence like the game's own.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nif_write                    # noqa: E402
import skeleton as skel             # noqa: E402

SEQ_NAME = 'Skill_FrostNova'
DURATION = 2.2                      # 03.11: the effect lives only as long as this animation (Div2Fix)
CAST_AT = 0.86
FPS = 30
L, F, U = np.array([1.0, 0, 0]), np.array([0, -1.0, 0]), np.array([0, 0, 1.0])


def _n(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def _mirror(d):
    return np.array([-d[0], d[1], d[2]])


REST_ARMS = {'arm': (0.20, -0.05, -1.0), 'fore': (0.12, -0.25, -1.0), 'hand': (0.05, -0.2, -1.0)}
# key poses: time, easing of the segment that ENDS here, left limb directions (mirrored for the right),
# spine bend (degrees, + = forward), head bend, hips drop (cm), finger curl (degrees, - = spread)
POSES = [
    (0.00, 'inout', REST_ARMS, 0.0, 0.0, 0.0, 10.0),
    # cup the cold in front of the belly, looking at the hands
    (0.30, 'inout', {'arm': (0.30, -0.55, -0.80), 'fore': (-0.80, -0.55, 0.20), 'hand': (-0.70, -0.30, 0.65)},
     10.0, 18.0, 6.0, 45.0),
    # rise: arms up and back in a wide V, chest open, head up, fingers spread
    (0.60, 'inout', {'arm': (0.45, 0.15, 0.88), 'fore': (0.30, 0.20, 0.95), 'hand': (0.25, 0.25, 0.95)},
     -14.0, -18.0, 0.0, -25.0),
    # anticipation: a little further back
    (0.72, 'out', {'arm': (0.42, 0.30, 0.86), 'fore': (0.25, 0.40, 0.88), 'hand': (0.20, 0.50, 0.85)},
     -18.0, -22.0, 0.0, -30.0),
    # slam: deep crouch, palms flat on the ground in front
    (CAST_AT, 'in', {'arm': (0.25, -0.84, -0.50), 'fore': (0.10, -0.32, -0.94), 'hand': (0.05, -0.97, -0.24)},
     50.0, 24.0, 38.0, -30.0),
    # recoil of the impact
    (1.00, 'out', {'arm': (0.29, -0.68, -0.67), 'fore': (0.14, -0.46, -0.88), 'hand': (0.06, -0.82, -0.56)},
     33.0, 4.0, 24.0, -8.0),
    # (05.11) and it settles down again
    (1.09, 'inout', {'arm': (0.27, -0.74, -0.61), 'fore': (0.12, -0.41, -0.90), 'hand': (0.05, -0.88, -0.46)},
     38.0, 2.0, 29.0, -12.0),
    # hold low, looking up at the burst
    (1.18, 'inout', {'arm': (0.28, -0.72, -0.63), 'fore': (0.12, -0.42, -0.90), 'hand': (0.05, -0.85, -0.50)},
     36.0, -5.0, 28.0, -10.0),
    # stay low a moment watching the frost spread, then stand up slowly
    (1.50, 'inout', {'arm': (0.28, -0.70, -0.65), 'fore': (0.12, -0.42, -0.90), 'hand': (0.05, -0.85, -0.50)},
     33.0, -8.0, 26.0, -10.0),
    (DURATION, 'inout', REST_ARMS, 0.0, 0.0, 0.0, 10.0),
]
# 03.11: no "Cast" key. On it the game lit a bright disc on the ground around the caster (with every variant of the
# effect, even all dark); Div2Fix does the hit and the effect of the own skill itself (OwnDelay57 = CAST_AT)
CAST_KEY = False


def TEXT_KEYS(nif):
    return [[CAST_AT, nif.string('Cast')]] if CAST_KEY else []


LIMBS = {'arm': ('Arm', 'ForeArm'), 'fore': ('ForeArm', 'Hand'), 'hand': ('Hand', 'HandMiddle1')}
SPINE = (('Spine', 0.3), ('Spine1', 0.35), ('Spine2', 0.35))
HEAD = (('Neck', 0.4), ('Head', 0.6))
FINGERS = ('Index', 'Middle', 'Ring', 'Pinky')
EASE = {'inout': lambda t: t * t * (3 - 2 * t), 'in': lambda t: t * t * t, 'out': lambda t: 1 - (1 - t) ** 3}


def _rot_from_to(a, b):
    a, b = _n(a), _n(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-8:
        return np.eye(3) if c > 0 else skel.axis_angle(np.cross(a, [1, 0, 0]) if abs(a[0]) < 0.9 else np.cross(a, [0, 1, 0]), 180)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))


class _Pose:
    """local (R, t) overrides on the rest pose, world transforms recomputed on demand"""
    def __init__(self, sk):
        self.sk = sk
        self.local = {bn: (sk.bones[bn][2].copy(), None) for bn in sk.order}

    def w(self):
        return self.sk.world(self.local)

    def set_world_rot(self, bn, R_world, w=None):
        w = w or self.w()
        PR = w[self.sk.bones[bn][0]][0]
        self.local[bn] = (PR.T @ R_world, self.local[bn][1])

    def aim(self, bn, child, d):
        if bn not in self.sk.bones or child not in self.sk.bones:
            return
        w = self.w()
        self.set_world_rot(bn, _rot_from_to(w[child][1] - w[bn][1], d) @ w[bn][0], w)

    def bend(self, bones, deg, axis=L):
        for bn, share in bones:
            if bn in self.sk.bones:
                w = self.w()
                self.set_world_rot(bn, skel.axis_angle(axis, deg * share) @ w[bn][0], w)


def _leg_ik(p, side, feet):
    """bend thigh and calf so the foot stays where it was in the rest pose; the knee goes forward and a bit out"""
    up, low, foot = side + 'UpLeg', side + 'Leg', side + 'Foot'
    if not all(b in p.sk.bones for b in (up, low, foot)):
        return
    w = p.w()
    hip, target = w[up][1], feet[side]
    a = np.linalg.norm(w[low][1] - w[up][1]); b = np.linalg.norm(w[foot][1] - w[low][1])
    d = target - hip; dist = min(np.linalg.norm(d), a + b - 1e-3)
    dn = _n(d)
    bend_dir = _n(F + (0.25 if side == 'Left' else -0.25) * L)
    bend_dir = _n(bend_dir - np.dot(bend_dir, dn) * dn)
    # knee position from the triangle hip-knee-foot
    x = (a * a - b * b + dist * dist) / (2 * dist)
    h = np.sqrt(max(a * a - x * x, 0.0))
    knee = hip + dn * x + bend_dir * h
    p.aim(up, low, knee - hip)
    p.aim(low, foot, target - p.w()[low][1])
    # the foot keeps its rest orientation in the world
    p.set_world_rot(foot, feet[side + 'R'])


def _curl(p, side, deg):
    """curl (+) or spread (-) the four fingers: each joint turns about the palm's side axis, the sign chosen so a
    positive curl brings the finger tip towards the wrist"""
    hand = side + 'Hand'
    if hand not in p.sk.bones:
        return
    for fname in FINGERS:
        for j in (1, 2, 3):
            bn = '%sHand%s%d' % (side, fname, j)
            if bn not in p.sk.bones:
                continue
            w = p.w()
            i1, p1 = w.get(side + 'HandIndex1'), w.get(side + 'HandPinky1')
            if i1 is None or p1 is None:
                return
            axis = _n(i1[1] - p1[1])
            tip_bn = '%sHand%s%d' % (side, fname, 3)
            share = deg / 3.0
            best = None
            for s in (1, -1):
                trial = dict(p.local)
                R = skel.axis_angle(axis, s * share) @ w[bn][0]
                PR = w[p.sk.bones[bn][0]][0]
                trial[bn] = (PR.T @ R, p.local[bn][1])
                tw = p.sk.world(trial)
                tip = tw[tip_bn][1] + (tw[tip_bn][0] @ np.array([3.0, 0, 0]) if j == 3 else 0)
                dist = np.linalg.norm(tip - tw[hand][1])
                if best is None or (dist < best[0]) == (deg > 0):
                    best = (dist, trial[bn])
            p.local[bn] = best[1]


def pose_locals(sk, limbs, spine, head, drop, curl):
    """local (R, t) of every bone for one pose"""
    p = _Pose(sk)
    rest_w = sk.world()
    feet = {}
    for side in ('Left', 'Right'):
        if side + 'Foot' in sk.bones:
            feet[side] = rest_w[side + 'Foot'][1]
            feet[side + 'R'] = rest_w[side + 'Foot'][0]
    if 'Hips' in sk.bones and drop:
        R, _ = p.local['Hips']
        p.local['Hips'] = (R, sk.bones['Hips'][1] - U * drop)
    for side in ('Left', 'Right'):
        _leg_ik(p, side, feet)
    p.bend(SPINE, spine)
    p.bend(HEAD, head)
    for side, sgn in (('Left', 1), ('Right', -1)):
        for limb in ('arm', 'fore', 'hand'):
            d = np.array(limbs[limb], float)
            p.aim(side + LIMBS[limb][0], side + LIMBS[limb][1], _mirror(d) if sgn < 0 else d)
        _curl(p, side, curl)
    return p.local


def _slerp(q0, q1, t):
    q0, q1 = np.asarray(q0), np.asarray(q1)
    d = float(np.dot(q0, q1))
    if d < 0:
        q1, d = -q1, -d
    if d > 0.9995:
        q = q0 + t * (q1 - q0)
        return q / np.linalg.norm(q)
    th = np.arccos(d)
    return (np.sin((1 - t) * th) * q0 + np.sin(t * th) * q1) / np.sin(th)


def sample(sk):
    """{bone: [(time, quaternion)]} at FPS for the bones that turn, [(time, hips translation)], key poses"""
    keyp = []
    for t, ease, limbs, spine, head, drop, curl in POSES:
        loc = pose_locals(sk, limbs, spine, head, drop, curl)
        keyp.append((t, ease, {bn: skel.m_to_quat(R) for bn, (R, _) in loc.items()},
                     loc['Hips'][1] if 'Hips' in loc and loc['Hips'][1] is not None else None))
    moving = [bn for bn in sk.order if any(np.abs(np.abs(np.dot(keyp[0][2][bn], kq[bn])) - 1) > 1e-6 for _, _, kq, _ in keyp)]
    frames = int(round(DURATION * FPS))
    out = {bn: [] for bn in moving}
    hips = []
    t_rest = sk.bones['Hips'][1] if 'Hips' in sk.bones else None
    for f in range(frames + 1):
        t = f / FPS
        k = max(i for i in range(len(keyp)) if keyp[i][0] <= t + 1e-9)
        k = min(k, len(keyp) - 2)
        (t0, _, q0, h0), (t1, ease, q1, h1) = keyp[k], keyp[k + 1]
        u = EASE[ease](np.clip((t - t0) / (t1 - t0), 0, 1))
        for bn in moving:
            out[bn].append((t, _slerp(q0[bn], q1[bn], u)))
        if t_rest is not None:
            a = h0 if h0 is not None else t_rest
            b = h1 if h1 is not None else t_rest
            hips.append((t, a + (b - a) * u))
    return out, hips, keyp


def build_kf(sk, bone_names, still=False):
    """a .kf with one NiControllerSequence: every bone of bone_names (the hero's animated bones), moving ones keyed;
    still: no keys at all, every bone at rest (test: does the registration work without our key data)"""
    keys, hips, _ = sample(sk)
    if still:
        keys, hips = {}, []
    nif = nif_write.Nif()
    seq = nif.add('NiControllerSequence', name=nif.string(SEQ_NAME), extra=[], controller=-1, blocks=None, weight=1.0,
                  text_keys=-1, cycle=2, frequency=1.0, start=0.0, stop=DURATION, manager=-1,
                  accum_root=nif.string('Reference'), tail=64)
    items = []
    tc = nif.string('NiTransformController')
    for bn in bone_names:
        rest = sk.bones.get(bn)
        if rest is None:
            continue
        t_rest, R_rest = rest[1], rest[2]
        trans = {'interp': None, 'keys': []}
        if bn == 'Hips' and hips:
            trans = {'interp': 1, 'keys': [[t] + list(v) for t, v in hips]}
        if bn in keys or trans['keys']:
            rot = ({'n': len(keys[bn]), 'type': 1, 'keys': [[t] + list(q) for t, q in keys[bn]]} if bn in keys else
                   {'n': 1, 'type': 1, 'keys': [[0.0] + list(skel.m_to_quat(R_rest))]})
            data = nif.add('NiTransformData', keys={'rot': rot, 'trans': trans,
                                                    'scale': {'interp': None, 'keys': []}})
            ip = nif.add('NiTransformInterpolator', translation=list(t_rest), rotation=list(rot['keys'][0][1:]),
                         scale=1.0, data=data)
        else:
            ip = nif.add('NiTransformInterpolator', translation=list(t_rest), rotation=list(skel.m_to_quat(R_rest)),
                         scale=1.0, data=-1)
        items.append({'interpolator': ip, 'controller': -1, 'node': nif.string(bn), 'property': -1, 'ctrl_type': tc,
                      'ctrl_id': -1, 'interp_id': -1})
    tk = nif.add('NiTextKeyExtraData', name=-1, keys=[[0.0, nif.string('start')]] + TEXT_KEYS(nif) +
                                                     [[DURATION, nif.string('end')]])
    nif.blocks[seq]['blocks'] = {'grow': 1, 'items': items}
    nif.blocks[seq]['text_keys'] = tk
    nif.roots = [seq]
    return nif.encode(), sorted(keys)


def main(argv):
    if len(argv) == 4 and argv[1] == 'frames':
        sk = skel.Skeleton(open(argv[2], 'rb').read())
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                        'tools', 'reborn'))
        from fx_select import sheet
        tiles = []
        for t, _, limbs, spine, head, drop, curl in POSES[:-1]:
            loc = pose_locals(sk, limbs, spine, head, drop, curl)
            w = sk.world(loc)
            tmp = argv[3] + '.tmp.png'
            skel.picture(sk, w, tmp, size=256)
            tiles.append(('t=%.2f' % t, _png_rgba(tmp)))
            os.remove(tmp)
        sheet(argv[3], tiles, tile=512, cols=4)
        print('frames ->', argv[3])
        return 0
    print(__doc__)
    return 2


def _png_rgba(path):
    """read back a PNG written by fx_select.sheet (8-bit RGBA, no interlace)"""
    import zlib
    d = open(path, 'rb').read()
    p, w, h, idat = 8, 0, 0, b''
    while p < len(d):
        n, typ = struct.unpack('>I4s', d[p:p + 8])
        body = d[p + 8:p + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        p += 12 + n
    raw = zlib.decompress(idat)
    ch = 4 if ctype == 6 else 3
    stride = w * ch
    img = np.zeros((h, w, ch), np.uint8)
    prev = np.zeros(stride, np.int32)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = np.frombuffer(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)], np.uint8).astype(np.int32)
        out = np.zeros(stride, np.int32)
        for x in range(stride):
            a = out[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 0:
                v = line[x]
            elif f == 1:
                v = line[x] + a
            elif f == 2:
                v = line[x] + b
            elif f == 3:
                v = line[x] + (a + b) // 2
            else:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                v = line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)
            out[x] = v & 255
        img[y] = out.reshape(w, ch)
        prev = out
    if ch == 3:
        img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], -1)
    return img


if __name__ == '__main__':
    sys.exit(main(sys.argv))
