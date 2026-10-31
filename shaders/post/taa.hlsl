// Divinity II Reborn: temporal anti-aliasing that DXVK patch 0012 (d3d9.postTAA) runs right after SMAA, i.e. on the
// final scene before the interface. Compiled by tools/shaders/build_post.py to taa.vs.dxso / taa.ps.dxso.
//
// While TAA is on, DXVK shifts every depth-tested draw into the scene-sized targets by a sub-pixel offset (Halton 2,3,
// 8 frames), so the frames sample different points of each pixel; this pass accumulates them. The engine writes no
// motion vectors: the previous position of a pixel comes from its depth in the engine's depth + normal buffer and the
// camera of both frames, which DXVK reads at the light pre-pass draw (world-space rays of the full-screen quad and the
// view matrix), see docs/TAA.md. Pixels that moved on their own (characters, animated objects) are caught by the
// depth test against the previous frame's buffer and by clipping the history to the colour range of the current
// neighbourhood, so they take the current frame (with SMAA) instead of smearing.
// Clipping alone would erase details thinner than a pixel (ropes, poles, branches): they are hit only by some of the
// jittered samples, and in the other frames the neighbourhood has no trace of them. A pixel that is brighter or darker
// than both neighbours in a row or column "locks" its history for one jitter cycle (lock texture: remaining time and
// the depth of the thin detail, carried along with the reprojection): while locked, the depth test passes and the
// surface seen now is not in front of the locked detail (something moving across it), the history is not clipped.
//
//   s0 = current frame after SMAA (point)       s1 = history (A16B16G16R16F, linear)
//   s2 = depth + normal buffer of this frame     s3 = depth + normal buffer of the previous frame (both point)
//   s4 = locks of the previous frame (G16R16F, point)
//   oC0 -> the engine's render target, oC1 -> the next history, oC2 -> the next locks
// c0 = (1/width, 1/height, width, height)
// c1 = eye - previous eye (world)
// c2, c3, c4 = ray(ndc) = c2 + ndc.x * c3 + ndc.y * c4: world-space ray from the eye to the far plane of this frame
// c5, c6, c7 = rows of the inverse of the previous frame's [ray0 rayX rayY] (columns): q = (t, t * ndc'.x, t * ndc'.y)
// c8 = (history weight at rest, history weight in fast motion, 1 = no history (first frame, camera cut), depth tolerance)
// c9 = (jitter of this frame in pixels: the scene was drawn shifted right / down by this, jitter of the previous frame)

float4 RtMetrics   : register(c0);
float3 EyeDelta    : register(c1);
float3 Ray0        : register(c2);
float3 RayX        : register(c3);
float3 RayY        : register(c4);
float3 PrevInv0    : register(c5);
float3 PrevInv1    : register(c6);
float3 PrevInv2    : register(c7);
float4 Params      : register(c8);
float4 Jitter      : register(c9);

sampler2D Current     : register(s0);
sampler2D History     : register(s1);
sampler2D GBuffer     : register(s2);
sampler2D PrevGBuffer : register(s3);
sampler2D Locks       : register(s4);

#define SKY_DEPTH        1000.0   // sky (depth 0 in the buffer) counts as farther than anything
#define CLIP_GAMMA       1.0      // history is clipped to mean +- gamma * deviation of the 3x3 neighbourhood
#define MOTION_PIXELS    2.0      // motion in pixels at which the history weight reaches Params.y
#define LOCK_THRESHOLD   0.04     // luma step that makes a pixel a thin feature (brighter or darker than both neighbours)
#define LOCK_FRAMES      8.0      // a lock outlives one cycle of the jitter
#define LOCK_DEPTH_TOL   0.05     // relative: a surface this much closer than the locked detail covers it
#define HELD_MIN_MOTION  2.0      // pixels the world must move before "carried by the camera" is considered

// Same as the SMAA passes: pixel centres on texel centres under the Direct3D 9 rules
float4 ClipPosition(float4 pos)
{
    return float4(pos.xy + float2(-1.0, 1.0) * RtMetrics.xy, pos.zw);
}

void TaaVS(float4 pos : POSITION, float2 uv : TEXCOORD0, out float4 oPos : POSITION, out float2 oUv : TEXCOORD0)
{
    oPos = ClipPosition(pos);
    oUv = uv;
}

float Depth(float4 g)
{
    return (g.x + g.y) * 1.52590219e-005;
}

float DepthOrSky(float4 g)
{
    float d = Depth(g);
    return d > 0 ? d : SKY_DEPTH;
}

