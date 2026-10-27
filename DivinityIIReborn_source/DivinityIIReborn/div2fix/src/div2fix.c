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

static int movie_selected(void* root)
{
    if (g_uiAll) return 1;
    void** vt = *(void***)root;
    void* def = ((GetMovieDef_t)vt[1])(root, 0);             /* GFxMovie::GetMovieDef -> [this+28h], no side effects */
    if (!def || !readable(def, 4) || *(void***)def != g_defVtbl) return 0;
    const char* url = ((GetFileURL_t)g_defVtbl[11])(def, 0); /* GFxMovieDefImpl::GetFileURL (verified by code bytes) */
    if (!url || !readable(url, 1)) return 0;
    /* basename without extension, lowercase */
    int e = 0; while (url[e] && e < 512) e++;
    int b = e; while (b > 0 && url[b - 1] != '\\' && url[b - 1] != '/') b--;
    int x = e; for (int i = b; i < e; i++) if (url[i] == '.') x = i;
    char key[80]; int k = 0; key[k++] = ';';
    for (int i = b; i < x && k < 76; i++) key[k++] = lc(url[i]);
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
    g_origNotifyMouse(self, 0, x - (float)VP_LEFT(self), y - (float)VP_TOP(self), buttons, idx);
}

static int __fastcall hk_HitTest(void* self, void* edx, float x, float y, int cond)
{
    (void)edx;
    return g_origHitTest(self, 0, x - (float)VP_LEFT(self), y - (float)VP_TOP(self), cond);
}

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
        logf("UIAspect: mouse input correction hooked (HandleEvent/NotifyMouseState/HitTest)");
    } else {
        logf("UIAspect: mouse functions not recognised -> mouse correction skipped (menus may need offset clicks)");
    }
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
        logf("Div2Fix 1.3 (Divinity II Reborn) loaded into %s", exe);
        Module m = main_module();
        version_check(m);
        if (GetPrivateProfileIntA("KeyNames", "Enable", 1, g_ini)) apply_keynames(m); else logf("KeyNames: disabled in ini");
        if (GetPrivateProfileIntA("UIAspect", "Enable", 1, g_ini)) apply_uiaspect(m); else logf("UIAspect: disabled in ini");
        gfx_init(m);
        mindread_init(m);
        camera_init(m);
        icons_init(m);
        intro_init(m);
        uitext_init(m);
        window_init(m, self);
        diag_init(m);
    }
    return 1;
}
