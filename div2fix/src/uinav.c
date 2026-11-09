/* Div2Fix - menu navigation with the d-pad without free cursor movement (docs/XBOX_UI.md).
 *
 * The game fills its Scaleform menus with logical data through GFxMovieRoot::Invoke (vtable slot 18:
 * Invoke(method, GFxValue* result, const GFxValue* args, count), found by the UI probe of 06.11), e.g.
 * addItemToMenu(id, label, enabled). The movie's ActionScript turns the ids into clips (main menu:
 * main_mc.buttons_mc.button<id>_mc; pause menu: overview_mc.tabs_mc.tab<id>_mc). We remember the ids per movie,
 * read the clips' positions and the movie's mouse position with GetVariable (slot 12, GFx 2.x order: SetVariable 11,
 * GetVariable 12, ..., Invoke 18) on the game thread (from SetViewport, which runs every frame for movies on
 * screen), and drive the cursor onto the chosen item with closed-loop relative mouse motion (gamepad.c sends it).
 * Hover highlight and clicks stay the game's own. Movies without a table keep the normal cursor.
 * Everything here runs on the game thread except uinav_request / uinav_take_move (pad thread, atomics).
 */
#include "common.h"

typedef struct { DWORD type, pad; union { double n; const char* s; BYTE b; } v; } GVal;      /* GFxValue, 16 bytes */
typedef int (__fastcall *GetVar_t)(void* root, void* edx, GVal* out, const char* path);
typedef int (__fastcall *Invoke_t)(void* root, void* edx, const char* method, GVal* result, const GVal* args, UINT n);
static GetVar_t g_getVar;
static Invoke_t g_origInvoke;

/* ---- per-menu tables ----
   Each source: an Invoke the game makes to add an item (method, which argument holds the id) and the clip the
   movie creates for it (paths from the movies' ActionScript, gfx_avm1.py). native = the movie has its own console
   cursor (moveCursor/executeSelected, left over from the Xbox version): we call that instead of steering the mouse. */
typedef struct { const char* method; int idArg; const char* path; const char* mask; int left; } NavSrc;   /* left: aim at the start of a text line, not its middle */   /* mask: the list clip that scrolls these items */
typedef struct { const char* movie; NavSrc src[6]; const char* mask; int native; } NavTable;
#define OC "options_mc.content_mc.container_mc."
#define DQ "dialog_mc.frames_mc.bottom_mc."
/* a source with method 0 is a fixed clip set: ids 0..idArg-1 (idArg 1 and no %d: a single clip) */
static const NavTable k_tables[] = {
    { "mainmenu",     { { "addItemToMenu", 0, "main_mc.buttons_mc.button%d_mc" } }, 0, 0 },
    { "menuoverview", { { "addItemToMenu", 0, "overview_mc.tabs_mc.tab%d_mc" } }, 0, 0 },
    { "save",         { { "addSave", 0, "save_mc.container_mc.item%d_mc" } }, 0, 1 },
    { "options",      { { "addTab", 0, "options_mc.tabs_mc.tab%d_mc.hotspot_mc" },
                        { "addSlider", 1, OC "slider%d_mc.handle_mc" },
                        { "addCheckBox", 1, OC "checkBox%d_mc.hotspot_mc" },
                        { "addControl", 1, OC "control%d_mc.btnPrimary_mc" },
                        { "addDropDownList", 1, OC "dropdownbox%d_mc.dddButton_mc" } },
      "options_mc.content_mc.mask_mc", 0 },
    /* dialog.gfx: answers attached as question<id>_mc by SetQuestion(id, text); trade / mind reading buttons are fixed */
    { "dialog",       { { "SetQuestion", 0, DQ "questionsHolder_mc.container_mc.question%d_mc" },
                        { 0, 1, DQ "buttonHolder_mc.btnTrade_mc" },
                        { 0, 1, DQ "buttonHolder_mc.btnMindread_mc" } },
      DQ "questionsHolder_mc.mask_mc", 0 },
    /* inventory.gfx: 6 tab buttons, item slots slot<id>_mc placed by placeSlot(id) in a scrolled list */
    { "inventory",    { { 0, 6, "playerInfo_mc.inventory_mc.tabs_mc.tab%d_mc.hotspot_mc" },
                        { 0, 120, "playerInfo_mc.inventory_mc.chInventory_mc.container_mc.slot%d_mc" } },
      "playerInfo_mc.inventory_mc.chInventory_mc.mask_mc", 0 },
    /* equipment.gfx: body slots render_mc.slot<n>_mc (setSlotIcon ids 0..13), weapon sets weaponsets_mc.set<n>_mc */
    { "equipment",    { { 0, 14, "playerInfo_mc.render_mc.slot%d_mc.hotspot_mc" },
                        { 0, 3, "playerInfo_mc.weaponsets_mc.set%d_mc.front_mc.hotspot_mc" } }, 0, 0 },   /* the set clip is wider than its slot */
    /* levelUp.gfx: character sheet rows stat0..16_mc (experience, health, mana, attributes, resistances), accept/cancel */
    { "levelup",      { { "setStat", 0, "levelup_mc.stats_mc.stat%d_mc.label_txt", 0, 1 },   /* rows the game fills (setStat id): stat15/16 stay empty; aim at the label */
                        { 0, 1, "levelup_mc.stats_mc.buttons_mc.btnAccept_mc" },
                        { 0, 1, "levelup_mc.stats_mc.buttons_mc.btnCancel_mc" } }, 0, 0 },
    /* popup.gfx: buttons button<n>_mc made by addButton(label) in order */
    { "popup",        { { 0, 4, "popup_mc.button%d_mc" } }, 0, 0 },
    /* trade.gfx: both sides are the same inventory sprite: 6-7 tabs and slot<n>_mc in a scrolled frame */
#define TP "trade_mc.player_mc."
#define TN "trade_mc.npc_mc."
    { "trade",        { { 0, 7, TP "tabs_mc.tab%d_mc.hotspot_mc" },
                        { 0, 120, TP "inventory_mc.frame_mc.container_mc.slot%d_mc", TP "inventory_mc.frame_mc.mask_mc" },
                        { 0, 7, TN "tabs_mc.tab%d_mc.hotspot_mc" },
                        { 0, 120, TN "inventory_mc.frame_mc.container_mc.slot%d_mc", TN "inventory_mc.frame_mc.mask_mc" } }, 0, 0 },
    /* questLog.gfx: diary tabs, quests quest<id>_mc from addQuest(id, ...) (sub-quests sit inside their quest: not yet) */
    { "questlog",     { { 0, 6, "diary_mc.tabs_mc.tab%d_mc.hotspot_mc" },
                        { "addQuest", 0, "diary_mc.frame_mc.container_mc.quest%d_mc.name_txt", "diary_mc.frame_mc.mask_mc", 1 },
                        /* sub-quests: quest<parent>_mc.cSubQuest_mc.subQuest<id>_mc, parent found by its name (addSubQuest arg 6) */
                        { "addSubQuest", 0, "diary_mc.frame_mc.container_mc.quest%d_mc.cSubQuest_mc.subQuest%d_mc.name_txt", "diary_mc.frame_mc.mask_mc", 1 } }, 0, 0 },
    /* waypointShrine.gfx: teleporters shrine<id>_mc from addTeleporter(id, ...) */
    { "waypointshrine", { { "addTeleporter", 0, "shrine_mc.shrines_mc.container_mc.shrine%d_mc", "shrine_mc.shrines_mc.mask_mc" } }, 0, 0 },
    /* skillTree.gfx: tabs cTabs_mc.tab<id>_mc (addTab), skills attached as skill<N>_mc where N counts addSkill calls
       since removeSkills (the movie's skillAmount), inside the container of their tree (addSkill arg 1). Skills of
       the Mage Expansion come through the same addSkill, so they are covered. */
#define SK "skillTree_mc.skills_mc."
    { "skilltree",    { { "addTab", 0, SK "cTabs_mc.tab%d_mc" },
                        { "addSkill", 1, SK "contentHolder_mc.container_mc.skillsContainer%d_mc.skillsContainer_mc.skill%d_mc.hotspot_mc", SK "contentHolder_mc.mask_mc" },
                        { 0, 1, SK "buttons_mc.btnAccept_mc" },
                        { 0, 1, SK "buttons_mc.btnCancel_mc" } }, 0, 0 },
    /* chest.gfx: the chest's contents fill 54 fixed slots (addItems -> slots of inventory_mc), plus the OK button */
    { "chest",        { { 0, 54, "chest_mc.inventory_mc.slot%d_mc" },
                        { 0, 1, "chest_mc.btnOK_mc" } }, 0, 0 },
};
#define NT (int)(sizeof k_tables / sizeof k_tables[0])
#define MAXI 280

