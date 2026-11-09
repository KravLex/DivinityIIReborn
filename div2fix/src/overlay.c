/* Div2Fix - archive overlay (Divinity II Reborn NG-2): Data\Win32\Packed\Reborn\*.dv2 on top of the game's archives.
 *
 * The exe has 12 fixed patch slots (DKS_Patch[_2..4], FOV_Patch[_2..4], Patch[_2..4]) and all of them are taken (HD
 * pack, dv2mod). 0xB39B00 registers the archives one by one, the patch slots first (flags 0,0,1), then the main
 * archives (1,0,0):
 *     name = 0x4B34D0(path)                  NiFixedString from char* (cdecl): pointer to the text, ref-counted
 *     [esi+0xD0]->vtbl[0](&name, a, b, c)    = 0x697D30, thiscall, ret 10h: registers one archive
 *     0x401170(&name)                        releases the NiFixedString (thiscall)
 * 0x697D30 is detoured. Before the first patch slot (\DKS_Patch.dv2) every Reborn\*.dv2 (alphabetical) is registered
 * the same way with the patch flags; with [Overlay] Order=last after the last one (\Patch_4.dv2) instead. The menu
 * test (docs/OVERLAY.md) showed that with Order=first the Reborn archives win over the HD pack. Files of the region
 * archives (World\<region>\...) are not overridden.
 * Nothing is written to the archives or the exe file. INI: [Overlay] Enable=1 (default), Order=first|last.
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
#define MAX_OVERLAY 64

typedef struct {
    DWORD dwFileAttributes;
    DWORD ftCreationTime[2], ftLastAccessTime[2], ftLastWriteTime[2];
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
} FindDataA;

__declspec(dllimport) HANDLE WINAPI FindFirstFileA(const char*, FindDataA*);
__declspec(dllimport) BOOL WINAPI FindNextFileA(HANDLE, FindDataA*);
__declspec(dllimport) BOOL WINAPI FindClose(HANDLE);

/* thiscall through fastcall: ecx = this, edx unused, the rest on the stack, callee pops */
typedef void (__fastcall *Register_t)(void* mgr, void* edx, char** name, int a, int b, int c);
typedef char* (__cdecl *FixedCreate_t)(const char* text);
typedef void (__fastcall *FixedRelease_t)(char** name, void* edx);

static Register_t     g_orig;
static FixedCreate_t  g_create;
static FixedRelease_t g_release;
static int            g_last;          /* Order=last */
static int            g_rounds;        /* registrations by the exe (start, data reload) */
static BYTE*          g_async;         /* [14CED60]: archive opening goes to the loading threads (0x68F03E) */
static int            g_sync;          /* [Overlay] Sync=1: ours are opened at once (a test, 07.11) */
static int            g_dot = 1;       /* [Overlay] DotPath=1: registered as Packed\.\Reborn\ (07.11) */

static int same(Module m, DWORD va, const char* hex)
{
    BYTE* p = VA(m, va); int n = s_len(hex) / 2;
    if (!readable(p, n)) return 0;
    for (int i = 0; i < n; i++) {
        char a = hex[i * 2], b = hex[i * 2 + 1];
        int v = ((a <= '9' ? a - '0' : (a | 32) - 'a' + 10) << 4) | (b <= '9' ? b - '0' : (b | 32) - 'a' + 10);
        if (p[i] != (BYTE)v) return 0;
    }
    return 1;
}

static int ends_with(const char* s, const char* tail)
{
    if (!s) return 0;
    int n = s_len(s), t = s_len(tail);
    if (n < t) return 0;
    for (int i = 0; i < t; i++) if (lc(s[n - t + i]) != lc(tail[i])) return 0;
    return 1;
}

static int less(const char* a, const char* b)
{
    for (;; a++, b++) {
        char x = lc(*a), y = lc(*b);
        if (x != y) return x < y;
        if (!x) return 0;
    }
}

static void copy(char* d, const char* s, int cap)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static char g_first[256];               /* [Overlay] First=a.dv2,b.dv2: registered as Packed\.\Reborn\ */

