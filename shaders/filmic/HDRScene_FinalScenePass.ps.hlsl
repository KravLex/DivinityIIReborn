// replaces: 93c1d04241ad56a274a25b9eba1892bb1a1755ab ps_3_0  HDRScene.fx / HDRScene_FinalScenePass / P0
// preshader-registers: c3 c4 c5
//
// Divinity II Reborn "filmic" tonemap: the game's tonemap pass (see identity/HDRScene_FinalScenePass.ps.hlsl)
// with only the curve replaced. The original 8 * x^N / (x^N + D^N) becomes the ACES filmic fit (K. Narkowicz)
// over the whole 0..1 output range, with the exposure solved per frame so that middle grey (gfMiddleGray, where
// the engine's eye adaptation puts the average luminance) lands on screen exactly where the original puts it.
// Average brightness stays the same; instead of clipping hard a little above middle grey, highlights roll off
// softly and desaturate, shadows get a filmic toe. Unsharp mask, bloom, colour matrix, colour correction and
// photo filter are unchanged, so the game's per-area grading still applies.

sampler2D SceneSampler : register(s0);   // BasePointClampSampler: HDR scene (A16B16G16R16F)
sampler2D BloomSampler : register(s1);   // Shader0LinearClampSampler: bloom
sampler2D LumSampler   : register(s2);   // Shader1PointClampSampler: adapted luminance (1x1 R32F)

float4 PPMatrix[3]             : register(c0);
float  InvWidth                : register(c3);
float  InvHeight               : register(c4);
float  DPowN                   : register(c5);
float2 HDRCaps                 : register(c6);
float  gfMiddleGray            : register(c7);
float  gfBloomScale            : register(c8);
float  gfN                     : register(c9);
float3 ColorCorrectSourceColor : register(c10);
float3 ColorCorrectTargetColor : register(c11);
float  UnsharpMask             : register(c12);
float3 PhotofilterColor        : register(c13);
float  PhotofilterAmount       : register(c14);

// ACES fit and its derivative
float  Aces(float  y) { return (y * (2.51 * y + 0.03)) / (y * (2.43 * y + 0.59) + 0.14); }
float3 Aces(float3 y) { return (y * (2.51 * y + 0.03)) / (y * (2.43 * y + 0.59) + 0.14); }

float AcesSlope(float y)
{
    float n = y * (2.51 * y + 0.03), d = y * (2.43 * y + 0.59) + 0.14;
    return ((5.02 * y + 0.03) * d - n * (4.86 * y + 0.59)) / (d * d);
}

float3 Tonemap(float3 x)
{
    // Where the original curve puts middle grey on screen. The game's PPMatrix is close to identity
    // (captured 31.10: diagonal 1.14/1.08/1.18), so the original 8 * x^N / (x^N + D^N) clips to white
    // already a little above middle grey. ACES therefore has to produce the final 0..1 range itself.
    float g  = gfMiddleGray;
    float gn = pow(max(g, 1e-4), gfN);
    float target = clamp(8.0 * gn / (gn + DPowN), 0.02, 0.8);

    // Newton on y = exposure * g so that Aces(y) == target (monotonic, starts at g)
    float y = max(g, 1e-3);
    for (int i = 0; i < 6; i++)
        y = max(y - (Aces(y) - target) / AcesSlope(y), 1e-4);

    float exposure = y / max(g, 1e-4);

    return saturate(Aces(max(x, 0.0) * exposure));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2D(SceneSampler, uv);
    float3 ring = tex2D(SceneSampler, uv + float2(InvWidth, 0)).rgb
                + tex2D(SceneSampler, uv - float2(InvWidth, 0)).rgb
                + tex2D(SceneSampler, uv + float2(0, InvHeight)).rgb
                + tex2D(SceneSampler, uv - float2(0, InvHeight)).rgb;
    float4 scene = float4(centre.rgb + UnsharpMask * (centre.rgb - 0.25 * ring), centre.a) + 1e-9;

    float lum = min(HDRCaps.y, max(tex2D(LumSampler, 0.5).r, HDRCaps.x));
    float3 x = scene.rgb * (gfMiddleGray / (lum + 0.01));

    float4 c = float4(Tonemap(x), scene.a) + gfBloomScale * tex2D(BloomSampler, uv);

    float4 c1 = float4(c.rgb, 1.0);
    float3 col = float3(dot(c1, PPMatrix[0]), dot(c1, PPMatrix[1]), dot(c1, PPMatrix[2]));

    float w = saturate(1.0 - length(col - ColorCorrectSourceColor));
    w *= w;
    col = lerp(col, clamp(col + ColorCorrectTargetColor, -1.0, 1.0), w);

    float luma = dot(col, float3(0.3, 0.59, 0.11));
    float lo = saturate(luma + luma);
    float hi = saturate(luma * 2.0 - 1.0);
    float3 filt = hi * (1.0 - lo * PhotofilterColor) + lo * PhotofilterColor;
    col = lerp(col, filt, saturate(luma * PhotofilterAmount));

    return float4(col, c.a);
}
