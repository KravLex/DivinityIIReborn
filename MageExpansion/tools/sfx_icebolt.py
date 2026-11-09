r"""Ice Bolt's cast sound, synthesized: MageExpansion\IceBolt.wav (Div2Fix CastWav14 plays it in place of the Fireball's
cast sound, 0x95E8C4).

  py -3 -I sfx_icebolt.py <out.wav>

03.11, second try (the first, made of noise bands, sounded electronic): every crack of ice is modal - a click exciting
a few damped resonances, as glass or ice ring - and the whole sound gets a short room reverb.
  gather  - cold air drawn in: pink noise in a band sweeping up, with sparse small cracks growing denser
  throw   - at THROW: a low thump, a burst of fracture grains (three overlapping clusters) and an airy whoosh
            sweeping down as the bolt leaves
  sparkle - fine high cracks dying away
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfx_frostnova as base       # noqa: E402

RATE = base.RATE
THROW = 0.30                        # s: the bolt leaves the hand
LENGTH = 1.3
PEAK = 0.55


def _pink(n, rng):
    """pink-ish noise (1/f) by FFT shaping"""
    x = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / RATE)
    x[1:] /= np.sqrt(f[1:])
    x[0] = 0
    y = np.fft.irfft(x, n)
    return y / np.abs(y).max()


def _sweep_band(x, t, f0, f1, t0, t1, width=0.6):
    """band-pass whose centre moves from f0 to f1 between t0 and t1 (block-wise FFT, overlap-add)"""
    out = np.zeros_like(x)
    hop, size = 1024, 2048
    win = np.hanning(size)
    for s in range(0, len(x) - size, hop):
        tc = (s + size / 2) / RATE
        k = np.clip((tc - t0) / max(t1 - t0, 1e-6), 0, 1)
        fc = f0 * (f1 / f0) ** k
        seg = np.fft.rfft(x[s:s + size] * win)
        fr = np.fft.rfftfreq(size, 1 / RATE)
        g = np.exp(-(np.log(np.maximum(fr, 1) / fc) / width) ** 2)
        out[s:s + size] += np.fft.irfft(seg * g, size) * win
    return out


def _grain(rng, high=False):
    """one crack of ice: a click into 3-4 damped resonances"""
    dur = 0.03 if high else 0.05
    n = int(dur * RATE)
    tt = np.arange(n) / RATE
    g = np.zeros(n)
    for _ in range(rng.integers(3, 5)):
        f = rng.uniform(5500, 11000) if high else rng.uniform(1600, 7500)
        d = rng.uniform(0.002, 0.006) if high else rng.uniform(0.003, 0.014)
        g += rng.uniform(0.4, 1.0) * np.sin(2 * np.pi * f * tt + rng.uniform(0, 6.28)) * np.exp(-tt / d)
    click = np.zeros(n)
    click[:12] = rng.standard_normal(12) * np.linspace(1, 0, 12)
    return g + 0.6 * click


def _scatter(n, rng, times, gain, high=False):
    out = np.zeros(n)
    for at, a in zip(times, gain):
        g = _grain(rng, high) * a
        i = int(at * RATE)
        if i < n:
            out[i:i + len(g)] += g[:n - i]
    return out


def _reverb(x, rng, decay=0.28, wet=0.28):
    n = int(decay * 5 * RATE)
    tt = np.arange(n) / RATE
    ir = rng.standard_normal(n) * np.exp(-tt / decay)
    ir = base._band(ir, 300, 9000)
    ir[:int(0.012 * RATE)] = 0                         # pre-delay
    ir /= np.sqrt((ir ** 2).sum())
    y = np.fft.irfft(np.fft.rfft(x, len(x) + n) * np.fft.rfft(ir, len(x) + n), len(x) + n)[:len(x)]
    return (1 - wet) * x + wet * y * 3.0


def synth(seed=31):
    rng = np.random.default_rng(seed)
    n = int(LENGTH * RATE)
    t = np.arange(n) / RATE
    out = np.zeros(n)

    # gather: cold air drawn in, the band rising; cracks getting denser towards the throw
    air = _sweep_band(_pink(n, rng), t, 900, 4500, 0.0, THROW, 0.5)
    out += 0.55 * air * base._env(t, [(0, 0), (0.06, 0.25), (THROW - 0.03, 1.0), (THROW + 0.02, 0.15), (THROW + 0.2, 0.0),
                                       (LENGTH, 0)])
    k = np.linspace(0, 1, 26) ** 0.6
    times = 0.04 + (THROW - 0.06) * k + rng.normal(0, 0.006, 26)
    out += 0.35 * _scatter(n, rng, times, 0.3 + 0.7 * k)

    # throw: thump, three overlapping fracture clusters, a whoosh falling away
    a = np.clip(t - THROW, 0, None) * (t >= THROW)
    out += 0.7 * np.sin(2 * np.pi * (60 + 90 * np.exp(-a / 0.03)) * a) * np.exp(-a / 0.07) * (t >= THROW)
    for c, (dt, cnt, g) in enumerate(((0.0, 40, 1.0), (0.045, 28, 0.7), (0.1, 18, 0.45))):
        tm = THROW + dt + np.abs(rng.normal(0, 0.018, cnt))
        out += 0.55 * g * _scatter(n, rng, tm, rng.uniform(0.5, 1.0, cnt))
    whoosh = _sweep_band(_pink(n, rng), t, 3500, 700, THROW, THROW + 0.5, 0.6)
    out += 0.6 * whoosh * np.sin(np.pi * np.clip(a / 0.5, 0, 1)) ** 1.2 * (t >= THROW)

    # sparkle: fine high cracks thinning out
    st = THROW + 0.08 + rng.exponential(0.22, 30)
    out += 0.3 * _scatter(n, rng, st[st < LENGTH - 0.05], np.exp(-(st[st < LENGTH - 0.05] - THROW) / 0.35), high=True)

    out = _reverb(out, rng)
    out *= np.clip(t / 0.01, 0, 1) * np.clip((LENGTH - t) / 0.2, 0, 1)
    out *= PEAK / np.abs(out).max()
    return out


RECORDING = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds',
                         'frost-spell-impact.wav')
SKIP, KEEP = 0.85, 2.6              # s: the swell cut (03.11: 0.5 put the peak a little after the impact)


def from_recording(path=RECORDING):
    """03.11 (the user's pick: Pixabay, DRAGON-STUDIO "frost spell impact"): SKIP s of the swell cut, KEEP s kept, faded"""
    import sfx_chain
    x = sfx_chain._read_wav(path)[int(SKIP * RATE):int((SKIP + KEEP) * RATE)]
    tt = np.arange(len(x)) / RATE
    x = x * np.clip(tt / 0.04, 0, 1) * np.clip((KEEP - tt) / 0.6, 0, 1)
    return x * (PEAK / np.abs(x).max())


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