static int in_first(const char* f)
{
    for (const char* p = g_first; *p; ) {
        int k = 0;
        while (p[k] && p[k] != ',' && f[k] && lc(p[k]) == lc(f[k])) k++;
        if ((!p[k] || p[k] == ',') && !f[k]) return 1;
        while (*p && *p != ',') p++;
        while (*p == ',' || *p == ' ') p++;
    }
    return 0;
}

/* registers every <dir of slot>\Reborn\*.dv2 with the patch flags */
static void register_overlay(void* mgr, void* edx, const char* slot)
{
    static char names[MAX_OVERLAY][MAX_PATH];
    char dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    int cut = -1, n = 0;
    for (int i = 0; slot[i]; i++) if (slot[i] == '\\' || slot[i] == '/') cut = i;
    if (cut < 0 || cut + 8 >= MAX_PATH - 16) { logf("Overlay: unexpected slot name '%s', nothing added", slot); return; }
    copy(dir, slot, cut + 2);                                   /* "...\Packed\" */
    /* 07.11: the game reads the archives in the alphabetical order of their full paths (DKS_Patch, FOV_Patch, GFX, GUI,
       MainData*, Patch*, then Reborn\*: ReadFile trace), and of two copies of a file the earlier path won: our skill
       prototypes lost to FOV_Patch_2/MainDataStartup. "Packed\.\Reborn\" is the same folder for Windows and sorts
       before every letter. Only for the archives named in [Overlay] First= (default MageData.dv2, the skill XML alone):
       given to all of them, files that used to lose came into play (the hero's KF/KFM with the appended sequences,
       Textures.bin, fonts) and the camera no longer turned up and down. DotPath=0 turns it off. */
    copy(dir + cut + 1, "Reborn\\", MAX_PATH - cut - 1);
    copy(pattern, dir, MAX_PATH);
    copy(pattern + s_len(pattern), "*.dv2", MAX_PATH - s_len(pattern));

    FindDataA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (g_rounds == 1) logf("Overlay: no archives in %s", dir);
        return;
    }
    do {
        if (fd.dwFileAttributes & 0x10) continue;               /* directory */
        if (n == MAX_OVERLAY) { logf("Overlay: more than %d archives, the rest is left out", MAX_OVERLAY); break; }
        int j = n++;
        while (j > 0 && less(fd.cFileName, names[j - 1])) { copy(names[j], names[j - 1], MAX_PATH); j--; }
        copy(names[j], fd.cFileName, MAX_PATH);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    for (int i = 0; i < n; i++) {
        int d = s_len(dir);
        if (d + s_len(names[i]) >= MAX_PATH) { logf("Overlay: path too long, skipped %s", names[i]); continue; }
        copy(path, dir, MAX_PATH);
        if (g_dot && in_first(names[i])) {                         /* "...\Packed\" + ".\Reborn\" */
            copy(path + cut + 1, ".\\Reborn\\", MAX_PATH - cut - 1);
            d = s_len(path);
        }
        copy(path + d, names[i], MAX_PATH - d);
        char* name = g_create(path);
        if (!name) { logf("Overlay: could not make a name for %s, skipped", path); continue; }
        /* 07.11: 0x68F030 queues the archive's opening to the loading threads while [14CED60] is set, else runs it at
           once (vtbl+20h). Queued, the archives came in by thread timing, and now and then the original prototypes
           (FOV_Patch_2 / MainDataStartup) won over ours: no new skills, and a crash at 0x89F9B0 loading a save that has
           them. Ours are opened at once, before anything queued. */
        /* (07.11: opening ours at once did not help either - still the 78 original entries - so it is off again:
           [Overlay] Sync=1 turns it on for tests) */
        BYTE was = g_async ? *g_async : 0;
        if (g_async && g_sync) *g_async = 0;
        g_orig(mgr, edx, &name, 0, 0, 1);
        if (g_async && g_sync) *g_async = was;
        g_release(&name, 0);
        logf("Overlay: registered %s (%s the patch slots, round %d%s)", path, g_last ? "after" : "before", g_rounds,
             !g_sync ? "" : g_async ? (was ? ", opened at once instead of queued" : ", opened at once") : ", queue flag unknown");
    }
}

