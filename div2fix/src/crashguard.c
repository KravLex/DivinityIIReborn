/* Div2Fix - crash guards and crash log.
 *
 * 1) FSMaterial guard ([General] FSMaterialGuard=1).
 *    Divinity2.exe v1.4.700.56, 0xBA4FC0 (cdecl, one object): called by the material code when it meets an extra
 *    data entry named "FSMaterial" on a scene object (caller 0xBA5AA8 compares the name). It looks the entry up again
 *    (NiObjectNET::GetExtraData 0x4DB330), casts it (NiIsKindOf 0x459420, NiStringExtraData) and reads the string at
 *    [result+0Ch] without a NULL check. Crashes seen at 0xBA4FE8 (28.10 with d3d9.textureMemory=512, 29.10 with
 *    1.4.2) - the lookup or the cast returned NULL right after the name was seen, which points to the object being
 *    changed by another thread meanwhile (timing dependent). The guard returns from the function (its own epilogue
 *    0xBA5074: pop edi / pop esi / ret) when either result is NULL: that object skips this material update.
 *    Patched: 0xBA4FD2..0xBA4FE7 (test eax,eax / jnz / xor edi,edi / jmp / push RTTI / mov ecx,eax / call / mov edi,eax)
 *    -> jmp to a code cave doing the same with both checks. The number of skips is logged at exit.
 *
 * 2) Crash log ([General] CrashLog=1): a vectored exception handler writes the code, the module+offset and the
 *    first return addresses of an access violation / illegal instruction / stack overflow / divide by zero to
 *    Div2Fix.log, at most 16 entries. First-chance only (the game may handle some itself) - it never changes how an
 *    exception is handled.
 *    Each entry also gets the address space (used / free / largest free block / private MB); a stack overflow also
 *    gets a scan of the whole stack (return addresses per module, the outermost calls in the game).
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))

static volatile long g_fsSkips;

static void fsmaterial_guard(Module m)
{
    static const BYTE k_site[] = { 0x85, 0xC0, 0x75, 0x04, 0x33, 0xFF, 0xEB, 0x0E, 0x68 };
    BYTE* p = VA(m, 0xBA4FD2);
    int ok = readable(p, 22) && readable(VA(m, 0xBA5074), 3);
    for (int i = 0; ok && i < (int)sizeof k_site; i++) if (p[i] != k_site[i]) ok = 0;
    if (ok && *(DWORD*)(p + 9) != (DWORD)VA(m, 0x14EFBB8)) ok = 0;                      /* push NiStringExtraData RTTI */
    if (ok && (p[13] != 0x8B || p[14] != 0xC8 || p[15] != 0xE8 || p[20] != 0x8B || p[21] != 0xF8)) ok = 0;
    if (ok && (DWORD)(p + 20) + *(long*)(p + 16) != (DWORD)VA(m, 0x459420)) ok = 0;    /* call NiIsKindOf */
    const BYTE* ep = VA(m, 0xBA5074);
    if (ok && (ep[0] != 0x5F || ep[1] != 0x5E || ep[2] != 0xC3)) ok = 0;
    if (!ok) { logf("FSMaterialGuard: site mismatch -> off"); return; }

    BYTE* c = (BYTE*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!c) return;
    BYTE* q = c;
    *q++ = 0x85; *q++ = 0xC0;                                                 /* test eax,eax              */
    *q++ = 0x74; BYTE* j1 = q++;                                              /* jz   skip                 */
    *q++ = 0x68; *(DWORD*)q = (DWORD)VA(m, 0x14EFBB8); q += 4;                /* push NiStringExtraData    */
    *q++ = 0x8B; *q++ = 0xC8;                                                 /* mov  ecx,eax              */
    *q++ = 0xB8; *(DWORD*)q = (DWORD)VA(m, 0x459420); q += 4;                 /* mov  eax,NiIsKindOf       */
    *q++ = 0xFF; *q++ = 0xD0;                                                 /* call eax (thiscall, ret 4) */
    *q++ = 0x85; *q++ = 0xC0;                                                 /* test eax,eax              */
    *q++ = 0x74; BYTE* j2 = q++;                                              /* jz   skip                 */
    *q++ = 0x8B; *q++ = 0xF8;                                                 /* mov  edi,eax              */
    *q++ = 0xE9; *(long*)q = (long)(VA(m, 0xBA4FE8) - (q + 4)); q += 4;       /* jmp  0xBA4FE8 (original)  */
    BYTE* skip = q;
    *j1 = (BYTE)(skip - (j1 + 1)); *j2 = (BYTE)(skip - (j2 + 1));
    *q++ = 0xF0; *q++ = 0xFF; *q++ = 0x05; *(DWORD*)q = (DWORD)&g_fsSkips; q += 4; /* lock inc [g_fsSkips]  */
    *q++ = 0xE9; *(long*)q = (long)(VA(m, 0xBA5074) - (q + 4)); q += 4;       /* jmp  epilogue             */

    BYTE jmp[22]; jmp[0] = 0xE9; *(long*)(jmp + 1) = (long)(c - (p + 5));
    for (int i = 5; i < 22; i++) jmp[i] = 0x90;
    if (write_mem(p, jmp, 22)) logf("FSMaterialGuard: missing FSMaterial data no longer crashes (0xBA4FC0)");
}

