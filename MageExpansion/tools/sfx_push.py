r"""Arcane Push's sound: MageExpansion\ArcanePush.wav (Div2Fix OwnSound54), from assets/sounds/magic-strike.wav (the user's
pick, 04.11). Its attack comes at 0.70 s; SKIP cuts the swell so that it falls on the release at 0.45 s after the cast
(anim_arcanepush.CAST_AT).

  py -3 -I sfx_push.py <out.wav>
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfx_chain                    # noqa: E402
import sfx_frostnova as base        # noqa: E402

RATE = base.RATE
RECORDING = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds', 'magic-strike.wav')
SKIP = 0.25
PEAK = 0.7


def best():
    x = sfx_chain._read_wav(RECORDING)[int(SKIP * RATE):]
    tt = np.arange(len(x)) / RATE
    x = x * np.clip(tt / 0.03, 0, 1) * np.clip((tt[-1] - tt) / 0.3, 0, 1)
    return x * (PEAK / np.abs(x).max())


def wav(samples):
    return base.wav(samples)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    open(argv[1], 'wb').write(wav(best()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
