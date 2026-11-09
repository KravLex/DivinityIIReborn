/*
 * Div2Fix — Divinity II: Developer's Cut runtime fixes (dinput8.dll proxy, 32-bit)
 *
 * 1) KeyNames: tutorial/hint key names ("[(W)]") are produced by the game with
 *    ToAsciiEx() using the CURRENT keyboard layout. With a Russian (or any
 *    non-Latin) layout this yields a CP1251 byte that the UI cannot render -> "[(□)]".
 *    We redirect only that one GetKeyboardLayout() call (inside the key-name
 *    builder) to an installed English layout. Text input is untouched.
 *
 * 2) UIAspect: on displays wider than 16:9 the Scaleform menus are laid out for 16:9
 *    but drawn across the whole width (broken pause/menu backgrounds). We hook
 *    GFxMovieRoot::SetViewport and shrink the viewport of selected menu movies to a
 *    centred 16:9 rectangle. HUD, cursor, damage numbers etc. stay full-width.
 *
 * Nothing on disk is modified; all changes are in memory, located by byte patterns
 * and RTTI, and each fix is skipped (and logged) if its pattern is not found.
 */
#include "common.h"

/* ---------- tiny libc replacements (no CRT) ---------- */
void* __cdecl memcpy(void* d, const void* s, SIZE_T n) { BYTE* a = d; const BYTE* b = s; while (n--) *a++ = *b++; return d; }
void* __cdecl memset(void* d, int c, SIZE_T n) { BYTE* a = d; while (n--) *a++ = (BYTE)c; return d; }
int _fltused = 0;

int s_len(const char* s) { int n = 0; while (s[n]) n++; return n; }
char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* ---------- logging & config ---------- */
char g_dir[MAX_PATH], g_ini[MAX_PATH], g_log[MAX_PATH];
int g_logOn = 1;

void logf(const char* fmt, ...)
{
    if (!g_logOn) return;
    char buf[1100];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, 1, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, buf, (DWORD)n, &w, 0); CloseHandle(h);
}

static void init_paths(HMODULE self)
{
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    int i = s_len(g_dir); while (i > 0 && g_dir[i - 1] != '\\') i--; g_dir[i] = 0;
    wsprintfA(g_ini, "%sDiv2Fix.ini", g_dir);
    wsprintfA(g_log, "%sDiv2Fix.log", g_dir);
}

/* ---------- memory helpers ---------- */
int readable(const void* p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mi.Protect == 0) return 0;
    return (const BYTE*)p + n <= (const BYTE*)mi.BaseAddress + mi.RegionSize;
}

int write_mem(void* dst, const void* src, SIZE_T n)
{
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}


Module main_module(void)
{
    Module m; m.base = (BYTE*)GetModuleHandleA(0);
    DWORD pe = *(DWORD*)(m.base + 0x3c);
    m.size = *(DWORD*)(m.base + pe + 24 + 56); /* OptionalHeader.SizeOfImage */
    return m;
}

/* Iterate committed, readable regions of the main module */
#define FOR_REGIONS(m, rs, re) \
    for (BYTE *rs = (m).base, *re = 0, *_p = (m).base; _p < (m).base + (m).size; _p = re) \
        if (!region_of(m, _p, &rs, &re)) break; else if (!region_ok(_p))  ; else

static int region_of(Module m, BYTE* p, BYTE** rs, BYTE** re)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi)) return 0;
    *rs = p; *re = (BYTE*)mi.BaseAddress + mi.RegionSize;
    if (*re > m.base + m.size) *re = m.base + m.size;
    return 1;
}
static int region_ok(BYTE* p) { return readable(p, 1); }

/* pattern: bytes with mask ('x' = must match, '?' = any) */
BYTE* find_pattern(Module m, const BYTE* pat, const char* mask, int* count)
{
    int n = s_len(mask); BYTE* hit = 0; *count = 0;
    FOR_REGIONS(m, rs, re) {
        for (BYTE* q = rs; q + n <= re; q++) {
            if (q[0] != pat[0] && mask[0] == 'x') continue;
            int i = 0;
            for (; i < n; i++) if (mask[i] == 'x' && q[i] != pat[i]) break;
            if (i == n) { if (!hit) hit = q; (*count)++; }
        }
    }
    return hit;
}

/* next 4-byte-aligned occurrence of a dword value at or after 'from' */
static BYTE* find_dword(Module m, DWORD v, BYTE* from)
{
    FOR_REGIONS(m, rs, re) {
        if (re <= from) continue;
        BYTE* q = rs < from ? from : rs;
        q = (BYTE*)(((DWORD)q + 3) & ~3u);
        for (; q + 4 <= re; q += 4) if (*(DWORD*)q == v) return q;
    }
    return 0;
}

/* Locate the primary vtable of an MSVC class by its RTTI type name (e.g. ".?AVGFxMovieRoot@@") */
static void** find_vtable(Module m, const char* tname)
{
    int cnt; int n = s_len(tname) + 1;                     /* include terminating NUL */
    char mask[96]; if (n >= (int)sizeof mask) return 0;
    for (int i = 0; i < n; i++) mask[i] = 'x'; mask[n] = 0;
    BYTE* name = find_pattern(m, (const BYTE*)tname, mask, &cnt);
    if (!name) return 0;
    DWORD td = (DWORD)(name - 8);                          /* TypeDescriptor: pVFTable, spare, name[] */
    for (BYTE* r = find_dword(m, td, m.base); r; r = find_dword(m, td, r + 4)) {
        DWORD* col = (DWORD*)(r - 12);                     /* CompleteObjectLocator: sig, offset, cdOffset, pTD, pCHD */
        if (!readable(col, 20) || col[0] != 0 || col[1] != 0) continue;
        BYTE* v = find_dword(m, (DWORD)col, m.base);
        if (v) return (void**)(v + 4);
    }
    return 0;
}