float3 ToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 FromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Catmull-Rom history sample from 5 bilinear taps (the corner taps of the 4x4 kernel are dropped)
float3 SampleHistory(float2 uv)
{
    float2 pos = uv * RtMetrics.zw;
    float2 centre = floor(pos - 0.5) + 0.5;
    float2 f = pos - centre;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0 = (centre - 1.0) * RtMetrics.xy;
    float2 tc3 = (centre + 2.0) * RtMetrics.xy;
    float2 tc12 = (centre + w2 / w12) * RtMetrics.xy;

    float3 c = tex2Dlod(History, float4(tc12.x, tc0.y, 0, 0)).rgb * (w12.x * w0.y)
             + tex2Dlod(History, float4(tc0.x, tc12.y, 0, 0)).rgb * (w0.x * w12.y)
             + tex2Dlod(History, float4(tc12.x, tc12.y, 0, 0)).rgb * (w12.x * w12.y)
             + tex2Dlod(History, float4(tc3.x, tc12.y, 0, 0)).rgb * (w3.x * w12.y)
             + tex2Dlod(History, float4(tc12.x, tc3.y, 0, 0)).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / weight, 0.0);
}

float3 ClipToBox(float3 lo, float3 hi, float3 p)
{
    float3 centre = 0.5 * (hi + lo);
    float3 extent = 0.5 * (hi - lo) + 1e-5;
    float3 v = p - centre;
    float3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : p;
}

struct TaaOut
{
    float4 target  : COLOR0;
    float4 history : COLOR1;
    float4 locks   : COLOR2;
};

// Whether the previous frame's depth buffer has a surface at the expected depth within the 2x2 texels around a
// position (pixels, centres on integers). The slack covers the depth change across a pixel (ground seen at a grazing
// angle changes depth by tens of percent from one row to the next); sky must stay sky.
bool PrevDepthMatches(float2 pixel, float expected, float slack)
{
    float2 base = (floor(pixel) + 0.5) * RtMetrics.xy;
    float p0 = DepthOrSky(tex2Dlod(PrevGBuffer, float4(base, 0, 0)));
    float p1 = DepthOrSky(tex2Dlod(PrevGBuffer, float4(base + float2(RtMetrics.x, 0), 0, 0)));
    float p2 = DepthOrSky(tex2Dlod(PrevGBuffer, float4(base + float2(0, RtMetrics.y), 0, 0)));
    float p3 = DepthOrSky(tex2Dlod(PrevGBuffer, float4(base + RtMetrics.xy, 0, 0)));
    return expected >= min(min(p0, p1), min(p2, p3)) - slack && expected <= max(max(p0, p1), max(p2, p3)) + slack;
}

