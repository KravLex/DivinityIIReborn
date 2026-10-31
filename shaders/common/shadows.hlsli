// Divinity II Reborn NG-6: shadows of the sun pass (included by prelight_sun.hlsli with SHADOW_QUALITY 1).
//
// The engine samples its VSM cascades once (pre-blurred moments, Chebyshev to the 4th power: the same penumbra
// everywhere, a hard seam where one cascade hands over to the next) and tests the character shadow map once (stair
// steps). Here:
//   - sun cascades: contact-hardening penumbra. The average blocker depth is estimated from the moments over a search
//     disc (variance soft shadows), the penumbra grows with the receiver's distance from it, the moments are averaged
//     over a disc of that size (they are linear) and fed to the engine's own Chebyshev test; near the end of a cascade
//     the next one is blended in;
//   - character shadow: percentage-closer soft shadows (blocker search, then 12-tap PCF of the size it implies);
//   - contact shadows: a short march towards the sun through the engine's depth buffer catches small things that are
//     not in the shadow maps (feet, stones, folds of armour).
// Filter discs are rotated per pixel; the noise is averaged away by TAA (DXVK patch 0012).
// Sizes are in world units (the engine's: AO and the camera suggest about a metre); the shadow maps' resolutions are
// not needed, the light matrices give world units per texture coordinate.

#define SUN_SOFTNESS        0.02    // tan of the angular radius used for penumbrae (the real sun 0.0047)
#define VSM_SEARCH          0.6     // blocker search radius of the sun cascades
#define VSM_MAX_RADIUS      0.5     // largest extra penumbra radius of the sun cascades
// the engine adds 0.001 to the variance: about 3.6 world units of light depth, so anything lower than a few units over
// its receiver casts a pale shadow (contact shadows make up for it). A smaller floor was tried and dropped: bumpy
// ground then shadows itself (its blurred mean depth lies slightly towards the light), in the nearest cascade only
// (twice the depth scale of the next), which showed in the game as a straight edge moving with the player.
#define VSM_MIN_VARIANCE(t) 0.001
#define CHAR_SEARCH         0.15    // blocker search radius of the character shadow
#define CHAR_MIN_RADIUS     0.02    // smallest filter radius of the character shadow (no stair steps)
#define CHAR_MAX_RADIUS     0.15
#define CASCADE_BLEND       0.15    // the last 15% of a cascade's depth range blends into the next one
#define CONTACT_LENGTH      1.0     // how far contact shadows look towards the sun
#define CONTACT_THICKNESS   0.35    // occluders thicker than this along the view ray do not count (backgrounds)
#define CONTACT_STEPS       16
// Contact shadows are for things near the hero. Dense grass blades shadow each other in them, the flat grass the
// engine draws beyond its dense-grass radius does not: the edge of that radius showed as a line moving with the
// player (F11 cascade view: one cascade, dark dense grass, bright beyond). They are gone before that radius.
#define CONTACT_FADE_START  5.0     // distance from the eye where contact shadows start to fade out
#define CONTACT_FADE_LENGTH 10.0
// ...and close to the camera too: seen from a low camera, dense grass is a wall of blades in the depth buffer and the
// march towards the sun always runs into the next one (F11 contact view: near grass almost black, edge where they fade).
// Thin things (blades, leaves, twigs) get no contact shadows: the depth FOLIAGE_PROBE world units to both sides of the
// pixel (across and along the screen) is checked against the pixel's own plane; off the plane on both sides of an axis,
// at least one of them further away, means a thin thing in front of its surroundings (or among other blades). Ground
// next to a foot or a stone differs on one side only, a crevice is nearer on both: these keep their contact shadows.
#define FOLIAGE_PROBE       0.06
#define FOLIAGE_MIN         0.01    // relative depth off the plane (both sides) where contact shadows start to go
#define FOLIAGE_RANGE       0.02

static const float2 Ring8[8] = {
    float2(1.0, 0.0), float2(0.7071, 0.7071), float2(0.0, 1.0), float2(-0.7071, 0.7071),
    float2(-1.0, 0.0), float2(-0.7071, -0.7071), float2(0.0, -1.0), float2(0.7071, -0.7071) };

static const float2 Disc12[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598) };

float2 Rotate(float2 v, float2 cs)
{
    return float2(v.x * cs.x - v.y * cs.y, v.x * cs.y + v.y * cs.x);
}

