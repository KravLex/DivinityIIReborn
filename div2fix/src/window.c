/* Div2Fix - game window icon + keyboard layout switching.
 *
 * Icon: the game registers its window class with LoadIconA(0, IDI_APPLICATION) (0xB416F6..0xB41733), so the
 * taskbar shows the blank default icon; the exe has no icon resource. We redirect the exe's import of
 * RegisterClassA (IAT slot 0x12B7A0C) and put our icon (resource 1 of dinput8.dll: the game's own Steam icon,
 * the same as on the desktop shortcut) into the class before it is registered.
 *
 * Keyboard layout: in game the layout hotkey did not switch the language . The DirectInput
 * keyboard is made non-exclusive without DISCL_NOWINKEY (Win+Space), and a helper thread watches
 * Alt+Shift / Ctrl+Shift / Win+Space: if Windows did not switch the layout, it asks the game window to.
 * INI: [Window] Icon=1, KeyboardShared=1.
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
typedef struct { UINT style; void* proc; int cls, wnd; HMODULE inst; HANDLE icon, cursor, brush; const char* menu; const char* name; } WNDCLASSA_;
typedef unsigned short (WINAPI *RegisterClassA_t)(const WNDCLASSA_*);
static RegisterClassA_t g_origRC;
static HANDLE g_icon;

static unsigned short WINAPI hk_RegisterClassA(const WNDCLASSA_* wc)
{
    if (wc && g_icon) {
        WNDCLASSA_ c = *wc; c.icon = g_icon;
        logf("Window: icon set for window class '%.40s'", c.name && ((DWORD)c.name >> 16) ? c.name : "?");
        return g_origRC(&c);
    }
    return g_origRC(wc);
}

void window_init(Module m, HMODULE self)
{
    if (!GetPrivateProfileIntA("Window", "Icon", 1, g_ini)) return;
    g_icon = LoadImageA(self, (const char*)1, 1 /*IMAGE_ICON*/, 0, 0, 0x40 /*LR_DEFAULTSIZE*/);
    if (!g_icon) { logf("Window: icon resource not found"); return; }
    void** slot = (void**)VA(m, 0x12B7A0C);
    void* rc = GetProcAddress(GetModuleHandleA("user32.dll"), "RegisterClassA");
    if (!readable(slot, 4) || *slot != rc) { logf("Window: RegisterClassA import not found -> icon unchanged"); return; }
    g_origRC = (RegisterClassA_t)*slot;
    void* h = (void*)hk_RegisterClassA;
    if (write_mem(slot, &h, 4)) logf("Window: game window icon enabled");
}

/* ---- DirectInput keyboard: non-exclusive ---- */
typedef long (WINAPI *DICreateDevice_t)(void*, const void*, void**, void*);
typedef long (WINAPI *SetCoop_t)(void*, void*, DWORD);
static DICreateDevice_t g_origCreate;
static SetCoop_t g_origCoop;
static void* g_kbd[4];
static int g_shared = 1;

static long WINAPI hk_SetCoop(void* dev, void* hwnd, DWORD flags)
{
    int kb = 0; for (int i = 0; i < 4; i++) if (g_kbd[i] == dev) kb = 1;
    if (kb && g_shared) {                                       /* DISCL_EXCLUSIVE -> NONEXCLUSIVE, drop DISCL_NOWINKEY */
        DWORD nf = (flags & 1) ? ((flags & ~(DWORD)(1 | 0x10)) | 2) : (flags & ~(DWORD)0x10);
        long hr = g_origCoop(dev, hwnd, nf);
        logf("Input: keyboard cooperative level %08X -> %08X (hr %08X)", flags, nf, (DWORD)hr);
        if (hr >= 0) return hr;
        return g_origCoop(dev, hwnd, flags);
    }
    return g_origCoop(dev, hwnd, flags);
}

/* layout hotkeys: Alt+Shift, Ctrl+Shift, Win+Space. If Windows itself did not switch the game thread's
   layout within ~200 ms after the combination is released, ask the game window to switch
   (WM_INPUTLANGCHANGEREQUEST, HKL_NEXT -> the game's window procedure passes it to DefWindowProc). */
