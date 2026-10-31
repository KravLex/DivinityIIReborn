// Divinity II Reborn: SMAA (third_party/smaa/SMAA.hlsli, J. Jimenez et al., MIT) as three Direct3D 9 passes that DXVK
// patch 0011 (d3d9.postAA) runs right after the engine's tonemap pass, i.e. on the final scene before the interface.
// Compiled per entry point by tools/shaders/build_post.py:
//   EdgeVS / EdgePS       s0 = scene (gamma space)                          -> edges   (A8R8G8B8, cleared to 0)
//   WeightVS / WeightPS   s0 = edges, s1 = AreaTex, s2 = SearchTex           -> weights (A8R8G8B8, cleared to 0)
//   BlendVS / BlendPS     s0 = scene, s1 = weights                           -> the engine's render target
// c0 (VS and PS) = (1/width, 1/height, width, height). Vertices: POSITION in clip space, TEXCOORD0 in 0..1.
// AreaTex is stored as A8R8G8B8 with the two area channels in r and g (the D3D9 demo used A8L8, hence .ra upstream).

#define SMAA_HLSL_3
#define SMAA_PRESET_ULTRA
#define SMAA_AREATEX_SELECT(sample) sample.rg

float4 RtMetrics : register(c0);
#define SMAA_RT_METRICS RtMetrics

#include "SMAA.hlsli"

sampler2D Tex0 : register(s0);
sampler2D Tex1 : register(s1);
sampler2D Tex2 : register(s2);

// Direct3D 9 puts pixel centres on integer coordinates: shift the full-screen geometry by half a pixel so that
// texcoords hit texel centres exactly (otherwise linear filtering would blur the whole picture)
float4 ClipPosition(float4 pos)
{
    return float4(pos.xy + float2(-1.0, 1.0) * RtMetrics.xy, pos.zw);
}

void EdgeVS(float4 pos : POSITION, float2 uv : TEXCOORD0,
            out float4 oPos : POSITION, out float2 oUv : TEXCOORD0,
            out float4 o0 : TEXCOORD1, out float4 o1 : TEXCOORD2, out float4 o2 : TEXCOORD3)
{
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
    oPos = ClipPosition(pos);
    oUv = uv;
    o0 = offset[0]; o1 = offset[1]; o2 = offset[2];
}

float4 EdgePS(float2 uv : TEXCOORD0, float4 o0 : TEXCOORD1, float4 o1 : TEXCOORD2, float4 o2 : TEXCOORD3) : COLOR0
{
    float4 offset[3] = { o0, o1, o2 };
    return float4(SMAALumaEdgeDetectionPS(uv, offset, Tex0), 0.0, 0.0);
}

// ---- Geometry-aware edges (EdgeGeoPS): s0 = scene, s1 = the engine's packed depth + view normal buffer ----------
// With the 4x HD textures and the tonemap's own unsharp mask, luma edges are found on nearly every texel of stone,
// wood and cloth (frame census 31.10, "show edges"): SMAA then blurs texture detail and the noise breaks the stair
// patterns of real edges, so the blending weights stay near zero. Here an edge counts only where the geometry has
// one: the depth does not continue the plane through the pixel (tested in 1/depth, which is linear across the screen
// for a plane, so slanted floors and walls are not edges), the normal turns sharply, or geometry meets sky.
// Luma then decides whether the edge is visible; local contrast adaptation is not used (texture noise next to a
// real edge would suppress it), geometry already rejects texture detail.

#define GEO_DEPTH_TOLERANCE 0.02    // relative deviation of 1/depth from the plane prediction
#define GEO_NORMAL_COS      0.8     // normals turning by more than ~37 degrees
#define GEO_LUMA_THRESHOLD  (SMAA_THRESHOLD * 0.5)
#define GEO_THIN_DEPTH      0.05    // a pixel this much closer than both neighbours in a row/column is a thin line

float GDepth(float4 g)
{
    return (g.x + g.y) * 1.52590219e-005;   // sky is cleared to depth 0
}

float3 GNormal(float4 g)
{
    float positive = (g.z - 1.1 >= 0) ? 1.0 : 0.0;
    float2 nxy = float2(g.z - 3.0 * positive, g.w) * 2.0 - 1.0;
    float nz = (1.0 - 2.0 * positive) * sqrt(saturate(1.0 - dot(nxy, nxy)));
    return normalize(float3(nxy, nz));
}