// One cascade of the sun's VSM: lit fraction before the engine's distance fade and terrain shadow
float CascadeShadow(float4 P, float4 m0, float4 m1, float4 m2, float4 m3, float2 off, float2 rot)
{
    float rw = 1.0 / dot(P, m3);
    float2 sxy = float2(dot(P, m0), dot(P, m1)) * 0.5 * rw + 0.5;
    float2 suv = float2(sxy.x, 1.0 - sxy.y) * 0.5 + off;
    float t = saturate(dot(P, m2) * rw);
    float2 uvPerWorld = float2(length(m0.xyz), length(m1.xyz)) * 0.25 * abs(rw);
    float depthPerWorld = max(length(m2.xyz) * abs(rw), 1e-6);
    float2 lo = off + 1e-4, hi = off + 0.5 - 1e-4;              // stay inside this cascade's quadrant

    // blocker search: mean and variance of the moments over the search disc
    float2 centre = tex2Dlod(ShadowSampler, float4(suv, 0, 0)).xy;
    float2 sum = centre;
    [unroll] for (int i = 0; i < 8; i++)
        sum += tex2Dlod(ShadowSampler, float4(clamp(suv + Rotate(Ring8[i], rot) * VSM_SEARCH * uvPerWorld, lo, hi), 0, 0)).xy;
    float mu = sum.x / 9.0;
    float var = max(sum.y / 9.0 - mu * mu, 1e-6);
    float radius = 0.0;
    if (t > mu) {
        float dd = t - mu;
        float lit = var / (var + dd * dd);
        float blocker = clamp((mu - lit * t) / max(1.0 - lit, 1e-3), 0.0, t);
        radius = min((t - blocker) / depthPerWorld * SUN_SOFTNESS, VSM_MAX_RADIUS);
    }

    // moments over the penumbra disc (the engine's pre-blurred sample when there is no penumbra to add)
    float2 mom = centre;
    if (radius > 1e-3) {
        float2 acc = centre;
        [unroll] for (int k = 0; k < 12; k++)
            acc += tex2Dlod(ShadowSampler, float4(clamp(suv + Rotate(Disc12[k], rot) * radius * uvPerWorld, lo, hi), 0, 0)).xy;
        mom = acc / 13.0;
    }

    // the engine's Chebyshev test with its light-bleeding reduction (4th power), with a smaller variance floor
    if (mom.x - t >= 0)
        return 1.0;
    float d = t - mom.x;
    float v = saturate(mom.y - mom.x * mom.x + VSM_MIN_VARIANCE(t));
    float pmax = v / (d * d + v);
    pmax *= pmax;
    return pmax * pmax;
}

// The cascade with number n (0..3) from the constant arrays (no indexing of constants in ps_3_0)
void SelectCascade(float n, out float4 m0, out float4 m1, out float4 m2, out float4 m3, out float2 off)
{
    float4 is = (abs(n - float4(0, 1, 2, 3)) < 0.5) ? 1.0 : 0.0;
    m0 = is.x * LightRows[0] + is.y * LightRows[4] + is.z * LightRows[8] + is.w * LightRows[12];
    m1 = is.x * LightRows[1] + is.y * LightRows[5] + is.z * LightRows[9] + is.w * LightRows[13];
    m2 = is.x * LightRows[2] + is.y * LightRows[6] + is.z * LightRows[10] + is.w * LightRows[14];
    m3 = is.x * LightRows[3] + is.y * LightRows[7] + is.z * LightRows[11] + is.w * LightRows[15];
    off = is.x * ShadowOffsets[0].xy + is.y * ShadowOffsets[1].xy + is.z * ShadowOffsets[2].xy + is.w * ShadowOffsets[3].xy;
}

