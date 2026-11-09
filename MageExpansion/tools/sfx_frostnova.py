r"""Frost Nova's own sound, synthesised from nothing but code (no game or third-party sound is used).

  py -3 -I sfx_frostnova.py <out.wav>

Timed to the cast animation (anim_frostnova): Div2Fix starts it together with the animation.
  0 .. CAST_AT   a cold wind swells, crystalline shimmer gathers between the hands, a short inhale before the slam
  CAST_AT        the slam: a low thump, a sharp ice crack
  after          the blade wave: a volley of ice cracks per ring, a whoosh with it, a crunch as they sink;
                 ice crackling, splinters tinkling as they land, an airy frost tail
44.1 kHz, 16-bit mono PCM WAV.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_frostnova               # noqa: E402

RATE = 44100
CAST_AT = anim_frostnova.CAST_AT
LENGTH = CAST_AT + 2.2
PEAK = 0.55                         # of full scale: PlaySound ignores the game's volume, keep it below the game's mix


def _band(x, lo, hi):
    """band-pass by FFT (soft edges)"""
    f = np.fft.rfftfreq(len(x), 1 / RATE)
    g = np.clip((f - lo * 0.7) / (lo * 0.3 + 1e-9), 0, 1) * np.clip((hi * 1.3 - f) / (hi * 0.3), 0, 1)
    return np.fft.irfft(np.fft.rfft(x) * g, len(x))


def _env(t, points):
    ts, vs = zip(*points)
    return np.interp(t, ts, vs)


def synth(seed=7):
    rng = np.random.default_rng(seed)
    n = int(LENGTH * RATE)
    t = np.arange(n) / RATE
    C = CAST_AT
    out = np.zeros(n)
    after = np.clip(t - C, 0, None) * (t >= C)

    # wind: noise whose band rises while it swells, cut at the slam
    noise = rng.standard_normal(n)
    low, high = _band(noise, 900, 3500), _band(noise, 3500, 11000)     # 03.11: colder, no low rumble
    rise = np.clip(t / C, 0, 1)
    wind = (low * (1 - rise) + high * rise) * _env(t, [(0, 0), (0.25, 0.25), (0.6, 0.45), (C - 0.05, 0.8), (C, 0.0),
                                                       (LENGTH, 0)])
    out += 1.0 * wind

    # shimmer: airy sparkle gathering towards the slam (03.11: narrow bands of noise, not pure tones: the steady
    # sines rang like metal)
    for k in range(10):
        fr = rng.uniform(3000, 8000)
        start = rng.uniform(0.05, C - 0.2)
        ph = rng.uniform(0, 2 * np.pi)
        trem = 0.5 + 0.5 * np.sin(2 * np.pi * rng.uniform(5, 11) * t + ph)
        e = np.clip((t - start) / 0.15, 0, 1) * (t < C) * _env(t, [(0, 0.3), (C, 1.0), (LENGTH, 1.0)])
        out += 0.12 * _band(rng.standard_normal(n), fr * 0.9, fr * 1.1) * trem * e

    # inhale: a short rising noise right before the slam
    inhale = _band(rng.standard_normal(n), 2500, 9000) * _env(t, [(0, 0), (C - 0.18, 0), (C - 0.01, 1.0), (C, 0),
                                                                  (LENGTH, 0)])
    out += 0.35 * inhale

    # thump: a falling sine with a fast decay
    f0 = 160 * np.exp(-after / 0.08) + 70
    phase = 2 * np.pi * np.cumsum(f0) / RATE
    out += 0.3 * np.sin(phase) * np.exp(-after / 0.12) * (t >= C)

    # crack: a bright noise burst
    crack = _band(rng.standard_normal(n), 2500, 14000) * np.exp(-after / 0.035) * (t >= C)
    out += 0.9 * crack

    # crackling: clicks spreading across the ground, thinning out
    clicks = np.zeros(n)
    for k in range(260):
        at = C + rng.exponential(0.35)
        if at >= LENGTH - 0.01:
            continue
        i = int(at * RATE)
        clicks[i] += rng.uniform(0.3, 1.0) * np.exp(-(at - C) / 0.8) * rng.choice([-1, 1])
    out += 0.9 * _band(np.convolve(clicks, np.exp(-np.arange(80) / 12.0), 'same'), 1200, 9000)

    # tinkles: splinters landing (short high pings)
    for k in range(70):
        at = C + rng.uniform(0.15, 1.2)
        fr = rng.uniform(3000, 9500)
        d = np.clip(t - at, 0, None)
        out += 0.04 * np.sin(2 * np.pi * fr * d) * np.exp(-d / rng.uniform(0.006, 0.016)) * (t >= at) \
            * np.exp(-(at - C) / 0.9)

    # frost forming in the palms: sparse brittle ticks that thicken towards the slam (03.11)
    ticks = np.zeros(n)
    for k in range(120):
        at = C * np.sqrt(rng.uniform(0.05, 1.0))
        ticks[int(at * RATE)] += rng.uniform(0.2, 1.0) * (at / C) * rng.choice([-1, 1])
    out += 0.5 * _band(np.convolve(ticks, np.exp(-np.arange(40) / 6.0), 'same'), 4000, 14000)

    # the blade wave (03.11): the five rings of fx_frostnova burst out of the ground one after another as the shock
    # ring passes (t = CAST_AT + 0.55 * distance); each ring is a volley of ice cracks with glassy pings, bigger and
    # lower outwards, over a whoosh running with the wave; the blades sink back with a gritty crunch
    sweep = np.clip(after / 0.55, 0, 1)
    whoosh = (_band(rng.standard_normal(n), 900, 2500) * (1 - sweep) + _band(rng.standard_normal(n), 2500, 7000) * sweep)
    out += 0.35 * whoosh * np.sin(np.pi * np.clip(after / 0.6, 0, 1)) * (t >= C)
    crack_src = _band(rng.standard_normal(n), 2500, 11000)
    crunch_src = _band(rng.standard_normal(n), 250, 1800)
    for far, count, size in ((0.22, 7, 0.35), (0.40, 10, 0.5), (0.58, 14, 0.65), (0.76, 18, 0.85), (0.95, 26, 1.1)):
        t0 = C + 0.55 * far
        for j in range(count // 2 + 2):
            at = t0 + rng.uniform(-0.02, 0.03)
            i0 = int(at * RATE)
            d = np.clip(t - at, 0, None) * (t >= at)
            # the crack: a slice of bright noise, a few ms
            out += 0.5 * size / np.sqrt(count / 2) * crack_src * np.exp(-d / 0.012) * (t >= at)   # a volley, not a wall
            # the click of the blade: a very short glassy tick (03.11: long pings sounded metallic)
            fr = rng.uniform(3500, 7000) / (0.8 + 0.3 * size)
            out += 0.03 * size * np.sin(2 * np.pi * fr * d) * np.exp(-d / rng.uniform(0.008, 0.02)) * (t >= at)
            # body: ice breaking out of the ground, a dull crunch of low noise (no tone)
            out += 0.25 * size / np.sqrt(count / 2) * crunch_src * np.exp(-d / 0.035) * (t >= at)
        # sinking back: a gritty crunch
        t_end = t0 + 0.22 + 0.15 * far if far < 0.9 else C + 0.86
        crunch = np.zeros(n)
        for k in range(18 + count):
            at = t_end + rng.uniform(0, 0.25)
            if at < LENGTH - 0.01:
                crunch[int(at * RATE)] += rng.uniform(0.3, 1.0) * rng.choice([-1, 1])
        out += 0.35 * size * _band(np.convolve(crunch, np.exp(-np.arange(60) / 9.0), 'same'), 1500, 8000)

    # tail: an airy frost hiss
    out += 0.3 * _band(rng.standard_normal(n), 3000, 11000) * np.exp(-after / 0.7) * (t >= C) \
        * np.clip(after / 0.05, 0, 1)

    # fades at both ends, peak normalised
    out *= np.clip(t / 0.02, 0, 1) * np.clip((LENGTH - t) / 0.3, 0, 1)
    out *= PEAK / np.abs(out).max()
    return out


def wav(samples):
    pcm = (np.clip(samples, -1, 1) * 32767).astype('<i2').tobytes()
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16)
            + b'data' + struct.pack('<I', len(pcm)) + pcm)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    data = wav(synth())
    open(argv[1], 'wb').write(data)
    print('%s: %d bytes, %.2f s' % (argv[1], len(data), LENGTH))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
