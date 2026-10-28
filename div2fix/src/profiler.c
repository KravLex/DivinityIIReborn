/*
 * Div2Fix - sampling profiler for frame-time spikes ([Diagnostics] Profiler=1, records together with F11).
 *
 * A time-critical thread wakes about every millisecond, suspends the render thread (the one calling Present),
 * reads EIP/ESP and copies the top of its stack, and resumes it. While the thread is suspended nothing is
 * allocated and no lock is taken (the render thread may hold the heap or loader lock). Afterwards the copied
 * stack is scanned for return addresses (values inside a module's code that follow a CALL instruction).
 *
 * Samples are kept only for frames longer than 16.7 ms ("spikes") and for every 20th frame (baseline). The
 * report splits spikes into frames with >= 2 ms of texture work (texmgr) and frames without, and shows per class:
 * modules, hottest code (64-byte blocks) and the first game (Divinity2.exe) function on the stack.
 * Output next to the frame-time report: Div2Fix_prof_<time>.txt and .csv (raw samples + module table, for
 * tools\benchmark\prof_symbolize.py).
 */
#include "common.h"

#define SPIKE_MS     16.7f
#define BASE_EVERY   20
#define CUR_MAX      512                 /* samples of one frame (512 ms at 1 kHz) */
#define STORE_MAX    300000              /* ~9.6 MB */
#define NRET         6
#define STACK_DW     768                 /* 3 KB of stack copied per sample */

typedef struct { DWORD frame, eip, ret[NRET]; } Sample;
typedef struct { DWORD eip, esp; DWORD n; DWORD stack[STACK_DW]; } Raw;

static int g_enabled;
static volatile long g_active;           /* recording in progress */
static volatile DWORD g_tid;             /* render thread */
static HANDLE g_thr;
static volatile long g_lock;
static Sample g_cur[CUR_MAX]; static int g_ncur;
static Sample* g_store; static int g_nstore; static DWORD g_dropped, g_taken, g_failed;
static Raw g_raw;                         /* sampler thread only */

static void lk(void) { while (__sync_lock_test_and_set(&g_lock, 1)) Sleep(0); }
static void ulk(void) { __sync_lock_release(&g_lock); }

/* ---------------- modules (code ranges) ---------------- */
typedef struct { BYTE* base; DWORD size; BYTE* code; DWORD codeSize; char name[40]; int sys; } Mod;
static Mod g_mods[160]; static int g_nmods;
static DWORD g_codeLo = 0xFFFFFFFF, g_codeHi;   /* quick reject for stack values (most are data) */

static void load_modules(void)
{
    HMODULE hs[160]; DWORD need = 0; char winDir[MAX_PATH];
    UINT wl = GetSystemDirectoryA(winDir, MAX_PATH);           /* C:\Windows\system32 -> C:\Windows\ */
    while (wl > 0 && winDir[wl - 1] != '\\') wl--; winDir[wl] = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), hs, sizeof hs, &need)) return;
    int n = (int)(need / sizeof(HMODULE)); if (n > 160) n = 160;
    g_nmods = 0;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi; if (!K32GetModuleInformation(GetCurrentProcess(), hs[i], &mi, sizeof mi)) continue;
        Mod* m = &g_mods[g_nmods];
        m->base = (BYTE*)mi.lpBaseOfDll; m->size = mi.SizeOfImage;
        char path[MAX_PATH]; GetModuleFileNameA(hs[i], path, MAX_PATH);
        int k = 0, sys = 1; while (winDir[k]) { if (lc(path[k]) != lc(winDir[k])) { sys = 0; break; } k++; }
        m->sys = sys;
        int b = s_len(path); while (b > 0 && path[b - 1] != '\\') b--;
        int j = 0; while (path[b + j] && j < 39) { m->name[j] = path[b + j]; j++; } m->name[j] = 0;
        /* first executable section = code range */
        m->code = m->base; m->codeSize = m->size;
        DWORD pe = *(DWORD*)(m->base + 0x3c); WORD nsec = *(WORD*)(m->base + pe + 6), opt = *(WORD*)(m->base + pe + 20);
        BYTE* sh = m->base + pe + 24 + opt;
        for (int s = 0; s < nsec; s++, sh += 40)
            if (*(DWORD*)(sh + 36) & 0x20000000) { m->code = m->base + *(DWORD*)(sh + 12); m->codeSize = *(DWORD*)(sh + 8); break; }
        if ((DWORD)m->code < g_codeLo) g_codeLo = (DWORD)m->code;
        if ((DWORD)m->code + m->codeSize > g_codeHi) g_codeHi = (DWORD)m->code + m->codeSize;
        g_nmods++;
    }
}
static int mod_of(DWORD a)
{
    for (int i = 0; i < g_nmods; i++) if (a - (DWORD)g_mods[i].base < g_mods[i].size) return i;
    return -1;
}
static int in_code(DWORD a)
{
    if (a < g_codeLo || a >= g_codeHi) return 0;
    for (int i = 0; i < g_nmods; i++) if (a - (DWORD)g_mods[i].code < g_mods[i].codeSize) return 1;
    return 0;
}
/* a value on the stack is a return address if the bytes before it form a CALL */
static DWORD g_okPage, g_badPage;                                 /* one-entry caches for code_readable */
static int page_readable(DWORD pg)
{
    if (pg == g_okPage) return 1;
    if (pg == g_badPage) return 0;
    MEMORY_BASIC_INFORMATION mi;
    int ok = VirtualQuery((void*)pg, &mi, sizeof mi) && mi.State == MEM_COMMIT
          && (mi.Protect & 0xEE) && !(mi.Protect & 0x101);           /* any readable protection; not GUARD / NOACCESS */
    if (ok) g_okPage = pg; else g_badPage = pg;
    return ok;
}
/* The module list is a snapshot: a DLL unloaded later leaves a stale code range, and reading it crashed
   the game (28.10, access violation in sampler). Check that the 7 bytes before 'a' are mapped first. */