static int down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static DWORD WINAPI layout_thread(void* p)
{
    (void)p;
    DWORD pid = GetCurrentProcessId(); int armed = 0, n = 0; HKL before = 0; void* hw = 0; DWORD tid = 0;
    for (;;) {
        Sleep(15);
        void* fg = GetForegroundWindow(); DWORD wp = 0; DWORD t = fg ? GetWindowThreadProcessId(fg, &wp) : 0;
        if (!fg || wp != pid) { armed = 0; continue; }
        int sh = down(0x10), alt = down(0x12), ctl = down(0x11), win = down(0x5B) || down(0x5C), sp = down(0x20);
        int combo = (sh && (alt || ctl)) || (win && sp);
        if (combo && !armed) { armed = 1; hw = fg; tid = t; before = GetKeyboardLayout(t); }
        if (armed && !sh && !alt && !ctl && !win && !sp) {
            armed = 0; Sleep(200);
            if (GetKeyboardLayout(tid) == before) {
                PostMessageA(hw, 0x50 /*WM_INPUTLANGCHANGEREQUEST*/, 0, 1 /*HKL_NEXT*/);
                if (n++ < 5) logf("Input: layout hotkey -> switch requested (Windows did not switch)");
            }
        }
    }
    return 0;
}

/* ---- strafe left + right held together = no strafe (Div2Patch issue #2) ----
   Default bindings (0x82E7FD..0x82E846): strafe left = A / Left arrow, strafe right = D / Right arrow.
   The game picks "left" when both are held. At the DirectInput keyboard we hide BOTH keys of a pair while
   both are physically held; releasing one gives the other back. Works for GetDeviceState (immediate) and
   GetDeviceData (buffered: the pair keys' events are replaced by synthesized transitions).
   Only the default keys are handled; rebound strafe keys behave as in the original game. */
typedef long (WINAPI *GDS_t)(void*, DWORD, void*);
typedef long (WINAPI *GDD_t)(void*, DWORD, void*, DWORD*, DWORD);
static GDS_t g_oGDS; static GDD_t g_oGDD; static int g_strafe = 1, g_gdsN, g_gddN;
static const BYTE k_vk[2][2] = { { 0x41, 0x44 }, { 0x25, 0x27 } };   /* VK_A, VK_D, VK_LEFT, VK_RIGHT */
static const BYTE k_pairs[2][2] = { { 0x1E, 0x20 } /* A, D */, { 0xCB, 0xCD } /* Left, Right */ };
static BYTE g_phys[256], g_view[256];
static int is_kbd(void* dev) { for (int i = 0; i < 4; i++) if (g_kbd[i] == dev) return 1; return 0; }
static int pair_key(DWORD k) { for (int p = 0; p < 2; p++) if (k == k_pairs[p][0] || k == k_pairs[p][1]) return 1; return 0; }
static BYTE wanted(DWORD k)
{
    for (int p = 0; p < 2; p++) {
        if (k == k_pairs[p][0]) return g_phys[k] && !g_phys[k_pairs[p][1]];
        if (k == k_pairs[p][1]) return g_phys[k] && !g_phys[k_pairs[p][0]];
    }
    return g_phys[k];
}
static long WINAPI hk_GDS(void* dev, DWORD cb, void* data)
{
    long hr = g_oGDS(dev, cb, data);
    if (hr >= 0 && g_strafe && cb == 256 && data && is_kbd(dev)) {
        BYTE* s = (BYTE*)data;
        if (!g_gdsN++) logf("Input: keyboard read with GetDeviceState (strafe pair fix active)");
        for (int p = 0; p < 2; p++)
            if ((s[k_pairs[p][0]] & 0x80) && (s[k_pairs[p][1]] & 0x80)) { s[k_pairs[p][0]] = 0; s[k_pairs[p][1]] = 0; }
    }
    return hr;
}
static long WINAPI hk_GDD(void* dev, DWORD cbObj, void* rg, DWORD* inout, DWORD flags)
{
    DWORD cap = inout ? *inout : 0;
    long hr = g_oGDD(dev, cbObj, rg, inout, flags);
    if (hr < 0 || !g_strafe || !is_kbd(dev) || !rg || !inout || (flags & 1 /*DIGDD_PEEK*/) || cbObj < 16) return hr;
    if (!g_gddN++) logf("Input: keyboard read with GetDeviceData (strafe pair fix active)");
    for (int p = 0; p < 2; p++) for (int j = 0; j < 2; j++)      /* a release missed while unfocused */
        if (g_phys[k_pairs[p][j]] && !(GetAsyncKeyState(k_vk[p][j]) & 0x8000)) g_phys[k_pairs[p][j]] = 0;
    BYTE* b = (BYTE*)rg; DWORD n = *inout, w = 0, ts = 0, seq = 0;
    for (DWORD i = 0; i < n; i++) {                    /* keep non-pair events, track physical state of pair keys */
        DWORD* e = (DWORD*)(b + i * cbObj);
        ts = e[2]; seq = e[3];
        if (pair_key(e[0])) { g_phys[e[0]] = (e[1] & 0x80) ? 1 : 0; continue; }
        if (w != i) { for (DWORD q = 0; q < cbObj; q++) b[w * cbObj + q] = b[i * cbObj + q]; }
        w++;
    }
    for (int p = 0; p < 2; p++)                        /* emit the transitions the game should see */
        for (int j = 0; j < 2; j++) {
            DWORD k = k_pairs[p][j]; BYTE want = wanted(k);
            if (want != g_view[k] && w < cap) {
                DWORD* e = (DWORD*)(b + w * cbObj);
                for (DWORD q = 0; q < cbObj; q++) ((BYTE*)e)[q] = 0;
                e[0] = k; e[1] = want ? 0x80 : 0; e[2] = ts; e[3] = seq;
                g_view[k] = want; w++;
            }
        }
    *inout = w;
    return hr;
}

