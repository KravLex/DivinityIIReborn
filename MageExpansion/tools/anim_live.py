r"""05.11: the life of the game's own cast animations in ours. The game keeps its motion as compressed B-splines (NiBSplineCompTransformInterpolator, read here without
changing anything). Our casts take the body (legs, hips, spine, head) from the game's Skill_Cast_Target, re-timed so that its "Cast" falls on our release, and keep only
their own arms and hands on top, faded in and out. So the weight shift, breathing and foot work are the game animators', the gesture is ours.
"""
import struct

import numpy as np

import anim_frostnova as base

SOURCE = 'Skill_Cast_Target'
GAME_CAST = 0.57                    # its "Cast" text key
FADE_IN, FADE_OUT = 0.14, 0.28      # seconds: the game's arms blend into ours
OWN = ('Arm', 'Hand', 'Clavicle', 'Shoulder', 'Thumb', 'Index', 'Middle', 'Ring', 'Pinky', 'Finger')


def _spline(ctrl, u):
    """clamped uniform cubic B-spline, u in 0..1, ctrl n x m"""
    n = len(ctrl)
    x = u * (n - 3)
    k = min(int(x), n - 4)
    t = x - k
    knots = lambda i: min(max(i - 3, 0), n - 3)

    def N(i, d, s):
        if d == 0:
            return 1.0 if knots(i) <= s < knots(i + 1) or (s == n - 3 and knots(i + 1) == n - 3 and knots(i) < n - 3) else 0.0
        a = knots(i + d) - knots(i)
        b = knots(i + d + 1) - knots(i + 1)
        return (((s - knots(i)) / a * N(i, d - 1, s)) if a else 0.0) + (((knots(i + d + 1) - s) / b * N(i + 1, d - 1, s)) if b else 0.0)
    return sum(N(i, 3, x) * ctrl[i] for i in range(max(0, k), min(n, k + 4)))


def read_sequence(gk, name):
    """{bone: (quaternion at time t function, translation at time t function or None)}, stop"""
    seq = next(b for b in gk.blocks if b.type == 'NiControllerSequence' and gk.strings[b['name']] == name)
    out = {}
    for it in seq['blocks']['items']:
        ip = gk.blocks[it['interpolator']]
        if ip.type != 'NiBSplineCompTransformInterpolator':
            continue
        start, stop, sd, bd = struct.unpack_from('<ffii', ip.raw, 0)
        tr = np.array(struct.unpack_from('<3f', ip.raw, 16))
        q = np.array(struct.unpack_from('<4f', ip.raw, 28))
        th, rh, _ = struct.unpack_from('<III', ip.raw, 48)
        tb, tm, rb, rm = struct.unpack_from('<4f', ip.raw, 60)
        if sd < 0 or bd < 0:                # no spline: a bone that stands still in this sequence
            out[gk.strings[it['node']]] = (start, stop, q, tr, None, None)
            continue
        data = gk.blocks[sd].raw
        nf = struct.unpack_from('<I', data, 0)[0]
        ns = struct.unpack_from('<I', data, 4 + nf * 4)[0]
        shorts = np.frombuffer(data, '<i2', ns, 8 + nf * 4).astype(np.float64) / 32767.0
        npts = struct.unpack_from('<I', gk.blocks[bd].raw, 0)[0]
        rot = None if rh >= 0xFFFF else shorts[rh:rh + 4 * npts].reshape(npts, 4) * rm + rb
        pos = None if th >= 0xFFFF else shorts[th:th + 3 * npts].reshape(npts, 3) * tm + tb
        out[gk.strings[it['node']]] = (start, stop, q, tr, rot, pos)
    return out, seq['stop']