typedef struct { void* root; int table; int ids[MAXI]; BYTE src[MAXI]; int nIds; DWORD logged; DWORD tick; double lmx, lmy; } NavMovie;
static int g_logN, g_traceN;
static NavMovie g_nm[16]; static int g_nmN;
static DWORD g_qHash[96]; static int g_qId[96], g_qN;   /* journal: quest names -> ids, to place sub-quests */
static int g_skillN;                                    /* skill tree: addSkill calls since removeSkills */
/* the game passes the quest names as wide strings (type 5), sometimes as char strings (type 4) */
static DWORD str_hash(const GVal* v)
{
    DWORD h = 2166136261u;
    if (v->type == 4 && v->v.s) { for (const unsigned char* p = (const unsigned char*)v->v.s; *p; p++) h = (h ^ *p) * 16777619u; return h; }
    if (v->type == 5 && v->v.s) { for (const unsigned short* p = (const unsigned short*)v->v.s; *p; p++) h = (h ^ *p) * 16777619u; return h; }
    return 0;
}
static void item_path(char* out, const char* fmt, int id)
{
    int two = 0; for (const char* p = fmt; *p; p++) if (p[0] == '%' && p[1] == 'd') two++;
    if (two == 2) wsprintfA(out, fmt, id >> 12, id & 0xFFF); else wsprintfA(out, fmt, id);
}

static int name_eq(const char* a, const char* b) { while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; } return !*a && !*b; }

static NavMovie* nav_movie(void* root, const char* name, int create)
{
    for (int i = 0; i < g_nmN; i++)
        if (g_nm[i].root == root && (!name || name_eq(name, k_tables[g_nm[i].table].movie))) return &g_nm[i];
    /* the game can show two movies under one root (pause menu + journal): an entry is matched by root and name,
       and one of another movie on this root is forgotten only once it is no longer drawn */
    for (int i = 0; i < g_nmN; i++)
        if (g_nm[i].root == root && GetTickCount() - g_nm[i].tick >= 1000 && g_nm[i].tick) g_nm[i].root = 0;
    if (!create || !name) return 0;
    for (int t = 0; t < NT; t++)
        if (name_eq(name, k_tables[t].movie)) {
            NavMovie* m = 0;
            for (int i = 0; i < g_nmN; i++) if (!g_nm[i].root) { m = &g_nm[i]; break; }
            if (!m && g_nmN < 16) m = &g_nm[g_nmN++];
            if (!m) { m = &g_nm[0]; for (int i = 1; i < g_nmN; i++) if (g_nm[i].tick < m->tick) m = &g_nm[i]; }   /* full: the one drawn longest ago */
            if (name_eq(name, "questlog")) g_qN = 0;
            if (name_eq(name, "skilltree")) g_skillN = 0;
            memset(m, 0, sizeof *m); m->root = root; m->table = t; g_logN = 0; g_traceN = 0;   /* fresh log lines for each menu */
            for (int k = 0; k < 6 && k_tables[t].src[k].path; k++)
                if (!k_tables[t].src[k].method)
                    for (int id = 0; id < k_tables[t].src[k].idArg && m->nIds < MAXI; id++) { m->ids[m->nIds] = id; m->src[m->nIds] = (BYTE)k; m->nIds++; }
            return m;
        }
    return 0;
}

/* called from the slot-18 hook: remember the item ids the game adds */
void uinav_on_invoke(void* root, const char* movieName, const char* method, const GVal* args, UINT n)
{
    NavMovie* m = nav_movie(root, movieName, 1);
    if (m && method && name_eq(method, "removeSkills")) {   /* the tree is rebuilt: its skill clips are gone */
        g_skillN = 0; int k = 0;
        for (int i = 0; i < m->nIds; i++) if (!k_tables[m->table].src[m->src[i]].method || !name_eq(k_tables[m->table].src[m->src[i]].method, "addSkill")) { m->ids[k] = m->ids[i]; m->src[k] = m->src[i]; k++; }
        m->nIds = k; return;
    }
    if (!m || !method || !n || !args) return;
    const NavTable* t = &k_tables[m->table];
    for (int s = 0; s < 6 && t->src[s].path; s++) {
        if (!t->src[s].method || !name_eq(method, t->src[s].method) || (UINT)t->src[s].idArg >= n || args[t->src[s].idArg].type != 3) continue;
        int id = (int)args[t->src[s].idArg].v.n;
        if (name_eq(method, "addQuest") && n > 1 && str_hash(&args[1])) {
            DWORD hh = str_hash(&args[1]); int q = 0;
            while (q < g_qN && g_qId[q] != id) q++;
            if (q == g_qN) { if (g_qN >= 96) q = g_qN = 0; g_qN++; }
            g_qHash[q] = hh; g_qId[q] = id;
        }
        if (name_eq(method, "addSubQuest")) {
            if (n <= 6 || !str_hash(&args[6])) return;
            DWORD hh = str_hash(&args[6]); int pid = -1;
            for (int q = 0; q < g_qN; q++) if (g_qHash[q] == hh) pid = g_qId[q];
            if (pid < 0 || pid > 0xFFFF || id < 0 || id > 0xFFF) return;
            id = pid << 12 | id;
        }
        if (name_eq(method, "addSkill")) {               /* id = tree, the clip number is the call count */
            if (id < 0 || id > 0xFFFF) return;
            id = id << 12 | (g_skillN++ & 0xFFF);
        }
        for (int i = 0; i < m->nIds; i++) if (m->ids[i] == id && m->src[i] == s) return;
        if (m->nIds < MAXI) { m->ids[m->nIds] = id; m->src[m->nIds] = (BYTE)s; m->nIds++; }
        return;
    }
}

