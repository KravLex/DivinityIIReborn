// 09.11.2026: the engine's variance floor; a set may define a smaller one before including this file (far: the blurred, pale shadows of NPCs)
#ifndef VSM_VAR_FLOOR
#define VSM_VAR_FLOOR 0.001
#endif
// Divinity II Reborn: the sun + ambient part of the engine's light pre-pass rewritten in HLSL from the disassembly,
// same math and registers:
//   TERRAIN 0: div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char          (ps_3_0 e6bef6f7376173eb73df5390447a1067939c5e8a)
//   TERRAIN 1: div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char_Terrain  (ps_3_0 d50c5e8e8201ae4165e7edeca1b991f8cf1c16bf)
// It writes the light accumulation buffer (rgb = ambient + backlight + sun * shadow, a = specular) that the material
// shaders read. Define TERRAIN and AO_ENABLED (0 = exact rewrite, 1 = with ambient occlusion) before including;
// SHADOW_QUALITY 1 replaces the shadow terms with those of shadows.hlsli (NG-6), 0 (default) keeps the engine's;
// SHADOW_FAR 1 keeps the engine's but blends the cascades and lets them reach twice as far (set "far").
//
// s1 = VSM moments atlas (4 cascades in 8192^2 G16R16F), s2 = character shadow map (4096^2 R32F),
// s3 (TERRAIN) = terrain shadow map over the terrain bounding box.

sampler2D NormalDepthSampler : register(s0);   // normalMapSampler
sampler2D ShadowSampler      : register(s1);   // ShadowMapSamplerFiltered
sampler2D CharShadowSampler  : register(s2);   // ShadowMapSamplerCharacter

float4 LightRows[16]       : register(c0);    // g_LightMatricesArr[4] (rows)
float4 CharRows[4]         : register(c16);   // g_matCharacterWorldToLightProj
float4 ShadowOffsets[4]    : register(c20);   // g_Offsets[4]: atlas quadrant of each cascade
float4 ViewRows[3]         : register(c24);   // ViewMat
float  CharEpsilon         : register(c27);   // g_fCharacterShadowEpsilon
float3 EyePosition         : register(c28);
float3 SunDir              : register(c29);
float3 BackLightColor      : register(c30);
float  BackLightIntensity  : register(c31);
float3 SunColor            : register(c32);
float  ShadowBleeding      : register(c33);
float4 ShadowDistances     : register(c34);   // g_afShadowDistances
float  ShadowFarDistance   : register(c35);   // g_fShadowFarDistance
float3 SkyColor            : register(c36);
float3 GroundColor         : register(c37);
float  GlobalAmbient       : register(c38);   // fGlobalAmbientIntensity
#if TERRAIN
sampler2D TerrainShadowSampler : register(s3); // TerrainShadowMapSampler
float2 TerrainBBOXMin      : register(c40);
float  SunIntensity        : register(c41);
float2 InvTerrainBBOXDim   : register(c42);   // preshader: 1 / TerrainBBOXDim
#else
float  SunIntensity        : register(c40);
#endif

#ifndef SHADOW_QUALITY
#define SHADOW_QUALITY 0
#endif
#ifndef SHADOW_FAR
#define SHADOW_FAR 0     // 1: the engine's shadows, cascades blended, reaching further (set "far")
#endif
#ifndef SHADOW_FAR_COMPARE
#define SHADOW_FAR_COMPARE 0   // 1 (set "far_compare", for screenshots): F11 "original lighting" shows the engine's
#endif                         // hard cascade switch and shadow distance instead

#include "gbuffer_ao.hlsli"
#if SHADOW_QUALITY
#include "shadows.hlsli"
#endif

