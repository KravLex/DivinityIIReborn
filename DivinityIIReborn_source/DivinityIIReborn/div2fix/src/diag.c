/*
 * Div2Fix — diagnostics module
 *
 * Measures real frame pacing at IDirect3DDevice9::Present and writes a report:
 * average FPS, 1% / 0.1% lows, frametime percentiles and histogram, spike counts,
 * frame-to-frame jitter, separately for frames with and without camera (mouse) motion,
 * process memory, and D3D9 device creation / Reset results.
 *
 * Hooks (only when [Diagnostics] Enable=1):
 *   - main module IAT: kernel32!GetProcAddress  -> catches the game's dynamic lookup of Direct3DCreate9
 *   - IDirect3D9::CreateDevice (vtbl 16), IDirect3DDevice9::Reset (16) / Present (17)
 *   - IDirectInput8::CreateDevice (vtbl 3), IDirectInputDevice8::GetDeviceState (9) / GetDeviceData (10)
 *     (read-only: only used to know whether the mouse/camera moved during a frame)
 * Hooks are vtable/IAT pointer swaps on whatever object the game receives, so they chain
 * correctly behind ReShade or other wrappers. Nothing is changed in game behaviour.
 */
#include "common.h"

/* ---------------- small helpers ---------------- */
static int s_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* append fixed-point number with 'dec' decimals (no CRT printf %f) */
static char* put_num(char* p, double v, int dec)
{
    if (v < 0) { *p++ = '-'; v = -v; }
    double scale = 1; for (int i = 0; i < dec; i++) scale *= 10;
    double xv = v * scale + 0.5; if (xv > 4.0e9) xv = 4.0e9;     /* 32-bit math only (no CRT 64-bit division helpers) */
    unsigned int x = (unsigned int)xv, sc = (unsigned int)scale;
    unsigned int ip = x / sc, fp = x % sc;
    char tmp[24]; int n = 0;
    do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    while (n) *p++ = tmp[--n];
    if (dec) {
        *p++ = '.';
        for (int i = dec - 1; i >= 0; i--) { unsigned int d = fp; for (int k = 0; k < i; k++) d /= 10; *p++ = (char)('0' + d % 10); }
    }
    *p = 0;
    return p;
}
static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char* put_int(char* p, long long v) { char t[24]; wsprintfA(t, "%d", (int)v); return put_str(p, t); }

/* --------------- file writer --------------- */
typedef struct { HANDLE h; char buf[8192]; int n; } Writer;
static int w_open(Writer* w, const char* path) { w->n = 0; w->h = CreateFileA(path, GENERIC_WRITE, 1, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0); return w->h != INVALID_HANDLE_VALUE; }
static void w_flush(Writer* w) { DWORD x; if (w->n) WriteFile(w->h, w->buf, (DWORD)w->n, &x, 0); w->n = 0; }
static void w_puts(Writer* w, const char* s) { while (*s) { if (w->n == (int)sizeof w->buf) w_flush(w); w->buf[w->n++] = *s++; } }
static void w_close(Writer* w) { w_flush(w); CloseHandle(w->h); }

/* --------------- heap sort (floats) --------------- */
static void sift(float* a, int start, int end)
{
    int root = start;
    while (2 * root + 1 <= end) {
        int c = 2 * root + 1, sw = root;
        if (a[sw] < a[c]) sw = c;
        if (c + 1 <= end && a[sw] < a[c + 1]) sw = c + 1;
        if (sw == root) return;
        float t = a[root]; a[root] = a[sw]; a[sw] = t; root = sw;
    }
}
static void heapsort_f(float* a, int n)
{
    if (n < 2) return;
    for (int s = (n - 2) / 2; s >= 0; s--) sift(a, s, n - 1);
    for (int e = n - 1; e > 0; e--) { float t = a[0]; a[0] = a[e]; a[e] = t; sift(a, 0, e - 1); }
}

/* ================= state ================= */
static int   g_on;
static int   g_hotkey = 0x7A;          /* F11 */
static int   g_maxFrames;
static float g_spikeFactor = 2.0f;     /* spike = frametime > factor * median */
static int   g_writeCsv = 1;