/* ================= Fix 1: key names ================= */
static HKL g_hklLatin;

static HKL WINAPI hk_GetKeyboardLayout_KeyName(DWORD tid)
{
    HKL cur = GetKeyboardLayout(tid);
    if (g_hklLatin) return g_hklLatin;
    return cur;
}

static void pick_latin_layout(void)
{
    HKL list[64]; int n = GetKeyboardLayoutList(64, list);
    int want = GetPrivateProfileIntA("KeyNames", "LanguageId", 0x0409, g_ini);
    if (want <= 0 || want > 0xFFFF) want = 0x0409;
    HKL anyEn = 0;
    for (int i = 0; i < n; i++) {
        DWORD lang = (DWORD)list[i] & 0xFFFF;
        if (lang == (DWORD)want) { g_hklLatin = list[i]; break; }
        if ((lang & 0x3FF) == 0x09 && !anyEn) anyEn = list[i];      /* any English variant */
    }
    if (!g_hklLatin) g_hklLatin = anyEn;
    logf("KeyNames: %d layouts installed, using HKL %08X%s", n, (DWORD)g_hklLatin,
         g_hklLatin ? "" : " (no English layout installed -> fix inactive, add English layout in Windows)");
}

static void apply_keynames(Module m)
{
    /* call [GetCurrentThreadId]; push eax; call [GetKeyboardLayout]; mov edi,eax; push edi; push 1; push esi;
       call [MapVirtualKeyExA]; push edi; push 0; lea ecx,[esp+14h]; push ecx; lea edx,[esp+0C4h]; push edx; push esi; push eax; call [ToAsciiEx] */
    static const BYTE pat[] = { 0xFF,0x15,0,0,0,0, 0x50, 0xFF,0x15,0,0,0,0, 0x8B,0xF8, 0x57, 0x6A,0x01, 0x56, 0xFF,0x15,0,0,0,0,
        0x57, 0x6A,0x00, 0x8D,0x4C,0x24,0x14, 0x51, 0x8D,0x94,0x24,0xC4,0x00,0x00,0x00, 0x52, 0x56, 0x50, 0xFF,0x15 };
    static const char mask[] = "xx????xxx????xxxxxxxx????xxxxxxxxxxxxxxxxxxxx";
    int cnt; BYTE* p = find_pattern(m, pat, mask, &cnt);
    if (!p || cnt != 1) { logf("KeyNames: pattern not found (count=%d) -> skipped", cnt); return; }
    pick_latin_layout();
    BYTE* call = p + 7;                                       /* the call [GetKeyboardLayout] (6 bytes) */
    BYTE patch[6]; patch[0] = 0xE8;
    *(DWORD*)(patch + 1) = (DWORD)((BYTE*)hk_GetKeyboardLayout_KeyName - (call + 5));
    patch[5] = 0x90;
    if (write_mem(call, patch, 6)) logf("KeyNames: patched at %08X", (DWORD)call);
}

/* 08.11.2026: no limit on the number of saves. 0x6E6EDA (the save function 0x6E6E70): cmp dword [edi+1Ch],64h /
   jae 0x6E7072 -> "Maximum number of save games reached, delete one first." (100 saves). The jae (6 bytes) becomes NOPs;
   the file names (savegame%d) have no limit. */
static void apply_unlimited_saves(Module m)
{
    static const BYTE want[] = { 0x83, 0x7F, 0x1C, 0x64, 0x0F, 0x83, 0x8E, 0x01, 0x00, 0x00 };
    BYTE* p = m.base + (0x6E6EDA - 0x400000);
    int same = readable(p, sizeof want);
    for (int i = 0; same && i < (int)sizeof want; i++) if (p[i] != want[i]) same = 0;
    if (!same) { logf("Saves: bytes at 0x6E6EDA differ -> the 100 saves limit stays"); return; }
    static const BYTE nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    logf("Saves: limit of 100 saves %s", write_mem(p + 4, nop6, 6) ? "removed (0x6E6EDE)" : "NOT removed (write failed)");
}

/* 08.11.2026: skill boosters on armour and weapons. The item booster function 0x8DC900 fills a non-jewel item's
   slots from the effects whose BaseType is NOT "Skill Boosters" (0x8DD24E: call [operator!=(std::string, char*)]),
   a jewel's (category J) only from skill boosters (operator==). The != becomes "true" (mov al,1 + 4 NOPs): armour and
   weapon slots take stats and skill boosters by Frequency (the weights: MageExpansion config/boosters.json). */
static void apply_boosters_everywhere(Module m)
{
    BYTE* p = m.base + (0x8DD24E - 0x400000);
    int ok = readable(p, 6) && p[0] == 0xFF && p[1] == 0x15 &&
             *(BYTE**)(p + 2) == m.base + (0x12B7318 - 0x400000) &&            /* [operator!= import] (relocated) */
             readable(p - 0x12, 5) && p[-0x12] == 0x68;                          /* push "Skill Boosters" before it */
    if (!ok) { logf("SkillBoosters: bytes at 0x8DD24E differ -> armour and weapons keep the game's rule"); return; }
    static const BYTE yes[6] = { 0xB0, 0x01, 0x90, 0x90, 0x90, 0x90 };
    logf("SkillBoosters: armour and weapons can get skill boosters %s", write_mem(p, yes, 6) ? "(0x8DD24E)" : "- write FAILED");
}

/* 08.11.2026 LadderTrace=1 ([Diagnostics], debug): the climbing hero drifts into the air when the camera turns (a bug
   of the original game). The ladder control tactic CRpgStats_V2_Character_State_Tactic_LadderAvatarControl has its
   vtable at 0x1330410 (slot 0 = 0x927930 returns its type 0x14F6438). Every slot gets a counting thunk
   (lock inc [n]; jmp original); a thread logs the counts every 2 s while they change. */