/* ---- reading the movie ---- */
static int get_num(void* root, const char* path, double* out)
{
    GVal v; memset(&v, 0, sizeof v);
    if (!g_getVar || !g_getVar(root, 0, &v, path)) return 0;
    if (v.type != 3) return 0;
    *out = v.v.n; return 1;
}

/* 1 unless the variable reads as boolean false (hidden clip, e.g. an item of another tab) */
static int get_vis(void* root, const char* path)
{
    GVal v; memset(&v, 0, sizeof v);
    if (!g_getVar || !g_getVar(root, 0, &v, path)) return 1;
    return !(v.type == 2 && !v.v.b);
}

/* position of a clip in _root coordinates: sum of _x/_y along the path, scaled by the parents' _xscale/_yscale */
static int clip_pos(void* root, const char* path, double* x, double* y, double* w, double* h)
{
    char part[160]; int n = 0; double ax = 0, ay = 0, sx = 1, sy = 1;
    for (int i = 0;; i++) {
        char c = path[i];
        if (c == '.' || !c) {
            part[n] = 0;
            char q[180]; double px, py, pxs = 100, pys = 100;
            wsprintfA(q, "%s._x", part); if (!get_num(root, q, &px)) return 0;
            wsprintfA(q, "%s._y", part); if (!get_num(root, q, &py)) return 0;
            wsprintfA(q, "%s._visible", part); if (!get_vis(root, q)) return 0;
            ax += px * sx; ay += py * sy;
            if (!c) {
                wsprintfA(q, "%s._width", part); if (!get_num(root, q, w)) *w = 0;
                wsprintfA(q, "%s._height", part); if (!get_num(root, q, h)) *h = 0;
                *w *= sx; *h *= sy;
                *x = ax; *y = ay; return 1;
            }
            wsprintfA(q, "%s._xscale", part); if (get_num(root, q, &pxs)) sx *= pxs / 100;
            wsprintfA(q, "%s._yscale", part); if (get_num(root, q, &pys)) sy *= pys / 100;
        }
        if (n < 158) part[n++] = c;
        if (!c) break;
    }
    return 0;
}

/* ---- navigation state ---- */
static volatile long g_req = -1;          /* direction asked by the pad thread: 0 up, 1 down, 2 left, 3 right */
static volatile long g_moveX, g_moveY;    /* motion for the pad thread to send (screen pixels) */
static volatile DWORD g_activeTick;       /* a movie with a table is on screen */
static double g_tx, g_ty; static int g_hasTarget; static void* g_targetRoot;
static double g_gain = 0.5;               /* screen pixels per movie unit, learned from the cursor's response */
static double g_startMx, g_startMy; static long g_sumSX, g_sumSY; static int g_learned;
static int g_focusId = -1; static void* g_focusRoot; static double g_fx, g_fy;
static long g_pendX, g_pendY, g_stepSX, g_stepSY; static double g_stepMx, g_stepMy;
static int g_steps, g_waiting, g_still; static DWORD g_waitUntil; static double g_prevMx, g_prevMy, g_sendMx, g_sendMy;

static void* volatile g_owner;      /* the menu that takes the d-pad when several are on screen */
static void* volatile g_nativeRoot; static volatile DWORD g_nativeTick; static volatile long g_nativeUsed, g_reqExec;
/* pad thread, cross pressed in a menu: 1 = taken as "press the selected item" of a movie with its own cursor */
int uinav_accept(void)
{
    if (GetTickCount() - g_nativeTick >= 300 || !g_nativeUsed || (g_owner && g_owner != g_nativeRoot)) return 0;   /* a popup over it owns the pad */
    __sync_lock_test_and_set(&g_reqExec, 1); return 1;
}
int uinav_active(void) { return GetTickCount() - g_activeTick < 300; }
static void* g_gRoot; static double g_gx, g_gy;
static volatile long g_tried;   /* movies (bit = index in g_nm) that already had this request */
static volatile DWORD g_reqTick; static DWORD g_setTick; static int g_arrivedLogged;
void uinav_request(int dir) { g_reqTick = GetTickCount(); g_tried = 0; __sync_lock_test_and_set(&g_req, dir); }

/* Several menus can be on screen at once (character menu: tabs + inventory + equipment), each a movie with its own
   coordinates. One movie owns the d-pad (the content windows before the tab bar); when it has nothing further in the
   pressed direction it hands the request to another menu on screen, which starts from the cursor in its own coordinates. */
/* the mask clips only the items that sit next to it (same parent), not the tabs or buttons of the window */
static int in_list(const char* mask, const char* path)
{
    int n = s_len(mask); while (n > 0 && mask[n - 1] != '.') n--;
    for (int i = 0; i < n; i++) if (mask[i] != path[i]) return 0;
    return 1;
}
static int movie_prio(const NavMovie* m) { const char* n = k_tables[m->table].movie; return name_eq(n, "popup") ? 2 : name_eq(n, "menuoverview") ? 0 : 1; }
static int alive(const NavMovie* m) { return m->root && GetTickCount() - m->tick < 300; }
/* the first menu drawn after a press takes it; while a popup is on screen only the popup does */
static int g_targetBlind;
static volatile long g_jScroll; static int g_jLogN;            /* journal scroll steps asked by the pad thread (+ down, - up) */
void uinav_scroll(int steps) { __sync_fetch_and_add(&g_jScroll, steps); }
int uinav_journal(void) { for (int i = 0; i < g_nmN; i++) if (alive(&g_nm[i]) && name_eq(k_tables[g_nm[i].table].movie, "questlog")) return 1; return 0; }
static long take_req(NavMovie* m)
{
    long d = g_req;
    if (d < 0) return -1;
    if (movie_prio(m) < 2)
        for (int i = 0; i < g_nmN; i++) if (alive(&g_nm[i]) && movie_prio(&g_nm[i]) == 2) return -1;
    return __sync_val_compare_and_swap(&g_req, d, -1) == d ? d : -1;
}
void uinav_take_move(long* dx, long* dy) { *dx = __sync_lock_test_and_set(&g_moveX, 0); *dy = __sync_lock_test_and_set(&g_moveY, 0); }

