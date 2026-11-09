r"""Arcane Push's own cast animation, on the machinery of anim_frostnova (its poses swapped for the build): the hands
draw in to the chest with a lean back, then both palms thrust forward with a step down into it (the release, CAST_AT =
Div2Fix OwnDelay54), a short recoil and back to rest.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_frostnova as base       # noqa: E402

SEQ_NAME = 'Skill_ArcanePush'
DURATION = 1.25
CAST_AT = 0.45
CAST_KEY = False
THRUST = {'arm': (0.20, -0.95, -0.15), 'fore': (0.12, -1.0, 0.05), 'hand': (0.10, -0.75, 0.65)}
# 05.11, the user: too wooden. A real wind-up with the hips sinking, a snap past the end of the thrust (overshoot), the push of the release carrying the
# body back in two smaller bounces, and everything not moving at once: see LAG
POSES = [
    (0.00, 'inout', base.REST_ARMS, 0.0, 0.0, 0.0, 10.0),
    # a small dip first
    (0.10, 'inout', {'arm': (0.28, 0.05, -0.95), 'fore': (0.05, -0.30, -0.90), 'hand': (0.0, -0.30, -0.90)}, 4.0, 4.0, 3.0, 25.0),
    # the hands drawn to the chest, leaning back and rising a little, fingers closing on the force
    (0.28, 'inout', {'arm': (0.48, 0.10, -0.85), 'fore': (-0.75, -0.50, 0.40), 'hand': (-0.55, -0.40, 0.72)}, -12.0, -8.0, 1.0, 55.0),
    (0.38, 'out', {'arm': (0.55, 0.22, -0.80), 'fore': (-0.70, -0.55, 0.45), 'hand': (-0.50, -0.45, 0.72)}, -18.0, -12.0, 5.0, 65.0),
    # the thrust: fast, past the end
    (CAST_AT, 'in', {'arm': (0.18, -1.0, -0.05), 'fore': (0.10, -1.0, 0.14), 'hand': (0.08, -0.65, 0.78)}, 24.0, 6.0, 15.0, -35.0),
    # the kick of the release pushes back
    (0.56, 'out', {'arm': (0.24, -0.88, -0.32), 'fore': (0.16, -0.95, -0.12), 'hand': (0.12, -0.85, 0.40)}, 9.0, -6.0, 9.0, -15.0),
    (0.72, 'inout', {'arm': (0.22, -0.92, -0.22), 'fore': (0.14, -0.98, -0.02), 'hand': (0.10, -0.78, 0.55)}, 14.0, 2.0, 11.0, -22.0),
    # the arms drop, hanging for a moment, and the body straightens
    (0.95, 'inout', {'arm': (0.25, -0.35, -0.90), 'fore': (0.14, -0.60, -0.70), 'hand': (0.08, -0.50, -0.80)}, 5.0, 4.0, 4.0, 5.0),
    (DURATION, 'inout', base.REST_ARMS, 0.0, 0.0, 0.0, 10.0),
]

def _lagged(sample):
    """sample with some bones later: the head follows the body, the right arm follows the left, so that nothing moves as one piece"""
    def run(sk):
        out, hips, keyp = sample(sk)
        for bn, keys in out.items():
            low = bn.lower()
            lag = 0.05 if 'head' in low else 0.03 if 'neck' in low else 0.0
            if ('right' in low or low.startswith('r_') or low.endswith('_r')) and ('arm' in low or 'hand' in low or 'clavicle' in low or 'shoulder' in low or 'finger' in low):
                lag = 0.03
            if lag:
                n = int(round(lag * base.FPS))
                out[bn] = [(keys[f][0], keys[max(0, f - n)][1]) for f in range(len(keys))]
        return out, hips, keyp
    return run


def build_kf(sk, bones, still=False):
    """anim_frostnova's builder with this skill's poses (its module globals are put back afterwards)"""
    names = ('SEQ_NAME', 'DURATION', 'CAST_AT', 'POSES')
    saved = {n: getattr(base, n) for n in names}
    real = base.sample
    try:
        for n in names:
            setattr(base, n, globals()[n])
        base.sample = _lagged(real)
        return base.build_kf(sk, bones, still=still)
    finally:
        base.sample = real
        for n, v in saved.items():
            setattr(base, n, v)