#define LADDER_SLOTS 24
int g_ladderLock;
static volatile long g_ladderN[LADDER_SLOTS];
static DWORD WINAPI ladder_log(void* p)
{
    (void)p;
    long last[LADDER_SLOTS] = { 0 };
    for (int rounds = 0; rounds < 400; ) {
        Sleep(2000);
        int changed = 0;
        for (int i = 0; i < LADDER_SLOTS; i++) if (g_ladderN[i] != last[i]) changed = 1;
        if (!changed) continue;
        char line[600]; int q = wsprintfA(line, "LadderTrace: calls in 2 s:");
        for (int i = 0; i < LADDER_SLOTS; i++) {
            long n = g_ladderN[i];
            if (n != last[i]) q += wsprintfA(line + q, " [%d]=%d", i, n - last[i]);
            last[i] = n;
        }
        logf("%s", line);
        rounds++;
    }
    return 0;
}
/* 08.11: the ladder tactic's update (slot 5, 0x920C80) runs once a frame while the hero is on a ladder: window.c
   then drops the mouse's horizontal motion, so the camera does not turn the climbing hero off the ladder */
int ladder_active(void)
{
    static long lastN; static DWORD lastT;
    long n = g_ladderN[5]; DWORD now = GetTickCount();
    if (n != lastN) { lastN = n; lastT = now; }
    return lastT && now - lastT < 150;
}

static void apply_ladder_trace(Module m, int log)
{
    void** vt = (void**)(m.base + (0x1330410 - 0x400000));
    BYTE* type_fn = m.base + (0x927930 - 0x400000);
    if (!readable(vt, LADDER_SLOTS * 4) || vt[0] != (void*)type_fn || !readable(type_fn, 5) || type_fn[0] != 0xB8) {
        logf("LadderTrace: vtable 0x1330410 not as expected -> not traced"); return;
    }
    BYTE* th = (BYTE*)VirtualAlloc(0, LADDER_SLOTS * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!th) return;
    for (int i = 0; i < LADDER_SLOTS; i++) {
        BYTE* c = th + i * 16;
        c[0] = 0xF0; c[1] = 0xFF; c[2] = 0x05; *(DWORD*)(c + 3) = (DWORD)&g_ladderN[i];      /* lock inc dword [n] */
        c[7] = 0xE9; *(long*)(c + 8) = (long)vt[i] - (long)(c + 12);                       /* jmp original */
        void* h = c;
        write_mem(&vt[i], &h, 4);
    }
    if (log) { HANDLE t = CreateThread(0, 0, ladder_log, 0, 0, 0); if (t) CloseHandle(t); }
    logf("Ladder: the ladder tactic is watched (%s)", log ? "calls logged" : "camera turn locked while climbing");
}

/* 08.11.2026 [UI] CompareBowWithWeapon=1: the tooltip's "equipped" item is the content of the hovered item's first
   preferred slot (0x88FEE0 thiscall(inventory, slot)). A bow's only slot is handL, so a bow was
   compared with the shield. When the hovered item is a bow and the slot holds a shield, the item of a neighbouring
   slot handR is used instead.
   0x8D6420 is bow, 0x8D68E0 is shield: thiscall(item) -> al. */
typedef void* (__fastcall *InvSlot_t)(void* inv, void* edx, int slot);
typedef BYTE (__fastcall *ItemIs_t)(void* item, void* edx);
static InvSlot_t g_invSlot;
static ItemIs_t g_isBow, g_isShield;
/* the game's own "slot name -> index" (0x894160, cdecl(std::string*), see 0x73CA9C with "handR") */
typedef void (__fastcall *CmpStrCtor_t)(void* self, void* edx, const char* s);
typedef void (__fastcall *CmpStrDtor_t)(void* self, void* edx);
typedef int (__cdecl *SlotIndex_t)(void* name);
static CmpStrCtor_t* g_cmpStrCtor; static CmpStrDtor_t* g_cmpStrDtor; static SlotIndex_t g_slotIndex;
static int slot_by_name(const char* name)
{
    BYTE s[64];
    (*g_cmpStrCtor)(s, 0, name);
    int r = g_slotIndex(s);
    (*g_cmpStrDtor)(s, 0);
    return r;
}
static void* __cdecl hk_compare_slot(void* inv, int slot, void* item)
{
    void* cur = g_invSlot(inv, 0, slot);
    if (!cur || !item || !g_isBow(item, 0) || !g_isShield(cur, 0)) return cur;
    static int handR = -2;
    if (handR == -2) { handR = slot_by_name("handR"); }
    if (handR < 0 || handR == slot) return cur;
    void* o = g_invSlot(inv, 0, handR);
    if (!o) return cur;
    if (g_isBow(o, 0) || g_isShield(o, 0)) return cur;
    static int n; if (n++ < 3) logf("UI: a bow compared with the weapon of slot %d instead of the shield of slot %d", handR, slot);
    return o;
}
/* 09.11.2026: the tooltip's item is chosen by four copies of one getter (0xAC4B70, ~0xAE8150, 0xB02810, 0xB03C20):
   "mov ecx, item; call 0x8D6E90 (slot list)" and then 0x88FEE0(inventory, first slot), by which time the item is no longer
   in a register. So the first call remembers the item and the second one is replaced. */
