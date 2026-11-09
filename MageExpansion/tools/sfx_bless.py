r"""Blessing's sound: MageExpansion\Blessing.wav (Div2Fix OwnSound102), from assets/sounds/healing-cast.wav (the user's pick, 06.11): faded in and out, at most KEEP s.

  py -3 -I sfx_bless.py <out.wav>
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfx_chain                    # noqa: E402
import sfx_frostnova as base        # noqa: E402

RATE = base.RATE
RECORDING = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds', 'healing-cast.wav')
KEEP = 4.0
PEAK = 0.7


def best():
    x = sfx_chain._read_wav(RECORDING)[:int(KEEP * RATE)]
    tt = np.arange(len(x)) / RATE
    x = x * np.clip(tt / 0.01, 0, 1) * np.clip((tt[-1] - tt) / 0.4, 0, 1)
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