// Sun cascades: g_afShadowDistances are the start distances of the cascades (the engine picks the last one the
// pixel lies beyond); near the start of the next cascade both are evaluated and blended.
// Returns (lit fraction, 1 where the last cascade runs out of coverage, cascade number + blend towards the next): the
// last cascade reaches much further than the engine's g_fShadowFarDistance (in the game +-330 units), shadows may go
// on until its edge.
float3 SunCascades(float4 P, float viewZ, float2 rot)
{
    float4 beyond = (ShadowDistances - viewZ >= 0) ? 0.0 : 1.0;
    float n = dot(beyond, 1.0) - 1.0;
    float4 m0, m1, m2, m3;
    float2 off;
    SelectCascade(n, m0, m1, m2, m3, off);
    float s = CascadeShadow(P, m0, m1, m2, m3, off, rot);
    float2 clip = float2(dot(P, m0), dot(P, m1)) / dot(P, m3);
    float edge = (n > 2.5) ? saturate((max(abs(clip.x), abs(clip.y)) - 0.85) / 0.15) : 0.0;

    float4 is = (abs(n - float4(0, 1, 2, 3)) < 0.5) ? 1.0 : 0.0;
    float start = dot(is, ShadowDistances);
    float next = dot(is.xyz, ShadowDistances.yzw) + is.w * 1e9;
    float band = CASCADE_BLEND * (next - start);
    float w = saturate((viewZ - (next - band)) / max(band, 1e-4));
    if (n < 2.5 && w > 0) {
        SelectCascade(n + 1.0, m0, m1, m2, m3, off);
        s = lerp(s, CascadeShadow(P, m0, m1, m2, m3, off, rot), w);
    }
    return float3(s, edge, n + ((n < 2.5) ? w : 0.0));
}

// F11 debug view: cascades 0..3 red, green, blue, yellow (blended where they blend), darker where shadowed
float3 CascadeDebugColour(float cascade, float shadow)
{
    static const float3 colours[4] = { float3(1.0, 0.25, 0.25), float3(0.25, 1.0, 0.25), float3(0.3, 0.45, 1.0), float3(1.0, 0.9, 0.2) };
    float n = floor(cascade);
    float w = cascade - n;
    float4 is = (abs(n - float4(0, 1, 2, 3)) < 0.5) ? 1.0 : 0.0;
    float3 c = is.x * colours[0] + is.y * colours[1] + is.z * colours[2] + is.w * colours[3];
    float3 next = is.x * colours[1] + is.y * colours[2] + is.z * colours[3] + is.w * colours[3];
    return lerp(c, next, w) * (0.15 + 0.85 * shadow) * 1.5;
}

// Character shadow map (R32F light depth; lit where map + epsilon >= depth), PCSS
float CharacterShadow(float4 P, float2 rot, float ndl)
{
    float invW = 1.0 / dot(P, CharRows[3]);
    float charDepth = saturate(invW * dot(P, CharRows[2]));
    float2 cxy = float2(dot(P, CharRows[0]), dot(P, CharRows[1])) * 0.5 * invW + 0.5;
    float2 cuv = float2(cxy.x, 1.0 - cxy.y);
    float2 uvPerWorld = float2(length(CharRows[0].xyz), length(CharRows[1].xyz)) * 0.5 * abs(invW);
    float depthPerWorld = max(length(CharRows[2].xyz) * abs(invW), 1e-6);

    float blockers = 0, count = 0;
    float zc = tex2Dlod(CharShadowSampler, float4(cuv, 0, 0)).x;
    if (zc + CharEpsilon < charDepth) { blockers += zc; count += 1; }
    [unroll] for (int i = 0; i < 8; i++) {
        float z = tex2Dlod(CharShadowSampler, float4(cuv + Rotate(Ring8[i], rot) * CHAR_SEARCH * uvPerWorld, 0, 0)).x;
        if (z + CharEpsilon < charDepth) { blockers += z; count += 1; }
    }
    if (count == 0)
        return 1.0;

    float blocker = blockers / count;
    float radius = clamp((charDepth - blocker) / depthPerWorld * SUN_SOFTNESS, CHAR_MIN_RADIUS, CHAR_MAX_RADIUS);
    // a surface tilted to the light changes its light depth across the filter disc: the taps away from the centre
    // get the extra bias of that slope, or the character would shadow itself in stripes
    float slope = sqrt(saturate(1.0 - ndl * ndl)) / max(ndl, 0.2);
    float lit = (zc + CharEpsilon - charDepth >= 0) ? 1.0 : 0.0;
    [unroll] for (int k = 0; k < 12; k++) {
        float2 o = Rotate(Disc12[k], rot) * radius;
        float z = tex2Dlod(CharShadowSampler, float4(cuv + o * uvPerWorld, 0, 0)).x;
        lit += (z + CharEpsilon + length(o) * slope * depthPerWorld - charDepth >= 0) ? 1.0 : 0.0;
    }
    return lit / 13.0;
}

