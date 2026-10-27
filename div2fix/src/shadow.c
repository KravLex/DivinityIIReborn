/* Div2Fix - soft shadow edges (variance shadow map blur fix).
 *
 * The cascaded sun shadows are variance shadow maps (two G16R16F render targets, 4096 or 8192). After the
 * depth pass the game blurs the map with two full-screen ps_2_0 passes, 5 taps 1-4-6-4-1 /16, offsets from
 * c0 = -2t, c2 = t, c1 = +2t (captured: t = 1/8192):
 *   pass 1 (horizontal, weights in c3): reads depth .x of each tap and writes (mean z, mean z^2) - correct;
 *   pass 2 (vertical,   weights in c4): reads ONLY .x again and squares the already blurred mean, so the
 *     variance becomes ~0 and the lighting shader's Chebyshev test degenerates into a hard comparison:
 *     shadow edges stay hard and stair-stepped, and a wider blur only dilates the shadows (tested 27.10).
 * Fix: for pass 2 use a copy of the game's own shader whose tail blurs both moments (.x and .y) with the same
 * weights; both passes sample with linear filtering. Softness scales the tap distance (1 = the game's).
 * The shaders are recognised by bytecode. INI: [Graphics] ShadowSoftness=1 (0 = off).
 */
#include "common.h"

typedef long (WINAPI *CreateTexture_t)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*);
typedef long (WINAPI *SetRT_t)(void*, DWORD, void*);
typedef long (WINAPI *SetPS_t)(void*, void*);
typedef long (WINAPI *DIP_t)(void*, DWORD, int, UINT, UINT, UINT, UINT);
typedef long (WINAPI *DP_t)(void*, DWORD, UINT, UINT);
typedef long (WINAPI *SetPSC_t)(void*, UINT, const float*, UINT);
typedef long (WINAPI *SetSS_t)(void*, DWORD, DWORD, DWORD);
typedef unsigned long (WINAPI *Rel_t)(void*);
static CreateTexture_t g_oCT; static SetRT_t g_oRT; static SetPS_t g_oPS; static DIP_t g_oDIP; static DP_t g_oDP;
static SetPSC_t g_SetPSC; static SetSS_t g_SetSS;
static float g_soft = 1.0f;
static void* g_fixedPS; static int g_fixFail;
static void* g_surf[16]; static UINT g_size[16]; static int g_nsurf;
static UINT g_rtSize;                   /* size of the current render target 0 if it is a shadow map, else 0 */
static void* g_ps;                      /* current pixel shader */
static void* g_yes[8]; static int g_yesk[8]; static int g_nyes; static void* g_no[1024]; static int g_nno;
static int g_logged;

static void* g_lastPs; static int g_lastRes;
static int is_blur_slow(void* ps);
static int is_blur(void* ps)
{
    if (ps != g_lastPs) { g_lastPs = ps; g_lastRes = is_blur_slow(ps); }
    return g_lastRes;
}
static int is_blur_slow(void* ps)
{
    for (int i = 0; i < g_nyes; i++) if (g_yes[i] == ps) return g_yesk[i];
    for (int i = 0; i < g_nno; i++) if (g_no[i] == ps) return 0;
    int yes = 0;
    typedef long (WINAPI *GF_t)(void*, void*, UINT*);
    DWORD b[400]; UINT sz = 0;
    if (((GF_t)(*(void***)ps)[4])(ps, 0, &sz) >= 0 && sz >= 200 && sz <= sizeof b && ((GF_t)(*(void***)ps)[4])(ps, b, &sz) >= 0) {
        /* ps_2_0, def cN 0.0625 0.25 0.375 0.25 (weights), exactly 5 texld, constants c0/c1/c2 used */
        int n = sz / 4, w = 0, tex = 0, wreg = -1;
        if (b[0] == 0xFFFF0200)
            for (int i = 2; i < n - 4; i++) {
                if (b[i] == 0x3D800000 && b[i+1] == 0x3E800000 && b[i+2] == 0x3EC00000 && b[i+3] == 0x3E800000 &&
                    b[i-2] == 0x05000051) { w++; wreg = b[i-1] & 0x7FF; }
                if ((b[i] & 0xFFFF) == 0x42 && ((b[i] >> 24) & 0xF) == 3) tex++;
            }
        if (w == 1 && tex == 5) yes = wreg == 3 ? 1 : wreg == 4 ? 2 : 0;
    }
    if (yes) { if (g_nyes < 8) { g_yesk[g_nyes] = yes; g_yes[g_nyes++] = ps; } logf("ShadowSoft: blur shader %08X recognised (pass %d)", (DWORD)ps, yes); }
    else if (g_nno < 1024) g_no[g_nno++] = ps;
    return yes;
}

