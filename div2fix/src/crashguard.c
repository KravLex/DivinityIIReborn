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

/* BucketGuard (03.11): 0xB929A0 thiscall(obj, type) puts obj into a render bucket. For type 0x11 it reads obj's
   "BucketID" extra data (0x4DB330 with the name at [0x14FB394]); without one it runs 0xB92750(obj), which gives
   every geometry under obj a BucketID but not obj itself, and then calls itself again (vtable +14h) for the same
   obj: an object that is not a geometry recurses until the stack is gone (crash at the tower teleport, 51666 frames of
   0xB92A0C). The repeat now happens only when obj has a BucketID by then. */
static volatile long g_bucketSkips;

/* 03.11: skipping such an object left the ground unlit (it was the terrain): its geometries go into their buckets
   instead, each with the BucketID 0xB92750 gave it. The geometry list as 0xB92750 builds it: 0x4DFC10 ctor, vtable
   0x13101C4, 0xDEF1F0(obj, &list, 0) fills it (+4 data, +0Ch count); freed with vtable 0x13101A4, 0x5FAB50(data),
   0x4DFC20. */
typedef struct { DWORD vt; void** data; DWORD cap; DWORD count; DWORD grow; } GeomList;
typedef void  (__fastcall *ListCtor_t)(GeomList* l, void* edx);
typedef void  (__cdecl *Gather_t)(void* obj, GeomList* l, int flags);
typedef void  (__cdecl *Free_t)(void* p);
typedef void* (__fastcall *Extra_t)(void* obj, void* edx, void* name);
typedef void  (__fastcall *Bucket_t)(void* self, void* edx, void* obj, int type);
static ListCtor_t g_listCtor, g_listDtor;
static Gather_t   g_gather;
static Free_t     g_free;
static Extra_t    g_extra;
static void*      g_bucketName;
static DWORD      g_vtList, g_vtListBase;

/* NiIsKindOf 0x459420 thiscall(obj, rtti) for the trace below: NiGeometry RTTI 0x14EF600, NiNode 0x14EFA18 */
typedef int  (__fastcall *KindOf_t)(void* obj, void* edx, void* rtti);
static KindOf_t g_kindOf;
static void*    g_rttiGeom;
static void*    g_rttiNode;
static void*    g_skipName;

static void __stdcall bucket_geometries(void* self, void* obj)
{
    GeomList l;
    g_listCtor(&l, 0);
    l.vt = g_vtList; l.data = 0; l.cap = 0; l.count = 0;
    g_gather(obj, &l, 0);
    int with = 0, without = 0;
    for (DWORD i = 0; i < l.count && l.data; i++) {
        void* g = l.data[i];
        if (g && g != obj && g_extra(g, 0, g_bucketName)) {        /* has a BucketID: no recursion possible */
            (*(Bucket_t*)(*(BYTE**)self + 0x14))(self, 0, g, 0x11);
            with++;
        } else if (g && g != obj) without++;
    }
    /* 03.11 trace (the ground still vanished now and then after the tower teleport): which object, how many of its
       geometries went into buckets and how many were dropped. NiObjectNET name: NiFixedString at [obj+8] */
    if (g_bucketSkips <= 40) {
        const char* nm = readable((BYTE*)obj + 8, 4) ? *(const char**)((BYTE*)obj + 8) : 0;
        /* 0xDEF1F0 gathers a NiGeometry itself, walks the children ([node+68h], count word [node+6Eh]) of a NiNode
           (RTTI 0x14EFA18) unless its name is the fixed string [0x150605C] */
        int geom = g_kindOf ? g_kindOf(obj, 0, g_rttiGeom) != 0 : -1;
        int node = g_kindOf ? g_kindOf(obj, 0, g_rttiNode) != 0 : -1;
        int kids = node == 1 && readable((BYTE*)obj + 0x6E, 2) ? *(WORD*)((BYTE*)obj + 0x6E) : -1;
        const char* skipName = g_skipName && readable(g_skipName, 4) ? *(const char**)g_skipName : 0;
        char mod[260] = "?"; MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(*(void**)obj, &mi, sizeof mi) && mi.AllocationBase)
            GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, sizeof mod);
        logf("BucketGuard:   vtable %p is in %s, BucketID lookup now %p", *(void**)obj, mod, g_extra(obj, 0, g_bucketName));
        logf("BucketGuard: object %p '%s' (vtable %p, geometry %d, node %d, %d children%s, props %p) had no BucketID: "
             "%d geometries bucketed, %d dropped", obj, nm && readable(nm, 1) ? nm : "?", *(void**)obj, geom, node, kids,
             nm && nm == skipName ? ", the skipped name" : "", *(void**)((BYTE*)obj + 0x64), with, without);
    }
    l.vt = g_vtListBase;
    g_free(l.data);
    g_listDtor(&l, 0);
}

