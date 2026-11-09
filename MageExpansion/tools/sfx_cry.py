r"""Battle Cry's shout, made from code (nothing recorded, no licence to track): a source-filter voice shouting an open "RAAH".

  py -3 -I sfx_cry.py <out_male.wav> <out_female.wav>

A sawtooth-like glottal source with a rising then falling pitch, vibrato and jitter, plus breath noise and a little
distortion for the strain, goes through vowel formants; a short room echo makes it carry.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfx_frostnova as base        # noqa: E402

RATE = base.RATE if hasattr(base, 'RATE') else 44100
PEAK = 0.55                         # PlaySound ignores the game's volume, as the other cast sounds
LENGTH = 1.15


def _resonator(x, freq, band):
    """a two-pole formant filter, freq may change per sample"""
    freq = np.broadcast_to(freq, x.shape)
    r = np.exp(-np.pi * band / RATE)
    a2 = -r * r
    a1 = 2 * r * np.cos(2 * np.pi * freq / RATE)
    g = 1 - r
    y = np.zeros_like(x)
    y1 = y2 = 0.0
    for i in range(len(x)):
        v = g * x[i] + a1[i] * y1 + a2 * y2
        y[i] = v
        y2, y1 = y1, v
    return y


def shout(pitch, formants, seed, rough_amount, drive, breath, voices):
    rng = np.random.default_rng(seed)
    n = int(LENGTH * RATE)
    t = np.arange(n) / RATE
    u = t / LENGTH
    # the pitch: a fast climb, held and straining upward, dropping at the end
    contour = np.interp(u, [0, 0.12, 0.65, 0.88, 1], [0.72, 1.08, 1.2, 0.95, 0.72])
    jitter = np.convolve(rng.normal(0, 1, n), np.ones(200) / 200, 'same') * 0.45
    source = np.zeros(n)
    for detune in voices:          # slightly detuned copies: less of a buzzer
        f0 = pitch * detune * contour * (1 + 0.018 * np.sin(2 * np.pi * 6.2 * t) + jitter * 0.1)
        phase = np.cumsum(f0) / RATE
        saw = 2 * (phase % 1) - 1
        # roughness: every other cycle a little different, what makes a voice sound strained
        source += saw * (1 + rough_amount * np.sign(np.sin(np.pi * phase)) * np.clip(u * 3, 0, 1))
    source = source / len(voices) + breath * rng.normal(0, 1, n)
    # the vowel opens from a growled "r" into "aa"
    open_ = np.clip(u / 0.13, 0, 1)
    voice = np.zeros(n)
    for (f_closed, f_open, band, gain) in formants:
        voice += gain * _resonator(source, f_closed + (f_open - f_closed) * open_, band)
    voice = np.tanh(voice / (np.abs(voice).max() + 1e-6) * drive)
    env = np.clip(t / 0.05, 0, 1) * np.clip((LENGTH - t) / 0.28, 0, 1) ** 1.4
    voice *= env
    # a room: three quiet echoes
    out = np.zeros(n + int(0.5 * RATE))
    out[:n] = voice
    for delay, gain in ((0.09, 0.28), (0.17, 0.17), (0.29, 0.1)):
        k = int(delay * RATE)
        out[k:k + n] += gain * voice
    tail = np.clip((len(out) / RATE - np.arange(len(out)) / RATE) / 0.1, 0, 1)
    out *= tail
    return out * (PEAK / np.abs(out).max())


MALE = dict(pitch=125.0, seed=5, rough_amount=0.22, drive=2.2, breath=0.2, voices=(1.0, 1.006, 0.995), formants=((450, 720, 90, 1.0), (1000, 1150, 110, 0.75), (2300, 2600, 170, 0.32), (3200, 3400, 220, 0.14)))
# 06.11 the first female voice (255 Hz, hard distortion) was heard as a man's: higher, cleaner, more air, higher formants
FEMALE = dict(pitch=370.0, seed=9, rough_amount=0.08, drive=1.3, breath=0.32, voices=(1.0, 1.005),
              formants=((700, 1050, 130, 1.0), (1500, 1750, 150, 0.75), (3000, 3400, 220, 0.4), (4200, 4600, 300, 0.2)))


def male():
    return shout(**MALE)


def female():
    return shout(**FEMALE)


def wav(samples):
    return base.wav(samples)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    open(argv[1], 'wb').write(wav(male()))
    open(argv[2], 'wb').write(wav(female()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