static long long g_qpf, g_lastQpc;
static int   g_rec;                    /* recording? */
static int   g_n;                      /* frames recorded */
static float* g_dt;                    /* ms */
static BYTE*  g_cam;                   /* 1 = mouse moved during frame */
static long long g_recStart;
static PROCESS_MEMORY_COUNTERS_EX g_memStart;
static int   g_keyPrev;
static volatile long g_mouseAccum;      /* |dx|+|dy| since last Present (written by input thread) */

static int g_bbw, g_bbh, g_interval, g_refresh, g_windowed;
static void* g_mouseDev[4];
static void** g_devVt;                 /* hooked device vtable (watchdog) */
static void** g_scVt;                  /* hooked swapchain vtable (watchdog) */
static volatile long g_presentCalls, g_scPresentCalls, g_pollCalls;

/* ================= D3D9 hooks ================= */
typedef void* (WINAPI *Direct3DCreate9_t)(UINT);
typedef long (WINAPI *CreateDevice_t)(void*, UINT, DWORD, void*, DWORD, void*, void**);
typedef long (WINAPI *Present_t)(void*, const void*, const void*, void*, const void*);
typedef long (WINAPI *Reset_t)(void*, void*);
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);

static Direct3DCreate9_t g_realCreate9;
static CreateDevice_t g_origCreateDevice;
static Present_t g_origPresent;
static Reset_t g_origReset;
static GetProcAddress_t g_realGPA;

typedef struct {
    UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; void* hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval;
} D3DPP;

static void note_pp(const char* what, const D3DPP* pp, long hr)
{
    if (!pp || !readable(pp, sizeof *pp)) { logf("D3D9 %s: hr=%08X (no params)", what, (DWORD)hr); return; }
    g_bbw = (int)pp->BackBufferWidth; g_bbh = (int)pp->BackBufferHeight; g_interval = (int)pp->PresentationInterval;
    g_refresh = (int)pp->FullScreen_RefreshRateInHz; g_windowed = pp->Windowed;
    logf("D3D9 %s: hr=%08X %s, %dx%d fmt=%d buffers=%d MSAA=%d swap=%d windowed=%d refresh=%dHz interval=%08X",
         what, (DWORD)hr, hr < 0 ? "FAILED" : "ok", g_bbw, g_bbh, (int)pp->BackBufferFormat, (int)pp->BackBufferCount,
         (int)pp->MultiSampleType, (int)pp->SwapEffect, g_windowed, g_refresh, (DWORD)g_interval);
}

static void rec_start(void);
static void rec_stop(const char* why);

/* Frames can be presented via IDirect3DDevice9::Present or IDirect3DSwapChain9::Present (Gamebryo
 * NiDX9 can use either). The first path seen becomes the frame clock; the other is ignored so a
 * frame is never counted twice. */
