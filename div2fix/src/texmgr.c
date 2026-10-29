/*
 * Div2Fix - texture manager, stage 1: telemetry (does not change what the game does).
 *
 * Why: the frame-pacing reports show that most frame-time spikes come from textures being created and filled on the
 * render thread the first time they are used (NiDX9SourceTextureData: CreateTexture, LockRect, memcpy per mip level,
 * UnlockRect; DXVK copies again on unlock). Before changing that path we need exact numbers: how many textures,
 * which sizes / formats / pools, which thread, and where the time goes - in the game's copy (between LockRect and
 * UnlockRect) or in the D3D9 runtime (CreateTexture / LockRect / UnlockRect themselves).
 *
 * Hooks (detours, chained behind shadow.c's CreateTexture detour):
 *   IDirect3DDevice9::CreateTexture (vtbl 23)
 *   IDirect3DTexture9::Release (2), LockRect (19), UnlockRect (20)
 *   IDirect3DSurface9::LockRect (13), UnlockRect (14)   (Gamebryo fills levels through GetSurfaceLevel)
 * Only 2D textures are tracked (no cube / volume textures).
 *
 * Output: with [Diagnostics] Enable=1 and [Textures] Stats=1, every frame-time recording (F11 or automatic, see
 * diag.c) also writes Div2Fix_tex_<time>.txt (summary) and .csv (per frame) next to the game.
 */
#include "common.h"

#define D3DFMT_DXT1 0x31545844
#define D3DFMT_DXT2 0x32545844
#define D3DFMT_DXT3 0x33545844
#define D3DFMT_DXT4 0x34545844
#define D3DFMT_DXT5 0x35545844

typedef long (WINAPI *CT_t)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*);
typedef unsigned long (WINAPI *Rel_t)(void*);
typedef long (WINAPI *TLock_t)(void*, UINT, void*, const void*, DWORD);
typedef long (WINAPI *TUnlock_t)(void*, UINT);
typedef long (WINAPI *SLock_t)(void*, void*, const void*, DWORD);
typedef long (WINAPI *SUnlock_t)(void*);
typedef long (WINAPI *GSL_t)(void*, UINT, void**);
typedef long (WINAPI *SDesc_t)(void*, void*);
typedef struct { DWORD fmt, type, usage, pool, ms, msq; UINT w, h; } SurfDesc;

static int g_on;
static CT_t g_oCT; static Rel_t g_oTRel; static TLock_t g_oTLock; static TUnlock_t g_oTUnlock;
static SLock_t g_oSLock; static SUnlock_t g_oSUnlock;
static long long g_qpf;
static volatile long g_lock;            /* spin lock for the tables below */
static volatile DWORD g_renderTid;      /* thread that calls Present */

static void lk(void) { while (__sync_lock_test_and_set(&g_lock, 1)) Sleep(0); }
static void ulk(void) { __sync_lock_release(&g_lock); }
static long long now(void) { long long t; QueryPerformanceCounter(&t); return t; }
/* no 64-bit integer division without the C runtime (__alldiv): use double, like diag.c */
static DWORD us(long long dt) { return (DWORD)((double)dt * 1000000.0 / (double)g_qpf); }

