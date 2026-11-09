r"""Chain Lightning's sound, synthesized (nothing of the game's is used): MageExpansion\ChainLightning.wav.

  py -3 -I sfx_chain.py <out.wav>

Div2Fix plays it with the cast (OwnSound28, PlaySound: not 3D, not under the game's volume), so its timeline follows
the skill's: the charge while the hand rises, the first strike at STRIKE_AT (OwnDelay28), then one crack per leap every
LEAP s (OwnChain28), each a little quieter, and a dying electric hiss.
  charge - a rising electric hum (a buzzing saw with jittered pitch) and sparse crackles
  strike - a sharp snap: a click, a bright noise burst and a short low thump
  leaps  - a crack per leap: a burst of very short clicks (the arc) and a high zap
  tail   - band-limited hiss and a few last sparks
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfx_frostnova as base       # noqa: E402

RATE = base.RATE
STRIKE_AT = 0.45                    # s, Div2Fix OwnDelay28
LEAP = 0.14                         # s, Div2Fix OwnChain28
LEAPS = 3                           # cracks after the first strike (rank 11+: 4 strikes in all)
LENGTH = STRIKE_AT + LEAPS * LEAP + 1.1
PEAK = 0.55


def _crackle(rng, n, t0, dur, rate, gain):
    """sparse random clicks between t0 and t0+dur (rate per second), each a decaying high burst"""
    x = np.zeros(n)
    for _ in range(int(rate * dur)):
        at = t0 + rng.uniform(0, dur)
        i = int(at * RATE)
        if 0 <= i < n:
            x[i] += rng.uniform(0.4, 1.0) * rng.choice([-1, 1])
    return gain * base._band(np.convolve(x, np.exp(-np.arange(90) / 12.0), 'same'), 2500, 12000)


def _snap(rng, n, t0, gain, low=True):
    """a strike: click + bright noise burst (+ a short thump)"""
    t = np.arange(n) / RATE
    a = np.clip(t - t0, 0, None) * (t >= t0)
    burst = base._band(rng.standard_normal(n), 1800, 12000) * np.exp(-a / 0.05) * (t >= t0)
    click = np.zeros(n)
    i = int(t0 * RATE)
    if i < n:
        click[i:i + 40] = np.linspace(1, 0, min(40, n - i))
    out = 1.2 * burst + 0.8 * click
    if low:
        out += 0.9 * np.sin(2 * np.pi * (70 + 60 * np.exp(-a / 0.04)) * a) * np.exp(-a / 0.12) * (t >= t0)
    return gain * out


def synth(seed=11):
    rng = np.random.default_rng(seed)
    n = int(LENGTH * RATE)
    t = np.arange(n) / RATE
    out = np.zeros(n)

    # charge: a buzzing saw (jittered pitch) swelling towards the strike, band limited so it hums, not drones
    f = 110 + 25 * np.sin(2 * np.pi * 7 * t) + 15 * base._band(rng.standard_normal(n), 5, 40) * 30
    phase = np.cumsum(f) / RATE
    saw = 2 * (phase % 1) - 1
    hum = base._band(saw, 300, 4000) * base._env(t, [(0, 0), (0.1, 0.15), (STRIKE_AT - 0.02, 0.6), (STRIKE_AT + 0.02, 0.0),
                                                      (LENGTH, 0)])
    out += 0.6 * hum
    out += _crackle(rng, n, 0.05, STRIKE_AT - 0.05, 40, 0.5)

    # the first strike and one crack per leap, each quieter
    out += _snap(rng, n, STRIKE_AT, 1.0)
    out += _crackle(rng, n, STRIKE_AT, 0.12, 260, 0.9)
    for k in range(1, LEAPS + 1):
        at = STRIKE_AT + k * LEAP
        g = 0.85 ** k
        out += _snap(rng, n, at, 0.75 * g, low=False)
        out += _crackle(rng, n, at, 0.1, 240, 0.8 * g)
        # a high zap riding the crack
        a = np.clip(t - at, 0, None) * (t >= at)
        out += 0.35 * g * np.sin(2 * np.pi * (2400 - 1600 * np.clip(a / 0.08, 0, 1)) * a) * np.exp(-a / 0.05) * (t >= at)

    # tail: the electric hiss dying away, a few last sparks
    end = STRIKE_AT + LEAPS * LEAP
    a = np.clip(t - end, 0, None)
    out += 0.35 * base._band(rng.standard_normal(n), 3000, 11000) * np.exp(-a / 0.35) * (t >= STRIKE_AT) \
        * np.clip((t - STRIKE_AT) / 0.03, 0, 1)
    out += _crackle(rng, n, end, 0.7, 25, 0.4)

    out *= np.clip(t / 0.02, 0, 1) * np.clip((LENGTH - t) / 0.25, 0, 1)
    out *= PEAK / np.abs(out).max()
    return out


RECORDING = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds',
                         'electric-spark-zap.wav')


def _read_wav(path):
    """16-bit PCM WAV -> mono float at RATE (the user's recording, 03.11)"""
    b = open(path, 'rb').read()
    i, fmt, data = 12, None, None
    while i + 8 <= len(b):
        tag, ln = b[i:i + 4], struct.unpack_from('<I', b, i + 4)[0]
        if tag == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', b, i + 8)
        elif tag == b'data':
            data = b[i + 8:i + 8 + ln]
        i += 8 + ln + (ln & 1)
    if not fmt or fmt[0] != 1 or fmt[5] != 16 or data is None:
        raise SystemExit('%s: not 16-bit PCM' % path)
    x = np.frombuffer(data, '<i2').astype(np.float64) / 32768.0
    x = x.reshape(-1, fmt[1]).mean(1)
    if fmt[2] != RATE:
        x = np.interp(np.arange(int(len(x) * RATE / fmt[2])) * fmt[2] / RATE, np.arange(len(x)), x)
    return x


def from_recording(path=RECORDING):
    """the recording at the first strike, then a short slice of its onset per leap, each quieter"""
    zap = _read_wav(path)
    zap = zap[np.argmax(np.abs(zap) > 0.02 * np.abs(zap).max()):]           # from its onset
    n = int((STRIKE_AT + len(zap) / RATE + LEAPS * LEAP + 0.1) * RATE)
    out = np.zeros(n)
    i0 = int(STRIKE_AT * RATE)
    out[i0:i0 + len(zap)] += zap
    piece = zap[:int(0.35 * RATE)] * np.clip((0.35 - np.arange(int(0.35 * RATE)) / RATE) / 0.15, 0, 1)
    for k in range(1, LEAPS + 1):
        i = int((STRIKE_AT + k * LEAP) * RATE)
        out[i:i + len(piece)] += 0.75 ** k * piece[:max(0, n - i)]
    out *= np.clip((n / RATE - np.arange(n) / RATE) / 0.2, 0, 1)
    return out * (PEAK / np.abs(out).max())


def best():
    """the recording when it is there (assets/sounds), else the synthesized sound"""
    return from_recording() if os.path.isfile(RECORDING) else synth()


def wav(samples):
    return base.wav(samples)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    data = wav(best())
    open(argv[1], 'wb').write(data)
    print('%s: %d bytes, %.2f s' % (argv[1], len(data), LENGTH))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