static int code_readable(DWORD a)
{
    return page_readable((a - 7) & ~0xFFFu) && page_readable((a - 1) & ~0xFFFu);
}
static int after_call(DWORD a)
{
    const BYTE* p = (const BYTE*)a;
    if (!code_readable(a)) return 0;
    if (p[-5] == 0xE8) return 1;                                   /* call rel32 */
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return 1;   /* call [abs32] / [reg+d32] */
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return 1;         /* call [reg+d8] */
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return 1;   /* call reg / [reg] */
    if (p[-7] == 0xFF && p[-6] == 0x94) return 1;                  /* call [reg+reg*s+d32] */
    if (p[-4] == 0xFF && p[-3] == 0x54) return 1;                  /* call [reg+reg*s+d8] */
    return 0;
}

/* ---------------- sampler thread ---------------- */
static void take_sample(HANDLE th)
{
    static __attribute__((aligned(16))) BYTE ctx[0x2CC];
    *(DWORD*)ctx = 0x10001;                                        /* CONTEXT_CONTROL (i386) */
    if (SuspendThread(th) == (DWORD)-1) { g_failed++; return; }
    if (!GetThreadContext(th, ctx)) { ResumeThread(th); g_failed++; return; }
    g_raw.eip = *(DWORD*)(ctx + 0xB8); g_raw.esp = *(DWORD*)(ctx + 0xC4);
    MEMORY_BASIC_INFORMATION mi; DWORD n = 0;
    if (VirtualQuery((void*)g_raw.esp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
        DWORD top = (DWORD)mi.BaseAddress + mi.RegionSize;
        n = (top - g_raw.esp) / 4; if (n > STACK_DW) n = STACK_DW;
        const DWORD* s = (const DWORD*)g_raw.esp;
        for (DWORD i = 0; i < n; i++) g_raw.stack[i] = s[i];
    }
    g_raw.n = n;
    ResumeThread(th);

    Sample sm; sm.frame = 0; sm.eip = g_raw.eip; int k = 0;
    for (DWORD i = 0; i < g_raw.n && k < NRET; i++) {
        DWORD v = g_raw.stack[i];
        if (v > 0x10000 && in_code(v) && after_call(v)) sm.ret[k++] = v;
    }
    while (k < NRET) sm.ret[k++] = 0;
    lk(); if (g_ncur < CUR_MAX) g_cur[g_ncur++] = sm; ulk();
    g_taken++;
}

static DWORD WINAPI sampler(void* arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    logf("Profiler: sampler thread started (%s timer)", timer ? "high-resolution" : "Sleep(1)");
    HANDLE th = 0; DWORD thId = 0;
    for (;;) {
        if (!g_active) { Sleep(20); continue; }
        if (g_tid != thId) {
            if (th) CloseHandle(th);
            thId = g_tid; th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, 0, thId);
            if (!th) { logf("Profiler: cannot open render thread %u", thId); g_active = 0; continue; }
        }
        if (timer) { long long due = -10000; SetWaitableTimer(timer, &due, 0, 0, 0, 0); WaitForSingleObject(timer, 50); }
        else Sleep(1);
        if (g_active && th) take_sample(th);
    }
}

