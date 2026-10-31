// Divinity II Reborn: decoding of the engine's packed depth + view normal buffer and screen-space ambient occlusion
// computed from it. Shared by the light pre-pass replacements.
//
// The buffer (3440x1440 A16B16G16R16F at 3440x1440, written by the *_PackedDepthNormal passes, frame census 31.10):
//   x + y = linear depth * 65535 (integer and fraction), z = n.x * 0.5 + 0.5 (+ 3 when n.z >= 0), w = n.y * 0.5 + 0.5
// Sky is cleared to (0, 0, 1, 1), i.e. depth 0. The light pre-pass vertex shader passes the ray from the eye to the far
// plane (TEXCOORD1), so position = eye + depth * ray.

// Divinity II Reborn: while TAA runs, DXVK (patch 0012) puts a value that changes every frame into this otherwise unused
// register for the light pre-pass draws (and restores it after them), so the per-pixel noise of AO and shadow filters
// changes from frame to frame and TAA averages it away; 0 without TAA. Shaders reading it list c39 as allowed.
// y = the F11 debug view: 1 shadow cascades, 2 original lighting (the engine's shadows, no AO), 3 contact shadows,
// 4 ambient occlusion; 0 otherwise.
float4 FrameNoise : register(c39);

bool DebugView(float view)
{
    return abs(FrameNoise.y - view) < 0.5;
}

// Interleaved gradient noise in 0..1, offset per frame
float PixelNoise(float2 vpos)
{
    return frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + FrameNoise.x);
}

float DecodeDepth(float4 p)
{
    return (p.x + p.y) * 1.52590219e-005;
}

float3 DecodeViewNormalRaw(float4 p)
{
    float positive = (p.z - 1.1 >= 0) ? 1.0 : 0.0;
    float2 nxy = float2(p.z - 3.0 * positive, p.w) * 2.0 - 1.0;
    float nz = (1.0 - 2.0 * positive) * sqrt(saturate(1.0 - nxy.x * nxy.x - nxy.y * nxy.y));
    return float3(nxy, nz);
}

#if AO_ENABLED
// Screen-space ambient occlusion from the engine's own G-buffer: AO_SAMPLES points on a golden-angle spiral within
// AO_RADIUS world units around the pixel, rotated per pixel. Every sample is turned back into a position with the
// engine's depth * ray reconstruction (the ray is linear across the screen, so its screen derivatives give the ray of
// any neighbour) relative to the eye, which cancels out, so no eye position constant is needed. A neighbour occludes
// by the cosine between the pixel's world normal and the direction to it, faded with distance; sky does not occlude.
// Returns 1 (no occlusion) for sky, a missing ray or far away pixels.
float AmbientOcclusion(sampler2D gbuffer, float2 uv, float3 ray, float depth, float3 nw, float2 vpos,
                       float3 dRayX, float3 dRayY, float2 dUv)
{
    float rayLength = length(ray);
    float pixelWorld = depth * length(dRayX);                 // world size of one pixel at this depth
    if (depth <= 0 || rayLength < 1e-3 || pixelWorld <= 0)
        return 1.0;

    float radiusPx = clamp(AO_RADIUS / pixelWorld, 2.0, AO_MAX_PIXELS);
    float noise = PixelNoise(vpos);
    float rot = noise * 6.2831853;
    float3 P = depth * ray;

    float sum = 0;
    [loop] for (int i = 0; i < AO_SAMPLES; i++) {
        float t = (i + 0.5) / AO_SAMPLES;
        float s, c;
        sincos(i * 2.39996323 + rot, s, c);
        float2 offPx = float2(c, s) * (sqrt(t) * radiusPx);
        float4 q = tex2Dlod(gbuffer, float4(uv + offPx * dUv, 0, 0));
        float qd = DecodeDepth(q);
        float3 v = qd * (ray + dRayX * offPx.x + dRayY * offPx.y) - P;
        float vv = dot(v, v);
        float cosine = dot(v, nw) * rsqrt(vv + 1e-6);
        float valid = (qd > 0) ? 1.0 : 0.0;
        sum += valid * saturate(cosine - AO_BIAS) * saturate(1.0 - vv / (AO_RADIUS * AO_RADIUS));
    }

    float ao = saturate(1.0 - AO_STRENGTH * 2.0 * sum / AO_SAMPLES);
    return lerp(ao, 1.0, saturate((depth * rayLength - AO_FADE_START) / AO_FADE_LENGTH));
}
#endif