/* copy of the game's pass-2 shader up to its 5th texld, then: blur .xy of the taps r0(-2) r1(-1) r4(0) r2(+1) r3(+2)
   with the weights c3.y=1/16, c4.y=4/16, c4.z=6/16, c4.w=4/16, c3.y; z = c5.x (0), w = c5.y (1). */
static const DWORD k_tail[] = {
    0x03000005,0x80030005,0x80E40000,0xA0550003,                    /* mul r5.xy, r0, c3.y      */
    0x04000004,0x80030005,0x80E40001,0xA0550004,0x80E40005,         /* mad r5.xy, r1, c4.y, r5  */
    0x04000004,0x80030005,0x80E40004,0xA0AA0004,0x80E40005,         /* mad r5.xy, r4, c4.z, r5  */
    0x04000004,0x80030005,0x80E40002,0xA0FF0004,0x80E40005,         /* mad r5.xy, r2, c4.w, r5  */
    0x04000004,0x80030005,0x80E40003,0xA0550003,0x80E40005,         /* mad r5.xy, r3, c3.y, r5  */
    0x02000001,0x80040005,0xA0000005,                               /* mov r5.z, c5.x           */
    0x02000001,0x80080005,0xA0550005,                               /* mov r5.w, c5.y           */
    0x02000001,0x800F0800,0x80E40005,                               /* mov oC0, r5              */
    0x0000FFFF };
static void* make_fixed(void* dev, void* ps)
{
    typedef long (WINAPI *GF_t)(void*, void*, UINT*);
    static DWORD b[512]; UINT sz = 0;
    if (((GF_t)(*(void***)ps)[4])(ps, 0, &sz) < 0 || sz > 400 * 4 || ((GF_t)(*(void***)ps)[4])(ps, b, &sz) < 0) return 0;
    int n = sz / 4, i = 1, tex = 0, cut = 0;
    while (i < n) {
        DWORD tk = b[i], op = tk & 0xFFFF;
        if (tk == 0xFFFF) break;
        if (op == 0xFFFE) { i += 1 + ((tk >> 16) & 0x7FFF); continue; }
        i += 1 + ((tk >> 24) & 0xF);
        if (op == 0x42 && ++tex == 5) { cut = i; break; }
    }
    if (!cut || cut + (int)(sizeof k_tail / 4) > 512) return 0;
    for (int k = 0; k < (int)(sizeof k_tail / 4); k++) b[cut + k] = k_tail[k];
    void* out = 0; typedef long (WINAPI *CPS_t)(void*, const DWORD*, void**);
    long hr = ((CPS_t)(*(void***)dev)[106])(dev, b, &out);
    logf("ShadowSoft: variance-correct blur shader %s (hr %08X)", hr >= 0 && out ? "created" : "FAILED", (DWORD)hr);
    return hr >= 0 ? out : 0;
}

/* Bloom: the HDR bloom chain (DXSDK HDRLighting layout) renders into X8R8G8B8 targets of backbuffer/4+2,
   backbuffer/8+2 and backbuffer/8; magnified 8x onto 3440x1440 they show as blocky patches. With
   [Graphics] BloomScale=2 these targets are created twice as large; the chain derives its texture rects and
   sample offsets from the texture sizes, so it adapts. */