// 1 when pixel p and its neighbour n lie on different surfaces; o is the neighbour on the opposite side of p
float GeometryEdge(float4 p, float4 n, float4 o)
{
    float dp = GDepth(p), dn = GDepth(n), dop = GDepth(o);
    if (dp <= 0 || dn <= 0)
        return (dp <= 0) != (dn <= 0) ? 1.0 : 0.0;
    float wp = 1.0 / dp, wn = 1.0 / dn;
    float predicted = dop > 0 ? 2.0 * wp - 1.0 / dop : wp;
    float depthEdge = abs(wn - predicted) > GEO_DEPTH_TOLERANCE * wp ? 1.0 : 0.0;
    float crease = dot(GNormal(p), GNormal(n)) < GEO_NORMAL_COS ? 1.0 : 0.0;
    return max(depthEdge, crease);
}

float FarDepth(float4 g)
{
    float d = GDepth(g);
    return d > 0 ? d : 1e3;
}

// 1 when the middle one is a line one pixel thin in front of both sides
float ThinRidge(float4 a, float4 m, float4 b)
{
    return FarDepth(m) < min(FarDepth(a), FarDepth(b)) * (1.0 - GEO_THIN_DEPTH) ? 1.0 : 0.0;
}

float4 EdgeGeoPS(float2 uv : TEXCOORD0, float4 o0 : TEXCOORD1, float4 o1 : TEXCOORD2, float4 o2 : TEXCOORD3) : COLOR0
{
    float4 g   = tex2Dlod(Tex1, float4(uv, 0, 0));
    float4 gl  = tex2Dlod(Tex1, float4(o0.xy, 0, 0));
    float4 gt  = tex2Dlod(Tex1, float4(o0.zw, 0, 0));
    float4 gr  = tex2Dlod(Tex1, float4(o1.xy, 0, 0));
    float4 gb  = tex2Dlod(Tex1, float4(o1.zw, 0, 0));
    float4 gll = tex2Dlod(Tex1, float4(o2.xy, 0, 0));
    float4 gtt = tex2Dlod(Tex1, float4(o2.zw, 0, 0));
    float2 geometry = float2(GeometryEdge(g, gl, gr), GeometryEdge(g, gt, gb));
    // Lines one pixel thin (a fishing line, a rope) have an edge on both sides; SMAA's patterns are made for edges of
    // areas and turn such a line into a doubled one. They are left to TAA (DXVK patch 0012), which resolves them.
    geometry.x *= 1.0 - max(ThinRidge(gl, g, gr), ThinRidge(gll, gl, g));
    geometry.y *= 1.0 - max(ThinRidge(gt, g, gb), ThinRidge(gtt, gt, g));
    if (dot(geometry, 1.0) == 0.0)
        discard;

    float3 weights = float3(0.2126, 0.7152, 0.0722);
    float L     = dot(tex2Dlod(Tex0, float4(uv, 0, 0)).rgb, weights);
    float Lleft = dot(tex2Dlod(Tex0, float4(o0.xy, 0, 0)).rgb, weights);
    float Ltop  = dot(tex2Dlod(Tex0, float4(o0.zw, 0, 0)).rgb, weights);
    float2 edges = step(GEO_LUMA_THRESHOLD, abs(L - float2(Lleft, Ltop))) * geometry;
    if (dot(edges, 1.0) == 0.0)
        discard;
    return float4(edges, 0.0, 0.0);
}

void WeightVS(float4 pos : POSITION, float2 uv : TEXCOORD0,
              out float4 oPos : POSITION, out float2 oUv : TEXCOORD0, out float2 oPix : TEXCOORD1,
              out float4 o0 : TEXCOORD2, out float4 o1 : TEXCOORD3, out float4 o2 : TEXCOORD4)
{
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, oPix, offset);
    oPos = ClipPosition(pos);
    oUv = uv;
    o0 = offset[0]; o1 = offset[1]; o2 = offset[2];
}

float4 WeightPS(float2 uv : TEXCOORD0, float2 pix : TEXCOORD1,
                float4 o0 : TEXCOORD2, float4 o1 : TEXCOORD3, float4 o2 : TEXCOORD4) : COLOR0
{
    float4 offset[3] = { o0, o1, o2 };
    return SMAABlendingWeightCalculationPS(uv, pix, offset, Tex0, Tex1, Tex2, 0);
}

void BlendVS(float4 pos : POSITION, float2 uv : TEXCOORD0,
             out float4 oPos : POSITION, out float2 oUv : TEXCOORD0, out float4 oOffset : TEXCOORD1)
{
    SMAANeighborhoodBlendingVS(uv, oOffset);
    oPos = ClipPosition(pos);
    oUv = uv;
}

float4 BlendPS(float2 uv : TEXCOORD0, float4 offset : TEXCOORD1) : COLOR0
{
    return SMAANeighborhoodBlendingPS(uv, offset, Tex0, Tex1);
}