static void bucket_guard(Module m)
{
    static const BYTE k_site[] = { 0x8B, 0x16, 0x8B, 0x42, 0x14, 0x6A, 0x11, 0x57, 0x8B, 0xCE, 0xFF, 0xD0, 0x5F, 0x5E };
    static const BYTE k_pre[] = { 0x57, 0xE8 };                              /* B929F3: push edi / call B92750 */
    BYTE* p = VA(m, 0xB92A00);
    BYTE* pre = VA(m, 0xB929F3);
    BYTE* look = VA(m, 0xB929B1);                                             /* push 14FB394 / mov ecx,edi / call 4DB330 */
    int ok = readable(p, sizeof k_site) && readable(pre, 7) && readable(look, 12);
    for (int i = 0; ok && i < (int)sizeof k_site; i++) if (p[i] != k_site[i]) ok = 0;
    for (int i = 0; ok && i < (int)sizeof k_pre; i++) if (pre[i] != k_pre[i]) ok = 0;
    if (ok && (DWORD)(pre + 6) + *(long*)(pre + 2) != (DWORD)VA(m, 0xB92750)) ok = 0;
    if (ok && (look[0] != 0x68 || *(DWORD*)(look + 1) != (DWORD)VA(m, 0x14FB394) || look[5] != 0x8B || look[6] != 0xCF ||
               look[7] != 0xE8 || (DWORD)(look + 12) + *(long*)(look + 8) != (DWORD)VA(m, 0x4DB330))) ok = 0;
    /* B92757: lea ecx,[esp+14h] / call 4DFC10; B92779: mov [esp+20h],13101C4h / call DEF1F0; B928AE: mov [esp+18h],
       13101A4h / call 5FAB50 ... call 4DFC20 */
    BYTE* g1 = VA(m, 0xB9275B), *g2 = VA(m, 0xB92781), *g3 = VA(m, 0xB928B6), *g4 = VA(m, 0xB928C2);
    if (ok && !(readable(g1, 5) && readable(g4, 5) && g1[0] == 0xE8 && (DWORD)(g1 + 5) + *(long*)(g1 + 1) == (DWORD)VA(m, 0x4DFC10) &&
                g2[0] == 0xE8 && (DWORD)(g2 + 5) + *(long*)(g2 + 1) == (DWORD)VA(m, 0xDEF1F0) &&
                g3[0] == 0xE8 && (DWORD)(g3 + 5) + *(long*)(g3 + 1) == (DWORD)VA(m, 0x5FAB50) &&
                g4[0] == 0xE8 && (DWORD)(g4 + 5) + *(long*)(g4 + 1) == (DWORD)VA(m, 0x4DFC20) &&
                *(DWORD*)VA(m, 0xB9277D) == (DWORD)VA(m, 0x13101C4) && *(DWORD*)VA(m, 0xB928B2) == (DWORD)VA(m, 0x13101A4)))
        ok = 0;
    if (!ok) { logf("BucketGuard: site mismatch -> off"); return; }
    g_listCtor = (ListCtor_t)VA(m, 0x4DFC10);
    g_listDtor = (ListCtor_t)VA(m, 0x4DFC20);
    g_gather = (Gather_t)VA(m, 0xDEF1F0);
    g_free = (Free_t)VA(m, 0x5FAB50);
    g_extra = (Extra_t)VA(m, 0x4DB330);
    g_bucketName = VA(m, 0x14FB394);
    g_vtList = (DWORD)VA(m, 0x13101C4);
    g_vtListBase = (DWORD)VA(m, 0x13101A4);
    if (same(m, 0x459420, "568bf1")) {
        g_kindOf = (KindOf_t)VA(m, 0x459420);
        g_rttiGeom = VA(m, 0x14EF600);
        g_rttiNode = VA(m, 0x14EFA18);
        g_skipName = VA(m, 0x150605C);
    }

    BYTE* c = (BYTE*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!c) return;
    BYTE* q = c;
    *q++ = 0x68; *(DWORD*)q = (DWORD)VA(m, 0x14FB394); q += 4;                /* push "BucketID"            */
    *q++ = 0x8B; *q++ = 0xCF;                                                 /* mov  ecx,edi (obj)         */
    *q++ = 0xB8; *(DWORD*)q = (DWORD)VA(m, 0x4DB330); q += 4;                 /* mov  eax,GetExtraData      */
    *q++ = 0xFF; *q++ = 0xD0;                                                 /* call eax (thiscall, ret 4) */
    *q++ = 0x85; *q++ = 0xC0;                                                 /* test eax,eax               */
    *q++ = 0x74; BYTE* j1 = q++;                                              /* jz   skip                  */
    for (int i = 0; i < 12; i++) *q++ = k_site[i];                            /* the original repeat        */
    *q++ = 0xE9; *(long*)q = (long)(VA(m, 0xB92A0C) - (q + 4)); q += 4;       /* jmp  0xB92A0C              */
    *j1 = (BYTE)(q - (j1 + 1));
    *q++ = 0xF0; *q++ = 0xFF; *q++ = 0x05; *(DWORD*)q = (DWORD)&g_bucketSkips; q += 4; /* lock inc [skips] */
    *q++ = 0x57;                                                              /* push edi (obj)             */
    *q++ = 0x56;                                                              /* push esi (self)            */
    *q++ = 0xB8; *(DWORD*)q = (DWORD)bucket_geometries; q += 4;               /* mov  eax,bucket_geometries */
    *q++ = 0xFF; *q++ = 0xD0;                                                 /* call eax (stdcall)         */
    *q++ = 0xE9; *(long*)q = (long)(VA(m, 0xB92A0C) - (q + 4)); q += 4;       /* jmp  0xB92A0C              */

    BYTE jmp[12]; jmp[0] = 0xE9; *(long*)(jmp + 1) = (long)(c - (p + 5));
    for (int i = 5; i < 12; i++) jmp[i] = 0x90;
    if (write_mem(p, jmp, 12)) logf("BucketGuard: an object without a BucketID no longer recurses (0xB92A00)");
}

