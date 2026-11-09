r"""Original skill icon art for Mage Skills Expansion, drawn procedurally (numpy only), plus a small DXT5 encoder.

  py -3 -I icon_art.py preview <out.png> [art]     the three button states side by side (art: ice_bolt)

The look follows the game's skill buttons (skillbutton_*_normal.nif, 512x512 DXT5, 10 mips): painted glow on black,
alpha following the brightness (transparent background), hilite brighter and pressed darker than normal (as in the
game's own buttons: about +20 % / -20 %). Everything is deterministic (fixed random seed), so a rebuild gives the same
bytes.
"""
import struct
import sys

import numpy as np


# ---------------------------------------------------------------- drawing helpers
def _blur(a, r):
    """Separable box blur three times (close to a gaussian of sigma ~ r)."""
    out = a.astype(np.float32)
    k = 2 * r + 1
    for _ in range(3):
        for axis in (0, 1):
            pad = [(0, 0)] * out.ndim
            pad[axis] = (r + 1, r)
            c = np.cumsum(np.pad(out, pad, mode='edge'), axis=axis)
            hi = np.take(c, np.arange(k, k + out.shape[axis]), axis=axis)
            lo = np.take(c, np.arange(0, out.shape[axis]), axis=axis)
            out = (hi - lo) / k
    return out