/* ---------------- interface for diag.c ---------------- */
void prof_init(void)
{
    g_enabled = GetPrivateProfileIntA("Diagnostics", "Profiler", 1, g_ini) != 0;
    logf("Profiler: %s ([Diagnostics] Profiler)", g_enabled ? "on with every recording" : "off");
}

void prof_rec_start(void)
{
    if (!g_enabled) return;
    if (!g_store) g_store = (Sample*)VirtualAlloc(0, sizeof(Sample) * STORE_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_store) { logf("Profiler: no memory -> off"); g_enabled = 0; return; }
    load_modules();
    g_nstore = 0; g_ncur = 0; g_dropped = g_taken = g_failed = 0;
    g_tid = GetCurrentThreadId();        /* rec_start runs on the render thread (Present) */
    if (!g_thr) { g_thr = CreateThread(0, 0, sampler, 0, 0, 0); if (!g_thr) { logf("Profiler: no thread -> off"); g_enabled = 0; return; } }
    g_active = 1;
}

/* end of a presented frame: idx = recorded frame index or -1, ms = its frame time */
void prof_frame(int idx, float ms)
{
    if (!g_active) return;
    lk();
    if (idx >= 0 && (ms > SPIKE_MS || idx % BASE_EVERY == 0)) {
        for (int i = 0; i < g_ncur; i++) {
            if (g_nstore >= STORE_MAX) { g_dropped += (DWORD)(g_ncur - i); break; }
            g_store[g_nstore] = g_cur[i]; g_store[g_nstore].frame = (DWORD)idx; g_nstore++;
        }
    }
    g_ncur = 0;
    ulk();
}

/* ---------------- report ---------------- */
typedef struct { DWORD key, n; } KN;
#define HN 4096
static KN g_h[3][2][HN];                 /* [class][0 = eip block, 1 = first game frame] */
static DWORD g_modN[3][161], g_clsN[3], g_clsFr[3];

static void hadd(KN* t, DWORD key)
{
    DWORD i = (key * 2654435761u) >> 20;
    for (int k = 0; k < HN; k++, i = (i + 1) & (HN - 1)) {
        if (t[i].key == key && t[i].n) { t[i].n++; return; }
        if (!t[i].n) { t[i].key = key; t[i].n = 1; return; }
    }
}
static char* addr_str(char* b, DWORD a)
{
    int m = mod_of(a);
    if (m < 0) wsprintfA(b, "%08X", a); else wsprintfA(b, "%s+%08X", g_mods[m].name, a - (DWORD)g_mods[m].base);
    return b;
}

typedef struct { HANDLE h; char buf[8192]; int n; } PW;
static int po(PW* w, const char* p) { w->n = 0; w->h = CreateFileA(p, GENERIC_WRITE, 1, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0); return w->h != INVALID_HANDLE_VALUE; }
static void pf(PW* w) { DWORD x; if (w->n) WriteFile(w->h, w->buf, (DWORD)w->n, &x, 0); w->n = 0; }
static void pp(PW* w, const char* s) { while (*s) { if (w->n == (int)sizeof w->buf) pf(w); w->buf[w->n++] = *s++; } }
static void pc(PW* w) { pf(w); CloseHandle(w->h); }

static void top(PW* w, KN* t, DWORD total, int count, const char* title)
{
    char b[256], a[64];
    wsprintfA(b, "  %s:\r\n", title); pp(w, b);
    static BYTE used[HN]; memset(used, 0, sizeof used);
    for (int k = 0; k < count; k++) {
        int best = -1;
        for (int i = 0; i < HN; i++) if (t[i].n && !used[i] && (best < 0 || t[i].n > t[best].n)) best = i;
        if (best < 0) break;
        used[best] = 1;
        wsprintfA(b, "    %3u%%  %6u  %s\r\n", total ? t[best].n * 100 / total : 0, t[best].n, addr_str(a, t[best].key)); pp(w, b);
    }
}

