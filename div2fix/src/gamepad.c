/* Div2Fix - gamepad (XInput + DualSense / DualShock 4 through HID) for Divinity II: Developer's Cut.
 *
 * Controllers: any XInput pad, and PlayStation pads read directly through HID (hid_thread below), so a DualSense
 * works without DSX, DS4Windows or Steam Input. When both deliver (Steam's virtual XInput pad + the real
 * DualSense) the states are merged.
 *
 * The PC build reads the keyboard and the mouse through DirectInput 8 (camera = mouse deltas) and draws its menu
 * cursor as a Scaleform movie (mouseCursor.gfx) that follows the Windows cursor (GetCursorPos at 0x8EB340,
 * 0x99EA60). The engine still has Xbox 360 binding tables (BUTTON_A.. at 0x14CFB30) but no PC code path feeds
 * them, and GUI\Xenon\controls.swf is not shipped. So this module turns the pad into input the game already knows:
 *   - keys: window.c merges g_padKey[] into the DirectInput keyboard (GetDeviceState / GetDeviceData);
 *   - camera: window.c adds the right stick to the DirectInput mouse deltas, mouse buttons the same way;
 *   - menus: the stick moves the Windows cursor (SetCursorPos), A/X are mouse buttons.
 * Menu or game: a menu movie got NotifyMouseState from the game in the last MenuHoldMs (hook in div2fix.c, UIAspect).
 * Default bindings follow the game's own defaults (0x82E790, names from 0x82CB00):
 *   W/S/A/D move, mouse left = AM_ATTACK, E / mouse right = AM_INTERACT_ITEM, Space = AM_JUMP, Tab = AM_COMBATMODE,
 *   F = AM_PICKCLOSESTITEM (and AM_PAUSE), Q = AM_PLAYER_MORPH, CapsLock = AM_TOGGLE_RUN, Shift = AM_QUICKLOCK /
 *   AM_FASTER, R = AM_NEXTLOCK, Y = AM_CANCELLOCK, 1-8 = AM_PC_HUD_BOTTOM_0..7, I/U/K/L/M = inventory / dragonstone /
 *   skills / quest log / map, F1-F3 = weapon sets, F5/F9 = quick save/load, Esc = AM_TOGGLE_MENU.
 * INI [Gamepad]: see Div2Fix.ini. Everything is read once at start.
 */
#include "common.h"

__declspec(dllimport) BOOL WINAPI GetCursorPos(long* pt);
__declspec(dllimport) BOOL WINAPI SetCursorPos(int x, int y);
__declspec(dllimport) BOOL WINAPI GetWindowRect(void* hwnd, long* rc);
__declspec(dllimport) UINT WINAPI SendInput(UINT n, void* inputs, int size);

typedef struct { WORD buttons; BYTE lt, rt; short lx, ly, rx, ry; } XGamepad;
typedef struct { DWORD packet; XGamepad g; } XState;
typedef DWORD (WINAPI *XGetState_t)(DWORD, XState*);

enum { B_DUP = 0x1, B_DDOWN = 0x2, B_DLEFT = 0x4, B_DRIGHT = 0x8, B_START = 0x10, B_BACK = 0x20, B_L3 = 0x40, B_R3 = 0x80,
       B_LB = 0x100, B_RB = 0x200, B_A = 0x1000, B_B = 0x2000, B_X = 0x4000, B_Y = 0x8000, B_LT = 0x10000, B_RT = 0x20000 };
#define NB 16
static const char* const k_bname[NB] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "BACK", "START", "L3", "R3",
                                         "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT" };
static const DWORD k_bbit[NB] = { B_A, B_B, B_X, B_Y, B_LB, B_RB, B_LT, B_RT, B_BACK, B_START, B_L3, B_R3,
                                  B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT };
enum { I_LB = 4, I_RB = 5 };

/* action: 1..255 = DirectInput scan code (= the game's KEY_* index), 0x100+n = mouse button n, 0x200/0x201 = wheel up/down */
#define A_MOUSE 0x100
#define A_WHEEL_UP 0x200
#define A_WHEEL_DOWN 0x201
#define A_SNAP 0x300                         /* 0x300..0x303: menu cursor jumps to the next item up/down/left/right */
enum { L_BASE, L_LB, L_RB, L_MENU, NL };
static const char* const k_prefix[NL] = { "", "LB_", "RB_", "MENU_" };
static const char* const k_default[NL][NB] = {
    /* A        B         X             Y      LB          RB            LT        RT            BACK  START     L3         R3   UP    DOWN    LEFT    RIGHT */
    { "SPACE", "TAB",    "E",          "F",   "",         "",           "LSHIFT", "MOUSE_LEFT", "U",  "ESCAPE", "CAPITAL", "R", "I",  "M",    "L",    "K" },
    { "1",     "2",      "3",          "4",   "",         "",           "",       "",           "",   "",       "",        "",  "F5", "F9",   "F1",   "F2" },
    { "5",     "6",      "7",          "8",   "",         "",           "",       "",           "",   "",       "",        "",  "Q",  "Y",    "F3",   "MOUSE_MIDDLE" },
    { "MOUSE_LEFT", "ESCAPE", "MOUSE_RIGHT", "", "WHEEL_UP", "WHEEL_DOWN", "MOUSE_RIGHT", "MOUSE_LEFT", "U",  "ESCAPE", "",        "",  "", "", "", "" },
};
static WORD g_map[NL][NB];
static int g_lbLayer, g_rbLayer;

static const struct { const char* n; BYTE dik; } k_keys[] = {
    {"ESCAPE",0x01},{"ESC",0x01},{"MINUS",0x0C},{"EQUALS",0x0D},{"BACKSPACE",0x0E},{"BACK",0x0E},{"TAB",0x0F},
    {"Q",0x10},{"W",0x11},{"E",0x12},{"R",0x13},{"T",0x14},{"Y",0x15},{"U",0x16},{"I",0x17},{"O",0x18},{"P",0x19},
    {"RETURN",0x1C},{"ENTER",0x1C},{"LCONTROL",0x1D},{"CTRL",0x1D},{"A",0x1E},{"S",0x1F},{"D",0x20},{"F",0x21},{"G",0x22},
    {"H",0x23},{"J",0x24},{"K",0x25},{"L",0x26},{"LSHIFT",0x2A},{"SHIFT",0x2A},{"Z",0x2C},{"X",0x2D},{"C",0x2E},{"V",0x2F},
    {"B",0x30},{"N",0x31},{"M",0x32},{"RSHIFT",0x36},{"LMENU",0x38},{"ALT",0x38},{"SPACE",0x39},{"CAPITAL",0x3A},
    {"CAPSLOCK",0x3A},{"F1",0x3B},{"F2",0x3C},{"F3",0x3D},{"F4",0x3E},{"F5",0x3F},{"F6",0x40},{"F7",0x41},{"F8",0x42},
    {"F9",0x43},{"F10",0x44},{"F11",0x57},{"F12",0x58},{"SUBTRACT",0x4A},{"ADD",0x4E},{"NUMPAD0",0x52},{"RCONTROL",0x9D},
    {"RMENU",0xB8},{"HOME",0xC7},{"UP",0xC8},{"PRIOR",0xC9},{"PGUP",0xC9},{"LEFT",0xCB},{"RIGHT",0xCD},{"END",0xCF},
    {"DOWN",0xD0},{"NEXT",0xD1},{"PGDN",0xD1},{"INSERT",0xD2},{"DELETE",0xD3},
};

static int eq_nc(const char* a, const char* b) { while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; } return !*a && !*b; }