static void* g_cmpItem;
static void apply_compare_bow(Module m)
{
    static const DWORD sites[4][2] = { { 0xAC4C8C, 0xAC4CE5 }, { 0xAE824F, 0xAE82A0 }, { 0xB028E3, 0xB02933 }, { 0xB03CF3, 0xB03D43 } };
    BYTE* list = m.base + (0x8D6E90 - 0x400000);
    BYTE* target = m.base + (0x88FEE0 - 0x400000);
    g_invSlot = (InvSlot_t)target;
    g_cmpStrCtor = (CmpStrCtor_t*)(m.base + (0x12B758C - 0x400000));
    g_cmpStrDtor = (CmpStrDtor_t*)(m.base + (0x12B7594 - 0x400000));
    g_slotIndex = (SlotIndex_t)(m.base + (0x894160 - 0x400000));
    g_isBow = (ItemIs_t)(m.base + (0x8D6420 - 0x400000));
    g_isShield = (ItemIs_t)(m.base + (0x8D68E0 - 0x400000));
    BYTE* th = (BYTE*)VirtualAlloc(0, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!th) return;
    /* th: mov [g_cmpItem], ecx / jmp 0x8D6E90 */
    th[0] = 0x89; th[1] = 0x0D; *(void***)(th + 2) = &g_cmpItem;
    th[6] = 0xE9; *(long*)(th + 7) = (long)list - (long)(th + 11);
    /* th2: push [g_cmpItem] / push [esp+8] (slot) / push ecx (inventory) / call hk / add esp,12 / ret 4 */
    BYTE* t2 = th + 16;
    t2[0] = 0xFF; t2[1] = 0x35; *(void***)(t2 + 2) = &g_cmpItem;
    t2[6] = 0xFF; t2[7] = 0x74; t2[8] = 0x24; t2[9] = 0x08; t2[10] = 0x51;
    t2[11] = 0xE8; *(long*)(t2 + 12) = (long)hk_compare_slot - (long)(t2 + 16);
    t2[16] = 0x83; t2[17] = 0xC4; t2[18] = 0x0C; t2[19] = 0xC2; t2[20] = 0x04; t2[21] = 0x00;
    int done = 0;
    for (int i = 0; i < 4; i++) {
        BYTE* c1 = m.base + (sites[i][0] - 0x400000), *c2 = m.base + (sites[i][1] - 0x400000);
        if (!readable(c1, 5) || !readable(c2, 5) || c1[0] != 0xE8 || c2[0] != 0xE8 ||
            c1 + 5 + *(int*)(c1 + 1) != list || c2 + 5 + *(int*)(c2 + 1) != target) { logf("UI: bytes at 0x%X differ -> this tooltip compares a bow as the game does", sites[i][1]); continue; }
        BYTE call1[5], call2[5];
        call1[0] = call2[0] = 0xE8;
        *(long*)(call1 + 1) = (long)th - (long)(c1 + 5);
        *(long*)(call2 + 1) = (long)t2 - (long)(c2 + 5);
        if (write_mem(c1, call1, 5) && write_mem(c2, call2, 5)) done++;
    }
    /* the hover comparison itself: 0x88FEE0 followed by 0x9B3CD0(item+0x94, equipped+0x94); the item is still in a register */
    static const struct { DWORD at; BYTE pushReg; } hover[] = { { 0xAD3502, 0x55 }, { 0xAD35D1, 0x55 }, { 0xA52EF4, 0x56 }, { 0xADEE4B, 0x57 }, { 0xADF5EB, 0x57 } };
    int hov = 0;
    for (int i = 0; i < 5; i++) {
        BYTE* c = m.base + (hover[i].at - 0x400000);
        if (!readable(c, 5) || c[0] != 0xE8 || c + 5 + *(int*)(c + 1) != target) { logf("UI: bytes at 0x%X differ -> this tooltip compares a bow as the game does", hover[i].at); continue; }
        BYTE* h = th + 48 + i * 24;
        h[0] = hover[i].pushReg; h[1] = 0xFF; h[2] = 0x74; h[3] = 0x24; h[4] = 0x08; h[5] = 0x51;
        h[6] = 0xE8; *(long*)(h + 7) = (long)hk_compare_slot - (long)(h + 11);
        h[11] = 0x83; h[12] = 0xC4; h[13] = 0x0C; h[14] = 0xC2; h[15] = 0x04; h[16] = 0x00;
        BYTE call[5]; call[0] = 0xE8; *(long*)(call + 1) = (long)h - (long)(c + 5);
        if (write_mem(c, call, 5)) hov++;
    }
    logf("UI: bow comparison fixed at %d of 4 tooltip getters, %d of 5 hover sites", done, hov);
}

/* ================= Fix 2: 16:9 menus on ultrawide ================= */
typedef struct {
    int BufferWidth, BufferHeight, Left, Top, Width, Height;
    int ScissorLeft, ScissorTop, ScissorWidth, ScissorHeight;
    float Scale, AspectRatio; DWORD Flags;
} GViewport;

typedef void (__fastcall *SetViewport_t)(void* self, void* edx, const GViewport* vp);
typedef const char* (__fastcall *GetFileURL_t)(void* self, void* edx);
typedef void* (__fastcall *GetMovieDef_t)(void* self, void* edx);

static SetViewport_t g_origSetViewport;
static void** g_defVtbl;
static char g_movies[2048];          /* ";name1;name2;" lowercase, without .swf */
static int g_uiAll;

/* movie file name without path and extension (as in the file, not lowercased); 0 if unknown */
static int movie_name(void* root, char* out, int cap)
{
    void** vt = *(void***)root;
    void* def = ((GetMovieDef_t)vt[1])(root, 0);             /* GFxMovie::GetMovieDef -> [this+28h], no side effects */
    if (!def || !readable(def, 4) || *(void***)def != g_defVtbl) return 0;
    const char* url = ((GetFileURL_t)g_defVtbl[11])(def, 0); /* GFxMovieDefImpl::GetFileURL (verified by code bytes) */
    if (!url || !readable(url, 1)) return 0;
    int e = 0; while (url[e] && e < 512) e++;
    int b = e; while (b > 0 && url[b - 1] != '\\' && url[b - 1] != '/') b--;
    int x = e; for (int i = b; i < e; i++) if (url[i] == '.') x = i;
    int k = 0; for (int i = b; i < x && k < cap - 1; i++) out[k++] = url[i];
    out[k] = 0;
    return k > 0;
}

