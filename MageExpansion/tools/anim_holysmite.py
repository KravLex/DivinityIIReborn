r"""Holy Smite's own cast animation, on the machinery of anim_frostnova and anim_arcanepush: both open hands rise to the sky calling the light, the head goes back, then the
arms sweep down and forward at the target on the release (CAST_AT = Div2Fix OwnDelay101), with a kick back. Its own whole body with a part of the game's upper body (anim_live.subtle): on the game's whole body it looked like the usual cast (06.11).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_arcanepush as push      # noqa: E402
import anim_frostnova as base       # noqa: E402

SEQ_NAME = 'Skill_HolySmite'
DURATION = 1.3
CAST_AT = 0.55
CAST_KEY = False
POSES = [
    (0.00, 'inout', base.REST_ARMS, 0.0, 0.0, 0.0, 10.0),
    # the hands open at the sides, gathering
    (0.14, 'inout', {'arm': (0.55, -0.20, -0.80), 'fore': (0.45, -0.60, -0.65), 'hand': (0.35, -0.70, -0.60)}, 3.0, 5.0, 3.0, -10.0),
    # up to the sky, chest open, looking up
    (0.36, 'inout', {'arm': (0.42, -0.10, 0.90), 'fore': (0.22, -0.15, 0.96), 'hand': (0.12, -0.20, 0.97)}, -12.0, -10.0, 0.0, -30.0),
    (0.46, 'out', {'arm': (0.38, 0.05, 0.92), 'fore': (0.18, 0.05, 0.98), 'hand': (0.10, 0.05, 0.99)}, -16.0, -12.0, 2.0, -35.0),
    # the sweep down at the target, past the end
    (CAST_AT, 'in', {'arm': (0.20, -0.98, -0.05), 'fore': (0.10, -1.0, 0.08), 'hand': (0.06, -0.80, 0.60)}, 22.0, 4.0, 13.0, -35.0),
    (0.66, 'out', {'arm': (0.24, -0.86, -0.34), 'fore': (0.15, -0.94, -0.16), 'hand': (0.10, -0.86, 0.36)}, 8.0, -4.0, 8.0, -15.0),
    (0.82, 'inout', {'arm': (0.22, -0.90, -0.24), 'fore': (0.13, -0.97, -0.06), 'hand': (0.08, -0.80, 0.50)}, 13.0, 2.0, 10.0, -20.0),
    (1.03, 'inout', {'arm': (0.25, -0.35, -0.90), 'fore': (0.14, -0.55, -0.75), 'hand': (0.06, -0.45, -0.85)}, 4.0, 3.0, 3.0, 5.0),
    (DURATION, 'inout', base.REST_ARMS, 0.0, 0.0, 0.0, 10.0),
]


def build_kf(sk, bones, still=False):
    names = ('SEQ_NAME', 'DURATION', 'CAST_AT', 'POSES')
    saved = {n: getattr(base, n) for n in names}
    real = base.sample
    try:
        for n in names:
            setattr(base, n, globals()[n])
        base.sample = push._lagged(real)
        return base.build_kf(sk, bones, still=still)
    finally:
        base.sample = real
        for n, v in saved.items():
            setattr(base, n, v)