static WORD parse_action(const char* s)
{
    while (*s == ' ') s++;
    char t[32]; int n = 0;
    while (s[n] && s[n] != ' ' && s[n] != ';' && n < 31) { t[n] = s[n]; n++; }
    t[n] = 0;
    if (!n || (n == 1 && t[0] == '-')) return 0;
    const char* u = t;
    if (n > 4 && lc(t[0]) == 'k' && lc(t[1]) == 'e' && lc(t[2]) == 'y' && t[3] == '_') u = t + 4;     /* KEY_W -> W */
    if (u[0] >= '0' && u[0] <= '9' && !u[1]) return (WORD)(u[0] == '0' ? 0x0B : 0x02 + (u[0] - '1'));
    if (u[0] == '0' && lc(u[1]) == 'x') {                                                            /* 0x39 */
        int v = 0; for (const char* p = u + 2; *p; p++) { char c = lc(*p); v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); }
        return (WORD)(v & 0xFF);
    }
    if (eq_nc(u, "MOUSE_LEFT") || eq_nc(u, "LMB")) return A_MOUSE + 0;
    if (eq_nc(u, "MOUSE_RIGHT") || eq_nc(u, "RMB")) return A_MOUSE + 1;
    if (eq_nc(u, "MOUSE_MIDDLE") || eq_nc(u, "MMB")) return A_MOUSE + 2;
    if (eq_nc(u, "WHEEL_UP")) return A_WHEEL_UP;
    if (eq_nc(u, "WHEEL_DOWN")) return A_WHEEL_DOWN;
    if (eq_nc(u, "SNAP_UP")) return A_SNAP + 0;
    if (eq_nc(u, "SNAP_DOWN")) return A_SNAP + 1;
    if (eq_nc(u, "SNAP_LEFT")) return A_SNAP + 2;
    if (eq_nc(u, "SNAP_RIGHT")) return A_SNAP + 3;
    for (int i = 0; i < (int)(sizeof k_keys / sizeof k_keys[0]); i++) if (eq_nc(u, k_keys[i].n)) return k_keys[i].dik;
    logf("Gamepad: unknown key name '%s' -> unbound", t);
    return 0;
}

/* ---- shared with window.c / div2fix.c ---- */
volatile BYTE g_padKey[256];          /* 1 = the pad holds this DirectInput key */
volatile BYTE g_padBtn[8];            /* 1 = the pad holds this mouse button */
BYTE g_padMapped[256];                /* keys any binding can produce (window.c filters only these) */
static volatile long g_mdx, g_mdy, g_mdz;
static int g_mouseOut;
static volatile DWORD g_uiTick, g_uiCalls;
static int g_on, g_uiHook;

int gamepad_on(void) { return g_on; }
int gamepad_injects_mouse(void) { return g_on && g_mouseOut == 1; }   /* 07.11: only then window.c touches the DirectInput mouse */
void gamepad_take_motion(long* dx, long* dy, long* dz)
{
    *dx = __sync_lock_test_and_set(&g_mdx, 0);
    *dy = __sync_lock_test_and_set(&g_mdy, 0);
    *dz = __sync_lock_test_and_set(&g_mdz, 0);
}
void gamepad_give_back(long dx, long dy, long dz)    /* GetDeviceData had no room: deliver next call */
{
    if (dx) __sync_fetch_and_add(&g_mdx, dx);
    if (dy) __sync_fetch_and_add(&g_mdy, dy);
    if (dz) __sync_fetch_and_add(&g_mdz, dz);
}

/* ---- Windows cursor ----
 * The menus (Scaleform, mouseCursor.gfx) take the cursor position from GetCursorPos (0x8EB340, 0x99EA60).
 * Motion for it is applied inside the game's own GetCursorPos call (IAT slot 0x12B7A34), i.e. on the game thread
 * right before the game reads it; if the game stops calling it, the pad thread applies it. */
typedef BOOL (WINAPI *GCP_t)(long*);
static GCP_t g_origGCP;
static volatile long g_cdx, g_cdy;
static volatile DWORD g_gcpTick, g_gcpCalls;
/* g_mouseOut (declared above): 0 = SendInput: the pad is a real mouse for Windows and DirectInput; 1 = injected into DirectInput */
static void apply_cursor(void)
{
    long dx = __sync_lock_test_and_set(&g_cdx, 0), dy = __sync_lock_test_and_set(&g_cdy, 0);
    if (!dx && !dy) return;
    long pt[2], rc[4];
    if (!GetCursorPos(pt)) return;
    long x = pt[0] + dx, y = pt[1] + dy;
    void* fg = GetForegroundWindow();
    if (fg && GetWindowRect(fg, rc)) {
        if (x < rc[0]) x = rc[0]; if (x > rc[2] - 1) x = rc[2] - 1;
        if (y < rc[1]) y = rc[1]; if (y > rc[3] - 1) y = rc[3] - 1;
    }
    SetCursorPos(x, y);
}
static BOOL WINAPI hk_GetCursorPos(long* pt)
{
    g_gcpTick = GetTickCount(); g_gcpCalls++;
    apply_cursor();
    return g_origGCP(pt);
}

/* ---- menu detection + statistics of what the UI movies receive (div2fix.c hooks, game thread) ----
 * kind 0 = GFxMovieRoot::NotifyMouseState, 1 = HandleEvent with a mouse event, 2 = SetViewport.
 * 0 and 1 for a movie that is not in GameMovies mean "a menu is open". */
static char g_gameMovies[512];         /* ";hud;radar;..." lowercase: movies that do NOT mean "menu open" */
typedef struct { DWORD h; char name[28]; volatile DWORD n[3]; int game; } UiStat;
static UiStat g_ui[32]; static volatile int g_uiN; static volatile int g_uiLast = -1;
void gamepad_ui_hooked(void) { g_uiHook = 1; }
void gamepad_ui_call(const char* name, int kind)
{
    if (!g_on) return;
    char key[80]; int k = 0; key[k++] = ';';
    for (int i = 0; name[i] && k < 76; i++) key[k++] = lc(name[i]);
    key[k++] = ';'; key[k] = 0;
    DWORD h = 2166136261u; for (int i = 0; key[i]; i++) h = (h ^ (BYTE)key[i]) * 16777619u;
    UiStat* s = 0;
    for (int i = 0; i < g_uiN; i++) if (g_ui[i].h == h) { s = &g_ui[i]; break; }
    if (!s && g_uiN < 32) {
        s = &g_ui[g_uiN];
        s->h = h; int j = 0; for (; name[j] && j < 27; j++) s->name[j] = name[j]; s->name[j] = 0;
        s->game = 0;
        for (int i = 0; g_gameMovies[i] && !s->game; i++) { int q = 0; while (key[q] && g_gameMovies[i + q] == key[q]) q++; if (!key[q]) s->game = 1; }
        g_uiN++;
        logf("Gamepad: UI movie '%s' seen (%s) -> %s", name, kind == 0 ? "NotifyMouseState" : kind == 1 ? "mouse event" : "SetViewport",
             s->game ? "HUD, not a menu" : "menu");
    }
    if (!s) return;
    s->n[kind]++;
    /* SetViewport is called every frame for each movie that is on screen (log of 06.11: mainMenu, popup,
       tacticalPause, save appear and disappear with the menu; hud, radar, summons, mouseCursor always there).
       Mouse events are no signal: tooltip and hud get them during play too. */
    if (kind == 2 && !s->game) { g_uiTick = GetTickCount(); g_uiCalls++; g_uiLast = (int)(s - g_ui); }
}
void gamepad_ui_movie(const char* name) { gamepad_ui_call(name, 0); }

