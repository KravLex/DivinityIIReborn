// replaces: 93c1d04241ad56a274a25b9eba1892bb1a1755ab ps_3_0  HDRScene.fx / HDRScene_FinalScenePass / P0
// preshader-registers: c3 c4 c5
//
// Divinity II Reborn: the game's tonemap pass rewritten in HLSL from its disassembly, same math and
// registers. Used to prove that a replacement shader gives the same image (tests/run_override_test.cmd)
// before any change of the look. D3DX keeps setting the registers of the original effect:
//   c0-c2 PPMatrix (colour matrix rows), c6 HDRCaps, c7 gfMiddleGray, c8 gfBloomScale, c9 gfN,
//   c10/c11 ColorCorrectSource/TargetColor, c12 UnsharpMask, c13/c14 PhotofilterColor/Amount,
//   c3/c4/c5 written by the effect's preshader: 1/WindowSize.x, 1/WindowSize.y, gfD^gfN.

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

float3 Tonemap(float3 x)
{
    // Original curve: x^N / (x^N + D^N), scaled by 8 (PPMatrix brings it back)
    float3 xn = pow(x, gfN);
    return xn / (xn + DPowN) * 8.0;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // Unsharp mask on the HDR scene: centre minus the mean of 4 neighbours
    float4 centre = tex2D(SceneSampler, uv);
    float3 ring = tex2D(SceneSampler, uv + float2(InvWidth, 0)).rgb
                + tex2D(SceneSampler, uv - float2(InvWidth, 0)).rgb
                + tex2D(SceneSampler, uv + float2(0, InvHeight)).rgb
                + tex2D(SceneSampler, uv - float2(0, InvHeight)).rgb;
    float4 scene = float4(centre.rgb + UnsharpMask * (centre.rgb - 0.25 * ring), centre.a) + 1e-9;

    // Exposure from the adapted luminance, clamped to HDRCaps
    float lum = min(HDRCaps.y, max(tex2D(LumSampler, 0.5).r, HDRCaps.x));
    float3 x = scene.rgb * (gfMiddleGray / (lum + 0.01));

    float4 c = float4(Tonemap(x), scene.a) + gfBloomScale * tex2D(BloomSampler, uv);

    // Colour matrix
    float4 c1 = float4(c.rgb, 1.0);
    float3 col = float3(dot(c1, PPMatrix[0]), dot(c1, PPMatrix[1]), dot(c1, PPMatrix[2]));

    // Colour correction: pull colours near the source colour towards source + target
    float w = saturate(1.0 - length(col - ColorCorrectSourceColor));
    w *= w;
    col = lerp(col, clamp(col + ColorCorrectTargetColor, -1.0, 1.0), w);

    // Photo filter: multiply below mid luma, screen above, faded in by luma * amount
    float luma = dot(col, float3(0.3, 0.59, 0.11));
    float lo = saturate(luma + luma);
    float hi = saturate(luma * 2.0 - 1.0);
    float3 filt = hi * (1.0 - lo * PhotofilterColor) + lo * PhotofilterColor;
    col = lerp(col, filt, saturate(luma * PhotofilterAmount));

    return float4(col, c.a);
}