static int g_bloomScale = 1;
static void* g_ls[8]; static UINT g_lsw[8], g_lsh[8]; static int g_nls; static UINT g_lsRTw, g_lsRTh; static int g_vplog;
static UINT bb_w(void* dev, UINT* hgt)
{
    void* bb = 0; typedef long (WINAPI *GBB_t)(void*, UINT, UINT, DWORD, void**); typedef long (WINAPI *GD_t)(void*, void*);
    struct { DWORD fmt, type, usage, pool, ms, msq; UINT w, h; } d = { 0 };
    if (((GBB_t)(*(void***)dev)[18])(dev, 0, 0, 0, &bb) >= 0 && bb) { ((GD_t)(*(void***)bb)[12])(bb, &d); ((Rel_t)(*(void***)bb)[2])(bb); }
    *hgt = d.h; return d.w;
}
typedef long (WINAPI *CreateDS_t)(void*, UINT, UINT, DWORD, DWORD, DWORD, int, void**, void*);
static CreateDS_t g_oDS;
static long WINAPI hk_DS(void* dev, UINT w, UINT h, DWORD fmt, DWORD ms, DWORD q, int disc, void** out, void* sh)
{
    if (g_bloomScale > 1 && w < 2048) {
        UINT bh, bw = bb_w(dev, &bh);
        if (bw && w == bw / 4 && h == bh / 4) {
            long r = g_oDS(dev, w * g_bloomScale, h * g_bloomScale, fmt, ms, q, disc, out, sh);
            logf("LightShafts: depth surface %ux%u -> %ux%u (hr %08X)", w, h, w * g_bloomScale, h * g_bloomScale, (DWORD)r);
            if (r >= 0) return r;
        }
    }
    return g_oDS(dev, w, h, fmt, ms, q, disc, out, sh);
}
static int before_draw(void* dev)          /* returns 1 if the pixel shader was swapped for this draw */
{
    if (g_lsRTw) {                          /* scaled light-shaft target: the game sets a viewport of the old size */
        struct { DWORD x, y, w, h; float mn, mx; } vp; typedef long (WINAPI *VP_t)(void*, void*);
        if (((VP_t)(*(void***)dev)[48])(dev, &vp) >= 0 && vp.w * g_bloomScale == g_lsRTw && vp.h * g_bloomScale == g_lsRTh) {
            if (g_vplog < 3) { g_vplog++; logf("LightShafts: viewport %ux%u at %u,%u -> %ux%u", vp.w, vp.h, vp.x, vp.y, g_lsRTw, g_lsRTh); }
            vp.x *= g_bloomScale; vp.y *= g_bloomScale; vp.w = g_lsRTw; vp.h = g_lsRTh;
            ((VP_t)(*(void***)dev)[47])(dev, &vp);
        }
    }
    if (!g_soft || !g_rtSize || !g_ps) return 0;
    int k = is_blur(g_ps);
    if (!k) return 0;
    float t = g_soft / 8192.0f;
    float c[12] = { -2*t, 0, 0, 0,  2*t, 0, 0, 0,  t, 0, 0, 0 };
    g_SetPSC(dev, 0, c, 3);
    g_SetSS(dev, 0, 5, 2); g_SetSS(dev, 0, 6, 2);               /* MAG/MIN = LINEAR */
    if (g_logged < 2) { g_logged++; logf("ShadowSoft: blur pass %d on %u map", k, g_rtSize); }
    if (k == 2) {
        if (!g_fixedPS && !g_fixFail) { g_fixedPS = make_fixed(dev, g_ps); if (!g_fixedPS) g_fixFail = 1; }
        if (g_fixedPS) { g_oPS(dev, g_fixedPS); return 1; }
    }
    return 0;
}

static long WINAPI hk_CT(void* dev, UINT w, UINT h, UINT lv, DWORD usage, DWORD fmt, DWORD pool, void** out, void* sh)
{
    if (g_bloomScale > 1 && (usage & 1) && fmt == 0x15 && w < 2048) {      /* light shafts: A8R8G8B8 backbuffer/4 */
        UINT bh, bw = bb_w(dev, &bh);
        if (bw && w == bw / 4 && h == bh / 4) {
            long r = g_oCT(dev, w * g_bloomScale, h * g_bloomScale, lv, usage, fmt, pool, out, sh);
            logf("LightShafts: render target %ux%u -> %ux%u (hr %08X)", w, h, w * g_bloomScale, h * g_bloomScale, (DWORD)r);
            if (r >= 0) {
                void* s = 0; typedef long (WINAPI *GSL_t)(void*, UINT, void**);
                if (((GSL_t)(*(void***)*out)[18])(*out, 0, &s) >= 0 && s) {
                    int slot = g_nls < 8 ? g_nls++ : (g_nls = 1, 0);
                    g_ls[slot] = s; g_lsw[slot] = w * g_bloomScale; g_lsh[slot] = h * g_bloomScale;
                    ((Rel_t)(*(void***)s)[2])(s);
                }
                return r;
            }
        }
    }
    if (g_bloomScale > 1 && (usage & 1) && fmt == 0x16 && w < 2048) {
        UINT bh, bw = bb_w(dev, &bh);
        if (bw && ((w == bw / 4 + 2 && h == bh / 4 + 2) || (w == bw / 8 + 2 && h == bh / 8 + 2) || (w == bw / 8 && h == bh / 8))) {
            UINT nw = w * g_bloomScale, nh = h * g_bloomScale;
            if (w == bw / 4 + 2) { nw = (bw / 4) * g_bloomScale + 2; nh = (bh / 4) * g_bloomScale + 2; }
            else if (w == bw / 8 + 2) { nw = (bw / 8) * g_bloomScale + 2; nh = (bh / 8) * g_bloomScale + 2; }
            long r = g_oCT(dev, nw, nh, lv, usage, fmt, pool, out, sh);
            logf("Bloom: render target %ux%u -> %ux%u (hr %08X)", w, h, nw, nh, (DWORD)r);
            if (r >= 0) return r;
        }
    }
    long hr = g_oCT(dev, w, h, lv, usage, fmt, pool, out, sh);
    if (hr >= 0 && out && *out && w >= 1024 && w == h && (usage & 1) && fmt == 0x70 /* G16R16F */) {
        void* s = 0; typedef long (WINAPI *GSL_t)(void*, UINT, void**);
        if (((GSL_t)(*(void***)*out)[18])(*out, 0, &s) >= 0 && s) {
            int slot = g_nsurf < 16 ? g_nsurf++ : (g_nsurf = 1, 0);
            g_surf[slot] = s; g_size[slot] = w;
            ((Rel_t)(*(void***)s)[2])(s);                       /* the texture keeps the surface alive */
        }
    }
    return hr;
}
static long WINAPI hk_RT(void* dev, DWORD idx, void* s)
{
    if (idx == 0) { g_rtSize = 0; for (int i = 0; i < g_nsurf; i++) if (g_surf[i] == s) g_rtSize = g_size[i];
                    g_lsRTw = 0; for (int i = 0; i < g_nls; i++) if (g_ls[i] == s) { g_lsRTw = g_lsw[i]; g_lsRTh = g_lsh[i]; } }
    return g_oRT(dev, idx, s);
}
static long WINAPI hk_PS(void* dev, void* ps) { g_ps = ps; return g_oPS(dev, ps); }
static long WINAPI hk_DIP(void* d, DWORD a, int b, UINT c, UINT e, UINT f, UINT g) { int sw = before_draw(d); long hr = g_oDIP(d, a, b, c, e, f, g); if (sw) g_oPS(d, g_ps); return hr; }
static long WINAPI hk_DP(void* d, DWORD a, UINT b, UINT c) { int sw = before_draw(d); long hr = g_oDP(d, a, b, c); if (sw) g_oPS(d, g_ps); return hr; }