/* ---- menu hover events ----
 * Every menu movie reports hover and clicks to the game with getURL("FSCommand:<name>", args); the game's menu base
 * class receives them in 0x99F1C0 (thiscall: std::string* cmd, std::string* args; it checks "soundEvent" and passes
 * the rest to the menu's own handler, vtbl+6Ch). Names seen in the movies: slotRollOver, skillRollOver,
 * selectedMenuItem, setRollOver, tabRollOver, ... and *RollOut / deselectedMenuItem. We count the "over" events. */
typedef void (__fastcall *MenuCmd_t)(void* menu, void* edx, void* cmd, void* args);
static MenuCmd_t g_origMenuCmd;
static volatile DWORD g_overSeq;
static const char* std_str(const BYTE* s)        /* MSVC 2008 std::string: buffer/pointer at +4, capacity at +18h */
{
    if (!s || !readable(s, 0x1C)) return 0;
    DWORD res = *(const DWORD*)(s + 0x18);
    const char* t = res >= 16 ? *(const char* const*)(s + 4) : (const char*)(s + 4);
    return readable(t, 1) ? t : 0;
}
static int ends_with(const char* t, const char* e) { int a = s_len(t), b = s_len(e); if (a < b) return 0; for (int i = 0; i < b; i++) if (t[a - b + i] != e[i]) return 0; return 1; }
static void __fastcall hk_MenuCmd(void* menu, void* edx, void* cmd, void* args)
{
    const char* t = std_str((const BYTE*)cmd);
    /* log of 06.11: only "soundEvent" arrives here - the movies play the hover sound on roll-over; while the d-pad
       sweeps the cursor nothing is clicked, so a sound then means "now over a new item" */
    const char* av = std_str((const BYTE*)args);
    /* log of 06.11: the hover sound is soundEvent 'buttonRollOver'; save slots send saveItemSelected */
    if (t && (ends_with(t, "RollOver") || ends_with(t, "Selected") || ends_with(t, "electedMenuItem") ||
              (ends_with(t, "soundEvent") && av && ends_with(av, "RollOver"))))
        g_overSeq++;
    static int logged;
    if (t && logged < 40) { logged++; logf("Gamepad: menu command '%.40s' args '%.60s'", t, av ? av : "?"); }
    g_origMenuCmd(menu, edx, cmd, args);
}

/* ---- settings ---- */
static float g_dzL, g_dzR, g_trig, g_camX, g_camY, g_cursor, g_scroll, g_moveThr;
static int g_invY, g_mode, g_hold, g_slotPref, g_hidRead;
static int g_dbgLeft; static DWORD g_dbgSig = 0xFFFFFFFF;
static volatile DWORD g_lastPadInput; static volatile int g_padKind;
static volatile DWORD g_lastKbmInput;   /* last real keyboard / mouse input (not injected by us), from low-level hooks */
/* prompts follow the device used last: the pad, unless the real keyboard or mouse was used after it */
static int pad_prompts(void) { return g_padKind && g_lastPadInput && (int)(g_lastPadInput - g_lastKbmInput) >= 0; }   /* 1 = PlayStation (HID), 2 = Xbox (XInput) */
static int g_glyphs, g_snapMs, g_snapLag, g_stickyMs; static float g_stickyFactor, g_dpadSlow; static float g_snapSpeed;
static int g_reports, g_reportMax, g_padUsedRecently;            /* [Gamepad] LogUI: 3-second UI reports */          /* [Gamepad] LogInput: first N input changes to the log */

