// replaces: 93c1d04241ad56a274a25b9eba1892bb1a1755ab ps_3_0  HDRScene.fx / HDRScene_FinalScenePass / P0
// preshader-registers: c3 c4 c5
//
// Divinity II Reborn "soft" tonemap: the game's tonemap pass (see identity/HDRScene_FinalScenePass.ps.hlsl) with
// only the highlight end of the curve changed. With the game's values (frame census 31.10: gfN = 1, D^N = 13..15.4)
// the original 8 * x / (x + D) is almost linear up to the point where it clips to white, about 8x middle grey.
// Here the curve is H. Uchimura's (Gran Turismo) with a linear toe: shadows and midtones keep the original slope
// (exposure solved so that middle grey lands where the original puts it), and above ~3.7x middle grey a smooth
// exponential shoulder replaces the hard clip, reaching white only at ~16x. Everything else as in the original,
// except the unsharp mask (UnsharpMask = 0.5 in the game): it sharpens the scene before the curve, after SMAA
// that gives the steps of edges their contrast back and makes texture detail shimmer in motion.

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

// Uchimura curve with P = 1 (white), a = 1 (slope of the linear part), m = 0.22, l = 0.4, linear toe (c = 1):
// y = x up to S0 = m + (1 - m) * l = 0.532, then y = 1 - (1 - S0) * exp(-(x - S0) / (1 - S0))
static const float S0 = 0.532;

// Fraction of the game's unsharp mask that is kept: 0 = off (default), 1 = as in the original
#ifndef SOFT_UNSHARP_SCALE
#define SOFT_UNSHARP_SCALE 0
#endif

float3 Shoulder(float3 y)
{
    float3 s = 1.0 - (1.0 - S0) * exp(-(y - S0) / (1.0 - S0));
    return y < S0 ? y : s;
}

float3 Tonemap(float3 x)
{
    // Where the original curve puts middle grey on screen; in the linear part the exposure follows directly
    float g  = max(gfMiddleGray, 1e-4);
    float gn = pow(g, gfN);
    float target = clamp(8.0 * gn / (gn + DPowN), 0.01, 0.5);
    float exposure = target / g;

    return Shoulder(max(x, 0.0) * exposure);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2D(SceneSampler, uv);
#if SOFT_UNSHARP_SCALE > 0
    float3 ring = tex2D(SceneSampler, uv + float2(InvWidth, 0)).rgb
                + tex2D(SceneSampler, uv - float2(InvWidth, 0)).rgb
                + tex2D(SceneSampler, uv + float2(0, InvHeight)).rgb
                + tex2D(SceneSampler, uv - float2(0, InvHeight)).rgb;
    float4 scene = float4(centre.rgb + UnsharpMask * SOFT_UNSHARP_SCALE * (centre.rgb - 0.25 * ring), centre.a) + 1e-9;
#else
    float4 scene = centre + 1e-9;
#endif

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