int div2fix_movie_name(void* root, char* out, int cap) { return movie_name(root, out, cap); }

static int movie_selected(void* root)
{
    if (g_uiAll) return 1;
    char name[72];
    if (!movie_name(root, name, sizeof name)) return 0;
    char key[80]; int k = 0; key[k++] = ';';
    for (int i = 0; name[i] && k < 76; i++) key[k++] = lc(name[i]);
    key[k++] = ';'; key[k] = 0;
    /* substring search */
    for (int i = 0; g_movies[i]; i++) {
        int j = 0; while (key[j] && g_movies[i + j] == key[j]) j++;
        if (!key[j]) return 1;
    }
    return 0;
}

static void __fastcall hk_SetViewport(void* self, void* edx, const GViewport* vp)
{
    (void)edx;
    if (gamepad_on()) { char name[72]; if (movie_name(self, name, sizeof name)) { gamepad_ui_call(name, 2); uinav_frame(self, name); } }
    if (vp && vp->Height > 0 && (long long)vp->Width * 9 > (long long)vp->Height * 16 + 9 && movie_selected(self)) {
        GViewport v = *vp;
        int nw = (v.Height * 16 + 4) / 9;
        v.Left += (v.Width - nw) / 2;
        v.Width = nw;
        if (v.ScissorWidth > 0) {
            int s0 = v.ScissorLeft, s1 = v.ScissorLeft + v.ScissorWidth;
            if (s0 < v.Left) s0 = v.Left;
            if (s1 > v.Left + v.Width) s1 = v.Left + v.Width;
            v.ScissorLeft = s0; v.ScissorWidth = s1 > s0 ? s1 - s0 : 0;
        }
        g_origSetViewport(self, 0, &v);
        return;
    }
    g_origSetViewport(self, 0, vp);
}

/* ---- mouse input correction ----
 * GFx converts mouse coordinates with a matrix that ignores Viewport.Left/Top, i.e. it
 * expects coordinates relative to the viewport origin. The game passes screen
 * coordinates (correct while every viewport started at 0,0). For movies we shifted,
 * subtract the viewport origin. For untouched movies Left/Top are 0 -> no change. */
#define VP_LEFT(self) (*(int*)((BYTE*)(self) + 0x38 + 8))   /* GFxMovieRoot::Viewport at +38h (see GetViewport) */
#define VP_TOP(self)  (*(int*)((BYTE*)(self) + 0x38 + 12))

typedef struct { DWORD Type; DWORD Mods; float x, y; float Scroll; DWORD Button; DWORD MouseIndex; } GFxMouseEvent; /* 0x1C */
typedef int  (__fastcall *HandleEvent_t)(void* self, void* edx, const void* ev);
typedef void (__fastcall *NotifyMouse_t)(void* self, void* edx, float x, float y, int buttons, int idx);
typedef int  (__fastcall *HitTest_t)(void* self, void* edx, float x, float y, int cond);
static HandleEvent_t g_origHandleEvent;
static NotifyMouse_t g_origNotifyMouse;
static HitTest_t     g_origHitTest;

static int __fastcall hk_HandleEvent(void* self, void* edx, const void* ev)
{
    (void)edx;
    const GFxMouseEvent* e = (const GFxMouseEvent*)ev;
    if (e && e->Type >= 1 && e->Type <= 4 && gamepad_on()) { char name[72]; if (movie_name(self, name, sizeof name)) gamepad_ui_call(name, 1); }
    if (e && e->Type >= 1 && e->Type <= 4 && (VP_LEFT(self) || VP_TOP(self))) {   /* MouseMove/Down/Up/Wheel */
        GFxMouseEvent c = *e;
        c.x -= (float)VP_LEFT(self);
        c.y -= (float)VP_TOP(self);
        return g_origHandleEvent(self, 0, &c);
    }
    return g_origHandleEvent(self, 0, ev);
}

static void __fastcall hk_NotifyMouse(void* self, void* edx, float x, float y, int buttons, int idx)
{
    (void)edx;
    if (gamepad_on()) { char name[72]; if (movie_name(self, name, sizeof name)) gamepad_ui_movie(name); }   /* menu detection */
    g_origNotifyMouse(self, 0, x - (float)VP_LEFT(self), y - (float)VP_TOP(self), buttons, idx);
}

static int __fastcall hk_HitTest(void* self, void* edx, float x, float y, int cond)
{
    (void)edx;
    return g_origHitTest(self, 0, x - (float)VP_LEFT(self), y - (float)VP_TOP(self), cond);
}

/* ---- UI probe ([Gamepad] ProbeUI=1, diagnostics only): which GFxMovieRoot vtable slot the game uses to call movie
 * functions (Invoke) and with what. Slots 10..19 get a stub that logs the first calls (method name / format if the
 * arguments are text) and jumps on to the original; nothing else changes. */
void* g_probeOrig[20];
static volatile DWORD g_probeN[20];
static int printable(const char* t) { if (!t || !readable(t, 2)) return 0; int n = 0; while (n < 48 && t[n]) { if (t[n] < 32 || t[n] > 126) return 0; n++; } return n > 1; }
/* GFx value (16 bytes): type at +0, value at +8 (number = double, string = char*, wide = wchar_t*, bool = byte);
   slot 18 = Invoke(method, GFxValue* result, const GFxValue* args, UINT count) - found by the first probe (06.11) */