static float ini_f(const char* key, const char* def)
{
    char b[32]; GetPrivateProfileStringA("Gamepad", key, def, b, sizeof b, g_ini);
    float v = 0, f = 0; int neg = 0, i = 0;
    if (b[0] == '-') { neg = 1; i = 1; }
    for (; b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
    if (b[i] == '.') { float d = 0.1f; for (i++; b[i] >= '0' && b[i] <= '9'; i++) { f += (b[i] - '0') * d; d *= 0.1f; } }
    return neg ? -(v + f) : v + f;
}

static void load_settings(void)
{
    g_dzL = ini_f("LeftDeadzone", "0.24"); g_dzR = ini_f("RightDeadzone", "0.20"); g_trig = ini_f("TriggerThreshold", "0.35");
    g_camX = ini_f("CameraSpeedX", "1400"); g_camY = ini_f("CameraSpeedY", "900"); g_cursor = ini_f("CursorSpeed", "1100");
    g_scroll = ini_f("ScrollSpeed", "10"); g_moveThr = ini_f("MoveThreshold", "0.35");
    g_invY = GetPrivateProfileIntA("Gamepad", "InvertY", 0, g_ini);
    g_mode = GetPrivateProfileIntA("Gamepad", "Mode", 0, g_ini);
    g_hold = GetPrivateProfileIntA("Gamepad", "MenuHoldMs", 250, g_ini);
    g_slotPref = GetPrivateProfileIntA("Gamepad", "Controller", 0, g_ini);   /* 0 = first connected, 1-4 = that slot */
    g_hidRead = GetPrivateProfileIntA("Gamepad", "PlayStationHID", 1, g_ini);  /* DualSense / DualShock 4 directly */
    g_dbgLeft = GetPrivateProfileIntA("Gamepad", "LogInput", 40, g_ini);
    g_reportMax = GetPrivateProfileIntA("Gamepad", "LogUI", 40, g_ini);
    g_glyphs = GetPrivateProfileIntA("Gamepad", "Glyphs", 1, g_ini);   /* button icons in key prompts */
    g_snapMs = GetPrivateProfileIntA("Gamepad", "SnapMaxMs", 800, g_ini);
    g_snapSpeed = ini_f("SnapSpeed", "1600");
    g_stickyMs = GetPrivateProfileIntA("Gamepad", "StickyMs", 350, g_ini);   /* slow-down after hovering a new item */
    g_stickyFactor = ini_f("StickyFactor", "0.30");
    g_dpadSlow = ini_f("DpadCursor", "0.25");                                 /* d-pad cursor speed (part of CursorSpeed) */
    g_snapLag = GetPrivateProfileIntA("Gamepad", "SnapLagMs", 25, g_ini);   /* hover sound delay to undo */
    g_mouseOut = GetPrivateProfileIntA("Gamepad", "MouseOutput", 0, g_ini);   /* 0 = SendInput (like a real mouse), 1 = DirectInput */
    char raw[512];
    GetPrivateProfileStringA("Gamepad", "GameMovies",
        "hud,hudSlots,radar,buff,hitDamage,notification,subtitles,timer,range,text_gui,achievement,levelUp,mapable,"
        "debug,hourGlass,animatedWait,dialogFade,mouseCursor,summons,tooltip,automatedDialog,loadingScreen,"
        "loadingScreen_still,simpleLoadingScreen", raw, sizeof raw, g_ini);
    int k = 0; g_gameMovies[k++] = ';';
    for (int i = 0; raw[i] && k < (int)sizeof g_gameMovies - 2; i++) {
        char c = raw[i];
        if (c == ',' || c == ' ' || c == ';') { if (g_gameMovies[k - 1] != ';') g_gameMovies[k++] = ';'; }
        else g_gameMovies[k++] = lc(c);
    }
    if (g_gameMovies[k - 1] != ';') g_gameMovies[k++] = ';';
    g_gameMovies[k] = 0;
    for (int l = 0; l < NL; l++)
        for (int b = 0; b < NB; b++) {
            char key[32], val[32]; wsprintfA(key, "%s%s", k_prefix[l], k_bname[b]);
            GetPrivateProfileStringA("Gamepad", key, k_default[l][b], val, sizeof val, g_ini);
            g_map[l][b] = parse_action(val);
            if (g_map[l][b] && g_map[l][b] < 0x100) g_padMapped[g_map[l][b]] = 1;
        }
    g_padMapped[0x11] = g_padMapped[0x1F] = g_padMapped[0x1E] = g_padMapped[0x20] = 1;   /* left stick: W S A D */
    for (int b = 0; b < NB; b++) { if (g_map[L_LB][b]) g_lbLayer = 1; if (g_map[L_RB][b]) g_rbLayer = 1; }
    /* the modifier itself has no action in the game layer while its layer is used */
    if (g_lbLayer) g_map[L_BASE][I_LB] = 0;
    if (g_rbLayer) g_map[L_BASE][I_RB] = 0;
}

/* stick value after the dead zone, -1..1, squared response for fine aiming */
static float axis(short v, float dz, int curve)
{
    float f = v / 32767.0f; if (f > 1) f = 1; if (f < -1) f = -1;
    float a = f < 0 ? -f : f;
    if (a <= dz) return 0;
    a = (a - dz) / (1 - dz);
    if (curve) a = a * a;
    return f < 0 ? -a : a;
}

static void kbm_start(void);
static int game_focused(void)
{
    void* fg = GetForegroundWindow(); DWORD pid = 0;
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void send_mouse(DWORD flags, DWORD data)
{
    struct { DWORD type; long dx, dy; DWORD data, flags, time; void* extra; } in;
    memset(&in, 0, sizeof in); in.type = 0 /*INPUT_MOUSE*/; in.flags = flags; in.data = data;
    SendInput(1, &in, sizeof in);
}

static void send_move(long dx, long dy)
{
    struct { DWORD type; long dx, dy; DWORD data, flags, time; void* extra; } in;
    memset(&in, 0, sizeof in); in.type = 0 /*INPUT_MOUSE*/; in.dx = dx; in.dy = dy; in.flags = 1 /*MOUSEEVENTF_MOVE*/;
    SendInput(1, &in, sizeof in);
}

static WORD resolve(int b, int menu, DWORD cur)
{
    if (menu) return g_map[L_MENU][b];
    if (b == I_LB && g_lbLayer) return 0;
    if (b == I_RB && g_rbLayer) return 0;
    if ((cur & B_LB) && g_lbLayer && g_map[L_LB][b]) return g_map[L_LB][b];
    if ((cur & B_RB) && g_rbLayer && g_map[L_RB][b]) return g_map[L_RB][b];
    return g_map[L_BASE][b];
}

/* ---- Sony controllers read directly through HID (no DSX / Steam Input / DS4Windows needed) ----
 * DualSense (054C:0CE6), DualSense Edge (0DF2), DualShock 4 (05C4, 09CC, dongle 0BA0); USB and Bluetooth.
 * Report layouts (public, e.g. Linux hid-playstation):
 *   DualSense USB 0x01 / BT 0x31 (data at +1 / +2): LX LY RX RY L2 R2 counter btn0 btn1 btn2
 *   DualShock 4 USB 0x01 / BT 0x11 (data at +1 / +3), DualSense BT "simple" 0x01: LX LY RX RY btn0 btn1 btn2 L2 R2
 *   btn0: hat (low nibble, 8 = none), 10 square, 20 cross, 40 circle, 80 triangle
 *   btn1: 01 L1, 02 R1, 04 L2, 08 R2, 10 create/share, 20 options, 40 L3, 80 R3;  btn2: 01 PS, 02 touchpad click
 * Over Bluetooth both pads start in the simple 0x01 report; reading feature report 05 (DualSense) / 02 (DS4) switches
 * them to the full report. The device is opened shared, so Steam or other tools keep working. */
typedef struct { DWORD Size; WORD VendorID, ProductID, Version; WORD pad; } HIDD_ATTR;   /* HIDD_ATTRIBUTES, 12 bytes */
typedef struct { DWORD cbSize; GUID cls; DWORD flags; DWORD reserved; } SP_DEVIFACE;
typedef void (WINAPI *HidGuid_t)(GUID*);
typedef BOOL (WINAPI *HidAttr_t)(HANDLE, HIDD_ATTR*);
typedef BOOL (WINAPI *HidFeat_t)(HANDLE, void*, DWORD);
typedef void* (WINAPI *SdGetClassDevs_t)(const GUID*, const char*, void*, DWORD);
typedef BOOL (WINAPI *SdEnumIfaces_t)(void*, void*, const GUID*, DWORD, SP_DEVIFACE*);
typedef BOOL (WINAPI *SdIfaceDetail_t)(void*, SP_DEVIFACE*, void*, DWORD, DWORD*, void*);
typedef BOOL (WINAPI *SdDestroy_t)(void*);

static volatile XState g_hidState;
static volatile int g_hidOn;                       /* a Sony pad is open and its reports arrive */
static volatile DWORD g_hidSeq;

static int sony_pid(WORD pid, int* ds5)
{
    *ds5 = pid == 0x0CE6 || pid == 0x0DF2;
    return *ds5 || pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0;
}

static short stick(BYTE v, int flip)
{
    int s = ((int)v - 128) * 256; if (flip) s = -s;                 /* 128 = centre = 0 */
    if (s > 32767) s = 32767; if (s < -32768) s = -32768;
    return (short)s;
}

/* one input report -> XInput layout; 0 if the report is not a pad state */
int sony_parse(const BYTE* r, DWORD n, int ds5, XGamepad* g)
{
    const BYTE* d; BYTE b0, b1, b2, l2, r2;
    if (n < 10) return 0;
    if (ds5 && r[0] == 0x31 && n >= 12) { d = r + 2; b0 = d[7]; b1 = d[8]; b2 = d[9]; l2 = d[4]; r2 = d[5]; }
    /* ReadFile returns the longest input report of the device: 64 bytes over USB, 78 (DualSense) / 547 (DS4) over Bluetooth */
    else if (ds5 && r[0] == 0x01 && n == 64) { d = r + 1; b0 = d[7]; b1 = d[8]; b2 = d[9]; l2 = d[4]; r2 = d[5]; }
    else if (!ds5 && r[0] == 0x11 && n >= 12) { d = r + 3; b0 = d[4]; b1 = d[5]; b2 = d[6]; l2 = d[7]; r2 = d[8]; }
    else if (r[0] == 0x01) { d = r + 1; b0 = d[4]; b1 = d[5]; b2 = d[6]; l2 = d[7]; r2 = d[8]; }   /* DS4 USB, BT simple */
    else return 0;
    static const WORD hat[9] = { B_DUP, B_DUP | B_DRIGHT, B_DRIGHT, B_DDOWN | B_DRIGHT, B_DDOWN, B_DDOWN | B_DLEFT,
                                 B_DLEFT, B_DUP | B_DLEFT, 0 };
    WORD b = (b0 & 15) <= 8 ? hat[b0 & 15] : 0;
    if (b0 & 0x10) b |= B_X; if (b0 & 0x20) b |= B_A; if (b0 & 0x40) b |= B_B; if (b0 & 0x80) b |= B_Y;
    if (b1 & 0x01) b |= B_LB; if (b1 & 0x02) b |= B_RB;
    if (b1 & 0x10) b |= B_BACK; if (b1 & 0x20) b |= B_START; if (b1 & 0x40) b |= B_L3; if (b1 & 0x80) b |= B_R3;
    if (b2 & 0x02) b |= B_BACK;                                     /* touchpad click = BACK as well */
    g->buttons = b; g->lt = l2; g->rt = r2;
    g->lx = stick(d[0], 0); g->ly = stick(d[1], 1); g->rx = stick(d[2], 0); g->ry = stick(d[3], 1);
    return 1;
}

static DWORD WINAPI hid_thread(void* arg)
{
    (void)arg;
    HMODULE hid = LoadLibraryA("hid.dll"), sapi = LoadLibraryA("setupapi.dll");
    HidGuid_t getGuid = hid ? (HidGuid_t)GetProcAddress(hid, "HidD_GetHidGuid") : 0;
    HidAttr_t getAttr = hid ? (HidAttr_t)GetProcAddress(hid, "HidD_GetAttributes") : 0;
    HidFeat_t getFeat = hid ? (HidFeat_t)GetProcAddress(hid, "HidD_GetFeature") : 0;
    SdGetClassDevs_t classDevs = sapi ? (SdGetClassDevs_t)GetProcAddress(sapi, "SetupDiGetClassDevsA") : 0;
    SdEnumIfaces_t enumIf = sapi ? (SdEnumIfaces_t)GetProcAddress(sapi, "SetupDiEnumDeviceInterfaces") : 0;
    SdIfaceDetail_t detail = sapi ? (SdIfaceDetail_t)GetProcAddress(sapi, "SetupDiGetDeviceInterfaceDetailA") : 0;
    SdDestroy_t destroy = sapi ? (SdDestroy_t)GetProcAddress(sapi, "SetupDiDestroyDeviceInfoList") : 0;
    if (!getGuid || !getAttr || !classDevs || !enumIf || !detail || !destroy) { logf("Gamepad: hid.dll/setupapi.dll not usable -> PlayStation pads only through XInput"); return 0; }
    GUID hg; getGuid(&hg);
    int logged = 0;
    for (;;) {
        /* find a Sony pad */
        HANDLE dev = INVALID_HANDLE_VALUE; int ds5 = 0; WORD pid = 0; DWORD nIf = 0, nSony = 0, nOpenFail = 0, lastErr = 0;
        void* set = classDevs(&hg, 0, 0, 0x12 /*DIGCF_PRESENT | DIGCF_DEVICEINTERFACE*/);
        if (set != INVALID_HANDLE_VALUE && set) {
            for (DWORD i = 0; dev == INVALID_HANDLE_VALUE && i < 256; i++) {
                SP_DEVIFACE ifd; memset(&ifd, 0, sizeof ifd); ifd.cbSize = sizeof ifd;
                if (!enumIf(set, 0, &hg, i, &ifd)) break;
                nIf++;
                struct { DWORD cbSize; char path[512]; } det; det.cbSize = 5;   /* sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) on x86 */
                if (!detail(set, &ifd, &det, sizeof det, 0, 0)) { lastErr = GetLastError() | 0x10000000; continue; }
                /* quick filter on the path before opening: Sony vendor id */
                /* USB: "...vid_054c&pid_0ce6...", Bluetooth: "..._vid&0002054c_pid&0ce6..." */
                int sony = 0; for (int k = 0; det.path[k] && det.path[k + 1] && det.path[k + 2] && det.path[k + 3] && det.path[k + 4]; k++) {
                    const char* p = det.path + k;
                    if (p[0] == '0' && p[1] == '5' && p[2] == '4' && lc(p[3]) == 'c' && (p[4] == '_' || p[4] == '&')) sony = 1;
                }
                if (!sony) continue;
                nSony++;
                if (!logged) logf("Gamepad: HID device %s", det.path);
                HANDLE h = CreateFileA(det.path, GENERIC_READ | GENERIC_WRITE, 3 /*share read|write*/, 0, OPEN_EXISTING, 0, 0);
                if (h == INVALID_HANDLE_VALUE) h = CreateFileA(det.path, GENERIC_READ, 3, 0, OPEN_EXISTING, 0, 0);
                if (h == INVALID_HANDLE_VALUE) { nOpenFail++; lastErr = GetLastError(); continue; }
                HIDD_ATTR at; memset(&at, 0, sizeof at); at.Size = sizeof at;
                if (getAttr(h, &at) && at.VendorID == 0x054C && sony_pid(at.ProductID, &ds5)) { dev = h; pid = at.ProductID; }
                else CloseHandle(h);
            }
            destroy(set);
        }
        if (dev == INVALID_HANDLE_VALUE) {
            if (!logged) {
                logf("Gamepad: no PlayStation controller found through HID (%u HID interfaces, %u Sony, %u could not be opened, error %08X; retry every 15 s)",
                     nIf, nSony, nOpenFail, lastErr);
                logged = 1;
            }
            Sleep(15000); continue;   /* 08.11: SetupDi every 2 s made the mouse/camera stutter */
        }
        logged = 0;
        logf("Gamepad: %s (054C:%04X) opened through HID", ds5 ? "DualSense" : "DualShock 4", pid);
        if (getFeat) {                                              /* Bluetooth: ask for the full report */
            BYTE f[80]; memset(f, 0, sizeof f); f[0] = ds5 ? 0x05 : 0x02;
            getFeat(dev, f, ds5 ? 41 : 37);
        }
        BYTE rep[600]; DWORD got = 0, bad = 0, firstId = 0xFFFF;
        for (;;) {
            if (!ReadFile(dev, rep, sizeof rep, &got, 0)) break;
            XGamepad g; memset(&g, 0, sizeof g);
            if (firstId == 0xFFFF) { firstId = rep[0]; logf("Gamepad: HID reports arrive (first report id %02X, %u bytes)", rep[0], got); }
            if (sony_parse(rep, got, ds5, &g)) {
                ((XState*)&g_hidState)->g = g; g_hidSeq++; g_hidOn = 1; bad = 0;
            } else if (++bad == 200) logf("Gamepad: HID report id %02X (%u bytes) not understood", rep[0], got);
        }
        g_hidOn = 0;
        CloseHandle(dev);
        logf("Gamepad: PlayStation controller disconnected");
        Sleep(1000);
    }
    return 0;
}

/* two controllers at once (e.g. Steam Input's virtual XInput pad and the real DualSense): buttons OR-ed,
   each stick and trigger from whichever is pushed further */
void merge_pad(XGamepad* a, const XGamepad* b)
{
    a->buttons |= b->buttons;
    if (b->lt > a->lt) a->lt = b->lt;
    if (b->rt > a->rt) a->rt = b->rt;
    int la = (a->lx < 0 ? -a->lx : a->lx) + (a->ly < 0 ? -a->ly : a->ly), lb = (b->lx < 0 ? -b->lx : b->lx) + (b->ly < 0 ? -b->ly : b->ly);
    if (lb > la) { a->lx = b->lx; a->ly = b->ly; }
    int ra = (a->rx < 0 ? -a->rx : a->rx) + (a->ry < 0 ? -a->ry : a->ry), rb = (b->rx < 0 ? -b->rx : b->rx) + (b->ry < 0 ? -b->ry : b->ry);
    if (rb > ra) { a->rx = b->rx; a->ry = b->ry; }
}

static DWORD WINAPI pad_thread(void* arg)
{
    (void)arg;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    XGetState_t get = 0;
    for (int i = 0; i < 3 && !get; i++) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) { get = (XGetState_t)GetProcAddress(h, "XInputGetState"); if (get) logf("Gamepad: %s loaded", dlls[i]); }
    }
    if (!get) logf("Gamepad: XInput not found -> only PlayStation controllers through HID");
    if (g_hidRead) { HANDLE th = CreateThread(0, 0, hid_thread, 0, 0, 0); if (th) CloseHandle(th); }
    if (!g_uiHook && g_mode == 0)
        logf("Gamepad: menu detection needs [UIAspect] Enable=1 (NotifyMouseState hook) -> always game mode; set Mode=2 for menus");

    HANDLE timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    long long fq, t0; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    int slot = -1; DWORD lastScan = 0, prev = 0, lastMenuLog = 0; int menu = 0, modeLogs = 0;
    WORD held[NB]; DWORD wheelNext[NB]; memset(held, 0, sizeof held); memset(wheelNext, 0, sizeof wheelNext);
    float accX = 0, accY = 0, accScroll = 0; BYTE sentBtn[3] = { 0, 0, 0 };
    DWORD menuStart = 0, menuCalls0 = 0, lastReport = 0, lastGcp = 0;
    int snapDir = -1; DWORD snapStart = 0, snapSeq = 0, snapNext = 0; int snapHold = -1; float snapAcc = 0;
    DWORD stickySeq = 0, stickyUntil = 0, navRepeat = 0, navPrev = 0;
    long snapHist[64]; DWORD snapHistT[64]; int snapHistN = 0;    /* recent sweep steps, to undo the overshoot */

    for (;;) {
        if (timer) { long long due = -40000; SetWaitableTimer(timer, &due, 0, 0, 0, 0); WaitForSingleObject(timer, 20); }
        else Sleep(4);
        long long t1; QueryPerformanceCounter(&t1);
        float dt = (float)(int)(t1 - t0) / (float)(int)fq; t0 = t1;
        if (dt > 0.05f) dt = 0.05f;
        DWORD now = GetTickCount();

        XState st; int ok = 0;
        memset(&st, 0, sizeof st);
        if (get && slot >= 0) {
            if (get((DWORD)slot, &st) == 0) ok = 1;
            else { logf("Gamepad: XInput controller %d disconnected", slot + 1); slot = -1; memset(&st, 0, sizeof st); }
        }
        if (get && slot < 0 && now - lastScan > 1000) {                 /* polling an empty slot is slow: once a second */
            lastScan = now;
            for (int i = 0; i < 4 && slot < 0; i++) {
                if (g_slotPref && i != g_slotPref - 1) continue;
                memset(&st, 0, sizeof st);
                if (get((DWORD)i, &st) == 0) { slot = i; ok = 1; logf("Gamepad: XInput controller %d connected", i + 1); }
            }
            if (slot < 0) memset(&st, 0, sizeof st);
        }
        if (g_hidOn) {                                                  /* PlayStation pad through HID: merged with XInput */
            XGamepad h = ((XState*)&g_hidState)->g;
            merge_pad(&st.g, &h); ok = 1;
        }
        int focus = game_focused();
        if (ok) {                                                       /* who was used last: for the prompt icons */
            int act = st.g.buttons || st.g.lt > 64 || st.g.rt > 64 || st.g.lx > 12000 || st.g.lx < -12000 ||
                      st.g.ly > 12000 || st.g.ly < -12000 || st.g.rx > 12000 || st.g.rx < -12000 || st.g.ry > 12000 || st.g.ry < -12000;
            if (act) { g_lastPadInput = now; g_padKind = g_hidOn ? 1 : 2; kbm_start(); }   /* 08.11: the low-level hooks only once a pad is used */
        }
        if (ok && g_dbgLeft > 0) {                                      /* first input changes go to the log */
            DWORD sig = st.g.buttons | (st.g.lt > 64 ? 0x10000 : 0) | (st.g.rt > 64 ? 0x20000 : 0) |
                        ((st.g.lx > 16000 || st.g.lx < -16000 || st.g.ly > 16000 || st.g.ly < -16000) ? 0x40000 : 0) |
                        ((st.g.rx > 16000 || st.g.rx < -16000 || st.g.ry > 16000 || st.g.ry < -16000) ? 0x80000 : 0);
            if (sig != g_dbgSig) {
                g_dbgSig = sig; g_dbgLeft--;
                logf("Gamepad: input buttons %04X LT %u RT %u L(%d,%d) R(%d,%d)%s", st.g.buttons, st.g.lt, st.g.rt,
                     st.g.lx, st.g.ly, st.g.rx, st.g.ry, focus ? "" : " - game window not in front, ignored");
            }
        }
        if (!ok || !focus) {                                           /* release everything */
            for (int i = 0; i < 256; i++) g_padKey[i] = 0;
            for (int i = 0; i < 8; i++) g_padBtn[i] = 0;
            { static const DWORD up[3] = { 4, 0x10, 0x40 }; for (int i = 0; i < 3; i++) if (sentBtn[i]) { send_mouse(up[i], 0); sentBtn[i] = 0; } }
            memset(held, 0, sizeof held); prev = 0; accX = accY = accScroll = 0;
            continue;
        }

        /* mode */
        int m = g_mode == 1 ? 0 : g_mode == 2 ? 1 : (g_uiHook && now - g_uiTick < (DWORD)g_hold);
        if (m != menu) {
            if (modeLogs < 60) {
                int li = g_uiLast;
                if (m) logf("Gamepad: menu mode ('%s' on screen)", li >= 0 ? g_ui[li].name : "?");
                else logf("Gamepad: game mode (menu mode lasted %u ms)", now - menuStart);
                if (++modeLogs == 60) logf("Gamepad: further mode changes not logged");
            }
            if (m) { menuStart = now; menuCalls0 = g_uiCalls; }
            (void)menuCalls0;
            menu = m; accX = accY = 0;
        }
        (void)lastMenuLog;

        DWORD cur = st.g.buttons;
        if (st.g.lt > g_trig * 255) cur |= B_LT;
        if (st.g.rt > g_trig * 255) cur |= B_RT;

        /* buttons: the action is chosen when the button goes down and kept until it is released,
           so a mode or modifier change while holding never turns a held key into another one */
        BYTE keys[256]; BYTE btn[8]; long wheel = 0;
        memset(keys, 0, sizeof keys); memset(btn, 0, sizeof btn);
        for (int b = 0; b < NB; b++) {
            DWORD bit = k_bbit[b];
            if ((cur & bit) && !(prev & bit)) {
                held[b] = resolve(b, menu, cur);
                if (menu && held[b] == A_MOUSE + 0 && uinav_accept()) held[b] = 0;   /* menu with its own cursor: press its selection */
                if (held[b] >= A_SNAP && held[b] < A_SNAP + 4) { snapDir = held[b] - A_SNAP; snapStart = now; snapSeq = g_overSeq; snapAcc = 0; snapHold = b; snapNext = 0; snapHistN = 0; }
                if (held[b] == A_WHEEL_UP || held[b] == A_WHEEL_DOWN) { wheel += held[b] == A_WHEEL_UP ? 120 : -120; wheelNext[b] = now + 400; }
            }
            if (!(cur & bit)) held[b] = 0;
            WORD a = held[b];
            if (!a) continue;
            if (a >= A_SNAP && a < A_SNAP + 4) {               /* held: jump again after the previous jump */
                if (snapDir < 0 && snapNext && (int)(now - snapNext) >= 0) { snapDir = a - A_SNAP; snapStart = now; snapSeq = g_overSeq; snapAcc = 0; snapHistN = 0; }
                continue;
            }
            if (a < 0x100) keys[a] = 1;
            else if (a < A_MOUSE + 8) btn[a - A_MOUSE] = 1;
            else if ((int)(now - wheelNext[b]) >= 0) { wheel += a == A_WHEEL_UP ? 120 : -120; wheelNext[b] = now + 120; }
        }
        prev = cur;

        float lx = axis(st.g.lx, g_dzL, 0), ly = axis(st.g.ly, g_dzL, 0);
        float rx = axis(st.g.rx, g_dzR, 1), ry = axis(st.g.ry, g_dzR, 1);
        if (g_invY) ry = -ry;
        long mdx = 0, mdy = 0;
        if (!menu) {
            /* left stick -> W/A/S/D (8 directions); right stick -> camera (DirectInput mouse deltas) */
            float ax = lx < 0 ? -lx : lx, ay = ly < 0 ? -ly : ly, mag = ax > ay ? ax : ay;
            if (mag > g_moveThr) {
                if (ly > 0.38f * mag) keys[0x11] = 1;
                if (ly < -0.38f * mag) keys[0x1F] = 1;
                if (lx > 0.38f * mag) keys[0x20] = 1;
                if (lx < -0.38f * mag) keys[0x1E] = 1;
            }
            /* right stick -> camera = mouse motion (the game turns the camera by DirectInput mouse deltas) */
            accX += rx * g_camX * dt; accY += -ry * g_camY * dt;
            mdx = (long)accX; mdy = (long)accY; accX -= mdx; accY -= mdy;
            if (mdx || mdy) {
                if (g_mouseOut == 0) send_move(mdx, mdy);
                else { __sync_fetch_and_add(&g_mdx, mdx); __sync_fetch_and_add(&g_mdy, mdy); }
            }
        } else {
            /* left stick -> menu cursor (mouse motion), right stick vertical -> wheel */
            float cx = axis(st.g.lx, g_dzL, 1), cy = axis(st.g.ly, g_dzL, 1);
            /* sticky cursor: right after the game reports hover over a new item, the cursor slows down for a moment,
               so it is easy to stop on buttons (the hover sound arrives a frame or two late; the slow-down covers it) */
            if (g_overSeq != stickySeq) { stickySeq = g_overSeq; stickyUntil = now + (DWORD)g_stickyMs; }
            float sp = g_cursor * ((int)(stickyUntil - now) > 0 ? g_stickyFactor : 1.0f);
            if (cx != 0 || cy != 0) uinav_release();       /* the stick takes over from the d-pad focus */
            if (uinav_active()) {                            /* d-pad: jump to the next menu item (uinav.c) */
                static const DWORD bits[4] = { B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT };
                for (int d = 0; d < 4; d++)
                    if ((cur & bits[d]) && (!(navPrev & bits[d]) || (int)(now - navRepeat) >= 0)) {
                        uinav_request(d); navRepeat = now + ((navPrev & bits[d]) ? 160 : 380);
                    }
            } else {
                /* d-pad: slow, precise cursor movement */
                if (cur & B_DUP) cy += g_dpadSlow; if (cur & B_DDOWN) cy -= g_dpadSlow;
                if (cur & B_DLEFT) cx -= g_dpadSlow; if (cur & B_DRIGHT) cx += g_dpadSlow;
            }
            { long nx, ny; uinav_take_move(&nx, &ny); if ((nx || ny) && g_mouseOut == 0) send_move(nx, ny); }
            navPrev = cur;
            accX += cx * sp * dt; accY += -cy * sp * dt;
            long ix = (long)accX, iy = (long)accY; accX -= ix; accY -= iy;
            if (ix || iy) {
                if (g_mouseOut == 0) send_move(ix, iy);
                else { __sync_fetch_and_add(&g_cdx, ix); __sync_fetch_and_add(&g_cdy, iy); }
            }
            accScroll += ry * g_scroll * dt;
            if (uinav_journal()) {                                     /* the journal ignores the wheel: scroll it directly */
                while (accScroll >= 1) { uinav_scroll(-1); accScroll -= 1; }
                while (accScroll <= -1) { uinav_scroll(1); accScroll += 1; }
            }
            while (accScroll >= 1) { wheel += 120; accScroll -= 1; }
            while (accScroll <= -1) { wheel -= 120; accScroll += 1; }
        }

        if (g_mouseOut == 1 && (!g_origGCP || now - g_gcpTick > 100)) apply_cursor();   /* the game is not polling the cursor */

        /* what the UI receives, every 3 s while the pad is used (for tuning the menu detection) */
        if (g_reports < g_reportMax && now - lastReport >= 3000) {
            int any = st.g.buttons || st.g.lt > 64 || st.g.rt > 64 || mdx || mdy || keys[0x11] || keys[0x1E] || keys[0x1F] || keys[0x20];
            if (any || g_padUsedRecently) {
                char line[700]; int p = wsprintfA(line, "Gamepad: last 3 s: %s mode, game GetCursorPos calls %u;",
                                                  menu ? "menu" : "game", g_gcpCalls - lastGcp);
                for (int i = 0; i < g_uiN && p < 620; i++) {
                    UiStat* s = &g_ui[i];
                    DWORD a = s->n[0], b = s->n[1], c = s->n[2];
                    if (a | b | c) p += wsprintfA(line + p, " %s(%s) notify %u ev %u vp %u;", s->name, s->game ? "hud" : "menu", a, b, c);
                    s->n[0] = s->n[1] = s->n[2] = 0;
                }
                logf("%s", line); g_reports++;
            } else for (int i = 0; i < g_uiN; i++) g_ui[i].n[0] = g_ui[i].n[1] = g_ui[i].n[2] = 0;
            g_padUsedRecently = any;
            lastReport = now; lastGcp = g_gcpCalls;
        }

        /* d-pad in menus: sweep the cursor until the game reports hover over a new item, then step a little into it */
        if (snapHold >= 0 && !(cur & k_bbit[snapHold])) { snapHold = -1; snapNext = 0; }
        if (snapDir >= 0) {
            static const int dx[4] = { 0, 0, -1, 1 }, dy[4] = { -1, 1, 0, 0 };
            int found = g_overSeq != snapSeq, timeout = now - snapStart > (DWORD)g_snapMs;
            long st_;
            if (found) {                                /* the hover sound comes a frame or two late: go back that far, then 10 px in */
                long back = 0;
                for (int i = 0; i < snapHistN; i++) if (now - snapHistT[i] <= (DWORD)g_snapLag) back += snapHist[i];
                st_ = 16 - back;   /* 06.11: 60 ms back + 10 in left the button again */
            } else {
                snapAcc += g_snapSpeed * dt; st_ = (long)snapAcc; snapAcc -= st_;
                if (st_) { if (snapHistN == 64) { for (int i = 1; i < 64; i++) { snapHist[i - 1] = snapHist[i]; snapHistT[i - 1] = snapHistT[i]; } snapHistN--; }
                           snapHist[snapHistN] = st_; snapHistT[snapHistN++] = now; }
            }
            if (st_ && g_mouseOut == 0) send_move(dx[snapDir] * st_, dy[snapDir] * st_);
            if (found || timeout) { snapDir = -1; snapNext = now + 260; }
        }

        for (int i = 0; i < 256; i++) g_padKey[i] = keys[i];
        if (g_mouseOut == 0) {                                         /* mouse buttons and wheel as real input */
            static const DWORD dn[3] = { 2, 8, 0x20 }, up[3] = { 4, 0x10, 0x40 };
            for (int i = 0; i < 3; i++) if (btn[i] != sentBtn[i]) { send_mouse(btn[i] ? dn[i] : up[i], 0); sentBtn[i] = btn[i]; }
            for (int i = 0; i < 8; i++) g_padBtn[i] = 0;
            if (wheel) send_mouse(0x800, (DWORD)wheel);
        } else {
            for (int i = 0; i < 8; i++) g_padBtn[i] = btn[i];
            if (wheel) __sync_fetch_and_add(&g_mdz, wheel);
        }
    }
    return 0;
}