void prof_report(const char* txt, const char* csv, const float* dt, int n)
{
    if (!g_enabled || !g_store) return;
    g_active = 0; Sleep(5);              /* sampler idles; module list stays as loaded at start */
    memset(g_h, 0, sizeof g_h); memset(g_modN, 0, sizeof g_modN); memset(g_clsN, 0, sizeof g_clsN); memset(g_clsFr, 0, sizeof g_clsFr);
    int exe = -1; for (int i = 0; i < g_nmods; i++) if (g_mods[i].base == main_module().base) exe = i;
    DWORD lastFr = (DWORD)-1;
    for (int i = 0; i < g_nstore; i++) {
        Sample* s = &g_store[i];
        if ((int)s->frame >= n) continue;
        float ms = dt[s->frame];
        int cls = ms > SPIKE_MS ? (texmgr_frame_work_us((int)s->frame) >= 2000 ? 1 : 0) : 2;
        g_clsN[cls]++; if (s->frame != lastFr) { g_clsFr[cls]++; lastFr = s->frame; }
        int m = mod_of(s->eip); g_modN[cls][m < 0 ? 160 : m]++;
        hadd(g_h[cls][0], s->eip & ~63u);
        DWORD g = 0;
        if (exe >= 0) {
            if (mod_of(s->eip) == exe) g = s->eip;
            else for (int k = 0; k < NRET && !g; k++) if (s->ret[k] && mod_of(s->ret[k]) == exe) g = s->ret[k];
        }
        hadd(g_h[cls][1], g ? g : 1);    /* key 1 = no game frame found on the copied stack */
    }

    PW w; char b[512];
    if (!po(&w, txt)) return;
    pp(&w, "Div2Fix sampling profiler (render thread, ~1 kHz)\r\n================================================\r\n");
    wsprintfA(b, "Samples taken %u, failed %u, kept %d (spike and baseline frames), dropped %u (store full)\r\n"
                 "Spike = frame > 16.7 ms. 'With textures' = >= 2 ms of texture work in that frame (texmgr).\r\n"
                 "Baseline = every %dth frame that was not a spike.\r\n", g_taken, g_failed, g_nstore, g_dropped, BASE_EVERY); pp(&w, b);
    static const char* cls[] = { "Spikes WITHOUT texture work", "Spikes with texture work", "Baseline (normal frames)" };
    for (int c = 0; c < 3; c++) {
        wsprintfA(b, "\r\n[%s]  frames %u, samples %u\r\n", cls[c], g_clsFr[c], g_clsN[c]); pp(&w, b);
        if (!g_clsN[c]) continue;
        pp(&w, "  by module (where the CPU was):\r\n");
        for (int k = 0; k < 12; k++) {
            int best = -1;
            for (int i = 0; i <= 160; i++) if (g_modN[c][i] && (best < 0 || g_modN[c][i] > g_modN[c][best])) best = i;
            if (best < 0) break;
            wsprintfA(b, "    %3u%%  %6u  %s\r\n", g_modN[c][best] * 100 / g_clsN[c], g_modN[c][best], best == 160 ? "?" : g_mods[best].name); pp(&w, b);
            g_modN[c][best] = 0;
        }
        top(&w, g_h[c][0], g_clsN[c], 20, "hottest code (64-byte blocks)");
        top(&w, g_h[c][1], g_clsN[c], 20, "first Divinity2.exe frame on the stack (00000001 = none found)");
    }
    pp(&w, "\r\nSymbols: py -3 -I tools\\benchmark\\prof_symbolize.py <this .csv>\r\n");
    pc(&w);

    if (csv && po(&w, csv)) {
        for (int i = 0; i < g_nmods; i++) { wsprintfA(b, "#mod,%s,%08X,%08X,%d\r\n", g_mods[i].name, (DWORD)g_mods[i].base, g_mods[i].size, g_mods[i].sys); pp(&w, b); }
        pp(&w, "frame,frame_us,tex_us,eip,ret1,ret2,ret3,ret4,ret5,ret6\r\n");
        for (int i = 0; i < g_nstore; i++) {
            Sample* s = &g_store[i]; if ((int)s->frame >= n) continue;
            wsprintfA(b, "%u,%u,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X\r\n", s->frame, (DWORD)(dt[s->frame] * 1000), texmgr_frame_work_us((int)s->frame),
                      s->eip, s->ret[0], s->ret[1], s->ret[2], s->ret[3], s->ret[4], s->ret[5]);
            pp(&w, b);
        }
        pc(&w);
    }
    logf("Profiler: %d samples kept (%u taken, %u failed) -> %s", g_nstore, g_taken, g_failed, txt);
}