/* ---------------- crash log ---------------- */
typedef struct { DWORD code, flags; void* rec; void* addr; DWORD nparams; DWORD info[15]; } ExRecord;
typedef struct { ExRecord* rec; BYTE* ctx; } ExPointers;
typedef long (WINAPI *VEH_t)(ExPointers*);
__declspec(dllimport) void* WINAPI AddVectoredExceptionHandler(DWORD, VEH_t);
static volatile long g_crashLogs;

static void mod_name(DWORD a, char* out, DWORD* off)
{
    MEMORY_BASIC_INFORMATION mi; out[0] = 0; *off = a;
    if (!VirtualQuery((void*)a, &mi, sizeof mi) || !mi.AllocationBase) return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA((HMODULE)mi.AllocationBase, path, MAX_PATH)) return;
    int b = s_len(path); while (b > 0 && path[b - 1] != '\\') b--;
    int i = 0; for (; path[b + i] && i < 39; i++) out[i] = path[b + i]; out[i] = 0;
    *off = a - (DWORD)mi.AllocationBase;
}
/* v looks like a return address: the bytes before it are a call (E8 rel32, FF 15 [abs], FF D0+r, FF 50+r disp8) */
static int is_return(DWORD v)
{
    if (v < 0x10000 || !readable((void*)(v - 6), 6)) return 0;
    const BYTE* q = (const BYTE*)v;
    return q[-5] == 0xE8 || (q[-6] == 0xFF && q[-5] == 0x15) || (q[-2] == 0xFF && (q[-1] & 0xF8) == 0xD0) || (q[-3] == 0xFF && (q[-2] & 0xF8) == 0x50);
}

/* Stack overflow: the 6 return addresses at the top only show where the stack ran out, not what filled it. Scan
   the whole stack (ESP up to the TEB's StackBase, fs:[4]) for return addresses: how many per module, and the
   outermost ones in the game (the call path that started the deep part). Static buffers: little stack is left. */
