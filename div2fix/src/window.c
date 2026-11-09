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
   Only the default keys are handled; rebound strafe keys behave as in the original game.

   ---- gamepad (gamepad.c) ----
   Keys the pad can press are merged the same way: for those keys "held" = keyboard OR pad, and the game's own
   events for them are replaced by synthesized transitions. The DirectInput mouse (SysMouse) gets the right
   stick as X/Y deltas, the wheel as Z, and the pad's mouse buttons OR-ed into the real ones. */
typedef long (WINAPI *GDS_t)(void*, DWORD, void*);
typedef long (WINAPI *GDD_t)(void*, DWORD, void*, DWORD*, DWORD);
__declspec(dllimport) UINT WINAPI MapVirtualKeyA(UINT code, UINT type);
extern volatile BYTE g_padKey[256], g_padBtn[8];
int gamepad_injects_mouse(void);
extern BYTE g_padMapped[256];
static GDS_t g_oGDS; static GDD_t g_oGDD; static int g_strafe = 1, g_pad, g_padMouse, g_gdsN, g_gddN, g_mgdsN, g_mgddN;
static const BYTE k_pairs[2][2] = { { 0x1E, 0x20 } /* A, D */, { 0xCB, 0xCD } /* Left, Right */ };
static BYTE g_phys[256], g_view[256];          /* keyboard: physical state from the game's events / state the game was given */
static void* g_mouse[4];
static BYTE g_mphys[8], g_mview[8];            /* mouse buttons, the same */
static int is_mouse(void* dev);
static void* g_wheelMouse[4];
static int wheel_mouse(void* dev) { for (int i = 0; i < 4; i++) if (g_wheelMouse[i] == dev) return 1; return 0; }
int uinav_journal(void);
void uinav_scroll(int steps);
int ladder_active(void);
extern int g_ladderLock;
/* offsets of the mouse objects in the game's data format (SetDataFormat); defaults = c_dfDIMouse2 */
static DWORD g_mx = 0, g_my = 4, g_mz = 8, g_mb[8] = { 12, 13, 14, 15, 16, 17, 18, 19 }, g_mbN = 8, g_mAbs;
static int mouse_button(DWORD ofs) { for (DWORD i = 0; i < g_mbN; i++) if (g_mb[i] == ofs) return (int)i; return -1; }
typedef long (WINAPI *SetFmt_t)(void*, const void*);
typedef long (WINAPI *SetProp_t)(void*, const void*, const void*);
static SetFmt_t g_oSetFmt; static SetProp_t g_oSetProp;
static long WINAPI hk_SetFmt(void* dev, const void* fmt)
{
    long hr = g_oSetFmt(dev, fmt);
    if (hr >= 0 && fmt && g_padMouse && is_mouse(dev) && readable(fmt, 24)) {
        /* DIDATAFORMAT { dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf }, DIOBJECTDATAFORMAT { pguid, dwOfs, dwType, dwFlags } */
        const DWORD* f = (const DWORD*)fmt; const DWORD* o = (const DWORD*)f[5]; DWORD n = f[4];
        DWORD ax = 0, bt = 0, x = 0xFFFFFFFF, y = 0xFFFFFFFF, z = 0xFFFFFFFF, b[8];
        if (o && n < 64 && readable(o, n * 16)) {
            for (DWORD i = 0; i < n; i++, o += 4) {
                const DWORD* g = (const DWORD*)o[0]; DWORD id = g && readable(g, 4) ? g[0] : 0, type = o[2];
                if (id == 0xA36D02E0 || (!g && (type & 3) && ax == 0)) x = o[1];
                else if (id == 0xA36D02E1 || (!g && (type & 3) && ax == 1)) y = o[1];
                else if (id == 0xA36D02E2 || (!g && (type & 3) && ax == 2)) z = o[1];
                else if ((id == 0xA36D02F0 || (!g && (type & 0xC))) && bt < 8) b[bt++] = o[1];
                if (type & 3) ax++;
            }
            if (x != 0xFFFFFFFF) g_mx = x; if (y != 0xFFFFFFFF) g_my = y; if (z != 0xFFFFFFFF) g_mz = z;
            if (bt) { for (DWORD i = 0; i < bt; i++) g_mb[i] = b[i]; g_mbN = bt; }
        }
        g_mAbs = (f[2] & 1) != 0;   /* DIDF_ABSAXIS */
        logf("Input: mouse data format: %u bytes, %u objects, %s axes; X at %u, Y at %u, Z at %u, %u buttons from %u",
             f[3], n, g_mAbs ? "ABSOLUTE" : "relative", g_mx, g_my, g_mz, g_mbN, g_mb[0]);
    }
    return hr;
}
static long WINAPI hk_SetProp(void* dev, const void* prop, const void* hdr)
{
    long hr = g_oSetProp(dev, prop, hdr);
    if (g_padMouse && is_mouse(dev) && (DWORD)prop < 0x100 && hdr && readable(hdr, 20)) {
        const DWORD* h = (const DWORD*)hdr;
        if ((DWORD)prop == 2) g_mAbs = h[4] == 1;           /* DIPROP_AXISMODE: DIPROPAXISMODE_ABS = 1 */
        logf("Input: mouse property %u = %u (hr %08X)%s", (DWORD)prop, h[4], (DWORD)hr, (DWORD)prop == 2 ? (h[4] == 1 ? " -> absolute axes" : " -> relative axes") : "");
    }
    return hr;
}
static int is_kbd(void* dev) { for (int i = 0; i < 4; i++) if (g_kbd[i] == dev) return 1; return 0; }
static int is_mouse(void* dev) { for (int i = 0; i < 4; i++) if (g_mouse[i] == dev) return 1; return 0; }
static int pair_key(DWORD k) { for (int p = 0; p < 2; p++) if (k == k_pairs[p][0] || k == k_pairs[p][1]) return 1; return 0; }
static int filtered(DWORD k) { return k < 256 && ((g_strafe && pair_key(k)) || (g_pad && g_padMapped[k])); }
static BYTE held(DWORD k) { return g_phys[k] || (g_pad && g_padKey[k]); }
static BYTE wanted(DWORD k)
{
    if (g_strafe)
        for (int p = 0; p < 2; p++) {
            if (k == k_pairs[p][0]) return held(k) && !held(k_pairs[p][1]);
            if (k == k_pairs[p][1]) return held(k) && !held(k_pairs[p][0]);
        }
    return held(k);
}
static long WINAPI hk_GDS(void* dev, DWORD cb, void* data)
{
    long hr = g_oGDS(dev, cb, data);
    if (hr < 0 || !data) return hr;
    if (cb == 256 && is_kbd(dev)) {
        BYTE* s = (BYTE*)data;
        if (!g_gdsN++) logf("Input: keyboard read with GetDeviceState (strafe pair fix %s, gamepad %s)", g_strafe ? "on" : "off", g_pad ? "on" : "off");
        if (g_pad) for (int k = 0; k < 256; k++) if (g_padKey[k]) s[k] |= 0x80;
        if (g_strafe)
            for (int p = 0; p < 2; p++)
                if ((s[k_pairs[p][0]] & 0x80) && (s[k_pairs[p][1]] & 0x80)) { s[k_pairs[p][0]] = 0; s[k_pairs[p][1]] = 0; }
    } else if (!g_padMouse && cb >= 16 && wheel_mouse(dev)) {   /* 08.11: GetDeviceState of the mouse: lZ at 8 */
        static int dbg2; long z = ((long*)data)[2];
        if (z && dbg2 < 6) { dbg2++; logf("Input: mouse state wheel %d (journal %d)", z, uinav_journal()); }
        static long acc2;
        if (z && uinav_journal()) {
            acc2 += z; int steps = 0;
            while (acc2 >= 120) { steps--; acc2 -= 120; }
            while (acc2 <= -120) { steps++; acc2 += 120; }
            if (steps) uinav_scroll(steps);
        }
    } else if (g_padMouse && cb >= 16 && is_mouse(dev)) {               /* DIMOUSESTATE / DIMOUSESTATE2 */
        long* s = (long*)data; long dx, dy, dz;
        if (!g_mgdsN++) logf("Input: mouse read with GetDeviceState (%u bytes, gamepad camera)", cb);
        gamepad_take_motion(&dx, &dy, &dz);
        BYTE* p = (BYTE*)data;
        if (g_mx + 4 <= cb) *(long*)(p + g_mx) += dx;
        if (g_my + 4 <= cb) *(long*)(p + g_my) += dy;
        if (g_mz + 4 <= cb) *(long*)(p + g_mz) += dz;
        for (DWORD i = 0; i < g_mbN; i++) if (g_padBtn[i] && g_mb[i] < cb) p[g_mb[i]] |= 0x80;
        (void)s;
    }
    return hr;
}
static void put_event(BYTE* b, DWORD cbObj, DWORD at, DWORD ofs, DWORD data, DWORD ts, DWORD seq)
{
    DWORD* e = (DWORD*)(b + at * cbObj);
    for (DWORD q = 0; q < cbObj; q++) ((BYTE*)e)[q] = 0;
    e[0] = ofs; e[1] = data; e[2] = ts; e[3] = seq;
}
static long WINAPI hk_GDD(void* dev, DWORD cbObj, void* rg, DWORD* inout, DWORD flags)
{
    DWORD cap = inout ? *inout : 0;
    long hr = g_oGDD(dev, cbObj, rg, inout, flags);
    if (hr < 0 || !rg || !inout || (flags & 1 /*DIGDD_PEEK*/) || cbObj < 16) return hr;
    int kbd = is_kbd(dev), mouse = !kbd && g_padMouse && is_mouse(dev);
    if (!kbd && !mouse && wheel_mouse(dev) && *inout && g_ladderLock && ladder_active()) {
        /* 08.11: on a ladder the camera's turn turned the hero off it (a bug of the game): no horizontal motion */
        for (DWORD i = 0; i < *inout; i++) {
            DWORD* e = (DWORD*)((BYTE*)rg + i * cbObj);
            if (e[0] == g_mx) e[1] = 0;
        }
    }
    if (!kbd && !mouse && wheel_mouse(dev) && *inout && uinav_journal()) {
        /* 08.11: the journal (questLog.gfx) ignores the wheel (its onMouseWheel checks fail); scroll it as the pad does.
           Z of c_dfDIMouse/2 is at offset 8 (the game's format; g_mz once SetDataFormat was seen) */
        static long acc;
        for (DWORD i = 0; i < *inout; i++) {
            DWORD* e = (DWORD*)((BYTE*)rg + i * cbObj);
            if (e[0] == g_mz) acc += (long)e[1];
        }
        int steps = 0;
        while (acc >= 120) { steps--; acc -= 120; }       /* wheel up: scroll up */
        while (acc <= -120) { steps++; acc += 120; }
        if (steps) { uinav_scroll(steps); static int logged; if (logged++ < 5) logf("Input: journal wheel %d", steps); }
    }
    if (kbd && !g_strafe && !g_pad) return hr;
    if (!kbd && !mouse) return hr;
    BYTE* b = (BYTE*)rg; DWORD n = *inout, w = 0, ts = GetTickCount(), seq = 0;
    if (kbd) {
        if (!g_gddN++) logf("Input: keyboard read with GetDeviceData (strafe pair fix %s, gamepad %s)", g_strafe ? "on" : "off", g_pad ? "on" : "off");
        for (DWORD k = 1; k < 256; k++)                         /* a release missed while unfocused */
            if (g_phys[k] && filtered(k)) {
                UINT vk = MapVirtualKeyA(k >= 0x80 ? (0xE000 | (k & 0x7F)) : k, 3 /*MAPVK_VSC_TO_VK_EX*/);
                if (vk && !(GetAsyncKeyState((int)vk) & 0x8000)) g_phys[k] = 0;
            }
        for (DWORD i = 0; i < n; i++) {                         /* keep other events, track physical state of ours */
            DWORD* e = (DWORD*)(b + i * cbObj);
            ts = e[2]; seq = e[3];
            if (filtered(e[0])) { g_phys[e[0]] = (e[1] & 0x80) ? 1 : 0; continue; }
            if (w != i) { for (DWORD q = 0; q < cbObj; q++) b[w * cbObj + q] = b[i * cbObj + q]; }
            w++;
        }
        for (DWORD k = 1; k < 256 && w < cap; k++)              /* emit the transitions the game should see */
            if (filtered(k)) {
                BYTE want = wanted(k);
                if (want != g_view[k]) { put_event(b, cbObj, w++, k, want ? 0x80 : 0, ts, seq); g_view[k] = want; }
            }
    } else {
        if (!g_mgddN++) logf("Input: mouse read with GetDeviceData (gamepad camera), room for %u events", cap);
        static const int k_mvk[5] = { 1, 2, 4, 5, 6 };          /* VK_LBUTTON, RBUTTON, MBUTTON, XBUTTON1, XBUTTON2 */
        for (int i = 0; i < 5; i++) if (g_mphys[i] && !(GetAsyncKeyState(k_mvk[i]) & 0x8000)) g_mphys[i] = 0;
        for (DWORD i = 0; i < n; i++) {
            DWORD* e = (DWORD*)(b + i * cbObj);
            ts = e[2]; seq = e[3];
            int mb = mouse_button(e[0]);
            if (mb >= 0) { g_mphys[mb] = (e[1] & 0x80) ? 1 : 0; continue; }
            if (w != i) { for (DWORD q = 0; q < cbObj; q++) b[w * cbObj + q] = b[i * cbObj + q]; }
            w++;
        }
        for (DWORD i = 0; i < 8 && w < cap; i++) {
            BYTE want = g_mphys[i] || g_padBtn[i];
            if (i < g_mbN && want != g_mview[i]) { put_event(b, cbObj, w++, g_mb[i], want ? 0x80 : 0, ts, seq); g_mview[i] = want; }
        }
        long dx, dy, dz; gamepad_take_motion(&dx, &dy, &dz);
        if (dx || dy || dz) {
            /* add to motion events the device already returned, append the rest if there is room, keep what does not fit */
            for (DWORD i = 0; i < w; i++) {
                DWORD* e = (DWORD*)(b + i * cbObj);
                if (dx && e[0] == g_mx) { e[1] += (DWORD)dx; dx = 0; }
                else if (dy && e[0] == g_my) { e[1] += (DWORD)dy; dy = 0; }
                else if (dz && e[0] == g_mz) { e[1] += (DWORD)dz; dz = 0; }
            }
            if (dx && w < cap) { put_event(b, cbObj, w++, g_mx, (DWORD)dx, ts, seq); dx = 0; }
            if (dy && w < cap) { put_event(b, cbObj, w++, g_my, (DWORD)dy, ts, seq); dy = 0; }
            if (dz && w < cap) { put_event(b, cbObj, w++, g_mz, (DWORD)dz, ts, seq); dz = 0; }
            if (dx || dy || dz) {
                static int noRoom; if (noRoom++ < 5) logf("Input: mouse GetDeviceData had no room for the gamepad motion (room %u)", cap);
                gamepad_give_back(dx, dy, dz);
            }
        }
    }
    *inout = w;
    return hr;
}