static long WINAPI hk_Create(void* di, const void* g, void** out, void* outer)
{
    long hr = g_origCreate(di, g, out, outer);
    static const BYTE SysKeyboard[16] = { 0x61,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
    if (hr >= 0 && out && *out && g && readable(g, 16)) {
        const BYTE* a = (const BYTE*)g; int eq = 1; for (int i = 0; i < 16; i++) if (a[i] != SysKeyboard[i]) eq = 0;
        if (eq) {
            for (int i = 0; i < 4; i++) if (!g_kbd[i]) { g_kbd[i] = *out; break; }
            /* All DirectInput devices share one vtable, and diag.c hooks the same GetDeviceState/GetDeviceData slots for
               the mouse. Hook each slot once only: re-hooking a slot that now holds another hook (ours or not) would
               save that hook as "original" and close a call loop (crash on the second keyboard/mouse device). */
            void** vt = *(void***)*out;
            if (!g_origCoop) { g_origCoop = (SetCoop_t)vt[13]; void* h = (void*)hk_SetCoop; write_mem(&vt[13], &h, 4); }
            if (g_strafe && !g_oGDS) { g_oGDS = (GDS_t)vt[9]; void* h = (void*)hk_GDS; write_mem(&vt[9], &h, 4); }
            if (g_strafe && !g_oGDD) { g_oGDD = (GDD_t)vt[10]; void* h = (void*)hk_GDD; write_mem(&vt[10], &h, 4); }
            logf("Input: keyboard device %08X", (DWORD)*out);
        }
    }
    return hr;
}

void input_on_dinput(void* di)
{
    g_strafe = GetPrivateProfileIntA("Input", "StrafeBothCancel", 1, g_ini);
    g_shared = GetPrivateProfileIntA("Window", "KeyboardShared", 1, g_ini);
    if (!di || (!g_shared && !g_strafe)) return;
    void** vt = *(void***)di;
    if (g_origCreate) return;           /* once only: diag.c chains its own CreateDevice hook on top (see hk_Create) */
    g_origCreate = (DICreateDevice_t)vt[3];
    void* h = (void*)hk_Create; write_mem(&vt[3], &h, 4);
    static int started; if (!started && g_shared) { started = 1; HANDLE th = CreateThread(0, 0, layout_thread, 0, 0, 0); if (th) CloseHandle(th); logf("Input: layout hotkey helper active (Alt+Shift, Ctrl+Shift, Win+Space)"); }
}