// Contact shadow from the depth buffer: 1 lit, 0 fully covered. Positions relative to the eye; the screen position
// of a point v is found from v = t * (ray + a * dRayX + b * dRayY) (t = depth as stored, a/b = pixel offsets).
// 1 where the pixel lies on something thin (see FOLIAGE_PROBE). The pixel's plane (its G-buffer normal through it) is
// followed along the neighbours' rays: 1 / depth of the plane along ray r is dot(nw, r) / dot(nw, Prel). (The screen
// gradient of the depth would do on smooth surfaces, but at the edges of objects the 2x2 quad mixes two surfaces.)
float ThinWeight(float2 uv, float2 dUv, float depth, float3 Prel, float3 ray, float3 dRayX, float3 dRayY, float3 nw)
{
    float n = floor(clamp(FOLIAGE_PROBE / max(depth * length(dRayX), 1e-6), 1.0, 32.0) + 0.5);
    float4 d = float4(DecodeDepth(tex2Dlod(NormalDepthSampler, float4(uv - float2(n * dUv.x, 0), 0, 0))),
                      DecodeDepth(tex2Dlod(NormalDepthSampler, float4(uv + float2(n * dUv.x, 0), 0, 0))),
                      DecodeDepth(tex2Dlod(NormalDepthSampler, float4(uv - float2(0, n * dUv.y), 0, 0))),
                      DecodeDepth(tex2Dlod(NormalDepthSampler, float4(uv + float2(0, n * dUv.y), 0, 0))));
    float4 inv = (d > 0) ? 1.0 / max(d, 1e-9) : 0.0;          // sky: infinitely far
    float planeN = dot(nw, Prel);
    float4 plane = float4(dot(nw, ray - n * dRayX), dot(nw, ray + n * dRayX),
                          dot(nw, ray - n * dRayY), dot(nw, ray + n * dRayY)) / planeN;
    float4 off = abs(inv - plane) * depth;                      // relative depth off the plane
    float thin = max(min(off.x, off.y), min(off.z, off.w));
    return saturate((thin - FOLIAGE_MIN) / FOLIAGE_RANGE);
}

float ContactShadow(float3 Prel, float3 ray, float depth, float3 nw, float3 dRayX, float3 dRayY,
                    float2 uv, float2 dUv, float noise)
{
    float rayLength = length(ray);
    float dist = depth * rayLength;
    if (depth <= 0 || dot(nw, SunDir) <= 0 || dist > CONTACT_FADE_START + CONTACT_FADE_LENGTH || rayLength < 1e-3)
        return 1.0;

    float3 c12 = cross(dRayX, dRayY), c20 = cross(dRayY, ray), c01 = cross(ray, dRayX);
    float det = dot(ray, c12);
    if (abs(det) < 1e-12)
        return 1.0;
    float3 r0 = c12 / det, r1 = c20 / det, r2 = c01 / det;
    float planeN = dot(nw, Prel);
    float bias = 0.02 / rayLength;

    // unrolled without break: a dynamic loop with break came out wrong once the rest of the pass was in the shader
    float occlusion = 0;
    [unroll] for (int i = 0; i < CONTACT_STEPS; i++) {
        float f = (i + noise) / CONTACT_STEPS;
        float s = f * sqrt(f) * CONTACT_LENGTH;             // denser near the receiver
        float3 q = Prel + SunDir * s;
        float t = dot(r0, q);
        float2 ab = float2(dot(r1, q), dot(r2, q)) / t;
        float2 uvq = uv + ab * dUv;
        float dq = DecodeDepth(tex2Dlod(NormalDepthSampler, float4(saturate(uvq), 0, 0)));
        float3 rq = ray + ab.x * dRayX + ab.y * dRayY;
        float plane = planeN / dot(nw, rq);                 // the receiver's own plane along that pixel's ray
        bool inside = t > 0 && all(uvq >= 0.0) && all(uvq <= 1.0);
        bool hit = inside && dq > 0 && dq < t - bias && dq < plane - bias && (t - dq) * rayLength < CONTACT_THICKNESS;
        occlusion = max(occlusion, hit ? 1.0 - s / CONTACT_LENGTH : 0.0);
    }
    float fade = saturate((dist - CONTACT_FADE_START) / CONTACT_FADE_LENGTH);
    fade = max(fade, ThinWeight(uv, dUv, depth, Prel, ray, dRayX, dRayY, nw));
    return 1.0 - saturate(occlusion * 1.5) * (1.0 - fade);
}