/* ExtraDataOrder (03.11): a NiObjectNET keeps its extra data sorted by the name's fixed string pointer ([obj+10h],
   count word [obj+14h], name at [extra+8] via 0x4FC6B0): insert 0x4DB110 bubbles the new entry down, GetExtraData
   0x4DB330 and RemoveExtraData 0x4DB3D0 search it binary. All three compare with "sub r,[eax] / je / test r,r / jle":
   a signed difference of two pointers. The game runs large address aware; once fixed strings live above 2 GB the
   order is no longer total, the search misses entries that are there, and 0xB92750 adds a "BucketID" that 0xB929A0
   can never find again: the endless recursion at the tower teleport (stack overflow) or, with BucketGuard, ground
   meshes that are never drawn (seen 03.11: geometry 'Editable Poly' with properties, BucketID lookup NULL).
   "test r,r / jle" -> "nop / nop / jb": the same order by the unsigned pointer, using the flags of the sub. */
static void extradata_order(Module m)
{
    static const struct { DWORD va; const char* game; BYTE jcc; } k_sites[] = {
        { 0x4DB203, "2b18742d85db7e20", 0x20 },   /* insert: previous - new, stop when previous < new */
        { 0x4DB395, "2b30741d85f67e06", 0x06 },   /* GetExtraData: name - middle, lower half when name < middle */
        { 0x4DB435, "2b30741d85f67e06", 0x06 },   /* RemoveExtraData: the same search */
    };
    for (int i = 0; i < 3; i++)
        if (!same(m, k_sites[i].va, k_sites[i].game)) { logf("ExtraDataOrder: code at 0x%X differs -> off", k_sites[i].va); return; }
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        BYTE b[4] = { 0x90, 0x90, 0x72, k_sites[i].jcc };
        ok &= write_mem(VA(m, k_sites[i].va + 4), b, 4);
    }
    logf("ExtraDataOrder: extra data sorted and searched by the unsigned name pointer (0x4DB203/0x4DB395/0x4DB435) -> %s",
         ok ? "on" : "write failed");
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
static void stack_report(DWORD esp, DWORD top)
{
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

/* 03.11: the report ran on the exhausted stack and never reached the log; a fresh thread scans the crashed one */
static DWORD g_soEsp, g_soTop;
static DWORD WINAPI stack_thread(void* unused)
{
    (void)unused;
    stack_report(g_soEsp, g_soTop);
    return 0;
}

static long WINAPI veh(ExPointers* ep)
{
    if (ep->rec->code == 0xC00000FD && g_crashLogs < 16) {     /* first, with the least stack: hand over and wait */
        __asm__ volatile("movl %%fs:4, %0" : "=r"(g_soTop));
        g_soEsp = *(DWORD*)(ep->ctx + 0xC4);
        HANDLE h = CreateThread(0, 0, stack_thread, 0, 0, 0);
        if (h) WaitForSingleObject(h, 10000);
    }
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
    return 0;
}

/* 04.11: the crash at 0xE5B2F1, in the AI's path code (Kynogon, 0xE5AE90; 3 times in fights, with and without moved enemies).
   A path query keeps a marked pointer to its result in [query+10h] (E5B22E: or esi,80000000h); here it is read:
   E5B2EC: and eax,7FFFFFFFh / E5B2F1: mov eax,[eax], and the memory can be gone by then. Not readable: the function's own
   "no result" way, E5B306 (writes -1), as for an unmarked value at E5B2E3. TODO: who frees it is not known. */
static void* g_pathOk, *g_pathNone;
static int   g_pathLogged;

static int __cdecl path_check(void* at)
{
    if (readable(at, 4)) return 1;
    if (g_pathLogged++ < 16) logf("PathGuard: a path query's result at %p is gone -> no result (0xE5B2F1)", at);
    return 0;
}

static __declspec(naked) void path_stub(void)
{
    __asm {
        and eax, 0x7FFFFFFF
        pushad
        push eax
        call path_check
        add esp, 4
        test eax, eax
        popad
        je none
        jmp g_pathOk
    none:
        jmp g_pathNone
    }
}

static void path_guard(Module m)
{
    /* E5B2E5: mov ecx,[esp+180h] / and eax,7FFFFFFFh / mov eax,[eax] / and eax,7FFFFFFFh; E5B306: mov edx,[esp+180h] / mov dword [edx],-1 */
    if (!same(m, 0xE5B2E5, "8b8c248001000025ffffff7f8b0025ffffff7f") || !same(m, 0xE5B306, "8b942480010000c702ffffffff")) {
        logf("PathGuard: bytes at 0xE5B2E5/0xE5B306 differ -> off");
        return;
    }
    g_pathOk = VA(m, 0xE5B2F1);
    g_pathNone = VA(m, 0xE5B306);
    BYTE* at = VA(m, 0xE5B2EC);
    BYTE jmp[5] = { 0xE9 };
    long rel = (long)((BYTE*)path_stub - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    if (write_mem(at, jmp, 5)) logf("PathGuard: a path query's lost result no longer crashes (0xE5B2EC)");
}

/* 05.11 diagnostics ([General] ShadowTrace=1): the necrosaur's torso is not in the shadow map. 0xB8D2C0 thiscall(self, a, mask, list)
   ret 0Ch filters the shadow pass's objects: list = {items, count}; an object stays when [obj+0ACh] & mask and its vtable+0BCh agrees.
   Logs the Creature_ objects going in with their mask, and which of them stay. */
typedef void (__fastcall *ShadowFilter_t)(void* self, void* edx, void* a, DWORD mask, void** list);
static ShadowFilter_t g_shadowFilter;
static int g_shadowLogged;

static const char* obj_name(BYTE* o)
{
    const char* n = o && readable(o + 8, 4) ? *(const char**)(o + 8) : 0;
    return n && readable(n, 8) ? n : 0;
}

static int is_creature(const char* n)
{
    return n && n[0] == 'C' && n[1] == 'r' && n[2] == 'e' && n[3] == 'a' && n[4] == 't' && n[5] == 'u' && n[6] == 'r' && n[7] == 'e';
}

/* one container of the shadow list: what its walk 0xB4CB20 sees: the children [+11Ch] count [+124h], and [+130h], [+134h] with their
   object [+18h]. A little tree of each child as well, by the NiNode layout: children {array [+0A0h], count word [+0A6h]} is NOT known
   here, so only names are logged. */
static void trace_part(const char* what, BYTE* o)
{
    const char* n = o ? obj_name(o) : 0;
    logf("ShadowTrace:     %s %p vt %p '%s'", what, o, o && readable(o, 4) ? *(void**)o : 0, n ? n : "?");
}

static void trace_container(BYTE* o, DWORD mask)
{
    if (!readable(o + 0x11C, 0x20)) return;
    BYTE** kids = *(BYTE***)(o + 0x11C);
    DWORD cnt = *(DWORD*)(o + 0x124);
    BYTE* a = *(BYTE**)(o + 0x130), *b = *(BYTE**)(o + 0x134);
    logf("ShadowTrace:   container %p vt %p [+ACh] %X mask %X '%s': %u children, [+130h] %p, [+134h] %p", o, *(void**)o, *(DWORD*)(o + 0xAC), mask,
         obj_name(o) ? obj_name(o) : "?", cnt, a, b);
    if (kids && cnt && cnt < 200 && readable(kids, cnt * 4))
        for (DWORD i = 0; i < cnt && i < 32; i++) {
            BYTE* k = kids[i];
            if (k && readable(k, 12)) trace_part("child", k);
        }
    if (a && readable(a + 0x18, 4)) { trace_part("[+130h]", a); trace_part("[+130h].18", *(BYTE**)(a + 0x18)); }
    if (b && readable(b + 0x18, 4)) { trace_part("[+134h]", b); trace_part("[+134h].18", *(BYTE**)(b + 0x18)); }
}

static void __fastcall hk_shadowFilter(void* self, void* edx, void* a, DWORD mask, void** list)
{
    BYTE* before[32];
    int nb = 0, total = list ? (int)(DWORD)list[1] : 0;
    if (g_shadowLogged < 300 && total > 0 && readable(list[0], total * 4))
        for (int i = 0; i < total && nb < 32; i++) {
            BYTE* o = ((BYTE**)list[0])[i];
            if (is_creature(obj_name(o))) before[nb++] = o;
        }
    /* what the list holds at all: the first calls, every object */
    /* 05.11, third pass: the first containers of each kind, after the scene is up (a few seconds of frames) */
    static int frames, shown;
    if (++frames > 4000 && frames % 200 == 0 && shown < 36 && total > 0 && readable(list[0], total * 4)) {
        for (int i = 0; i < total && shown < 36; i++) {
            BYTE* o = ((BYTE**)list[0])[i];
            DWORD m = o && readable(o + 0xAC, 4) ? *(DWORD*)(o + 0xAC) : 0;
            /* 0xB487E0's class (characters: [+1BCh] model; gathers [+11Ch] and, unless hidden, [+130h].18, [+134h].18): those holding
               Creature_ parts first, the first few others too */
            static int others;
            if (m != 0x400 || !readable(o + 0x11C, 12)) continue;
            int creature = 0;
            BYTE** kids = *(BYTE***)(o + 0x11C);
            DWORD cnt = *(DWORD*)(o + 0x124);
            if (kids && cnt && cnt < 64 && readable(kids, cnt * 4))
                for (DWORD q = 0; q < cnt; q++) if (kids[q] && is_creature(obj_name(kids[q]))) creature = 1;
            if (creature || others < 4) { if (!creature) others++; shown++; trace_container(o, mask); }
        }
    }
    static int sample;
    if (sample < 0 && total > 0 && readable(list[0], total * 4)) {
        sample++;
        logf("ShadowTrace: call: mask %X, %d objects", mask, total);
        for (int i = 0; i < total && i < 40; i++) {
            BYTE* o = ((BYTE**)list[0])[i];
            const char* n = obj_name(o);
            logf("ShadowTrace:   %p vt %p [+ACh] %X '%s'", o, o && readable(o, 4) ? *(void**)o : 0, o && readable(o + 0xAC, 4) ? *(DWORD*)(o + 0xAC) : 0, n ? n : "?");
        }
    }
    g_shadowFilter(self, edx, a, mask, list);
    for (int k = 0; k < nb && g_shadowLogged < 300; k++) {
        int stays = 0;
        int after = (int)(DWORD)list[1];
        for (int i = 0; i < after; i++) if (((BYTE**)list[0])[i] == before[k]) stays = 1;
        g_shadowLogged++;
        logf("ShadowTrace: mask %X, %s: [+ACh]=%X, %s", mask, obj_name(before[k]), *(DWORD*)(before[k] + 0xAC), stays ? "stays" : "REMOVED");
    }
}

static void shadow_trace(Module m)
{
    if (!same(m, 0xB8D2C0, "515355578b7c241c") && !same(m, 0xB8D2C0, "515355578b7c241c")) { logf("ShadowTrace: bytes at 0xB8D2C0 differ -> off"); return; }
    g_shadowFilter = (ShadowFilter_t)detour(VA(m, 0xB8D2C0), (void*)hk_shadowFilter);
    logf("ShadowTrace: %s", g_shadowFilter ? "on (0xB8D2C0)" : "NOT installed");
}

/* 05.11: the necrosaur's "Pet" parts (and any skinned part with an alpha property) cast no shadow in the VSM cascades. The shadow
   renderer [obj+44h] has four shader slots: rigid +0Ch (0xBD5530), skinned +10h (0xBD5580), alpha +14h (0xBD55D0), skinned alpha +18h
   (0xBD5620). The library has no div2_ShadowMapRenderShadow_AlphaSkinned_VSM and the cascades (0xC32946.., 0xC33B29..) set only the first
   three, so that slot stays empty. Here it gets the cascade's skinned shader [obj+418h]: no alpha test, the whole part casts. */
typedef void (__fastcall *SetSlot_t)(void* renderer, void* edx, void* shader);
static SetSlot_t g_setAlpha, g_setAlphaSkinned;
static BYTE* g_cascade;
static BYTE* g_vsmFlag;

static void __fastcall alpha_slots(void* renderer, void* edx, void* shader)
{
    g_setAlpha(renderer, edx, shader);
    if (*g_vsmFlag) g_setAlphaSkinned(renderer, edx, *(void**)(g_cascade + 0x418));
}

static __declspec(naked) void alpha_stub(void)
{
    __asm {
        mov g_cascade, esi
        jmp alpha_slots
    }
}

static void skinned_alpha_shadow(Module m)
{
    BYTE* c[2] = { VA(m, 0xC32959), VA(m, 0xC33B3C) };
    DWORD d = (DWORD)m.base - 0x400000;
    int ok = same(m, 0xBD5620, "53568bd98b7318") && same(m, 0xC3294B, "8b861c040000") && same(m, 0xC33B2E, "8b861c040000") &&
             same(m, 0xC3293C, "8b9618040000") && same(m, 0xC339A5, "803d") && *(DWORD*)VA(m, 0xC339A7) == 0x14D0F0B + d;
    for (int i = 0; i < 2; i++) ok = ok && c[i][0] == 0xE8 && (DWORD)(c[i] + 5 + *(int*)(c[i] + 1)) == 0xBD55D0 + d;
    if (!ok) { logf("SkinnedAlphaShadow: bytes differ -> off"); return; }
    g_setAlpha = (SetSlot_t)VA(m, 0xBD55D0);
    g_setAlphaSkinned = (SetSlot_t)VA(m, 0xBD5620);
    g_vsmFlag = VA(m, 0x14D0F0B);
    int n = 0;
    for (int i = 0; i < 2; i++) {
        BYTE jmp[5] = { 0xE8 };
        long rel = (long)((BYTE*)alpha_stub - (c[i] + 5));
        memcpy(jmp + 1, &rel, 4);
        n += write_mem(c[i], jmp, 5) != 0;
    }
    logf("SkinnedAlphaShadow: %d of 2 call sites (0xC32959, 0xC33B3C): skinned parts with alpha cast VSM shadows", n);
}

void crashguard_init(Module m)
{
    if (GetPrivateProfileIntA("General", "SkinnedAlphaShadow", 1, g_ini)) skinned_alpha_shadow(m);
    if (GetPrivateProfileIntA("General", "ShadowTrace", 0, g_ini)) shadow_trace(m);
    if (GetPrivateProfileIntA("General", "FSMaterialGuard", 1, g_ini)) fsmaterial_guard(m);
    else logf("FSMaterialGuard: off (ini)");
    if (GetPrivateProfileIntA("General", "ExtraDataOrder", 1, g_ini)) extradata_order(m);
    else logf("ExtraDataOrder: off (ini)");
    if (GetPrivateProfileIntA("General", "BucketGuard", 1, g_ini)) bucket_guard(m);
    else logf("BucketGuard: off (ini)");
    if (GetPrivateProfileIntA("General", "PathGuard", 1, g_ini)) path_guard(m);
    else logf("PathGuard: off (ini)");
    if (GetPrivateProfileIntA("General", "CrashLog", 1, g_ini)) {
        if (AddVectoredExceptionHandler(0, veh)) logf("CrashLog: exceptions are written to this log (first 16)");
    }
}

void crashguard_exit(void)
{
    if (g_fsSkips) logf("FSMaterialGuard: %d material updates skipped instead of crashing", (int)g_fsSkips);
    if (g_bucketSkips) logf("BucketGuard: %d bucket repeats skipped instead of recursing", (int)g_bucketSkips);
}