#if SHADOW_FAR
// The engine's VSM test in cascade n (0..3): one sample, Chebyshev to the 4th power, variance floor 0.001; 1 = lit.
// edge: 1 where the point leaves the cascade's square (fading over its outer 15%).
float EngineCascade(float4 P, float n, out float edge)
{
    float4 is = (abs(n - float4(0, 1, 2, 3)) < 0.5) ? 1.0 : 0.0;
    float4 m0 = is.x * LightRows[0] + is.y * LightRows[4] + is.z * LightRows[8] + is.w * LightRows[12];
    float4 m1 = is.x * LightRows[1] + is.y * LightRows[5] + is.z * LightRows[9] + is.w * LightRows[13];
    float4 m2 = is.x * LightRows[2] + is.y * LightRows[6] + is.z * LightRows[10] + is.w * LightRows[14];
    float4 m3 = is.x * LightRows[3] + is.y * LightRows[7] + is.z * LightRows[11] + is.w * LightRows[15];
    float2 off = is.x * ShadowOffsets[0].xy + is.y * ShadowOffsets[1].xy + is.z * ShadowOffsets[2].xy + is.w * ShadowOffsets[3].xy;

    float rw = 1.0 / dot(P, m3);
    float2 clip = float2(dot(P, m0), dot(P, m1)) * rw;
    float2 sxy = clip * 0.5 + 0.5;
    float2 suv = float2(sxy.x, 1.0 - sxy.y) * 0.5 + off;
    float sdepth = saturate(dot(P, m2) * rw);
    float2 mom = tex2Dlod(ShadowSampler, float4(suv, 0, 0)).xy;

    float d = sdepth - mom.x;
    float var = saturate(mom.y - mom.x * mom.x + VSM_VAR_FLOOR);
    float pmax = var / (d * d + var);
    pmax *= pmax;
    pmax *= pmax;
    edge = saturate((max(abs(clip.x), abs(clip.y)) - 0.85) / 0.15);
    return (mom.x - sdepth >= 0) ? 1.0 : pmax;
}
#endif