typedef struct { DWORD base, n; } ModCount;
static ModCount g_mods[12], g_hot[64];        /* g_hot: game return addresses and how often they occur (the loop) */
static DWORD g_outer[10], g_inner[24];
static char g_sline[1000];
static void stack_report(DWORD esp)
{
    DWORD top; __asm__ volatile("movl %%fs:4, %0" : "=r"(top));
    if (top <= esp || top - esp > 0x4000000) return;
    BYTE* exe = main_module().base;
    int nm = 0, no = 0, nh = 0; DWORD total = 0, pageOk = 0;
    for (DWORD a = esp & ~3u; a < top; a += 4) {
        if ((a & 0xFFF) == 0 || a == (esp & ~3u)) pageOk = readable((void*)a, 4);
        if (!pageOk) continue;
        DWORD v = *(const DWORD*)a;
        if (!is_return(v)) continue;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)v, &mi, sizeof mi) || mi.Type != MEM_IMAGE) continue;
        DWORD b = (DWORD)mi.AllocationBase; int k = 0;
        while (k < nm && g_mods[k].base != b) k++;
        if (k == nm && nm < 12) { g_mods[nm].base = b; g_mods[nm].n = 0; nm++; }
        if (k < nm) g_mods[k].n++;
        total++;
        if ((BYTE*)b == exe) {
            if (no < 24) g_inner[no] = v;
            g_outer[no % 10] = v; no++;                                 /* ring: keeps the 10 closest to the base */
            int h = 0; while (h < nh && g_hot[h].base != v) h++;
            if (h == nh && nh < 64) { g_hot[nh].base = v; g_hot[nh].n = 0; nh++; }
            if (h < nh) g_hot[h].n++;
        }
    }
    int n = wsprintfA(g_sline, "  stack: %u KB used, %u return addresses:", (top - esp) >> 10, total);
    for (int k = 0; k < nm && n < 800; k++) {
        char mm[40]; DWORD o; mod_name(g_mods[k].base, mm, &o);
        n += wsprintfA(g_sline + n, " %s %u", mm[0] ? mm : "?", g_mods[k].n);
    }
    logf("%s", g_sline);
    n = wsprintfA(g_sline, "  outermost game calls (innermost first):");
    for (int i = no > 10 ? no - 10 : 0; i < no && n < 900; i++)
        n += wsprintfA(g_sline + n, " %08X", g_outer[i % 10] - (DWORD)exe + 0x400000);
    logf("%s", g_sline);
    n = wsprintfA(g_sline, "  innermost game calls:");
    for (int i = 0; i < no && i < 24; i++) n += wsprintfA(g_sline + n, " %08X", g_inner[i] - (DWORD)exe + 0x400000);
    logf("%s", g_sline);
    n = wsprintfA(g_sline, "  most frequent game return addresses (address x count):");
    for (int k = 0; k < 10; k++) {                                       /* selection of the 10 biggest counts */
        int best = -1;
        for (int h = 0; h < nh; h++) if (g_hot[h].n && (best < 0 || g_hot[h].n > g_hot[best].n)) best = h;
        if (best < 0) break;
        n += wsprintfA(g_sline + n, " %08X x%u", g_hot[best].base - (DWORD)exe + 0x400000, g_hot[best].n);
        g_hot[best].n = 0;
    }
    logf("%s", g_sline);
}

static long WINAPI veh(ExPointers* ep)
{
    DWORD code = ep->rec->code;
    if (code != 0xC0000005 && code != 0xC000001D && code != 0xC00000FD && code != 0xC0000094) return 0;   /* continue search */
    if (__sync_fetch_and_add(&g_crashLogs, 1) >= 16) return 0;
    char m[40], line[512]; DWORD off;
    mod_name((DWORD)ep->rec->addr, m, &off);
    int n = wsprintfA(line, "Exception %08X at %s+%08X", code, m[0] ? m : "?", off);
    if (code == 0xC0000005 && ep->rec->nparams >= 2) n += wsprintfA(line + n, " (%s %08X)", ep->rec->info[0] ? "write" : "read", ep->rec->info[1]);
    /* first return addresses found on the stack (x86 CONTEXT: Esp at 0xC4) */
    const DWORD* sp = (const DWORD*)*(DWORD*)(ep->ctx + 0xC4);
    int found = 0;
    for (int i = 0; i < 256 && found < 6 && readable(sp + i, 4); i++) {
        DWORD v = sp[i]; char mm[40]; DWORD o;
        if (!is_return(v)) continue;
        mod_name(v, mm, &o); if (!mm[0]) continue;
        n += wsprintfA(line + n, " <- %s+%08X", mm, o); found++;
        if (n > 420) break;
    }
    n += wsprintfA(line + n, " (thread %u)", GetCurrentThreadId());
    logf("%s", line);
    AddrSpace as; addr_space(&as);
    logf("  memory: %u MB used, %u MB free (largest block %u MB), %u MB private", as.usedMB, as.freeMB, as.largestMB, as.privMB);
    if (code == 0xC00000FD) stack_report((DWORD)sp);
    return 0;
}

void crashguard_init(Module m)
{
    if (GetPrivateProfileIntA("General", "FSMaterialGuard", 1, g_ini)) fsmaterial_guard(m);
    else logf("FSMaterialGuard: off (ini)");
    if (GetPrivateProfileIntA("General", "CrashLog", 1, g_ini)) {
        if (AddVectoredExceptionHandler(0, veh)) logf("CrashLog: exceptions are written to this log (first 16)");
    }
}

void crashguard_exit(void)
{
    if (g_fsSkips) logf("FSMaterialGuard: %d material updates skipped instead of crashing", (int)g_fsSkips);
}