def _noise(size, scale, rng):
    """Smooth value noise in 0..1."""
    n = rng.random((size // scale + 2, size // scale + 2)).astype(np.float32)
    ys = np.linspace(0, size / scale, size, endpoint=False)
    y0 = ys.astype(int)
    f = ys - y0
    f = f * f * (3 - 2 * f)
    a = n[y0][:, y0] * (1 - f)[None, :] + n[y0][:, y0 + 1] * f[None, :]
    b = n[y0 + 1][:, y0] * (1 - f)[None, :] + n[y0 + 1][:, y0 + 1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def _segment(xx, yy, x0, y0, x1, y1):
    """Distance to a segment and the position t (0..1) along it."""
    dx, dy = x1 - x0, y1 - y0
    t = np.clip(((xx - x0) * dx + (yy - y0) * dy) / (dx * dx + dy * dy), 0, 1)
    return np.hypot(xx - (x0 + t * dx), yy - (y0 + t * dy)), t


# ---------------------------------------------------------------- the art
def ice_bolt(size=512, seed=1405):
    """A crystal of ice flying up-right out of a frost trail, cyan/teal glow, shards and sparkles."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    rgb = np.zeros((size, size, 3), np.float32)
    deep = np.array([0.05, 0.22, 0.45])
    teal = np.array([0.10, 0.62, 0.70])
    cyan = np.array([0.55, 0.92, 1.00])
    white = np.array([0.96, 1.00, 1.00])

    # axis of the bolt: from the tail (lower left) to the tip (upper right)
    tail = np.array([96.0, 424.0]) * s
    tip = np.array([438.0, 78.0]) * s
    d = (tip - tail) / np.linalg.norm(tip - tail)
    nrm = np.array([d[1], -d[0]])                       # left of the flight direction (towards the upper left)
    length = np.linalg.norm(tip - tail)
    t = ((xx - tail[0]) * d[0] + (yy - tail[1]) * d[1]) / length          # 0 at the tail, 1 at the tip
    u = (xx - tail[0]) * nrm[0] + (yy - tail[1]) * nrm[1]                 # across, pixels

    # background: dark blue haze and a soft trail glow behind the crystal
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb += deep[None, None] * (0.30 + 0.55 * haze[..., None])
    centre = np.exp(-((xx - size * 0.52) ** 2 + (yy - size * 0.48) ** 2) / (2 * (150 * s) ** 2))
    rgb += teal[None, None] * (0.45 * centre)[..., None]
    behind = np.clip(1 - np.abs(u) / (95 * s), 0, 1) * np.clip(1 - (0.42 - t) / 0.75, 0, 1) * (t < 0.55)
    rgb += teal[None, None] * (0.55 * _blur(behind, int(18 * s))[..., None])

    # frost trail: thin streaks along the axis, fading backwards
    trail = np.zeros((size, size), np.float32)
    for k in range(14):
        off = rng.normal(0, 26) * s
        start, end = rng.uniform(-0.55, -0.15), rng.uniform(0.20, 0.42)
        w = rng.uniform(1.2, 3.2) * s
        line = np.exp(-((u - off) / w) ** 2) * (t > start) * (t < end)
        fade = np.clip((t - start) / (end - start), 0, 1) ** 1.5
        trail += line * fade * rng.uniform(0.35, 0.9)
    trail = trail + 0.6 * _blur(trail, int(4 * s))
    rgb += np.clip(trail, 0, 1.5)[..., None] * (0.55 * cyan + 0.45 * teal)[None, None]

    # the crystal: half-width grows to the widest point at 62 % and closes at the tip
    widest, wmax, wtail = 0.60, 60.0 * s, 10.0 * s
    halfw = np.where(t < widest, wtail + (wmax - wtail) * np.clip(t / widest, 0, 1),
                     wmax * np.clip((1 - t) / (1 - widest), 0, 1))
    inside = (np.abs(u) < halfw) & (t > 0.30) & (t < 1.0)
    edge = np.clip(1 - np.abs(np.abs(u) - halfw) / (2.0 * s), 0, 1) * (t > 0.30) * (t < 1.0)
    # facets: the upper-left half is lit, the other half in shade; a bright ridge along the axis
    lit = (u > 0)
    across = np.clip(np.abs(u) / np.maximum(halfw, 1e-3), 0, 1)
    shade_lit = 0.62 + 0.38 * (1 - across)
    shade_dark = 0.30 + 0.35 * (1 - across)
    body = np.where(lit, shade_lit, shade_dark)
    grain = 0.78 + 0.22 * _noise(size, int(12 * s), rng) + 0.18 * _noise(size, int(4 * s), rng)
    crystal = np.where(lit[..., None], (0.55 * cyan + 0.45 * white)[None, None], (0.55 * deep + 0.45 * teal)[None, None])
    crystal = crystal * (body * grain)[..., None]
    # second ridge: the facet line from the widest point to the tip
    ridge = np.exp(-(u / (2.2 * s)) ** 2) * (t > 0.32)
    facet = np.exp(-((u - 0.45 * halfw) / (1.6 * s)) ** 2) * (t > widest) * 0.6
    # cracks inside the ice
    cracks = np.zeros((size, size), np.float32)
    for k in range(7):
        a0 = rng.uniform(0.38, 0.9)
        p0 = tail + d * length * a0 + nrm * rng.uniform(-20, 20) * s
        ang = rng.uniform(-1.2, 1.2)
        dirc = d * np.cos(ang) + nrm * np.sin(ang)
        p1 = p0 + dirc * rng.uniform(14, 34) * s
        dist, _ = _segment(xx, yy, p0[0], p0[1], p1[0], p1[1])
        cracks += np.exp(-(dist / (0.9 * s)) ** 2) * rng.uniform(0.3, 0.7)
    m = inside.astype(np.float32)
    m = _blur(m, 1) if s >= 1 else m
    rgb = rgb * (1 - 0.85 * m[..., None]) + crystal * m[..., None]
    rgb += (white[None, None] * (0.9 * ridge + facet + 0.35 * np.clip(cracks, 0, 1))[..., None]) * m[..., None]
    rgb += white[None, None] * (0.9 * edge)[..., None]

    # glow around the crystal, strongest near the tip
    glow = _blur(m, int(14 * s)) * (0.6 + 0.8 * np.clip(t, 0, 1))
    rgb += (0.6 * cyan + 0.4 * teal)[None, None] * (0.9 * glow)[..., None]
    halo = _blur(m, int(60 * s))
    rgb += teal[None, None] * (1.1 * halo)[..., None]
    tipglow = np.exp(-(np.hypot(xx - tip[0], yy - tip[1]) / (26 * s)) ** 2)
    rgb += (0.5 * white + 0.5 * cyan)[None, None] * (1.1 * tipglow)[..., None]
    for ang in (0.0, np.pi / 2, np.pi / 4 + 0.35, -np.pi / 4 + 0.35):
        v = np.array([np.cos(ang), np.sin(ang)])
        along = (xx - tip[0]) * v[0] + (yy - tip[1]) * v[1]
        acr = -(xx - tip[0]) * v[1] + (yy - tip[1]) * v[0]
        ray = np.exp(-(acr / (1.4 * s)) ** 2) * np.exp(-(along / (70 * s)) ** 2)
        rgb += (0.6 * white + 0.4 * cyan)[None, None] * (0.9 * ray)[..., None]
    # frost mist along the trail
    mist = _noise(size, int(22 * s), rng) * _noise(size, int(9 * s), rng)
    band = np.exp(-(u / (60 * s)) ** 2) * np.clip(1 - (0.5 - t) / 0.9, 0, 1) * (t < 0.62)
    rgb += (0.5 * teal + 0.5 * cyan)[None, None] * (1.4 * _blur(mist * band, int(3 * s)))[..., None]

    # ice shards breaking off along the trail and sparkles
    for k in range(16):
        a0 = rng.uniform(-0.25, 0.55)
        c = tail + d * length * a0 + nrm * rng.normal(0, 40) * s
        r = rng.uniform(4, 13) * s
        ang = rng.uniform(0, np.pi)
        v = np.array([np.cos(ang), np.sin(ang)])
        pu = (xx - c[0]) * v[0] + (yy - c[1]) * v[1]
        pv = -(xx - c[0]) * v[1] + (yy - c[1]) * v[0]
        shard = (np.abs(pu) / r + np.abs(pv) / (0.42 * r)) < 1
        sm = shard.astype(np.float32)
        light = np.where(pv > 0, 1.0, 0.55)
        rgb = rgb * (1 - 0.7 * sm[..., None]) + (sm * light)[..., None] * (0.5 * cyan + 0.5 * white)[None, None]
        rgb += cyan[None, None] * (0.35 * _blur(sm, int(5 * s)))[..., None]
    for k in range(22):
        c = tail + d * length * rng.uniform(-0.3, 1.05) + nrm * rng.normal(0, 55) * s
        r = rng.uniform(2, 7) * s
        dx, dy = np.abs(xx - c[0]), np.abs(yy - c[1])
        star = np.exp(-(dx / (0.8 * s)) ** 2) * np.exp(-(dy / r) ** 2) + np.exp(-(dy / (0.8 * s)) ** 2) * np.exp(-(dx / r) ** 2)
        star += np.exp(-(np.hypot(dx, dy) / (1.6 * s)) ** 2)
        rgb += (0.7 * white + 0.3 * cyan)[None, None] * (rng.uniform(0.5, 1.0) * star)[..., None]

    # tone: soft clip, slight vignette; alpha follows the brightness like the game's buttons
    rgb = 1 - np.exp(-1.25 * rgb)
    vig = 1 - 0.35 * np.clip((np.hypot(xx - size / 2, yy - size / 2) / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def frost_nova(size=512, seed=2711):
    """A burst of frost around a bright core: a ring of ice spikes pointing outwards, a shockwave ring, mist, shards."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    rgb = np.zeros((size, size, 3), np.float32)
    deep = np.array([0.05, 0.22, 0.45])
    teal = np.array([0.10, 0.62, 0.70])
    cyan = np.array([0.55, 0.92, 1.00])
    white = np.array([0.96, 1.00, 1.00])
    cx, cy = size * 0.5, size * 0.53
    dx, dy = xx - cx, yy - cy
    r = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)

    # background: dark blue haze, a teal disc of cold air
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb += deep[None, None] * (0.30 + 0.55 * haze[..., None])
    rgb += teal[None, None] * (0.55 * np.exp(-(r / (170 * s)) ** 2))[..., None]

    # shockwave: a bright ring with a soft inner edge and frosty breaks
    ring_r = 182 * s
    breaks = 0.65 + 0.35 * _noise(size, int(10 * s), rng)
    ring = np.exp(-((r - ring_r) / (7 * s)) ** 2) * breaks
    rgb += (0.6 * cyan + 0.4 * white)[None, None] * (1.1 * ring)[..., None]
    rgb += teal[None, None] * (0.8 * _blur(ring, int(16 * s)))[..., None]
    inner = np.clip(1 - (ring_r - r) / (70 * s), 0, 1) * (r < ring_r)
    rgb += cyan[None, None] * (0.25 * inner)[..., None]

    # ice spikes: thin lit/shaded triangles from near the core outwards, in two layers
    spikes = np.zeros((size, size), np.float32)
    lit_all = np.zeros((size, size), np.float32)
    n = 13
    for layer, (count, r0, r1, w0) in enumerate(((n, 34, 168, 17), (n, 30, 118, 11))):
        for k in range(count):
            a = 2 * np.pi * (k + 0.5 * layer + rng.uniform(-0.18, 0.18)) / count
            length = (r1 + rng.uniform(-18, 22)) * s
            v = np.array([np.cos(a), np.sin(a)])
            along = dx * v[0] + dy * v[1]
            acr = -dx * v[1] + dy * v[0]
            t = (along - r0 * s) / (length - r0 * s)
            half = w0 * s * np.clip(1 - t, 0, 1)
            m = ((t > 0) & (t < 1) & (np.abs(acr) < half)).astype(np.float32)
            spikes = np.maximum(spikes, m)
            lit_all = np.maximum(lit_all, m * np.where(acr > 0, 1.0, 0.45) * (0.65 + 0.35 * t))
            spikes_edge = np.exp(-((np.abs(acr) - half) / (1.4 * s)) ** 2) * (t > 0) * (t < 1)
            rgb += white[None, None] * (0.45 * spikes_edge)[..., None]
    grain = 0.8 + 0.2 * _noise(size, int(8 * s), rng)
    body = (lit_all * grain)[..., None] * (0.55 * cyan + 0.45 * white)[None, None] + \
           ((1 - lit_all) * spikes)[..., None] * (0.5 * deep + 0.5 * teal)[None, None]
    rgb = rgb * (1 - 0.8 * spikes[..., None]) + body * spikes[..., None]
    rgb += (0.6 * cyan + 0.4 * teal)[None, None] * (0.7 * _blur(spikes, int(12 * s)))[..., None]

    # core: white-hot cold centre with a star flare
    core = np.exp(-(r / (34 * s)) ** 2)
    rgb += (0.6 * white + 0.4 * cyan)[None, None] * (1.6 * core)[..., None]
    for a in (0.0, np.pi / 2, np.pi / 4, -np.pi / 4):
        v = np.array([np.cos(a), np.sin(a)])
        along = dx * v[0] + dy * v[1]
        acr = -dx * v[1] + dy * v[0]
        ray = np.exp(-(acr / (1.6 * s)) ** 2) * np.exp(-(along / ((95 if a in (0.0, np.pi / 2) else 55) * s)) ** 2)
        rgb += (0.6 * white + 0.4 * cyan)[None, None] * (1.0 * ray)[..., None]

    # mist swirling at the ring, shards flying outwards, sparkles
    mist = _noise(size, int(22 * s), rng) * _noise(size, int(9 * s), rng)
    band = np.exp(-((r - ring_r * 0.92) / (48 * s)) ** 2)
    rgb += (0.5 * teal + 0.5 * cyan)[None, None] * (1.3 * _blur(mist * band, int(3 * s)))[..., None]
    for k in range(18):
        a = rng.uniform(0, 2 * np.pi)
        rad = rng.uniform(150, 225) * s
        c = np.array([cx + np.cos(a) * rad, cy + np.sin(a) * rad])
        rr = rng.uniform(4, 11) * s
        v = np.array([np.cos(a), np.sin(a)])
        pu = (xx - c[0]) * v[0] + (yy - c[1]) * v[1]
        pv = -(xx - c[0]) * v[1] + (yy - c[1]) * v[0]
        sm = ((np.abs(pu) / rr + np.abs(pv) / (0.42 * rr)) < 1).astype(np.float32)
        light = np.where(pv > 0, 1.0, 0.55)
        rgb = rgb * (1 - 0.7 * sm[..., None]) + (sm * light)[..., None] * (0.5 * cyan + 0.5 * white)[None, None]
        rgb += cyan[None, None] * (0.3 * _blur(sm, int(5 * s)))[..., None]
    for k in range(20):
        a = rng.uniform(0, 2 * np.pi)
        rad = rng.uniform(40, 230) * s
        c = (cx + np.cos(a) * rad, cy + np.sin(a) * rad)
        rr = rng.uniform(2, 7) * s
        ax, ay = np.abs(xx - c[0]), np.abs(yy - c[1])
        star = np.exp(-(ax / (0.8 * s)) ** 2) * np.exp(-(ay / rr) ** 2) + np.exp(-(ay / (0.8 * s)) ** 2) * np.exp(-(ax / rr) ** 2)
        star += np.exp(-(np.hypot(ax, ay) / (1.6 * s)) ** 2)
        rgb += (0.7 * white + 0.3 * cyan)[None, None] * (rng.uniform(0.5, 1.0) * star)[..., None]

    rgb = 1 - np.exp(-1.25 * rgb)
    vig = 1 - 0.35 * np.clip((np.hypot(xx - size / 2, yy - size / 2) / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def _bolt(xx, yy, a, b, rng, s, jag=0.22, steps=9):
    """A jagged bolt from a to b: (distance field of the core, the polyline)."""
    d = b - a
    nrm = np.array([-d[1], d[0]]) / max(np.linalg.norm(d), 1e-3)
    pts = [a]
    for k in range(1, steps):
        t = k / steps
        pts.append(a + d * t + nrm * rng.normal(0, jag) * np.linalg.norm(d) * 0.18)
    pts.append(b)
    dist = np.full(xx.shape, 1e9, np.float32)
    for p, q in zip(pts[:-1], pts[1:]):
        seg, _ = _segment(xx, yy, p[0], p[1], q[0], q[1])
        dist = np.minimum(dist, seg)
    return dist, pts


def chain_lightning(size=512, seed=3307):
    """A bolt leaping from the upper left down through three glowing points, forks, violet-blue glow, sparks."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    rgb = np.zeros((size, size, 3), np.float32)
    deep = np.array([0.12, 0.08, 0.38])
    violet = np.array([0.45, 0.30, 0.95])
    blue = np.array([0.45, 0.70, 1.00])
    white = np.array([0.97, 0.97, 1.00])

    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb += deep[None, None] * (0.30 + 0.55 * haze[..., None])
    rgb += violet[None, None] * (0.35 * np.exp(-((xx - size * 0.5) ** 2 + (yy - size * 0.5) ** 2) / (2 * (170 * s) ** 2)))[..., None]

    # the chain: the hand (upper left) -> three targets
    nodes = [np.array(p) * s for p in ((70.0, 70.0), (215.0, 205.0), (390.0, 165.0), (330.0, 410.0))]
    core = np.zeros((size, size), np.float32)
    for k, (a, b) in enumerate(zip(nodes[:-1], nodes[1:])):
        dist, pts = _bolt(xx, yy, a, b, rng, s)
        w = (5.5 - 1.2 * k) * s
        core = np.maximum(core, np.exp(-(dist / w) ** 2) * (1.0 - 0.12 * k))
        for f in range(3):                                      # forks off the bolt
            p = pts[rng.integers(2, len(pts) - 2)]
            ang = rng.uniform(0, 2 * np.pi)
            q = p + np.array([np.cos(ang), np.sin(ang)]) * rng.uniform(30, 60) * s
            fd, _ = _bolt(xx, yy, p, q, rng, s, jag=0.35, steps=5)
            core = np.maximum(core, np.exp(-(fd / (1.6 * s)) ** 2) * 0.7)
    rgb += white[None, None] * (1.3 * core)[..., None]
    rgb += (0.6 * blue + 0.4 * violet)[None, None] * (1.3 * _blur(core, int(6 * s)))[..., None]
    rgb += violet[None, None] * (1.4 * _blur(core, int(26 * s)))[..., None]

    # the struck points: bright balls with a star flare
    for k, c in enumerate(nodes):
        r = np.hypot(xx - c[0], yy - c[1])
        size_k = (30 if k == 0 else 22) * s
        rgb += (0.6 * white + 0.4 * blue)[None, None] * (1.5 * np.exp(-(r / size_k) ** 2))[..., None]
        rgb += violet[None, None] * (0.8 * np.exp(-(r / (size_k * 2.6)) ** 2))[..., None]
        for a in (0.0, np.pi / 2):
            v = np.array([np.cos(a), np.sin(a)])
            along = (xx - c[0]) * v[0] + (yy - c[1]) * v[1]
            acr = -(xx - c[0]) * v[1] + (yy - c[1]) * v[0]
            ray = np.exp(-(acr / (1.3 * s)) ** 2) * np.exp(-(along / (60 * s)) ** 2)
            rgb += white[None, None] * (0.8 * ray)[..., None]

    for k in range(26):                                         # sparks
        c = nodes[rng.integers(1, 4)] + rng.normal(0, 38, 2) * s
        rr = rng.uniform(2, 6) * s
        ax, ay = np.abs(xx - c[0]), np.abs(yy - c[1])
        star = np.exp(-(ax / (0.8 * s)) ** 2) * np.exp(-(ay / rr) ** 2) + np.exp(-(ay / (0.8 * s)) ** 2) * np.exp(-(ax / rr) ** 2)
        rgb += (0.7 * white + 0.3 * blue)[None, None] * (rng.uniform(0.4, 0.9) * star)[..., None]

    rgb = 1 - np.exp(-1.25 * rgb)
    vig = 1 - 0.35 * np.clip((np.hypot(xx - size / 2, yy - size / 2) / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def arcane_blink(size=512, seed=5401):
    """Two violet rifts, the far one bright, joined by a streak of motes."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    deep = np.array([0.10, 0.06, 0.32])
    violet = np.array([0.50, 0.32, 1.00])
    white = np.array([0.96, 0.95, 1.00])
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb = deep[None, None] * (0.30 + 0.55 * haze[..., None])
    a, b = np.array((135.0, 370.0)) * s, np.array((375.0, 140.0)) * s
    for c, gain, w in ((a, 0.55, 34.0), (b, 1.3, 46.0)):           # upright glowing rifts
        r = np.hypot((xx - c[0]) * 2.6, yy - c[1])
        rgb += violet[None, None] * (gain * np.exp(-(r / (w * 2.4 * s)) ** 2))[..., None]
        rgb += white[None, None] * (gain * 1.2 * np.exp(-(r / (w * s)) ** 2))[..., None]
    for k in range(70):                                            # the streak
        t = rng.uniform(0, 1)
        c = a + (b - a) * t + rng.normal(0, 16, 2) * s
        rr = rng.uniform(2.5, 8) * s
        r = np.hypot(xx - c[0], yy - c[1])
        rgb += (0.7 * white + 0.3 * violet)[None, None] * ((0.3 + 0.7 * t) * np.exp(-(r / rr) ** 2))[..., None]
    rgb = 1 - np.exp(-1.25 * rgb)
    vig = 1 - 0.35 * np.clip((np.hypot(xx - size / 2, yy - size / 2) / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def arcane_push(size=512, seed=5402):
    """A bright centre and violet rings of force bursting outwards, motes thrown along the rays."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    deep = np.array([0.10, 0.06, 0.32])
    violet = np.array([0.50, 0.32, 1.00])
    white = np.array([0.96, 0.95, 1.00])
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb = deep[None, None] * (0.30 + 0.55 * haze[..., None])
    r = np.hypot(xx - size / 2, yy - size / 2)
    rgb += white[None, None] * (1.5 * np.exp(-(r / (42 * s)) ** 2))[..., None]
    rgb += violet[None, None] * (1.0 * np.exp(-(r / (95 * s)) ** 2))[..., None]
    for rad, w, g in ((120, 9, 1.1), (178, 8, 0.8), (225, 7, 0.5)):
        rgb += (0.5 * white + 0.5 * violet)[None, None] * (g * np.exp(-((r - rad * s) / (w * s)) ** 2))[..., None]
        rgb += violet[None, None] * (g * 0.6 * np.exp(-((r - rad * s) / (w * 3.2 * s)) ** 2))[..., None]
    for k in range(60):
        a, d = rng.uniform(0, 2 * np.pi), rng.uniform(70, 235) * s
        c = (size / 2 + np.cos(a) * d, size / 2 + np.sin(a) * d)
        q = np.hypot(xx - c[0], yy - c[1])
        rgb += white[None, None] * (0.7 * np.exp(-(q / (rng.uniform(2.5, 6) * s)) ** 2))[..., None]
    rgb = 1 - np.exp(-1.25 * rgb)
    vig = 1 - 0.35 * np.clip((r / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def arcane_shield(size=512, seed=5403):
    """A figure-sized glowing shell: blue-violet rim round a dark centre, with facets of light."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    deep = np.array([0.06, 0.08, 0.30])
    blue = np.array([0.35, 0.55, 1.00])
    white = np.array([0.95, 0.97, 1.00])
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb = deep[None, None] * (0.30 + 0.55 * haze[..., None])
    r = np.hypot((xx - size / 2) * 1.25, yy - size / 2)
    rgb += blue[None, None] * (1.3 * np.exp(-((r - 195 * s) / (34 * s)) ** 2))[..., None]
    rgb += white[None, None] * (1.3 * np.exp(-((r - 195 * s) / (8 * s)) ** 2))[..., None]
    for k in range(40):
        a, d = rng.uniform(0, 2 * np.pi), rng.uniform(40, 180) * s
        c = (size / 2 + np.cos(a) * d / 1.25, size / 2 + np.sin(a) * d)
        q = np.hypot(xx - c[0], yy - c[1])
        rgb += (0.6 * white + 0.4 * blue)[None, None] * (0.5 * np.exp(-(q / (rng.uniform(3, 8) * s)) ** 2))[..., None]
    rgb = 1 - np.exp(-1.25 * rgb)
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def _polygon(xx, yy, pts):
    """1 inside a polygon, by crossings (numpy)"""
    inside = np.zeros_like(xx, dtype=bool)
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        hit = ((y0 > yy) != (y1 > yy)) & (xx < (x1 - x0) * (yy - y0) / (y1 - y0 + 1e-6) + x0)
        inside ^= hit
    return inside.astype(np.float32)


def ice_bolt_new(size=512, seed=1411):
    """05.11: our bolt as it flies: a cluster of ice blades flying to the upper right, a frost trail with flakes behind it."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    deep = np.array([0.03, 0.10, 0.26])
    blue = np.array([0.30, 0.65, 1.00])
    pale = np.array([0.75, 0.92, 1.00])
    white = np.array([0.97, 0.99, 1.00])
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb = deep[None, None] * (0.45 + 0.55 * haze[..., None])
    head = np.array((345.0, 165.0)) * s
    tail = np.array((70.0, 445.0)) * s
    d = (head - tail) / np.linalg.norm(head - tail)
    n_ = np.array((-d[1], d[0]))
    along = (xx - tail[0]) * d[0] + (yy - tail[1]) * d[1]
    acr = (xx - tail[0]) * n_[0] + (yy - tail[1]) * n_[1]
    L = np.linalg.norm(head - tail)
    t = np.clip(along / L, 0, 1)
    trail = np.exp(-(acr / ((10 + 62 * t) * s)) ** 2) * (along > 0) * (along < L) * t ** 1.2
    cloud = _blur((_noise(size, int(32 * s), rng) > 0.5).astype(np.float32), int(8 * s))
    rgb += blue[None, None] * (1.1 * trail * (0.55 + 0.9 * cloud))[..., None]
    rgb += pale[None, None] * (0.5 * trail ** 2)[..., None]
    r = np.hypot(xx - head[0], yy - head[1])
    rgb += blue[None, None] * (1.2 * np.exp(-(r / (110 * s)) ** 2))[..., None]
    # the blades: long spikes out of the centre, the longest forward
    ang0 = np.arctan2(d[1], d[0])
    blades = [(0.0, 150, 30), (0.55, 92, 24), (-0.6, 96, 24), (1.25, 72, 20), (-1.3, 74, 20), (2.0, 62, 19), (-2.05, 60, 19), (2.75, 66, 18), (-2.8, 58, 18)]
    for k, (da, ln, w) in enumerate(sorted(blades, key=lambda b: b[1])):
        a = ang0 + da
        u = np.array((np.cos(a), np.sin(a)))
        v = np.array((-u[1], u[0]))
        tip = head + u * ln * s
        base1, base2 = head + v * w * s, head - v * w * s
        back = head - u * w * 0.8 * s
        left = _polygon(xx, yy, [tip, base1, back])
        right = _polygon(xx, yy, [tip, back, base2])
        mask = np.maximum(left, right)
        lit = 0.55 if (u[0] - u[1]) > 0 else 0.3
        col = 2.6 * (left[..., None] * (lit * pale + (1 - lit) * blue) + right[..., None] * (0.15 * pale + 0.5 * blue))
        rgb = rgb * (1 - mask[..., None]) + col * mask[..., None]
    rgb += white[None, None] * (0.7 * np.exp(-(r / (16 * s)) ** 2))[..., None]
    for k in range(34):                                         # flakes in the trail
        tt = rng.uniform(0.05, 0.85) ** 0.8
        c = tail + (head - tail) * tt + n_ * rng.normal(0, 16 + 50 * tt) * s
        rr = rng.uniform(3, 9) * s
        ax, ay = np.abs(xx - c[0]), np.abs(yy - c[1])
        star = np.exp(-(ax / (1.0 * s)) ** 2) * np.exp(-(ay / rr) ** 2) + np.exp(-(ay / (1.0 * s)) ** 2) * np.exp(-(ax / rr) ** 2)
        rgb += white[None, None] * (rng.uniform(0.4, 1.0) * star)[..., None]
    rgb = np.clip(rgb, 0, None)
    rgb = 1 - np.exp(-1.5 * rgb)
    vig = 1 - 0.35 * np.clip((np.hypot(xx - size / 2, yy - size / 2) / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def arcane_shield_new(size=512, seed=5404):
    """05.11: plainly a shield: a heraldic shield of violet-blue force with a bright rim (05.11: the rune in the middle taken away)."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    deep = np.array([0.05, 0.05, 0.22])
    violet = np.array([0.42, 0.32, 1.00])
    blue = np.array([0.35, 0.65, 1.00])
    white = np.array([0.95, 0.97, 1.00])
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb = deep[None, None] * (0.45 + 0.55 * haze[..., None])
    cx, top, bot, half = 256.0 * s, 95.0 * s, 435.0 * s, 145.0 * s
    pts = []
    for t in np.linspace(0, 1, 24):                             # the right side down to the point
        y = top + (bot - top) * t
        w = half * (1.0 if t < 0.45 else np.sqrt(max(0.0, 1 - ((t - 0.45) / 0.55) ** 2)))
        pts.append((cx + w, y))
    pts += [(2 * cx - x, y) for x, y in reversed(pts)]
    inner = _polygon(xx, yy, [(cx + (x - cx) * 0.86, (top + bot) / 2 + (y - (top + bot) / 2) * 0.88) for x, y in pts])
    body = _polygon(xx, yy, pts)
    rim = np.clip(body - inner, 0, 1)
    glow = _blur(body, int(30 * s))
    rgb += violet[None, None] * (1.3 * glow * (1 - body))[..., None]
    h = np.clip((yy - top) / (bot - top), 0, 1)
    side = np.clip((xx - cx) / half, -1, 1)
    fill = (0.55 + 0.45 * (1 - h)) * (0.8 - 0.25 * side)
    tex = _blur((_noise(size, int(24 * s), rng) > 0.5).astype(np.float32), int(5 * s))
    col = (0.65 * violet + 0.35 * blue)[None, None] * (fill * (0.65 + 0.5 * tex))[..., None]
    rgb = rgb * (1 - body[..., None]) + col * body[..., None]
    rgb += white[None, None] * (1.3 * _blur(rim, int(2 * s)))[..., None]
    rgb += blue[None, None] * (0.8 * _blur(rim, int(9 * s)))[..., None]
    rgb = 1 - np.exp(-1.35 * rgb)
    vig = 1 - 0.35 * np.clip((np.hypot(xx - size / 2, yy - size / 2) / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def spell_critical(size=512, seed=5405):
    """05.11: a white-hot four-pointed star cracking through a violet orb, golden sparks flying out."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    deep = np.array([0.08, 0.04, 0.24])
    violet = np.array([0.50, 0.28, 1.00])
    gold = np.array([1.00, 0.72, 0.22])
    white = np.array([1.00, 0.98, 0.92])
    cx, cy = 256.0 * s, 256.0 * s
    dx, dy = xx - cx, yy - cy
    r = np.hypot(dx, dy)
    haze = _blur((_noise(size, int(64 * s), rng) > 0.55).astype(np.float32), int(24 * s))
    rgb = deep[None, None] * (0.45 + 0.55 * haze[..., None])
    rgb += violet[None, None] * (1.3 * np.exp(-(r / (140 * s)) ** 2))[..., None]
    a, b = (dx + dy) / 1.414, (dx - dy) / 1.414
    star = np.exp(-(np.abs(dx) / (10 * s + 0.0 * r)) ) * np.exp(-(np.abs(dy) / (190 * s)) ** 2) + np.exp(-(np.abs(dy) / (10 * s))) * np.exp(-(np.abs(dx) / (190 * s)) ** 2)
    small = np.exp(-(np.abs(a) / (7 * s))) * np.exp(-(np.abs(b) / (95 * s)) ** 2) + np.exp(-(np.abs(b) / (7 * s))) * np.exp(-(np.abs(a) / (95 * s)) ** 2)
    rgb += gold[None, None] * (1.5 * _blur(star + small, int(6 * s)))[..., None]
    rgb += white[None, None] * (2.0 * (star + 0.7 * small))[..., None]
    rgb += white[None, None] * (2.5 * np.exp(-(r / (34 * s)) ** 2))[..., None]
    for k in range(26):
        ang, dist = rng.uniform(0, 6.283), rng.uniform(70, 220) * s
        px, py = cx + np.cos(ang) * dist, cy + np.sin(ang) * dist
        rgb += gold[None, None] * (rng.uniform(0.5, 1.2) * np.exp(-(np.hypot(xx - px, yy - py) / (rng.uniform(3, 7) * s)) ** 2))[..., None]
    rgb = 1 - np.exp(-1.4 * np.clip(rgb, 0, None))
    vig = 1 - 0.35 * np.clip((r / (size * 0.62)) ** 2, 0, 1)
    rgb *= vig[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def hunters_mark(size=512, seed=5407):
    """05.11: a beast's burning eye seen through an engraved bronze sight, an arrow across it, a dark forest behind."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    dx, dy = xx - 256 * s, yy - 256 * s
    r, ang = np.hypot(dx, dy), np.arctan2(dy, dx)
    red = np.array([1.00, 0.14, 0.05])
    amber = np.array([1.00, 0.62, 0.15])
    bronze = np.array([0.80, 0.55, 0.25])
    pale = np.array([1.00, 0.90, 0.65])
    # the forest: trunks and a green mist
    mist = _blur((_noise(size, int(48 * s), rng) > 0.5).astype(np.float32), int(18 * s))
    rgb = np.array([0.03, 0.10, 0.06])[None, None] * (0.6 + 1.2 * mist[..., None] * np.clip(1.2 - yy / size, 0, 1)[..., None])
    for k in range(13):
        x0, w = rng.uniform(0, size), rng.uniform(7, 22) * s
        rgb *= (1 - 0.8 * np.exp(-((xx - x0 - 0.05 * (yy - 256 * s)) / w) ** 2))[..., None]
    # inside the sight it is darker and red
    inner = np.clip((168 * s - r) / (3 * s), 0, 1)
    rgb = rgb * (1 - 0.75 * inner[..., None]) + red[None, None] * (0.25 * inner * np.exp(-(r / (120 * s)) ** 2))[..., None]
    # the eye: an almond with a slit pupil
    h = np.clip(1 - (dx / (118 * s)) ** 2, 0, 1) * 58 * s
    eye = np.clip((h - np.abs(dy)) / (3 * s), 0, 1)
    iris = np.exp(-(r / (60 * s)) ** 2)
    pupil = np.clip(1 - (np.abs(dx) / (9 * s + 0.0 * r)) ** 2 - (dy / (54 * s)) ** 2, 0, 1)
    veins = np.clip(np.cos(ang * 19 + 3 * np.sin(ang * 5)) * 0.5 + 0.5, 0, 1) ** 3
    col = amber[None, None] * (1.5 * iris + 0.3)[..., None] + red[None, None] * (0.8 + 0.8 * veins)[..., None]
    col = col * (1 - np.clip(pupil * 4, 0, 1))[..., None]
    rgb = rgb * (1 - eye[..., None]) + col * eye[..., None]
    rgb += red[None, None] * (0.9 * _blur(eye, int(16 * s)))[..., None]
    # the ring: bronze, lit from the upper left, with engraved ticks
    band = np.clip((np.minimum(r - 168 * s, 206 * s - r)) / (2.5 * s), 0, 1)
    lit = 0.55 + 0.45 * (-(dx + dy) / (r + 1) * 0.707)
    ticks = 1 - 0.65 * (np.abs(np.mod(ang, 2 * np.pi / 48) - np.pi / 48) > np.pi / 48 * 0.72)
    edge = np.exp(-((r - 170 * s) / (2.5 * s)) ** 2) + np.exp(-((r - 204 * s) / (2.5 * s)) ** 2)
    metal = bronze[None, None] * (lit * ticks)[..., None] * 1.1 + pale[None, None] * (0.8 * edge * lit)[..., None]
    rgb = rgb * (1 - band[..., None]) + metal * band[..., None]
    # four sight blades reaching in
    for a in (0.0, np.pi / 2, np.pi, -np.pi / 2):
        u, v = np.cos(a) * dx + np.sin(a) * dy, -np.sin(a) * dx + np.cos(a) * dy
        blade = np.clip((((u - 128 * s) / (42 * s)) * 9 * s - np.abs(v)) / (1.5 * s), 0, 1) * (u < 172 * s)
        shade = 0.7 + 0.5 * (v > 0)
        rgb = rgb * (1 - blade[..., None]) + (pale * 0.5 + bronze * 0.6)[None, None] * (blade * shade)[..., None]
    # the arrow across, from the lower left to the upper right
    a = -np.pi / 4
    u, v = np.cos(a) * dx + np.sin(a) * dy, -np.sin(a) * dx + np.cos(a) * dy
    shaft = np.clip((5 * s - np.abs(v)) / (1.5 * s), 0, 1) * (u > -235 * s) * (u < 180 * s)
    head = np.clip(((232 * s - u) * 0.38 - np.abs(v)) / (1.5 * s), 0, 1) * (u >= 170 * s)
    fl = np.clip((22 * s - np.abs(v)) / (1.5 * s), 0, 1) * (u > -240 * s) * (u < -175 * s) * (np.mod(u / (11 * s), 1.0) < 0.72)
    rgb = rgb * (1 - shaft[..., None]) + np.array([0.42, 0.26, 0.12])[None, None] * ((0.75 + 0.5 * (v < 0)) * shaft)[..., None]
    rgb = rgb * (1 - fl[..., None]) + np.array([0.75, 0.75, 0.65])[None, None] * fl[..., None]
    rgb = rgb * (1 - head[..., None]) + (pale * 0.9)[None, None] * (head * (0.7 + 0.5 * (v < 0)))[..., None]
    rgb = 1 - np.exp(-1.5 * np.clip(rgb, 0, None))
    rgb *= (1 - 0.4 * np.clip((r / (size * 0.66)) ** 2, 0, 1))[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def second_wind(size=512, seed=5409):
    """05.11, second try: a sword planted in the ground at dusk, a golden wind spiralling up around it and a burst of light at the hilt."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    x, y = xx / s - 256, yy / s
    r = np.hypot(x, y - 150)
    # dusk sky and dark ground
    sky = np.clip(y / 512, 0, 1)
    cloud = _blur((_noise(size, int(56 * s), rng) > 0.52).astype(np.float32), int(16 * s))
    rgb = (np.array([0.07, 0.09, 0.20])[None, None] * (1 - sky[..., None]) + np.array([0.45, 0.20, 0.10])[None, None] * sky[..., None]) * (0.75 + 0.5 * cloud[..., None])
    ground = np.clip((y - 408 - 7 * np.sin(x / 40)) / 3, 0, 1)
    rgb = rgb * (1 - ground[..., None]) + np.array([0.035, 0.03, 0.03])[None, None] * ground[..., None]
    # back half of the wind
    def wind(front):
        out = np.zeros((size, size), np.float32)
        for k, (ph, w0) in enumerate(((0.0, 9.0), (3.14, 6.0))):
            t = np.linspace(0, 1, 1400)
            py = 400 - 330 * t
            amp = 22 + 120 * t ** 0.8
            a = t * 1.6 * 2 * np.pi + ph
            px = amp * np.sin(a)
            depth = np.cos(a)
            for q in range(0, 1400):
                if (depth[q] > 0) != front:
                    continue
                wd = w0 * (0.25 + 0.9 * np.sin(t[q] * np.pi)) * s
                cx, cy = (256 + px[q]) * s, py[q] * s
                x0, x1, y0, y1 = int(max(cx - 3 * wd, 0)), int(min(cx + 3 * wd + 1, size)), int(max(cy - 3 * wd, 0)), int(min(cy + 3 * wd + 1, size))
                if x1 <= x0 or y1 <= y0:
                    continue
                g = np.exp(-((xx[y0:y1, x0:x1] - cx) ** 2 + (yy[y0:y1, x0:x1] - cy) ** 2) / (wd * wd))
                out[y0:y1, x0:x1] = np.maximum(out[y0:y1, x0:x1], g)
        return out
    gold = np.array([1.0, 0.70, 0.22])
    white = np.array([1.0, 0.95, 0.75])
    back = wind(False)
    rgb += gold[None, None] * (0.45 * back)[..., None]
    # the sword: blade point down in the ground, lit on one side
    half = np.clip((430 - y) / 300, 0, 1) * 15 + 2
    blade = np.clip((half - np.abs(x)) / 1.5, 0, 1) * (y > 135) * (y < 428)
    shade = np.where(x < 0, 0.95, 0.42)
    rgb = rgb * (1 - blade[..., None]) + np.array([0.75, 0.80, 0.90])[None, None] * (blade * shade)[..., None]
    rgb += white[None, None] * (0.8 * np.exp(-(x / 1.6) ** 2) * blade)[..., None]
    guard = np.clip((58 - np.abs(x)) / 1.5, 0, 1) * np.clip((9 + np.abs(x) * 0.09 - np.abs(y - 128)) / 1.5, 0, 1)
    grip = np.clip((8 - np.abs(x)) / 1.5, 0, 1) * (y > 70) * (y < 122)
    pommel = np.clip((13 - np.hypot(x, y - 60)) / 1.5, 0, 1)
    for mask, col in ((grip, (0.25, 0.13, 0.07)), (guard, (0.85, 0.60, 0.25)), (pommel, (0.95, 0.70, 0.30))):
        rgb = rgb * (1 - mask[..., None]) + np.array(col)[None, None] * mask[..., None]
    # front half of the wind and the light
    front = wind(True)
    rgb += gold[None, None] * (0.7 * _blur(front, int(5 * s)) + 0.7 * front)[..., None] + white[None, None] * (0.9 * front ** 3)[..., None]
    rgb += gold[None, None] * (0.5 * np.exp(-(r / 70) ** 2))[..., None]
    for k in range(22):
        a, d = rng.uniform(0, 6.283), rng.uniform(60, 215)
        px, py = (256 + np.cos(a) * d) * s, (235 + np.sin(a) * d) * s
        rgb += white[None, None] * (rng.uniform(0.5, 1.3) * np.exp(-((xx - px) ** 2 + (yy - py) ** 2) / (rng.uniform(2, 4.5) * s) ** 2))[..., None]
    rgb = 1 - np.exp(-1.5 * np.clip(rgb, 0, None))
    rgb *= (1 - 0.3 * np.clip((np.hypot(x, y - 256) / 340) ** 2, 0, 1))[..., None]
    alpha = np.clip(rgb.max(-1) * 2.2, 0, 1)
    return np.concatenate([rgb, alpha[..., None]], -1)


def pack_hunger(size=512, seed=5411):
    """05.11, in the game's style (Fear, Regenerate, Whirlwind): no scene and a transparent ground; a shaded glowing subject: a maw of fangs in a crimson ring,
    a swirl of blood light around it, white flares."""
    rng = np.random.default_rng(seed)
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    x, y = xx / s - 256, yy / s - 256
    r, ang = np.hypot(x, y), np.arctan2(y, x)
    crimson = np.array([1.0, 0.10, 0.08])
    hot = np.array([1.0, 0.55, 0.45])
    white = np.array([1.0, 0.95, 0.92])
    R = 138.0
    rgb = np.zeros((size, size, 3), np.float32)
    alpha = np.zeros((size, size), np.float32)
    # the swirl behind: bright strokes along a spiral, as Whirlwind's
    sw = 0
    for k, (ph, g) in enumerate(((0.0, 1.0), (2.1, 0.8), (4.2, 0.9))):
        a = ang + ph - (r / 62.0)
        band = np.exp(-(np.sin(a * 0.5) / 0.10) ** 2) * np.clip((r - 120) / 20, 0, 1) * np.clip((245 - r) / 45, 0, 1)
        sw = sw + g * band
    sw = np.clip(sw, 0, 1.2)
    rgb += crimson[None, None] * (1.3 * _blur(sw, int(8 * s)))[..., None] + hot[None, None] * (0.9 * sw ** 2)[..., None]
    alpha = np.maximum(alpha, np.clip(_blur(sw, int(6 * s)) * 1.4, 0, 1))
    # the outer glow
    glow = np.exp(-((r - R) / 40) ** 2)
    rgb += crimson[None, None] * (0.9 * glow)[..., None]
    alpha = np.maximum(alpha, np.clip(glow * 0.95, 0, 1))
    # the ball: dark throat, lit from the upper left, deep red at the rim
    ball = np.clip((R - r) / 2, 0, 1)
    nz = np.sqrt(np.clip(1 - (r / R) ** 2, 0, 1))
    throat = np.array([0.20, 0.0, 0.02])[None, None] * (1.5 * (r / R) ** 3)[..., None] + np.array([0.01, 0.0, 0.0])[None, None]
    rgb = rgb * (1 - ball[..., None]) + throat * ball[..., None]
    alpha = np.maximum(alpha, ball)
    # the fangs: long cones from the rim, shaded across, as a ring
    teeth = np.zeros((size, size), np.float32); shade = np.zeros((size, size), np.float32)
    for k, (aa, ln, w) in enumerate((zip(np.linspace(0, 2 * np.pi, 13)[:-1] + rng.uniform(-0.06, 0.06, 12), rng.uniform(42, 80, 12), rng.uniform(9, 13, 12)))):
        ca, sa = np.cos(aa), np.sin(aa)
        u = -(x * ca + y * sa) + R + 3          # 0 at the rim, towards the centre
        v = -x * sa + y * ca + 3 * np.sin(u / ln * 2.2)
        half = w * np.clip(1 - u / ln, 0, 1) ** 0.75
        m = np.clip((half - np.abs(v)) / 1.6, 0, 1) * (u > 0)
        sh = 0.45 + 0.65 * np.clip((v / (half + 0.5) + 1) / 2, 0, 1)
        teeth = np.where(m > teeth, m, teeth); shade = np.where(m >= teeth, sh * (1.15 - 0.3 * u / ln), shade)
    tcol = np.array([1.0, 0.92, 0.80])[None, None] * shade[..., None]
    rgb = rgb * (1 - teeth[..., None]) + tcol * teeth[..., None]
    rgb += crimson[None, None] * (0.7 * _blur(teeth, int(7 * s)) * (1 - teeth))[..., None]
    # the rim: wet, bright, with a highlight
    rim = np.exp(-((r - R) / 7) ** 2)
    light = np.clip(0.55 + 0.6 * (-(x + y) / (r + 1) * 0.707), 0.15, 1.2)
    rgb += crimson[None, None] * (1.3 * rim)[..., None] + white[None, None] * (0.95 * rim ** 3 * light ** 2)[..., None]
    # flares and motes
    for (fx, fy, fl) in ((-118, -112, 62), (128, 88, 44), (-40, 168, 30)):
        d, e = x - fx, y - fy
        f = np.exp(-(np.abs(d) / 2.2) - (e / fl) ** 2 * 1.2) + np.exp(-(np.abs(e) / 2.2) - (d / fl) ** 2 * 1.2)
        f = f * 1.0 + np.exp(-(np.hypot(d, e) / 11) ** 2)
        rgb += white[None, None] * (1.1 * f)[..., None]
        alpha = np.maximum(alpha, np.clip(f, 0, 1))
    for k in range(16):
        a0, d0 = rng.uniform(0, 6.283), rng.uniform(170, 238)
        g = np.exp(-(np.hypot(x - np.cos(a0) * d0, y - np.sin(a0) * d0) / rng.uniform(3, 6)) ** 2)
        rgb += hot[None, None] * (1.3 * g)[..., None]
        alpha = np.maximum(alpha, g)
    rgb = 1 - np.exp(-1.7 * np.clip(rgb, 0, None))
    return np.concatenate([rgb, np.clip(alpha, 0, 1)[..., None]], -1)


# ---------------------------------------------------------------- 05.11: in the style of the game's own icons
# no scene and a transparent ground; one big shaded subject with a bright coloured contour, strokes of light, white flares
def _st(size):
    s = size / 512.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    x, y = xx / s - 256, yy / s - 256
    return s, x, y, np.hypot(x, y), np.arctan2(y, x)


class _Art:
    def __init__(self, size):
        self.s, self.x, self.y, self.r, self.a = _st(size)
        self.rgb = np.zeros((size, size, 3), np.float32)
        self.al = np.zeros((size, size), np.float32)

    def glow(self, m, col, gain=1.0, alpha=1.0):
        self.rgb += np.array(col, np.float32)[None, None] * (gain * m)[..., None]
        self.al = np.maximum(self.al, np.clip(m * alpha, 0, 1))

    def paint(self, m, col):
        """col: colour or an image"""
        c = np.array(col, np.float32)
        c = c[None, None] if c.ndim == 1 else c
        self.rgb = self.rgb * (1 - m[..., None]) + c * m[..., None]
        self.al = np.maximum(self.al, m)

    def contour(self, m, col, wide=16, gain=1.5):
        """the bright contour of the game's icons around a shape"""
        b = _blur(m, max(1, int(wide * self.s)))
        self.glow(np.clip(b * (1 - m) * 2.4, 0, 1), col, gain, 1.0)

    def flare(self, fx, fy, ln, col=(1.0, 0.97, 0.92)):
        d, e = self.x - fx, self.y - fy
        f = np.exp(-(np.abs(d) / 2.2) - (e / ln) ** 2 * 1.2) + np.exp(-(np.abs(e) / 2.2) - (d / ln) ** 2 * 1.2) + np.exp(-(np.hypot(d, e) / (ln * 0.18)) ** 2)
        self.glow(np.clip(f, 0, 1.5), col, 1.1)

    def motes(self, rng, n, lo, hi, col):
        for k in range(n):
            a0, d0 = rng.uniform(0, 6.283), rng.uniform(lo, hi)
            self.glow(np.exp(-(np.hypot(self.x - np.cos(a0) * d0, self.y - np.sin(a0) * d0) / rng.uniform(3, 6)) ** 2), col, 1.2)

    def swirl(self, col, hot, lo=120, hi=245, turns=62.0, arms=3, gain=1.3):
        sw = 0
        for k in range(arms):
            a = self.a + k * 6.283 / arms - self.r / turns
            sw = sw + np.exp(-(np.sin(a * 0.5) / 0.10) ** 2) * np.clip((self.r - lo) / 20, 0, 1) * np.clip((hi - self.r) / 45, 0, 1)
        sw = np.clip(sw, 0, 1.2)
        self.glow(_blur(sw, max(1, int(8 * self.s))), col, gain, 1.4)
        self.glow(sw ** 2, hot, 0.9)

    def done(self, exposure=1.7):
        rgb = 1 - np.exp(-exposure * np.clip(self.rgb, 0, None))
        return np.concatenate([rgb, np.clip(self.al, 0, 1)[..., None]], -1)


def _seg(x, y, a, b):
    ax, ay, bx, by = a[0], a[1], b[0], b[1]
    t = np.clip(((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2 + 1e-6), 0, 1)
    return np.hypot(x - ax - t * (bx - ax), y - ay - t * (by - ay))


def _facets(art, pts, tip_col, dark_col):
    """a shaded crystal: a polygon, light on one side of its long axis, dark on the other"""
    m = _polygon(art.x, art.y, pts)
    a, b = np.array(pts[0], float), np.array(pts[len(pts) // 2], float)
    d = (b - a) / np.linalg.norm(b - a)
    side = -(art.x - a[0]) * d[1] + (art.y - a[1]) * d[0]
    t = np.clip((side + 10) / 20, 0, 1)[..., None]
    return m, np.array(tip_col, np.float32)[None, None] * t + np.array(dark_col, np.float32)[None, None] * (1 - t)


def ice_bolt_st(size=512, seed=6001):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    cyan, pale = (0.15, 0.75, 1.0), (0.75, 0.95, 1.0)
    # the frost trail: strokes from the lower left
    for k, (off, w) in enumerate(((0, 26), (-42, 13), (44, 14))):
        u, v = (A.x - A.y) * 0.707, (A.x + A.y) * 0.707 + off
        A.glow(np.exp(-(v / (w * np.clip((u + 240) / 200, 0.1, 1.0))) ** 2) * np.clip((u + 245) / 60, 0, 1) * np.clip((40 - u) / 30, 0, 1), cyan, 0.9, 0.9)
    # the bolt: three blades
    m0 = np.zeros_like(A.al)
    parts = []
    for da, ln, w in ((-0.58, 175, 42), (0.58, 170, 42), (0.0, 300, 62)):
        a = -0.785 + da
        ux, uy = np.cos(a), np.sin(a)
        vx, vy = -uy, ux
        c = np.array((-30.0, 30.0))
        tip = c + (ux * ln, uy * ln)
        pts = [tuple(tip), tuple(c + (vx * w + ux * ln * 0.25, vy * w + uy * ln * 0.25)), tuple(c - (ux * 40, uy * 40)), tuple(c - (vx * w - ux * ln * 0.25, vy * w - uy * ln * 0.25))]
        parts.append(_facets(A, pts, (0.92, 0.99, 1.0), (0.10, 0.45, 0.85)))
        m0 = np.maximum(m0, parts[-1][0])
    A.contour(m0, cyan, 20, 1.7)
    for m, col in parts:
        A.paint(m, col)
    A.glow(np.exp(-(np.hypot(A.x + 30, A.y - 30) / 44) ** 2), pale, 1.3)
    A.flare(182, -182, 62)
    A.flare(-72, -30, 26)
    A.motes(rng, 22, 90, 240, pale)
    return A.done()


def frost_nova_st(size=512, seed=6002):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    cyan, pale = (0.12, 0.72, 1.0), (0.8, 0.96, 1.0)
    ring = np.exp(-((A.r - 205) / 13) ** 2)
    A.glow(_blur(ring, max(1, int(10 * A.s))), cyan, 1.4, 1.5)
    A.glow(ring ** 2, pale, 1.0)
    A.glow(np.exp(-(A.r / 165) ** 2), (0.05, 0.35, 0.85), 0.8, 0.85)
    # a big snowflake
    seg = np.mod(A.a + np.pi / 6, np.pi / 3) - np.pi / 6
    u, v = A.r * np.cos(seg), np.abs(A.r * np.sin(seg))
    m = np.clip((np.clip(1 - u / 195, 0, 1) * 15 + 3 - v) / 2, 0, 1) * (u < 195)
    for at, ln in ((70, 62), (118, 50), (156, 30)):
        al, ac = (u - at) * 0.5 + v * 0.866, -(u - at) * 0.866 + v * 0.5
        m = np.maximum(m, np.clip((np.clip(1 - al / ln, 0, 1) * 9 + 1.5 - np.abs(ac)) / 2, 0, 1) * (al > 0) * (al < ln))
    A.contour(m, cyan, 10, 1.4)
    shade = np.clip(0.75 + 0.4 * (-(A.x + A.y) / (A.r + 1)), 0.45, 1.2)
    A.paint(m, (np.array((0.92, 0.99, 1.0), np.float32)[None, None] * shade[..., None]))
    A.glow(np.exp(-(A.r / 26) ** 2), (1, 1, 1), 1.4)
    # ice spikes bursting outward from the ring
    for k in range(12):
        a = k * 6.283 / 12 + 0.26
        c, sn = np.cos(a), np.sin(a)
        pts = [(c * 250, sn * 250), (c * 192 - sn * 13, sn * 192 + c * 13), (c * 192 + sn * 13, sn * 192 - c * 13)]
        mk, col = _facets(A, pts, (0.9, 0.98, 1.0), (0.2, 0.55, 0.95))
        A.paint(mk, col)
    A.flare(-140, -142, 50)
    A.flare(150, 118, 34)
    A.motes(rng, 16, 120, 240, pale)
    return A.done()


def _jag(rng, a, b, n=9, amp=26):
    a, b = np.array(a, float), np.array(b, float)
    d = b - a
    nrm = np.array((-d[1], d[0])) / (np.linalg.norm(d) + 1e-6)
    pts = [a + d * k / n + nrm * (rng.uniform(-amp, amp) if 0 < k < n else 0) for k in range(n + 1)]
    return pts


def chain_lightning_st(size=512, seed=6003):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    violet, pale = (0.55, 0.25, 1.0), (0.85, 0.85, 1.0)
    nodes = [(-160, -150), (135, -70), (-70, 150)]
    d = 1e9 * np.ones_like(A.x)
    thin = 1e9 * np.ones_like(A.x)
    for a, b, n in ((nodes[0], nodes[1], 8), (nodes[1], nodes[2], 8), (nodes[1], (225, 120), 5), (nodes[0], (-235, 60), 5), (nodes[2], (120, 225), 5)):
        pts = _jag(rng, a, b, n)
        for p, q in zip(pts, pts[1:]):
            dd = _seg(A.x, A.y, p, q)
            if n == 8:
                d = np.minimum(d, dd)
            else:
                thin = np.minimum(thin, dd)
    A.glow(np.exp(-(d / 34) ** 2), violet, 1.5, 1.2)
    A.glow(np.exp(-(thin / 18) ** 2), violet, 1.0, 1.0)
    A.glow(np.exp(-(d / 8) ** 2), (1, 1, 1), 2.4)
    A.glow(np.exp(-(thin / 4.5) ** 2), pale, 1.6)
    for n in nodes:
        r = np.hypot(A.x - n[0], A.y - n[1])
        A.glow(np.exp(-(r / 62) ** 2), violet, 1.5, 1.1)
        ball = np.clip((36 - r) / 2, 0, 1)
        lit = np.clip(1.25 - np.hypot(A.x - n[0] + 11, A.y - n[1] + 11) / 44, 0.25, 1.3)
        A.paint(ball, np.array((0.8, 0.72, 1.0), np.float32)[None, None] * lit[..., None])
        A.flare(n[0], n[1], 58)
    A.motes(rng, 18, 60, 245, pale)
    return A.done()


def arcane_push_st(size=512, seed=6004):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    violet, pale = (0.50, 0.22, 1.0), (0.85, 0.75, 1.0)
    # rings of force, each broken, thicker going out
    for R, w, ph, gain in ((205, 15, 0.4, 1.2), (150, 11, 2.0, 1.1), (98, 8, 3.7, 1.0)):
        arc = np.clip(np.cos((A.a + ph) * 3) * 2.2 + 1.2, 0, 1)
        m = np.exp(-((A.r - R) / w) ** 2) * arc
        A.glow(_blur(m, max(1, int(9 * A.s))), violet, 1.5 * gain, 1.5)
        A.glow(m ** 2, pale, 1.1 * gain)
    # rays thrown out
    rays = np.clip(np.cos(A.a * 8 + 0.5), 0, 1) ** 28 * np.clip((A.r - 45) / 30, 0, 1) * np.clip((248 - A.r) / 90, 0, 1)
    A.glow(rays, pale, 1.3, 1.0)
    # the core: a shaded sphere
    A.glow(np.exp(-(A.r / 88) ** 2), violet, 1.6, 1.1)
    ball = np.clip((48 - A.r) / 2, 0, 1)
    lit = np.clip(1.3 - np.hypot(A.x + 15, A.y + 15) / 58, 0.3, 1.4)
    A.paint(ball, np.array((0.9, 0.82, 1.0), np.float32)[None, None] * lit[..., None])
    A.flare(-16, -16, 76)
    A.flare(160, -128, 34)
    A.motes(rng, 20, 90, 245, pale)
    return A.done()


def arcane_shield_st(size=512, seed=6005):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    violet, blue, pale = (0.45, 0.22, 1.0), (0.25, 0.55, 1.0), (0.85, 0.88, 1.0)
    A.swirl(violet, pale, lo=150, hi=250, turns=75.0, arms=2, gain=1.0)
    top, bot, half = -185.0, 205.0, 165.0
    pts = []
    for t in np.linspace(0, 1, 26):
        w = half * (1.0 if t < 0.42 else np.sqrt(max(0.0, 1 - ((t - 0.42) / 0.58) ** 2)))
        pts.append((w, top + (bot - top) * t))
    pts += [(-a, b) for a, b in reversed(pts)]
    body = _polygon(A.x, A.y, pts)
    inner = _polygon(A.x, A.y, [(a * 0.84, (b + 0) * 0.85 + 3) for a, b in pts])
    A.contour(body, violet, 22, 1.8)
    # steel-glass, lit from the upper left, halves of different shade
    h = np.clip((A.y - top) / (bot - top), 0, 1)
    shade = (0.95 - 0.55 * h) * np.where(A.x < 0, 1.0, 0.62)
    A.paint(body, np.array((0.40, 0.36, 0.95), np.float32)[None, None] * shade[..., None])
    rim = np.clip(body - inner, 0, 1)
    A.paint(rim, np.array((0.80, 0.82, 1.0), np.float32)[None, None] * np.clip(1.05 - 0.55 * h + 0.25 * (A.x < 0), 0.3, 1.3)[..., None])
    A.glow(np.exp(-(A.x / 4.5) ** 2) * inner, pale, 0.5)
    A.glow(np.clip(np.exp(-((A.y + 120 + A.x * 0.45) / 22) ** 2) * inner, 0, 1), (1, 1, 1), 0.55)
    A.flare(-105, -160, 66)
    A.flare(128, 96, 30)
    A.motes(rng, 16, 150, 245, pale)
    return A.done()


def spell_critical_st(size=512, seed=6006):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    violet, gold, pale = (0.55, 0.20, 1.0), (1.0, 0.68, 0.15), (1.0, 0.95, 0.75)
    A.glow(np.exp(-((A.r - 138) / 42) ** 2), violet, 1.1, 0.95)
    ball = np.clip((135 - A.r) / 2, 0, 1)
    lit = np.clip(1.15 - np.hypot(A.x + 42, A.y + 46) / 165, 0.08, 1.2)
    A.paint(ball, np.array((0.42, 0.20, 0.95), np.float32)[None, None] * lit[..., None])
    # cracks from the middle
    cr = 1e9 * np.ones_like(A.x)
    for k in range(9):
        a = k * 6.283 / 9 + rng.uniform(-0.2, 0.2)
        for p, q in zip(*(lambda P: (P, P[1:]))(_jag(rng, (0, 0), (np.cos(a) * 138, np.sin(a) * 138), 5, 13))):
            cr = np.minimum(cr, _seg(A.x, A.y, p, q))
    A.glow(np.exp(-(cr / 8) ** 2) * ball, gold, 1.4)
    A.glow(np.exp(-(cr / 2.5) ** 2) * ball, pale, 1.3)
    A.glow(np.exp(-((A.r - 135) / 6) ** 2), pale, 0.9)
    # the star bursting out
    star = np.exp(-(np.abs(A.x) / (14 * np.clip(1 - np.abs(A.y) / 252, 0, 1) + 1.5))) * (np.abs(A.y) < 252) + np.exp(-(np.abs(A.y) / (14 * np.clip(1 - np.abs(A.x) / 252, 0, 1) + 1.5))) * (np.abs(A.x) < 252)
    A.glow(_blur(np.clip(star, 0, 1), max(1, int(6 * A.s))), gold, 1.5, 1.3)
    A.glow(star, (1, 1, 1), 1.6, 1.0)
    A.glow(np.exp(-(A.r / 52) ** 2), (1, 1, 1), 1.8)
    A.flare(-128, -128, 42, pale)
    A.flare(128, 128, 34, pale)
    A.motes(rng, 20, 120, 240, (1.0, 0.8, 0.35))
    return A.done()


def hunters_mark_st(size=512, seed=6007):
    rng = np.random.default_rng(seed)
    A = _Art(size)
    red, hot = (1.0, 0.12, 0.06), (1.0, 0.65, 0.45)
    # the sight: a bright ring and four blades
    ring = np.exp(-((A.r - 175) / 11) ** 2)
    A.glow(_blur(ring, max(1, int(11 * A.s))), red, 1.6, 1.5)
    A.glow(ring ** 2, hot, 1.2)
    inner = np.exp(-((A.r - 82) / 6) ** 2)
    A.glow(_blur(inner, max(1, int(7 * A.s))), red, 1.1, 1.3)
    A.glow(inner ** 2, hot, 1.0)
    for a in (0.0, 1.5708, 3.1416, -1.5708):
        c, sn = np.cos(a), np.sin(a)
        pts = [(c * 100, sn * 100), (c * 215 - sn * 19, sn * 215 + c * 19), (c * 248, sn * 248), (c * 215 + sn * 19, sn * 215 - c * 19)]
        mk, col = _facets(A, pts, (1.0, 0.86, 0.72), (0.60, 0.05, 0.03))
        A.contour(mk, red, 9, 1.3)
        A.paint(mk, col)
    A.glow(np.exp(-(A.r / 60) ** 2), red, 1.3, 1.0)
    # the arrow through it
    u, v = (A.x - A.y) * 0.707, (A.x + A.y) * 0.707
    shaft = np.clip((6.5 - np.abs(v)) / 1.5, 0, 1) * (u > -232) * (u < 145)
    head = _polygon(A.x, A.y, [(195 * 0.707 + 0, -195 * 0.707), (130 * 0.707 + 30 * 0.707, -130 * 0.707 + 30 * 0.707), (130 * 0.707 - 30 * 0.707, -130 * 0.707 - 30 * 0.707)])
    fl = np.clip((26 - np.abs(v)) / 1.5, 0, 1) * (u > -240) * (u < -172) * (np.mod(u / 12, 1.0) < 0.7)
    all_ = np.clip(shaft + head + fl, 0, 1)
    A.contour(all_, red, 8, 1.2)
    A.paint(shaft, np.array((0.65, 0.42, 0.22), np.float32)[None, None] * (0.7 + 0.6 * (v < 0))[..., None])
    A.paint(fl, (0.95, 0.92, 0.85))
    A.paint(head, np.array((0.95, 0.95, 0.95), np.float32)[None, None] * (0.6 + 0.6 * (v < 0))[..., None])
    A.flare(0, 0, 60)
    A.flare(138, -138, 36)
    A.motes(rng, 16, 130, 245, hot)
    return A.done()


def second_wind_st(size=512, seed=6008):
    """as the game's Regenerate: a shaded heart in a whirl of light, in gold"""
    rng = np.random.default_rng(seed)
    A = _Art(size)
    gold, pale = (1.0, 0.62, 0.12), (1.0, 0.93, 0.70)
    A.swirl(gold, pale, lo=115, hi=250, turns=58.0, arms=3, gain=1.5)
    dx, dy = A.x / 118, (15 - A.y) / 118
    f = (dx ** 2 + dy ** 2 - 1) ** 3 - dx ** 2 * dy ** 3
    heart = np.clip(-f * 7, 0, 1)
    A.contour(heart, gold, 22, 1.9)
    lit = np.clip(1.15 - (A.y + 90) / 300 - A.x / 700, 0.35, 1.2)
    A.paint(heart, np.array((1.0, 0.30, 0.10), np.float32)[None, None] * lit[..., None])
    for cx in (-52, 52):
        A.glow(np.exp(-((A.x - cx - 8) ** 2 / 900 + (A.y + 58) ** 2 / 500)) * heart, (1, 0.8, 0.6), 0.75)
    A.glow(np.exp(-(f / 0.05) ** 2), pale, 0.75)
    A.flare(-100, -95, 56)
    A.flare(122, 72, 32)
    A.motes(rng, 20, 130, 245, pale)
    return A.done()


ARTS = {'pack_hunger': pack_hunger, 'second_wind': second_wind, 'hunters_mark': hunters_mark, 'spell_critical': spell_critical, 'arcane_shield': arcane_shield_new, 'arcane_push': arcane_push, 'ice_bolt': ice_bolt_new, 'frost_nova': frost_nova, 'chain_lightning': chain_lightning, 'arcane_blink': arcane_blink}
ARTS.update({'ice_bolt': ice_bolt_st, 'frost_nova': frost_nova_st, 'chain_lightning': chain_lightning_st, 'arcane_push': arcane_push_st, 'arcane_shield': arcane_shield_st,
             'spell_critical': spell_critical_st, 'hunters_mark': hunters_mark_st, 'second_wind': second_wind_st})


def button_states(art, size=512):
    """normal / hilite / pressed as uint8 RGBA (hilite +20 %, pressed -20 % of the colour, as the game's buttons)."""
    base = ARTS[art](size)
    out = {}
    for name, f in (('normal', 1.0), ('hilite', 1.2), ('pressed', 0.8)):
        img = base.copy()
        img[..., :3] = np.clip(img[..., :3] * f, 0, 1)
        out[name] = (img * 255 + 0.5).astype(np.uint8)
    return out


def _picture(name, scale=1.0):
    """05.11: a finished picture, assets/icons/<name>.png (RGBA, filter 0, 512x512): made with an image generator from the user's prompts (docs/ICON_PROMPTS.md)"""
    import os, struct, zlib
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'icons', name + '.png')

    def art(size=512):
        d = open(path, 'rb').read()
        w, h = struct.unpack('>II', d[16:24])
        p, idat = 8, b''
        while p < len(d):
            n = struct.unpack('>I', d[p:p + 4])[0]
            if d[p + 4:p + 8] == b'IDAT':
                idat += d[p + 8:p + 8 + n]
            p += 12 + n
        raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, w * 4 + 1)
        if (raw[:, 0] != 0).any():
            raise SystemExit(path + ': only filter 0 is read')
        img = raw[:, 1:].reshape(h, w, 4).astype(np.float32) / 255.0
        if scale != 1.0:                                  # smaller in the middle of the same square: bilinear, nothing outside
            yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
            u, v = (xx - w / 2 + 0.5) / scale + w / 2 - 0.5, (yy - h / 2 + 0.5) / scale + h / 2 - 0.5
            inside = ((u >= 0) & (u <= w - 1) & (v >= 0) & (v <= h - 1))[..., None]
            u0, v0 = np.clip(u.astype(int), 0, w - 2), np.clip(v.astype(int), 0, h - 2)
            fu, fv = np.clip(u - u0, 0, 1)[..., None], np.clip(v - v0, 0, 1)[..., None]
            img = (img[v0, u0] * (1 - fu) * (1 - fv) + img[v0, u0 + 1] * fu * (1 - fv) + img[v0 + 1, u0] * (1 - fu) * fv + img[v0 + 1, u0 + 1] * fu * fv) * inside
        if size > w and size % w == 0:                    # the game's own smaller pictures (WeaponExpertiseBow)
            k = size // w
            yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
            u, v = np.clip((xx + 0.5) / k - 0.5, 0, w - 1.001), np.clip((yy + 0.5) / k - 0.5, 0, h - 1.001)
            u0, v0 = u.astype(int), v.astype(int)
            fu, fv = (u - u0)[..., None], (v - v0)[..., None]
            img = img[v0, u0] * (1 - fu) * (1 - fv) + img[v0, u0 + 1] * fu * (1 - fv) + img[v0 + 1, u0] * (1 - fu) * fv + img[v0 + 1, u0 + 1] * fu * fv
            w = size
        if size != w:
            k = w // size
            if k * size != w:
                raise SystemExit('%s: %d is not a multiple of %d' % (path, w, size))
            img = img.reshape(size, k, size, k, 4).mean((1, 3))
        return img
    return art


for _n in ('ice_bolt', 'frost_nova', 'chain_lightning', 'arcane_push', 'arcane_shield', 'spell_critical', 'hunters_mark', 'second_wind', 'pack_hunger', 'holy_smite', 'blessing', 'weapon_expertise_bow', 'ice_arrow', 'aimed_shot', 'arrow_volley', 'armor_breaker', 'battle_cry', 'ram_charge', 'blood_bond', 'summon_explosion', 'call_of_vengeance', 'eloquence'):
    ARTS[_n] = _picture(_n, 0.84 if _n == 'arcane_shield' else 1.0)   # 05.11 the user: the shield did not fit


def drain_st(size=512, seed=6009):
    """06.11: until the user's picture comes (assets/icons/drain.png takes over): a red heart in a whirl of sickly green"""
    rng = np.random.default_rng(seed)
    A = _Art(size)
    green, pale = (0.35, 0.9, 0.12), (0.8, 1.0, 0.5)
    A.swirl(green, pale, lo=115, hi=250, turns=58.0, arms=3, gain=1.5)
    dx, dy = A.x / 118, (15 - A.y) / 118
    f = (dx ** 2 + dy ** 2 - 1) ** 3 - dx ** 2 * dy ** 3
    heart = np.clip(-f * 7, 0, 1)
    A.contour(heart, green, 22, 1.8)
    lit = np.clip(1.15 - (A.y + 90) / 300 - A.x / 700, 0.3, 1.2)
    A.paint(heart, np.array((0.65, 0.04, 0.03), np.float32)[None, None] * lit[..., None])
    A.motes(rng, 20, 130, 245, pale)
    return A.done()


ARTS['drain'] = _picture('drain')


def contagious_curse_st(size=512, seed=7111):
    """07.11: until a picture comes (assets/icons/contagious_curse.png takes over): a dark skull-like orb in a toxic purple
    whirl, with three small purple orbs flying off it (the curse jumping on)"""
    rng = np.random.default_rng(seed)
    A = _Art(size)
    purple, pale = (0.55, 0.12, 0.85), (0.85, 0.6, 1.0)
    A.swirl(purple, pale, lo=110, hi=250, turns=52.0, arms=3, gain=1.4)
    orb = np.clip((95 - np.sqrt(A.x ** 2 + (A.y + 10) ** 2)) / 6, 0, 1)
    A.contour(orb, purple, 20, 1.6)
    lit = np.clip(1.1 - (A.y + 80) / 280 - A.x / 650, 0.25, 1.1)
    A.paint(orb, np.array((0.12, 0.05, 0.16), np.float32)[None, None] * lit[..., None])
    for ex, ey in ((-34, -20), (34, -20)):                  # the eye holes, glowing
        eye = np.clip((17 - np.sqrt((A.x - ex) ** 2 + (A.y - ey) ** 2)) / 4, 0, 1)
        A.paint(eye, np.array((0.75, 0.35, 1.0), np.float32)[None, None])
    for cx, cy, r in ((150, -140, 30), (-165, -95, 24), (120, 165, 26)):   # the curse flying on
        o = np.clip((r - np.sqrt((A.x - cx) ** 2 + (A.y - cy) ** 2)) / 4, 0, 1)
        A.contour(o, purple, 10, 1.4)
        A.paint(o, np.array((0.5, 0.15, 0.75), np.float32)[None, None])
    A.motes(rng, 24, 120, 245, pale)
    return A.done()


def detective_st(size=512, seed=7115):
    """07.11: until a picture comes (assets/icons/detective.png takes over): a magnifying glass over a heap of gold coins"""
    rng = np.random.default_rng(seed)
    A = _Art(size)
    gold, pale = (1.0, 0.72, 0.15), (1.0, 0.93, 0.6)
    A.swirl(gold, pale, lo=150, hi=250, turns=40.0, arms=2, gain=0.9)
    for cx, cy, r in ((-60, 120, 62), (40, 140, 58), (120, 95, 52), (-130, 60, 48), (-20, 60, 55)):   # the coins
        c = np.clip((r - np.sqrt((A.x - cx) ** 2 + ((A.y - cy) * 1.8) ** 2)) / 4, 0, 1)
        A.contour(c, gold, 8, 1.2)
        A.paint(c, np.array((0.85, 0.6, 0.12), np.float32)[None, None] * np.clip(1.2 - (A.y - cy) / 120, 0.6, 1.3)[..., None])
    ring = np.sqrt((A.x + 25) ** 2 + (A.y + 55) ** 2)
    lens = np.clip((110 - ring) / 5, 0, 1)
    A.paint(lens * 0.55, np.array((0.55, 0.75, 0.9), np.float32)[None, None])
    rim = np.clip((14 - np.abs(ring - 118)) / 3, 0, 1)
    A.contour(rim, gold, 14, 1.4)
    A.paint(rim, np.array((0.95, 0.8, 0.4), np.float32)[None, None])
    u = (A.x + A.y + 30) / np.sqrt(2.0); v = (A.x - A.y - 80) / np.sqrt(2.0)   # the handle, down to the right
    handle = np.clip((22 - np.abs(v - 0)) / 3, 0, 1) * np.clip((u - 135) / 3, 0, 1) * np.clip((330 - u) / 3, 0, 1)
    handle *= (A.x > 40)
    A.paint(handle, np.array((0.35, 0.18, 0.08), np.float32)[None, None])
    A.motes(rng, 16, 120, 240, pale)
    return A.done()


import os as _os
ARTS['detective'] = (_picture('detective') if _os.path.isfile(_os.path.join(
    _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))), 'assets', 'icons', 'detective.png')) else detective_st)
ARTS['contagious_curse'] = (_picture('contagious_curse') if _os.path.isfile(_os.path.join(
    _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))), 'assets', 'icons', 'contagious_curse.png')) else contagious_curse_st)


# ---------------------------------------------------------------- DXT5
def _to565(c):
    c = c.astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255 << 11) | ((c[..., 1] * 63 + 127) // 255 << 5) | ((c[..., 2] * 31 + 127) // 255)


def _from565(v):
    return np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], -1).astype(np.int32)


def dxt5(img):
    """uint8 RGBA (h, w multiples of 4, or 1/2) -> DXT5 bytes. Colour: endpoints from the extremes along the block's
    main axis (max - min corner), 4-colour mode (always in BC3); alpha: 8-level mode between the block's min and max."""
    h, w = img.shape[:2]
    if h < 4 or w < 4:
        img = np.pad(img, ((0, max(0, 4 - h)), (0, max(0, 4 - w)), (0, 0)), mode='edge')
        h, w = img.shape[:2]
    blocks = img.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.int32)
    col, a = blocks[..., :3], blocks[..., 3]
    # alpha block
    a0, a1 = a.max(1), a.min(1)
    pal = np.empty((len(a), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    for i in range(1, 7):
        pal[:, i + 1] = ((7 - i) * a0 + i * a1) // 7
    aidx = np.abs(a[:, :, None] - pal[:, None, :]).argmin(2).astype(np.uint64)
    abits = np.zeros(len(a), np.uint64)
    for p in range(16):
        abits |= aidx[:, p] << np.uint64(3 * p)
    # colour block
    lo, hi = col.min(1), col.max(1)
    axis = (hi - lo).astype(np.float32)
    proj = (col * axis[:, None, :]).sum(2)
    cmax = col[np.arange(len(col)), proj.argmax(1)]
    cmin = col[np.arange(len(col)), proj.argmin(1)]
    e0, e1 = _to565(cmax), _to565(cmin)
    swap = e0 < e1
    e0, e1 = np.where(swap, e1, e0), np.where(swap, e0, e1)
    same = e0 == e1
    e0 = np.where(same & (e0 < 0xFFFF), e0 + 1, e0)       # keep 4-colour order (c0 > c1)
    e1 = np.where(same & (e0 == e1), e1 - 1, e1)
    c0, c1 = _from565(e0), _from565(e1)
    cp = np.stack([c0, c1, (2 * c0 + c1) // 3, (c0 + 2 * c1) // 3], 1)
    cidx = ((col[:, :, None, :] - cp[:, None, :, :]) ** 2).sum(3).argmin(2).astype(np.uint32)
    cbits = np.zeros(len(col), np.uint32)
    for p in range(16):
        cbits |= cidx[:, p] << np.uint32(2 * p)
    out = bytearray()
    for i in range(len(col)):
        out += struct.pack('<BB', a0[i], a1[i]) + int(abits[i]).to_bytes(6, 'little')
        out += struct.pack('<HHI', int(e0[i]), int(e1[i]), int(cbits[i]))
    return bytes(out)


def mip_chain(img, levels):
    """DXT5 of every mip level (box filter), largest first."""
    data, cur = [], img.astype(np.float32)
    for _ in range(levels):
        data.append(dxt5((cur + 0.5).astype(np.uint8)))
        h, w = cur.shape[:2]
        if h > 1 and w > 1:
            cur = cur.reshape(h // 2, 2, w // 2, 2, 4).mean((1, 3))
    return b''.join(data)


def main(argv):
    if len(argv) >= 3 and argv[1] == 'preview':
        sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.dirname(
            __import__('os').path.dirname(__import__('os').path.abspath(__file__)))), 'tools', 'reborn'))
        from fx_select import sheet
        states = button_states(argv[3] if len(argv) > 3 else 'ice_bolt')
        sheet(argv[2], [(k, v) for k, v in states.items()], tile=512, cols=3)
        print('preview ->', argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