/* 09.11.2026: the game releases and recreates its devices, and a new device can get the address of a released one (a mouse
   at the old keyboard's address went through the keyboard filter: the camera did not turn in those runs). So an address
   belongs only to the device created last. */
static void dev_add(void** tab, void* d)
{
    for (int i = 0; i < 4; i++) if (tab[i] == d) return;
    for (int i = 0; i < 4; i++) if (!tab[i]) { tab[i] = d; return; }
    for (int i = 0; i < 3; i++) tab[i] = tab[i + 1];
    tab[3] = d;
}
static void dev_forget(void** tab, void* d, const char* what)
{
    for (int i = 0; i < 4; i++) if (tab[i] == d) { tab[i] = 0; logf("Input: address %08X reused, no longer a %s", (DWORD)d, what); }
}

static long WINAPI hk_Create(void* di, const void* g, void** out, void* outer)
{
    long hr = g_origCreate(di, g, out, outer);
    static const BYTE SysKeyboard[16] = { 0x61,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
    static const BYTE SysMouse[16]    = { 0x60,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
    if (hr >= 0 && out && *out && g && readable(g, 16)) {
        const BYTE* a = (const BYTE*)g; int kb = 1, ms = 1;
        for (int i = 0; i < 16; i++) { if (a[i] != SysKeyboard[i]) kb = 0; if (a[i] != SysMouse[i]) ms = 0; }
        if (ms) dev_forget(g_kbd, *out, "keyboard");
        if (kb) { dev_forget(g_wheelMouse, *out, "mouse"); dev_forget(g_mouse, *out, "mouse"); }
        if (ms && !g_padMouse) {                     /* 08.11: the journal wheel only reads the mouse's events */
            dev_add(g_wheelMouse, *out);
            logf("Input: mouse device %08X (journal wheel)", (DWORD)*out);
        }
        if (kb || (ms && g_padMouse)) {
            void** tab = kb ? g_kbd : g_mouse;
            dev_add(tab, *out);
            /* All DirectInput devices share one vtable, and diag.c hooks the same GetDeviceState/GetDeviceData slots for
               the mouse. Hook each slot once only: re-hooking a slot that now holds another hook (ours or not) would
               save that hook as "original" and close a call loop (crash on the second keyboard/mouse device). */
            void** vt = *(void***)*out;
            if (!g_origCoop) { g_origCoop = (SetCoop_t)vt[13]; void* h = (void*)hk_SetCoop; write_mem(&vt[13], &h, 4); }
            if ((g_strafe || g_pad) && !g_oGDS) { g_oGDS = (GDS_t)vt[9]; void* h = (void*)hk_GDS; write_mem(&vt[9], &h, 4); }
            if ((g_strafe || g_pad) && !g_oGDD) { g_oGDD = (GDD_t)vt[10]; void* h = (void*)hk_GDD; write_mem(&vt[10], &h, 4); }
            if (g_pad && !g_oSetFmt) { g_oSetFmt = (SetFmt_t)vt[11]; void* h = (void*)hk_SetFmt; write_mem(&vt[11], &h, 4); }
            if (g_pad && !g_oSetProp) { g_oSetProp = (SetProp_t)vt[6]; void* h = (void*)hk_SetProp; write_mem(&vt[6], &h, 4); }
            logf("Input: %s device %08X", kb ? "keyboard" : "mouse", (DWORD)*out);
        }
    }
    return hr;
}

void input_on_dinput(void* di)
{
    g_strafe = GetPrivateProfileIntA("Input", "StrafeBothCancel", 1, g_ini);
    g_shared = GetPrivateProfileIntA("Window", "KeyboardShared", 1, g_ini);
    g_pad = gamepad_on();
    if (!di || (!g_shared && !g_strafe && !g_pad)) return;
    void** vt = *(void***)di;
    if (g_origCreate) return;           /* once only: diag.c chains its own CreateDevice hook on top (see hk_Create) */
    g_origCreate = (DICreateDevice_t)vt[3];
    void* h = (void*)hk_Create; write_mem(&vt[3], &h, 4);
    static int started; if (!started && g_shared) { started = 1; HANDLE th = CreateThread(0, 0, layout_thread, 0, 0, 0); if (th) CloseHandle(th); logf("Input: layout hotkey helper active (Alt+Shift, Ctrl+Shift, Win+Space)"); }
}