static int g_clock;                    /* 0 = undecided, 1 = device Present, 2 = swapchain Present */
static void frame_tick(int src)
{
    if (g_clock == 0) { g_clock = src; logf("Diagnostics: frame clock = %s", src == 1 ? "IDirect3DDevice9::Present" : "IDirect3DSwapChain9::Present"); }
    if (g_clock != src) return;
    long long now; QueryPerformanceCounter(&now);
    if (g_lastQpc && g_rec) {
        if (g_n < g_maxFrames) {
            g_dt[g_n] = (float)((double)(now - g_lastQpc) * 1000.0 / (double)g_qpf);
            g_cam[g_n] = (BYTE)(__sync_lock_test_and_set(&g_mouseAccum, 0) > 0);
            g_n++;
        } else rec_stop("buffer full");
    } else {
        __sync_lock_test_and_set(&g_mouseAccum, 0);
    }
    g_lastQpc = now;

    int k = (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
    if (k && !g_keyPrev) { if (g_rec) rec_stop("hotkey"); else rec_start(); }
    g_keyPrev = k;
}

static long WINAPI hk_Present(void* dev, const void* a, const void* b, void* c, const void* d)
{
    g_presentCalls++;
    frame_tick(1);
    return g_origPresent(dev, a, b, c, d);
}

typedef long (WINAPI *SCPresent_t)(void*, const void*, const void*, void*, const void*, DWORD);
typedef long (WINAPI *GetSwapChain_t)(void*, UINT, void**);
typedef long (WINAPI *CreateAddSC_t)(void*, void*, void**);
static SCPresent_t g_origSCPresent;
static GetSwapChain_t g_origGetSwapChain;
static CreateAddSC_t g_origCreateAddSC;
static void hook_slot(void** slot, void* hook, void** orig);

static long WINAPI hk_SCPresent(void* sc, const void* a, const void* b, void* c, const void* d, DWORD flags)
{
    g_scPresentCalls++;
    frame_tick(2);
    return g_origSCPresent(sc, a, b, c, d, flags);
}
static void hook_swapchain(void* sc)
{
    if (!sc) return;
    void** vt = *(void***)sc;
    g_scVt = vt;
    hook_slot(&vt[3], (void*)hk_SCPresent, (void**)&g_origSCPresent);   /* IDirect3DSwapChain9::Present */
}
static long WINAPI hk_GetSwapChain(void* dev, UINT i, void** out)
{
    long hr = g_origGetSwapChain(dev, i, out);
    if (hr >= 0 && out) hook_swapchain(*out);
    return hr;
}
static long WINAPI hk_CreateAddSC(void* dev, void* pp, void** out)
{
    long hr = g_origCreateAddSC(dev, pp, out);
    if (hr >= 0 && out) hook_swapchain(*out);
    logf("D3D9 CreateAdditionalSwapChain: hr=%08X", (DWORD)hr);
    return hr;
}

static long WINAPI hk_Reset(void* dev, void* pp)
{
    long hr = g_origReset(dev, pp);
    note_pp("Reset (Alt+Tab / mode change)", (const D3DPP*)pp, hr);
    g_lastQpc = 0;                     /* don't count the reset gap as a frame */
    return hr;
}

static void hook_slot(void** slot, void* hook, void** orig)
{
    if (*slot == hook) return;          /* already hooked (shared vtable) */
    void* prev = *slot;
    *orig = prev;
    int ok = write_mem(slot, &hook, 4);
    logf("  hook slot %08X: %08X -> %08X %s", (DWORD)slot, (DWORD)prev, (DWORD)hook, ok && *slot == hook ? "ok" : "FAILED");
}

static long WINAPI hk_CreateDevice(void* d3d, UINT adapter, DWORD type, void* hwnd, DWORD flags, void* pp, void** out)
{
    long hr = g_origCreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    if (hr >= 0 && out && *out) shadow_on_device(*out);
    if (!g_on) return hr;
    note_pp("CreateDevice", (const D3DPP*)pp, hr);
    if (hr >= 0 && out && *out) {
        void** vt = *(void***)*out;
        g_devVt = vt;
        logf("  device %08X vtable %08X", (DWORD)*out, (DWORD)vt);
        /* Some overlays keep a private, periodically refreshed copy of the device vtable, so a vtable
           slot hook gets silently undone. Detour the implementation functions themselves instead;
           fall back to the slot hook only if the detour refuses. Never both (would double count). */
        if (!g_origPresent) {
            void* tp = detour(vt[17], (void*)hk_Present);
            if (tp) g_origPresent = (Present_t)tp; else hook_slot(&vt[17], (void*)hk_Present, (void**)&g_origPresent);
        }
        if (!g_origReset) {
            void* tr = detour(vt[16], (void*)hk_Reset);
            if (tr) g_origReset = (Reset_t)tr; else hook_slot(&vt[16], (void*)hk_Reset, (void**)&g_origReset);
        }
        hook_slot(&vt[13], (void*)hk_CreateAddSC, (void**)&g_origCreateAddSC);
        hook_slot(&vt[14], (void*)hk_GetSwapChain, (void**)&g_origGetSwapChain);
        void* sc = 0;
        if (g_origGetSwapChain(*out, 0, &sc) >= 0 && sc) {           /* implicit swap chain */
            hook_swapchain(sc);
            typedef unsigned long (WINAPI *Release_t)(void*);
            ((Release_t)(*(void***)sc)[2])(sc);
        }
        logf("Diagnostics: Present/Reset hooked (device %08X). Press the hotkey (VK %02X) in game to start/stop a recording.", (DWORD)*out, (DWORD)g_hotkey);
    }
    return hr;
}

static void* WINAPI hk_Direct3DCreate9(UINT sdk)
{
    void* d3d = g_realCreate9 ? g_realCreate9(sdk) : 0;
    if (d3d) {
        void** vt = *(void***)d3d;
        hook_slot(&vt[16], (void*)hk_CreateDevice, (void**)&g_origCreateDevice);
    } else logf("D3D9: Direct3DCreate9(%d) FAILED", (int)sdk);
    return d3d;
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE h, LPCSTR name)
{
    FARPROC f = g_realGPA(h, name);
    if (f && ((DWORD)name >> 16) && s_eq(name, "Direct3DCreate9") && (void*)f != (void*)hk_Direct3DCreate9) {
        g_realCreate9 = (Direct3DCreate9_t)f;
        return (FARPROC)hk_Direct3DCreate9;
    }
    return f;
}

/* patch one import of the main module by name */
static int hook_import(Module m, const char* dll, const char* func, void* hook, void** orig)
{
    DWORD pe = *(DWORD*)(m.base + 0x3c);
    DWORD rva = *(DWORD*)(m.base + pe + 24 + 96 + 8);      /* DataDirectory[1] = imports */
    if (!rva) return 0;
    for (DWORD* d = (DWORD*)(m.base + rva); d[3]; d += 5) {   /* IMAGE_IMPORT_DESCRIPTOR: OFT, ts, fwd, Name, FT */
        const char* n = (const char*)(m.base + d[3]);
        int eq = 1; for (int i = 0; dll[i] || n[i]; i++) if (lc(dll[i]) != lc(n[i])) { eq = 0; break; }
        if (!eq || !d[0]) continue;
        DWORD* oft = (DWORD*)(m.base + d[0]); void** ft = (void**)(m.base + d[4]);
        for (int i = 0; oft[i]; i++) {
            if (oft[i] & 0x80000000) continue;
            if (s_eq((const char*)(m.base + oft[i] + 2), func)) {
                *orig = ft[i];
                return write_mem(&ft[i], &hook, 4);
            }
        }
    }
    return 0;
}

/* ================= archive open log (which .dv2 files the game actually opens) ================= */
typedef void* (__cdecl *fopen_t)(const char*, const char*);
typedef int   (__cdecl *fopen_s_t)(void**, const char*, const char*);
static fopen_t   g_origFopen;
static fopen_s_t g_origFopenS;
static char g_seen[64][96]; static volatile long g_nseen, g_seenLock;
static void note_archive(const char* path, int ok)
{
    if (!path) return;
    int n = s_len(path); if (n < 4 || lc(path[n-4]) != '.' || lc(path[n-3]) != 'd' || lc(path[n-2]) != 'v' || path[n-1] != '2') return;
    int b = n; while (b > 0 && path[b-1] != '\\' && path[b-1] != '/') b--;
    const char* base = path + b;
    while (__sync_lock_test_and_set(&g_seenLock, 1)) Sleep(0);
    int dup = 0;
    for (long i = 0; i < g_nseen && !dup; i++) if (s_eq(g_seen[i], base)) dup = 1;
    if (!dup && g_nseen < 64) { int k = 0; while (base[k] && k < 95) { g_seen[g_nseen][k] = base[k]; k++; } g_seen[g_nseen][k] = 0; g_nseen++; }
    __sync_lock_release(&g_seenLock);
    if (!dup) logf("Archive: %s %s", base, ok ? "opened" : "NOT opened");
}
static void* __cdecl hk_fopen(const char* p, const char* m) { void* f = g_origFopen(p, m); note_archive(p, f != 0); return f; }
static int __cdecl hk_fopen_s(void** f, const char* p, const char* m) { int r = g_origFopenS(f, p, m); note_archive(p, r == 0 && f && *f); return r; }

/* ================= DirectInput (camera motion detection, read-only) ================= */
typedef long (WINAPI *DICreateDevice_t)(void*, const GUID*, void**, void*);
typedef long (WINAPI *GetDeviceState_t)(void*, DWORD, void*);
typedef long (WINAPI *GetDeviceData_t)(void*, DWORD, void*, DWORD*, DWORD);
static DICreateDevice_t g_origDICreate;
static GetDeviceState_t g_origGDS;
static GetDeviceData_t g_origGDD;

static int is_mouse(void* dev) { for (int i = 0; i < 4; i++) if (g_mouseDev[i] == dev) return 1; return 0; }
static void add_motion(long v) { if (v < 0) v = -v; if (v) __sync_fetch_and_add(&g_mouseAccum, v); }

static void watchdog(void)
{
    long n = ++g_pollCalls;
    if (n == 600 || n == 6000 || n == 60000)
        logf("Diagnostics watchdog: mouse polls=%d device Present calls=%d swapchain Present calls=%d dev slot17=%08X (hook %08X) sc slot3=%08X (hook %08X)",
             (int)n, (int)g_presentCalls, (int)g_scPresentCalls,
             g_devVt ? (DWORD)g_devVt[17] : 0, (DWORD)hk_Present, g_scVt ? (DWORD)g_scVt[3] : 0, (DWORD)hk_SCPresent);
    /* NOTE: deliberately no automatic re-hooking: if another tool chained its own hook in front of ours,
       re-hooking would create a call loop. We only report. */
}

static long WINAPI hk_GDS(void* dev, DWORD cb, void* data)
{
    long hr = g_origGDS(dev, cb, data);
    if (is_mouse(dev)) watchdog();
    if (hr >= 0 && data && cb >= 8 && is_mouse(dev)) { long* s = (long*)data; add_motion(s[0]); add_motion(s[1]); }
    return hr;
}

static long WINAPI hk_GDD(void* dev, DWORD cbObj, void* rg, DWORD* inout, DWORD flags)
{
    long hr = g_origGDD(dev, cbObj, rg, inout, flags);
    if (is_mouse(dev)) watchdog();
    if (hr >= 0 && rg && inout && cbObj >= 8 && is_mouse(dev)) {
        BYTE* p = (BYTE*)rg;
        for (DWORD i = 0; i < *inout; i++, p += cbObj) {
            DWORD ofs = ((DWORD*)p)[0];
            if (ofs == 0 || ofs == 4) add_motion((long)((DWORD*)p)[1]);   /* DIMOFS_X / DIMOFS_Y */
        }
    }
    return hr;
}

static long WINAPI hk_DICreateDevice(void* di, const GUID* g, void** out, void* outer)
{
    long hr = g_origDICreate(di, g, out, outer);
    static const GUID SysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
    if (hr >= 0 && out && *out && g) {
        const BYTE* a = (const BYTE*)g; const BYTE* b = (const BYTE*)&SysMouse; int eq = 1;
        for (int i = 0; i < 16; i++) if (a[i] != b[i]) eq = 0;
        if (eq) {
            for (int i = 0; i < 4; i++) if (!g_mouseDev[i]) { g_mouseDev[i] = *out; break; }
            void** vt = *(void***)*out;
            hook_slot(&vt[9], (void*)hk_GDS, (void**)&g_origGDS);
            hook_slot(&vt[10], (void*)hk_GDD, (void**)&g_origGDD);
            logf("Diagnostics: mouse device %08X tracked for camera-motion statistics", (DWORD)*out);
        }
    }
    return hr;
}

void diag_on_dinput(void* idi8)
{
    if (!g_on || !idi8) return;
    void** vt = *(void***)idi8;
    hook_slot(&vt[3], (void*)hk_DICreateDevice, (void**)&g_origDICreate);
}

/* ================= recording & report ================= */
static void rec_start(void)
{
    if (!g_dt) {
        g_dt = (float*)VirtualAlloc(0, (SIZE_T)g_maxFrames * 4 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_cam = (BYTE*)VirtualAlloc(0, (SIZE_T)g_maxFrames, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_dt || !g_cam) { logf("Diagnostics: cannot allocate buffers"); return; }
    }
    g_n = 0; g_rec = 1; g_lastQpc = 0;
    QueryPerformanceCounter(&g_recStart);
    g_memStart.cb = sizeof g_memStart; K32GetProcessMemoryInfo(GetCurrentProcess(), &g_memStart, sizeof g_memStart);
    logf("Diagnostics: recording started");
}

typedef struct { int n; double avg, med, p1, p01, low1avg, low01avg, maxv, sd, jitter; int sp, s33, s50, s100; } Stats;

static void compute(const float* src, const BYTE* cam, int want, Stats* st, float* tmp)
{
    int n = 0; double sum = 0, sq = 0, jit = 0; float prev = -1; double mx = 0;
    for (int i = 0; i < g_n; i++) {
        if (want >= 0 && cam[i] != want) { prev = -1; continue; }
        float v = src[i]; tmp[n++] = v; sum += v; sq += (double)v * v; if (v > mx) mx = v;
        if (prev >= 0) jit += (v > prev ? v - prev : prev - v);
        prev = v;
    }
    memset(st, 0, sizeof *st); st->n = n;
    if (!n) return;
    st->avg = sum / n; st->maxv = mx;
    double var = sq / n - st->avg * st->avg; st->sd = 0;
    if (var > 0) { double x = var; for (int i = 0; i < 30; i++) x = 0.5 * (x + var / x); st->sd = x; }
    st->jitter = n > 1 ? jit / (n - 1) : 0;
    heapsort_f(tmp, n);
    st->med = tmp[n / 2];
    st->p1 = tmp[(int)((n - 1) * 0.99)];
    st->p01 = tmp[(int)((n - 1) * 0.999)];
    int k1 = n / 100; if (k1 < 1) k1 = 1; int k01 = n / 1000; if (k01 < 1) k01 = 1;
    double s1 = 0, s01 = 0;
    for (int i = 0; i < k1; i++) s1 += tmp[n - 1 - i];
    for (int i = 0; i < k01; i++) s01 += tmp[n - 1 - i];
    st->low1avg = s1 / k1; st->low01avg = s01 / k01;
    double spikeAt = st->med * g_spikeFactor;
    for (int i = 0; i < n; i++) {
        float v = tmp[i];
        if (v > spikeAt) st->sp++;
        if (v > 33.4f) st->s33++;
        if (v > 50.0f) st->s50++;
        if (v > 100.0f) st->s100++;
    }
}

static void write_stats(Writer* w, const char* title, const Stats* s)
{
    char b[512], *p;
    p = put_str(b, "\r\n["); p = put_str(p, title); p = put_str(p, "]  frames: "); p = put_int(p, s->n); p = put_str(p, "\r\n"); w_puts(w, b);
    if (!s->n) return;
    p = put_str(b, "  Avg FPS:              "); p = put_num(p, 1000.0 / s->avg, 1); p = put_str(p, "   (avg frametime "); p = put_num(p, s->avg, 3); p = put_str(p, " ms)\r\n"); w_puts(w, b);
    p = put_str(b, "  Median frametime:     "); p = put_num(p, s->med, 3); p = put_str(p, " ms  ("); p = put_num(p, 1000.0 / s->med, 1); p = put_str(p, " FPS)\r\n"); w_puts(w, b);
    p = put_str(b, "  1% low   (P99):       "); p = put_num(p, 1000.0 / s->p1, 1); p = put_str(p, " FPS  ("); p = put_num(p, s->p1, 2); p = put_str(p, " ms)\r\n"); w_puts(w, b);
    p = put_str(b, "  0.1% low (P99.9):     "); p = put_num(p, 1000.0 / s->p01, 1); p = put_str(p, " FPS  ("); p = put_num(p, s->p01, 2); p = put_str(p, " ms)\r\n"); w_puts(w, b);
    p = put_str(b, "  1% low   (avg worst): "); p = put_num(p, 1000.0 / s->low1avg, 1); p = put_str(p, " FPS\r\n"); w_puts(w, b);
    p = put_str(b, "  0.1% low (avg worst): "); p = put_num(p, 1000.0 / s->low01avg, 1); p = put_str(p, " FPS\r\n"); w_puts(w, b);
    p = put_str(b, "  Max frametime:        "); p = put_num(p, s->maxv, 2); p = put_str(p, " ms\r\n"); w_puts(w, b);
    p = put_str(b, "  Std deviation:        "); p = put_num(p, s->sd, 3); p = put_str(p, " ms\r\n"); w_puts(w, b);
    p = put_str(b, "  Frame-to-frame jitter:"); p = put_num(p, s->jitter, 3); p = put_str(p, " ms (avg |dt[i]-dt[i-1]|)\r\n"); w_puts(w, b);
    p = put_str(b, "  Spikes > "); p = put_num(p, g_spikeFactor, 1); p = put_str(p, "x median: "); p = put_int(p, s->sp);
    p = put_str(p, "   >33ms: "); p = put_int(p, s->s33); p = put_str(p, "   >50ms: "); p = put_int(p, s->s50); p = put_str(p, "   >100ms: "); p = put_int(p, s->s100); p = put_str(p, "\r\n"); w_puts(w, b);
}

static void rec_stop(const char* why)
{
    if (!g_rec) return;
    g_rec = 0;
    long long end; QueryPerformanceCounter(&end);
    PROCESS_MEMORY_COUNTERS_EX me; me.cb = sizeof me; K32GetProcessMemoryInfo(GetCurrentProcess(), &me, sizeof me);
    SYSTEMTIME t; GetLocalTime(&t);
    char path[MAX_PATH], csv[MAX_PATH];
    wsprintfA(path, "%sDiv2Fix_perf_%04d%02d%02d_%02d%02d%02d.txt", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wsprintfA(csv, "%sDiv2Fix_perf_%04d%02d%02d_%02d%02d%02d.csv", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    float* tmp = g_dt + g_maxFrames;
    Stats all, mov, still;
    compute(g_dt, g_cam, -1, &all, tmp);
    compute(g_dt, g_cam, 1, &mov, tmp);
    compute(g_dt, g_cam, 0, &still, tmp);

    Writer w;
    if (w_open(&w, path)) {
        char b[512], *p;
        w_puts(&w, "Div2Fix frame pacing report\r\n===========================\r\n");
        p = put_str(b, "Stopped by: "); p = put_str(p, why); p = put_str(p, "\r\nDuration: ");
        p = put_num(p, (double)(end - g_recStart) / (double)g_qpf, 1); p = put_str(p, " s\r\n"); w_puts(&w, b);
        wsprintfA(b, "Device: %dx%d, windowed=%d, refresh=%d Hz (0 = desktop), PresentationInterval=%08X (1 = V-Sync on, 80000000 = off)\r\n",
                  g_bbw, g_bbh, g_windowed, g_refresh, (DWORD)g_interval); w_puts(&w, b);
        wsprintfA(b, "Memory: working set %d MB -> %d MB (peak %d MB), private %d MB -> %d MB\r\n",
                  (int)(g_memStart.WorkingSetSize >> 20), (int)(me.WorkingSetSize >> 20), (int)(me.PeakWorkingSetSize >> 20),
                  (int)(g_memStart.PrivateUsage >> 20), (int)(me.PrivateUsage >> 20)); w_puts(&w, b);
        write_stats(&w, "All frames", &all);
        write_stats(&w, "Camera moving (mouse input during frame)", &mov);
        write_stats(&w, "Camera still", &still);
        /* histogram */
        static const float edges[] = { 2, 4, 5.26f, 6.94f, 8.33f, 11.1f, 16.7f, 25, 33.4f, 50, 100, 1e9f };
        static const char* labels[] = { "<2ms", "2-4", "4-5.26 (190fps)", "5.26-6.94 (144)", "6.94-8.33 (120)", "8.33-11.1 (90)",
                                        "11.1-16.7 (60)", "16.7-25 (40)", "25-33.4 (30)", "33.4-50", "50-100", ">100ms" };
        int cnt[12] = { 0 };
        for (int i = 0; i < g_n; i++) { int k = 0; while (g_dt[i] >= edges[k]) k++; cnt[k]++; }
        w_puts(&w, "\r\n[Frametime histogram]\r\n");
        for (int k = 0; k < 12; k++) {
            p = put_str(b, "  "); p = put_str(p, labels[k]); p = put_str(p, ": "); p = put_int(p, cnt[k]);
            p = put_str(p, "  ("); p = put_num(p, g_n ? 100.0 * cnt[k] / g_n : 0, 2); p = put_str(p, "%)\r\n"); w_puts(&w, b);
        }
        w_puts(&w, "\r\nNotes: 1% low (P99) = FPS at the 99th-percentile frametime. Spikes and jitter matter more for\r\n"
                   "perceived smoothness than average FPS. Compare recordings made on the same route with the same settings.\r\n");
        w_close(&w);
    }
    if (g_writeCsv && w_open(&w, csv)) {
        char b[96], *p; double tt = 0;
        w_puts(&w, "frame,time_ms,frametime_ms,camera_moving\r\n");
        for (int i = 0; i < g_n; i++) {
            tt += g_dt[i];
            p = put_int(b, i); p = put_str(p, ","); p = put_num(p, tt, 3); p = put_str(p, ","); p = put_num(p, g_dt[i], 3);
            p = put_str(p, g_cam[i] ? ",1\r\n" : ",0\r\n"); w_puts(&w, b);
        }
        w_close(&w);
    }
    logf("Diagnostics: recording stopped (%s), %d frames -> %s", why, g_n, path);
    g_lastQpc = 0;
}

void diag_init(Module m)
{
    if (GetPrivateProfileIntA("General", "LogArchives", 1, g_ini)) {
        int a = hook_import(m, "MSVCR90.dll", "fopen", (void*)hk_fopen, (void**)&g_origFopen);
        int b = hook_import(m, "MSVCR90.dll", "fopen_s", (void*)hk_fopen_s, (void**)&g_origFopenS);
        logf("Archive log: %s", a && b ? "on" : "could not hook fopen");
    }
    g_on = GetPrivateProfileIntA("Diagnostics", "Enable", 0, g_ini);
    if (!g_on) {
        /* the D3D device is still needed for the shadow blur module (shadow.c) */
        logf("Diagnostics: disabled");
        if (shadow_wanted()) {
            if (!hook_import(m, "KERNEL32.dll", "GetProcAddress", (void*)hk_GetProcAddress, (void**)&g_realGPA))
                logf("ShadowSoft: could not hook GetProcAddress import -> off");
        }
        return;
    }
    g_hotkey = GetPrivateProfileIntA("Diagnostics", "HotkeyVK", 0x7A, g_ini);
    if (g_hotkey < 1 || g_hotkey > 254) { logf("Diagnostics: HotkeyVK=%d out of range, using F11", g_hotkey); g_hotkey = 0x7A; }
    int maxSec = GetPrivateProfileIntA("Diagnostics", "MaxMinutes", 30, g_ini);
    if (maxSec < 1) maxSec = 1; if (maxSec > 120) maxSec = 120;
    g_maxFrames = maxSec * 60 * 400;    /* enough for 400 FPS */
    int sf = GetPrivateProfileIntA("Diagnostics", "SpikeFactorPercent", 200, g_ini);
    if (sf < 120) sf = 120; if (sf > 1000) sf = 1000;
    g_spikeFactor = sf / 100.0f;
    g_writeCsv = GetPrivateProfileIntA("Diagnostics", "WriteCsv", 1, g_ini);
    QueryPerformanceFrequency(&g_qpf);
    if (hook_import(m, "KERNEL32.dll", "GetProcAddress", (void*)hk_GetProcAddress, (void**)&g_realGPA))
        logf("Diagnostics: enabled (hotkey VK %02X, max %d min). Waiting for Direct3DCreate9...", (DWORD)g_hotkey, maxSec);
    else
        logf("Diagnostics: could not hook GetProcAddress import -> frametime measurement unavailable");
}