static int probe_val(char* out, int cap, const BYTE* v)
{
    if (!readable(v, 16)) return wsprintfA(out, "?");
    DWORD t = *(const DWORD*)v; const void* pv = *(void* const*)(v + 8);
    if (t == 3) { double d = *(const double*)(v + 8); long i = (long)d; long f = (long)((d - i) * 100); if (f < 0) f = -f; return wsprintfA(out, "%ld.%02ld", i, f); }
    if (t == 2) return wsprintfA(out, "%s", v[8] ? "true" : "false");
    if (t == 4 && printable((const char*)pv)) return wsprintfA(out, "'%.40s'", (const char*)pv);
    if (t == 5 && readable(pv, 2)) { char a[48]; int n = 0; const unsigned short* w = pv; while (n < 40 && readable(w + n, 2) && w[n]) { a[n] = w[n] < 128 ? (char)w[n] : '#'; n++; } a[n] = 0; return wsprintfA(out, "L'%s'", a); }
    (void)cap; return wsprintfA(out, "t%u", t);
}
typedef struct { DWORD h; int n; } ProbeMovie;
static ProbeMovie g_pm[48]; static int g_pmN;
void __cdecl probe_log(int idx, void* self, void** args)
{
    if (idx != 18) { if (g_probeN[idx] < 2) { g_probeN[idx]++; logf("UIProbe: slot %d args %08X %08X %08X", idx, (DWORD)args[0], (DWORD)args[1], (DWORD)args[2]); } return; }
    const char* m = (const char*)args[0];
    if (!printable(m)) return;
    char name[72] = "?"; movie_name(self, name, sizeof name);
    DWORD h = 2166136261u; for (int i = 0; name[i]; i++) h = (h ^ (BYTE)name[i]) * 16777619u;
    ProbeMovie* pm = 0; for (int i = 0; i < g_pmN; i++) if (g_pm[i].h == h) pm = &g_pm[i];
    if (!pm && g_pmN < 48) { pm = &g_pm[g_pmN++]; pm->h = h; pm->n = 0; }
    if (!pm || pm->n >= 40) return;
    pm->n++;
    char line[600]; int p = wsprintfA(line, "UIProbe: [%s] %s(", name, m);
    const BYTE* av = (const BYTE*)args[2]; DWORD n = (DWORD)args[3];
    for (DWORD i = 0; av && i < n && i < 10 && p < 520; i++) { if (i) line[p++] = ','; p += probe_val(line + p, 64, av + 16 * i); }
    line[p++] = ')'; line[p] = 0;
    logf("%s", line);
}
#define PROBE_STUB(N) __attribute__((naked)) static void probe_stub_##N(void) { __asm__ volatile( \
    "pushl %ecx\n\t leal 8(%esp), %eax\n\t pushl %eax\n\t pushl %ecx\n\t pushl $" #N "\n\t call _probe_log\n\t addl $12, %esp\n\t popl %ecx\n\t jmp *(_g_probeOrig+4*" #N ")\n\t"); }
PROBE_STUB(10) PROBE_STUB(11) PROBE_STUB(12) PROBE_STUB(13) PROBE_STUB(14) PROBE_STUB(15) PROBE_STUB(16) PROBE_STUB(17) PROBE_STUB(18) PROBE_STUB(19)
static void* const k_probeStubs[10] = { probe_stub_10, probe_stub_11, probe_stub_12, probe_stub_13, probe_stub_14, probe_stub_15, probe_stub_16, probe_stub_17, probe_stub_18, probe_stub_19 };

static int code_is(void* fn, const char* hex)
{
    const BYTE* p = (const BYTE*)fn;
    for (int i = 0; hex[2 * i]; i++) {
        int hi = hex[2 * i], lo = hex[2 * i + 1];
        hi = hi <= '9' ? hi - '0' : hi - 'A' + 10; lo = lo <= '9' ? lo - '0' : lo - 'A' + 10;
        if (p[i] != (BYTE)(hi * 16 + lo)) return 0;
    }
    return 1;
}

static void hook_vslot(void** slot, void* hook, void** orig)
{
    *orig = *slot;
    write_mem(slot, &hook, 4);
}