TaaOut TaaPS(float2 uv : TEXCOORD0)
{
    TaaOut o;
    float4 cur = tex2Dlod(Current, float4(uv, 0, 0));

    // 3x3 neighbourhood of the current frame: colour range for the history, the colour at the pixel centre
    // reconstructed from the jittered samples (sample of neighbour k lies at k - jitter; Gaussian fit of
    // Blackman-Harris, keeps the jitter from flickering in the result), and the depths
    float3 m1 = 0, m2 = 0, lo = 1e3, hi = -1e3, filtered = 0;
    float filterSum = 0;
    float luma[9], depth[9];
    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            float2 offset = float2(x, y) * RtMetrics.xy;
            float3 c = ToYCoCg(tex2Dlod(Current, float4(uv + offset, 0, 0)).rgb);
            m1 += c;
            m2 += c * c;
            lo = min(lo, c);
            hi = max(hi, c);
            luma[(y + 1) * 3 + x + 1] = c.x;
            depth[(y + 1) * 3 + x + 1] = DepthOrSky(tex2Dlod(GBuffer, float4(uv + offset, 0, 0)));
            float2 dist = float2(x, y) - Jitter.xy;
            float w = exp(-2.29 * dot(dist, dist));
            filtered += c * w;
            filterSum += w;
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float3 boxLo = max(lo, mean - CLIP_GAMMA * sigma);
    float3 boxHi = min(hi, mean + CLIP_GAMMA * sigma);
    float3 curY = filtered / filterSum;
    float closest = min(min(min(depth[0], depth[1]), min(depth[2], depth[3])),
                        min(min(depth[4], depth[5]), min(depth[6], min(depth[7], depth[8]))));
    float dHere = depth[4];

    // Thin geometry (rope, pole, branch): brighter or darker than both neighbours in a row or column AND in front of
    // both of them. Texture detail (runes, cracks) is flat in depth and gets no lock.
    float2 stepH = float2(luma[4] - luma[3], luma[4] - luma[5]);
    float2 stepV = float2(luma[4] - luma[1], luma[4] - luma[7]);
    bool thin = (stepH.x * stepH.y > 0 && min(abs(stepH.x), abs(stepH.y)) > LOCK_THRESHOLD
                 && dHere < min(depth[3], depth[5]) * (1.0 - LOCK_DEPTH_TOL))
             || (stepV.x * stepV.y > 0 && min(abs(stepV.x), abs(stepV.y)) > LOCK_THRESHOLD
                 && dHere < min(depth[1], depth[7]) * (1.0 - LOCK_DEPTH_TOL));

    // Candidate 1, the world: where the pixel was if it stood still, from the centre's ray at the closest depth of the
    // 3x3 neighbourhood (edges move with the object in front) and the previous camera's rays (sky: direction only)
    float2 ndc = float2(uv.x * 2.0 - 1.0 - RtMetrics.x, 1.0 - uv.y * 2.0 + RtMetrics.y);
    float3 ray = Ray0 + ndc.x * RayX + ndc.y * RayY;
    float3 v = closest < SKY_DEPTH ? EyeDelta + closest * ray : ray;
    float3 q = float3(dot(PrevInv0, v), dot(PrevInv1, v), dot(PrevInv2, v));
    float2 ndcPrev = q.yz / max(q.x, 1e-6);
    float2 uvPrev = float2((ndcPrev.x + 1.0 + RtMetrics.x) * 0.5, (1.0 + RtMetrics.y - ndcPrev.y) * 0.5);
    float slack = Params.w * dHere + abs(ddx(dHere)) + abs(ddy(dHere));

    // The surface point actually sampled here (jittered) must be in the previous depth buffer where that frame's
    // jitter put it; its previous depth scales like the closest one's
    float expectedWorld = dHere < SKY_DEPTH ? dHere * q.x / closest : SKY_DEPTH;
    bool validWorld = Params.z < 0.5 && q.x > 1e-6 && all(uvPrev >= 0.0) && all(uvPrev <= 1.0)
                   && PrevDepthMatches(uvPrev * RtMetrics.zw - 0.5 - Jitter.xy + Jitter.zw, expectedWorld,
                                       slack * expectedWorld / dHere);

    // Candidate 2, carried by the camera: the third-person camera follows the hero, who stays in place on the screen
    // while the world moves. Same place, same depth.
    bool validHeld = Params.z < 0.5 && PrevDepthMatches(uv * RtMetrics.zw - 0.5 - Jitter.xy + Jitter.zw, dHere, slack);

    // The candidate whose history matches the current pattern better (luma of the centre and its 4 neighbours) wins;
    // the world wins ties (flat areas) and needs only to be about as good
    float sadWorld = 0, sadHeld = 0;
    [unroll] for (int k = 0; k < 5; k++) {
        static const float2 cross5[5] = { float2(0, 0), float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1) };
        static const int index5[5] = { 4, 3, 5, 1, 7 };
        float2 o5 = cross5[k] * RtMetrics.xy;
        sadWorld += abs(ToYCoCg(tex2Dlod(History, float4(uvPrev + o5, 0, 0)).rgb).x - luma[index5[k]]);
        sadHeld += abs(ToYCoCg(tex2Dlod(History, float4(uv + o5, 0, 0)).rgb).x - luma[index5[k]]);
    }
    float3 histWorld = ToYCoCg(SampleHistory(uvPrev));
    float3 histHeld = ToYCoCg(tex2Dlod(History, float4(uv, 0, 0)).rgb);
    float worldMotion = length((uvPrev - uv) * RtMetrics.zw);
    bool held = validHeld && worldMotion > HELD_MIN_MOTION && (!validWorld || sadHeld + 0.02 < 0.5 * sadWorld);
    bool valid = validWorld || validHeld;
    float2 uvFrom = held ? uv : uvPrev;
    float3 histRaw = held ? histHeld : histWorld;
    float expected = held ? dHere : expectedWorld;

    // Locks follow the chosen history; the locked detail must not be hidden by something in front of it now
    float2 prevLock = valid ? tex2Dlod(Locks, float4((floor(uvFrom * RtMetrics.zw) + 0.5) * RtMetrics.xy, 0, 0)).xy : 0.0;
    bool locked = prevLock.x > 0.0 && expected >= prevLock.y * (1.0 - LOCK_DEPTH_TOL);
    float2 lock = locked ? float2(prevLock.x - 1.0 / LOCK_FRAMES, prevLock.y) : 0.0;
    if (thin)
        lock = float2(1.0, lock.x > 0.0 ? min(lock.y, dHere) : dHere);
    float3 hist = locked ? histRaw : ClipToBox(boxLo, boxHi, histRaw);

    float motion = length((uvFrom - uv) * RtMetrics.zw);
    float hw = valid ? lerp(Params.x, Params.y, saturate(motion / MOTION_PIXELS)) : 0.0;

    // Luminance weighting against flicker of bright sub-pixel details
    float wc = (1.0 - hw) / (1.0 + curY.x);
    float wh = hw / (1.0 + hist.x);
    float3 result = FromYCoCg((curY * wc + hist * wh) / (wc + wh));

    o.target = float4(result, cur.a);
    o.history = float4(result, 1.0);
    o.locks = float4(lock, 0.0, 1.0);
    return o;
}