static long g_probeDir = -1, g_probeDone; static double g_probeX, g_probeY, g_probeBest, g_pcx, g_pcy; static void* g_probeRoot; static int g_pkey; static DWORD g_probeTick; static const char* g_probeName;
static void set_target(void* root, double cx, double cy, int key, double mx, double my)
{
    g_setTick = GetTickCount(); g_arrivedLogged = 0;
    if (g_logN < 200) { g_logN++; logf("UINav: target (%d,%d) chosen %u ms after the press", (int)cx, (int)cy, (unsigned)(g_setTick - g_reqTick)); }
    g_focusId = key; g_focusRoot = root; g_fx = cx; g_fy = cy; g_tx = cx; g_ty = cy; g_hasTarget = 1; g_targetRoot = root;
    g_startMx = mx; g_startMy = my; g_sumSX = g_sumSY = 0; g_learned = 0; g_steps = 0; g_waiting = 0; g_pendX = g_pendY = 0;
}
/* the item of this movie nearest in the pressed direction from (bx,by); returns its score, 1e18 if none */
static double best_item(NavMovie* m, void* root, long dir, double bx, double by, double* ocx, double* ocy, int* okey)
{
    const NavTable* t = &k_tables[m->table];
    double best = 1e18;
    double kx = 0, ky = -1e9, kw = 0, kh = 2e9;
    if (t->mask && clip_pos(root, t->mask, &kx, &ky, &kw, &kh) && kh <= 0) { ky = -1e9; kh = 2e9; }
    int maskSrc = -1, maskOk = 0; double mY = 0, mH = 0;
    for (int i = 0; i < m->nIds; i++) {
        char path[160]; double x, y, w, h;
        item_path(path, t->src[m->src[i]].path, m->ids[i]);
        if (!clip_pos(root, path, &x, &y, &w, &h)) continue;
        if (x + w / 2 < 0 || y + h / 2 < 0) continue;          /* parked off the stage (mind reading button when unavailable) */
        if (!(m->logged & (1u << m->src[i]))) { m->logged |= 1u << m->src[i]; logf("UINav: %s items at '%s'", t->movie, t->src[m->src[i]].path); }
        double ix = x + w / 2, iy = y + h / 2;
        if (t->src[m->src[i]].left && w > 80) ix = x + 40;      /* quest titles: the start of the name */
        if (t->mask && in_list(t->mask, path) && (iy < ky || iy > ky + kh)) continue;   /* scrolled out of the list's mask */
        const char* sm = t->src[m->src[i]].mask;
        if (sm) {
            double sx0, sy0, sw0, sh0;
            if (m->src[i] != maskSrc) { maskSrc = m->src[i]; maskOk = clip_pos(root, sm, &sx0, &sy0, &sw0, &sh0) && sh0 > 0; mY = sy0; mH = sh0; }
            if (maskOk && (iy < mY || iy > mY + mH)) continue;
        }
        int key = m->ids[i] * 8 + m->src[i];
        if (g_focusRoot == root && key == g_focusId) continue;
        double dx = ix - bx, dy = iy - by;
        double along = dir == 0 ? -dy : dir == 1 ? dy : dir == 2 ? -dx : dx;
        double perp = dir <= 1 ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
        if (along <= 2 || perp > 3 * along) continue;          /* mostly sideways: not in the pressed direction */
        double score = along + 2 * perp;
        if (score < best) { best = score; *ocx = ix; *ocy = iy; *okey = key; }
    }
    return best;
}

/* ---- Xbox-style skill HUD: the 8 skill slots of hudSlots.gfx as two crosses (LB and RB), pad glyphs on them ----
   Done at run time on the game's own movie (nothing in the archives changes): GFxMovieRoot::SetVariable (vtable
   slot 11, found by its code in the exe: thiscall, 3 arguments, path checked first - checked again at run time by
   writing and reading back a value before anything is moved). [Gamepad] XboxHud=1, XboxHudSpread = distance of each
   cross from the middle of the original row, in slot widths. */