static void apply_uiaspect(Module m)
{
    void** root = find_vtable(m, ".?AVGFxMovieRoot@@");
    void** def = find_vtable(m, ".?AVGFxMovieDefImpl@@");
    if (!root || !def) { logf("UIAspect: RTTI not found (root=%08X def=%08X) -> skipped", (DWORD)root, (DWORD)def); return; }
    /* verify SetViewScaleMode (vt[22]) = mov eax,[esp+4]; mov edx,[ecx]; mov edx,[edx+50h]; mov [ecx+80h],eax  -> vt[20] is SetViewport */
    static const BYTE svsm[] = { 0x8B,0x44,0x24,0x04, 0x8B,0x11, 0x8B,0x52,0x50, 0x89,0x81,0x80,0x00,0x00,0x00 };
    static const BYTE gmd[]  = { 0x8B,0x41,0x28,0xC3 };
    static const BYTE gurl[] = { 0x8B,0x41,0x1C, 0x8B,0x48,0x10, 0x8B,0x51,0x1C, 0x8B,0x42,0x20, 0x83,0xC0,0x0C, 0xC3 };
    int ok = 1;
    for (int i = 0; i < (int)sizeof svsm; i++) if (((BYTE*)root[22])[i] != svsm[i]) ok = 0;
    for (int i = 0; i < (int)sizeof gmd; i++)  if (((BYTE*)root[1])[i] != gmd[i]) ok = 0;
    for (int i = 0; i < (int)sizeof gurl; i++) if (((BYTE*)def[11])[i] != gurl[i]) ok = 0;
    if (!ok) { logf("UIAspect: vtable layout mismatch -> skipped"); return; }
    g_defVtbl = def;

    /* popup, popup_buttons, save, charms, credits, itemSplitter lay themselves out with Stage.visibleRect
     * (noScale + full-screen black_mc): narrowing them clips their dimming background -> not listed */
    char raw[1800];
    GetPrivateProfileStringA("UIAspect", "Movies",
        "menuOverview,tacticalPause,options,questLog,skillTree,equipment,equipmentPopup,inventory,"
        "trade,chest,book,mainmap,mainMenu,characterCreation,humanCustomizing,colorCalibration,"
        "questReward,waypointShrine,summons,battletower_main,bt_alchemistgarden,bt_chest,bt_disenchant,"
        "bt_em_selection,bt_enchant,bt_necromancer,bt_skilltrainer,tooltip,contextMenu,loadingscreen,loadingscreen_still,simpleloadingscreen",
        raw, sizeof raw, g_ini);
    int k = 0; g_movies[k++] = ';';
    for (int i = 0; raw[i] && k < (int)sizeof g_movies - 2; i++) {
        char c = raw[i];
        if (c == ',' || c == ' ' || c == ';') { if (g_movies[k - 1] != ';') g_movies[k++] = ';'; }
        else g_movies[k++] = lc(c);
    }
    if (g_movies[k - 1] != ';') g_movies[k++] = ';';
    g_movies[k] = 0;
    g_uiAll = (raw[0] == '*');

    g_origSetViewport = (SetViewport_t)root[20];
    void* hook = (void*)hk_SetViewport;
    if (write_mem(&root[20], &hook, 4))
        logf("UIAspect: SetViewport hooked (vtbl %08X, orig %08X), movies=%s", (DWORD)root, (DWORD)g_origSetViewport, g_uiAll ? "*" : g_movies);

    /* mouse correction: verify GetViewport (vt21, viewport at +38h), HandleEvent (35), NotifyMouseState (37), HitTest (38) */
    if (code_is(root[21], "56578B7C240C8D7138B90D000000") &&
        code_is(root[35], "83EC08568BF18B068B90AC000000") &&
        code_is(root[37], "558BEC83E4F883EC1453568BF1D94670") &&
        code_is(root[38], "558BEC83E4F883EC445356578BF9D94770")) {
        hook_vslot(&root[35], (void*)hk_HandleEvent, (void**)&g_origHandleEvent);
        hook_vslot(&root[37], (void*)hk_NotifyMouse, (void**)&g_origNotifyMouse);
        hook_vslot(&root[38], (void*)hk_HitTest, (void**)&g_origHitTest);
        gamepad_ui_hooked();
        logf("UIAspect: mouse input correction hooked (HandleEvent/NotifyMouseState/HitTest)");
        if (GetPrivateProfileIntA("Gamepad", "ProbeUI", 0, g_ini)) {
            for (int k = 10; k < 20; k++) { g_probeOrig[k] = root[k]; void* h = k_probeStubs[k - 10]; write_mem(&root[k], &h, 4); }
            logf("UIProbe: GFxMovieRoot slots 10..19 logged (diagnostics)");
        }
        uinav_install(root, GetPrivateProfileIntA("Gamepad", "ProbeUI", 0, g_ini) ? g_probeOrig[18] : 0);
    } else {
        logf("UIAspect: mouse functions not recognised -> mouse correction skipped (menus may need offset clicks)");
    }
}

/* ================= gamepad prompt icons =================
 * 0x8E84A0 (key -> display name, cdecl(out, key)) and 0x9F1400 (mouse button -> name, cdecl(out, button)) build the
 * wide-string objects shown in prompts. Callers: 0x8EC610 (binding name; tutorial/hint text, and the controls
 * options screen through 0x797987) and the HUD action prompt 0xAE05B0. While the gamepad is in use, we build the
 * object ourselves with the pad button glyph through the game's own constructor 0x8E4FD0(text, &mode, assign-as-is)
 * - the same call 0x8EC610 uses for an unknown binding. The controls options screen keeps the key names. */
typedef void* (__cdecl *NameFn_t)(void* out, int code);
typedef void* (__fastcall *StrCtor_t)(void* self, void* edx, const unsigned short* text, int* mode, int raw);
static NameFn_t g_origKeyName, g_origMouseName;
static StrCtor_t g_strCtor;
static DWORD g_base;
int gamepad_glyph_text(WORD action, unsigned short* buf, int cap);
static int memcmp_(const BYTE* a, const BYTE* b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 1; return 0; }
static int from_options(void** args)
{
    DWORD ret = (DWORD)args[-1] - g_base, outer = (DWORD)args[4] - g_base;
    return (ret == 0x8EC64F || ret == 0x8EC663) && outer == 0x79798C;   /* static addresses (load delta removed) */
}
static void* __cdecl hk_KeyName(void* out, int key)
{
    unsigned short t[8];
    if (key >= 0 && key < 256 && !from_options(&out) && gamepad_glyph_text((WORD)key, t, 8)) { int mode = 0; return g_strCtor(out, 0, t, &mode, 0); }
    return g_origKeyName(out, key);
}
static void* __cdecl hk_MouseName(void* out, int btn)
{
    unsigned short t[8];
    if (btn >= 0 && btn < 3 && !from_options(&out) && gamepad_glyph_text((WORD)(0x100 + btn), t, 8)) { int mode = 0; return g_strCtor(out, 0, t, &mode, 0); }
    return g_origMouseName(out, btn);
}
static void apply_glyphs(Module m)
{
    g_base = (DWORD)m.base - 0x400000;
    BYTE* kn = m.base + (0x8E84A0 - 0x400000); BYTE* mn = m.base + (0x9F1400 - 0x400000); BYTE* ct = m.base + (0x8E4FD0 - 0x400000);
    static const BYTE k1[] = { 0x8B,0x44,0x24,0x08, 0x81,0xEC,0x84,0x01,0x00,0x00 }, k2[] = { 0x8B,0x44,0x24,0x08, 0x83,0xEC,0x30, 0x56 },
                      k3[] = { 0x56, 0x57, 0x8B,0xF9, 0x6A,0x04, 0xC7,0x07,0x00,0x00,0x00,0x00 };
    if (!readable(kn, 10) || memcmp_(kn, k1, 10) || memcmp_(mn, k2, 8) || memcmp_(ct, k3, 12)) { logf("GamepadGlyphs: code bytes differ -> prompts unchanged"); return; }
    g_strCtor = (StrCtor_t)ct;
    g_origKeyName = (NameFn_t)detour(kn, (void*)hk_KeyName);
    g_origMouseName = (NameFn_t)detour(mn, (void*)hk_MouseName);
    logf("GamepadGlyphs: key prompts %s", g_origKeyName && g_origMouseName ? "hooked (pad button icons while the pad is used)" : "NOT hooked");
}