def live(real, gk, duration, cast):
    """base.sample with the body of the game's animation"""
    game, stop = read_sequence(gk, SOURCE)

    def warp(t):
        return t * GAME_CAST / cast if t <= cast else GAME_CAST + (t - cast) * (stop - GAME_CAST) / (duration - cast)

    def at(entry, g):
        start, end, q, tr, rot, pos = entry
        u = float(np.clip((g - start) / max(end - start, 1e-6), 0, 1 - 1e-6))
        r = q if rot is None else _spline(rot, u)
        return r / np.linalg.norm(r), (tr if pos is None else _spline(pos, u))

    def run(sk):
        own, hips, keyp = real(sk)
        times = [f / base.FPS for f in range(int(round(duration * base.FPS)) + 1)]
        out = {}
        for bn in sk.order:
            if bn not in game:
                if bn in own:
                    out[bn] = own[bn]
                continue
            mine = own.get(bn)
            arm = any(w in bn for w in OWN)         # the twist bones, not moved by ours, go to rest: else the game's twist wrings our arm
            rest_q = base.skel.m_to_quat(sk.bones[bn][2])
            keys = []
            for i, t in enumerate(times):
                q = at(game[bn], warp(t))[0]
                if arm:
                    w = min(1.0, t / FADE_IN, max(0.0, duration - t) / FADE_OUT)
                    q = base._slerp(q, np.array(mine[min(i, len(mine) - 1)][1]) if mine is not None else rest_q, w * w * (3 - 2 * w))
                keys.append((t, q))
            out[bn] = keys
        if 'Hips' in game and 'Hips' in sk.bones:
            hips = [(t, at(game['Hips'], warp(t))[1]) for t in times]
        return out, hips, keyp
    return run


def _qmul(a, b):
    w, x, y, z = a
    W, X, Y, Z = b
    return np.array((w * W - x * X - y * Y - z * Z, w * X + x * W + y * Z - z * Y, w * Y - x * Z + y * W + z * X, w * Z + x * Y - y * X + z * W))


def subtle(real, gk, duration, cast, weight=0.35, bones=('Spine1', 'Spine2', 'Neck', 'Head')):
    """for a cast whose pose is far from the game's (Frost Nova, 05.11: the game's whole body broke it): our pose stays, only a part of how the game's
    upper body moves away from its first frame is added on top; legs, hips and arms stay ours"""
    game, stop = read_sequence(gk, SOURCE)

    def warp(t):
        return t * GAME_CAST / cast if t <= cast else GAME_CAST + (t - cast) * (stop - GAME_CAST) / (duration - cast)

    def q_at(e, g):
        start, end, q, _, rot, _ = e
        r = q if rot is None else _spline(rot, float(np.clip((g - start) / max(end - start, 1e-6), 0, 1 - 1e-6)))
        return r / np.linalg.norm(r)

    def run(sk):
        own, hips, keyp = real(sk)
        times = [f / base.FPS for f in range(int(round(duration * base.FPS)) + 1)]
        one = np.array((1.0, 0.0, 0.0, 0.0))
        for bn in bones:
            if bn not in game or bn not in sk.bones:
                continue
            first = q_at(game[bn], 0.0)
            inv = first * np.array((1.0, -1.0, -1.0, -1.0))
            rest = base.skel.m_to_quat(sk.bones[bn][2])
            mine = own.get(bn)
            keys = []
            for i, t in enumerate(times):
                base_q = np.array(mine[min(i, len(mine) - 1)][1]) if mine is not None else rest
                fade = min(1.0, t / 0.2, max(0.0, duration - t) / 0.3)
                delta = base._slerp(one, _qmul(inv, q_at(game[bn], warp(t))), weight * fade)
                q = _qmul(base_q, delta)
                keys.append((t, q / np.linalg.norm(q)))
            own[bn] = keys
        return own, hips, keyp
    return run


def check(gk):
    """what the reader sees: for the log of the build; a wrong layout shows at once as quaternions off length 1"""
    game, stop = read_sequence(gk, SOURCE)
    moved, bad = [], 0
    for bn, e in game.items():
        if e[4] is None:
            continue
        n = np.linalg.norm(e[4], axis=1)
        bad += int((np.abs(n - 1) > 0.15).sum())
        moved.append((round(float(2 * np.degrees(np.arccos(np.clip(np.abs(e[4] @ e[4][0] / (n * n[0] + 1e-9)), 0, 1)).max())), 1), bn))
    return stop, len(game), bad, sorted(moved, reverse=True)[:12]