typedef int (__fastcall *SetVar_t)(void* root, void* edx, const char* path, const GVal* v, int type);
static SetVar_t g_setVar; static int g_xhudOn, g_xhudOk = -1; static double g_xhudSpread = 6.5, g_xhudScale = 1.0, g_xhudLift = 1.4;
static int set_num(void* root, const char* path, double v) { GVal g; memset(&g, 0, sizeof g); g.type = 3; g.v.n = v; return g_setVar(root, 0, path, &g, 0); }
static int set_wstr(void* root, const char* path, const unsigned short* w) { GVal g; memset(&g, 0, sizeof g); g.type = 5; g.v.s = (const char*)w; return g_setVar(root, 0, path, &g, 0); }
#define XS "hudSlots_mc.slots_mc.slot%d_mc"
/* the empty sockets: hud.gfx (health bar) is drawn over hudSlots and its sockets are holes, so the cover goes into hud */
static volatile int g_coverWant; static double g_cov[4]; static int g_covOk;
static void xhud_cover(void* root)
{
    static void* drawnRoot; static int drawn = -1;
    if (drawnRoot != root) { drawnRoot = root; drawn = -1; }
    int want = g_coverWant && g_covOk;
    if (want == drawn) return;
    drawn = want;
    if (!want) { set_num(root, "hudSlots_mc.xhudCover_mc._visible", 0); return; }
    GVal a[4], res; memset(a, 0, sizeof a); memset(&res, 0, sizeof res);
    a[0].type = 4; a[0].v.s = "xhudCover_mc"; a[1].type = 3; a[1].v.n = 1048000;
    int ok = g_origInvoke(root, 0, "hudSlots_mc.createEmptyMovieClip", &res, a, 2);
    char m[64];
    #define CC(name, n) (wsprintfA(m, "hudSlots_mc.xhudCover_mc.%s", name), g_origInvoke(root, 0, m, &res, a, n))
    #define CN(k, val) (a[k].type = 3, a[k].v.n = (val))
    CC("clear", 0);
    double x0 = g_cov[0], y0 = g_cov[1], x1 = g_cov[2], y1 = g_cov[3];
    CN(0, 2); CN(1, 0x6E5631); CN(2, 100); CC("lineStyle", 3);
    CN(0, 0x15120F); CN(1, 100); CC("beginFill", 2);
    CN(0, x0); CN(1, y0); CC("moveTo", 2);
    CN(0, x1); CN(1, y0); CC("lineTo", 2);
    CN(0, x1); CN(1, y1); CC("lineTo", 2);
    CN(0, x0); CN(1, y1); CC("lineTo", 2);
    CN(0, x0); CN(1, y0); CC("lineTo", 2);
    CC("endFill", 0);
    #undef CC
    #undef CN
    set_num(root, "hudSlots_mc.xhudCover_mc._visible", 1);
    double w = -1; get_num(root, "hudSlots_mc.xhudCover_mc._width", &w);
    logf("XboxHud: socket cover %s at (%d,%d)-(%d,%d), width %d", ok && w > 10 ? "drawn" : "NOT drawn", (int)x0, (int)y0, (int)x1, (int)y1, (int)w);
}
static void xhud_frame(void* root, const char* movieName)
{
    if (!g_xhudOn || !g_setVar || !g_getVar || !name_eq(movieName, "hudslots")) return;
    static void* lastRoot; static double ox[8], oy[8], cell; static int have, applied = -1; static DWORD next;
    if (root != lastRoot) { lastRoot = root; have = 0; applied = -1; }
    char q[96];
    if (!have) {
        static int failLogged;
        for (int i = 0; i < 8; i++) {                     /* the movie has slot0..7 only (slot8/9 are named in its script) */
            int ok = 1;
            wsprintfA(q, XS "._x", i); if (!get_num(root, q, &ox[i])) ok = 0;
            if (ok) { wsprintfA(q, XS "._y", i); if (!get_num(root, q, &oy[i])) ok = 0; }
            if (!ok) { if (!failLogged) { failLogged = 1; logf("XboxHud: cannot read '%s'", q); } return; }
        }
        cell = ox[1] - ox[0]; if (cell < 0) cell = -cell;
        { double w; wsprintfA(q, XS "._width", 0); if (get_num(root, q, &w) && w > cell) cell = w; }
        if (cell < 8) { logf("XboxHud: slot width %d looks wrong -> off", (int)cell); g_xhudOn = 0; return; }
        have = 1;
    }
    if (g_xhudOk < 0) {                       /* check the SetVariable slot: move slot 7 by 7 units, read it back, put it back */
        double back = -1; wsprintfA(q, XS "._x", 7);
        int w = set_num(root, q, ox[7] + 7);
        g_xhudOk = w && get_num(root, q, &back) && back > ox[7] + 6.5 && back < ox[7] + 7.5;
        set_num(root, q, ox[7]);
        logf("XboxHud: SetVariable check %s (wrote %d, read back %d), slot width %d", g_xhudOk ? "OK" : "FAILED -> off", (int)(ox[7] + 7), (int)back, (int)cell);
        if (!g_xhudOk) { g_xhudOn = 0; return; }
    }
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0) return;
    next = now + 500;
    int layer[8], face[8], any = 0; unsigned short gl[8][4];
    for (int i = 0; i < 8; i++) { layer[i] = -1; if (gamepad_hud_binding((WORD)(0x02 + i), &layer[i], &face[i], gl[i])) any = 1; else layer[i] = -1; }
    /* the row stays where the game puts it (07.11, user: crosses rejected); only the labels change: the pad buttons
       of each slot (LB/RB + face button) instead of the digits 1..8 */
    for (int i = 0; i < 8; i++) {
        unsigned short num[3] = { (unsigned short)('1' + i), 0, 0 };
        wsprintfA(q, XS ".number_txt.text", i); set_wstr(root, q, any && layer[i] >= 0 ? gl[i] : num);
    }
    (void)face; (void)xhud_cover;
    if (applied != any) { applied = any; logf("XboxHud: %s", any ? "pad button labels" : "digit labels"); }
}

