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
        if (v < 0x10000 || !readable((void*)(v - 6), 6)) continue;
        const BYTE* q = (const BYTE*)v;
        if (q[-5] != 0xE8 && !(q[-6] == 0xFF && q[-5] == 0x15) && !(q[-2] == 0xFF && (q[-1] & 0xF8) == 0xD0) && !(q[-3] == 0xFF && (q[-2] & 0xF8) == 0x50)) continue;
        mod_name(v, mm, &o); if (!mm[0]) continue;
        n += wsprintfA(line + n, " <- %s+%08X", mm, o); found++;
        if (n > 420) break;
    }
    logf("%s", line);
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