// The engine's shadow term: character shadow map and one VSM cascade, faded out beyond the shadow distance, times the
// terrain shadow (before the bleeding is added)
float EngineShadow(float4 P, float viewZ, float terrain)
{
    // character / dragon shadow map
    float invCharW = 1.0 / dot(P, CharRows[3]);
    float charDepth = saturate(invCharW * dot(P, CharRows[2]));
    float2 cxy = float2(dot(P, CharRows[0]), dot(P, CharRows[1])) * 0.5 * invCharW + 0.5;
    float charTest = tex2D(CharShadowSampler, float2(cxy.x, 1.0 - cxy.y)).x + CharEpsilon - charDepth;

#if SHADOW_FAR
    // Set "far": the engine's test, but (1) the engine switches from one cascade to the next at once, and the next has
    // coarser texels and a wider blur: on walls and ground that is a line moving with the camera (in the game: across a
    // church facade, where the eave's shadow changes). Over the last 15% of a cascade's range the next one is blended
    // in. (2) The engine fades shadows out over 50 units beyond g_fShadowFarDistance (200 in the game); here they go on
    // to twice that distance and fade over one more (at least the engine's 50 units, so never earlier than the engine),
    // as long as the last cascade covers the point.
    float4 beyond = (ShadowDistances - viewZ >= 0) ? 0.0 : 1.0;
    float n = max(dot(beyond, 1.0) - 1.0, 0.0);
    float edge, edgeNext;
    float pmax = EngineCascade(P, n, edge);
    float4 is = (abs(n - float4(0, 1, 2, 3)) < 0.5) ? 1.0 : 0.0;
    float start = dot(is, ShadowDistances);
    float next = dot(is.xyz, ShadowDistances.yzw) + is.w * 1e9;
    float band = 0.15 * (next - start);
    float w = saturate((viewZ - (next - band)) / max(band, 1e-4));
#if SHADOW_FAR_COMPARE
    bool original = DebugView(2.0);
    w = original ? 0.0 : w;
#endif
    if (n < 2.5 && w > 0)
        pmax = lerp(pmax, EngineCascade(P, n + 1.0, edgeNext), w);
    edge = (n > 2.5) ? edge : 0.0;
    float3 vp = float3(dot(P, ViewRows[0]), dot(P, ViewRows[1]), viewZ);
    float fade = max(edge, saturate((length(vp) - 2.0 * ShadowFarDistance) / max(ShadowFarDistance, 50.0)));
#if SHADOW_FAR_COMPARE
    fade = original ? saturate((length(vp) - ShadowFarDistance) * 0.02) : fade;
#endif
#if TERRAIN
    float sun = terrain * pmax * (1.0 - fade) + terrain * fade;
#else
    float sun = lerp(fade, 1.0, pmax);
#endif
#else
    // cascade: number of cascade distances the pixel lies beyond (1..4)
    float4 beyond = (ShadowDistances - viewZ >= 0) ? 0.0 : 1.0;
    float4 sel = dot(beyond, 1.0) + float4(-1, -2, -3, -4);
    float2 off = 0;
    float4 m0 = 0, m1 = 0, m2 = 0, m3 = 0;
    off = (-abs(sel.x) >= 0) ? ShadowOffsets[0].xy : off;
    off = (-abs(sel.y) >= 0) ? ShadowOffsets[1].xy : off;
    off = (-abs(sel.z) >= 0) ? ShadowOffsets[2].xy : off;
    off = (-abs(sel.w) >= 0) ? ShadowOffsets[3].xy : off;
    m0 = (-abs(sel.x) >= 0) ? LightRows[0] : m0;  m0 = (-abs(sel.y) >= 0) ? LightRows[4] : m0;
    m0 = (-abs(sel.z) >= 0) ? LightRows[8] : m0;  m0 = (-abs(sel.w) >= 0) ? LightRows[12] : m0;
    m1 = (-abs(sel.x) >= 0) ? LightRows[1] : m1;  m1 = (-abs(sel.y) >= 0) ? LightRows[5] : m1;
    m1 = (-abs(sel.z) >= 0) ? LightRows[9] : m1;  m1 = (-abs(sel.w) >= 0) ? LightRows[13] : m1;
    m2 = (-abs(sel.x) >= 0) ? LightRows[2] : m2;  m2 = (-abs(sel.y) >= 0) ? LightRows[6] : m2;
    m2 = (-abs(sel.z) >= 0) ? LightRows[10] : m2; m2 = (-abs(sel.w) >= 0) ? LightRows[14] : m2;
    m3 = (-abs(sel.x) >= 0) ? LightRows[3] : m3;  m3 = (-abs(sel.y) >= 0) ? LightRows[7] : m3;
    m3 = (-abs(sel.z) >= 0) ? LightRows[11] : m3; m3 = (-abs(sel.w) >= 0) ? LightRows[15] : m3;

    float rw = 1.0 / dot(P, m3);
    float2 sxy = float2(dot(P, m0), dot(P, m1)) * 0.5 * rw + 0.5;
    float2 suv = float2(sxy.x, 1.0 - sxy.y) * 0.5 + off;
    float sdepth = saturate(dot(P, m2) * rw);
    float2 mom = tex2D(ShadowSampler, suv).xy;

    // variance shadow map (Chebyshev), faded out beyond the shadow distance, times the terrain shadow
    float sun;
    if (mom.x - sdepth >= 0) {
        sun = terrain;
    } else {
        float d = sdepth - mom.x;
        float var = saturate(mom.y - mom.x * mom.x + VSM_VAR_FLOOR);
        float pmax = var / (d * d + var);
        float3 vp = float3(dot(P, ViewRows[0]), dot(P, ViewRows[1]), viewZ);
        float fade = saturate((length(vp) - ShadowFarDistance) * 0.02);
        pmax *= pmax;
        pmax *= pmax;
#if TERRAIN
        sun = terrain * pmax * (1.0 - fade) + terrain * fade;
#else
        sun = lerp(fade, 1.0, pmax);
#endif
    }
#endif

    return saturate(sun * ((charTest >= 0) ? 1.0 : 0.0));
}