/* every frame for each movie on screen (div2fix.c, SetViewport hook) */
void uinav_frame(void* root, const char* movieName)
{
    xhud_frame(root, movieName);
    NavMovie* m = nav_movie(root, movieName, 1);
    if (!m || !m->nIds || !g_getVar) return;
    const NavTable* t = &k_tables[m->table];
    double mx, my;
    if (!get_num(root, "_root._xmouse", &mx) || !get_num(root, "_root._ymouse", &my)) return;
    g_activeTick = GetTickCount(); m->tick = g_activeTick;
    /* journal: the game does not pass the mouse wheel to this movie, so press the scrollbar arrows of the open list
       (their onPress moves the list one step, the same code the wheel listener would run) */
    if (name_eq(t->movie, "questlog") && g_jScroll) {
        long st = __sync_lock_test_and_set(&g_jScroll, 0);
        static const char* const bars[2] = { "diary_mc.frame_mc", "diary_mc.achievements_mc.cAchievements_mc" };
        for (int k = 0; k < 2; k++) {
            char q[120]; wsprintfA(q, "%s._visible", bars[k]); if (!get_vis(root, q)) continue;
            wsprintfA(q, "%s.scrollbar_mc.%s.onPress", bars[k], st > 0 ? "arrowDown_mc" : "arrowUp_mc");
            for (long i = 0; i < (st > 0 ? st : -st) && i < 8; i++) {
                GVal res; memset(&res, 0, sizeof res);
                int ok = g_origInvoke(root, 0, q, &res, 0, 0);
                if (g_logN < 200 && i == 0 && ++g_jLogN <= 4) { g_logN++; logf("UINav: journal scroll %s -> %s", q, ok ? "ok" : "failed"); }
            }
        }
    }
    /* some movies (equipment) report the cursor as the stage's top-left corner (-631,0 at 16:9) on frames where they
       have no fresh mouse position: such a reading is not where the cursor is, wait for the next frame */
    /* (-631,0) from equipment and a constant (0,0) from the character sheet (levelup) are never the cursor: steering
       on them threw it across the screen. A movie the cursor is not over keeps the position where the cursor left it.
       The menus of one screen share the stage coordinates, so the cursor is where a movie last saw it move. */
    int own = !(my == 0 && mx <= 0), noMouse = 0;
    if (own && (mx != m->lmx || my != m->lmy)) { m->lmx = mx; m->lmy = my; g_gx = mx; g_gy = my; g_gRoot = root; }
    else {
        int ok = 0; for (int i = 0; i < g_nmN; i++) if (g_gRoot && g_gRoot != root && g_nm[i].root == g_gRoot && alive(&g_nm[i])) ok = 1;
        if (ok) { mx = g_gx; my = g_gy; }
        /* no other menu sees the cursor: (0,0) is also where a freshly opened menu (main menu at start, journal)
           reports it before the mouse first moves, so steer from there - skipping it left the d-pad dead */
    }

    /* learn the gain over a whole move (input reaches the movie a frame or two late, so per-frame ratios lie):
       total motion sent since the target was set / distance the cursor actually travelled */
    if (g_hasTarget && g_targetRoot == root && (g_sumSX || g_sumSY)) {
        double ex0 = g_tx - mx, ey0 = g_ty - my;
        if (ex0 * ex0 + ey0 * ey0 < 4 && !g_learned) {
            double dmx = mx - g_startMx, dmy = my - g_startMy, moved = dmx * dmx + dmy * dmy;
            double sent = (double)g_sumSX * g_sumSX + (double)g_sumSY * g_sumSY;
            if (moved > 400 && sent > 400) {
                double r = sent / moved, gnew = 1; for (int k = 0; k < 20; k++) gnew = 0.5 * (gnew + r / gnew);
                g_gain = 0.5 * g_gain + 0.5 * gnew;
                if (g_gain < 0.3) g_gain = 0.3; if (g_gain > 4) g_gain = 4;
            }
            g_learned = 1;
        }
    }

    if (t->native) {
        /* the movie's own console cursor. save.gfx: moveCursor('up') selects selectedID+1, the next save down the
           list, 'down' the one above (it also scrolls the list); executeSelected presses the selected save */
        g_nativeTick = GetTickCount(); g_nativeRoot = root;
        long d = take_req(m);
        if (d == 0 || d == 1) {
            GVal arg, res; memset(&arg, 0, sizeof arg); memset(&res, 0, sizeof res);
            arg.type = 4; arg.v.s = d == 1 ? "up" : "down";
            g_origInvoke(root, 0, "moveCursor", &res, &arg, 1);
            g_nativeUsed = 1;
            if (g_logN < 30) { g_logN++; logf("UINav: %s moveCursor('%s')", movieName, arg.v.s); }
        }
        if (__sync_lock_test_and_set(&g_reqExec, 0)) {
            GVal res; memset(&res, 0, sizeof res);
            g_origInvoke(root, 0, "executeSelected", &res, 0, 0);
            if (g_logN < 30) { g_logN++; logf("UINav: %s executeSelected()", movieName); }
        }
        return;
    }
    long dir = take_req(m);
    if (dir >= 0) {
        /* start from the focused item (its own centre may shift a little while it is highlighted), else from the cursor */
        double bx = mx, by = my; int haveFocus = g_focusId >= 0;
        if (haveFocus) { bx = g_fx; by = g_fy; }
        double cx, cy; int key;
        double sc = best_item(m, root, dir, bx, by, &cx, &cy, &key);
        if (g_logN < 200) { g_logN++; logf("UINav: %s dir %d from (%d,%d): %s (%d,%d), %d items, gain %d%%", movieName, (int)dir, (int)bx, (int)by,
                                          sc < 1e17 ? "item" : "nothing", (int)(sc < 1e17 ? cx : 0), (int)(sc < 1e17 ? cy : 0), m->nIds, (int)(g_gain * 100)); }
        if (movie_prio(m) == 2) { if (sc < 1e17) { g_owner = root; set_target(root, cx, cy, key, mx, my); } }   /* a popup is modal */
        else {
            /* every menu on screen answers from the same point (they share stage coordinates), each in its own draw:
               a movie may only be read while the game draws it (reading another movie's root crashed when that
               movie had just been closed, 07.11). The nearest item in that direction wins, whichever window it is in. */
            g_probeDir = dir; g_probeX = bx; g_probeY = by; g_probeDone = 1L << (m - g_nm); g_probeBest = sc;
            g_probeRoot = sc < 1e17 ? root : 0; g_pcx = cx; g_pcy = cy; g_pkey = key; g_probeTick = GetTickCount();
        }
    }
    if (g_probeDir >= 0) {
        long bit = 1L << (m - g_nm);
        if (!(g_probeDone & bit)) {
            g_probeDone |= bit;
            double cx, cy; int key;
            double sc = best_item(m, root, g_probeDir, g_probeX, g_probeY, &cx, &cy, &key);
            if (sc < g_probeBest) { g_probeBest = sc; g_probeRoot = root; g_pcx = cx; g_pcy = cy; g_pkey = key; }
        }
        int all = 1;
        for (int i = 0; i < g_nmN; i++) if (alive(&g_nm[i]) && g_nm[i].nIds && !(g_probeDone & (1L << i))) all = 0;
        if (all || GetTickCount() - g_probeTick > 150) {
            if (g_probeRoot) {
                const char* wn = "?"; for (int i = 0; i < g_nmN; i++) if (g_nm[i].root == g_probeRoot) wn = k_tables[g_nm[i].table].movie;
                if (g_logN < 200) { g_logN++; logf("UINav: nearest that way is in %s (%d,%d)%s", wn, (int)g_pcx, (int)g_pcy, all ? "" : " (not every menu answered)"); }
                g_owner = g_probeRoot; set_target(g_probeRoot, g_pcx, g_pcy, g_pkey, g_probeX, g_probeY);
            }
            g_probeDir = -1;
        }
    }
    /* a movie without its own cursor reading (levelup) is steered from the movie that sees the cursor move */
    if (root == g_targetRoot) g_targetBlind = !own;
    if (noMouse) return;
    void* steerRoot = g_targetBlind && root == g_gRoot ? root : g_targetRoot;
    /* steer the cursor to the target */
    if (g_hasTarget && steerRoot == root && !g_pendX && !g_pendY) {
        double ex = g_tx - mx, ey = g_ty - my;
        if (ex * ex + ey * ey < 100) {
            if (!g_arrivedLogged && g_logN < 200) { g_arrivedLogged = 1; g_logN++; logf("UINav: arrived %u ms after the press, %d steps", (unsigned)(GetTickCount() - g_reqTick), g_steps); }
            return;
        }                  /* within 10 px (items are 40+ px): close enough, no slow final steps */
        /* the game sees our motion several frames late: send one step, then wait until the cursor has settled
           (same position two frames running, or 200 ms) before the next correction; give up after 4 steps */
        DWORD now = GetTickCount();
        if (g_waiting) {
            /* settled = moved since the step, then unchanged for 3 draws, and at least 120 ms after it (the game's
               input lags; deciding early sent a second step on top of the first and the cursor swung back and forth) */
            if (mx == g_prevMx && my == g_prevMy) g_still++; else g_still = 0;
            g_prevMx = mx; g_prevMy = my;
            int settled = g_still >= 2 && (mx != g_sendMx || my != g_sendMy) && (int)(now - (g_waitUntil - 300)) >= 70;
            if (!settled && (int)(now - g_waitUntil) < 0) return;
            g_waiting = 0;
            /* learn the scale from this step: motion sent / distance travelled (averaged, since a step that hits the edge of the movie reads short) */
            double dmx = mx - g_stepMx, dmy = my - g_stepMy, moved = dmx * dmx + dmy * dmy;
            double sent = (double)g_stepSX * g_stepSX + (double)g_stepSY * g_stepSY;
            if (moved > 400 && sent > 400) {
                double r = sent / moved, gnew = 1; for (int k = 0; k < 20; k++) gnew = 0.5 * (gnew + r / gnew);
                g_gain = 0.5 * g_gain + 0.5 * gnew; if (g_gain < 0.2) g_gain = 0.2; if (g_gain > 4) g_gain = 4;
            }
        }
        if (g_steps >= 4) { g_hasTarget = 0; return; }
        long sx = (long)(ex * g_gain), sy = (long)(ey * g_gain);
        if (!sx && !sy) { sx = ex > 0 ? 1 : ex < 0 ? -1 : 0; sy = ey > 0 ? 1 : ey < 0 ? -1 : 0; }
        g_pendX = sx; g_pendY = sy; g_steps++; g_stepMx = mx; g_stepMy = my; g_stepSX = sx; g_stepSY = sy;
        if (g_traceN < 120) { g_traceN++; logf("UINav: step %d: cursor (%d,%d) target (%d,%d), moving (%d,%d)", g_steps, (int)mx, (int)my, (int)g_tx, (int)g_ty, (int)sx, (int)sy); }
    }
    /* large single mouse moves confuse the game (the cursor jumped to the opposite edge): feed the step in small pieces */
    if (g_hasTarget && steerRoot == root && (g_pendX || g_pendY)) {
        long px = g_pendX, py = g_pendY;
        if (px > 60) px = 60; if (px < -60) px = -60; if (py > 60) py = 60; if (py < -60) py = -60;
        g_pendX -= px; g_pendY -= py;
        __sync_fetch_and_add(&g_moveX, px); __sync_fetch_and_add(&g_moveY, py);
        g_sumSX += px; g_sumSY += py;
        if (!g_pendX && !g_pendY) { g_waiting = 1; g_still = 0; g_waitUntil = GetTickCount() + 300; g_sendMx = g_prevMx = mx; g_sendMy = g_prevMy = my; }
    }
}