/* ---- prompt icons: key / mouse button -> pad button glyph text (codes drawn by tools\glyphs\build_glyphs.py) ---- */
static const WORD k_glyphPS[NB]   = { 0xE000, 0xE001, 0xE002, 0xE003, 0xE004, 0xE005, 0xE006, 0xE007, 0xE00B, 0xE00A, 0xE008, 0xE009, 0xE00C, 0xE00D, 0xE00E, 0xE00F };
static const WORD k_glyphXbox[NB] = { 0xE020, 0xE021, 0xE022, 0xE023, 0xE024, 0xE025, 0xE026, 0xE027, 0xE02B, 0xE02A, 0xE010, 0xE011, 0xE00C, 0xE00D, 0xE00E, 0xE00F };
int gamepad_glyph_text(WORD action, unsigned short* buf, int cap)
{
    if (!g_on || !g_glyphs || !g_padKind || cap < 4) return 0;
    if (!pad_prompts()) return 0;                                    /* keyboard / mouse used after the pad */
    const WORD* gl = g_padKind == 1 ? k_glyphPS : k_glyphXbox;
    for (int b = 0; b < NB; b++) if (g_map[L_BASE][b] == action) { buf[0] = gl[b]; buf[1] = 0; return 1; }
    for (int l = L_LB; l <= L_RB; l++)
        for (int b = 0; b < NB; b++)
            if (g_map[l][b] == action) { buf[0] = gl[l == L_LB ? I_LB : I_RB]; buf[1] = '+'; buf[2] = gl[b]; buf[3] = 0; return 3; }
    /* the HUD shows the first binding of an action: AM_INTERACT_ITEM = mouse right (and E), AM_ATTACK = mouse left */
    if (action == A_MOUSE + 1) return gamepad_glyph_text(0x12, buf, cap);
    if (action == 0x11 || action == 0x1E || action == 0x1F || action == 0x20) { buf[0] = 0xE010; buf[1] = 0; return 1; }
    return 0;
}