static float ini_soft(void)
{
    char v[32]; GetPrivateProfileStringA("Graphics", "ShadowSoftness", "1", v, sizeof v, g_ini);
    float f = 0; int frac = 0, div = 1; const char* p = v;
    if (*p == '-') return 0;
    while (*p >= '0' && *p <= '9') f = f * 10 + (*p++ - '0');
    if (*p == '.' || *p == ',') { p++; while (*p >= '0' && *p <= '9' && div < 100000) { frac = frac * 10 + (*p++ - '0'); div *= 10; } }
    f += (float)frac / div;
    return f <= 0.01f ? 0 : f > 6 ? 6 : f;
}
static int ini_bloom(void)
{
    int b = GetPrivateProfileIntA("Graphics", "BloomScale", 2, g_ini);
    return b < 1 ? 1 : b > 4 ? 4 : b;
}
/* called from diag.c: does this module need the D3D9 device at all? */
int shadow_wanted(void) { return ini_soft() > 0 || ini_bloom() > 1; }

void shadow_on_device(void* dev)
{
    if (g_oCT) return;
    g_soft = ini_soft(); g_bloomScale = ini_bloom();
    if (!g_soft && g_bloomScale < 2) { logf("ShadowSoft/Bloom: off (ini)"); return; }
    void** vt = *(void***)dev;
    g_SetPSC = (SetPSC_t)vt[109]; g_SetSS = (SetSS_t)vt[69];
    void* a = detour(vt[23], (void*)hk_CT);
    if (!a) { logf("ShadowSoft/Bloom: CreateTexture hook failed -> off"); return; }
    g_oCT = (CreateTexture_t)a;
    g_oRT = (SetRT_t)detour(vt[37], (void*)hk_RT);
    g_oDS = (CreateDS_t)detour(vt[29], (void*)hk_DS);
    g_oPS = (SetPS_t)detour(vt[107], (void*)hk_PS);
    g_oDIP = (DIP_t)detour(vt[82], (void*)hk_DIP);
    g_oDP = (DP_t)detour(vt[81], (void*)hk_DP);
    if (!g_oRT || !g_oPS || !g_oDIP || !g_oDP) logf("ShadowSoft/Bloom: some draw hooks failed (RT %d PS %d DIP %d DP %d)", !!g_oRT, !!g_oPS, !!g_oDIP, !!g_oDP);
    logf("ShadowSoft: %s (softness %d.%02d); Bloom/LightShafts scale x%d", g_soft > 0 ? "on" : "off", (int)g_soft, (int)(g_soft * 100) % 100, g_bloomScale);
}