/* the stick moved the cursor: forget the target (pad thread) */
void uinav_release(void) { g_owner = 0; g_hasTarget = 0; g_focusId = -1; g_nativeUsed = 0; }

/* ---- hooks: slot 18 Invoke (ids), slot 12 GetVariable (pointer only) ---- */
extern int div2fix_movie_name(void* root, char* out, int cap);
/* 08.11.2026 [UI] CompareColors=1: the tooltip compares the selected item with the equipped one (the exe sends
   setTooltipText(text, false) for the selected, then setTooltipText(text, true) for the equipped; UTF-16 HTML, lines
   split by \n). Every line with a number gives "key = text before the number" (tags, ':' and '+' stripped) and its
   value (a range "33 - 99" counts as the mean); values with the same key are summed per item. When the equipped text
   arrives, the numbers of the selected item's lines whose key the equipped item has too are coloured: green when the
   selected sum is higher, red when lower, and the selected text is sent again. The required level line is skipped. */
#define TIP_MAX 8192
static unsigned short g_tipSel[TIP_MAX];
static int g_tipSelOk, g_tipCmp = -1;
typedef struct { unsigned short key[48]; int klen; double val; } TipKV;

static int tip_digit(unsigned short c) { return c >= '0' && c <= '9'; }

/* walks one item text; for every numbered line calls out(key, klen, value, numStart, numEnd) with indexes into s */
typedef void (*TipLineFn)(void* ctx, const unsigned short* key, int klen, double v, int a, int b);
static void tip_lines(const unsigned short* s, void* ctx, TipLineFn fn)
{
    unsigned short plain[512]; int pmap[512];
    int i = 0;
    while (s[i]) {
        int np = 0;
        while (s[i] && s[i] != '\n') {                        /* one line: plain text and where each plain char came from */
            if (s[i] == '<') { while (s[i] && s[i] != '>') i++; if (s[i]) i++; continue; }
            if (np < 511) { plain[np] = s[i]; pmap[np] = i; np++; }
            i++;
        }
        plain[np] = 0;
        if (s[i] == '\n') i++;
        /* the last number of the line and, before it, "N - " for a range */
        int e = np - 1;
        while (e >= 0 && !tip_digit(plain[e])) e--;
        if (e < 0) continue;
        int b = e; while (b > 0 && tip_digit(plain[b - 1])) b--;
        double v = 0; for (int k = b; k <= e; k++) v = v * 10 + (plain[k] - '0');
        int a = b;
        if (a > 0 && plain[a - 1] == '-') { v = -v; a--; }         /* a range has spaces round its dash */
        else if (a > 0 && plain[a - 1] == '+') a--;
        int r = a - 1; while (r >= 0 && plain[r] == ' ') r--;
        if (r >= 2 && plain[r] == '-' && plain[r - 1] == ' ') {               /* "33 - 99" */
            int e2 = r - 2; while (e2 >= 0 && plain[e2] == ' ') e2--;
            if (e2 >= 0 && tip_digit(plain[e2])) {
                int b2 = e2; while (b2 > 0 && tip_digit(plain[b2 - 1])) b2--;
                double v2 = 0; for (int k = b2; k <= e2; k++) v2 = v2 * 10 + (plain[k] - '0');
                v = (v + v2) / 2; a = b2;
            }
        }
        int ke = a - 1;                                       /* key: before the number, without ' ', ':', '+' */
        while (ke >= 0 && (plain[ke] == ' ' || plain[ke] == ':' || plain[ke] == '+' || plain[ke] == '\t')) ke--;
        int ks = 0; while (ks <= ke && plain[ks] == ' ') ks++;
        int klen = ke - ks + 1;
        if (klen <= 1 || klen > 46) continue;
        /* the required level ("... : Уровень 26" / "Level 26") is not a stat */
        static const unsigned short lvl_ru[] = { 0x0423, 0x0440, 0x043E, 0x0432, 0x0435, 0x043D, 0x044C };
        static const unsigned short lvl_en[] = { 'L', 'e', 'v', 'e', 'l' };
        int isLvl = 0;
        if (klen >= 7) { isLvl = 1; for (int k = 0; k < 7; k++) if (plain[ke - 6 + k] != lvl_ru[k]) isLvl = 0; }
        if (!isLvl && klen >= 5) { isLvl = 1; for (int k = 0; k < 5; k++) if (plain[ke - 4 + k] != lvl_en[k]) isLvl = 0; }
        if (isLvl) continue;
        int endOrig = pmap[e] + 1;
        fn(ctx, plain + ks, klen, v, pmap[a], endOrig);
    }
}