/* ---------------- sizes ---------------- */
static DWORD level_bytes(DWORD fmt, UINT w, UINT h)
{
    if (!w) w = 1; if (!h) h = 1;
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (fmt) {
    case D3DFMT_DXT1: return bw * bh * 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return bw * bh * 16;
    case 23: case 24: case 25: case 26: case 51: case 111: return w * h * 2;   /* 16-bit formats, R16F */
    case 28: case 50: return w * h;                                             /* A8, L8 */
    case 36: case 113: case 115: return w * h * 8;                              /* 64-bit formats */
    case 116: return w * h * 16;                                                /* A32B32G32R32F */
    default: return w * h * 4;
    }
}
static DWORD tex_bytes(DWORD fmt, UINT w, UINT h, UINT levels)
{
    DWORD s = 0; UINT n = 0;
    for (;;) {
        s += level_bytes(fmt, w, h); n++;
        if ((levels && n >= levels) || (w <= 1 && h <= 1)) break;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return s;
}
static int fmt_class(DWORD fmt)         /* 0 DXT1, 1 DXT3, 2 DXT5, 3 32-bit RGBA, 4 other */
{
    return fmt == D3DFMT_DXT1 ? 0 : fmt == D3DFMT_DXT3 ? 1 : fmt == D3DFMT_DXT5 ? 2 : (fmt == 21 || fmt == 22) ? 3 : 4;
}
static const char* k_fmt[] = { "DXT1", "DXT3", "DXT5", "A8R8G8B8/X8R8G8B8", "other" };
static int side_class(UINT s) { return s <= 256 ? 0 : s <= 512 ? 1 : s <= 1024 ? 2 : s <= 2048 ? 3 : s <= 4096 ? 4 : 5; }
static const char* k_side[] = { "<=256", "512", "1024", "2048", "4096", ">4096" };

/* ---------------- live textures (pointer -> bytes | pool) ---------------- */
#define LIVE_N 65536                    /* open addressing, power of two */
typedef struct { void* p; DWORD bytes; DWORD pool; } Live;
static Live* g_live;
static DWORD g_liveBytes[4], g_livePeak[4], g_liveCount, g_liveFull;
#define TOMB ((void*)1)
static DWORD hsh(void* p) { DWORD x = (DWORD)p; x ^= x >> 16; x *= 0x7FEB352D; x ^= x >> 15; return x & (LIVE_N - 1); }
static void live_add(void* p, DWORD bytes, DWORD pool)
{
    if (pool > 3) pool = 3;
    DWORD i = hsh(p);
    for (int k = 0; k < LIVE_N; k++, i = (i + 1) & (LIVE_N - 1))
        if (!g_live[i].p || g_live[i].p == TOMB || g_live[i].p == p) {
            if (g_live[i].p == p) g_liveBytes[g_live[i].pool] -= g_live[i].bytes; else g_liveCount++;
            g_live[i].p = p; g_live[i].bytes = bytes; g_live[i].pool = pool;
            g_liveBytes[pool] += bytes;
            if (g_liveBytes[pool] > g_livePeak[pool]) g_livePeak[pool] = g_liveBytes[pool];
            return;
        }
    g_liveFull++;
}
static void live_del(void* p)
{
    DWORD i = hsh(p);
    for (int k = 0; k < LIVE_N && g_live[i].p; k++, i = (i + 1) & (LIVE_N - 1))
        if (g_live[i].p == p) { g_liveBytes[g_live[i].pool] -= g_live[i].bytes; g_liveCount--; g_live[i].p = TOMB; return; }
}

/* ---------------- per-frame accumulators ---------------- */
typedef struct {
    DWORD createN, createKB, createUs, offThreadN;   /* CreateTexture */
    DWORD lockN, lockKB, lockUs, copyUs, unlockUs;   /* lock = runtime LockRect, copy = game between lock/unlock, unlock = runtime UnlockRect */
    DWORD engUs, engDraw, engQuery, engOther;        /* NiDX9SourceTextureData::Create: inclusive time, calls by path */
    DWORD deferN, querySkipN;                        /* upload budget: draw-prepare calls deferred, queries answered without creating */
} Frame;
static Frame g_cur;                     /* current frame (updated with atomics) */
static Frame* g_fr; static int g_maxFr, g_nFr;
static DWORD g_liveStart[4];

/* totals since device creation and per recording */
typedef struct { DWORD n, kb; } NK;
#define g_byPool g_tc.pool
#define g_byFmt  g_tc.fmt
#define g_bySide g_tc.side
typedef struct { DWORD n, kb, lockUs, copyUs, unlockUs; } LockCls;   /* by size of the locked level */
/* Breakdowns are collected per frame (g_pc) and added to the recording totals (g_tc) only for frames that diag.c
   stores - like the per-frame arrays - so the sections of the report always add up to the same totals. */
typedef struct { NK pool[4], fmt[5], side[6]; LockCls lock[5]; DWORD engCalls[4], engUs[4]; DWORD engMax[4]; } Cats;
static Cats g_pc, g_tc;
#define g_lockCls g_tc.lock
#define g_engCalls g_tc.engCalls
#define g_engUs g_tc.engUs
#define g_engMaxUs g_tc.engMax
static void cats_frame(int stored)
{
    if (stored) {
        DWORD* s = (DWORD*)&g_pc; DWORD* d = (DWORD*)&g_tc;
        int nsum = (int)(__builtin_offsetof(Cats, engMax) / 4);   /* everything before engMax is a sum */
        for (int i = 0; i < nsum; i++) d[i] += s[i];
        for (int i = 0; i < 4; i++) if (g_pc.engMax[i] > g_tc.engMax[i]) g_tc.engMax[i] = g_pc.engMax[i];
    }
    memset(&g_pc, 0, sizeof g_pc);
}
static const char* k_lcls[] = { "<64 KB", "64 KB-1 MB", "1-4 MB", "4-16 MB", ">=16 MB" };
static int lcls(DWORD bytes) { return bytes < 65536 ? 0 : bytes < (1u << 20) ? 1 : bytes < (4u << 20) ? 2 : bytes < (16u << 20) ? 3 : 4; }

static void add(volatile DWORD* a, DWORD v) { __sync_fetch_and_add(a, v); }

/* pending locks: (object, level) -> time the runtime returned from LockRect, bytes of the level */
typedef struct { void* obj; UINT level; long long t; DWORD bytes; } Pend;
static Pend g_pend[64];
static void pend_put(void* o, UINT lv, long long t, DWORD b)
{
    lk();
    int free_ = -1;
    for (int i = 0; i < 64; i++) { if (g_pend[i].obj == o && g_pend[i].level == lv) { free_ = i; break; } if (!g_pend[i].obj && free_ < 0) free_ = i; }
    if (free_ >= 0) { g_pend[free_].obj = o; g_pend[free_].level = lv; g_pend[free_].t = t; g_pend[free_].bytes = b; }
    ulk();
}
static int pend_take(void* o, UINT lv, long long* t, DWORD* b)
{
    int ok = 0; lk();
    for (int i = 0; i < 64; i++) if (g_pend[i].obj == o && g_pend[i].level == lv) { *t = g_pend[i].t; *b = g_pend[i].bytes; g_pend[i].obj = 0; ok = 1; break; }
    ulk(); return ok;
}

static void note_lock(DWORD bytes, DWORD lockUs)
{
    add(&g_cur.lockN, 1); add(&g_cur.lockKB, bytes >> 10); add(&g_cur.lockUs, lockUs);
    LockCls* c = &g_pc.lock[lcls(bytes)]; add(&c->n, 1); add(&c->kb, bytes >> 10); add(&c->lockUs, lockUs);
}
static void note_unlock(DWORD bytes, DWORD copyUs, DWORD unlockUs)
{
    add(&g_cur.copyUs, copyUs); add(&g_cur.unlockUs, unlockUs);
    LockCls* c = &g_pc.lock[lcls(bytes)]; add(&c->copyUs, copyUs); add(&c->unlockUs, unlockUs);
}

/* ---------------- hooks ---------------- */
static void hook_texture_vtable(void* tex);

static long WINAPI hk_CT(void* dev, UINT w, UINT h, UINT lv, DWORD usage, DWORD fmt, DWORD pool, void** out, void* sh)
{
    long long t0 = now();
    long hr = g_oCT(dev, w, h, lv, usage, fmt, pool, out, sh);
    DWORD dt = us(now() - t0);
    if (hr < 0 || !out || !*out) return hr;
    if (usage & 3) {                     /* render-target / depth-stencil textures: log each distinct kind once */
        static DWORD seen[64][4]; static int nseen;
        int dup = 0; for (int i = 0; i < nseen; i++) if (seen[i][0] == w && seen[i][1] == h && seen[i][2] == fmt && seen[i][3] == usage) dup = 1;
        if (!dup && nseen < 64) {
            seen[nseen][0] = w; seen[nseen][1] = h; seen[nseen][2] = fmt; seen[nseen][3] = usage; nseen++;
            /* who creates it: return addresses into the game found on our stack (value after a CALL) */
            char cs[160]; int cn = 0, cp = 0; cs[0] = 0;
            Module mm = main_module(); DWORD lo = (DWORD)mm.base, hi = lo + mm.size;
            const DWORD* sp = (const DWORD*)__builtin_frame_address(0);
            for (int i = 0; i < 400 && cn < 6 && readable(sp + i, 4); i++) {
                DWORD v = sp[i];
                if (v - lo >= hi - lo || !readable((void*)(v - 6), 6)) continue;
                const BYTE* q = (const BYTE*)v;
                if (q[-5] != 0xE8 && !(q[-6] == 0xFF && q[-5] == 0x15) && !(q[-2] == 0xFF && (q[-1] & 0xF8) == 0xD0) && !(q[-3] == 0xFF && (q[-2] & 0xF8) == 0x50)) continue;
                cp += wsprintfA(cs + cp, " %08X", v - lo + 0x400000); cn++;          /* as VA of the unrelocated exe */
            }
            logf("Textures: %s texture %ux%u fmt %08X usage %08X levels %u (first of this kind); game callers:%s",
                 (usage & 2) ? "depth-stencil" : "render-target", w, h, fmt, usage, lv, cs);
        }
    }
    if (!g_oTLock) hook_texture_vtable(*out);
    DWORD b = tex_bytes(fmt, w, h, lv);
    lk(); live_add(*out, b, pool); ulk();
    add(&g_cur.createN, 1); add(&g_cur.createKB, b >> 10); add(&g_cur.createUs, dt);
    if (g_renderTid && GetCurrentThreadId() != g_renderTid) add(&g_cur.offThreadN, 1);
    NK* a = &g_pc.pool[pool > 3 ? 3 : pool]; add(&a->n, 1); add(&a->kb, b >> 10);
    a = &g_pc.fmt[fmt_class(fmt)]; add(&a->n, 1); add(&a->kb, b >> 10);
    a = &g_pc.side[side_class(w > h ? w : h)]; add(&a->n, 1); add(&a->kb, b >> 10);
    return hr;
}

static unsigned long WINAPI hk_TRel(void* tex)
{
    lk();                                /* hold the lock across Release: the address may be reused right after */
    unsigned long r = g_oTRel(tex);
    if (r == 0) live_del(tex);
    ulk();
    return r;
}

static DWORD tex_level_bytes(void* tex, UINT level)
{
    SurfDesc d; typedef long (WINAPI *GLD_t)(void*, UINT, SurfDesc*);
    if (((GLD_t)(*(void***)tex)[17])(tex, level, &d) < 0) return 0;
    return level_bytes(d.fmt, d.w, d.h);
}
/* Runtimes may implement Texture::LockRect by calling the level surface's LockRect (Microsoft's d3d9 does). Our
   detours sit on the functions themselves, so that inner call would be counted twice: a per-thread flag marks
   "inside a texture lock/unlock" and the surface hooks then pass straight through. */
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static int inner(void) { return g_tls != TLS_OUT_OF_INDEXES && TlsGetValue(g_tls) != 0; }
static void set_inner(int v) { if (g_tls != TLS_OUT_OF_INDEXES) TlsSetValue(g_tls, (void*)v); }

static long WINAPI hk_TLock(void* tex, UINT level, void* lr, const void* rc, DWORD flags)
{
    long long t0 = now();
    set_inner(1);
    long hr = g_oTLock(tex, level, lr, rc, flags);
    set_inner(0);
    long long t1 = now();
    if (hr >= 0) { DWORD b = tex_level_bytes(tex, level); note_lock(b, us(t1 - t0)); pend_put(tex, level, t1, b); }
    return hr;
}
static long WINAPI hk_TUnlock(void* tex, UINT level)
{
    long long t0 = now(), tl; DWORD b;
    int had = pend_take(tex, level, &tl, &b);
    set_inner(1);
    long hr = g_oTUnlock(tex, level);
    set_inner(0);
    if (had) note_unlock(b, us(t0 - tl), us(now() - t0));
    return hr;
}
static long WINAPI hk_SLock(void* s, void* lr, const void* rc, DWORD flags)
{
    if (inner()) return g_oSLock(s, lr, rc, flags);
    long long t0 = now();
    long hr = g_oSLock(s, lr, rc, flags);
    long long t1 = now();
    if (hr >= 0) {
        SurfDesc d; DWORD b = ((SDesc_t)(*(void***)s)[12])(s, &d) >= 0 ? level_bytes(d.fmt, d.w, d.h) : 0;
        note_lock(b, us(t1 - t0)); pend_put(s, 0xFFFFFFFF, t1, b);
    }
    return hr;
}
static long WINAPI hk_SUnlock(void* s)
{
    if (inner()) return g_oSUnlock(s);
    long long t0 = now(), tl; DWORD b;
    int had = pend_take(s, 0xFFFFFFFF, &tl, &b);
    long hr = g_oSUnlock(s);
    if (had) note_unlock(b, us(t0 - tl), us(now() - t0));
    return hr;
}

static void* hook_fn(void** vt, int i, void* hook, const char* what)
{
    void* tr = detour(vt[i], hook);
    if (tr) return tr;
    void* prev = vt[i];                  /* detour refused: fall back to the vtable slot */
    if (write_mem(&vt[i], &hook, 4)) return prev;
    logf("Textures: hook %s failed", what);
    return 0;
}
static void hook_texture_vtable(void* tex)
{
    void** vt = *(void***)tex;
    g_oTRel = (Rel_t)hook_fn(vt, 2, (void*)hk_TRel, "Texture::Release");
    g_oTLock = (TLock_t)hook_fn(vt, 19, (void*)hk_TLock, "Texture::LockRect");
    g_oTUnlock = (TUnlock_t)hook_fn(vt, 20, (void*)hk_TUnlock, "Texture::UnlockRect");
    void* s = 0;
    if (((GSL_t)vt[18])(tex, 0, &s) >= 0 && s) {
        void** sv = *(void***)s;
        g_oSLock = (SLock_t)hook_fn(sv, 13, (void*)hk_SLock, "Surface::LockRect");
        g_oSUnlock = (SUnlock_t)hook_fn(sv, 14, (void*)hk_SUnlock, "Surface::UnlockRect");
        ((Rel_t)sv[2])(s);
    }
    logf("Textures: texture hooks %s (Release %d, LockRect %d/%d, UnlockRect %d/%d)",
         g_oTRel && g_oTLock && g_oTUnlock && g_oSLock && g_oSUnlock ? "active" : "PARTIAL",
         !!g_oTRel, !!g_oTLock, !!g_oSLock, !!g_oTUnlock, !!g_oSUnlock);
}

/* ---------------- engine side: NiDX9SourceTextureData::Create ----------------
 * Divinity2.exe v1.4.700.56, 0x5D9FC0: static NiDX9SourceTextureData* __cdecl Create(NiTexture*, NiDX9Renderer*).
 * Loads/converts the pixel data, creates the D3D texture, fills every level, links texture->rendererData
 * ([tex+0x24]) and returns the new object. Its three callers (found by disassembly) are:
 *   0x5C128D  renderer precache (vtable 0x1308A1C): checks the result, NULL = "not precached"
 *   0x5E1B56  texture preparation for drawing (vtable 0x130B244): uses width/height/flags/D3D texture at once
 *   0x5E1C61  property query (vtable 0x130B248): reads flags of the result at once
 * Stage 1 only measures which path creates the textures and the inclusive engine-side time, to decide where a
 * per-frame budget can be applied (only the draw path can safely be deferred). */
#define ENG_RET_PRECACHE 0x5C128D
#define ENG_RET_DRAW     0x5E1B56
#define ENG_RET_QUERY    0x5E1C61
typedef void* (__cdecl *EngCreate_t)(void*, void*);
static EngCreate_t g_oEng;
static Module g_m;
/* by path: 0 precache, 1 draw, 2 query, 3 other - g_pc.engCalls/engUs/engMax, totals in g_tc (see Cats) */
static const char* k_path[] = { "precache (0x5C128D)", "draw prepare (0x5E1B56)", "property query (0x5E1C61)", "other" };
#define EVA(a) ((DWORD)g_m.base + ((DWORD)(a) - 0x400000))

static void* __cdecl hk_Eng(void* tex, void* rnd)
{
    DWORD ret = (DWORD)__builtin_return_address(0);
    int path = ret == EVA(ENG_RET_PRECACHE) ? 0 : ret == EVA(ENG_RET_DRAW) ? 1 : ret == EVA(ENG_RET_QUERY) ? 2 : 3;
    long long t0 = now();
    void* r = g_oEng(tex, rnd);
    DWORD dt = us(now() - t0);
    add(&g_cur.engUs, dt);
    add(path == 1 ? &g_cur.engDraw : path == 2 ? &g_cur.engQuery : &g_cur.engOther, 1);
    add(&g_pc.engCalls[path], 1); add(&g_pc.engUs[path], dt);
    if (dt > g_pc.engMax[path]) g_pc.engMax[path] = dt;
    return r;
}

/* ---------------- upload budget (stage 2, [Textures] UploadBudgetMs > 0) ----------------
 * Measured (28.10 04:51): 387 of 397 textures were created by the property query 0x5E1C10, not by the draw path.
 * That query only asks whether a texture is usable: for texture data of NiSourceTexture (flags word +0x62 = bit 0;
 * bits 1/2 are set only by the rendered/dynamic texture data constructors 0x5DE210/0x5DE400/0x5DE740/0x5DE9DC)
 * it always returns true. So with the budget on:
 *   1. query 0x5E1C10 (thiscall, 3 args): a source texture without renderer data -> answer true, create nothing;
 *   2. draw prepare 0x5E1AF0 (thiscall: texture, 3 out-bytes): if this frame already spent the budget inside Create
 *      and the texture has no renderer data yet -> clear the out-bytes and return 0, exactly what the function itself
 *      returns for a texture without renderer data that it cannot create. The object is drawn without that texture
 *      for this frame and the engine asks again next frame.
 * Guarantees: at least one texture is created per frame; a texture is never deferred more than MaxDeferFrames frames;
 * precache is never touched. */
typedef char (__fastcall *Query_t)(void* self, void* edx, void* tex, DWORD a2, DWORD a3);
typedef void* (__fastcall *DrawPrep_t)(void* self, void* edx, void* tex, BYTE* o1, BYTE* o2, BYTE* o3);
typedef void* (__fastcall *IsKindOf_t)(void* obj, void* edx, void* rtti);
static Query_t g_oQuery; static DrawPrep_t g_oDraw;
static DWORD g_budgetUs, g_maxDefer = 4, g_frameNo;
static DWORD g_deferTotal, g_deferTex, g_forced, g_maxAge, g_querySkipTotal;
typedef struct { void* tex; DWORD first; long long t0; } Def;
#define MAX_DEFER_US 250000                          /* hard cap in time too: if Present stops ticking, frames do not advance */
static Def g_def[512];                               /* textures currently being deferred (render thread only) */

/* the pointer comes from the engine, which dereferences it itself; created textures cost one read here */
static int is_source_without_data(void* tex)
{
    if (!tex || *(void**)((BYTE*)tex + 0x24)) return 0;
    return ((IsKindOf_t)EVA(0x459420))(tex, 0, (void*)EVA(0x14EF5D8)) != 0;   /* NiSourceTexture RTTI */
}
static int defer_find(void* tex) { for (int i = 0; i < 512; i++) if (g_def[i].tex == tex) return i; return -1; }
static int defer_add(void* tex)
{
    for (int i = 0; i < 512; i++)
        if (!g_def[i].tex || g_frameNo - g_def[i].first > 600) {   /* free or stale (texture never drawn again) */
            g_def[i].tex = tex; g_def[i].first = g_frameNo; g_def[i].t0 = now(); g_deferTex++; return 1;
        }
    return 0;                                         /* table full: create now */
}

static char __fastcall hk_Query(void* self, void* edx, void* tex, DWORD a2, DWORD a3)
{
    if (is_source_without_data(tex)) { add(&g_cur.querySkipN, 1); g_querySkipTotal++; return 1; }
    return g_oQuery(self, edx, tex, a2, a3);
}
static void* __fastcall hk_Draw(void* self, void* edx, void* tex, BYTE* o1, BYTE* o2, BYTE* o3)
{
    if (is_source_without_data(tex)) {
        int i = defer_find(tex);
        DWORD age = i >= 0 ? g_frameNo - g_def[i].first : 0;
        int young = i < 0 || (age < g_maxDefer && us(now() - g_def[i].t0) < MAX_DEFER_US);
        DWORD created = g_cur.engDraw + g_cur.engQuery + g_cur.engOther;
        if (created && g_cur.engUs >= g_budgetUs && young && (i >= 0 || defer_add(tex))) {
            *o1 = 0; *o2 = 0; *o3 = 0;                /* as the function does for a texture it cannot prepare */
            add(&g_cur.deferN, 1); g_deferTotal++;
            return 0;
        }
        if (i >= 0) { if (!young) g_forced++; if (age > g_maxAge) g_maxAge = age; g_def[i].tex = 0; }
    }
    return g_oDraw(self, edx, tex, o1, o2, o3);
}

/* called from DllMain: the engine function exists before any device */
void texmgr_init(Module m)
{
    g_m = m;
    QueryPerformanceFrequency(&g_qpf);
    if (!texmgr_wanted() || !GetPrivateProfileIntA("Diagnostics", "Enable", 0, g_ini)) return;
    BYTE* p = (BYTE*)EVA(0x5D9FC0);
    static const BYTE k_pro[] = { 0x83, 0xEC, 0x24, 0x57, 0x8B, 0x7C, 0x24, 0x2C };   /* sub esp,24h / push edi / mov edi,[esp+2Ch] */
    static const BYTE k_r1[] = { 0x83, 0xC4, 0x08 }, k_r2[] = { 0x8B, 0x54, 0x24 }, k_r3[] = { 0x83, 0xC4, 0x08 };
    int ok = readable(p, sizeof k_pro) && readable((void*)EVA(ENG_RET_DRAW), 4);
    for (int i = 0; ok && i < (int)sizeof k_pro; i++) if (p[i] != k_pro[i]) ok = 0;
    /* the three return addresses must follow "call 5D9FC0" */
    const BYTE* r1 = (const BYTE*)EVA(ENG_RET_PRECACHE); const BYTE* r2 = (const BYTE*)EVA(ENG_RET_DRAW); const BYTE* r3 = (const BYTE*)EVA(ENG_RET_QUERY);
    for (int i = 0; ok && i < 3; i++) if (r1[i] != k_r1[i] || r2[i] != k_r2[i] || r3[i] != k_r3[i]) ok = 0;
    if (ok && (r1[-5] != 0xE8 || r2[-5] != 0xE8 || r3[-5] != 0xE8)) ok = 0;
    if (!ok) { logf("Textures: engine Create site mismatch -> engine-side statistics off"); return; }
    g_oEng = (EngCreate_t)detour(p, (void*)hk_Eng);
    logf("Textures: engine-side statistics %s (NiDX9SourceTextureData::Create)", g_oEng ? "on" : "FAILED");
    if (!g_oEng) return;

    int ms = GetPrivateProfileIntA("Textures", "UploadBudgetMs", 0, g_ini);
    if (ms <= 0) { logf("Textures: upload budget off ([Textures] UploadBudgetMs=0)"); return; }
    if (ms > 50) ms = 50;
    int md = GetPrivateProfileIntA("Textures", "MaxDeferFrames", 4, g_ini);
    g_maxDefer = md < 1 ? 1 : md > 30 ? 30 : (DWORD)md;
    /* query 0x5E1C10: push ebx / mov ebx,[esp+8] / push ebp / mov ebp,ecx / test ebx,ebx
       draw prepare 0x5E1AF0: mov eax,[esp+8] / mov edx,[esp+10h] / push ebx / mov ebx,[esp+8] */
    static const BYTE k_q[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x8B, 0xE9, 0x85, 0xDB };
    static const BYTE k_d[] = { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x08 };
    const BYTE* q = (const BYTE*)EVA(0x5E1C10); const BYTE* dp = (const BYTE*)EVA(0x5E1AF0);
    ok = readable(q, sizeof k_q) && readable(dp, sizeof k_d);
    for (int i = 0; ok && i < (int)sizeof k_q; i++) if (q[i] != k_q[i]) ok = 0;
    for (int i = 0; ok && i < (int)sizeof k_d; i++) if (dp[i] != k_d[i]) ok = 0;
    if (!ok) { logf("Textures: upload budget sites mismatch -> budget off"); return; }
    g_oQuery = (Query_t)detour((void*)q, (void*)hk_Query);
    if (!g_oQuery) { logf("Textures: query hook failed -> budget off"); return; }
    g_oDraw = (DrawPrep_t)detour((void*)dp, (void*)hk_Draw);
    if (!g_oDraw) { logf("Textures: draw-prepare hook failed -> budget off (query hook stays: same answers, creation moves to drawing)"); return; }
    g_budgetUs = (DWORD)ms * 1000;
    logf("Textures: upload budget %d ms per frame, a texture waits at most %u frames", ms, g_maxDefer);
}

/* ---------------- interface for diag.c ---------------- */
int texmgr_wanted(void) { return GetPrivateProfileIntA("Textures", "Stats", 1, g_ini) != 0; }

void texmgr_on_device(void* dev)
{
    if (g_oCT || !texmgr_wanted()) return;
    QueryPerformanceFrequency(&g_qpf);
    g_tls = TlsAlloc();
    g_live =(Live*)VirtualAlloc(0, sizeof(Live) * LIVE_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_live) { logf("Textures: no memory for the texture table -> off"); return; }
    void** vt = *(void***)dev;
    g_oCT = (CT_t)hook_fn(vt, 23, (void*)hk_CT, "Device::CreateTexture");
    g_on = g_oCT != 0;
    logf("Textures: statistics %s", g_on ? "on (texture hooks follow with the first texture)" : "OFF");
}

/* called by diag.c once per presented frame (render thread) */
void texmgr_frame(int recording)
{
    if (!g_on) return;
    g_renderTid = GetCurrentThreadId();
    Frame f;
    f.createN = __sync_lock_test_and_set(&g_cur.createN, 0); f.createKB = __sync_lock_test_and_set(&g_cur.createKB, 0);
    f.createUs = __sync_lock_test_and_set(&g_cur.createUs, 0); f.offThreadN = __sync_lock_test_and_set(&g_cur.offThreadN, 0);
    f.lockN = __sync_lock_test_and_set(&g_cur.lockN, 0); f.lockKB = __sync_lock_test_and_set(&g_cur.lockKB, 0);
    f.lockUs = __sync_lock_test_and_set(&g_cur.lockUs, 0); f.copyUs = __sync_lock_test_and_set(&g_cur.copyUs, 0);
    f.unlockUs = __sync_lock_test_and_set(&g_cur.unlockUs, 0);
    f.engUs = __sync_lock_test_and_set(&g_cur.engUs, 0); f.engDraw = __sync_lock_test_and_set(&g_cur.engDraw, 0);
    f.engQuery = __sync_lock_test_and_set(&g_cur.engQuery, 0); f.engOther = __sync_lock_test_and_set(&g_cur.engOther, 0);
    f.deferN = __sync_lock_test_and_set(&g_cur.deferN, 0); f.querySkipN = __sync_lock_test_and_set(&g_cur.querySkipN, 0);
    g_frameNo++;
    cats_frame(recording && g_fr && g_nFr < g_maxFr);
    if (recording && g_fr && g_nFr < g_maxFr) g_fr[g_nFr++] = f;
}

/* texture work (create + lock + copy + unlock, microseconds) of recorded frame i, for the profiler */
DWORD texmgr_frame_work_us(int i)
{
    if (!g_on || !g_fr || i < 0 || i >= g_nFr) return 0;
    Frame* f = &g_fr[i];
    return f->createUs + f->lockUs + f->copyUs + f->unlockUs;
}

void texmgr_rec_start(int maxFrames)
{
    if (!g_on) return;
    if (!g_fr || g_maxFr < maxFrames) {
        if (g_fr) VirtualFree(g_fr, 0, MEM_RELEASE);
        g_fr = (Frame*)VirtualAlloc(0, sizeof(Frame) * (SIZE_T)maxFrames, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_maxFr = g_fr ? maxFrames : 0;
    }
    g_nFr = 0;
    lk(); for (int i = 0; i < 4; i++) { g_liveStart[i] = g_liveBytes[i]; g_livePeak[i] = g_liveBytes[i]; } ulk();
    memset(&g_tc, 0, sizeof g_tc);
    g_deferTotal = g_deferTex = g_forced = g_maxAge = g_querySkipTotal = 0;
}

/* ---------------- report ---------------- */
typedef struct { HANDLE h; char buf[8192]; int n; } W;
static int wo(W* w, const char* p) { w->n = 0; w->h = CreateFileA(p, GENERIC_WRITE, 1, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0); return w->h != INVALID_HANDLE_VALUE; }
static void wf(W* w) { DWORD x; if (w->n) WriteFile(w->h, w->buf, (DWORD)w->n, &x, 0); w->n = 0; }
static void wp(W* w, const char* s) { while (*s) { if (w->n == (int)sizeof w->buf) wf(w); w->buf[w->n++] = *s++; } }
static void wc(W* w) { wf(w); CloseHandle(w->h); }
/* "123.456" from microseconds / kilobytes without floating-point printf */
static char* ms3(char* b, DWORD usv) { wsprintfA(b, "%u.%03u", usv / 1000, usv % 1000); return b; }
static char* mb1(char* b, DWORD kb) { wsprintfA(b, "%u.%u", kb / 1024, (kb % 1024) * 10 / 1024); return b; }

/* dt: frame times (ms) recorded by diag.c for the same frames; n: frame count */
void texmgr_report(const char* txt, const char* csv, const float* dt, int n)
{
    if (!g_on || !g_fr) return;
    if (n > g_nFr) n = g_nFr;
    W w; char b[512], x1[24], x2[24], x3[24], x4[24], x5[24];
    if (!wo(&w, txt)) { logf("Textures: cannot write %s", txt); return; }
    wp(&w, "Div2Fix texture statistics (texmgr)\r\n====================================\r\n");
    wsprintfA(b, "Frames: %d. Times are wall-clock microseconds measured around the D3D9 calls on the calling thread.\r\n", n); wp(&w, b);

    /* totals over the recording */
    DWORD cN = 0, cKB = 0, cUs = 0, off = 0, lN = 0, lKB = 0, lUs = 0, cpUs = 0, ulUs = 0;
    for (int i = 0; i < n; i++) {
        Frame* f = &g_fr[i];
        cN += f->createN; cKB += f->createKB; cUs += f->createUs; off += f->offThreadN;
        lN += f->lockN; lKB += f->lockKB; lUs += f->lockUs; cpUs += f->copyUs; ulUs += f->unlockUs;
    }
    wp(&w, "\r\n[Totals]\r\n");
    wsprintfA(b, "  CreateTexture: %u textures, %s MB, %s ms in the call; created off the render thread: %u\r\n", cN, mb1(x1, cKB), ms3(x2, cUs), off); wp(&w, b);
    wsprintfA(b, "  LockRect: %u locks, %s MB; runtime LockRect %s ms, game copy (lock..unlock) %s ms, runtime UnlockRect %s ms\r\n",
              lN, mb1(x1, lKB), ms3(x2, lUs), ms3(x3, cpUs), ms3(x4, ulUs)); wp(&w, b);

    { DWORD pc, sc, pmb; parcopy_stats(&pc, &sc, &pmb);
      wsprintfA(b, "  ParallelCopy (since game start): %u level copies split between threads (%u MB), %u copies by the game's memcpy_s\r\n", pc, pmb, sc);
      wp(&w, b); }
    static const char* pools[] = { "DEFAULT", "MANAGED", "SYSTEMMEM", "SCRATCH/other" };
    wp(&w, "\r\n[Created textures by pool]\r\n");
    for (int i = 0; i < 4; i++) { wsprintfA(b, "  %-14s %6u  %10s MB\r\n", pools[i], g_byPool[i].n, mb1(x1, g_byPool[i].kb)); wp(&w, b); }
    wp(&w, "\r\n[Created textures by format]\r\n");
    for (int i = 0; i < 5; i++) { wsprintfA(b, "  %-18s %6u  %10s MB\r\n", k_fmt[i], g_byFmt[i].n, mb1(x1, g_byFmt[i].kb)); wp(&w, b); }
    wp(&w, "\r\n[Created textures by largest side]\r\n");
    for (int i = 0; i < 6; i++) { wsprintfA(b, "  %-6s %6u  %10s MB\r\n", k_side[i], g_bySide[i].n, mb1(x1, g_bySide[i].kb)); wp(&w, b); }

    wp(&w, "\r\n[Lock/copy/unlock cost by size of the locked level]\r\n  size          locks        MB   lock ms   copy ms  unlock ms   copy MB/s  unlock MB/s\r\n");
    for (int i = 0; i < 5; i++) {
        LockCls* c = &g_lockCls[i];
        DWORD cps = c->copyUs ? (DWORD)((double)c->kb * 1000000.0 / 1024.0 / c->copyUs) : 0;
        DWORD ups = c->unlockUs ? (DWORD)((double)c->kb * 1000000.0 / 1024.0 / c->unlockUs) : 0;
        wsprintfA(b, "  %-11s %7u %9s %9s %9s %10s %11u %12u\r\n", k_lcls[i], c->n, mb1(x1, c->kb), ms3(x2, c->lockUs), ms3(x3, c->copyUs), ms3(x4, c->unlockUs), cps, ups);
        wp(&w, b);
    }

    if (g_oEng) {
        wp(&w, "\r\n[Engine: NiDX9SourceTextureData::Create by caller (inclusive time: load/convert + D3D create + fill)]\r\n"
               "  path                         calls    total ms     avg ms     max ms\r\n");
        for (int i = 0; i < 4; i++) {
            DWORD n = g_engCalls[i];
            wsprintfA(b, "  %-26s %7u %11s %10s %10s\r\n", k_path[i], n, ms3(x1, g_engUs[i]), ms3(x2, n ? g_engUs[i] / n : 0), ms3(x3, g_engMaxUs[i]));
            wp(&w, b);
        }
    } else wp(&w, "\r\n[Engine: NiDX9SourceTextureData::Create] not hooked (see Div2Fix.log)\r\n");
    if (g_budgetUs) {
        DWORD fr = 0; for (int i = 0; i < n; i++) if (g_fr[i].deferN) fr++;
        wsprintfA(b, "\r\n[Upload budget %u ms/frame, max %u frames]\r\n  queries answered without creating: %u\r\n"
                     "  deferred draw calls: %u in %u frames; textures deferred: %u; forced after the limit: %u; longest wait: %u frames\r\n",
                  g_budgetUs / 1000, g_maxDefer, g_querySkipTotal, g_deferTotal, fr, g_deferTex, g_forced, g_maxAge);
        wp(&w, b);
    } else wp(&w, "\r\n[Upload budget] off\r\n");

    lk(); DWORD lb[4], lp[4]; for (int i = 0; i < 4; i++) { lb[i] = g_liveBytes[i]; lp[i] = g_livePeak[i]; } DWORD lc = g_liveCount, lf = g_liveFull; ulk();
    wp(&w, "\r\n[Live 2D textures (created and not yet released by the game)]\r\n  pool           start MB      end MB     peak MB\r\n");
    for (int i = 0; i < 4; i++) { wsprintfA(b, "  %-14s %9s %11s %11s\r\n", pools[i], mb1(x1, g_liveStart[i] >> 10), mb1(x2, lb[i] >> 10), mb1(x3, lp[i] >> 10)); wp(&w, b); }
    wsprintfA(b, "  live textures now: %u%s\r\n", lc, lf ? " (table overflowed - live numbers are low)" : ""); wp(&w, b);

    /* upload frames vs other frames */
    if (dt) {
        DWORD upN = 0, othN = 0; double upMs = 0, othMs = 0, upTex = 0;
        for (int i = 0; i < n; i++) {
            Frame* f = &g_fr[i]; DWORD work = f->createUs + f->lockUs + f->copyUs + f->unlockUs;
            if (work >= 2000) { upN++; upMs += dt[i]; upTex += work / 1000.0; } else { othN++; othMs += dt[i]; }
        }
        wp(&w, "\r\n[Frames with >= 2 ms of texture work (create+lock+copy+unlock)]\r\n");
        wsprintfA(b, "  frames: %u of %d; their average frame time %s ms, of it texture work %s ms; other frames %s ms\r\n",
                  upN, n, ms3(x1, upN ? (DWORD)(upMs * 1000 / upN) : 0), ms3(x2, upN ? (DWORD)(upTex * 1000 / upN) : 0),
                  ms3(x3, othN ? (DWORD)(othMs * 1000 / othN) : 0)); wp(&w, b);

        wp(&w, "\r\n[Worst 25 frames]  frame | frame ms | created | created MB | create ms | locks | locked MB | lock ms | copy ms | unlock ms | engine ms | draw/query/other\r\n");
        static int used[25]; int nu = 0;
        for (int k = 0; k < 25 && k < n; k++) {
            int best = -1;
            for (int i = 0; i < n; i++) {
                int skip = 0; for (int j = 0; j < nu; j++) if (used[j] == i) skip = 1;
                if (!skip && (best < 0 || dt[i] > dt[best])) best = i;
            }
            if (best < 0) break;
            used[nu++] = best; Frame* f = &g_fr[best];
            char x6[24], x7[24], x8[24];
            wsprintfA(b, "  %6d | %8s | %7u | %10s | %9s | %5u | %9s | %7s | %7s | %9s | %9s | %u/%u/%u\r\n", best, ms3(x1, (DWORD)(dt[best] * 1000)),
                      f->createN, mb1(x2, f->createKB), ms3(x3, f->createUs), f->lockN, mb1(x4, f->lockKB), ms3(x5, f->lockUs),
                      ms3(x6, f->copyUs), ms3(x7, f->unlockUs), ms3(x8, f->engUs), f->engDraw, f->engQuery, f->engOther);
            wp(&w, b);
        }
    }
    wp(&w, "\r\nNotes: 'game copy' is the time between the runtime returning from LockRect and the game calling UnlockRect,\r\n"
           "i.e. the game writing the pixels (memcpy from the NIF data). 'unlock' is the runtime (DXVK) uploading the level.\r\n");
    wc(&w);

    if (csv && wo(&w, csv)) {
        wp(&w, "frame,frametime_ms,created,created_kb,create_us,offthread,locks,locked_kb,lock_us,copy_us,unlock_us,eng_us,eng_draw,eng_query,eng_other,deferred,query_skipped\r\n");
        for (int i = 0; i < n; i++) {
            Frame* f = &g_fr[i];
            wsprintfA(b, "%d,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\r\n", i, ms3(x1, dt ? (DWORD)(dt[i] * 1000) : 0), f->createN, f->createKB, f->createUs,
                      f->offThreadN, f->lockN, f->lockKB, f->lockUs, f->copyUs, f->unlockUs, f->engUs, f->engDraw, f->engQuery, f->engOther,
                      f->deferN, f->querySkipN);
            wp(&w, b);
        }
        wc(&w);
    }
    logf("Textures: statistics written to %s", txt);
}
