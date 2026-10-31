// Divinity II Reborn: div2_PreLightPass_Hemisphere_Packed_ViewSpace (ps_3_0 6119555ad010ccf40568e7dfdeaf0405dc7ea8ff)
// rewritten in HLSL from its disassembly, same math and registers: the sky/ground ambient of the light pre-pass in
// areas without the sun pass (interiors, mines). Define AO_ENABLED before including.
// The original reads only TEXCOORD0; its vertex shader (shared with the sun pass) also passes the eye ray in
// TEXCOORD1, which the AO version uses for positions relative to the eye (no other constant is needed).

sampler2D NormalDepthSampler : register(s0);   // normalMapSampler

float4 ViewRows[3]   : register(c0);    // ViewMat
float3 SkyColor      : register(c3);
float3 GroundColor   : register(c4);
float  GlobalAmbient : register(c5);    // fGlobalAmbientIntensity

#include "gbuffer_ao.hlsli"

float4 main(float2 uv : TEXCOORD0
#if AO_ENABLED
            , float3 ray : TEXCOORD1, float2 vpos : VPOS
#endif
            ) : COLOR0
{
#if AO_ENABLED
    float3 dRayX = ddx(ray), dRayY = ddy(ray);
    float2 dUv = float2(ddx(uv).x, ddy(uv).y);
#endif
    float4 p = tex2D(NormalDepthSampler, uv);
    float3 n = normalize(DecodeViewNormalRaw(p));
    float nwz = n.y * ViewRows[1].z + ViewRows[0].z * n.x - ViewRows[2].z * n.z;

    float3 ambient = saturate(nwz + GlobalAmbient) * SkyColor + saturate(GlobalAmbient - nwz) * GroundColor;

#if AO_ENABLED
    float3 nw = n.y * ViewRows[1].xyz + n.x * ViewRows[0].xyz - n.z * ViewRows[2].xyz;
    float ao = AmbientOcclusion(NormalDepthSampler, uv, ray, DecodeDepth(p), nw, vpos, dRayX, dRayY, dUv);
    ao = DebugView(2.0) ? 1.0 : ao;
    ambient *= ao;
    if (DebugView(4.0))
        ambient = ao;
#endif
    return float4(ambient, 0.0);
}