typedef struct { TipKV kv[64]; int n; } TipSet;
static int tip_key_eq(const TipKV* x, const unsigned short* k, int l)
{
    if (x->klen != l) return 0;
    for (int i = 0; i < l; i++) if (x->key[i] != k[i]) return 0;
    return 1;
}
static void tip_collect(void* ctx, const unsigned short* key, int klen, double v, int a, int b)
{
    TipSet* s = (TipSet*)ctx; (void)a; (void)b;
    for (int i = 0; i < s->n; i++) if (tip_key_eq(&s->kv[i], key, klen)) { s->kv[i].val += v; return; }
    if (s->n >= 64) return;
    TipKV* x = &s->kv[s->n++];
    for (int i = 0; i < klen; i++) x->key[i] = key[i];
    x->klen = klen; x->val = v;
}
typedef struct { TipSet* sel; TipSet* eq; int pos[96], end[96], col[96], n; } TipMark;
static void tip_mark(void* ctx, const unsigned short* key, int klen, double v, int a, int b)
{
    TipMark* m = (TipMark*)ctx; (void)v;
    double sv = 0, ev = 0; int inE = 0;
    for (int i = 0; i < m->sel->n; i++) if (tip_key_eq(&m->sel->kv[i], key, klen)) sv = m->sel->kv[i].val;
    for (int i = 0; i < m->eq->n; i++) if (tip_key_eq(&m->eq->kv[i], key, klen)) { ev = m->eq->kv[i].val; inE = 1; }
    if (!inE || sv == ev || m->n >= 96) return;
    m->pos[m->n] = a; m->end[m->n] = b; m->col[m->n] = sv > ev; m->n++;
}

static int tip_len(const unsigned short* w) { int n = 0; while (n < TIP_MAX - 1 && w[n]) n++; return n; }

/* called before the game's Invoke: remembers the selected text; returns 1 when it handled the equipped call */
static int tip_compare(void* root, void* edx, const char* method, GVal* result, const GVal* args, UINT n)
{
    if (g_tipCmp < 0) g_tipCmp = GetPrivateProfileIntA("UI", "CompareColors", 1, g_ini);
    if (!g_tipCmp || !method || n < 1 || args[0].type != 5) return 0;
    int equipped = n >= 2 && args[1].type == 2 && args[1].v.b;   /* the selected item comes with one argument */
    const char* m = method; int ok = 0;
    for (; *m && !ok; m++) ok = m[0] == 's' && m[1] == 'e' && m[2] == 't' && m[3] == 'T' && m[4] == 'o' && m[5] == 'o' && m[6] == 'l' && m[7] == 't';
    if (!ok) return 0;
    const unsigned short* w = (const unsigned short*)args[0].v.s;
    if (!w || !readable(w, 2)) return 0;
    int len = tip_len(w);
    if (!equipped) {                                          /* the selected item: keep its text */
        for (int i = 0; i < len; i++) g_tipSel[i] = w[i];
        g_tipSel[len] = 0; g_tipSelOk = len > 0;
        return 0;
    }
    if (!g_tipSelOk) return 0;
    g_tipSelOk = 0;
    static TipSet sel, eq; static TipMark mk;
    sel.n = eq.n = 0;
    tip_lines(g_tipSel, &sel, tip_collect);
    tip_lines(w, &eq, tip_collect);
    mk.sel = &sel; mk.eq = &eq; mk.n = 0;
    tip_lines(g_tipSel, &mk, tip_mark);
    if (!mk.n) return 0;                                       /* nothing to colour: the game's call goes on as it is */
    static unsigned short out[TIP_MAX + 96 * 48];
    static const unsigned short green[] = { '<','f','o','n','t',' ','c','o','l','o','r','=','"','#','4','0','F','F','4','0','"','>',0 };
    static const unsigned short red[] = { '<','f','o','n','t',' ','c','o','l','o','r','=','"','#','F','F','5','0','5','0','"','>',0 };
    static const unsigned short close[] = { '<','/','f','o','n','t','>',0 };
    int o = 0, slen = tip_len(g_tipSel), k = 0;
    for (int i = 0; i <= slen && o < TIP_MAX + 96 * 48 - 40; i++) {
        if (k < mk.n && i == mk.pos[k]) { const unsigned short* c = mk.col[k] ? green : red; while (*c) out[o++] = *c++; }
        if (k < mk.n && i == mk.end[k]) { const unsigned short* c = close; while (*c) out[o++] = *c++; k++; }
        if (i < slen) out[o++] = g_tipSel[i];
    }
    out[o] = 0;
    /* the selected text again BEFORE the equipped one, with one argument as the game sends it: a false comparableItem
       hides the second frame (framesComp_mc), the equipped call that follows shows it */
    GVal a2[1]; memset(a2, 0, sizeof a2);
    a2[0].type = 5; a2[0].v.s = (const char*)out;
    GVal res; memset(&res, 0, sizeof res);
    g_origInvoke(root, edx, method, &res, a2, 1);
    static int logged; if (logged++ < 5) logf("UI: item comparison coloured %d number(s)", mk.n);
    return 0;                                                  /* the game's equipped call goes on */
}

static int __fastcall hk_Invoke(void* root, void* edx, const char* method, GVal* result, const GVal* args, UINT n)
{
    char name[72];
    if (args && tip_compare(root, edx, method, result, args, n)) return 1;
    if (method && n && args && div2fix_movie_name(root, name, sizeof name)) uinav_on_invoke(root, name, method, args, n);
    return g_origInvoke(root, edx, method, result, args, n);
}

void uinav_install(void** rootVt, void* probeOrig18)
{
    if ((!GetPrivateProfileIntA("Gamepad", "Enable", 1, g_ini) || !GetPrivateProfileIntA("Gamepad", "MenuFocus", 1, g_ini)) &&
        !GetPrivateProfileIntA("UI", "CompareColors", 1, g_ini)) return;      /* 08.11: also for the item comparison */
    g_getVar = (GetVar_t)rootVt[12];
    g_setVar = (SetVar_t)rootVt[11];
    g_xhudOn = GetPrivateProfileIntA("Gamepad", "XboxHud", 1, g_ini);
    { char v[16]; GetPrivateProfileStringA("Gamepad", "XboxHudSpread", "6", v, sizeof v, g_ini); int k = 0; double d = 0; while (v[k] >= '0' && v[k] <= '9') d = d * 10 + (v[k++] - '0'); if (d >= 2 && d <= 30) g_xhudSpread = d; }
    /* slot 18 may already hold the UI probe stub; chain behind whatever is there */
    g_origInvoke = (Invoke_t)(probeOrig18 ? probeOrig18 : rootVt[18]);
    if (probeOrig18) { extern void* g_probeOrig[20]; void* h = (void*)hk_Invoke; g_probeOrig[18] = h; }
    else { void* h = (void*)hk_Invoke; write_mem(&rootVt[18], &h, 4); }
    logf("UINav: d-pad menu focus on (Invoke slot 18, GetVariable slot 12; tables: main menu, save (own cursor), options, dialog, chest, inventory, equipment, popup, trade, questlog, waypointshrine, levelup, skilltree)");
}