/* Xbox-style skill HUD (uinav.c): which LB/RB face button (0 A, 1 B, 2 X, 3 Y) the key is bound to, and its glyph.
   0 when the pad was not used for 2 minutes (the HUD then shows the PC row). */
int gamepad_hud_binding(WORD action, int* layer, int* face, unsigned short* glyph)
{
    if (!g_on || !pad_prompts()) return 0;
    const WORD* gl = g_padKind == 1 ? k_glyphPS : k_glyphXbox;
    for (int l = L_LB; l <= L_RB; l++)
        for (int b = 0; b < 4; b++)
            if (g_map[l][b] == action) { *layer = l == L_LB ? 0 : 1; *face = b; glyph[0] = gl[l == L_LB ? I_LB : I_RB]; glyph[1] = gl[b]; glyph[2] = 0; return 1; }
    return 0;
}

/* real keyboard / mouse input: low-level hooks see injected events flagged (ours: SendInput from the pad thread) */
__declspec(dllimport) void* WINAPI SetWindowsHookExA(int id, void* proc, void* mod, DWORD tid);
__declspec(dllimport) long WINAPI CallNextHookEx(void* h, int code, UINT w, long l);
__declspec(dllimport) BOOL WINAPI GetMessageA(void* msg, void* hwnd, UINT lo, UINT hi);
static long WINAPI ll_kbd(int code, UINT w, long l)
{
    if (code == 0 && !(((DWORD*)l)[2] & 0x10) && game_focused()) g_lastKbmInput = GetTickCount();   /* KBDLLHOOKSTRUCT.flags: LLKHF_INJECTED */
    return CallNextHookEx(0, code, w, l);
}
static long WINAPI ll_mouse(int code, UINT w, long l)
{
    /* MSLLHOOKSTRUCT { POINT pt; mouseData; flags; time; extra }: flags bit 0 = injected. A resting mouse's tremor is
       ignored: a move counts after 24 px within half a second */
    if (code == 0 && !(((DWORD*)l)[3] & 1) && game_focused()) {
        static long px, py, acc; static DWORD t0;
        long x = ((long*)l)[0], y = ((long*)l)[1]; DWORD now = GetTickCount();
        if (w != 0x200) g_lastKbmInput = now;                         /* buttons, wheel */
        else {
            if (now - t0 > 500) { acc = 0; t0 = now; }
            long dx = x - px, dy = y - py; acc += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (acc > 24) { g_lastKbmInput = now; acc = 0; }
        }
        px = x; py = y;
    }
    return CallNextHookEx(0, code, w, l);
}
static DWORD WINAPI kbm_thread(void* p)
{
    (void)p;
    void* hk = SetWindowsHookExA(13, (void*)ll_kbd, GetModuleHandleA(0), 0);
    void* hm = SetWindowsHookExA(14, (void*)ll_mouse, GetModuleHandleA(0), 0);
    logf("Gamepad: prompts follow the last input device (keyboard hook %s, mouse hook %s)", hk ? "on" : "FAILED", hm ? "on" : "FAILED");
    if (!hk && !hm) return 0;
    struct { void* h; UINT m; UINT w; long l; DWORD t; long x, y; } msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {}
    return 0;
}
static void kbm_start(void)
{
    static volatile long started;
    if (__sync_lock_test_and_set(&started, 1)) return;
    HANDLE t2 = CreateThread(0, 0, kbm_thread, 0, 0, 0); if (t2) CloseHandle(t2);
}