static void __fastcall hk_register(void* mgr, void* edx, char** name, int a, int b, int c)
{
    const char* n = (name && readable(name, 4) && *name && readable(*name, 1)) ? *name : 0;
    int before = !g_last && c == 1 && ends_with(n, "\\DKS_Patch.dv2");
    int after = g_last && c == 1 && ends_with(n, "\\Patch_4.dv2");
    if (before || after) g_rounds++;
    if (before) register_overlay(mgr, edx, n);
    /* 07.11: opening every archive at once crashed the start (0x846954, before the menu). The 12 patch slots (c = 1) are
       opened at once, in order, so that ours (Order=last, after Patch_4) come in after them: of two copies of a file the
       archive added last wins (the 'Skills' asset had the 78 entries of FOV_Patch_2 while ours came first) */
    /* (tried 07.11 with Order=last: still the 78 original entries, and the cursor was gone: reverted) */
    g_orig(mgr, edx, name, a, b, c);
    if (after) register_overlay(mgr, edx, n);
}

void overlay_init(Module m)
{
    if (!GetPrivateProfileIntA("Overlay", "Enable", 1, g_ini)) { logf("Overlay: disabled (ini [Overlay] Enable=0)"); return; }
    char order[16];
    GetPrivateProfileStringA("Overlay", "Order", "first", order, sizeof order, g_ini);
    g_last = lc(order[0]) == 'l';
    g_sync = GetPrivateProfileIntA("Overlay", "Sync", 0, g_ini);
    g_dot = GetPrivateProfileIntA("Overlay", "DotPath", 1, g_ini);
    GetPrivateProfileStringA("Overlay", "First", "MageData.dv2", g_first, sizeof g_first, g_ini);

    /* the exe can be relocated: opcodes and embedded addresses are compared separately */
    DWORD d = (DWORD)m.base - 0x400000;
    BYTE* reg = VA(m, 0x697D30);
    BYTE* rel = VA(m, 0x401170);
    /* 697D30: mov eax,[14CED24] / push esi / push edi / push eax */
    int okReg = same(m, 0x697D30, "a1") && *(DWORD*)(reg + 1) == 0x14CED24 + d && same(m, 0x697D35, "565750");
    /* 4B34D0: sub esp,0Ch / push ebx / mov ebx,[esp+14h] */
    int okCreate = same(m, 0x4B34D0, "83ec0c538b5c2414");
    /* 401170: push edi / mov edi,ecx / mov eax,[edi] / cmp eax,[12EF960] */
    int okRelease = same(m, 0x401170, "578bf98b073b05") && readable(rel + 7, 4) && *(DWORD*)(rel + 7) == 0x12EF960 + d;
    if (!okReg || !okCreate || !okRelease) {
        logf("Overlay: code mismatch (register %d, name %d, release %d) -> no overlay archives", okReg, okCreate, okRelease);
        return;
    }
    /* 697D74: call 68F030; 68F03E: cmp byte [14CED60],0 / jne (queue) (checked in the exe 07.11) */
    BYTE* cq = VA(m, 0x697D74);
    if (cq[0] == 0xE8 && (DWORD)(cq + 5 + *(int*)(cq + 1)) == 0x68F030 + d && same(m, 0x68F03E, "803d") &&
        *(DWORD*)VA(m, 0x68F040) == 0x14CED60 + d && same(m, 0x68F044, "0075"))
        g_async = VA(m, 0x14CED60);
    else logf("Overlay: archive queue code differs -> overlay archives are queued as before");
    g_create = (FixedCreate_t)VA(m, 0x4B34D0);
    g_release = (FixedRelease_t)VA(m, 0x401170);
    g_orig = (Register_t)detour(reg, (void*)hk_register);
    if (!g_orig) { logf("Overlay: detour of the archive registration refused -> no overlay archives"); return; }
    logf("Overlay: on, Data\\Win32\\Packed\\Reborn\\*.dv2 %s the patch slots", g_last ? "after" : "before");
}