/* ================= dinput8 proxy ================= */
static HMODULE g_real;
static FARPROC real_proc(const char* name)
{
    if (!g_real) {
        char p[MAX_PATH]; UINT n = GetSystemDirectoryA(p, MAX_PATH);
        wsprintfA(p + n, "\\dinput8.dll");
        g_real = LoadLibraryA(p);
    }
    return g_real ? GetProcAddress(g_real, name) : 0;
}

HRESULT WINAPI Proxy_DirectInput8Create(void* hinst, DWORD ver, const void* riid, void** out, void* outer)
{
    typedef HRESULT (WINAPI *F)(void*, DWORD, const void*, void**, void*);
    F f = (F)real_proc("DirectInput8Create");
    HRESULT hr = f ? f(hinst, ver, riid, out, outer) : (HRESULT)0x80004005;
    if (hr >= 0 && out && *out) { input_on_dinput(*out); diag_on_dinput(*out); }
    return hr;
}
HRESULT WINAPI Proxy_DllCanUnloadNow(void) { typedef HRESULT (WINAPI *F)(void); F f = (F)real_proc("DllCanUnloadNow"); return f ? f() : 1; }
HRESULT WINAPI Proxy_DllGetClassObject(const void* a, const void* b, void** c) { typedef HRESULT (WINAPI *F)(const void*, const void*, void**); F f = (F)real_proc("DllGetClassObject"); return f ? f(a, b, c) : (HRESULT)0x80040111; }
HRESULT WINAPI Proxy_DllRegisterServer(void) { typedef HRESULT (WINAPI *F)(void); F f = (F)real_proc("DllRegisterServer"); return f ? f() : (HRESULT)0x80004005; }
HRESULT WINAPI Proxy_DllUnregisterServer(void) { typedef HRESULT (WINAPI *F)(void); F f = (F)real_proc("DllUnregisterServer"); return f ? f() : (HRESULT)0x80004005; }
const void* WINAPI Proxy_GetdfDIJoystick(void) { typedef const void* (WINAPI *F)(void); F f = (F)real_proc("GetdfDIJoystick"); return f ? f() : 0; }

BOOL WINAPI DllMain(HMODULE self, DWORD reason, void* reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        init_paths(self);
        g_logOn = GetPrivateProfileIntA("General", "Log", 1, g_ini);
        if (g_logOn) { HANDLE h = CreateFileA(g_log, GENERIC_WRITE, 1, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
        logf("Div2Fix 2.0 (Divinity II Reborn) loaded into %s", exe);
        Module m = main_module();
        version_check(m);
        if (GetPrivateProfileIntA("Gamepad", "Enable", 1, g_ini) && GetPrivateProfileIntA("Gamepad", "Glyphs", 1, g_ini)) apply_glyphs(m);
        gamepad_init(m);        /* before UIAspect (menu detection hook) and before the game creates DirectInput */
        if (GetPrivateProfileIntA("KeyNames", "Enable", 1, g_ini)) apply_keynames(m); else logf("KeyNames: disabled in ini");
        if (GetPrivateProfileIntA("Saves", "Unlimited", 1, g_ini)) apply_unlimited_saves(m); else logf("Saves: limit kept (ini)");
        if (GetPrivateProfileIntA("SkillBoosters", "Everywhere", 1, g_ini)) apply_boosters_everywhere(m);
        if (GetPrivateProfileIntA("UI", "CompareBowWithWeapon", 1, g_ini)) apply_compare_bow(m);
        { int lt = GetPrivateProfileIntA("Diagnostics", "LadderTrace", 0, g_ini), ll = GetPrivateProfileIntA("Fixes", "LadderCameraLock", 1, g_ini);
          if (lt || ll) apply_ladder_trace(m, lt); g_ladderLock = ll; }
        if (GetPrivateProfileIntA("UIAspect", "Enable", 1, g_ini)) apply_uiaspect(m); else logf("UIAspect: disabled in ini");
        gfx_init(m);
        mindread_init(m);
        camera_init(m);
        icons_init(m);
        intro_init(m);
        overlay_init(m);
        mageskills_init(m);     /* before skilltrace: it compares the bytes of 0x7692D0, which skilltrace detours */
        skilltrace_init(m);
        uitext_init(m);
        window_init(m, self);
        diag_init(m);
        texmgr_init(m);
        parcopy_init(m);
        crashguard_init(m);
    } else if (reason == DLL_PROCESS_DETACH) {
        crashguard_exit();
    }
    return 1;
}