float4 main(float2 uv : TEXCOORD0, float3 ray : TEXCOORD1
#if AO_ENABLED || SHADOW_QUALITY
            , float2 vpos : VPOS
#endif
            ) : COLOR0
{
#if AO_ENABLED || SHADOW_QUALITY
    // gradients before any flow control
    float3 dRayX = ddx(ray), dRayY = ddy(ray);
    float2 dUv = float2(ddx(uv).x, ddy(uv).y);
#endif
    float4 p = tex2D(NormalDepthSampler, uv);
    float depth = DecodeDepth(p);
    float3 n = normalize(normalize(DecodeViewNormalRaw(p)));
    float3 nw = n.y * ViewRows[1].xyz + n.x * ViewRows[0].xyz - n.z * ViewRows[2].xyz;

    float back = max(dot(-SunDir, nw), 0.0) * BackLightIntensity;
    float4 P = float4(depth * ray + EyePosition, 1.0);

    float viewZ = dot(P, ViewRows[2]);
    float groundW = saturate(GlobalAmbient - nw.z);
    float skyW = saturate(nw.z + GlobalAmbient);

    float terrain = 1.0;
#if TERRAIN
    terrain = tex2Dlod(TerrainShadowSampler, float4(saturate((P.xy - TerrainBBOXMin) * InvTerrainBBOXDim), 0, 0)).x;
#endif

#if SHADOW_QUALITY
    // per-pixel rotation of the filter discs and offset of the contact march (averaged away by TAA)
    float noise = PixelNoise(vpos);
    float2 rot;
    sincos(noise * 6.2831853, rot.y, rot.x);
    float3 vp = float3(dot(P, ViewRows[0]), dot(P, ViewRows[1]), viewZ);
    // the engine fades shadows out over 50 units beyond g_fShadowFarDistance; here they go on to twice that distance
    // (fading over one more such distance) as long as the last cascade covers the pixel
    float3 cascades = SunCascades(P, viewZ, rot);
    float fade = max(cascades.y, saturate((length(vp) - 2.0 * ShadowFarDistance) / max(ShadowFarDistance, 1.0)));
    float sun = terrain * (cascades.x * (1.0 - fade) + fade);
    float shadow = saturate(sun * CharacterShadow(P, rot, dot(SunDir, nw)));
    float contact = ContactShadow(depth * ray, ray, depth, nw, dRayX, dRayY, uv, dUv, noise);
    shadow *= contact;
    // F11 "original lighting": the engine's shadows (and no AO) for comparison
    shadow = DebugView(2.0) ? EngineShadow(P, viewZ, terrain) : shadow;
    shadow = saturate(shadow + ShadowBleeding);
#else

    float shadow = EngineShadow(P, viewZ, terrain);
    shadow = saturate(shadow + ShadowBleeding);
#endif

    float diff = max(dot(SunDir, nw), 0.0) * SunIntensity;
    float3 ambient = groundW * GroundColor + skyW * SkyColor;
    float3 backlight = back * BackLightColor;

#if AO_ENABLED
    float ao = AmbientOcclusion(NormalDepthSampler, uv, ray, depth, nw, vpos, dRayX, dRayY, dUv);
    ao = DebugView(2.0) ? 1.0 : ao;
    ambient *= ao;
    backlight *= lerp(1.0, ao, AO_BACKLIGHT);
#endif

    // half vector for the specular term
    float3 V = EyePosition - P.xyz;
    float3 H = normalize(V * rsqrt(dot(V, V)) + SunDir);
    float NdotH = dot(nw, H);

    float4 o;
    o.xyz = ambient + (diff * SunColor * shadow + backlight);
    // the original uses the pow instruction (log, mul, exp); pow() with a literal 16 would compile to exact
    // multiplications and differ in the last bits, so the same sequence is written out. Its last instruction is
    // dp2add oC0.w, r0.y, r0.x, 0 with replicated swizzles, i.e. spec * shadow + spec * shadow: the engine's
    // specular is written doubled, and so is it here.
    o.w = 2.0 * diff * exp2(log2(max(NdotH, 0.0)) * 16.0) * shadow;
#if SHADOW_QUALITY
    if (DebugView(1.0))
        o = float4(CascadeDebugColour(cascades.z, shadow), 0.0);
    if (DebugView(3.0))
        o = float4(contact.xxx, 0.0);
#endif
#if AO_ENABLED
    if (DebugView(4.0))
        o = float4(ao.xxx, 0.0);
#endif
    return o;
}
