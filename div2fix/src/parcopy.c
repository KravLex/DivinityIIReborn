/* Div2Fix - parallel copy of texture levels ([Textures] ParallelCopyThreads, default 4; 1 = as the game).
 *
 * NiDX9SourceTextureData::CopyRendererDataToSurfaceLevel copies every mip level of a new texture into the locked
 * surface with MSVCR90!memcpy_s (Divinity2.exe v1.4.700.56, 0x5D962D: push ecx / push esi / push ecx / push edx /
 * call [memcpy_s]). With DXVK the destination is freshly mapped file-backed memory (the 32-bit friendly copy of a
 * managed texture), so the copy is bound by page faults: 0.48 ms/MB measured in game, 0.13 ms/MB into warm memory.
 * Page faults are handled in parallel by the kernel: in tools/benchmark/filemap_bench the same copy takes
 * 0.33 / 0.24 / 0.20 ms/MB with 2 / 4 / 8 threads (i5-12600KF).
 *
 * The call is redirected to parcopy_memcpy_s: large copies are split between the calling thread and a pool of
 * workers, and the function returns only when every part is done - the game sees the same bytes at the same moment.
 * Small copies, invalid arguments, and calls while the pool is busy (another thread) go to the original memcpy_s.
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
#define MAXW 7

typedef int (__cdecl *MemcpyS_t)(void*, SIZE_T, const void*, SIZE_T);
typedef struct { HANDLE go, done; BYTE* dst; const BYTE* src; DWORD n; } Job;

static MemcpyS_t* g_iat;                 /* the game's import slot of MSVCR90!memcpy_s */
static Job g_job[MAXW];
static int g_workers;                    /* threads - 1 */
static DWORD g_minBytes = 1u << 20;
static volatile long g_busy;
static volatile long g_parCalls, g_serCalls;
static long long g_parBytes;

static void copy_bytes(void* d, const void* s, DWORD n)
{
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static DWORD WINAPI worker(void* p)
{
    Job* j = (Job*)p;
    for (;;) {
        WaitForSingleObject(j->go, 0xFFFFFFFF);
        copy_bytes(j->dst, j->src, j->n);
        SetEvent(j->done);
    }
}

/* the copy itself, exposed for tests: splits n bytes into page-aligned parts, the caller copies the last part */
void parcopy_run(void* dst, const void* src, DWORD n, int workers)
{
    int parts = workers + 1;
    DWORD part = (n / (DWORD)parts) & ~4095u;
    if (!part) { copy_bytes(dst, src, n); return; }
    HANDLE done[MAXW]; int nw = 0; DWORD off = 0;
    for (int i = 0; i < workers; i++) {
        Job* j = &g_job[i];
        j->dst = (BYTE*)dst + off; j->src = (const BYTE*)src + off; j->n = part; off += part;
        done[nw++] = j->done;
        SetEvent(j->go);
    }
    copy_bytes((BYTE*)dst + off, (const BYTE*)src + off, n - off);
    WaitForMultipleObjects((DWORD)nw, done, 1, 0xFFFFFFFF);
}

int __cdecl parcopy_memcpy_s(void* dst, SIZE_T dstSize, const void* src, SIZE_T count)
{
    MemcpyS_t real = *g_iat;
    if (count < g_minBytes || !dst || !src || count > dstSize || g_workers < 1 || __sync_lock_test_and_set(&g_busy, 1)) {
        __sync_fetch_and_add(&g_serCalls, 1);
        return real(dst, dstSize, src, count);
    }
    parcopy_run(dst, src, (DWORD)count, g_workers);
    g_parBytes += count;                 /* only the pool owner writes it */
    __sync_fetch_and_add(&g_parCalls, 1);
    __sync_lock_release(&g_busy);
    return 0;
}

/* creates the pool; returns the number of workers started */
int parcopy_pool(int threads)
{
    if (threads > MAXW + 1) threads = MAXW + 1;
    int n = 0;
    for (int i = 0; i < threads - 1; i++) {
        Job* j = &g_job[i];
        j->go = CreateEventA(0, 0, 0, 0); j->done = CreateEventA(0, 0, 0, 0);
        if (!j->go || !j->done) break;
        HANDLE th = CreateThread(0, 0, worker, j, 0, 0);
        if (!th) break;
        SetThreadPriority(th, 1 /* ABOVE_NORMAL: short bursts the render thread waits for */);
        CloseHandle(th);
        n++;
    }
    g_workers = n;
    return n;
}

void parcopy_init(Module m)
{
    int threads = GetPrivateProfileIntA("Textures", "ParallelCopyThreads", 4, g_ini);
    int minKB = GetPrivateProfileIntA("Textures", "ParallelCopyMinKB", 1024, g_ini);
    if (threads <= 1) { logf("ParallelCopy: off ([Textures] ParallelCopyThreads=%d)", threads); return; }
    if (minKB < 64) minKB = 64;
    g_minBytes = (DWORD)minKB << 10;
    /* 0x5D9629: push ecx / push esi / push ecx / push edx / call [memcpy_s] / add esp,10h */
    BYTE* p = VA(m, 0x5D9629);
    if (!readable(p, 16) || p[0] != 0x51 || p[1] != 0x56 || p[2] != 0x51 || p[3] != 0x52 || p[4] != 0xFF || p[5] != 0x15 ||
        *(DWORD*)(p + 6) != (DWORD)VA(m, 0x12B7800) || p[10] != 0x83 || p[11] != 0xC4 || p[12] != 0x10) {
        logf("ParallelCopy: call site mismatch -> off"); return;
    }
    g_iat = (MemcpyS_t*)VA(m, 0x12B7800);
    if (!readable(g_iat, 4) || !*g_iat) { logf("ParallelCopy: memcpy_s import not resolved -> off"); return; }
    int w = parcopy_pool(threads);
    if (w < 1) { logf("ParallelCopy: could not start worker threads -> off"); return; }
    BYTE patch[6]; patch[0] = 0xE8; *(DWORD*)(patch + 1) = (DWORD)parcopy_memcpy_s - (DWORD)(p + 4 + 5); patch[5] = 0x90;
    if (!write_mem(p + 4, patch, 6)) { logf("ParallelCopy: patch failed -> off"); g_workers = 0; return; }
    logf("ParallelCopy: texture levels >= %d KB copied by %d threads (caller + %d workers)", minKB, w + 1, w);
}

/* for the texture report */
void parcopy_stats(DWORD* parCalls, DWORD* serCalls, DWORD* parMB)
{
    *parCalls = (DWORD)g_parCalls; *serCalls = (DWORD)g_serCalls; *parMB = (DWORD)(g_parBytes >> 20);
}