void gamepad_init(Module m)
{
    g_on = GetPrivateProfileIntA("Gamepad", "Enable", 1, g_ini);
    if (!g_on) { logf("Gamepad: disabled in ini"); return; }
    load_settings();
    /* the game's GetCursorPos import (IAT slot 0x12B7A34, called from 0x8EB34B, 0x99EA99 and the thunk 0x1025BCA) */
    void** slot = (void**)(m.base + (0x12B7A34 - 0x400000));
    void* real = GetProcAddress(GetModuleHandleA("user32.dll"), "GetCursorPos");
    if (g_mouseOut == 1 && readable(slot, 4) && real && *slot == real) {
        g_origGCP = (GCP_t)real; void* h = (void*)hk_GetCursorPos;
        if (!write_mem(slot, &h, 4)) g_origGCP = 0;
    }
    if (g_mouseOut == 1)
        logf("Gamepad: GetCursorPos import %s", g_origGCP ? "hooked (cursor moves on the game thread)" : "not found -> cursor moved from the pad thread");
    {   /* menu hover events (0x99F1C0): push ebx; push ebp; mov ebp,[esp+0Ch]; push esi; push "soundEvent" */
        BYTE* f = m.base + (0x99F1C0 - 0x400000);
        static const BYTE pro[] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x68 };
        int same = readable(f, 8); for (int i = 0; same && i < 8; i++) if (f[i] != pro[i]) same = 0;
        if (same) g_origMenuCmd = (MenuCmd_t)detour(f, (void*)hk_MenuCmd);
        logf("Gamepad: menu hover events %s", g_origMenuCmd ? "hooked (d-pad jumps between menu items)" : "NOT hooked (code bytes differ) -> d-pad only moves");
    }
    HANDLE th = CreateThread(0, 0, pad_thread, 0, 0, 0);
    if (!th) { logf("Gamepad: thread not started -> gamepad off"); g_on = 0; return; }
    CloseHandle(th);
    logf("Gamepad: on (mode %s, menu hold %d ms, camera %d/%d, mouse output %s)",
         g_mode == 1 ? "always game" : g_mode == 2 ? "always menu" : "auto", g_hold, (int)g_camX, (int)g_camY,
         g_mouseOut == 0 ? "SendInput (real mouse input)" : "DirectInput injection");
}
