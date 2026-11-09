r"""Holy Smite's sound: MageExpansion\HolySmite.wav (Div2Fix OwnSound101), from assets/sounds/magic-boom.wav (the user's pick, 06.11): the sound starts with the cast, so silence
is put in front to bring its loudest moment onto the hit (HIT_AT = OwnDelay101); faded out, at most KEEP s after it.

  py -3 -I sfx_smite.py <out.wav>
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfx_chain                    # noqa: E402
import sfx_frostnova as base        # noqa: E402

RATE = base.RATE
RECORDING = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds', 'magic-boom.wav')
HIT_AT = 0.55
KEEP = 3.0
PEAK = 0.8


def best():
    x = sfx_chain._read_wav(RECORDING)
    win = int(0.02 * RATE)
    env = np.convolve(np.abs(x), np.ones(win) / win, 'same')
    peak = int(np.argmax(env))
    start = max(0, peak - int(HIT_AT * RATE))
    x = x[start:peak + int(KEEP * RATE)]
    lead = int(HIT_AT * RATE) - (peak - start)
    if lead > 0:
        x = np.concatenate([np.zeros(lead), x])
    tt = np.arange(len(x)) / RATE
    x = x * np.clip(tt / 0.01, 0, 1) * np.clip((tt[-1] - tt) / 0.5, 0, 1)
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
