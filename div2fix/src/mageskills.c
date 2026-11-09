/* Div2Fix - Mage Skills Expansion: gives the hero the new skills so that the skill tree can show them.
 *
 * The skill tree window lists only the skills the character owns. The hero's skills come from the save, or in a new game
 * from Episode_*\InitSaveGame.dv2 (init_savegame.dsg); Episodes\<ep>\DefaultSkills.xml is not applied in a normal new
 * game (trace 01.11, MageExpansion/docs/TEST_RESULTS.md). So a skill added by data (MageExpansion.dv2: prototype, tree
 * cell, texts, icon) never reaches the hero. This module adds it at rank 0 the way the game itself does when it loads
 * the character's skills (0x769B90) and applies DefaultSkills (0x6F3040):
 *     0x7692D0  thiscall(stats, id, rank, rankCap) ret 0Ch   gives a skill (cap 5 = the game's starting cap)
 *     0x753330  thiscall(stats, id) ret 4                    the character's skill with this id, or 0
 * Trigger: the skill tree builds its tab lists with 0xA0BCE0 (cdecl out, character handle, tab 0..5; returns out). On
 * tab 0 the character is resolved exactly as 0xA0BCE0 does it:
 *     0x8C9330  stdcall(handle) ret 4 -> character; [character+4] & 0xFFF = index
 *     0x818370  thiscall([0x14F13C4], &index) ret 4 -> stats (skills at stats+0x1B0)
 * A skill is given only when its prototype is loaded (prototype table [0x14F1474]+0x10, by id: the overlay archive is
 * installed) and the character already owns Magic Missile (0x1B), i.e. it is the hero, not the dragon form. Learning,
 * ranks, mana, cooldown and the hotbar are then the game's own. The game saves the skill like any other one.
 *
 * Icons: the image name "skl/<id> btn" (id = identity, [proto+0x38]) is resolved in 0xBC2380: proto = 0x74D350(id),
 * then 0xBC277C call 0x89D350 (= [proto+0x24], the BEHAVIOUR id, i.e. the CodePath skill) and 0xBC2784 call 0x8E07B0
 * (thiscall(behaviour id, NiFixedString* name) ret 8: icon name of that behaviour). So a skill on another skill's code
 * gets that skill's icon. Both calls are redirected: the first remembers the prototype and returns the same value, the
 * second gives our skills the name of their own UUID ([proto+0x30], e.g. "Chain Hit" -> skillbutton_ChainHit_*.nif from
 * MageExpansion.dv2) through NiFixedString = const char* (0x416FF0, as the prototype loader does) and passes every
 * other skill to 0x8E07B0 unchanged.
 *
 * Projectile effects: a skill on Fireball's code flies with Fireball's effects. The spell executor 0x9685E0 (vtable
 * 0x1333A48, slot 1) sets them for every Fireball cast at 0x96999E:
 *     push &FireBall_Impact (0x14F1B60) / push &Player_Fireball (0x14F1B50) / lea ecx,[esp+0F8h] / call 0x8FFB20
 *     0x8FFB20  thiscall(desc, NiFixedString* travel, NiFixedString* impact) ret 8 -> [desc+64h], [desc+68h]
 * where esi is the spell instance and [esi+2Ch] its skill id (the game passes it to 0x74D350 just before). That call
 * goes to a stub that saves [esi+2Ch] and continues in fx_effects(): our skills get the effect names of
 * [MageSkills] Fx<id>=<travel>,<impact>[,<cast>] (names of rpgstats_effectprototypes.xml), every other cast is unchanged.
 * Cast flash: the Fireball cast object (vtable 0x1333608, same layout as the spell instance: skill id at [this+2Ch])
 * spawns FireBall_Cast in 0x95E6E0 (ebp = this) by value: "mov eax,[0x14F1B68]" at 0x95E842 and again at 0x95E864
 * (after InterlockedIncrement of its reference count), then 0x741140. Both loads become "call fx_cast_stub", which
 * returns our cast effect for our skills and the value of FireBall_Cast otherwise; ecx/edx are dead there, the FPU
 * stack holds a value for the next fstp, so the C part uses no floating point.
 * Slow: at 0x96999E the spell instance also holds the target ([esi+24h], a character handle, -1 for a point on the
 * ground) and the rank ([esi+30h]). A request is queued there; the message loop 0xB38880 (main thread, between frames)
 * applies it after <delay> ms and lifts it after <ms>. The target's stats come from 0x818330(&handle) as the spell
 * code does at 0x969644; RunningSpeed is stat 11, changed by 0x75B870(11, delta) (adds and clamps to the record's
 * min/max) exactly as the game's Lua SetRunningSpeed does (0x9BA0B0 -> 0x75BAF0). The amount the clamp let through is
 * given back; a second hit extends the slow instead of stacking it; nothing is given back when the handle no longer
 * resolves to the same stats. Known limits: applied at the cast (+delay), not at the impact; a save made while a
 * character is slowed may keep its lowered speed.
 * INI: [MageSkills] Enable=1, Grant=14 (skill ids, comma separated; 14 = 0x0E "Chain Hit" = Ice Bolt), RankCap=5,
 * OwnIcons=1, Fx14=MagicMissile_Travel,MagicMissile_Impact,MagicMissile_Cast, Slow14=30,5,2000,250,300.
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
#define ID_MISSILE 0x1B
#define MAX_GRANT  32

typedef void* (__cdecl *TabList_t)(void* out, int handle, int tab);
typedef void* (__stdcall *Character_t)(int handle);
typedef void* (__fastcall *Stats_t)(void* mgr, void* edx, int* index);
typedef int   (__fastcall *Grant_t)(void* stats, void* edx, int id, int rank, int cap);
typedef void* (__fastcall *Find_t)(void* stats, void* edx, int id);

static TabList_t   g_tabList;
static Character_t g_character;
static Stats_t     g_stats;
static Grant_t     g_grant;
static Find_t      g_find;
static void**      g_statsMgr;          /* [0x14F13C4] */
static BYTE**      g_protoMgr;          /* [0x14F1474] */
static int         g_ids[MAX_GRANT], g_n, g_cap;
static int         g_logged[MAX_GRANT];

typedef BYTE (__fastcall *IconName_t)(void* mgr, void* edx, int behaviour, void* name);
typedef void (__fastcall *Assign_t)(void* fixedString, void* edx, const char* text);
static IconName_t  g_iconName;          /* 0x8E07B0 */
static Assign_t    g_assign;            /* 0x416FF0 */
static BYTE*       g_iconProto;         /* prototype of the image being resolved (UI thread) */
static int         g_iconLogged, g_controlLogged, g_iconTrace;   /* [MageSkills] IconTrace=1: log icon requests */
static int         g_iconRequests;      /* every request through 0xBC2784, for the order in the log */

/* projectile effects of our skills (see the header) */
typedef void  (__fastcall *SetFx_t)(void* desc, void* edx, char** travel, char** impact);
typedef char* (__cdecl *Fixed_t)(const char* text);
static SetFx_t     g_setFx;             /* 0x8FFB20 */
static Fixed_t     g_fixed;             /* 0x4B34D0 */
static int         g_fxId;              /* [esi+2Ch] at 0x96999E, written by fx_stub */
static char        g_fxName[MAX_GRANT][3][64];                      /* travel, impact, cast from the ini */
static char*       g_fxTravel[MAX_GRANT], *g_fxImpact[MAX_GRANT], *g_fxCast[MAX_GRANT];  /* NiFixedStrings */
static int         g_fxLogged[MAX_GRANT], g_castLogged[MAX_GRANT];
static char**      g_fbCast;            /* the game's FireBall_Cast (0x14F1B68) */

/* NiFixedStrings are made on first use: at DLL load the game's string table does not exist yet (creating them in
   fx_init crashed the game at start, 01.11 13:00). Kept for the process (one reference each). */
static char* fx_string(char** slot, const char* name)
{
    if (!*slot && name[0]) *slot = g_fixed(name);
    return *slot;
}

/* ---- slow (see the header): requests from fx_effects, applied and lifted in the message loop (main thread) ---- */
#define STAT_RUNNING 11                 /* RunningSpeed: 0x75BAF0 (Lua SetRunningSpeed) uses record 0xE8 = 0xC + 11*20 */
#define MAX_SLOW     32
typedef void* (__fastcall *ByHandle_t)(void* mgr, void* edx, int* handle);         /* 0x818330 */
typedef void  (__fastcall *AddStat_t)(void* stats, void* edx, int index, float delta); /* 0x75B870 */
typedef int   (__fastcall *Pump_t)(void* self, void* edx);                         /* 0xB38880 */
static ByHandle_t  g_byHandle;
static AddStat_t   g_addStat;
static Pump_t      g_pump;
static BYTE*       g_fxInst;            /* the spell instance (esi) at 0x96999E, written by fx_stub */
static int         g_slowPct[MAX_GRANT][2], g_slowMs[MAX_GRANT][2], g_slowDelay[MAX_GRANT];
static int         g_freezeFrom[MAX_GRANT], g_freezeChance[MAX_GRANT], g_freezeMs[MAX_GRANT];
static int         g_slowLogged;
/* trainer list trace (TrainerTrace=1): 0xB1B200 builds the trainer window's list, record -> skill id via 0x89F0D0
   (record's Skill string -> prototype UUID map 0x6FEEB0), status per record in the map at [list+0x100]:
   0 = can buy, 1 = shown but level/gold too low, 2 = not shown (unknown skill, not owned, cap reached, lower tier first) */
typedef int  (__fastcall *TrainerSkillId_t)(void* handle, void* edx, int index);   /* 0x89F0D0 */
typedef void (__fastcall *TrainerList_t)(void* self, void* edx);                    /* 0xB1B200 */
static TrainerSkillId_t g_trainerSkillId;
static TrainerList_t    g_trainerList;
static int g_trainerIdx[16], g_trainerIdxN, g_trainerLogged;
static DWORD g_trainerRet, g_trainerRa[16];
static int g_trainerRaCalls[16], g_trainerRaN;
static struct { int state, handle, skill, ms, rank, frozen; float pct, applied, freezeApplied; void* stats; DWORD at, freezeAt; } g_slow[MAX_SLOW];   /* at: GetTickCount ms */

static void slow_request(int i, BYTE* inst)
{
    if (!g_slowPct[i][0] || !inst || !readable(inst + 0x24, 16)) return;
    int handle = *(int*)(inst + 0x24), rank = *(int*)(inst + 0x30);
    if (handle < 0) return;                                       /* aimed at the ground */
    if (rank < 1) rank = 1;
    for (int k = 0; k < MAX_SLOW; k++) {
        if (g_slow[k].state) continue;
        DWORD now = GetTickCount();
        int pct = g_slowPct[i][0] + g_slowPct[i][1] * (rank - 1);
        g_slow[k].pct = (float)(pct > 80 ? 80 : pct);
        g_slow[k].ms = g_slowMs[i][0] + g_slowMs[i][1] * (rank - 1);
        g_slow[k].rank = rank;
        g_slow[k].skill = g_ids[i];
        g_slow[k].handle = handle;
        g_slow[k].at = now + (DWORD)g_slowDelay[i];
        g_slow[k].frozen = 0;
        g_slow[k].freezeApplied = 0;
        g_slow[k].freezeAt = 0;
        g_slow[k].state = 1;
        return;
    }
}

static void status_fx(void* stats, int i, int frozen, DWORD ms);   /* below: the mark on a slowed/frozen enemy */

static float* running_speed(void* stats)
{
    BYTE* box = readable((BYTE*)stats + 0x104, 4) ? *(BYTE**)((BYTE*)stats + 0x104) : 0;
    return (box && readable(box + 0xE8 + 8, 4)) ? (float*)(box + 0xE8 + 8) : 0;
}

static void slow_tick(void)
{
    DWORD now = GetTickCount();
    void* mgr = *g_statsMgr;
    for (int k = 0; k < MAX_SLOW; k++) {
        if (!g_slow[k].state) continue;
        if (g_slow[k].state == 2 && g_slow[k].frozen && (int)(now - g_slow[k].freezeAt) >= 0) {
            void* fs = mgr ? g_byHandle(mgr, 0, &g_slow[k].handle) : 0;
            float* fc = fs ? running_speed(fs) : 0;
            g_slow[k].frozen = 0;
            if (fc && fs == g_slow[k].stats) g_addStat(fs, 0, STAT_RUNNING, -g_slow[k].freezeApplied);
        }
        if ((int)(now - g_slow[k].at) < 0) continue;     /* wrap-safe */
        int h = g_slow[k].handle;
        void* stats = mgr ? g_byHandle(mgr, 0, &h) : 0;
        float* cur = stats ? running_speed(stats) : 0;
        if (g_slow[k].state == 1) {                               /* apply */
            g_slow[k].state = 0;
            if (!cur) continue;
            int dup = -1;
            for (int j = 0; j < MAX_SLOW; j++)
                if (j != k && g_slow[j].state == 2 && g_slow[j].handle == h && g_slow[j].stats == stats) dup = j;
            if (dup >= 0) {                                       /* already slowed: extend, never stack */
                g_slow[dup].at = now + (DWORD)g_slow[k].ms;
                continue;
            }
            float before = *cur;
            g_addStat(stats, 0, STAT_RUNNING, -before * g_slow[k].pct / 100.0f);
            g_slow[k].applied = *cur - before;                    /* what the clamp let through */
            int rank = g_slow[k].rank;
            /* The rank is carried by the projectile instance only until slow_request; the configured
               threshold/chance are intentionally stored per skill and applied through a compact roll. */
            for (int i = 0; i < g_n; i++) if (g_ids[i] == g_slow[k].skill) {
                if (g_freezeFrom[i]) {
                    DWORD roll = GetTickCount() ^ (DWORD)(h * 2654435761u);
                    int chance = g_freezeChance[i];
                    if (rank >= g_freezeFrom[i] && (roll % 100) < (DWORD)chance) {
                        float fbefore = *cur;
                        g_addStat(stats, 0, STAT_RUNNING, -fbefore);
                        g_slow[k].freezeApplied = *cur - fbefore;
                        g_slow[k].frozen = 1;
                        g_slow[k].freezeAt = now + (DWORD)g_freezeMs[i];
                    }
                    break;
                }
            }
            g_slow[k].stats = stats;
            g_slow[k].at = now + (DWORD)g_slow[k].ms;
            g_slow[k].state = 2;
            for (int i = 0; i < g_n; i++) if (g_ids[i] == g_slow[k].skill) {
                status_fx(stats, i, 0, (DWORD)g_slow[k].ms);
                if (g_slow[k].frozen) status_fx(stats, i, 1, g_slow[k].freezeAt - now);
                break;
            }
            if (g_slowLogged < 20) {
                g_slowLogged++;
                logf("MageSkills: slow %d%% for %d ms on character %d: running speed %d.%02d -> %d.%02d",
                     (int)g_slow[k].pct, g_slow[k].ms, h, (int)before, (int)(before * 100) % 100, (int)*cur,
                     (int)(*cur * 100) % 100);
            }
        } else {                                                  /* lift */
            g_slow[k].state = 0;
            if (!cur || stats != g_slow[k].stats) {
                if (g_slowLogged < 20) { g_slowLogged++; logf("MageSkills: slowed character %d is gone, nothing to lift", h); }
                continue;
            }
            g_addStat(stats, 0, STAT_RUNNING, -g_slow[k].applied);
            if (g_slow[k].frozen) g_addStat(stats, 0, STAT_RUNNING, -g_slow[k].freezeApplied);
            if (g_slowLogged < 20) {
                g_slowLogged++;
                logf("MageSkills: slow lifted from character %d: running speed %d.%02d", h, (int)*cur, (int)(*cur * 100) % 100);
            }
        }
    }
}

static void own_tick(void);                            /* below: pending hits of own skills */

static int __fastcall hk_pump(void* self, void* edx)
{
    int r = g_pump(self, edx);
    slow_tick();
    own_tick();
    return r;
}

static void stack_chain(const char* what, DWORD* sp);   /* CastTrace (below) */
static DWORD g_textLo, g_castAt;
static int g_fxChainLogged, g_statLogged;

static void __fastcall fx_effects(void* desc, void* edx, char** travel, char** impact)
{
    if (g_textLo && g_fxChainLogged++ < 6) {          /* CastTrace=1: how the hero's cast reaches the spell */
        stack_chain("hero spell (0x96999E)", (DWORD*)__builtin_frame_address(0));
        g_castAt = GetTickCount(); g_statLogged = 0;
    }
    for (int i = 0; i < g_n; i++) {
        if (g_ids[i] != g_fxId) continue;
        slow_request(i, g_fxInst);
        if (!g_fxName[i][0][0]) break;
        fx_string(&g_fxTravel[i], g_fxName[i][0]);
        fx_string(&g_fxImpact[i], g_fxName[i][1]);
        if (!g_fxLogged[i]++)
            logf("MageSkills: skill %d casts with effects '%s' / '%s' instead of '%s' / '%s'", g_fxId, g_fxTravel[i],
                 g_fxImpact[i], (travel && *travel) ? *travel : "?", (impact && *impact) ? *impact : "?");
        travel = &g_fxTravel[i];
        impact = &g_fxImpact[i];
        break;
    }
    g_setFx(desc, edx, travel, impact);
}

/* replaces "call 0x8FFB20" at 0x96999E: the stack is that of the call, ecx = desc */
static __declspec(naked) void fx_stub(void)
{
    __asm {
        mov g_fxInst, esi
        mov eax, dword ptr [esi + 2Ch]
        mov g_fxId, eax
        jmp fx_effects
    }
}

/* the cast effect for skill id: ours from the ini, otherwise the game's FireBall_Cast. No floating point here. */
/* test aid: a cast name list "a|b|c" shows the next name at every cast (both call sites of one cast come within
   300 ms and get the same name); each change is logged */
static char  g_cycName[6][48];
static char* g_cycFixed[6];
static int   g_cycN = -1, g_cycAt;
static DWORD g_cycLast;

static char* cast_cycle(const char* list)
{
    if (g_cycN < 0) {
        g_cycN = 0;
        for (const char* p = list; *p && g_cycN < 6; ) {
            int k = 0;
            while (*p && *p != '|' && k < 47) g_cycName[g_cycN][k++] = *p++;
            g_cycName[g_cycN++][k] = 0;
            if (*p == '|') p++;
        }
        g_cycAt = -1;
    }
    DWORD now = GetTickCount();
    if (g_cycAt < 0 || now - g_cycLast > 300) {
        g_cycAt = (g_cycAt + 1) % g_cycN;
        logf("MageSkills: TEST cast flash #%d = '%s'", g_cycAt + 1, g_cycName[g_cycAt]);
    }
    g_cycLast = now;
    return fx_string(&g_cycFixed[g_cycAt], g_cycName[g_cycAt]);
}

static char* __cdecl fx_cast_name(int id)
{
    for (int i = 0; i < g_n; i++) {
        if (g_ids[i] != id || !g_fxName[i][2][0]) continue;
        int cyc = 0;
        for (const char* p = g_fxName[i][2]; *p; p++) if (*p == '|') cyc = 1;
        if (cyc) return cast_cycle(g_fxName[i][2]);
        char* s = fx_string(&g_fxCast[i], g_fxName[i][2]);
        if (!g_castLogged[i]++) logf("MageSkills: skill %d cast flash '%s' instead of '%s'", id, s, *g_fbCast);
        return s;
    }
    return *g_fbCast;
}

/* replaces "mov eax,[0x14F1B68]" at 0x95E842 and 0x95E864 (5 bytes each): eax = effect name, ebp = cast object */
static __declspec(naked) void fx_cast_stub(void)
{
    __asm {
        push dword ptr [ebp + 2Ch]
        call fx_cast_name
        add esp, 4
        ret
    }
}

/* IconTrace only: every image URL the GUI asks for (0xBC2380, vtable 0x1363F90: thiscall(url, out) ret 8 -> al) */
typedef BYTE (__fastcall *Image_t)(void* self, void* edx, const char* url, void* out);
static Image_t     g_image;
static int         g_urls;

static BYTE __fastcall hk_image(void* self, void* edx, const char* url, void* out)
{
    BYTE r = g_image(self, edx, url, out);
    if (g_urls < 300) {
        g_urls++;
        logf("MageSkills: image #%d '%s' -> %d (icon requests so far %d)", g_urls,
             (url && readable(url, 1)) ? url : "?", r, g_iconRequests);
    }
    return r;
}

static int ours(int id)
{
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id) return 1;
    return 0;
}

/* replaces "call 0x89D350" at 0xBC277C: ecx = prototype, returns [proto+0x24] like the original */
static int __fastcall icon_behaviour(BYTE* proto)
{
    g_iconProto = proto;
    return *(int*)(proto + 0x24);
}

/* replaces "call 0x8E07B0" at 0xBC2784 */
static BYTE __fastcall icon_name(void* mgr, void* edx, int behaviour, void* name)
{
    BYTE* p = g_iconProto;
    g_iconProto = 0;
    g_iconRequests++;
    if (p && readable(p + 0x30, 12) && ours(*(int*)(p + 0x38))) {
        const char* uuid = *(const char**)(p + 0x30);
        if (uuid && readable(uuid, 1)) {
            g_assign(name, 0, uuid);
            if (g_iconLogged < 40 && g_iconTrace) {
                g_iconLogged++;
                logf("MageSkills: icon request #%d for skill %d -> '%s'", g_iconRequests, *(int*)(p + 0x38),
                     *(const char**)name);
            }
            return 1;
        }
    }
    BYTE r = g_iconName(mgr, edx, behaviour, name);
    if (g_iconTrace && behaviour == ID_MISSILE && g_controlLogged < 5) {
        g_controlLogged++;
        const char* t = (name && readable(name, 4) && *(const char**)name && readable(*(const char**)name, 1)) ? *(const char**)name : "?";
        logf("MageSkills: control: icon request #%d for behaviour %d -> %d '%s'", g_iconRequests, behaviour, r, t);
    }
    return r;
}

static int patch_call(BYTE* at, void* target, void* fn)
{
    if (at[0] != 0xE8 || at + 5 + *(long*)(at + 1) != (BYTE*)target) return 0;
    BYTE code[5] = { 0xE8 };
    long rel = (long)((BYTE*)fn - (at + 5));
    memcpy(code + 1, &rel, 4);
    return write_mem(at, code, 5);
}

static int prototype_loaded(int id)
{
    BYTE* mgr = *g_protoMgr;
    if (!mgr || !readable(mgr + 0x10, 8)) return 0;
    void** begin = *(void***)(mgr + 0x10);
    void** end = *(void***)(mgr + 0x14);
    return begin && id >= 0 && id < (int)(end - begin) && readable(begin + id, 4) && begin[id] != 0;
}

static int g_bowSkill;
static void give_skills_ch(void* ch)
{
    if (!ch || !readable(ch, 8)) return;
    int index = *(int*)((BYTE*)ch + 4) & 0xFFF;
    void* mgr = *g_statsMgr;
    if (!mgr) return;
    void* stats = g_stats(mgr, 0, &index);
    if (!stats || !g_find(stats, 0, ID_MISSILE)) return;          /* not the hero (dragon form, other character) */
    for (int i = 0; i < g_n; i++) {
        int id = g_ids[i];
        if (g_find(stats, 0, id)) continue;
        if (!prototype_loaded(id)) {
            if (g_logged[i] != 1) {
                BYTE* pm = *g_protoMgr;
                void** pb = pm && readable(pm + 0x10, 8) ? *(void***)(pm + 0x10) : 0, **pe = pb ? *(void***)(pm + 0x14) : 0;
                logf("MageSkills: skill %d has no prototype (MageExpansion.dv2 not installed?), not given (table %p: %d entries, this one %p)",
                     id, pm, pb ? (int)(pe - pb) : -1, pb && id < pe - pb ? pb[id] : 0);
            }
            g_logged[i] = 1;
            continue;
        }
        g_grant(stats, 0, id, id == g_bowSkill ? 1 : 0, g_cap);      /* a weapon skill starts with one point */
        void* s = g_find(stats, 0, id);
        logf("MageSkills: skill %d given to the hero at rank 0, cap %d -> %s", id, g_cap, s ? "ok" : "refused by the game");
        g_logged[i] = 2;
    }
}

static void give_skills(int handle) { give_skills_ch(g_character(handle)); }

static void* __cdecl hk_tabList(void* out, int handle, int tab)
{
    if (tab == 0) {
        if (g_iconTrace) logf("MageSkills: skill tree lists built (icon requests so far: %d)", g_iconRequests);
        give_skills(handle);
    }
    return g_tabList(out, handle, tab);
}

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

static void parse_ids(const char* s)
{
    while (*s && g_n < MAX_GRANT) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        int v = 0, base = 10;
        if (s[0] == '0' && (s[1] | 32) == 'x') { base = 16; s += 2; }
        for (;; s++) {
            char c = lc(*s);
            int d = (c >= '0' && c <= '9') ? c - '0' : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * base + d;
        }
        if (v > 0 && v < 0x1000) g_ids[g_n++] = v;
        while (*s && *s != ',') s++;
    }
}

/* [MageSkills] Fx<id>=<travel>,<impact>[,<cast>] -> names[0..2]; returns 1 when travel and impact are given */
static int fx_entry(int id, char names[3][64])
{
    char key[16] = "Fx", digits[8], val[200];
    int n = 2, k = 0;
    do { digits[k++] = (char)('0' + id % 10); id /= 10; } while (id && k < 7);
    while (k) key[n++] = digits[--k];
    key[n] = 0;
    GetPrivateProfileStringA("MageSkills", key, "", val, sizeof val, g_ini);
    const char* p = val;
    for (int f = 0; f < 3; f++) {
        int len = 0;
        while (p[len] && p[len] != ',') len++;
        if (len >= 64) return 0;
        memcpy(names[f], p, (SIZE_T)len);
        names[f][len] = 0;
        p += len;
        if (*p == ',') p++;
    }
    return names[0][0] && names[1][0];
}

/* [MageSkills] Slow<id>=<percent>,<percent per rank>,<ms>,<ms per rank>[,<delay ms>] -> 1 when percent > 0 */
static int slow_entry(int i)
{
    char key[16] = "Slow", digits[8], val[96];
    int id = g_ids[i], n = 4, k = 0, v[5] = { 0, 0, 0, 0, 0 };
    do { digits[k++] = (char)('0' + id % 10); id /= 10; } while (id && k < 7);
    while (k) key[n++] = digits[--k];
    key[n] = 0;
    GetPrivateProfileStringA("MageSkills", key, "", val, sizeof val, g_ini);
    const char* p = val;
    for (int f = 0; f < 5 && *p; f++) {
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') v[f] = v[f] * 10 + (*p++ - '0');
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    if (v[0] <= 0 || v[2] <= 0) return 0;
    g_slowPct[i][0] = v[0]; g_slowPct[i][1] = v[1];
    g_slowMs[i][0] = v[2];  g_slowMs[i][1] = v[3];
    g_slowDelay[i] = v[4];
    char fkey[24] = "Freeze";
    /* Freeze<id>=<rank>,<chance>,<milliseconds>; optional. */
    int fn = 6, fid = g_ids[i]; char rev[8]; int fk = 0;
    do { rev[fk++] = (char)('0' + fid % 10); fid /= 10; } while (fid && fk < 7);
    while (fk) fkey[fn++] = rev[--fk]; fkey[fn] = 0;
    char fv[64]; GetPrivateProfileStringA("MageSkills", fkey, "", fv, sizeof fv, g_ini);
    int q[3] = {0,0,0}; int qi = 0, cur = 0;
    for (int j = 0; fv[j] && qi < 3; j++) { if (fv[j] == ',') { q[qi++] = cur; cur = 0; } else if (fv[j] >= '0' && fv[j] <= '9') cur = cur * 10 + fv[j] - '0'; }
    if (fv[0]) q[qi++] = cur;
    if (qi == 3) { g_freezeFrom[i] = q[0]; g_freezeChance[i] = q[1]; g_freezeMs[i] = q[2]; }
    return 1;
}

static int __fastcall trainer_skill_id(void* handle, void* edx, int index)
{
    int id = g_trainerSkillId(handle, edx, index);
    DWORD ra = (DWORD)__builtin_return_address(0);
    /* every caller: the first 2 calls from each return address (which window path runs), and all records that
       are ours or unknown (-1) */
    int first = 0, k = 0;
    for (; k < g_trainerRaN; k++) if (g_trainerRa[k] == ra) break;
    if (k == g_trainerRaN && g_trainerRaN < 16) { g_trainerRa[g_trainerRaN] = ra; g_trainerRaCalls[g_trainerRaN++] = 0; }
    if (k < 16 && g_trainerRaCalls[k]++ < 2) first = 1;
    int ours = 0;
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id) ours = 1;
    if ((ours || id < 0 || first) && g_trainerLogged < 96) {
        BYTE* p = readable((BYTE*)handle + 0x24, 4) ? *(BYTE**)((BYTE*)handle + 0x24) + index * 0x30 : 0;
        char* nm = (p && readable(p, 4)) ? *(char**)p : 0;
        if (nm && !readable(nm, 1)) nm = 0;
        logf("MageSkills: trainer record %d '%s' -> skill id %d (new cap %d, gold %d, list %08X, %d records, return %08X)",
             index, nm ? nm : "?", id,
             p && readable(p + 4, 4) ? *(int*)(p + 4) : -1, p && readable(p + 0x24, 4) ? *(int*)(p + 0x24) : -1,
             (DWORD)handle, readable((BYTE*)handle + 0x24, 8) ?
                 (int)((*(DWORD*)((BYTE*)handle + 0x28) - *(DWORD*)((BYTE*)handle + 0x24)) / 0x30) : -1, ra);
        g_trainerLogged++;
    }
    if (ours && ra == g_trainerRet && g_trainerIdxN < 16) g_trainerIdx[g_trainerIdxN++] = index;
    return id;
}

static void __fastcall trainer_list_trace(void* self, void* edx)
{
    g_trainerIdxN = 0;
    g_trainerList(self, edx);
    /* std::map<int,int> at [self+0x100]: head [+0x18]; node: left +0, parent +4, right +8, key +0xC, value +0x10, isnil +0x15 */
    BYTE* head = readable((BYTE*)self + 0x118, 4) ? *(BYTE**)((BYTE*)self + 0x118) : 0;
    for (int k = 0; k < g_trainerIdxN && head; k++) {
        BYTE* n = readable(head + 4, 4) ? *(BYTE**)(head + 4) : 0;
        int status = -1;
        for (int guard = 0; n && guard < 64 && readable(n, 0x16) && !n[0x15]; guard++) {
            int key = *(int*)(n + 0xC);
            if (key == g_trainerIdx[k]) { status = *(int*)(n + 0x10); break; }
            n = *(BYTE**)(n + (g_trainerIdx[k] < key ? 0 : 8));
        }
        if (g_trainerLogged++ < 96)
            logf("MageSkills: trainer record %d status %d (0 can buy, 1 level/gold too low, 2 hidden)", g_trainerIdx[k], status);
    }
}

/* ---- rank-cap purchases at the trainers ----
   A loaded game does not read rpgstats_skilltrainerlists.xml: the save holds every trainer's whole list
   ("SaveLoadPreMisc - CRpgStats_V2_SkillTrainerManager", 0x6E25A9 -> 0x8A0950 -> 0x8A0640 per trainer). On load
   0x8A0640 empties the list (vector at [handle+0x18], records [+0x24..+0x28), 0x30 bytes each) and push_backs every
   saved record with 0x8A50C0. Record: +0 NiFixedString Skill, +4 NewMaxLevel, +8 std::string Description,
   +0x24 GoldCost, +0x28 bool Available, +0x2C RequiredLevel. After a load we append our purchases the same way to
   every trainer that sells the template skill and does not have ours yet (a new game has them from the XML).
   INI: Trainer<id>=<UUID>,<template UUID>,<shown name>,<cap>,<gold>,<level>[,<cap>,<gold>,<level>...] */
#define MAX_TRAIN 32          /* Trainer<id> of the granted skills + TrainerExtra1..16 (game skills, 03.11) */
#define MAX_TIER 4
typedef void (__fastcall *TrainerSerial_t)(void* handle, void* edx, BYTE* stream);   /* 0x8A0640 */
typedef void (__fastcall *PushRecord_t)(void* vec, void* edx, void* record);          /* 0x8A50C0 */
typedef void (__fastcall *StrCtor_t)(void* s, void* edx, const char* text);           /* basic_string(const char*) */
typedef void (__fastcall *StrDtor_t)(void* s, void* edx);
static TrainerSerial_t g_trainerSerial;
static PushRecord_t    g_pushRecord;
static StrCtor_t       g_strCtor;
static StrDtor_t       g_strDtor;
static struct { char uuid[32], from[32], name[48]; int n, cap[MAX_TIER], gold[MAX_TIER], level[MAX_TIER]; char* fixed, *shown; } g_tr[MAX_TRAIN];
typedef void* (__cdecl *Translate_t)(void* out, char* text, int* category);   /* 0x8E5920 */
static Translate_t g_translate;
static int g_trN, g_trLogged;

static void str_copy(char* d, const char* s, int n)
{
    while (--n > 0 && *s) *d++ = *s++;
    *d = 0;
}

static int str_eq(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void trainer_append(BYTE* handle)
{
    for (int t = 0; t < g_trN; t++) {
        BYTE* p = *(BYTE**)(handle + 0x24), *end = *(BYTE**)(handle + 0x28);
        /* 03.11: per purchase, not per skill: a save made before a new cap was added (Ice Bolt 13) already has the
           others, and the new one is still appended */
        int sells = 0, have[MAX_TIER] = {0}, added = 0;
        for (; p && p < end; p += 0x30) {
            char* nm = *(char**)p;
            if (!nm || !readable(nm, 1)) continue;
            if (str_eq(nm, g_tr[t].from)) sells = 1;
            /* a full ladder (3+ caps): the game's odd lower cap (the expansion's 7/8) becomes the first step in place
               (the scripts open the game's purchases by record number: no record may move) */
            if (str_eq(nm, g_tr[t].uuid) && g_tr[t].n >= 3 && *(int*)(p + 4) < g_tr[t].cap[0]) {
                *(int*)(p + 4) = g_tr[t].cap[0];
                p[0x28] = 1;
            }
            if (str_eq(nm, g_tr[t].uuid))
                for (int k = 0; k < g_tr[t].n; k++)
                    if (*(int*)(p + 4) == g_tr[t].cap[k]) {
                        have[k] = 1;
                        /* a save may hold it with the price of an older build: the ini's gold and level win */
                        *(int*)(p + 0x24) = g_tr[t].gold[k];
                        *(int*)(p + 0x2C) = g_tr[t].level[k];
                    }
        }
        if (!sells) continue;
        if (!g_tr[t].fixed) g_tr[t].fixed = g_fixed(g_tr[t].uuid);      /* kept for the process */
        for (int k = 0; k < g_tr[t].n; k++) {
            if (have[k]) continue;
            added++;
            char text[128];
            wsprintfA(text, k == 0 ? "You will be able to study %s in more detail." :
                            k == g_tr[t].n - 1 ? "You will be able to become a master of %s" :
                                                 "You will be able to take %s to new extremes", g_tr[t].name);
            BYTE rec[0x30];
            memset(rec, 0, sizeof rec);
            *(char**)rec = g_tr[t].fixed;
            *(int*)(rec + 4) = g_tr[t].cap[k];
            g_strCtor(rec + 8, 0, text);
            *(int*)(rec + 0x24) = g_tr[t].gold[k];
            rec[0x28] = 1;                       /* scripts unlock the game's own tiers by index; ours are always open */
            *(int*)(rec + 0x2C) = g_tr[t].level[k];
            g_pushRecord(handle + 0x18, 0, rec);
            g_strDtor(rec + 8, 0);
        }
        /* the trainer offers a skill's purchases in list order (after 10 the next record of the skill): keep ours
           sorted by cap. 03.11: a save had Ice Bolt 10, 15 and then the new 13, so 15 came right after 10.
           A record is moved as raw bytes (its std::string holds no pointer to itself) */
        if (added) {
            BYTE* b = *(BYTE**)(handle + 0x24), *e = *(BYTE**)(handle + 0x28);
            BYTE* mine[16]; int m = 0;
            for (BYTE* q = b; q < e && m < 16; q += 0x30)
                if (*(char**)q && readable(*(char**)q, 1) && str_eq(*(char**)q, g_tr[t].uuid)) mine[m++] = q;
            for (int x = 1; x < m; x++)                    /* insertion sort of the contents over the same slots */
                for (int y = x; y > 0 && *(int*)(mine[y - 1] + 4) > *(int*)(mine[y] + 4); y--) {
                    BYTE tmp[0x30];
                    memcpy(tmp, mine[y - 1], 0x30); memcpy(mine[y - 1], mine[y], 0x30); memcpy(mine[y], tmp, 0x30);
                }
        }
        if (added && g_trLogged++ < 16)
            logf("MageSkills: trainer list %08X: added %d purchase(s) of '%s' after the save loaded (%d records now)",
                 (DWORD)handle, added, g_tr[t].uuid,
                 (int)((*(DWORD*)(handle + 0x28) - *(DWORD*)(handle + 0x24)) / 0x30));
    }
}

/* the trainer row's name: 0x89EE80 translates the record's Skill string (the UUID, "Chain Hit"); the English game has
   no translation table and showed the UUID. Give the translator the shown name instead ("Ice Bolt": English as is,
   Russian from Skills_Localisation.xml). Patched call at 0x89EEA6 (only 0x89EE80, the trainer rows, uses it). */
static void* __cdecl trainer_name(void* out, char* text, int* category)
{
    for (int t = 0; t < g_trN && text; t++)
        if (str_eq(text, g_tr[t].uuid)) {
            if (!g_tr[t].shown) g_tr[t].shown = g_fixed(g_tr[t].name);    /* kept for the process */
            text = g_tr[t].shown;
            break;
        }
    return g_translate(out, text, category);
}

static void __fastcall trainer_serial(void* handle, void* edx, BYTE* stream)
{
    g_trainerSerial(handle, edx, stream);
    if (handle && stream && stream[0x40]) trainer_append((BYTE*)handle);   /* [stream+0x40] = loading (0x8A0655) */
}

static void trainer_init(Module m)
{
    for (int i = 0; i < g_n + 16; i++) {
        char key[24], v[160];
        if (i < g_n) wsprintfA(key, "Trainer%d", g_ids[i]);
        else wsprintfA(key, "TrainerExtra%d", i - g_n + 1);    /* a cap of a game skill: <UUID>,<UUID>,<UUID>,<cap>,... */
        GetPrivateProfileStringA("MageSkills", key, "", v, sizeof v, g_ini);
        if (!v[0] || g_trN >= MAX_TRAIN) continue;
        char* f[3 + 3 * MAX_TIER]; int nf = 0;
        for (char* s = v; *s && nf < 3 + 3 * MAX_TIER; ) {
            while (*s == ' ') s++;
            f[nf++] = s;
            while (*s && *s != ',') s++;
            if (*s) *s++ = 0;
        }
        if (nf < 6 || (nf - 3) % 3) { logf("MageSkills: %s: expected uuid,template,name,cap,gold,level[,...] -> ignored", key); continue; }
        str_copy(g_tr[g_trN].uuid, f[0], 32); str_copy(g_tr[g_trN].from, f[1], 32); str_copy(g_tr[g_trN].name, f[2], 48);
        int n = 0;
        for (int k = 3; k + 2 < nf; k += 3, n++) {
            g_tr[g_trN].cap[n] = 0; g_tr[g_trN].gold[n] = 0; g_tr[g_trN].level[n] = 0;
            for (char* c = f[k]; *c >= '0' && *c <= '9'; c++) g_tr[g_trN].cap[n] = g_tr[g_trN].cap[n] * 10 + *c - '0';
            for (char* c = f[k + 1]; *c >= '0' && *c <= '9'; c++) g_tr[g_trN].gold[n] = g_tr[g_trN].gold[n] * 10 + *c - '0';
            for (char* c = f[k + 2]; *c >= '0' && *c <= '9'; c++) g_tr[g_trN].level[n] = g_tr[g_trN].level[n] * 10 + *c - '0';
        }
        g_tr[g_trN].n = n;
        logf("MageSkills: trainers selling '%s' get %d purchase(s) of '%s' (first: cap %d, %d gold, level %d)",
             g_tr[g_trN].from, n, g_tr[g_trN].uuid, g_tr[g_trN].cap[0], g_tr[g_trN].gold[0], g_tr[g_trN].level[0]);
        g_trN++;
    }
    if (!g_trN) return;
    DWORD d = (DWORD)m.base - 0x400000;
    /* 8A0640: sub esp,4Ch / mov eax,[14D10B0]; 8A50C0: sub esp,8 / push ebx / push esi / mov esi,ecx / mov ebx,[esi+0Ch];
       the string import pair as used at 0x6E25B2 (ctor from const char*) / 0x6E25C9 (dtor); 4B34D0 NiFixedString */
    int ok = same(m, 0x8A0640, "83ec4ca1") && *(DWORD*)VA(m, 0x8A0644) == 0x14D10B0 + d &&
             same(m, 0x8A0655, "8a4640") && same(m, 0x8A50C0, "83ec0853568bf18b5e0c") &&
             same(m, 0x8A08CE, "8d4d18e8") &&
             same(m, 0x6E25B2, "ff15") && *(DWORD*)VA(m, 0x6E25B4) == 0x12B758C + d &&
             same(m, 0x6E25C9, "ff15") && *(DWORD*)VA(m, 0x6E25CB) == 0x12B7594 + d &&
             same(m, 0x4B34D0, "83ec0c538b5c2414");
    if (!ok) { logf("MageSkills: trainer code mismatch at 0x8A0640/0x8A50C0/0x6E25B2 -> no trainer purchases"); g_trN = 0; return; }
    g_strCtor = *(StrCtor_t*)VA(m, 0x12B758C);
    g_strDtor = *(StrDtor_t*)VA(m, 0x12B7594);
    g_pushRecord = (PushRecord_t)VA(m, 0x8A50C0);
    g_fixed = (Fixed_t)VA(m, 0x4B34D0);
    g_trainerSerial = (TrainerSerial_t)detour(VA(m, 0x8A0640), (void*)trainer_serial);
    logf("MageSkills: trainer purchases %s (0x8A0640, after a save loads)", g_trainerSerial ? "on" : "NOT installed");
    if (!g_trainerSerial) { g_trN = 0; return; }
    g_translate = (Translate_t)VA(m, 0x8E5920);
    /* 89EEA6: call 8E5920 (push ecx / push edx / push esi before it); 8E5920: mov eax,[esp+0Ch] / mov ecx,[esp+8] */
    int okName = same(m, 0x8E5920, "8b44240c8b4c2408") && same(m, 0x89EEA3, "515256") &&
                 patch_call(VA(m, 0x89EEA6), VA(m, 0x8E5920), (void*)trainer_name);
    logf("MageSkills: trainer row names %s (0x89EEA6)", okName ? "shown by name" : "NOT patched (rows show the UUID)");
}

/* ---- our skills on dragon code are human skills ----
   The prototype loader 0x8A0B00 sets flag bytes from the behaviour id [proto+0x24] (CodePath): [proto+0x5A] = 1 for
   the dragon and zeppelin skills (0x31-0x36, 0x37, 0x38, 0x39, 0x51-0x54; list at 0x8A1066). With it a skill does
   not go on the human hotbar and is not raised past rank 1 (Frost Nova on Dragon Burst code, 01.11). After the
   prototype is registered under its id (0x8A4380 thiscall(proto, id, name) ret 0Ch) the flag is cleared for our ids. */
typedef BYTE (__fastcall *Register_t)(void* self, void* edx, BYTE* proto, int id, char** name);
static Register_t g_register;

static BYTE __fastcall human_register(void* self, void* edx, BYTE* proto, int id, char** name)
{
    BYTE ok = g_register(self, edx, proto, id, name);
    /* 04.11 diagnostics (main game saves lack our skills): every build of the prototype table */
    static int regCount, regLast = -1, regLogged;
    BYTE* mgrNow = *g_protoMgr;
    regCount++;
    if (id < regLast && regLogged < 12) { regLogged++; logf("MageSkills: prototype table: a new pass after %d registrations (id %d after %d, this %p, table %p)", regCount, id, regLast, self, mgrNow); regCount = 0; }
    regLast = id;
    if (id >= 76 && regLogged < 60) { regLogged++; logf("MageSkills: prototype id %d registered (this %p, table %p, '%s')", id, self, mgrNow, name && *name ? *name : "?"); }
    if (proto && ours(id) && readable(proto + 0x5A, 1) && proto[0x5A]) {
        proto[0x5A] = 0;
        logf("MageSkills: skill %d (behaviour %d) is a human skill: dragon flag [proto+5Ah] cleared", id,
             readable(proto + 0x24, 4) ? *(int*)(proto + 0x24) : -1);
    }
    return ok;
}

/* 07.11 diagnostics (the original prototypes now and then, a crash at 0x89F9B0 on load): 0x8A1360 thiscall(asset) ret 4
   reads the 'Skills' asset ([asset+10h] entries of 2Ch bytes, [asset+14h] count), each name -> id by 0x89D560, an
   unknown name (-1) is thrown away. The count tells whose file it is (ours has more entries). */
typedef BYTE (__fastcall *ProtoLoad_t)(void* self, void* edx, BYTE* asset);
static ProtoLoad_t g_protoLoad;
static int g_protoSeen;
static BYTE __fastcall proto_load(void* self, void* edx, BYTE* asset)
{
    g_protoSeen = 1;
    int n = asset && readable(asset + 0x10, 8) ? *(int*)(asset + 0x14) : -1;
    logf("MageSkills: prototype asset %p: %d entries (caller %X)", asset, n, (DWORD)__builtin_return_address(0));
    if (asset && readable(asset, 0x40))
        for (int k = 0; k < 0x40; k += 4) {
            char* s = *(char**)(asset + k);
            if (s && readable(s, 8) && s[0] >= 0x20 && s[0] < 0x7F && s[1] >= 0x20 && s[1] < 0x7F) logf("MageSkills:   asset +%02X: '%.60s'", k, s);
        }
    return g_protoLoad(self, edx, asset);
}

/* 07.11 diagnostics: which archive the prototypes are read from. The stored (zlib) sizes of
   Episodes\Episode_2\rpgstats_skillprototypes.xml: MageExpansion 33026, FOV_Patch_2 27510, MainDataStartup 27534;
   Episode_1_Extended: logged by size range. [MageSkills] ReadTrace=1 logs every ReadFile of 20..40 KB (first 200)
   with the file and its position. */
typedef BOOL (__stdcall *ReadFile_t)(HANDLE, void*, DWORD, DWORD*, void*);
typedef DWORD (__stdcall *FinalPath_t)(HANDLE, char*, DWORD, DWORD);
typedef DWORD (__stdcall *SetPtr_t)(HANDLE, LONG, LONG*, DWORD);
static ReadFile_t g_readReal;
static FinalPath_t g_finalPath;
static SetPtr_t g_setPtr;
static int g_readLogged;
static BOOL __stdcall read_trace(HANDLE h, void* buf, DWORD n, DWORD* got, void* ov)
{
    /* until the prototypes are read: every read of the three archives that hold them, any size */
    if (!g_protoSeen && g_readLogged < 400) {
        char path[300]; path[0] = 0;
        if (g_finalPath) g_finalPath(h, path, sizeof path, 0);
        int k = s_len(path), b = k;
        while (b > 0 && path[b - 1] != '\\') b--;
        const char* f = path + b;
        if ((f[0] == 'M' && (f[4] == 'E' || f[8] == 'S' || (f[2] == 'g' && f[4] == 'D'))) || (f[0] == 'F' && f[3] == '_' && f[10] == '2')) {   /* MageExpansion, MainData(S...), FOV_Patch_2 */
            LONG hi = 0; DWORD pos = g_setPtr && !ov ? g_setPtr(h, 0, &hi, 1) : 0xFFFFFFFF;
            g_readLogged++;
            logf("ReadTrace: %u bytes at %u of %s%s (thread %u)", n, pos, f, ov ? " (overlapped)" : "", GetCurrentThreadId());
        }
    }
    return g_readReal(h, buf, n, got, ov);
}

static void human_init(Module m)
{
    if (GetPrivateProfileIntA("MageSkills", "ReadTrace", 0, g_ini)) {
        HMODULE kb = GetModuleHandleA("kernelbase.dll");
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        void* rf = kb ? (void*)GetProcAddress(kb, "ReadFile") : 0;
        g_finalPath = k32 ? (FinalPath_t)GetProcAddress(k32, "GetFinalPathNameByHandleA") : 0;
        g_setPtr = k32 ? (SetPtr_t)GetProcAddress(k32, "SetFilePointer") : 0;
        g_readReal = rf ? (ReadFile_t)detour(rf, (void*)read_trace) : 0;
        logf("MageSkills: read trace %s", g_readReal ? "on (kernelbase ReadFile)" : "OFF: detour failed");
    }
    /* 8A1360: sub esp,8 / push ebp / mov ebp,[esp+10h] / push esi (checked in the exe 07.11) */
    if (same(m, 0x8A1360, "83ec08558b6c241056")) g_protoLoad = (ProtoLoad_t)detour(VA(m, 0x8A1360), (void*)proto_load);
    logf("MageSkills: prototype asset trace %s", g_protoLoad ? "on (0x8A1360)" : "OFF");
    /* 8A4380: sub esp,20h / push ebx / mov ebx,[esp+2Ch]; 8A10B0: mov [edx+5Ah],cl (the flag written by the loader) */
    if (!same(m, 0x8A4380, "83ec20538b5c242c") || !same(m, 0x8A10B0, "884a5a")) {
        logf("MageSkills: prototype register code differs -> dragon flags of our skills left as they are");
        return;
    }
    g_register = (Register_t)detour(VA(m, 0x8A4380), (void*)human_register);
    if (!g_register) logf("MageSkills: prototype register hook NOT installed");
}

/* ---- own skills (no other skill's code): Own<id>=1, the prototype's CodePath is "Newt" (behaviour 0x39) ----
   The hero's cast 0x923FC0 thiscall(id) ret 4 switches on the behaviour at 0x924440; 0x39 has no branch there
   (0x92599B, nothing happens). For our own skills the cast is taken here instead: the character's stats
   (0x8FD5A0 on the controller), the game's own check 0x763AD0(id) (mana, form, "out_of_mana"), the mana cost
   0x764330(id) and the cooldown of the character's skill object 0x89EC50 (skill+8 = CooldownTime of the rank), the
   same three calls the Fireball spell makes at 0x969D2A/0x969D46. Stage 2: no effect yet, a line in the log. */
typedef void* (__fastcall *Stats0_t)(void* self, void* edx);
typedef BYTE  (__fastcall *Skill1b_t)(void* stats, void* edx, int id);
typedef void  (__fastcall *Cooldown_t)(void* skill, void* edx);
typedef BYTE  (__fastcall *HeroCast_t)(void* self, void* edx, int id);
static HeroCast_t g_heroCast;
static Stats0_t   g_ctrlStats;
static Skill1b_t  g_canUse0, g_pay0;
static Cooldown_t g_cooldown;
static int g_own[MAX_GRANT], g_ownCasts;

static int own_skill(int id)
{
    for (int i = 0; i < g_n; i++)
        if (g_ids[i] == id && g_own[i]) {
            BYTE* mgr = *g_protoMgr;
            BYTE** begin = mgr && readable(mgr + 0x10, 8) ? *(BYTE***)(mgr + 0x10) : 0;
            BYTE* proto = begin && readable(begin + id, 4) ? begin[id] : 0;
            return proto && readable(proto + 0x24, 4) && *(int*)(proto + 0x24) == 0x39;
        }
    return 0;
}

/* stage 3: characters around the caster. The character stats table of [0x14F13C4] (+0x30 begin, +0x3C count, the
   index is the handle, as 0x818330 reads it); 0x755500(stats) -> the position (x, y, z) of its game object;
   0x756250 active (game object flags 0x2000|0x4000), 0x754E50 game object flag bit 21 (skipped by the game's own
   area search 0x724E30, there with 0x7563B0 bit 12); relation 0x886BF0 thiscall(alignment of the searcher,
   alignment of the other) ret 8 on [0x14F17E8], alignment = [stats+0x144]; 0x724E30 counts 50..100 as friends. */
typedef float* (__fastcall *Pos_t)(void* stats, void* edx);
typedef BYTE   (__fastcall *Flag_t)(void* stats, void* edx);
typedef int    (__fastcall *Relation_t)(void* mgr, void* edx, int a, int b);
static Pos_t g_pos;
static Flag_t g_active, g_bit21, g_dead, g_bit15;   /* g_dead: 0x7563B0, Lua IsDead (03.11) */
static Relation_t g_relation;
static void** g_alignMgr;

static float own_sqrt(float v)
{
    if (v <= 0) return 0;
    float r = v > 1 ? v : 1;
    for (int k = 0; k < 24; k++) r = 0.5f * (r + v / r);
    return r;
}

/* stage 4: magic damage as the game's own spells deal it (the damage tick at 0x962030):
   0x7B9C00 thiscall(attacker handle, min, max, 0, 1, attacker level) ret 18h on the formula manager [0x14F1828]
   (intelligence and level difference mods, random min..max) -> magic damage; the hit 0x981120 (0x38 bytes:
   +8 magic damage, +0x14 = 3, +0x18 attacker handle, +0x1C = 0, +0x20 = 1, +0x24 attacker position) given to the
   target's 0x76C800(&hit) ret 4. Level 0x751CF0(stats); rank params prototype.0x89E930(rank, index) ret 8. */
typedef float (__fastcall *MagicDmg_t)(void* mgr, void* edx, int attacker, float min, float max, int a, int b, int level);
typedef void  (__fastcall *HitCtor_t)(void* hit, void* edx);
typedef BYTE  (__fastcall *TakeHit_t)(void* stats, void* edx, void* hit);
typedef int   (__fastcall *Level_t)(void* stats, void* edx);
typedef float (__fastcall *Param_t)(void* proto, void* edx, int rank, int index);
typedef int   (__fastcall *Rank_t)(void* skill, void* edx);
static MagicDmg_t g_magicDmg;
static HitCtor_t  g_hitCtor;
static TakeHit_t  g_takeHit;
static Level_t    g_level;
/* 07.11: 0x751CF0 reads the level from the attribute table [stats+100h], which the hero does not have (0 for him: the
   magic damage of our skills was figured for level 0); 0x751910 (mov eax,[ecx+16Ch] / ret) gives his level (43 = 43) */
static Level_t    g_heroLevel;
static int caster_level(void* st)
{
    int lv = g_heroLevel ? g_heroLevel(st, 0) : 0;
    if (lv <= 0 || lv > 100) lv = g_level ? g_level(st, 0) : 1;
    return lv > 0 ? lv : 1;
}
static Param_t    g_param;
static Rank_t     g_rank;
static void**     g_formulaMgr;

static void hit_effect(void* st, int i);                /* below: the effect on a character hit by own skill i */
static void push_start(void* hero, void* st, int i);    /* below: Arcane Push */
static int status_start(void* stats, char** slot, const char* name, DWORD ms, int repeat);
static int own_enemy(void* hero, void* st);            /* below: alive, on the stage, hostile */

static int own_targets(void* hero, float radius, float damage, int log, int fx)
{
    BYTE* mgr = *(BYTE**)g_statsMgr;
    if (!mgr || !readable(mgr + 0x30, 0x10)) return 0;
    void** table = *(void***)(mgr + 0x30);
    int count = *(int*)(mgr + 0x3C);
    float* hp = g_pos(hero, 0);
    if (!table || !hp || count <= 0 || count > 100000) return 0;
    int heroHandle = *(int*)((BYTE*)hero + 0x144), hit = 0;
    int level = caster_level(hero);
    for (int i = 0; i < count && hit < 32; i++) {
        void* st = readable(table + i, 4) ? table[i] : 0;
        if (!st || st == hero || !readable((BYTE*)st + 0x148, 4)) continue;
        float* p = g_pos(st, 0);
        if (!p) continue;
        float dx = p[0] - hp[0], dy = p[1] - hp[1], dz = p[2] - hp[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > radius * radius) continue;
        if (!own_enemy(hero, st)) continue;            /* the game's own target rules (03.11, Chain Lightning) */
        int rel = (g_alignMgr && *g_alignMgr) ? g_relation(*g_alignMgr, 0, heroHandle, *(int*)((BYTE*)st + 0x144)) : 100;
        float dmg = (g_magicDmg && *g_formulaMgr) ? g_magicDmg(*g_formulaMgr, 0, heroHandle, damage, damage, 0, 1, level) : 0;
        BYTE info[0x38];
        g_hitCtor(info, 0);
        *(float*)(info + 8) = dmg;
        *(int*)(info + 0x14) = 5;   /* a magic hit: Destruction applies to 2 and 5 (0x98129C), 05.11 */
        *(int*)(info + 0x18) = heroHandle;
        *(int*)(info + 0x1C) = 0;
        *(int*)(info + 0x20) = 1;
        *(float*)(info + 0x24) = hp[0]; *(float*)(info + 0x28) = hp[1]; *(float*)(info + 0x2C) = hp[2];
        info[0x32] = 1;          /* a magic hit: 0x76CA87 -> 0x767FB0, the attacker's Confusion may stun (05.11) */
        g_takeHit(st, 0, info);
        if (fx >= 0) hit_effect(st, fx);
        push_start(hero, st, fx);
        if (log)
            logf("MageSkills:   hit character %d at %d.%02d m (relation %d): magic damage %d (base %d, level %d)", i,
                 (int)own_sqrt(d2), (int)(own_sqrt(d2) * 100) % 100, rel, (int)dmg, (int)damage, level);
        hit++;
    }
    return hit;
}

/* stage 5a: our own effect on the caster, the way the Fireball cast shows its flash (0x95E0A0): the character's effect
   component (type 0x14F2FAC: [stats+0xC4] -> 0x74FF40(type) -> 0x69FBA0(type)), 0x741140 thiscall(NiFixedString name,
   f32 time, int, vec3 [0x14EF5CC], vec3 [0x14EF900], 3x3 [0x14CD590]) ret 48h sets the effect (the name is passed by
   value: one reference for the callee, as 0x95E85E adds it), 0x74FD10 thiscall(component, int* out) ret 8 on the
   component container starts it. The effect itself is MageExpansion\FrostNova_Burst.item (made by fx_frostnova.py). */
typedef struct { float v[3]; } Vec3;
typedef struct { float m[9]; } Mat33;
typedef void* (__fastcall *CompEntry_t)(void* container, void* edx, void* type);
typedef void* (__fastcall *CompCast_t)(void* entry, void* edx, void* type);
typedef void  (__fastcall *SetFx_t2)(void* comp, void* edx, char* name, float time, int flag, Vec3 a, Vec3 b, Mat33 m);
typedef BYTE  (__fastcall *Start_t)(void* container, void* edx, void* comp, int* out);
typedef long  (__stdcall *AddRef_t)(void* counter);
static CompEntry_t g_compEntry;
typedef BYTE (__fastcall *MoveEnds_t)(void* comp, void* edx);                 /* 0x739750 */
typedef void (__fastcall *MoveStop_t)(void* comp, void* edx, int stop);       /* 0x739770: [comp+28h] = stop */
static void*       g_moveType;
static MoveEnds_t  g_moveEnds;
static MoveStop_t  g_moveStop;
static CompCast_t  g_compCast;
static SetFx_t2    g_setCasterFx;
static Start_t     g_startComp;
static AddRef_t    g_addRef;
static void*       g_fxType;
static Vec3*       g_fxVecA;
static Vec3*       g_fxVecB;
static Mat33*      g_fxMat;
static char        g_ownFxName[MAX_GRANT][128], g_ownAnim[MAX_GRANT][48];
typedef void (__fastcall *SetAnim_t)(void* comp, void* edx, void* str);      /* 0x739AF0: [comp+1Ch] = string */
typedef void (__fastcall *SetFlag_t)(void* comp, void* edx, int flag);       /* 0x739B00: [comp+40h] = flag */
static SetAnim_t g_setAnim;
static SetFlag_t g_setAnimFlag;
static StrCtor_t g_animStrCtor;
static StrDtor_t g_animStrDtor;
static char*       g_ownFx[MAX_GRANT];
static int         g_ownFxStarted;
/* 03.11: OwnHitFx<id> = effect on every character the skill hits; OwnDelay<id> = ms from the cast to the hit (the
   "Cast" key of the animation); OwnSound<id> = a WAV next to the game (bin), played with the cast (PlaySound: not 3D,
   not under the game's volume) */
static char        g_ownHitFxName[MAX_GRANT][48];
static char*       g_ownHitFx[MAX_GRANT];
static int         g_ownDelay[MAX_GRANT], g_hitFxLogged;
static BYTE*       g_ownSound[MAX_GRANT], *g_ownSoundF[MAX_GRANT];
typedef BOOL (__stdcall *PlaySound_t)(const void* sound, HMODULE mod, DWORD flags);
static PlaySound_t g_playSound;
static struct { int state, hero, i, id; float damage, radius; DWORD due; } g_pend[8];
/* OwnHold<id> = ms: keep the caster's effect component alive this long after the cast. The per-frame update
   0x739B10 counts [comp+18h] down and stops the component (and its effect) at 0; 03.11: the burst after the "Cast" key
   was cut off, so own_tick keeps [comp+18h] at the time left until the end of the animation */
static int g_ownHold[MAX_GRANT], g_holdHero, g_holdLogged;
static DWORD g_holdUntil, g_holdFrom;
static int g_ownLock[MAX_GRANT], g_lockHero;           /* OwnLock<id> = ms the cast action may not be replaced */
static DWORD g_lockUntil;

/* the character's effect component, or 0 */
static void* fx_component(void* stats, void** container)
{
    *container = readable((BYTE*)stats + 0xC4, 4) ? *(void**)((BYTE*)stats + 0xC4) : 0;
    if (!*container) return 0;
    void* entry = g_compEntry(*container, 0, g_fxType);
    return entry ? g_compCast(entry, 0, g_fxType) : 0;
}

static void hit_effect(void* st, int i)
{
    if (!g_ownHitFxName[i][0]) return;
    /* 04.11: a free effect moved with the target (status_start); the old way only without the effect manager */
    if (status_start(st, &g_ownHitFx[i], g_ownHitFxName[i], 2400, 0)) return;
    if (!g_setCasterFx) return;
    char* name = fx_string(&g_ownHitFx[i], g_ownHitFxName[i]);
    void* container;
    void* comp = name ? fx_component(st, &container) : 0;
    if (!comp) { if (g_hitFxLogged++ < 8) logf("MageSkills:   hit effect: the target has no effect component"); return; }
    Vec3 zero = {{0, 0, 0}};
    Mat33 orient = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    g_addRef(name - 8);
    /* attach mode 0: the target itself; the first vector is the global, as in own_effect (else a lit disc) */
    g_setCasterFx(comp, 0, name, 0.0f, 0, *g_fxVecA, zero, orient);
    static int out;
    BYTE ok = g_startComp(container, 0, comp, &out);
    if (g_hitFxLogged++ < 8) logf("MageSkills:   hit effect '%s' on the target -> started %d", g_ownHitFxName[i], ok);
}

static void own_effect(void* stats, int i, int rank)
{
    if (!g_setCasterFx || !g_ownFxName[i][0]) return;
    /* OwnFx<id> may list several effects ("A,B,C"): each cast takes the next one (03.11: test variants) */
    static int cycle[MAX_GRANT];
    static char* slots[MAX_GRANT][4];
    char pick[48];
    const char* list = g_ownFxName[i];
    int n = 1;
    for (const char* c = list; *c; c++) n += *c == ',';
    if (n > 4) n = 4;
    /* "A@1,B@6,C@11": by rank instead (the entry with the highest rank not above the skill's; 03.11 the radius) */
    int k = -1, best = -1, len = 0;
    const char* c = list;
    for (int e = 0; e < n; e++) {
        const char* at = c;
        while (*at && *at != ',' && *at != '@') at++;
        if (*at == '@') {
            int r = 0;
            for (const char* q = at + 1; *q >= '0' && *q <= '9'; q++) r = r * 10 + *q - '0';
            if (r <= rank && r > best) { best = r; k = e; }
        }
        while (*c && *c != ',') c++;
        if (*c) c++;
    }
    if (k < 0) k = cycle[i]++ % n;
    for (int at = 0; at < k; list++) if (*list == ',') at++;
    while (list[len] && list[len] != ',' && list[len] != '@' && len < 47) { pick[len] = list[len]; len++; }
    pick[len] = 0;
    char* name = fx_string(&slots[i][k], pick);
    void* container;
    void* comp = name ? fx_component(stats, &container) : 0;
    if (!name || !container) return;
    if (!comp) { if (g_ownFxStarted++ < 4) logf("MageSkills: own effect: the caster has no effect component"); return; }
    if (g_ownAnim[i][0] && g_setAnim) {               /* the cast animation of the same action, as 0x95E16C does */
        BYTE str[0x20];
        g_animStrCtor(str, 0, g_ownAnim[i]);
        g_setAnim(comp, 0, str);
        /* [comp+40h]: the per-frame update 0x739B10 stops the component when its timer [comp+18h] runs out, unless this
           flag is set and the effect still plays (vtable +28h). Fireball passes 0. 03.11: 1 kept the component alive,
           but the hero then repeated the cast animation until the effect ended; so 0, and the effect fits in the
           animation (fx_frostnova DURATION = anim_frostnova DURATION) */
        g_setAnimFlag(comp, 0, 0);
        g_animStrDtor(str, 0);
    }
    /* Fireball's cast path fills these globals with the projectile's transform.  An own
       skill has no projectile, so those values may be stale or point away from the hero.
       Place Frost Nova at the caster and use an identity orientation. */
    Vec3 at = *g_fxVecA, to = *g_fxVecB;
    Mat33 orient = *g_fxMat;
    if (g_pos) {
        float* p = g_pos(stats, 0);
        if (p) {
            /* 03.11: the first vector stays the global [14EF5CC], as the Fireball flash passes it: when [obj+50h]
               differs from it, the attach 0x9827B0 also calls the instance's 0x8100E0(7, ...), which lit a bright disc
               on the ground around the caster (any effect, even the game's FireBall_Cast, launched our way) */
            to.v[0] = p[0];
            to.v[1] = p[1];
            to.v[2] = p[2];
            orient.m[0] = 1.0f; orient.m[1] = 0.0f; orient.m[2] = 0.0f;
            orient.m[3] = 0.0f; orient.m[4] = 1.0f; orient.m[5] = 0.0f;
            orient.m[6] = 0.0f; orient.m[7] = 0.0f; orient.m[8] = 1.0f;
        }
    }
    g_addRef(name - 8);                               /* the callee releases the copy it gets */
    /* The original Fireball path passes 0.0 here (the effect's start time).  Passing
       the duration would make FrostNova_Burst, whose lifetime is 1.6 s, expire on
       creation. */
    /* the int is the attach mode (switch 0x9824DC, table 0x982750): 1 = bone Dummy_Cast_Primary (the hand, as the
       Fireball flash), 0 = the character itself (its position and facing), 7 = the given transform */
    g_setCasterFx(comp, 0, name, 0.0f, 0, at, to, orient);
    static int out;
    BYTE ok = g_startComp(container, 0, comp, &out);
    /* lock the action for the animation: 0x8FD5C0 (may the current action be replaced?) answers no while [comp+0Ch] is
       set. 03.11: cast while running, the held run key replaced it at once (the hero twitched, no effect); own_tick
       clears it OwnLock<id> ms later */
    if (ok && g_ownLock[i] > 0 && readable((BYTE*)comp + 0xC, 1)) {
        *((BYTE*)comp + 0xC) = 1;
        g_lockHero = *(int*)((BYTE*)stats + 0x144);
        g_lockUntil = GetTickCount() + (DWORD)g_ownLock[i];
    }
    if (g_ownFxStarted++ < 40) logf("MageSkills: own effect '%s', animation '%s' on the caster -> started %d", pick,
                                   g_ownAnim[i][0] ? g_ownAnim[i] : "-", ok);
}

/* 03.11 Chain Lightning (own skill on the free id 0x1C): OwnChain<id> = <percent less per leap>,<ms per leap>,<metres
   to the first enemy>. The first strike goes to the nearest enemy within that range of the caster, then the bolt
   leaps from each struck enemy to the nearest one not struck yet within Param3 metres, Param1 more times (own_tick,
   one leap per step). OwnBeam<id> = <effect>@<cm>,... the beam variants (tools/fx_chain.py, the flying tower's
   lightning cut to length): the one closest to the leap is placed on the struck enemy's effect component with attach
   mode 7 (switch 0x9824DC -> 0x9824E3: [entry+14h] position = the second vector of 0x741140, [entry+20h] rotation),
   at the point the bolt comes from, +X turned towards the enemy. */
typedef void* (__stdcall *Obj_t)(int handle);                                /* 0x8C9330 */
static Obj_t g_obj;                     /* set by summon_init */
#define CHAIN_MAX 8
static int   g_chainFall[MAX_GRANT], g_chainStep[MAX_GRANT], g_chainRange[MAX_GRANT];
#define MAX_BEAMS 32
static char  g_beamName[MAX_GRANT][MAX_BEAMS][40];
static int   g_beamCm[MAX_GRANT][MAX_BEAMS], g_beamN[MAX_GRANT];
static char* g_beamFixed[MAX_GRANT][MAX_BEAMS];
static int   g_chainLogged, g_chainSwap;
static struct {
    int active, hero, i, left, hits[CHAIN_MAX + 1], nhits, first;
    float damage, radius, from[3];
    DWORD due;
} g_chain;

/* an enemy of the hero that a strike may hit: active, not flagged, relation below 50 (0x724E30's friends) */
static int own_enemy(void* hero, void* st)
{
    if (!st || st == hero || !readable((BYTE*)st + 0x148, 4)) return 0;
    if (!g_active(st, 0) || g_bit21(st, 0)) return 0;
    /* 03.11: the chain struck empty places - corpses: the game's own area search skips them too (0x7563B0 bit 12) */
    if (g_dead && g_dead(st, 0)) return 0;
    /* 03.11: still struck corpses - the IsDead flag comes later: vitality [stats+18Ch] (0x75EAD0 clamps it) */
    if (readable((BYTE*)st + 0x18C, 4) && *(float*)((BYTE*)st + 0x18C) <= 0.0f) return 0;
    /* 03.11: leaps into empty places with damage numbers there - characters off the stage (hidden, not yet put in by a
       script). The game's own area search 0x724E30 takes only these: the object of vtable+6Ch with byte +2 set
       (0x724EDA..0x724EF9, 0x94FA70), and an enemy (relation < 50) with flag bit 15 (0x724FE5: 0x7565C0) */
    typedef void* (__fastcall *Sub_t)(void* self, void* edx);
    void** vt = readable(st, 4) ? *(void***)st : 0;
    if (vt && readable(vt + 0x1B, 4)) {
        BYTE* sub = (BYTE*)((Sub_t)vt[0x1B])(st, 0);
        if (sub && readable(sub + 2, 1) && !sub[2]) return 0;
    }
    int rel = (g_alignMgr && *g_alignMgr) ? g_relation(*g_alignMgr, 0, *(int*)((BYTE*)hero + 0x144), *(int*)((BYTE*)st + 0x144)) : 100;
    if (rel < 50 && g_bit15 && !g_bit15(st, 0)) return 0;
    return rel < 50;
}

/* one magic hit as own_targets deals it; returns the damage dealt */
static float own_hit(void* hero, void* st, float damage, const float* from)
{
    int heroHandle = *(int*)((BYTE*)hero + 0x144);
    int level = caster_level(hero);
    float dmg = (g_magicDmg && *g_formulaMgr) ? g_magicDmg(*g_formulaMgr, 0, heroHandle, damage, damage, 0, 1, level) : 0;
    BYTE info[0x38];
    g_hitCtor(info, 0);
    *(float*)(info + 8) = dmg;
    *(int*)(info + 0x14) = 5;   /* a magic hit: Destruction applies to 2 and 5 (0x98129C), 05.11 */
    *(int*)(info + 0x18) = heroHandle;
    *(int*)(info + 0x1C) = 0;
    *(int*)(info + 0x20) = 1;
    *(float*)(info + 0x24) = from[0]; *(float*)(info + 0x28) = from[1]; *(float*)(info + 0x2C) = from[2];
    info[0x32] = 1;          /* a magic hit: 0x76CA87 -> 0x767FB0, the attacker's Confusion may stun (05.11) */
    g_takeHit(st, 0, info);
    return dmg;
}

/* the beam from `from` to `to` (metres) as a free effect in the world. 03.11: a character's "effect component" is
   its current action (0x74FA10 starts it only when 0x8FD5C0 lets the action be replaced): on the caster the cast lock
   refused it, on an enemy its AI replaced it at once, so no beam was ever seen. The attach 0x9827B0 makes its effect
   itself: 0x810BA0 thiscall(NiFixedString* name, 0, int* id) ret 0Ch on the effect manager [0x14F170C] -> the
   instance, 0x810060(&position), 0x810270(&3x3 rotation), 0x80FFC0(byte) with the entry's flag (1 by default,
   0x982770). Here without the attach's extra turn of pi about Z: the bolt runs along -X in the file (fx_chain.py), so
   -X is turned towards the enemy. */
typedef void* (__fastcall *FxCreate_t)(void* mgr, void* edx, char** name, int zero, int* id);
typedef void  (__fastcall *FxSet_t)(void* inst, void* edx, const void* value);
typedef void  (__fastcall *FxFlag_t)(void* inst, void* edx, int flag);
static FxCreate_t g_fxCreate;
static FxSet_t    g_fxPos, g_fxRot;
static FxFlag_t   g_fxFlag;
static void**     g_fxMgr;

/* 03.11: the free effects are never taken away by the game (TrailTrace counted the instance table [14F1430]: 2-9
   alive without our effects, 564 of 572 slots after a series of Ice Bolts, never falling; the fps fell). So each one
   is released after its length: the manager's vtable+34h with the id, as 0x810C16 releases an effect it could not
   set up (mov edx,[esi] / mov eax,[esp+10h] / mov edx,[edx+34h] / push eax / mov ecx,esi / call edx). */
#define MAX_EXPIRE 1024
typedef void (__fastcall *FxFree_t)(void* mgr, void* edx, int id);
static int   g_fxFreeOk;
static struct { int id; DWORD at; void* inst; } g_expire[MAX_EXPIRE];
static BYTE** g_fxTable;              /* [0x14F1430]: the effect instance table ([+38h] slots, [+3Eh] count) */
static int   g_expireN, g_expireLost;

static void fx_expire(int id, void* inst, DWORD ms)
{
    if (!g_fxFreeOk || id < 0) return;
    if (g_expireN == MAX_EXPIRE) { g_expireLost++; return; }
    g_expire[g_expireN].id = id;
    g_expire[g_expireN].inst = inst;
    g_expire[g_expireN].at = GetTickCount() + ms;
    g_expireN++;
}

static void expire_tick(DWORD now)
{
    void* mgr = g_fxMgr ? *g_fxMgr : 0;
    if (!mgr || !g_expireN) return;
    FxFree_t release = (FxFree_t)(*(void***)mgr)[0x34 / 4];
    for (int k = 0; k < g_expireN; ) {
        if ((int)(now - g_expire[k].at) < 0) { k++; continue; }
        /* only while the manager's slot still holds our instance (its table [mgr+30h]..[mgr+34h], read as 0x818370
           does: 0x810BA0 returns that slot's object; a new level clears the table and reuses the ids) */
        BYTE* t = (BYTE*)mgr;
        int id = g_expire[k].id;
        void** lo = readable(t + 0x30, 8) ? *(void***)(t + 0x30) : 0, **hi = lo ? *(void***)(t + 0x34) : 0;
        if (lo && hi > lo && id < hi - lo && lo[id] == g_expire[k].inst)
            release(mgr, 0, id);
        g_expire[k] = g_expire[--g_expireN];
    }
}

/* a free effect at a point (the path chain_beam uses); slot keeps the effect name's fixed string; released after ms */
/* 03.11: the trail puffs go at 950 ms: the game started a free effect over again after about a second */
static void* world_fx(char** slot, const char* name, const float* at, const Mat33* m, DWORD ms)
{
    if (!g_fxCreate || !*g_fxMgr || !fx_string(slot, name)) return 0;
    int id = -1;
    void* inst = g_fxCreate(*g_fxMgr, 0, slot, 0, &id);
    if (inst) {
        fx_expire(id, inst, ms);
        Vec3 v = {{ at[0], at[1], at[2] }};
        g_fxPos(inst, 0, &v);
        g_fxRot(inst, 0, m);
        /* no 0x80FFC0: when the flag changes, 0x785270 -> 0x792050 hands the instance index to a lookup that takes a
           pointer (03.11 crash at 0x7864BA, read 00000001); the instance plays without it */
    }
    return inst;
}

/* 04.11: the marks on an enemy (slowed, frozen, Frost Nova's chill). Started on its action component they were never
   seen (the AI replaced the action at once; "started 0" as well). Moving one free effect every frame (0x810060) did
   not show either (in game 04.11: created, never seen), so as the Ice Bolt trail: every 250 ms a short puff that
   stays where it is started at the enemy's feet (0x755500) while the state lasts. Frost Nova's chill: one, at the
   hit. SlowFx<id>, FreezeFx<id>; OwnHitFx<id>. */
#define MAX_STATUS 32
#define STATUS_EVERY 120                                /* 04.11: over the body, short puffs: keep up with it */
#define STATUS_PUFF 950                                 /* the puff's life (the game starts it over after ~1 s) */
static struct { int handle; char** slot; const char* name; void* stats; DWORD until, next; } g_status[MAX_STATUS];
static char  g_slowFxName[MAX_GRANT][48], g_freezeFxName[MAX_GRANT][48];
static char* g_slowFxFixed[MAX_GRANT], *g_freezeFxFixed[MAX_GRANT];
static int   g_statusLogged;

static int status_puff(void* stats, char** slot, const char* name, DWORD ms)
{
    float* p = g_pos(stats, 0);
    Mat33 one = {{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
    return p && world_fx(slot, name, p, &one, ms) != 0;
}

/* repeat 0: one puff living ms (the chill); else a puff every STATUS_EVERY ms for ms */
static int status_start(void* stats, char** slot, const char* name, DWORD ms, int repeat)
{
    if (!g_fxCreate || !g_pos || !*g_fxMgr || !name[0] || !ms || !readable((BYTE*)stats + 0x144, 4)) return 0;
    int h = *(int*)((BYTE*)stats + 0x144);
    if (!repeat) {
        int ok = status_puff(stats, slot, name, ms);
        if (g_statusLogged++ < 12) logf("MageSkills:   mark '%s' on character %d (one, %u ms) -> %d", name, h, ms, ok);
        return 1;
    }
    int k = -1;
    for (int j = 0; j < MAX_STATUS; j++)
        if (g_status[j].name == name && g_status[j].handle == h && g_status[j].stats == stats) k = j;     /* again: extend */
    for (int j = 0; k < 0 && j < MAX_STATUS; j++) if (!g_status[j].name) k = j;
    if (k < 0) return 1;
    DWORD now = GetTickCount();
    if (g_status[k].name != name) g_status[k].next = now;
    g_status[k].handle = h;
    g_status[k].slot = slot;
    g_status[k].name = name;
    g_status[k].stats = stats;
    g_status[k].until = now + ms;
    if (g_statusLogged++ < 12) logf("MageSkills:   mark '%s' on character %d for %u ms", name, h, ms);
    return 1;
}

static void status_fx(void* stats, int i, int frozen, DWORD ms)
{
    if (frozen) status_start(stats, &g_freezeFxFixed[i], g_freezeFxName[i], ms, 1);
    else status_start(stats, &g_slowFxFixed[i], g_slowFxName[i], ms, 1);
}

static void status_tick(DWORD now)
{
    void* mgr = *g_statsMgr;
    for (int k = 0; k < MAX_STATUS; k++) {
        if (!g_status[k].name) continue;
        int h = g_status[k].handle;
        void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
        if ((int)(now - g_status[k].until) >= 0 || !st || st != g_status[k].stats) { g_status[k].name = 0; continue; }
        if ((int)(now - g_status[k].next) < 0) continue;
        g_status[k].next = now + STATUS_EVERY;
        status_puff(st, g_status[k].slot, g_status[k].name, STATUS_PUFF);
    }
}

/* 03.11: OwnStrikeFx<id> = up to 3 of the game's own effects started where each strike lands (the native impacts:
   the beam alone looked foreign next to the game's spells) */
static char  g_strikeName[MAX_GRANT][3][40];
static char* g_strikeFixed[MAX_GRANT][3];
static int   g_strikeN[MAX_GRANT];

static void chain_strike_fx(int i, const float* at)
{
    Mat33 one = {{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
    for (int k = 0; k < g_strikeN[i]; k++) {
        void* inst = world_fx(&g_strikeFixed[i][k], g_strikeName[i][k], at, &one, 4000);
        if (g_chainLogged < 60) { g_chainLogged++; logf("MageSkills:   strike effect '%s' -> instance %p", g_strikeName[i][k], inst); }
    }
}

/* 03.11: the game's own sounds (3D, under the game's volume; PlaySound is neither): OwnStrikeSound<id> = an event per
   strike (on the struck enemy), OwnCastSound<id> = one at the cast (on the caster); events of General_Sounds.bnk */
static char      g_strikeSound[MAX_GRANT][48], g_castSound[MAX_GRANT][48], g_gameSoundF[MAX_GRANT][48];
static int       g_sndLogged;

/* 03.11: the posting on a sound object of our own (0x6EB2B0/0x6EB2D0/0x6EBE90) was silent (posted, nothing heard) and
   crashed once at 0x6EB200; the game plays a character's skill sounds on the character itself, as the Fireball cast
   does (0x95E8AB): the sound component [stats+224h] (0x752E20), 0x8F5910 thiscall(component, event name, 0) ret 8 */
typedef BYTE (__fastcall *CharSound_t)(void* comp, void* edx, const char* ev, int zero);
static CharSound_t g_charSound;

/* 07.11 SoundTrace=1 ([MageSkills]): every new event name a character posts through 0x8F5910 goes to the log once (to find the
   hero's shout of the jump attack for Battle Cry). Debug only: remove before the release. */
static CharSound_t g_sndTraceReal;
static DWORD g_sndSeen[512];
static int   g_sndSeenN;
static BYTE __fastcall snd_trace(void* comp, void* edx, const char* ev, int zero)
{
    if (ev && readable(ev, 1) && g_sndSeenN < 512) {
        DWORD h = 2166136261u; int n = 0;
        while (n < 80 && readable(ev + n, 1) && ev[n]) h = (h ^ (BYTE)ev[n++]) * 16777619u;
        int seen = 0;
        for (int k = 0; k < g_sndSeenN && !seen; k++) seen = g_sndSeen[k] == h;
        if (!seen) { g_sndSeen[g_sndSeenN++] = h; logf("SoundTrace: '%s' (component %p, %u ms)", ev, comp, GetTickCount()); }
    }
    return g_sndTraceReal(comp, edx, ev, zero);
}

/* 07.11: the jump attack's shout did not pass 0x8F5910; 0x6EBE90 thiscall(sound manager, object, name, a, b) ret 10h is
   below it (0xA13680 calls it) and may see the animations' sounds too */
typedef BYTE (__fastcall *PostByName_t)(void* mgr, void* edx, int obj, const char* ev, int a, int b);
static PostByName_t g_postReal, g_postDirect;
static void** g_postMgr;
static BYTE __fastcall post_trace(void* mgr, void* edx, int obj, const char* ev, int a, int b)
{
    if (ev && readable(ev, 1) && g_sndSeenN < 512) {
        DWORD h = 2166136261u; int n = 0;
        while (n < 80 && readable(ev + n, 1) && ev[n]) h = (h ^ (BYTE)ev[n++]) * 16777619u;
        int seen = 0;
        for (int k = 0; k < g_sndSeenN && !seen; k++) seen = g_sndSeen[k] == h;
        if (!seen) { g_sndSeen[g_sndSeenN++] = h; logf("SoundTrace: post '%s' (object %X, %u ms)", ev, obj, GetTickCount()); }
    }
    BYTE r = g_postReal(mgr, edx, obj, ev, a, b);
    static int voiceN;
    if (ev && readable(ev, 14) && voiceN < 30) {                 /* the hero's voices: object, arguments, result */
        const char* v = "PlayerFemale_02_"; int k = 0;
        while (v[k] && ev[k] == v[k]) k++;
        if (!v[k]) { voiceN++; logf("SoundTrace: voice '%s' object %X a %d b %d mgr %p ret %X -> %d", ev, obj, a, b, mgr, (DWORD)__builtin_return_address(0), r); }
    }
    return r;
}

static void game_sound(const char* ev, void* st)
{
    if (!ev[0] || !g_charSound || !st || !readable((BYTE*)st + 0x224, 4)) return;
    void* comp = *(void**)((BYTE*)st + 0x224);
    BYTE ok = comp ? g_charSound(comp, 0, ev, 0) : 0;
    int direct = 0;
    /* 07.11: 0x8F5910 knows only the events of its table (0x818330 on [14F13C4]); the voices of the hero's animations
       (PlayerFemale_02_<animation>) are posted straight by 0x6EBE90 on the character's sound object: what 0x8F5910 does
       after the table (0xA13680: [14F13F8] as this, the object [comp+10h], the name, 0, 0) */
    if (!ok && comp && g_postDirect && g_postMgr && *g_postMgr && readable((BYTE*)comp + 0x10, 4)) {
        ok = g_postDirect(*g_postMgr, 0, *(int*)((BYTE*)comp + 0x10), ev, 0, 0);
        direct = 1;
        if (g_sndLogged < 20) logf("MageSkills:   direct post: object %X (component +10h)", *(int*)((BYTE*)comp + 0x10));
    }
    if (g_sndLogged++ < 20) logf("MageSkills:   sound '%s' on character %p (component %p)%s -> %d", ev, st, comp, direct ? " posted directly" : "", ok);
}

static void chain_beam(int i, const float* from, const float* to)
{
    if (!g_fxCreate || !*g_fxMgr || !g_beamN[i]) return;
    float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    float len = own_sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.2f) return;
    /* 03.11: the closest length fell short of the enemy; now the shortest one not shorter than the leap (else the
       longest), squeezed along the bolt to the exact length by the rotation matrix's first axis */
    int best = -1;
    for (int k = 0; k < g_beamN[i]; k++)
        if (g_beamCm[i][k] >= len * 100 && (best < 0 || g_beamCm[i][k] < g_beamCm[i][best])) best = k;
    if (best < 0)
        for (int k = 0; k < g_beamN[i]; k++) if (best < 0 || g_beamCm[i][k] > g_beamCm[i][best]) best = k;
    float squeeze = len * 100 / g_beamCm[i][best];
    char* name = fx_string(&g_beamFixed[i][best], g_beamName[i][best]);
    if (!name) return;
    /* -X of the file onto the leap: x = -dir, y = (up x dir) negated with it, z = x cross y (a proper rotation) */
    float x[3] = { -d[0] / len, -d[1] / len, -d[2] / len };
    float y[3] = { x[1], -x[0], 0 };
    float yl = own_sqrt(y[0] * y[0] + y[1] * y[1]);
    if (yl < 1e-3f) { y[0] = 0; y[1] = 1; y[2] = 0; } else { y[0] /= yl; y[1] /= yl; }
    float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    Mat33 m;
    if (!g_chainSwap) {               /* columns x, y, z (NiMatrix3 row major, v' = M v) */
        m.m[0] = x[0]; m.m[1] = y[0]; m.m[2] = z[0];
        m.m[3] = x[1]; m.m[4] = y[1]; m.m[5] = z[1];
        m.m[6] = x[2]; m.m[7] = y[2]; m.m[8] = z[2];
    } else {                          /* ChainSwap=1: rows */
        m.m[0] = x[0]; m.m[1] = x[1]; m.m[2] = x[2];
        m.m[3] = y[0]; m.m[4] = y[1]; m.m[5] = y[2];
        m.m[6] = z[0]; m.m[7] = z[1]; m.m[8] = z[2];
    }
    if (!g_chainSwap) { m.m[0] *= squeeze; m.m[3] *= squeeze; m.m[6] *= squeeze; }
    else { m.m[0] *= squeeze; m.m[1] *= squeeze; m.m[2] *= squeeze; }
    Vec3 at = {{ from[0], from[1], from[2] }};
    int id = -1;
    void* inst = g_fxCreate(*g_fxMgr, 0, &g_beamFixed[i][best], 0, &id);
    if (inst) {
        fx_expire(id, inst, 1000);
        g_fxPos(inst, 0, &at);
        g_fxRot(inst, 0, &m);
        /* no 0x80FFC0: when the flag changes, 0x785270 -> 0x792050 hands the instance index to a lookup that takes a
           pointer (03.11 crash at 0x7864BA, read 00000001); the instance plays without it */
    }
    if (g_chainLogged < 60) {
        g_chainLogged++;
        logf("MageSkills:   beam '%s' (%d cm) for a leap of %d cm from %d,%d,%d -> instance %p id %d", g_beamName[i][best],
             g_beamCm[i][best], (int)(len * 100), (int)from[0], (int)from[1], (int)from[2], inst, id);
    }
}

static float g_chainHand[3] = { 0.55f, 0.2f, 1.35f };   /* ChainHand= forward, right, up, cm */

static void chain_start(void* hero, int i, float damage, int jumps, float radius, DWORD delay)
{
    float* hp = g_pos(hero, 0);
    if (!hp) return;
    g_chain.hero = *(int*)((BYTE*)hero + 0x144);
    g_chain.i = i;
    g_chain.left = 1 + (jumps < 0 ? 0 : jumps > CHAIN_MAX - 1 ? CHAIN_MAX - 1 : jumps);
    g_chain.nhits = 0;
    g_chain.damage = damage;
    g_chain.radius = radius;
    g_chain.from[0] = hp[0]; g_chain.from[1] = hp[1]; g_chain.from[2] = hp[2] + 1.3f;   /* the hands */
    g_chain.due = GetTickCount() + delay;
    /* 03.11: the first bolt goes to the enemy the player has targeted, as the Fireball takes its target (0x9244FF:
       0x8CA320 on the controller [stats+0FCh] = [controller+44h], -1 = none); without one, the nearest enemy */
    BYTE* ctrl = readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
    g_chain.first = ctrl && readable(ctrl + 0x44, 4) ? *(int*)(ctrl + 0x44) : -1;
    g_chain.active = 1;
}

static void chain_tick(DWORD now)
{
    if (!g_chain.active || (int)(now - g_chain.due) < 0) return;
    BYTE* mgr = (BYTE*)*g_statsMgr;
    void* hero = mgr ? g_byHandle(mgr, 0, &g_chain.hero) : 0;
    if (!hero || !readable(mgr + 0x30, 0x10)) { g_chain.active = 0; return; }
    void** table = *(void***)(mgr + 0x30);
    int count = *(int*)(mgr + 0x3C);
    int i = g_chain.i;
    /* the first strike: from the caster, within OwnChain's range; then from the last struck enemy, within Param3 */
    float range = g_chain.nhits ? g_chain.radius : (float)g_chainRange[i];
    float* hp = g_pos(hero, 0);
    float c[3] = { g_chain.from[0], g_chain.from[1], g_chain.from[2] - 1.1f };
    if (!g_chain.nhits && hp) { c[0] = hp[0]; c[1] = hp[1]; c[2] = hp[2]; }
    float best2 = range * range;
    void* best = 0;
    if (!g_chain.nhits && g_chain.first != -1) {
        int h = g_chain.first;
        void* st = g_byHandle(mgr, 0, &h);
        float* p = st && own_enemy(hero, st) ? g_pos(st, 0) : 0;
        if (p) {
            float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2], d2 = dx * dx + dy * dy + dz * dz;
            float far = range > 30.0f ? range : 30.0f;          /* a chosen enemy may stand further than the auto range */
            if (d2 < far * far) { best = st; best2 = d2; }
        }
        if (g_chainLogged < 60) { g_chainLogged++; logf("MageSkills: chain: the player's target %d -> %s", g_chain.first,
                                                       best ? "struck first" : "not an enemy in reach, the nearest instead"); }
    }
    for (int k = 0; !best && table && k < count && k < 100000; k++) {
        void* st = readable(table + k, 4) ? table[k] : 0;
        if (!own_enemy(hero, st)) continue;
        int h = *(int*)((BYTE*)st + 0x144), seen = 0;
        for (int j = 0; j < g_chain.nhits; j++) seen |= g_chain.hits[j] == h;
        if (seen) continue;
        float* p = g_pos(st, 0);
        if (!p) continue;
        float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        if (dz > 3.0f || dz < -3.0f) continue;          /* another floor or a ledge (03.11: leaps into empty places) */
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best2) { best2 = d2; best = st; }
    }
    if (!best) {
        if (g_chainLogged < 60) { g_chainLogged++; logf("MageSkills: chain ends after %d strike(s): no enemy within %d m", g_chain.nhits, (int)range); }
        g_chain.active = 0;
        return;
    }
    float* p = g_pos(best, 0);
    float to[3] = { p[0], p[1], p[2] + 1.1f };
    /* the hit first: the game's own hit reaction starts its effect on the target's effect component and would replace
       the beam (03.11: beams "started" but never seen; Frost Nova's frost is set after its hit, too) */
    /* the hit comes from the caster, as Frost Nova's (03.11: with the previous enemy as the source point the game crashed
       in the AI's path finding, 0xE5B2F1, Kynogon TraceLine/strafe code, after the third strike) */
    float* src = g_pos(hero, 0);
    float dealt = own_hit(hero, best, g_chain.damage, src ? src : g_chain.from);
    /* 07.11: the first bolt from the hand, not the chest: the cast animation holds the hand out towards the target, so
       the point is ChainHand=<forward>,<right>,<up> cm from the feet, forward = towards the first enemy (not the bone
       Dummy_Cast_Primary: its world position is not read yet, TODO) */
    if (!g_chain.nhits && hp) {
        float fx = p[0] - hp[0], fy = p[1] - hp[1], l = own_sqrt(fx * fx + fy * fy);
        if (l > 0.05f) {
            fx /= l; fy /= l;
            g_chain.from[0] = hp[0] + fx * g_chainHand[0] + fy * g_chainHand[1];
            g_chain.from[1] = hp[1] + fy * g_chainHand[0] - fx * g_chainHand[1];
            g_chain.from[2] = hp[2] + g_chainHand[2];
        }
    }
    chain_beam(i, g_chain.from, to);
    chain_strike_fx(i, to);
    game_sound(g_strikeSound[i], best);
    if (g_chainLogged < 60) {
        g_chainLogged++;
        logf("MageSkills: chain strike %d: %d m, damage %d (base %d)", g_chain.nhits + 1, (int)own_sqrt(best2),
             (int)dealt, (int)g_chain.damage);
    }
    g_chain.hits[g_chain.nhits++] = *(int*)((BYTE*)best + 0x144);
    g_chain.from[0] = to[0]; g_chain.from[1] = to[1]; g_chain.from[2] = to[2];
    g_chain.damage *= (100 - g_chainFall[i]) / 100.0f;
    g_chain.due = now + (DWORD)g_chainStep[i];
    if (--g_chain.left <= 0 || g_chain.nhits > CHAIN_MAX) g_chain.active = 0;
}

/* 04.11 Arcane Blink (own skill on the free id 0x36): OwnBlink<id>=<effect where he leaves>,<effect where he lands>;
   OwnBlinkAxis<id> = the axis of [stats+2ACh] the hero faces, +-1..3 (TODO: -2 to be confirmed in game, the matrix is
   logged). Param3 = metres. As the game's own stuck check 0x921A0C..0x921A79:
   0x6D0620 cdecl PhysX ray (from, dir, 3 = all shapes, hit, -1, metres, mask [14F641C], 0) -> nonzero, impact at hit+4;
   0x6CF550 cdecl (pos, [stats+22Ch] = 0x752DB0, [char+14h] = 0x756440) -> nonzero when the hero's capsule does not fit;
   vtable+64h on stats (&pos) puts him there (0x921963). No place found: nothing is paid and no cooldown starts. */
typedef void* (__cdecl *Ray_t)(const float* from, const float* dir, int type, BYTE* hit, int groups, float dist, int mask, int zero);
typedef BYTE  (__cdecl *Fits_t)(const float* pos, float a, float b);
typedef float (__fastcall *StatF_t)(void* stats, void* edx);
typedef void  (__fastcall *SetPos_t)(void* stats, void* edx, const float* pos);
static Ray_t   g_ray;
static Fits_t  g_blocked;
static StatF_t g_fitA, g_fitB;
static int*    g_rayMask;
static char    g_blinkFx[MAX_GRANT][2][40];
static char*   g_blinkFixed[MAX_GRANT][2];
static int     g_blinkAxis[MAX_GRANT], g_blinkOn[MAX_GRANT], g_blinkLogged;

/* the first ray hit from `from`: the distance to it, or -1 */
static float blink_ray(const float* from, const float* dir, float dist, float* impact)
{
    BYTE hit[64];
    if (!g_ray(from, dir, 3, hit, -1, dist, *g_rayMask, 0)) return -1.0f;
    float* p = (float*)(hit + 4);
    float d[3] = { p[0] - from[0], p[1] - from[1], p[2] - from[2] };
    if (impact) { impact[0] = p[0]; impact[1] = p[1]; impact[2] = p[2]; }
    return own_sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* where the hero lands going `metres` along his facing, nearer when walls, a drop or no room say so; 0: nowhere */
static int blink_find(void* stats, int i, float metres, float* out)
{
    float* pos = g_pos(stats, 0);
    float* m = readable((BYTE*)stats + 0x2AC, 36) ? (float*)((BYTE*)stats + 0x2AC) : 0;
    if (!pos || !m) return 0;
    int a = g_blinkAxis[i] < 0 ? -g_blinkAxis[i] - 1 : g_blinkAxis[i] - 1;
    float s = g_blinkAxis[i] < 0 ? -1.0f : 1.0f;
    float f[3] = { s * m[a], s * m[3 + a], 0 };
    float l = own_sqrt(f[0] * f[0] + f[1] * f[1]);
    if (g_blinkLogged < 12) {
        logf("MageSkills: blink from %d,%d,%d cm, matrix x100 %d %d %d / %d %d %d / %d %d %d", (int)(pos[0] * 100), (int)(pos[1] * 100),
             (int)(pos[2] * 100), (int)(m[0] * 100), (int)(m[1] * 100), (int)(m[2] * 100), (int)(m[3] * 100), (int)(m[4] * 100),
             (int)(m[5] * 100), (int)(m[6] * 100), (int)(m[7] * 100), (int)(m[8] * 100));
    }
    if (l < 0.1f) return 0;
    f[0] /= l; f[1] /= l;
    /* walls: a ray at the chest, from just in front of him (his own capsule) */
    float from[3] = { pos[0] + f[0] * 0.5f, pos[1] + f[1] * 0.5f, pos[2] + 1.2f };
    float wall = blink_ray(from, f, metres, 0);
    float reach = wall < 0 ? metres : wall + 0.5f - 0.7f;
    if (reach > metres) reach = metres;
    float down[3] = { 0, 0, -1.0f };
    for (float d = reach; d >= 1.5f; d -= 0.5f) {
        float top[3] = { pos[0] + f[0] * d, pos[1] + f[1] * d, pos[2] + 2.0f };
        float g[3];
        if (blink_ray(top, down, 5.0f, g) < 0) continue;                 /* no ground: a drop */
        if (g[2] < pos[2] - 2.5f) continue;
        g[2] += 0.1f;                                                    /* as 0x921A40 */
        if (g_blocked(g, g_fitA(stats, 0), g_fitB(stats, 0))) continue;  /* no room for him */
        out[0] = g[0]; out[1] = g[1]; out[2] = g[2];
        if (g_blinkLogged++ < 12) logf("MageSkills: blink %d cm of %d (wall at %d cm) -> %d,%d,%d", (int)(d * 100), (int)(metres * 100),
                                       (int)(wall * 100), (int)(g[0] * 100), (int)(g[1] * 100), (int)(g[2] * 100));
        return 1;
    }
    if (g_blinkLogged++ < 12) logf("MageSkills: blink: no place within %d cm (wall at %d cm) -> not cast", (int)(reach * 100), (int)(wall * 100));
    return 0;
}

static void blink_init(Module m)
{
    DWORD d = (DWORD)m.base - 0x400000;
    /* 6D0620: mov ecx,[14F185C] / sub esp,34h; 6CF550: mov ecx,[14F185C] / sub esp,38h; 752DB0: fld [ecx+22Ch]; 756440's tail;
       921963: mov edx,[eax+64h] / add ebp,78h; 9219B1: mov eax,[14F641C]; the two calls of the game's check */
    BYTE* c1 = VA(m, 0x921A0C), *c2 = VA(m, 0x921A79);
    int ok = same(m, 0x6D0620, "8b0d") && *(DWORD*)VA(m, 0x6D0622) == 0x14F185C + d && same(m, 0x6D0626, "83ec34") &&
             same(m, 0x6CF550, "8b0d") && *(DWORD*)VA(m, 0x6CF552) == 0x14F185C + d && same(m, 0x6CF556, "83ec38") &&
             same(m, 0x752DB0, "d9812c020000c3") && same(m, 0x756440, "81c148010000") &&
             same(m, 0x921963, "8b5064") && same(m, 0x9219B1, "a1") && *(DWORD*)VA(m, 0x9219B2) == 0x14F641C + d &&
             c1[0] == 0xE8 && (DWORD)(c1 + 5 + *(int*)(c1 + 1)) == 0x6D0620 + d &&
             c2[0] == 0xE8 && (DWORD)(c2 + 5 + *(int*)(c2 + 1)) == 0x6CF550 + d;
    for (int i = 0; i < g_n; i++) {
        char key[32], v[96];
        wsprintfA(key, "OwnBlink%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", v, sizeof v, g_ini);
        if (!v[0]) continue;
        if (!ok || !g_pos || !g_param) { logf("MageSkills: skill %d blink off: bytes of the ray code differ", g_ids[i]); continue; }
        char* c = v;
        for (int k = 0; k < 2; k++) {
            int n = 0;
            while (*c && *c != ',' && n < 39) g_blinkFx[i][k][n++] = *c++;
            g_blinkFx[i][k][n] = 0;
            if (*c == ',') c++;
        }
        wsprintfA(key, "OwnBlinkAxis%d", g_ids[i]);
        int axis = GetPrivateProfileIntA("MageSkills", key, 2, g_ini);
        g_blinkAxis[i] = (axis >= -3 && axis <= 3 && axis) ? axis : 2;
        g_blinkOn[i] = 1;
        logf("MageSkills: skill %d is a blink: effects '%s' / '%s', facing axis %d", g_ids[i], g_blinkFx[i][0], g_blinkFx[i][1], g_blinkAxis[i]);
    }
    if (!ok) return;
    g_ray = (Ray_t)VA(m, 0x6D0620);
    g_blocked = (Fits_t)VA(m, 0x6CF550);
    g_fitA = (StatF_t)VA(m, 0x752DB0);
    g_fitB = (StatF_t)VA(m, 0x756440);
    g_rayMask = (int*)VA(m, 0x14F641C);
}

/* 1: this skill is a blink and is finished here (cast or refused; *result says which) */
static int blink_cast(void* stats, void* skill, int id, BYTE* result)
{
    int i = -1;
    for (int k = 0; k < g_n; k++) if (g_ids[k] == id && g_blinkOn[k]) i = k;
    if (i < 0) return 0;
    *result = 0;
    BYTE* mgr = *g_protoMgr;
    BYTE** begin = mgr && readable(mgr + 0x10, 8) ? *(BYTE***)(mgr + 0x10) : 0;
    BYTE* proto = begin && readable(begin + id, 4) ? begin[id] : 0;
    float metres = proto ? g_param(proto, 0, g_rank(skill, 0), 2) : 0;     /* Param3 */
    float to[3];
    float* pos = g_pos(stats, 0);
    if (metres < 1.5f || !pos || !blink_find(stats, i, metres, to)) return 1;
    float at[3] = { pos[0], pos[1], pos[2] };
    Mat33 one = {{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
    g_pay0(stats, 0, id);
    g_cooldown(skill, 0);
    if (g_blinkFx[i][0][0]) world_fx(&g_blinkFixed[i][0], g_blinkFx[i][0], at, &one, 950);
    ((SetPos_t)(*(void***)stats)[0x64 / 4])(stats, 0, to);
    if (g_blinkFx[i][1][0]) world_fx(&g_blinkFixed[i][1], g_blinkFx[i][1], to, &one, 950);
    if (g_ownSound[i] && g_playSound) g_playSound(g_ownSound[i], 0, 0x0004 | 0x0001 | 0x0002);
    if (g_castSound[i][0]) game_sound(g_castSound[i], stats);
    *result = 1;
    return 1;
}

/* 04.11 Arcane Push: OwnPush<id>=1, Param1 = metres, Param2 = damage, Param3 = radius. Every enemy own_targets hits is
   slid away from the hero for PUSH_MS, a small step each frame, by vtable+64h (as the blink); each step looks for a
   wall ahead and ground below with the blink's rays and stops there, so nobody goes through a wall or over an edge.
   Enemies bigger than OwnPushMaxSize<id> cm (the capsule value 0x756440, people: 50; 0 = all) only take the damage.
   04.11: the sequence Hit_Knockdown (in most characters' .kf) is played as the hero's own cast animation is, on the
   target's action component, whose [comp+0Ch] then keeps the AI from replacing it for KNOCK_MS (tried in the tick
   for KNOCK_TRY ms: the game's own hit reaction may hold the component at first). The arc in the air did not work. */
#define MAX_PUSH 32
#define PUSH_MS 500
#define PUSH_HOP 0.0f
#define KNOCK_MS 1400
#define KNOCK_TRY 400
static struct { int handle; void* stats; float dir[2], left, speed, total, hop; DWORD last, until; } g_push[MAX_PUSH];
static int   g_pushOn[MAX_GRANT], g_pushMax[MAX_GRANT], g_pushLogged;
static void* g_ownCaster;
static float g_pushMetres[MAX_GRANT];

typedef float (__fastcall *AnimLen_t)(void* stats, void* edx, const char* name);
typedef BYTE  (__fastcall *CanStop_t)(void* action, void* edx);
static AnimLen_t g_animLen;
static CanStop_t g_canStop;
static struct { int handle, locked, i; void* stats; DWORD until; } g_knock[MAX_PUSH];
static char* g_knockFixed;
static int   g_knockOn[MAX_GRANT], g_knockLogged;
static char  g_pushFx[MAX_GRANT][40];
static char* g_pushFxFixed[MAX_GRANT];

static void knock_start(void* st, int i)
{
    if (!g_knockOn[i] || !g_animLen || !g_setAnim || !g_setCasterFx || !g_ownHitFxName[i][0]) return;
    int k = -1, h = *(int*)((BYTE*)st + 0x144);
    for (int j = 0; j < MAX_PUSH; j++) if (g_knock[j].stats && g_knock[j].handle == h) return;   /* already down */
    for (int j = 0; k < 0 && j < MAX_PUSH; j++) if (!g_knock[j].stats) k = j;
    if (k < 0) return;
    g_knock[k].handle = h; g_knock[k].stats = st; g_knock[k].i = i; g_knock[k].locked = 0;
    g_knock[k].until = GetTickCount() + KNOCK_TRY;
}

static void knock_tick(DWORD now)
{
    void* mgr = *g_statsMgr;
    for (int k = 0; k < MAX_PUSH; k++) {
        if (!g_knock[k].stats) continue;
        int h = g_knock[k].handle, i = g_knock[k].i;
        void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
        void* container = 0;
        BYTE* comp = st == g_knock[k].stats ? (BYTE*)fx_component(st, &container) : 0;
        int over = (int)(now - g_knock[k].until) >= 0;
        if (g_knock[k].locked) {
            if (!over && st == g_knock[k].stats) continue;
            if (comp && readable(comp + 0xC, 1)) comp[0xC] = 0;
            g_knock[k].stats = 0;
            continue;
        }
        if (!comp || !container || over || !own_enemy(g_ownCaster, st)) {
            if (g_knockLogged < 24) { g_knockLogged++; logf("MageSkills: knockdown of character %d not started", h); }
            g_knock[k].stats = 0;
            continue;
        }
        char* name = fx_string(&g_ownHitFx[i], g_ownHitFxName[i]);
        if (!name) { g_knock[k].stats = 0; continue; }
        /* 04.11: armed characters have it under the weapon's name: 0x7521E0 gives a sequence's length, 0 = none */
        static const char* const kinds[] = { "Hit_Knockdown", "Hit_Knockdown_1H", "Hit_Knockdown_1HSH", "Hit_Knockdown_2HSW",
                                             "Hit_Knockdown_BOW", "Hit_Knockdown_DW", "Hit_Knockdown_UA", "HitL_Knockdown" };
        const char* kind = 0;
        float len = 0;
        for (int a = 0; a < 8 && !kind; a++) {
            len = g_animLen(st, 0, kinds[a]);
            if (len > 0) kind = kinds[a];
        }
        if (!kind) {
            if (g_knockLogged < 24) { g_knockLogged++; logf("MageSkills: character %d has no knockdown sequence", h); }
            g_knock[k].stats = 0;
            continue;
        }
        BYTE str[0x20];
        g_animStrCtor(str, 0, kind);
        g_setAnim(comp, 0, str);
        g_setAnimFlag(comp, 0, 0);
        g_animStrDtor(str, 0);
        Vec3 zero = {{0, 0, 0}};
        Mat33 orient = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
        g_addRef(name - 8);
        g_setCasterFx(comp, 0, name, 0.0f, 0, *g_fxVecA, zero, orient);
        static int out;
        BYTE ok = g_startComp(container, 0, comp, &out);
        int forced = 0;
        BYTE** slot = (BYTE**)((BYTE*)container + 0xC);
        BYTE* cur = !ok && readable(slot, 4) ? *slot : 0;
        /* refused (the action table 0x953720, or a lock): what 0x74FA9B..0x74FAAD do when it is allowed: may it stop
           (0x8FD4D0), vtable+20h stops it; the slot empty, the start has nothing to ask */
        if (cur && readable(cur, 0x10) && g_canStop(cur, 0)) {
            cur[0xC] = 0;
            ((void (__fastcall*)(void*, void*))(*(void***)cur)[0x20 / 4])(cur, 0);
            *slot = 0;
            g_setCasterFx(comp, 0, (g_addRef(name - 8), name), 0.0f, 0, *g_fxVecA, zero, orient);
            ok = g_startComp(container, 0, comp, &out);
            forced = 1;
        }
        if (ok && readable(comp + 0xC, 1)) {
            comp[0xC] = 1;
            g_knock[k].locked = 1;
            g_knock[k].until = now + (DWORD)(len * 1000.0f) + 100;
        } else g_knock[k].stats = 0;                     /* one try: no retry every frame */
        if (g_knockLogged < 40) { g_knockLogged++; logf("MageSkills: knockdown of character %d '%s' %d ms%s -> started %d", h, kind, (int)(len * 1000), forced ? " (its action stopped)" : "", ok); }
    }
}

static void push_start(void* hero, void* st, int i)
{
    if (i < 0 || !g_pushOn[i] || g_pushMetres[i] <= 0) return;
    g_ownCaster = hero;
    float* hp = g_pos(hero, 0), *p = g_pos(st, 0);
    if (!hp || !p) return;
    float dx = p[0] - hp[0], dy = p[1] - hp[1], l = own_sqrt(dx * dx + dy * dy);
    if (l < 0.05f) return;
    int k = -1, h = *(int*)((BYTE*)st + 0x144);
    if (g_pushMax[i] && g_fitB(st, 0) * 100 > g_pushMax[i]) {
        if (g_pushLogged < 30) { g_pushLogged++; logf("MageSkills: character %d is too big to push: size %d", h, (int)(g_fitB(st, 0) * 100)); }
        return;
    }
    for (int j = 0; j < MAX_PUSH; j++) if (g_push[j].stats && g_push[j].handle == h) k = j;
    for (int j = 0; k < 0 && j < MAX_PUSH; j++) if (!g_push[j].stats) k = j;
    if (k < 0) return;
    DWORD now = GetTickCount();
    g_push[k].handle = h;
    g_push[k].stats = st;
    g_push[k].dir[0] = dx / l; g_push[k].dir[1] = dy / l;
    g_push[k].left = g_pushMetres[i];
    g_push[k].total = g_pushMetres[i];
    g_push[k].hop = 0;
    g_push[k].speed = g_pushMetres[i] * 1000.0f / PUSH_MS;
    g_push[k].last = now;
    g_push[k].until = now + PUSH_MS * 2;
    knock_start(st, i);
    if (g_pushLogged < 30) { g_pushLogged++; logf("MageSkills: push character %d, %d cm, size %d", h, (int)(g_pushMetres[i] * 100), (int)(g_fitB(st, 0) * 100)); }
}

static void push_tick(DWORD now)
{
    knock_tick(now);
    void* mgr = *g_statsMgr;
    float down[3] = { 0, 0, -1.0f };
    for (int k = 0; k < MAX_PUSH; k++) {
        if (!g_push[k].stats) continue;
        int h = g_push[k].handle;
        void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
        float* p = st == g_push[k].stats ? g_pos(st, 0) : 0;
        if (!p || g_push[k].left <= 0 || (int)(now - g_push[k].until) >= 0) { g_push[k].stats = 0; continue; }
        float step = g_push[k].speed * (now - g_push[k].last) / 1000.0f;
        if (step < 0.01f) continue;
        g_push[k].last = now;
        if (step > g_push[k].left) step = g_push[k].left;
        if (step > 0.5f) step = 0.5f;
        g_push[k].left -= step;
        float f[3] = { g_push[k].dir[0], g_push[k].dir[1], 0 };
        float from[3] = { p[0] + f[0] * 0.6f, p[1] + f[1] * 0.6f, p[2] + 1.0f };
        const char* why = 0;
        float g[3];
        float wall = blink_ray(from, f, step + 0.3f, 0);
        if (wall >= 0) why = "wall";
        float top[3] = { p[0] + f[0] * step, p[1] + f[1] * step, p[2] + 1.5f };
        float feet = p[2] - g_push[k].hop;
        if (!why && blink_ray(top, down, 4.0f, g) < 0) why = "no ground";
        if (!why && g[2] < feet - 1.2f) why = "edge";
        if (!why && g[2] > feet + 0.6f) why = "step up";
        if (why) {
            if (g_pushLogged < 30) { g_pushLogged++; logf("MageSkills: push of character %d stops: %s, %d cm left", h, why, (int)(g_push[k].left * 100)); }
            g_push[k].stats = 0;
            continue;
        }
        /* the arc over the ground below him, 4h t(1-t) */
        float t = 1.0f - g_push[k].left / g_push[k].total;
        float to[3] = { top[0], top[1], g[2] + 0.02f + 4.0f * PUSH_HOP * t * (1.0f - t) };
        g_push[k].hop = to[2] - g[2] - 0.02f;
        ((SetPos_t)(*(void***)st)[0x64 / 4])(st, 0, to);
    }
}

static void push_init(Module m)
{
    /* 744C20: call 7521E0, the sequence's length (name) ret 4 (not 752230: that is (sequence, text key) ret 8, 04.11 crash:
       it took a second argument off the stack); 74FA9D: call 8FD4D0, then test al,al / je */
    BYTE* c1 = VA(m, 0x744C20), *c2 = VA(m, 0x74FA9D);
    DWORD d = (DWORD)m.base - 0x400000;
    if (c1[0] == 0xE8 && (DWORD)(c1 + 5 + *(int*)(c1 + 1)) == 0x7521E0 + d && c2[0] == 0xE8 &&
        (DWORD)(c2 + 5 + *(int*)(c2 + 1)) == 0x8FD4D0 + d && same(m, 0x74FAA6, "8b178b4220")) {
        g_animLen = (AnimLen_t)VA(m, 0x7521E0);
        g_canStop = (CanStop_t)VA(m, 0x8FD4D0);
    } else logf("MageSkills: knockdown off: bytes at 0x744C20/0x74FA9D differ");
    for (int i = 0; i < g_n; i++) {
        char key[32];
        wsprintfA(key, "OwnPush%d", g_ids[i]);
        if (!GetPrivateProfileIntA("MageSkills", key, 0, g_ini)) continue;
        g_pushOn[i] = g_ray != 0;
        wsprintfA(key, "OwnPushMaxSize%d", g_ids[i]);
        g_pushMax[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnKnock%d", g_ids[i]);
        g_knockOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnPushFx%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_pushFx[i], sizeof g_pushFx[i], g_ini);
        logf("MageSkills: skill %d pushes the enemies it hits%s", g_ids[i], g_ray ? "" : " - OFF, the ray code differs");
    }
}

/* 04.11 Arcane Shield: OwnShield<id>=<effect at a hit and at the end>. Param1 = % of the damage absorbed, Param2 = seconds,
   Param3 = mana per 100 damage absorbed. 0x76C800 thiscall(stats, hit) is detoured: while the shield is up, the hero's four
   damage values in the hit ([hit+0..0Ch], TODO: their meaning, all scaled the same) are lowered and mana [stats+190h] pays;
   no mana: it falls. The look is the dragon shield's second skin: vtable+68h of the character -> 0x8FCB70(flag), as the
   status 0x10 does (0xA5C530..0xA5C540; that status itself dies outside the dragon form, 0xA5C5E1). */
typedef void* (__fastcall *Visual_t)(void* stats, void* edx);
typedef void  (__fastcall *Skin_t)(void* visual, void* edx, int on);
static TakeHit_t g_takeHitReal;
static Skin_t    g_skin;
/* 04.11 test: the sweep from the feet up belongs to the material of 0x8FCB70. OwnShieldSkin<id>=-1 takes the next one of these
   sister calls (same code: flag -> 0x7933D0-family setter on the visual) at every cast, N keeps it; OwnShieldPulse<id> = ms */
#define SKIN_KINDS 5
static const DWORD g_skinVa[SKIN_KINDS] = { 0x8FCB70, 0x8FCBF0, 0x8FCC70, 0x8FCAF0, 0x8FCA70 };
static Skin_t g_skins[SKIN_KINDS];
static int    g_skinPick = 0, g_skinNext, g_skinPulse;
static struct { int on, hero, i; void* stats; float absorb, cost; DWORD until, nextFx, nextSkin; } g_shield;
static int   g_shieldOn[MAX_GRANT], g_shieldLogged;
static char  g_shieldFx[MAX_GRANT][40];
static char* g_shieldFxFixed[MAX_GRANT];

/* 06.11 the icon of Blessing: status 0x10 (no icon of its own, 0xA5C720; ends itself outside the dragon form, 0xA5C5EB). As status 0xF for the shield: slots 3, 4, 5 and 18 of its vtable
   0x13465E0 go through bless_*: in the dragon form the game's code, otherwise only its time and "buf/levelup" (no status reads that name: Buff_LevelUp.nif, our art). */
typedef void (__fastcall *St10_t)(void* status, void* edx);
typedef void (__fastcall *St10Up_t)(void* status, void* edx, float dt, int b);
typedef char** (__fastcall *St10Name_t)(void* status, void* edx, char** out);
static St10_t g_st10Start, g_st10End;
static St10Up_t g_st10Update;
static St10Name_t g_st10Name;
static char* g_blessBuff;
static int g_blessIcon;

static int st15_dragon(void* status);
static void __fastcall bless_start(void* st, void* edx) { if (st15_dragon(st)) g_st10Start(st, edx); }
static void __fastcall bless_end(void* st, void* edx) { if (st15_dragon(st)) g_st10End(st, edx); }
static void __fastcall bless_update(void* st, void* edx, float dt, int b) { if (st15_dragon(st)) g_st10Update(st, edx, dt, b); }
static char** __fastcall bless_name(void* st, void* edx, char** out)
{
    char* n = st15_dragon(st) ? 0 : fx_string(&g_blessBuff, "buf/levelup");
    if (!n) return g_st10Name(st, edx, out);
    __sync_fetch_and_add((long*)(n - 8), 1);
    *out = n;
    return out;
}

/* 05.11: the icon over the bar. Status 0x10 has an empty icon name (0xA5C720); "buf/shielded" belongs to status 0xF (0x9833E0, class
   0x982D80, vtable 0x1335A28): 0x912710(0xF) cdecl makes it, 0x9154E0(seconds, 0) sets its time, 0x914660 on the list [[stats+0FCh]+18h]
   adds it, 0x914740(0xF) takes it off. It is the Dragon Shield itself: start 0x982E70 turns the skin on, update 0x982F20 ends it outside
   the dragon form and drains mana, end 0x982FE0 turns the skin off and adds 0x10. Its three vtable slots go through icon_*: in the dragon form
   the game's code as before, otherwise nothing, so the status is only its icon and its time (counted down by the list). */
typedef void (__fastcall *St15_t)(void* status, void* edx);
typedef void (__fastcall *St15Up_t)(void* status, void* edx, float dt, int b);
typedef void* (__fastcall *StatusChar_t)(void* status, void* edx);
static St15_t g_st15Start, g_st15End;
static St15Up_t g_st15Update;
static StatusChar_t g_statusChar;

static int st15_dragon(void* status)
{
    BYTE* c = (BYTE*)g_statusChar(status, 0);
    return c && (*(DWORD*)(c + 4) >> 0x13) & 1;
}
static void __fastcall icon_start(void* st, void* edx) { if (st15_dragon(st)) g_st15Start(st, edx); }
static void __fastcall icon_end(void* st, void* edx) { if (st15_dragon(st)) g_st15End(st, edx); }
static void __fastcall icon_update(void* st, void* edx, float dt, int b) { if (st15_dragon(st)) g_st15Update(st, edx, dt, b); }

/* slot 18, 0x9833E0: thiscall(NiFixedString* out) ret 4, gives the name with one more reference (InterlockedIncrement on the string - 8).
   The dragon keeps "buf/shielded". Our shield takes "buf/skillup" (Buff_SkillUp.nif, our art in MageExpansion.dv2): no status reads that name
   (0x14F1F84 is only made and freed). Returns out in eax: without that the icon was blank (05.11). */
typedef char** (__fastcall *BuffName_t)(void* status, void* edx, char** out);
static BuffName_t g_st15Name;
static char* g_ownBuff;
static char** __fastcall buff_name(void* st, void* edx, char** out)
{
    char* n = st15_dragon(st) ? 0 : fx_string(&g_ownBuff, "buf/skillup");
    if (!n) return g_st15Name(st, edx, out);
    __sync_fetch_and_add((long*)(n - 8), 1);
    *out = n;
    return out;                 /* eax = out, as 0x9833E0: the caller reads the name through it */
}

typedef void* (__cdecl *StatusNew_t)(int id);
typedef void  (__fastcall *StatusTime_t)(void* status, void* edx, float secs, int flag);
typedef void  (__fastcall *StatusList_t)(void* list, void* edx, void* status);
typedef void  (__fastcall *StatusOff_t)(void* list, void* edx, int id);
static void* g_statusHasVa;
static StatusNew_t g_statusNew;
static StatusTime_t g_statusTime;
static StatusList_t g_statusAdd;
static StatusOff_t g_statusOff;
static BYTE* g_statusDbg, *g_statusDbgList;
static DWORD g_statusDbgAt;
static int g_statusDbgN;
typedef BYTE (__fastcall *StatusHas_t)(void* list, void* edx, int id);


static void* status_list(void* stats)
{
    BYTE* c = readable((BYTE*)stats + 0xFC, 4) ? *(BYTE**)((BYTE*)stats + 0xFC) : 0;
    return c && readable(c + 0x18, 4) ? *(void**)(c + 0x18) : 0;
}

static void shield_status(void* stats, float secs)
{
    void* list = g_statusNew ? status_list(stats) : 0;
    if (!list) return;
    void* st = g_statusNew(0xF);
    if (!st) return;
    g_statusTime(st, 0, secs, 0);
    g_statusAdd(list, 0, st);
    g_statusDbg = (BYTE*)st; g_statusDbgList = (BYTE*)list; g_statusDbgAt = GetTickCount(); g_statusDbgN = 0;
    logf("MageSkills: shield status %p added to list %p (stats+FC %p): id %d mode %d slot18 %d time %d", st, list, *(void**)((BYTE*)stats + 0xFC), *(int*)((BYTE*)st + 0x10),
         *(int*)((BYTE*)st + 0x14), *(int*)((BYTE*)st + 0x18), (int)*(float*)((BYTE*)st + 0x24));
}

static void shield_skin(void* stats, int on)
{
    void* v = ((Visual_t)(*(void***)stats)[0x68 / 4])(stats, 0);
    if (v) g_skin(v, 0, on);
    if (g_shieldLogged < 30) { g_shieldLogged++; logf("MageSkills: shield skin %d on visual %p", on, v); }
}

static void shield_fx(void* stats, int i)
{
    Mat33 one = {{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
    float* p = g_pos(stats, 0);
    if (p && g_shieldFx[i][0]) world_fx(&g_shieldFxFixed[i], g_shieldFx[i], p, &one, 950);
}

static void shield_end(const char* why)
{
    g_shield.on = 0;
    if (g_shieldLogged < 30) { g_shieldLogged++; logf("MageSkills: shield ends: %s", why); }
    void* mgr = *g_statsMgr;
    int h = g_shield.hero;
    void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (st && st == g_shield.stats) {
        shield_skin(st, 0);
        shield_fx(st, g_shield.i);
        void* list = g_statusOff ? status_list(st) : 0;
        if (list) g_statusOff(list, 0, 0xF);
    }
}

/* 05.11 diagnostics (HitFieldTrace=1): every hit with its fields and caller, to tell a jump attack and Reflect from a usual hit. TODO: remove before release. */
static int g_hitFieldTrace, g_hitFieldN;
typedef void (__fastcall *ShowHit_t)(void* stats, void* edx, void* hit);
static ShowHit_t g_showHit;
static DWORD g_reflectRet;
/* life leech: does the jump attack reach it? TODO: remove with HitFieldTrace */
typedef void (__fastcall *Leech_t)(void* stats, void* edx, void* hit);
static Leech_t g_leechReal;
static void __fastcall hk_leech(void* stats, void* edx, void* hit)
{
    if (g_hitFieldN < 300) logf("HitTrace: life leech entered, dmg %d, attacker %X", (int)((float*)hit)[0], *(int*)((BYTE*)hit + 0x18));
    g_leechReal(stats, edx, hit);
}
/* 05.11 Hunter's Mark: OwnMark<id>=1. Param1 = % more damage the marked enemy takes from everyone, Param2 = seconds, Param3 = metres. The enemy
   the player has targeted is marked (no target or out of range: no cast, nothing paid); one mark at a time. hk_takeHit raises the hit, mark_tick renews the effect on the target. */
static int g_markOn[MAX_GRANT], g_smiteOn[MAX_GRANT], g_blessOn[MAX_GRANT], g_drainOn[MAX_GRANT], g_cryOn[MAX_GRANT];
static struct { int on, handle, i; float pct; DWORD until, nextFx; } g_mark;
static int g_markLogged;
/* 05.11, the user: as the Ranger's Surprise: the skill runs on the game's code of that skill (CodePath, skills.json): the arrow, its effects and the game's status
   0x29 on the target (time from Param4; Param1..3, its penalties, are 0). MarkSkill=<id>; Param5 = % more damage. A target with status 0x29 takes it while the
   last cast of the two skills was the mark (a cast of the Surprise itself clears it). 0x913100 thiscall(id) on the status list. */
static int g_markSkill;
static float g_markPct;
static void* g_markHas;

/* 05.11 Spell Critical (own passive, id SpellCrit in [MageSkills], Param1 = % chance): a direct magic hit (type [+14h] = 5, not the [+20h] = 5 of burns and reflections)
   from a character (no [+1Ch]) who has the skill does double damage. 0x763AD0 thiscall(id) has the skill; 0x75BDE0 thiscall(id, index) the rank's parameter. */
typedef BYTE (__fastcall *HasSkill_t)(void* stats, void* edx, int id);
typedef float (__fastcall *SkillParam_t)(void* stats, void* edx, int id, int index);
static HasSkill_t g_hasSkill;
static SkillParam_t g_skillParam;
static int g_critId, g_critLogged;
static DWORD g_critSeed;
static Module g_critModule;

static void spell_crit(BYTE* hit)
{
    if (*(int*)(hit + 0x14) != 5 || *(int*)(hit + 0x20) == 5 || *(int*)(hit + 0x1C) != 0) return;
    void* mgr = *g_statsMgr;
    int h = *(int*)(hit + 0x18);
    void* from = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (!from || !g_hasSkill(from, 0, g_critId)) return;
    g_critSeed = g_critSeed * 214013 + 2531011 + GetTickCount();
    int roll = (int)((g_critSeed >> 16) & 0x7FFF) % 100;
    float chance = g_skillParam(from, 0, g_critId, 0);
    if (g_critLogged < 40) { g_critLogged++; logf("MageSkills: spell critical: chance %d, roll %d", (int)chance, roll); }
    if ((float)roll >= chance) return;
    /* the game's own crit: [+10h] != 0 multiplies by 1.5 and the crit bonus (0x98165C) and the number is shown as a crit (0x7662AC); 4/3 makes it x2 in all */
    /* 05.11: the crit bonus is the attacker's 0x7530C0 (%, 0x98168C); left out it made x3. Taken out here, so the total is the x2 of the description */
    float bonus = ((float (__fastcall *)(void*, void*))VA(g_critModule, 0x7530C0))(from, 0);
    float mul = 2.0f / (1.5f * (1.0f + bonus / 100.0f));
    if (g_critLogged < 60) { g_critLogged++; logf("MageSkills: spell critical hit: crit bonus %d%%", (int)bonus); }
    for (int k = 0; k < 4; k++) ((float*)hit)[k] *= mul;
    *(int*)(hit + 0x10) = 1;
}

/* 06.11 Aimed Shot (own passive): AimSkill=<id>. An arrow's critical hit (type 3, [+10h] set) is stronger with distance: nothing up to Param2 metres, Param1 % from Param3 metres, linear between. */
static int g_aimSkill, g_aimLogged;
static void aimed_shot(void* stats, BYTE* hit)
{
    void* mgr = *g_statsMgr;
    int h = *(int*)(hit + 0x18);
    void* from = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (g_aimLogged < 40 && *(int*)(hit + 0x14) == 3) { g_aimLogged++; logf("MageSkills: aimed shot: arrow hit, crit flag %d", *(int*)(hit + 0x10)); }
    if (!from || !*(int*)(hit + 0x10) || !g_hasSkill(from, 0, g_aimSkill)) return;
    float* a = g_pos(from, 0), *b = g_pos(stats, 0);
    if (!a || !b) return;
    float dx = a[0] - b[0], dy = a[1] - b[1], d2 = dx * dx + dy * dy;
    float near_ = g_skillParam(from, 0, g_aimSkill, 1), far_ = g_skillParam(from, 0, g_aimSkill, 2);
    if (d2 <= near_ * near_ || far_ <= near_) return;
    float part = 1.0f;
    if (d2 < far_ * far_) {             /* no CRT: a few Newton steps for the distance */
        float d = far_;
        for (int k = 0; k < 8; k++) d = 0.5f * (d + d2 / d);
        part = (d - near_) / (far_ - near_);
    }
    float pct = g_skillParam(from, 0, g_aimSkill, 0) * part;
    for (int k = 0; k < 4; k++) ((float*)hit)[k] *= 1.0f + pct / 100.0f;
    if (g_aimLogged < 80) { g_aimLogged++; logf("MageSkills: aimed shot: critical +%d%%", (int)pct); }
}

/* 06.11 Armor Breaker (own passive): BreakSkill=<id>, BreakFx=<effect>. A melee hit (type 1) has Param1 % to crack the target: for Param3 seconds every hit on it deals Param2 % more. Eight targets at once. */
static int g_breakSkill, g_breakLogged;
static char g_breakFx[48];
static char* g_breakFixed;
static DWORD g_breakSeed;
static struct { int handle; float pct; DWORD until; } g_broken[8];
static void armor_breaker(void* stats, BYTE* hit)
{
    int target = *(int*)((BYTE*)stats + 0x144);
    DWORD now = GetTickCount();
    for (int i = 0; i < 8; i++)
        if (g_broken[i].handle == target && (int)(now - g_broken[i].until) < 0) {
            for (int k = 0; k < 4; k++) ((float*)hit)[k] *= 1.0f + g_broken[i].pct / 100.0f;
            break;
        }
    if (*(int*)(hit + 0x14) != 1 || *(int*)(hit + 0x20) == 5) return;
    void* mgr = *g_statsMgr;
    int h = *(int*)(hit + 0x18);
    void* from = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (!from || from == stats || !g_hasSkill(from, 0, g_breakSkill)) return;
    g_breakSeed = g_breakSeed * 214013 + 2531011 + now;
    if ((float)((int)((g_breakSeed >> 16) & 0x7FFF) % 100) >= g_skillParam(from, 0, g_breakSkill, 0)) return;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (g_broken[i].handle == target) { slot = i; break; }
        if ((int)(g_broken[i].until - g_broken[slot].until) < 0) slot = i;
    }
    g_broken[slot].handle = target;
    g_broken[slot].pct = g_skillParam(from, 0, g_breakSkill, 1);
    g_broken[slot].until = now + (DWORD)(g_skillParam(from, 0, g_breakSkill, 2) * 1000.0f);
    if (g_breakFx[0]) status_puff(stats, &g_breakFixed, g_breakFx, 2000);
    if (g_breakLogged++ < 30) logf("MageSkills: armor breaker: target %X +%d%%", target, (int)g_broken[slot].pct);
}

/* 05.11 Second Wind (own passive): WindSkill=<id>, WindFx=<effect>. When a hit leaves its owner alive under 25% health: Param1 = % of max health healed, Param2 = % less
   damage for Param3 seconds, again after Param4 seconds. 0x75EAD0 thiscall health, 0x75BBD0 thiscall(0) max health, vtable+24h thiscall(float) adds to health (0x76CB5C). */
static int g_windSkill, g_windLogged;
static char g_windFx[48];
static char* g_windFixed;
static struct { int handle; float less; DWORD until, ready; } g_wind;
typedef float (__fastcall *Health_t)(void* stats, void* edx);
typedef float (__fastcall *MaxHealth_t)(void* stats, void* edx, int flag);
static Health_t g_health;
static MaxHealth_t g_maxHealth;

static void wind_after_hit(void* stats)
{
    if (!g_hasSkill(stats, 0, g_windSkill)) return;
    DWORD now = GetTickCount();
    int h = *(int*)((BYTE*)stats + 0x144);
    if (g_wind.handle == h && (int)(now - g_wind.ready) < 0) return;
    float hp = g_health(stats, 0), top = g_maxHealth(stats, 0, 0);
    if (hp <= 0 || top <= 0 || hp >= top * 0.25f) return;
    float heal = top * g_skillParam(stats, 0, g_windSkill, 0) / 100.0f;
    (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)stats + 0x24))(stats, 0, heal);
    g_wind.handle = h;
    g_wind.less = g_skillParam(stats, 0, g_windSkill, 1);
    g_wind.until = now + (DWORD)(g_skillParam(stats, 0, g_windSkill, 2) * 1000.0f);
    g_wind.ready = now + (DWORD)(g_skillParam(stats, 0, g_windSkill, 3) * 1000.0f);
    if (g_windFx[0]) status_puff(stats, &g_windFixed, g_windFx, 1500);
    if (g_windLogged++ < 20) logf("MageSkills: second wind: health %d of %d, +%d, %d%% less damage", (int)hp, (int)top, (int)heal, (int)g_wind.less);
}

/* 05.11 Pack Hunger (own passive): HungerSkill=<id>, Param1 = % of the damage a summon deals healed on it. The hero is the character of 0x6EF4D0 on [0x14F13D0]; his summons
   are the list [[stats+0FCh]+54h]..[+58h], 8 bytes each, handle first (SummonLimit below); the hit's final damage is [+34h] after the hit (0x9816E0). Burns and the like ([+20h] = 5) do not count. */
static int g_hungerSkill, g_hungerLogged;
static void* (__fastcall *g_hungerPlayer)(void* mgr, void* edx);
static void** g_hungerMgr;
static void** g_hungerSkull;
static void* g_hungerSkullGet;

static void hunger_after_hit(BYTE* hit)
{
    if (*(int*)(hit + 0x20) == 5 || *(int*)(hit + 0x1C) != 0 || !*g_hungerMgr || !*g_statsMgr) return;
    BYTE* ch = (BYTE*)g_hungerPlayer(*g_hungerMgr, 0);
    if (!ch || !readable(ch + 4, 4)) return;
    int index = *(int*)(ch + 4) & 0xFFF;
    void* hero = g_stats(*g_statsMgr, 0, &index);
    if (!hero || !g_hasSkill(hero, 0, g_hungerSkill)) return;
    BYTE* ctrl = readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
    if (!ctrl || !readable(ctrl + 0x54, 8)) return;
    int* lo = *(int**)(ctrl + 0x54), *hi = *(int**)(ctrl + 0x58), from = *(int*)(hit + 0x18), is = 0;
    for (int* e = lo; e && e < hi && readable(e, 8); e += 2) is |= e[0] == from;
    /* the Crystal Skull's creature is not on that list: 0x7BD2D0 on [0x14F17DC] gives its handle, as the skull's use asks at 0x762A16 */
    if (!is && *g_hungerSkull) is = ((int (__fastcall *)(void*, void*))g_hungerSkullGet)(*g_hungerSkull, 0) == from;
    if (!is) return;
    int h = from;
    void* who = g_byHandle(*g_statsMgr, 0, &h);
    float dealt = *(float*)(hit + 0x34);
    if (!who || dealt <= 0 || dealt > 1000000.0f) return;
    float heal = dealt * g_skillParam(hero, 0, g_hungerSkill, 0) / 100.0f;
    (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)who + 0x24))(who, 0, heal);
    if (g_hungerLogged++ < 30) logf("MageSkills: pack hunger: summon %d dealt %d, heals %d", from, (int)dealt, (int)heal);
}

/* 07.11 Summons:
   1) SkullMastery=1: Summon Mastery (0x2E) for the Crystal Skull's creature. The summon skills give it at 0x960C48..0x960CC3:
      p = (int)Param1 of the mastery, then 0x75B8C0, 0x75B910, 0x75B960, 0x75B9B0, 0x75BA00 thiscall(creature, p), then
      0x758500 thiscall(0.0, 0.0, 1) and vtable+24h thiscall([0x13338D8]); the skull's creature (0x762A16) gets none of it.
      own_tick does the same once for each new creature of the skull.
   2) SummonLevelPct=<n>: the hero's summons (list and skull) deal n% more damage per hero level and take as much less. */
typedef void (__fastcall *StatBonus_t)(void* st, void* edx, int v);
typedef void (__fastcall *StatRecalc_t)(void* st, void* edx, float a, float b, int c);
static StatBonus_t g_statBonus[5];
static StatRecalc_t g_statRecalc;
static float* g_statHeal;
static int g_skullMastery, g_skullDone = -1, g_summonLevelPct, g_summonLogged, g_summonTrace, g_summonTraceN;
static DWORD g_skullNext;
static float g_skullDamage = 1.0f, g_skullArmor = 1.0f;

static void* summon_hero(void)
{
    if (!g_hungerPlayer || !*g_hungerMgr || !*g_statsMgr) return 0;
    BYTE* ch = (BYTE*)g_hungerPlayer(*g_hungerMgr, 0);
    if (!ch || !readable(ch + 4, 4)) return 0;
    int index = *(int*)(ch + 4) & 0xFFF;
    return g_stats(*g_statsMgr, 0, &index);
}

static int skull_creature(void)
{
    return g_hungerSkull && *g_hungerSkull ? ((int (__fastcall *)(void*, void*))g_hungerSkullGet)(*g_hungerSkull, 0) : -1;
}

static int is_summon(void* hero, int h)
{
    BYTE* ctrl = hero && readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
    if (ctrl && readable(ctrl + 0x54, 8)) {
        int* lo = *(int**)(ctrl + 0x54), *hi = *(int**)(ctrl + 0x58);
        for (int* e = lo; e && e < hi && readable(e, 8); e += 2) if (e[0] == h) return 1;
    }
    return h != -1 && skull_creature() == h;
}

static void skull_tick(DWORD now)
{
    if (!g_skullMastery || (int)(now - g_skullNext) < 0) return;
    g_skullNext = now + 500;
    int h = skull_creature();
    if (h == -1 || h == g_skullDone) return;
    void* hero = summon_hero();
    int hh = h;
    void* st = *g_statsMgr ? g_byHandle(*g_statsMgr, 0, &hh) : 0;
    if (!hero || !st) return;
    g_skullDone = h;
    if (!g_hasSkill(hero, 0, 0x2E)) { if (g_summonLogged++ < 20) logf("MageSkills: skull creature %d: the hero has no Summon Mastery", h); return; }
    int p = (int)g_skillParam(hero, 0, 0x2E, 0);
    for (int k = 0; k < 5; k++) g_statBonus[k](st, 0, p);
    g_statRecalc(st, 0, 0.0f, 0.0f, 1);
    (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)st + 0x24))(st, 0, *g_statHeal);
    if (g_summonLogged++ < 20) logf("MageSkills: skull creature %d gets Summon Mastery +%d%%", h, p);
}

static void summon_level_hit(void* target, BYTE* hit)
{
    void* hero = summon_hero();
    if (!hero || !g_level) return;
    /* 07.11: 0x751CF0 ([+100h] attribute table) gave 0 for the hero; his level is [stats+16Ch] (43 = the level in the
       test), read by 0x751910 (mov eax,[ecx+16Ch] / ret) */
    int lv = caster_level(hero);
    if (lv < 0 || lv > 100) lv = 0;
    float f = 1.0f + g_summonLevelPct * lv / 100.0f;
    static int lvLogged;
    if (lvLogged++ < 5) logf("MageSkills: summons: hero %p level %d -> x%d%%", hero, lv, (int)(f * 100));
    int from = *(int*)(hit + 0x18), to = *(int*)((BYTE*)target + 0x144);
    /* 07.11 SkullDamagePct / SkullArmorPct: the Crystal Skull's creature dealt about half of the summons' damage
       (main attack 230-290 against 360-500 at level 43): it deals and takes that much more / less on top */
    int skull = skull_creature();
    if (from != to && is_summon(hero, from)) {
        float g = f * (from == skull ? g_skullDamage : 1.0f);
        for (int k = 0; k < 4; k++) ((float*)hit)[k] *= g;
        if (g_summonLogged++ < 40) logf("MageSkills: summon %X hits x%d%%", from, (int)(g * 100));
    } else if (is_summon(hero, to)) {
        if (to == skull) f *= g_skullArmor;
        for (int k = 0; k < 4; k++) ((float*)hit)[k] /= f;
        if (g_summonLogged++ < 40) logf("MageSkills: summon %X is hit /%d%%", to, (int)(f * 100));
    }
}

static void summon_power_init(Module m, DWORD d)
{
    g_skullMastery = GetPrivateProfileIntA("MageSkills", "SkullMastery", 0, g_ini);
    g_summonLevelPct = GetPrivateProfileIntA("MageSkills", "SummonLevelPct", 0, g_ini);
    g_summonTrace = GetPrivateProfileIntA("MageSkills", "SummonTrace", 0, g_ini);
    g_skullDamage = 1.0f + GetPrivateProfileIntA("MageSkills", "SkullDamagePct", 0, g_ini) / 100.0f;
    g_skullArmor = 1.0f + GetPrivateProfileIntA("MageSkills", "SkullArmorPct", 0, g_ini) / 100.0f;
    if (g_skullDamage != 1.0f || g_skullArmor != 1.0f)
        logf("MageSkills: the Crystal Skull's creature: damage x%d%%, damage taken /%d%%", (int)(g_skullDamage * 100), (int)(g_skullArmor * 100));
    if (!g_skullMastery && !g_summonLevelPct && !g_summonTrace) return;
    if (g_summonTrace) logf("MageSkills: summon damage trace on (first 300 hits)");
    g_hasSkill = (HasSkill_t)VA(m, 0x763AD0);
    g_skillParam = (SkillParam_t)VA(m, 0x75BDE0);
    *(void**)&g_hungerPlayer = VA(m, 0x6EF4D0);
    g_hungerMgr = (void**)VA(m, 0x14F13D0);
    g_hungerSkull = (void**)VA(m, 0x14F17DC);
    g_hungerSkullGet = VA(m, 0x7BD2D0);
    if (g_skullMastery) {
        /* 960C75: push ebx / mov ecx,esi / call 75B8C0 ... five times 8 bytes apart; 960CAD: call 758500; 960CB4: fld [13338D8] */
        static const DWORD fn[5] = { 0x75B8C0, 0x75B910, 0x75B960, 0x75B9B0, 0x75BA00 };
        int ok = 1;
        for (int k = 0; k < 5; k++) {
            BYTE* c = VA(m, 0x960C75 + k * 8);
            ok &= c[0] == 0x53 && c[1] == 0x8B && c[2] == 0xCE && c[3] == 0xE8 && (DWORD)(c + 8 + *(int*)(c + 4)) == fn[k] + d;
            g_statBonus[k] = (StatBonus_t)VA(m, fn[k]);
        }
        BYTE* r = VA(m, 0x960CAD);
        ok &= r[0] == 0xE8 && (DWORD)(r + 5 + *(int*)(r + 1)) == 0x758500 + d && same(m, 0x960CB4, "d905") && *(DWORD*)VA(m, 0x960CB6) == 0x13338D8 + d;
        g_statRecalc = (StatRecalc_t)VA(m, 0x758500);
        g_statHeal = (float*)VA(m, 0x13338D8);
        if (!ok) g_skullMastery = 0;
        logf("MageSkills: Summon Mastery for the Crystal Skull's creature %s", ok ? "on" : "OFF: bytes at 0x960C75 differ");
    }

    if (!g_heroLevel && same(m, 0x751910, "8b816c010000c3")) g_heroLevel = (Level_t)VA(m, 0x751910);
    if (g_summonLevelPct) logf("MageSkills: summons +%d%% damage and -as much damage taken per hero level (level %s)", g_summonLevelPct,
                               g_heroLevel ? "0x751910" : "0x751CF0: 0 for the hero, bytes at 0x751910 differ");
}

/* ---- 07.11 the summoner and curse skills (designs proposed by Claude while the user slept: TO BE CHECKED IN GAME) ----
   CurseSkill=<id> (Contagious Curse, passive): the enemies caught by the hero's Curse (game skill 0x1A: within its Param4
   metres, for its Param2 seconds) are tracked; when one dies, the curse passes to Param1 nearest living enemies within
   Param2 metres of it, which deal Param3 % less damage for Param4 seconds (and pass it on in turn, 3 generations at most).
   OwnHitFx<id> is shown on each new victim.
   BondSkill=<id> (Blood Bond, passive): Param1 % of every hit on the hero goes to his nearest summon within Param2 metres.
   VengeSkill=<id> (Call of Vengeance, passive): when one of the hero's summons dies, his hits deal Param1 % more for
   Param2 seconds. OwnHitFx<id> on the hero.
   OwnBoom<id>=1 (Summon Explosion, own skill): the hero's nearest summon within 30 m explodes: Param2 magic damage to every
   enemy within Param3 metres of it, then it dies. No summon: no cast, nothing paid. OwnFx<id> at the summon. */
#define CURSE_MAX 48
#define CURSE_GENS 3
static int g_curseSkill, g_bondSkill, g_vengeSkill, g_newLogged, g_inBond;
static struct { int h, gen; float pct; DWORD until; } g_cursed[CURSE_MAX];
static struct { float pct; DWORD until; } g_venge;
static int g_deadSeen[32], g_deadSeenN;

static int skill_index(int id)
{
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id) return i;
    return -1;
}

static int cursed_at(int h)
{
    DWORD now = GetTickCount();
    for (int k = 0; k < CURSE_MAX; k++)
        if (g_cursed[k].h == h && g_cursed[k].until && (int)(now - g_cursed[k].until) < 0) return k;
    return -1;
}

static void curse_add(int h, int gen, float pct, DWORD ms)
{
    int k = cursed_at(h);
    if (k < 0) {
        DWORD now = GetTickCount();
        for (int j = 0; j < CURSE_MAX && k < 0; j++) if (!g_cursed[j].until || (int)(now - g_cursed[j].until) >= 0) k = j;
    }
    if (k < 0) return;
    g_cursed[k].h = h; g_cursed[k].gen = gen; g_cursed[k].pct = pct; g_cursed[k].until = GetTickCount() + ms;
}

/* the living enemies of the hero within r metres of c, nearest first (at most n) */
static int enemies_near(void* hero, const float* c, float r, void** out, int n, int skipCursed)
{
    BYTE* mgr = *(BYTE**)g_statsMgr;
    if (!mgr || !readable(mgr + 0x30, 0x10)) return 0;
    void** table = *(void***)(mgr + 0x30);
    int count = *(int*)(mgr + 0x3C), m = 0;
    void* all[64]; float d2s[64];
    for (int i = 0; table && i < count && i < 100000 && m < 64; i++) {
        void* st = readable(table + i, 4) ? table[i] : 0;
        if (!st || !own_enemy(hero, st)) continue;
        if (skipCursed) {          /* 07.11: the Curse itself catches everyone around (pct 0): only skip the infected */
            int ck = cursed_at(*(int*)((BYTE*)st + 0x144));
            if (ck >= 0 && g_cursed[ck].pct > 0) continue;
        }
        float* p = g_pos(st, 0);
        if (!p) continue;
        float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r * r) continue;
        all[m] = st; d2s[m] = d2; m++;
    }
    for (int a = 1; a < m; a++) {                          /* nearest first */
        void* s = all[a]; float d = d2s[a]; int b = a;
        while (b > 0 && d2s[b - 1] > d) { all[b] = all[b - 1]; d2s[b] = d2s[b - 1]; b--; }
        all[b] = s; d2s[b] = d;
    }
    if (m > n) m = n;
    for (int a = 0; a < m; a++) out[a] = all[a];
    return m;
}

static void curse_cast(void* hero)
{
    if (!g_curseSkill || !hero || !g_hasSkill(hero, 0, g_curseSkill)) return;
    float r = g_skillParam(hero, 0, 0x1A, 3), secs = g_skillParam(hero, 0, 0x1A, 1);
    float* hp = g_pos(hero, 0);
    void* list[32];
    int n = hp ? enemies_near(hero, hp, r > 0 ? r : 10.0f, list, 32, 0) : 0;
    for (int k = 0; k < n; k++) curse_add(*(int*)((BYTE*)list[k] + 0x144), 0, 0, (DWORD)(secs * 1000.0f));
    if (g_newLogged++ < 60) logf("MageSkills: contagious curse: Curse caught %d enemies (%d m, %d s)", n, (int)r, (int)secs);
}

/* 09.11.2026: the infected show our ContagiousCurse_Aura (the game's Curse_Active was created but not seen; names are the effect list's Name; [MageSkills] CurseFx) all the time
   the curse lasts, repeated like the slow effect; it stops when the victim dies */
static char g_curseFx[48]; static char* g_curseFxFixed;
static void curse_fx_stop(int h)
{
    for (int j = 0; j < MAX_STATUS; j++) if (g_status[j].name == g_curseFx && g_status[j].handle == h) g_status[j].name = 0;
}

static void curse_spread(void* hero, void* dead, int k)
{
    curse_fx_stop(*(int*)((BYTE*)dead + 0x144));
    float* p = g_pos(dead, 0);
    if (!p || g_cursed[k].gen >= CURSE_GENS) { static int e; if (e++ < 10) logf("MageSkills: contagious curse: no spread (position %d, generation %d)", p != 0, g_cursed[k].gen); return; }
    int n = (int)(g_skillParam(hero, 0, g_curseSkill, 0) + 0.5f);
    float r = g_skillParam(hero, 0, g_curseSkill, 1), pct = g_skillParam(hero, 0, g_curseSkill, 2), secs = g_skillParam(hero, 0, g_curseSkill, 3);
    float c[3] = { p[0], p[1], p[2] };
    void* list[8];
    if (n > 8) n = 8;
    int got = n > 0 ? enemies_near(hero, c, r, list, n, 1) : 0;
    int gi = skill_index(g_curseSkill);
    for (int j = 0; j < got; j++) {
        curse_add(*(int*)((BYTE*)list[j] + 0x144), g_cursed[k].gen + 1, pct, (DWORD)(secs * 1000.0f));
        if (gi >= 0) hit_effect(list[j], gi);
        if (g_curseFx[0]) status_start(list[j], &g_curseFxFixed, g_curseFx, (DWORD)(secs * 1000.0f), 1);
    }
    static int spreadLogged;
    if (spreadLogged++ < 60) logf("MageSkills: contagious curse: %X died (generation %d) -> %d new victim(s), -%d%% damage, %d s",
                                 *(int*)((BYTE*)dead + 0x144), g_cursed[k].gen, got, (int)pct, (int)secs);
}

/* before the hit: Blood Bond and the weakening of cursed enemies / Vengeance's bonus */
static void newskills_before(void* target, BYTE* hit, void** hero_out, int* summonHit)
{
    void* hero = summon_hero();
    *hero_out = hero;
    *summonHit = 0;
    if (!hero) return;
    int from = *(int*)(hit + 0x18), to = *(int*)((BYTE*)target + 0x144), hh = *(int*)((BYTE*)hero + 0x144);
    if (g_curseSkill) {
        int k = cursed_at(from);
        if (k >= 0 && g_cursed[k].pct > 0)
            for (int j = 0; j < 4; j++) ((float*)hit)[j] *= 1.0f - g_cursed[k].pct / 100.0f;
    }
    if (g_vengeSkill && from == hh && g_venge.until && (int)(GetTickCount() - g_venge.until) < 0)
        for (int j = 0; j < 4; j++) ((float*)hit)[j] *= 1.0f + g_venge.pct / 100.0f;
    if (g_vengeSkill && to != hh && is_summon(hero, to)) *summonHit = to;
    if (g_bondSkill && !g_inBond && target == hero && from != hh && g_hasSkill(hero, 0, g_bondSkill)) {
        float pct = g_skillParam(hero, 0, g_bondSkill, 0), r = g_skillParam(hero, 0, g_bondSkill, 1);
        float* hp = g_pos(hero, 0);
        BYTE* ctrl = readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
        void* best = 0; float bd = r * r;
        int cand[9], nc = 0;
        if (ctrl && readable(ctrl + 0x54, 8))
            for (int* e = *(int**)(ctrl + 0x54); e && e < *(int**)(ctrl + 0x58) && readable(e, 8) && nc < 8; e += 2) cand[nc++] = e[0];
        int sk = skull_creature();
        if (sk != -1) cand[nc++] = sk;
        for (int j = 0; j < nc && hp; j++) {
            int h = cand[j];
            void* st = g_byHandle(*g_statsMgr, 0, &h);
            float* p = st && !(g_dead && g_dead(st, 0)) ? g_pos(st, 0) : 0;
            if (!p) continue;
            float dx = p[0] - hp[0], dy = p[1] - hp[1], dz = p[2] - hp[2], d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bd) { bd = d2; best = st; }
        }
        if (best && pct > 0) {
            BYTE part[0x38];
            for (int j = 0; j < 0x38; j++) part[j] = hit[j];
            for (int j = 0; j < 4; j++) { ((float*)part)[j] = ((float*)hit)[j] * pct / 100.0f; ((float*)hit)[j] -= ((float*)part)[j]; }
            g_inBond = 1;
            g_takeHit(best, 0, part);
            g_inBond = 0;
            if (g_newLogged++ < 60) logf("MageSkills: blood bond: %d%% of the hit (%d) goes to summon %X", (int)pct,
                                         (int)(((float*)part)[0] + ((float*)part)[1] + ((float*)part)[2] + ((float*)part)[3]), *(int*)((BYTE*)best + 0x144));
        }
    }
}

/* after the hit: deaths (a cursed enemy, a summon) */
static void newskills_after(void* target, void* hero, int summonHit)
{
    if (!hero || !g_dead || !g_dead(target, 0)) return;
    int h = *(int*)((BYTE*)target + 0x144);
    for (int j = 0; j < g_deadSeenN; j++) if (g_deadSeen[j] == h) return;     /* one death, one reaction */
    g_deadSeen[g_deadSeenN++ % 32] = h;
    if (g_deadSeenN >= 32) g_deadSeenN = 0;
    if (g_curseSkill) {
        int k = cursed_at(h);
        if (k >= 0 && g_hasSkill(hero, 0, g_curseSkill)) { curse_spread(hero, target, k); g_cursed[k].until = 0; }
    }
    if (g_vengeSkill && summonHit == h && g_hasSkill(hero, 0, g_vengeSkill)) {
        g_venge.pct = g_skillParam(hero, 0, g_vengeSkill, 0);
        g_venge.until = GetTickCount() + (DWORD)(g_skillParam(hero, 0, g_vengeSkill, 1) * 1000.0f);
        int gi = skill_index(g_vengeSkill);
        if (gi >= 0) hit_effect(hero, gi);
        if (g_newLogged++ < 60) logf("MageSkills: call of vengeance: summon %X died -> +%d%% damage for %d ms", h, (int)g_venge.pct,
                                     (int)(g_venge.until - GetTickCount()));
    }
}

/* Summon Explosion: the hero's nearest summon within 30 m (list and skull), or 0 */
static void* nearest_summon(void* hero)
{
    float* hp = g_pos(hero, 0);
    BYTE* ctrl = readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
    int cand[9], nc = 0;
    if (ctrl && readable(ctrl + 0x54, 8))
        for (int* e = *(int**)(ctrl + 0x54); e && e < *(int**)(ctrl + 0x58) && readable(e, 8) && nc < 8; e += 2) cand[nc++] = e[0];
    int sk = skull_creature();
    if (sk != -1) cand[nc++] = sk;
    void* best = 0; float bd = 30.0f * 30.0f;
    for (int j = 0; j < nc && hp; j++) {
        int h = cand[j];
        void* st = g_byHandle(*g_statsMgr, 0, &h);
        float* p = st && !(g_dead && g_dead(st, 0)) ? g_pos(st, 0) : 0;
        if (!p) continue;
        float dx = p[0] - hp[0], dy = p[1] - hp[1], dz = p[2] - hp[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bd) { bd = d2; best = st; }
    }
    return best;
}

/* 09.11 the user: every summon within 30 m explodes, each with the full damage, one after another (BOOM_GAP ms) */
#define BOOM_GAP 100
static struct { int on, hero, i, list[9], n, pos; float damage, radius; DWORD due; } g_boom;
static int all_summons(void* hero, int* out)
{
    float* hp = g_pos(hero, 0);
    BYTE* ctrl = readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
    int cand[9], nc = 0, n = 0;
    if (ctrl && readable(ctrl + 0x54, 8))
        for (int* e = *(int**)(ctrl + 0x54); e && e < *(int**)(ctrl + 0x58) && readable(e, 8) && nc < 8; e += 2) cand[nc++] = e[0];
    int sk = skull_creature();
    if (sk != -1) cand[nc++] = sk;
    for (int j = 0; j < nc && hp; j++) {
        int h = cand[j], dup = 0;
        for (int k = 0; k < n; k++) if (out[k] == h) dup = 1;
        if (dup) continue;
        void* st = g_byHandle(*g_statsMgr, 0, &h);
        float* p = st && !(g_dead && g_dead(st, 0)) ? g_pos(st, 0) : 0;
        if (!p) continue;
        float dx = p[0] - hp[0], dy = p[1] - hp[1], dz = p[2] - hp[2];
        if (dx * dx + dy * dy + dz * dz < 30.0f * 30.0f) out[n++] = h;
    }
    return n;
}
static int g_boomOn[MAX_GRANT];
static char g_boomFx[MAX_GRANT][48];      /* OwnBoomFx<id>: the effect at the exploding summon */
static char g_boomSound[MAX_GRANT][48];   /* 08.11 OwnBoomSound<id>: a game sound event at the exploding summon */
static char* g_boomFixed[MAX_GRANT];

static void boom_tick(DWORD now)
{
    if (!g_boom.on || (int)(now - g_boom.due) < 0) return;
    int a = g_boom.hero, b = g_boom.list[g_boom.pos++];
    if (g_boom.pos >= g_boom.n) g_boom.on = 0; else g_boom.due = now + BOOM_GAP;
    void* hero = g_byHandle(*g_statsMgr, 0, &a), *st = g_byHandle(*g_statsMgr, 0, &b);
    float* p = st ? g_pos(st, 0) : 0;
    if (!hero || !p) { logf("MageSkills: summon explosion: the summon is gone"); return; }
    float c[3] = { p[0], p[1], p[2] };
    if (g_boomFx[g_boom.i][0]) status_puff(st, &g_boomFixed[g_boom.i], g_boomFx[g_boom.i], 1500);
    if (g_boomSound[g_boom.i][0]) game_sound(g_boomSound[g_boom.i], st);
    void* list[32];
    int n = enemies_near(hero, c, g_boom.radius, list, 32, 0);
    float sum = 0;
    for (int k = 0; k < n; k++) { sum += own_hit(hero, list[k], g_boom.damage, c); hit_effect(list[k], g_boom.i); }
    BYTE info[0x38];                                       /* then the summon itself dies */
    g_hitCtor(info, 0);
    *(float*)(info + 8) = 1000000.0f;
    *(int*)(info + 0x14) = 5;
    *(int*)(info + 0x18) = b;
    *(int*)(info + 0x20) = 5;                              /* no reflection */
    *(float*)(info + 0x24) = c[0]; *(float*)(info + 0x28) = c[1]; *(float*)(info + 0x2C) = c[2];
    g_takeHit(st, 0, info);
    if (g_newLogged++ < 60) logf("MageSkills: summon explosion: summon %X, %d enemies within %d m, damage %d each (%d in all), dead %d",
                                 b, n, (int)g_boom.radius, n ? (int)(sum / n) : 0, (int)sum, g_dead ? g_dead(st, 0) : -1);
}

/* 07.11 Detective (DetectSkill=<id>, passive): 0x7578C0 thiscall(stats, level) ret 4 makes a character's treasure once
   (flag [stats+80h], 0x754F80 = the treasure of the template and of the character, by level; the Lua GenerateTreasure
   0x9B9F10 calls 0x754F80 too). For a dead character other than the hero, with Param1 % chance the flag is cleared and
   the treasure is made a second time: one more set of loot. */
typedef int (__fastcall *Treasure_t)(void* st, void* edx, int level);
static Treasure_t g_treasureReal;
static int g_detectSkill;
static int __fastcall hk_treasure(void* st, void* edx, int level)
{
    BYTE was = readable((BYTE*)st + 0x80, 1) ? *((BYTE*)st + 0x80) : 1;
    int r = g_treasureReal(st, edx, level);
    void* hero = summon_hero();
    if (was || !hero || st == hero || !g_hasSkill(hero, 0, g_detectSkill) || !readable((BYTE*)st + 0x80, 1) || !*((BYTE*)st + 0x80)) return r;
    float pct = g_skillParam(hero, 0, g_detectSkill, 0);
    static DWORD seed;
    seed = seed * 1103515245u + 12345u + GetTickCount();
    int roll = (int)((seed >> 16) % 100);
    if (roll < (int)pct) {
        *((BYTE*)st + 0x80) = 0;
        g_treasureReal(st, edx, level);
    }
    if (g_newLogged++ < 60) logf("MageSkills: detective: loot of %X (level %d): %d%% chance, roll %d -> %s", *(int*)((BYTE*)st + 0x144), level,
                                 (int)pct, roll, roll < (int)pct ? "a second set" : "no extra");
    return r;
}

/* 07.11 the user: Detective works on chests too. 0x8D5480 thiscall(item, level) ret 4 makes a container's treasure once
   (flag [item+E4h]; 0x8D53E0 -> the table generator 0x894670, as the characters' 0x754F80). With the same Param1 %
   chance the flag is cleared and the treasure is made again: twice the random loot. */
typedef int (__fastcall *ItemTreasure_t)(void* it, void* edx, int level);
static ItemTreasure_t g_itemTreasureReal;
static int __fastcall hk_itemTreasure(void* it, void* edx, int level)
{
    BYTE was = readable((BYTE*)it + 0xE4, 1) ? *((BYTE*)it + 0xE4) : 1;
    int r = g_itemTreasureReal(it, edx, level);
    void* hero = summon_hero();
    if (was || !hero || !g_detectSkill || !g_hasSkill(hero, 0, g_detectSkill) || !*((BYTE*)it + 0xE4)) return r;
    float pct = g_skillParam(hero, 0, g_detectSkill, 0);
    static DWORD seed;
    seed = seed * 1103515245u + 12345u + GetTickCount();
    int roll = (int)((seed >> 16) % 100);
    if (roll < (int)pct) {
        *((BYTE*)it + 0xE4) = 0;
        g_itemTreasureReal(it, edx, level);
    }
    if (g_newLogged++ < 60) logf("MageSkills: detective: container %p (level %d): %d%% chance, roll %d -> %s", it, level, (int)pct, roll,
                                 roll < (int)pct ? "twice the loot" : "no extra");
    return r;
}

/* 07.11 Eloquence (TalkSkill=<id>, passive): 0x8D8470 thiscall(item, &a, &b, bool swap) ret 0Ch returns the trade price
   (float, ST0): base [item+1B4h] + runes, times the attitude factor; the price the trader pays is divided by 5.0 at
   0x8D86C2 (fdiv qword [0x1319010]). That fdiv now reads g_sellDiv. The wrapper prices the item twice: with 5.0 and
   with 5.0/(1+Param2%); a different result means the sell branch (dearer by Param2 %), the same one a purchase
   (cheaper by Param1 %, not below 1). TODO: check in game that the gold paid is the same price as the window shows. */
typedef float (__fastcall *Price_t)(void* item, void* edx, void* a, void* b, int swap);
static Price_t g_priceReal;
static int g_talkSkill;
static double g_sellDiv = 5.0;
/* 07.11 the user: the discount adds to the attitude one. The price's only call of the attitude 0xA11940 thiscall(x, id)
   ret 4 (call at 0x8D864E) goes through hk_attitude: while g_attNeutral it answers 50 (factor 1.0 in both branches). */
typedef int (__fastcall *Attitude_t)(void* x, void* edx, int id);
static Attitude_t g_attitudeReal;
static int g_attNeutral;
static int __fastcall hk_attitude(void* x, void* edx, int id)
{
    if (g_attNeutral) return 50;
    int v = g_attitudeReal(x, edx, id);
    static int last = -12345, n;
    if (v != last && n++ < 200) { last = v; logf("MageSkills: eloquence: trader attitude %d toward %X", v, id); }
    return v;
}
static float __fastcall hk_price(void* item, void* edx, void* a, void* b, int swap)
{
    void* hero = summon_hero();
    g_sellDiv = 5.0;
    if (!hero || !g_hasSkill(hero, 0, g_talkSkill)) return g_priceReal(item, edx, a, b, swap);
    float buy = g_skillParam(hero, 0, g_talkSkill, 0), sell = g_skillParam(hero, 0, g_talkSkill, 1);
    float r5 = g_priceReal(item, edx, a, b, swap);
    g_sellDiv = 5.0 / (1.0 + sell / 100.0);
    float rs = g_priceReal(item, edx, a, b, swap);
    g_sellDiv = 5.0;
    float r = rs, base = r5;
    if (rs == r5) {                                   /* a purchase: base x (attitude factor - discount) */
        if (g_attitudeReal) { g_attNeutral = 1; base = g_priceReal(item, edx, a, b, swap); g_attNeutral = 0; }
        float f = base > 0.0f ? r5 / base : 1.0f;
        r = base * (f - buy / 100.0f);
        if (r < 1.0f) r = r5 < 1.0f ? r5 : 1.0f;
    }
    static int seen[512], nseen, talkLogged;      /* each price once per run: the window re-prices its rows every frame */
    int key = (int)r5 * 2 + (rs != r5), dup = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) { dup = 1; break; }
    if (!dup && nseen < 512) seen[nseen++] = key;
    if (!dup && talkLogged++ < 600) logf("MageSkills: eloquence: price %d (no attitude %d) -> %d (%s, -%d%% buy / +%d%% sell)", (int)r5,
                                 (int)base, (int)r, rs == r5 ? "buy" : "sell", (int)buy, (int)sell);
    return r;
}

static void newskills_init(Module m)
{
    g_detectSkill = GetPrivateProfileIntA("MageSkills", "DetectSkill", 0, g_ini);
    if (g_detectSkill) {
        /* 7578C0: push ecx / push ebx / xor ebx,ebx / push esi / mov esi,ecx / mov [esp+8],ebx / cmp [esi+80h],bl (checked 07.11) */
        if (same(m, 0x7578C0, "515333db568bf1895c2408389e80000000"))
            g_treasureReal = (Treasure_t)detour(VA(m, 0x7578C0), (void*)hk_treasure);
        logf("MageSkills: detective (skill %d) %s", g_detectSkill, g_treasureReal ? "on (0x7578C0)" : "OFF: bytes differ");
        /* 8D5480: push ecx / push esi / mov esi,ecx / cmp byte [esi+E4h],0 (checked 07.11) */
        if (g_treasureReal && same(m, 0x8D5480, "51568bf180bee400000000"))
            g_itemTreasureReal = (ItemTreasure_t)detour(VA(m, 0x8D5480), (void*)hk_itemTreasure);
        logf("MageSkills: detective for containers %s", g_itemTreasureReal ? "on (0x8D5480)" : "OFF: bytes differ");
        if (!g_treasureReal) g_detectSkill = 0;
    }
    g_talkSkill = GetPrivateProfileIntA("MageSkills", "TalkSkill", 0, g_ini);
    if (g_talkSkill) {
        /* 8D8470: push ecx / push ebx / mov ebx,ecx / fld [ebx+1B4h]; 8D86C2: fdiv qword [01319010h] (checked 07.11) */
        if (same(m, 0x8D8470, "51538bd9d983b4010000") && same(m, 0x8D86C2, "dc35")
            && *(BYTE**)((BYTE*)VA(m, 0x8D86C4)) == (BYTE*)VA(m, 0x1319010)   /* relocated: 07.11 the image sat 0x180000 higher */) {
            double* pd = &g_sellDiv;
            if (write_mem(VA(m, 0x8D86C4), &pd, 4))
                g_priceReal = (Price_t)detour(VA(m, 0x8D8470), (void*)hk_price);
            /* 8D864E: call 0xA11940 (checked 07.11); without it the two discounts multiply */
            if (g_priceReal && same(m, 0x8D864E, "e8ed921300")) {
                BYTE c[5]; c[0] = 0xE8; *(long*)(c + 1) = (long)hk_attitude - ((long)VA(m, 0x8D864E) + 5);
                g_attitudeReal = (Attitude_t)VA(m, 0xA11940);
                if (!write_mem(VA(m, 0x8D864E), c, 5)) g_attitudeReal = 0;
            }
            logf("MageSkills: eloquence: attitude call %s", g_attitudeReal ? "redirected (discounts add up)" : "NOT redirected (discounts multiply)");
        }
        logf("MageSkills: eloquence (skill %d) %s", g_talkSkill, g_priceReal ? "on (0x8D8470, 0x8D86C2)" : "OFF: bytes differ");
        if (!g_priceReal) {
            BYTE* x = VA(m, 0x8D8470); BYTE* y = VA(m, 0x8D86C2);
            logf("  8D8470: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X; 8D86C2: %02X %02X %02X %02X %02X %02X", x[0], x[1], x[2], x[3],
                 x[4], x[5], x[6], x[7], x[8], x[9], y[0], y[1], y[2], y[3], y[4], y[5]);
        }
        if (!g_priceReal) g_talkSkill = 0;
    }
    g_curseSkill = GetPrivateProfileIntA("MageSkills", "CurseSkill", 0, g_ini);
    GetPrivateProfileStringA("MageSkills", "CurseFx", "ContagiousCurse_Aura", g_curseFx, sizeof g_curseFx, g_ini);
    g_bondSkill = GetPrivateProfileIntA("MageSkills", "BondSkill", 0, g_ini);
    g_vengeSkill = GetPrivateProfileIntA("MageSkills", "VengeSkill", 0, g_ini);
    int boom = 0;
    for (int i = 0; i < g_n; i++) {
        char key[24]; wsprintfA(key, "OwnBoom%d", g_ids[i]);
        g_boomOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnBoomFx%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_boomFx[i], sizeof g_boomFx[i], g_ini);
        wsprintfA(key, "OwnBoomSound%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_boomSound[i], sizeof g_boomSound[i], g_ini);
        if (g_boomOn[i] && !g_castSound[i][0]) {           /* 08.11: OwnCastSound was read only for chain skills */
            wsprintfA(key, "OwnCastSound%d", g_ids[i]);
            GetPrivateProfileStringA("MageSkills", key, "", g_castSound[i], sizeof g_castSound[i], g_ini);
        }
        if ((g_boomSound[i][0] || g_castSound[i][0]) && !g_charSound) {
            /* 8F5910: push esi / push edi (the character sound, as the chain skills' check) */
            if (same(m, 0x8F5910, "5657")) g_charSound = (CharSound_t)VA(m, 0x8F5910);
        }
        if (g_boomOn[i]) { boom = 1; logf("MageSkills: skill %d: summon explosion (effect '%s', sound '%s', cast sound '%s')", g_ids[i],
                                          g_boomFx[i], g_boomSound[i], g_castSound[i]); }
    }
    if (g_curseSkill || g_bondSkill || g_vengeSkill || boom || g_detectSkill || g_talkSkill) {     /* the pointers of Pack Hunger (05.11) */
        g_hasSkill = (HasSkill_t)VA(m, 0x763AD0);
        g_skillParam = (SkillParam_t)VA(m, 0x75BDE0);
        *(void**)&g_hungerPlayer = VA(m, 0x6EF4D0);
        g_hungerMgr = (void**)VA(m, 0x14F13D0);
        g_hungerSkull = (void**)VA(m, 0x14F17DC);
        g_hungerSkullGet = VA(m, 0x7BD2D0);
    }
    if (g_curseSkill || g_bondSkill || g_vengeSkill)
        logf("MageSkills: contagious curse %d, blood bond %d, call of vengeance %d (needs the summon pointers: %s)", g_curseSkill,
             g_bondSkill, g_vengeSkill, g_hungerPlayer ? "ok" : "MISSING");
}

/* 06.11 Battle Cry: OwnCry<id>=1. Param1 % less damage from enemies caught by the cry (within Param3 metres at that moment; they stay weakened wherever they go, nobody else is), Param2 % more from the hero and his summons, for Param4 seconds.
   OwnSoundF<id> = the shout of a female hero: 0x759820 thiscall(stats) != 0 (0xA2D813, "female."). */
static struct { int on, hero, n, caught[32]; float less, more; DWORD until; } g_cry;
static int g_cryLogged;
static BYTE (__fastcall *g_isFemale)(void* stats, void* edx);
static void cry_catch(void* hero, float metres)
{
    BYTE* mgr = *(BYTE**)g_statsMgr;
    g_cry.n = 0;
    float* hp = g_pos(hero, 0);
    if (!mgr || !hp || !readable(mgr + 0x30, 0x10)) return;
    void** table = *(void***)(mgr + 0x30);
    int count = *(int*)(mgr + 0x3C);
    if (!table || count <= 0 || count > 100000) return;
    for (int i = 0; i < count && g_cry.n < 32; i++) {
        void* st = readable(table + i, 4) ? table[i] : 0;
        if (!st || st == hero || !readable((BYTE*)st + 0x148, 4)) continue;
        float* p = g_pos(st, 0);
        if (!p) continue;
        float dx = p[0] - hp[0], dy = p[1] - hp[1], dz = p[2] - hp[2];
        if (dx * dx + dy * dy + dz * dz > metres * metres || !own_enemy(hero, st)) continue;
        g_cry.caught[g_cry.n++] = *(int*)((BYTE*)st + 0x144);
    }
}

static void cry_hit(BYTE* hit)
{
    if ((int)(GetTickCount() - g_cry.until) >= 0) { g_cry.on = 0; return; }
    void* mgr = *g_statsMgr;
    int a = g_cry.hero, from = *(int*)(hit + 0x18);
    void* hero = mgr ? g_byHandle(mgr, 0, &a) : 0;
    if (!hero) return;
    int mine = from == g_cry.hero;
    BYTE* ctrl = readable((BYTE*)hero + 0xFC, 4) ? *(BYTE**)((BYTE*)hero + 0xFC) : 0;
    if (!mine && ctrl && readable(ctrl + 0x54, 8)) {
        int* lo = *(int**)(ctrl + 0x54), *hi = *(int**)(ctrl + 0x58);
        for (int* e = lo; e && e < hi && readable(e, 8); e += 2) mine |= e[0] == from;
    }
    if (!mine && g_hungerSkull && *g_hungerSkull) mine = ((int (__fastcall *)(void*, void*))g_hungerSkullGet)(*g_hungerSkull, 0) == from;
    float mul = 1.0f;
    if (mine) mul = 1.0f + g_cry.more / 100.0f;
    else {
        int caught = 0;
        for (int k = 0; k < g_cry.n; k++) caught |= g_cry.caught[k] == from;
        if (!caught) return;
        mul = 1.0f - g_cry.less / 100.0f;
    }
    for (int k = 0; k < 4; k++) ((float*)hit)[k] *= mul;
    if (g_cryLogged++ < 40) logf("MageSkills: battle cry: hit from %X x%d%%", from, (int)(mul * 100.0f));
}

/* 06.11 Ice Arrow: IceArrowSkill=<id>, on the game's Poison Arrows code (CodePath; Param1, Param2 = its poison, 0; the Stun Arrows code deals no damage and never reaches 0x76C800), Slow<id> as Ice Bolt's.
   While its shot is pending the arrow's effects are Ice Bolt's: 0x748AD8 call 0x8FFAF0 thiscall(&fly, &impact, &third) ret 0Ch on the projectile's description. own_cast remembers the cast; the shooter's next hit within
   3 s takes the slow: slow_request reads the target at [+24h] and the rank at [+30h] of what it is given. */
static int g_iceSkill, g_iceHero, g_iceRank, g_iceGi, g_iceLogged;
static float g_iceDamage;
static float (__fastcall *g_iceParam)(void* stats, void* edx, int id, int index);
static DWORD g_iceUntil;   /* the released arrow is flying: its hit takes the damage and the slow */
static DWORD g_iceShot;    /* 06.11 cast, the shot not built yet: its effects are ice, no poison. Apart from g_iceUntil: another arrow landing during the draw used to clear the state, and the shot came out poisoned */
/* 06.11 Arrow Volley: VolleySkill=<id>, on the game's Poison Arrows code, Param1, Param2 = its poison, 0 (06.11 anything else poisons the target); Param3 arrows in all, Param4 % of the bow's damage each. The shot 0x748520 thiscall(x, y, z) ret 0Ch (called at 0x74A47B, 0x74A799, 0x74A8C9, 0x74AA8E, 0x74AD37)
   is called again from own_tick every VOLLEY_GAP ms with the same aim, while the action is readable and keeps the vtable it had at the first shot, and only for VOLLEY_WINDOW ms after it. TODO: no proof of the action's life beyond that check. */
#define VOLLEY_GAP 100
#define VOLLEY_WINDOW 800
static int g_volleySkill, g_volleyHero, g_volleyLeft, g_volleyHits, g_volleyLogged;
static float g_volleyPct, g_volleyAim[3];
static DWORD g_volleyCast, g_volleyNext, g_volleyUntil;
static void* g_volleyAction, *g_volleyVt;
static DWORD g_volleyEnd;
static char** g_plainFly, **g_plainImpact;
typedef void (__fastcall *Shot_t)(void* action, void* edx, float x, float y, float z);
static Shot_t g_shot;
static int volley_casting(void) { return g_volleyCast && (int)(GetTickCount() - g_volleyCast) < 0; }
static void __fastcall volley_shot(void* action, void* edx, float x, float y, float z)
{
    int mine = volley_casting() && !g_volleyAction;
    g_shot(action, edx, x, y, z);
    if (!mine) return;
    g_volleyAction = action;
    g_volleyVt = *(void**)action;
    g_volleyEnd = GetTickCount() + VOLLEY_WINDOW;
    g_volleyAim[0] = x; g_volleyAim[1] = y; g_volleyAim[2] = z;
    g_volleyNext = GetTickCount() + VOLLEY_GAP;
    g_volleyHits++;
    g_volleyUntil = GetTickCount() + 4000;
}
static void volley_tick(DWORD now)
{
    if (!g_volleyAction || g_volleyLeft <= 0 || (int)(now - g_volleyNext) < 0) return;
    if ((int)(now - g_volleyEnd) >= 0 || !readable(g_volleyAction, 4) || *(void**)g_volleyAction != g_volleyVt) {
        if (g_volleyLogged++ < 40) logf("MageSkills: volley: stopped, %d arrows not fired", g_volleyLeft);
        g_volleyLeft = 0;
        return;
    }
    g_volleyLeft--;
    g_volleyNext = now + VOLLEY_GAP;
    g_volleyHits++;
    g_volleyUntil = now + 4000;
    g_shot(g_volleyAction, 0, g_volleyAim[0], g_volleyAim[1], g_volleyAim[2]);
    if (g_volleyLogged++ < 40) logf("MageSkills: volley: extra arrow, %d left", g_volleyLeft);
}
static char* g_iceFly, *g_iceImpact;
typedef void (__fastcall *ArrowFx_t)(void* desc, void* edx, char** fly, char** impact, void* third);
static ArrowFx_t g_arrowFx;
static void __fastcall ice_arrow_fx(void* desc, void* edx, char** fly, char** impact, void* third)
{
    if (g_iceShot && (int)(GetTickCount() - g_iceShot) < 0) {
        char* a = fx_string(&g_iceFly, "IceArrow_Travel"), *b = fx_string(&g_iceImpact, "IceArrow_Impact");
        if (a && b) { fly = &g_iceFly; impact = &g_iceImpact; }
    }
    else if (volley_casting() && g_plainFly) { fly = g_plainFly; impact = g_plainImpact; }
    g_arrowFx(desc, edx, fly, impact, third);
}

/* 06.11 the poison status the arrow would put on its target (green cloud on the enemy): 0x748B2E call 0x8FFD70 thiscall(type = 7, time, flag, 6 dwords by value, 3 dwords) ret 30h.
   The by-value block owns two blocks freed by the callee, [0] always, [4] if set (0x8FFE3B..0x8FFE60, operator delete 0xC8FCAC): skipping the call frees them here. */
typedef void (__fastcall *ArrowStatus_t)(void* desc, void* edx, int type, float time, int flag, void* s0, int s1, int s2, int s3, void* s4, int s5, int a, int b, int c);
static ArrowStatus_t g_arrowStatus;
typedef void (__cdecl *GameFree_t)(void*);
static GameFree_t g_gameFree;
static void __fastcall ice_arrow_status(void* desc, void* edx, int type, float time, int flag, void* s0, int s1, int s2, int s3, void* s4, int s5, int a, int b, int c)
{
    if (type == 7 && (volley_casting() || (g_iceShot && (int)(GetTickCount() - g_iceShot) < 0))) {
        if (!volley_casting()) g_iceUntil = GetTickCount() + 3000;
        GameFree_t f = g_gameFree;
        if (s4) f(s4);
        f(s0);
        return;
    }
    g_arrowStatus(desc, edx, type, time, flag, s0, s1, s2, s3, s4, s5, a, b, c);
}

/* 06.11 the arrow's cast effect: the shared tail of the shot, 0x749E15 call 0x810BA0 thiscall(&name, 1, out) ret 0Ch, skips the effect at 0x749E1E when it returns 0.
   While an Ice Arrow is pending the poison code's Arrow_Poison_Cast (green circle and glow round the hero) is replaced by IceArrow_Cast, the same effect recoloured with snowflakes. */
typedef void* (__fastcall *CastFx_t)(void* self, void* edx, char** name, int one, void* out);
static CastFx_t g_castFx;
static char* g_iceCast;
static void* __fastcall ice_cast_fx(void* self, void* edx, char** name, int one, void* out)
{
    if (volley_casting() && name && *name) {
        const char* a = *name, *b = "Arrow_Poison_Cast";
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return 0;
    }
    if (g_iceShot && (int)(GetTickCount() - g_iceShot) < 0 && name && *name) {
        const char* a = *name, *b = "Arrow_Poison_Cast";
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) {
            g_iceShot = 0;
            if (fx_string(&g_iceCast, "IceArrow_Cast")) name = &g_iceCast;
        }
    }
    return g_castFx(self, edx, name, one, out);
}

/* 07.11 Ram Charge (RamSkill=<id>, on the code of Rush Attack: the rush, Param1 = times the damage, Param2 = % stun of the
   target, both the game's). Ours: the hero's first melee hit within 3 s of the cast hits every other enemy within Param4 m
   of the target for Param3 % of that hit, and each of them (the target too) falls with Param5 % chance (knock_start, the
   way Arcane Push does it: OwnKnock<id>=1, OwnHitFx<id> = the effect shown with the fall). */
static int   g_ramSkill, g_ramI = -1, g_ramLogged;
static DWORD g_ramSeed;
static struct { int hero; DWORD until; float pct, metres, knock; void* stats; } g_ram;

static int ram_roll(float pct)
{
    g_ramSeed = g_ramSeed * 1103515245u + 12345u + GetTickCount();
    return (int)((g_ramSeed >> 16) % 100) < (int)pct;
}

static void ram_cast(void* who)
{
    if (!who || !g_iceParam) return;
    g_ram.hero = *(int*)((BYTE*)who + 0x144);
    g_ram.stats = who;
    g_ram.pct = g_iceParam(who, 0, g_ramSkill, 2);
    g_ram.metres = g_iceParam(who, 0, g_ramSkill, 3);
    g_ram.knock = g_iceParam(who, 0, g_ramSkill, 4);
    if (g_ram.metres <= 0) g_ram.metres = 3;
    g_ram.until = GetTickCount() + 3000;
    if (g_ramLogged++ < 20) logf("MageSkills: ram charge: %d%% around, %d m, fall %d%%", (int)g_ram.pct, (int)g_ram.metres, (int)g_ram.knock);
}

static void ram_hit(void* target, BYTE* hit)
{
    if ((int)(GetTickCount() - g_ram.until) >= 0) { g_ram.until = 0; return; }
    if (*(int*)(hit + 0x18) != g_ram.hero || *(int*)(hit + 0x14) != 1 || *(int*)((BYTE*)target + 0x144) == g_ram.hero) return;
    g_ram.until = 0;                                       /* once: the hits below come back here */
    void* hero = g_ram.stats;
    float* tp = g_pos(target, 0);
    BYTE* mgr = *(BYTE**)g_statsMgr;
    if (!tp || !mgr || !readable(mgr + 0x30, 0x10) || !readable(hero, 0x148) || *(int*)((BYTE*)hero + 0x144) != g_ram.hero) return;
    float c[3] = { tp[0], tp[1], tp[2] };
    float* f = (float*)hit, part[4];
    for (int k = 0; k < 4; k++) part[k] = f[k] * g_ram.pct / 100.0f;
    g_ownCaster = hero;
    int fell = 0, hitN = 0;
    if (g_ramI >= 0 && ram_roll(g_ram.knock)) { knock_start(target, g_ramI); fell++; }
    void** table = *(void***)(mgr + 0x30);
    int count = *(int*)(mgr + 0x3C);
    for (int i = 0; table && i < count && i < 100000 && hitN < 16; i++) {
        void* st = readable(table + i, 4) ? table[i] : 0;
        if (!st || st == hero || st == target || !readable((BYTE*)st + 0x148, 4)) continue;
        float* p = g_pos(st, 0);
        if (!p) continue;
        float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        if (dx * dx + dy * dy + dz * dz > g_ram.metres * g_ram.metres || !own_enemy(hero, st)) continue;
        BYTE info[0x38];
        g_hitCtor(info, 0);
        for (int k = 0; k < 4; k++) ((float*)info)[k] = part[k];
        *(int*)(info + 0x14) = 1;
        *(int*)(info + 0x18) = g_ram.hero;
        *(int*)(info + 0x1C) = 0;
        *(int*)(info + 0x20) = 1;
        float* hp = g_pos(hero, 0);
        if (hp) { *(float*)(info + 0x24) = hp[0]; *(float*)(info + 0x28) = hp[1]; *(float*)(info + 0x2C) = hp[2]; }
        g_takeHit(st, 0, info);
        hitN++;
        if (g_ramI >= 0 && ram_roll(g_ram.knock)) { knock_start(st, g_ramI); fell++; }
    }
    if (g_ramLogged++ < 40) logf("MageSkills: ram charge hit %d %d %d %d: %d more enemies hit, %d fall", (int)f[0], (int)f[1], (int)f[2], (int)f[3], hitN, fell);
}

static BYTE __fastcall hk_takeHit(void* stats, void* edx, void* hit)
{
    int iceFixed = 0;
    if (g_volleyHits > 0 && hit && *(int*)((BYTE*)hit + 0x14) == 3 && *(int*)((BYTE*)hit + 0x18) == g_volleyHero && (int)(GetTickCount() - g_volleyUntil) < 0) {
        g_volleyHits--;
        for (int k = 0; k < 4; k++) ((float*)hit)[k] *= g_volleyPct / 100.0f;
        if (g_volleyLogged++ < 40) logf("MageSkills: volley: hit x%d%%, %d arrows on the way", (int)g_volleyPct, g_volleyHits);
    }
    if (g_iceUntil && hit && *(int*)((BYTE*)hit + 0x14) == 3 && *(int*)((BYTE*)hit + 0x18) == g_iceHero && *(int*)((BYTE*)stats + 0x144) != g_iceHero) {
        if ((int)(GetTickCount() - g_iceUntil) < 0) {
            if (*(int*)((BYTE*)hit + 0x14) == 3 && g_iceDamage > 0) {     /* 06.11 the arrow's own damage (Param3, magic) instead of the bow's */
                float* f = (float*)hit;
                f[0] = f[1] = f[3] = 0.0f;
                f[2] = g_iceDamage;
                iceFixed = 1;
            }
            BYTE inst[0x40];
            for (int k = 0; k < 0x40; k++) inst[k] = 0;
            *(int*)(inst + 0x24) = *(int*)((BYTE*)stats + 0x144);
            *(int*)(inst + 0x30) = g_iceRank;
            slow_request(g_iceGi, inst);
            if (g_iceLogged++ < 20) logf("MageSkills: ice arrow: target %X, rank %d, hit type %d", *(int*)(inst + 0x24), g_iceRank, *(int*)((BYTE*)hit + 0x14));
        }
        g_iceUntil = 0;
    }
    if (g_windSkill && hit && g_wind.handle == *(int*)((BYTE*)stats + 0x144) && (int)(GetTickCount() - g_wind.until) < 0)
        for (int k = 0; k < 4; k++) ((float*)hit)[k] *= 1.0f - g_wind.less / 100.0f;
    if (g_mark.on && hit && *(int*)((BYTE*)stats + 0x144) == g_mark.handle) {
        if ((int)(GetTickCount() - g_mark.until) >= 0) g_mark.on = 0;
        else {
            for (int k = 0; k < 4; k++) ((float*)hit)[k] *= 1.0f + g_mark.pct / 100.0f;
            if (g_markLogged < 30) { g_markLogged++; logf("MageSkills: mark: hit on the marked target +%d%%", (int)g_mark.pct); }
        }
    }
    if (g_markPct > 0 && hit) {
        void* list = status_list(stats);
        if (list && ((BYTE (__fastcall *)(void*, void*, int))g_markHas)(list, 0, 0x29)) {
            for (int k = 0; k < 4; k++) ((float*)hit)[k] *= 1.0f + g_markPct / 100.0f;
            if (g_markLogged < 30) { g_markLogged++; logf("MageSkills: mark: hit on the marked target +%d%%", (int)g_markPct); }
        }
    }
    if (g_cry.on && hit) cry_hit((BYTE*)hit);
    if (g_ram.until && hit) ram_hit(stats, (BYTE*)hit);
    float sumIn[4] = { 0 };
    int sumFrom = 0;
    if (g_summonTrace && hit && g_hungerPlayer) {        /* 07.11 SummonTrace=1: what the hero's summons deal */
        void* hero = summon_hero();
        int from = *(int*)((BYTE*)hit + 0x18);
        if (hero && from != *(int*)((BYTE*)stats + 0x144) && is_summon(hero, from)) {
            sumFrom = from;
            for (int k = 0; k < 4; k++) sumIn[k] = ((float*)hit)[k];
        }
    }
    if (g_summonLevelPct && hit) summon_level_hit(stats, (BYTE*)hit);
    void* nsHero = 0; int nsSummon = 0;
    if ((g_curseSkill || g_bondSkill || g_vengeSkill) && hit) newskills_before(stats, (BYTE*)hit, &nsHero, &nsSummon);
    if (g_breakSkill && g_critId && hit) armor_breaker(stats, (BYTE*)hit);
    if (g_critId && hit) spell_crit((BYTE*)hit);
    if (g_aimSkill && g_critId && hit && !iceFixed && *(int*)((BYTE*)hit + 0x14) == 3) aimed_shot(stats, (BYTE*)hit);
    if (g_hitFieldTrace && hit && g_hitFieldN < 300) {
        BYTE* h = (BYTE*)hit;
        g_hitFieldN++;
        logf("HitTrace: from %X to %X dmg %d %d %d %d type14 %d f1C %d f20 %d flags %d %d %d %d caller %X t %u", *(int*)(h + 0x18), *(int*)((BYTE*)stats + 0x144),
             (int)((float*)h)[0], (int)((float*)h)[1], (int)((float*)h)[2], (int)((float*)h)[3], *(int*)(h + 0x14), *(int*)(h + 0x1C), *(int*)(h + 0x20),
             h[0x30], h[0x31], h[0x32], h[0x33], (DWORD)__builtin_return_address(0), GetTickCount());
    }
    if (g_shield.on && stats == g_shield.stats && hit) {
        float* v = (float*)hit;
        float most = 0;
        for (int k = 0; k < 4; k++) if (v[k] > most) most = v[k];
        float* mana = (float*)((BYTE*)stats + 0x190);
        if (most > 0) {
            float keep = 1.0f - g_shield.absorb, taken = most * g_shield.absorb, cost = taken * g_shield.cost;
            if (cost > *mana && cost > 0) {            /* what the mana left can pay */
                float part = *mana / cost;
                keep = 1.0f - g_shield.absorb * part; taken *= part; cost = *mana;
            }
            if (g_shieldLogged < 30) {
                g_shieldLogged++;
                logf("MageSkills: shield hit %d %d %d %d (type %d): absorbs %d, mana -%d", (int)v[0], (int)v[1], (int)v[2], (int)v[3],
                     *(int*)((BYTE*)hit + 0x14), (int)taken, (int)cost);
            }
            for (int k = 0; k < 4; k++) if (v[k] > 0) v[k] *= keep;
            *mana -= cost;
            DWORD now = GetTickCount();
            if ((int)(now - g_shield.nextFx) >= 0) { g_shield.nextFx = now + 300; shield_fx(stats, g_shield.i); }
            if (*mana <= 0.5f) shield_end("no mana");
        }
    }
    /* 05.11: Reflect's hit (0x76CE58) has [+20h] = 5, which guards against reflecting a reflection and also skips 0x76B330 at 0x76C92C, the
       part of a usual hit that shows it. It is called here on a copy with a usual [+20h]; the real hit keeps its 5. */
    if (g_showHit && hit && __builtin_return_address(0) == (void*)g_reflectRet) {
        BYTE copy[0x38];
        for (int k = 0; k < 0x38; k++) copy[k] = ((BYTE*)hit)[k];
        *(int*)(copy + 0x20) = 1;
        ((BYTE*)hit)[0x31] = 1;     /* 05.11: the game clears it for Reflect (0x76CE41), which leaves the target 1 hit point (0x76CB69..0x76CB84): now it can kill */
        g_showHit(stats, 0, copy);
    }
    BYTE taken = g_takeHitReal(stats, edx, hit);
    if (nsHero) newskills_after(stats, nsHero, nsSummon);
    if (sumFrom && g_summonTraceN < 300) {
        BYTE* h = (BYTE*)hit;
        float* f = (float*)hit;
        int sk = sumFrom == skull_creature();
        g_summonTraceN++;
        logf("SummonTrace: %s %X -> %X: in %d/%d/%d/%d, out %d/%d/%d/%d, dealt %d (type %d, crit %d)", sk ? "skull creature" : "summon",
             sumFrom, *(int*)((BYTE*)stats + 0x144), (int)sumIn[0], (int)sumIn[1], (int)sumIn[2], (int)sumIn[3],
             (int)f[0], (int)f[1], (int)f[2], (int)f[3], (int)*(float*)(h + 0x34), *(int*)(h + 0x14), *(int*)(h + 0x10));
    }
    if (g_windSkill) wind_after_hit(stats);
    if (g_hungerSkill && hit) hunger_after_hit((BYTE*)hit);
    return taken;
}

static void shield_tick(DWORD now)
{
    if (!g_shield.on) return;
    void* mgr = *g_statsMgr;
    int h = g_shield.hero;
    void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (st != g_shield.stats) { g_shield.on = 0; return; }           /* another level: the visual is gone */
    if (g_statusDbg && g_statusDbgN < 8 && now - g_statusDbgAt > (DWORD)(g_statusDbgN + 1) * 500) {
        g_statusDbgN++;
        logf("MageSkills: shield status after %u ms: has 0xF = %d; time %d, [+A4] %d, [+A8] %d, owner [+8] %X, [+29] %d, list pending %d", now - g_statusDbgAt,
             ((StatusHas_t)g_statusHasVa)(g_statusDbgList, 0, 0xF), (int)(*(float*)(g_statusDbg + 0x24) * 100), (int)(*(float*)(g_statusDbg + 0xA4) * 100),
             g_statusDbg[0xA8], *(DWORD*)(g_statusDbg + 8), g_statusDbg[0x29], g_statusDbgList[0x5C]);
    }
    if ((int)(now - g_shield.until) >= 0) { shield_end("time"); return; }
    /* 04.11: the skin showed once only. 0x7935A0 does its work (0x792050) only when the flag changes, and the look does
       not stay by itself, so it is set again off and on (the game's status 0x10 also goes on and off) */
    if (g_skinPulse && (int)(now - g_shield.nextSkin) >= 0) {
        g_shield.nextSkin = now + (DWORD)g_skinPulse;
        void* v = ((Visual_t)(*(void***)st)[0x68 / 4])(st, 0);
        if (v) { g_skin(v, 0, 0); g_skin(v, 0, 1); }
    }
}

static void shield_start(void* stats, int i, float absorb, float secs, float cost)
{
    if (absorb > 95) absorb = 95;
    g_shield.hero = *(int*)((BYTE*)stats + 0x144);
    g_shield.stats = stats;
    g_shield.i = i;
    g_shield.absorb = absorb / 100.0f;
    g_shield.cost = cost / 100.0f;
    g_shield.until = GetTickCount() + (DWORD)(secs * 1000.0f);
    g_shield.nextFx = 0;
    g_shield.nextSkin = GetTickCount() + (DWORD)g_skinPulse;
    if (!g_shield.on) {
        int k = g_skinPick >= 0 ? g_skinPick : g_skinNext++ % SKIN_KINDS;
        if (g_skins[k]) g_skin = g_skins[k];
        logf("MageSkills: shield skin kind %d (0x%X)", k, g_skinVa[k]);
        shield_skin(stats, 1);
        shield_status(stats, secs);
    }
    g_shield.on = 1;
    logf("MageSkills: shield up: %d%% for %d s, %d mana per 100", (int)absorb, (int)secs, (int)cost);
}

/* 05.11 diagnostics (MeleeTrace=1): after Thousand Strikes a jump attack gives mana back, takes it at its end and spins the
   camera. 0x959C10 is the start of Thousand Strikes (camera 0x945Exx, cooldown and mana of 0x1D and 0x3A), called from
   0x95C297/0x95C2B0, the action's vtable slot (vt 0x13330D4..). Suspect: it runs a second time on the jump attack. */
typedef void (__fastcall *ThousandStart_t)(BYTE* action, void* edx);
static ThousandStart_t g_thousandStart;
#define JUMP_GUARD 1500
typedef void* (__fastcall *ActionOwner_t)(BYTE* action, void* edx);
static ActionOwner_t g_actionOwner;              /* 0x8FD5A0 */
static void* g_jumpHero;
static DWORD g_jumpAt;
static int g_meleeLogged;
static void __fastcall hk_thousand(BYTE* action, void* edx)
{
    /* 05.11, the game's own bug (log: pay 31, mana to the top by Mana Leech, ~700 ms later pay 29 from 0x959D4A): the start runs
       again when a jump attack ends, with its mana, cooldown and camera. Never within JUMP_GUARD ms after the hero's jump attack. */
    void* who = g_actionOwner(action, 0);
    if (who && who == g_jumpHero && GetTickCount() - g_jumpAt < JUMP_GUARD) {
        if (g_meleeLogged++ < 20) logf("MageSkills: Thousand Strikes start after a jump attack: skipped");
        action[0x20] = 0;
        return;
    }
    g_thousandStart(action, edx);
    /* 05.11: the camera. The action's flag [+20h] makes its start 0x95B920 turn the camera (0x95BCxx) and this end restore it; left
       set after Thousand Strikes, a jump attack on the same action spins the camera too */
    if (who && who == g_jumpHero && action[0x20]) { action[0x20] = 0; if (g_meleeLogged++ < 20) logf("MageSkills: camera flag of the action cleared"); }
}

/* 05.11 MeleeTrace, second part: every skill payment 0x764330 of a character who owns Magic Missile (the hero) with its caller and
   the mana, and every jump of his mana by 2 or more seen by the message loop */
static Skill1b_t g_payReal;
static int g_meleeTrace;
static void* g_traceHero;
static float g_traceMana;
static int   g_traceLogged;
static BYTE __fastcall hk_pay(void* stats, void* edx, int id)
{
    void* ra = __builtin_return_address(0);
    int hero = g_find(stats, 0, ID_MISSILE) != 0;
    if (hero) g_jumpHero = stats;
    if (hero && id == 0x1F) g_jumpAt = GetTickCount();
    float before = hero ? *(float*)((BYTE*)stats + 0x190) : 0;
    BYTE r = g_payReal(stats, edx, id);
    if (hero) {
        g_traceHero = stats;
        g_traceMana = *(float*)((BYTE*)stats + 0x190);
        if (g_meleeTrace && g_traceLogged++ < 200) logf("MageSkills: pay skill %d from %p: mana %d -> %d, t %u", id, ra, (int)before, (int)g_traceMana, GetTickCount());
    }
    return r;
}

static void trace_tick(DWORD now)
{
    if (!g_traceHero || !readable((BYTE*)g_traceHero + 0x190, 4)) return;
    float m = *(float*)((BYTE*)g_traceHero + 0x190), d = m - g_traceMana;
    if (d >= 2.0f || d <= -2.0f) {
        if (g_traceLogged++ < 200) logf("MageSkills: mana %d -> %d, t %u", (int)g_traceMana, (int)m, now);
        g_traceMana = m;
    }
}

/* 05.11: no hit reaction under the shield. The log (HitTrace, 05.11) showed the hero's reaction starts in 0x73D530, the start method of the action
   with vtable 0x131FD2C (slots 38, 52), called through 0x8FD4C0 from 0x74FADA; [this+4] is the character's handle, [this+30h] picks Hit_Knockdown.
   It returns 0 by itself when the animation is missing (0x73D9A1) and the caller then drops the action (0x74FAFE), so the same answer here means
   no reaction. A knockdown still plays. */
typedef BYTE (__fastcall *HitStart_t)(void* action, void* edx);
static HitStart_t g_hitStart;

static BYTE __fastcall hk_hitStart(void* action, void* edx)
{
    BYTE* a = (BYTE*)action;
    if (g_shield.on && *(int*)(a + 4) == g_shield.hero && !a[0x30]) return 0;
    return g_hitStart(action, edx);
}

/* 05.11: buying from a trainer crashed (read 0Ch at 0x89D490 from 0x9D0AA3). After a purchase 0x9D0A68.. walks every record of the trainer: 0x8ABE30(id) finds the
   hero's skill, 0x89D490 reads its cap, and the row is shown when the cap is below the row's (0x9D0AAF). The game counts on the hero having each sold skill, and null is
   read when he does not. The null now answers "cap is at its highest", which skips the row (the next read, 0x9D0B11, is on the same path). The id goes to the log. */
typedef void* (__fastcall *HeroSkill_t)(void* stats, void* edx, int id);
static HeroSkill_t g_heroSkill;
static int g_rowLogged;
static void* __fastcall row_skill(void* stats, void* edx, int id)
{
    void* r = g_heroSkill(stats, edx, id);
    if (!r && g_rowLogged++ < 20) logf("MageSkills: trainer record of skill %d: the hero has no such skill, row skipped", id);
    return r;
}
static int __fastcall row_cap(BYTE* skill) { return skill ? *(int*)(skill + 0xC) : 0x7FFFFFFF; }

/* 05.11: the trainer's window lists a skill only when the hero has it (0xB1B2BC), and our skills were given only when the skill tree opens: so they are given
   here too. 0x6EF4D0 on [0x14F13D0] is the character the window itself asks for (0xB1B215). */
typedef void* (__fastcall *Player_t)(void* mgr, void* edx);
static TrainerList_t g_trainerOpen;
static Player_t g_player;
static void** g_playerMgr;
static void __fastcall hk_trainerOpen(void* self, void* edx)
{
    void* mgr = *g_playerMgr;
    if (mgr) give_skills_ch(g_player(mgr, 0));
    g_trainerOpen(self, edx);
}

/* 05.11 diagnostics (HitFieldTrace=1): what the poison status 7 gets: 0xA5E1F0(x) makes the tick [+0A8h] = x / time [+24h]. TODO: remove before release. */
typedef void (__fastcall *PoisonSet_t)(BYTE* st, void* edx, float x);
static PoisonSet_t g_poisonSet;
static void __fastcall poison_set(BYTE* st, void* edx, float x)
{
    logf("HitTrace: poison set from %X: value %d, time %d.%02d, period %d.%02d, t %u", (DWORD)__builtin_return_address(0), (int)x, (int)*(float*)(st + 0x24), (int)(*(float*)(st + 0x24) * 100) % 100,
         (int)*(float*)(st + 0xAC), (int)(*(float*)(st + 0xAC) * 100) % 100, GetTickCount());
    g_poisonSet(st, edx, x);
}

/* 06.11 Weapon Expertise Bow: BowSkill=<id>, Param1 = % faster, five ranks, starting at rank 1 like the other weapon skills. The kf has copies of the shots (Ranged_*_BOW_S1..S4, rank 1 plays the game's own, MageExpansion build_mage BOW_TIERS) with their frequency and,
   in the animation data, their times. 0x744C0D: 0x7445E0 puts the shot's sequence name in the action's string [+50h]; the suffix is added here for the rank of the character who has the skill. */
static int g_bowLogged;
static void (__fastcall *g_bowName)(BYTE* action, void* edx);
static void* (__fastcall *g_bowStats)(BYTE* action, void* edx);
static const char* (__fastcall *g_strGet)(void* str, void* edx);
static void (__fastcall *g_strSet)(void* str, void* edx, const char* text);

static void __fastcall bow_name(BYTE* action, void* edx)
{
    g_bowName(action, edx);
    void* st = g_bowStats(action, 0);
    if (!st || !g_hasSkill(st, 0, g_bowSkill)) return;
    float pct = g_skillParam(st, 0, g_bowSkill, 0);
    const char* tag = pct >= 50 ? "_S4" : pct >= 40 ? "_S3" : pct >= 25 ? "_S2" : pct >= 10 ? "_S1" : 0;
    const char* now = g_strGet(action + 0x50, 0);
    int n = s_len(now);
    if (!tag || n < 11 || n > 40 || now[0] != 'R' || now[n - 1] != 'W' || now[n - 4] != '_') return;
    char buf[64];
    for (int i = 0; i < n; i++) buf[i] = now[i];
    for (int i = 0; i < 4; i++) buf[n + i] = tag[i];
    g_strSet(action + 0x50, 0, buf);
    if (g_bowLogged++ < 12) logf("MageSkills: bow: %s", buf);
}

static void shield_init(Module m)
{
    g_bowSkill = GetPrivateProfileIntA("MageSkills", "BowSkill", 0, g_ini);
    if (g_bowSkill) {
        g_hasSkill = (HasSkill_t)VA(m, 0x763AD0);
        g_skillParam = (SkillParam_t)VA(m, 0x75BDE0);
        *(void**)&g_bowName = VA(m, 0x7445E0);
        *(void**)&g_bowStats = VA(m, 0x8FD5A0);
        *(void**)&g_strGet = *(void**)VA(m, 0x12B7590);
        *(void**)&g_strSet = *(void**)VA(m, 0x12B7570);
        logf("MageSkills: bow expertise (skill %d) -> %s", g_bowSkill, patch_call(VA(m, 0x744C0D), VA(m, 0x7445E0), (void*)bow_name) ? "on (0x744C0D)" : "OFF: bytes differ");
    }
    if (!GetPrivateProfileIntA("MageSkills", "TrainerTrace", 0, g_ini)) {
        /* B1B200: push ebp / mov ebp,esp / and esp,-8 / sub esp,50h; B1B20F: mov ecx,[14F13D0h] / call 6EF4D0 */
        BYTE* c = VA(m, 0xB1B215);
        if (same(m, 0xB1B200, "558bec83e4f883ec50") && same(m, 0xB1B20F, "8b0d") && *(DWORD*)VA(m, 0xB1B211) == 0x14F13D0 + (DWORD)m.base - 0x400000 && c[0] == 0xE8 &&
            (DWORD)(c + 5 + *(int*)(c + 1)) == 0x6EF4D0 + (DWORD)m.base - 0x400000) {
            g_player = (Player_t)VA(m, 0x6EF4D0);
            g_playerMgr = (void**)VA(m, 0x14F13D0);
            g_trainerOpen = (TrainerList_t)detour(VA(m, 0xB1B200), (void*)hk_trainerOpen);
        }
        logf("MageSkills: skills given when a trainer opens: %s", g_trainerOpen ? "on (0xB1B200)" : "OFF: bytes differ");
    }
    g_hitFieldTrace = GetPrivateProfileIntA("MageSkills", "HitFieldTrace", 0, g_ini);
    if (same(m, 0x76CE50, "c744243c05000000e8") && same(m, 0x76B330, "56578b7c240c")) {   /* mov [esp+3Ch],5 / call 76C800; push esi / push edi / mov edi,[esp+0Ch] */
        g_reflectRet = (DWORD)VA(m, 0x76CE5D);
        g_showHit = (ShowHit_t)VA(m, 0x76B330);
        logf("MageSkills: Reflect's damage is shown (0x76B330 from 0x76CE58)");
    } else logf("MageSkills: Reflect's damage not shown: bytes differ");
    if (g_hitFieldTrace && same(m, 0x768510, "81ecc8000000")) g_leechReal = (Leech_t)detour(VA(m, 0x768510), (void*)hk_leech);
    g_hungerSkill = GetPrivateProfileIntA("MageSkills", "HungerSkill", 0, g_ini);
    if (g_hungerSkill) {
        g_hasSkill = (HasSkill_t)VA(m, 0x763AD0);
        g_skillParam = (SkillParam_t)VA(m, 0x75BDE0);
        *(void**)&g_hungerPlayer = VA(m, 0x6EF4D0);
        g_hungerMgr = (void**)VA(m, 0x14F13D0);
        g_hungerSkull = (void**)VA(m, 0x14F17DC);
        g_hungerSkullGet = VA(m, 0x7BD2D0);
        logf("MageSkills: pack hunger on (skill %d)", g_hungerSkill);
    }
    g_windSkill = GetPrivateProfileIntA("MageSkills", "WindSkill", 0, g_ini);
    if (g_windSkill) {
        g_hasSkill = (HasSkill_t)VA(m, 0x763AD0);
        g_skillParam = (SkillParam_t)VA(m, 0x75BDE0);
        g_health = (Health_t)VA(m, 0x75EAD0);
        g_maxHealth = (MaxHealth_t)VA(m, 0x75BBD0);
        GetPrivateProfileStringA("MageSkills", "WindFx", "", g_windFx, sizeof g_windFx, g_ini);
        logf("MageSkills: second wind on (skill %d)", g_windSkill);
    }
    g_iceSkill = GetPrivateProfileIntA("MageSkills", "IceArrowSkill", 0, g_ini);
    if (g_iceSkill) {
        g_arrowFx = (ArrowFx_t)VA(m, 0x8FFAF0);
        g_castFx = (CastFx_t)VA(m, 0x810BA0);
        *(void**)&g_iceParam = VA(m, 0x75BDE0);
        g_arrowStatus = (ArrowStatus_t)VA(m, 0x8FFD70); g_gameFree = (GameFree_t)VA(m, 0xC8FCAC);
        logf("MageSkills: ice arrow no poison -> %s", patch_call(VA(m, 0x748B2E), VA(m, 0x8FFD70), (void*)ice_arrow_status) ? "on" : "OFF: bytes differ");
        logf("MageSkills: ice arrow cast effect -> %s", patch_call(VA(m, 0x749E15), VA(m, 0x810BA0), (void*)ice_cast_fx) ? "on" : "OFF: bytes differ");
        logf("MageSkills: ice arrow (skill %d) -> %s", g_iceSkill, patch_call(VA(m, 0x748AD8), VA(m, 0x8FFAF0), (void*)ice_arrow_fx) ? "on" : "effects OFF: bytes differ");
    }
    g_markSkill = GetPrivateProfileIntA("MageSkills", "MarkSkill", 0, g_ini);
    if (g_markSkill) { g_skillParam = (SkillParam_t)VA(m, 0x75BDE0); g_markHas = VA(m, 0x913100); logf("MageSkills: hunter's mark on (skill %d)", g_markSkill); }
    g_volleySkill = GetPrivateProfileIntA("MageSkills", "VolleySkill", 0, g_ini);
    if (g_volleySkill && g_iceSkill) {       /* shares the Poison Arrows hooks of Ice Arrow, installed above */
        if (same(m, 0x748520, "81ec48050000")) {
            g_shot = (Shot_t)VA(m, 0x748520);
            g_plainFly = (char**)VA(m, 0x14F1BE4); g_plainImpact = (char**)VA(m, 0x14F1BE8);
            static const DWORD sites[] = { 0x74A47B, 0x74A799, 0x74A8C9, 0x74AA8E, 0x74AD37 };
            int a = 0, b = 5;
            for (int k = 0; k < 5; k++) a += patch_call(VA(m, sites[k]), VA(m, 0x748520), (void*)volley_shot);
            logf("MageSkills: arrow volley (skill %d): shot sites %d of %d", g_volleySkill, a, b);
        } else { g_volleySkill = 0; logf("MageSkills: arrow volley OFF: bytes differ"); }
    }
    g_ramSkill = GetPrivateProfileIntA("MageSkills", "RamSkill", 0, g_ini);
    if (g_ramSkill) {
        *(void**)&g_iceParam = VA(m, 0x75BDE0);
        for (int i = 0; i < g_n; i++) if (g_ids[i] == g_ramSkill) g_ramI = i;
        if (g_ramI >= 0) {
            char key[24]; wsprintfA(key, "OwnKnock%d", g_ramSkill);
            g_knockOn[g_ramI] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        }
        logf("MageSkills: ram charge on (skill %d, fall %s)", g_ramSkill, g_ramI >= 0 && g_knockOn[g_ramI] ? "on" : "off");
    }
    g_breakSkill = GetPrivateProfileIntA("MageSkills", "BreakSkill", 0, g_ini);
    if (g_breakSkill) {
        GetPrivateProfileStringA("MageSkills", "BreakFx", "", g_breakFx, sizeof g_breakFx, g_ini);
        logf("MageSkills: armor breaker on (skill %d)", g_breakSkill);
    }
    g_aimSkill = GetPrivateProfileIntA("MageSkills", "AimSkill", 0, g_ini);
    if (g_aimSkill) logf("MageSkills: aimed shot on (skill %d)", g_aimSkill);
    g_critId = GetPrivateProfileIntA("MageSkills", "SpellCrit", 0, g_ini);
    if (g_critId) {   /* calls, no bytes written: 763AD0 and 75BDE0 as the game calls them at 0x768004, 0x7680B0 */
        g_critModule = m;
        g_hasSkill = (HasSkill_t)VA(m, 0x763AD0);
        g_skillParam = (SkillParam_t)VA(m, 0x75BDE0);
        logf("MageSkills: spell critical on (skill %d)", g_critId);
    }
    g_heroSkill = (HeroSkill_t)VA(m, 0x8ABE30);
    if (patch_call(VA(m, 0x9D0A97), VA(m, 0x8ABE30), (void*)row_skill) && patch_call(VA(m, 0x9D0A9E), VA(m, 0x89D490), (void*)row_cap))
        logf("MageSkills: trainer rows of skills the hero lacks no longer crash (0x9D0A97, 0x9D0A9E)");
    else logf("MageSkills: trainer row guard off: bytes at 0x9D0A97/0x9D0A9E differ");
    if (GetPrivateProfileIntA("MageSkills", "ShieldNoReaction", 1, g_ini)) {
        /* 73D530: sub esp,124h / mov eax,[14D10B0h]; the slot of the vtable */
        DWORD d0 = (DWORD)m.base - 0x400000;
        if (same(m, 0x73D530, "81ec24010000") && ((DWORD*)VA(m, 0x131FD2C))[38] == 0x73D530 + d0) g_hitStart = (HitStart_t)detour(VA(m, 0x73D530), (void*)hk_hitStart);
        logf("MageSkills: no hit reaction under the shield: %s", g_hitStart ? "on (0x73D530 detoured)" : "OFF: bytes at 0x73D530 differ");
    }
    g_meleeTrace = GetPrivateProfileIntA("MageSkills", "MeleeTrace", 0, g_ini);
    if (GetPrivateProfileIntA("MageSkills", "JumpThousandFix", 1, g_ini)) {
        /* 764330: mov eax,[esp+4] / push ebp / push edi / push eax / mov edi,ecx; 959C10: push esi / push edi / mov edi,ecx / call 8FD5A0 */
        BYTE* c = VA(m, 0x959C14);
        DWORD d0 = (DWORD)m.base - 0x400000;
        if (same(m, 0x764330, "8b4424045557508bf9") && same(m, 0x959C10, "56578bf9e8") && (DWORD)(c + 5 + *(int*)(c + 1)) == 0x8FD5A0 + d0) {
            g_actionOwner = (ActionOwner_t)VA(m, 0x8FD5A0);
            g_thousandStart = (ThousandStart_t)VA(m, 0x959C10);
            g_payReal = (Skill1b_t)detour(VA(m, 0x764330), (void*)hk_pay);
            int a1 = g_payReal ? patch_call(VA(m, 0x95C297), VA(m, 0x959C10), (void*)hk_thousand) : 0;
            int a2 = g_payReal ? patch_call(VA(m, 0x95C2B0), VA(m, 0x959C10), (void*)hk_thousand) : 0;
            logf("MageSkills: jump attack after Thousand Strikes: %s", a1 && a2 ? "fixed (0x95C297, 0x95C2B0, 0x764330)" : "NOT patched");
        } else logf("MageSkills: jump attack fix off: bytes at 0x764330/0x959C10 differ");
    }
    int any = 0;
    for (int i = 0; i < g_n; i++) {
        char key[32];
        wsprintfA(key, "OwnDrain%d", g_ids[i]);
        g_drainOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnCry%d", g_ids[i]);
        g_cryOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnBless%d", g_ids[i]);
        g_blessOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        if (g_blessOn[i]) { g_health = (Health_t)VA(m, 0x75EAD0); g_maxHealth = (MaxHealth_t)VA(m, 0x75BBD0); }
        wsprintfA(key, "OwnSmite%d", g_ids[i]);
        g_smiteOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnMark%d", g_ids[i]);
        g_markOn[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        wsprintfA(key, "OwnShield%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_shieldFx[i], sizeof g_shieldFx[i], g_ini);
        g_shieldOn[i] = g_shieldFx[i][0] != 0;
        any |= g_shieldOn[i];
    }
    if (!any) return;
    /* 76C800: sub esp,0Ch / push ebx / push esi / mov esi,ecx; 8FCB70: push ebx / mov ebx,[esp+8] / mov [ecx+10h],bl;
       A5C537: mov eax,[edx+68h] / call... */
    if (same(m, 0x76C800, "83ec0c53568bf1") && same(m, 0x8FCB70, "538b5c2408885910") && same(m, 0xA5C537, "8b4268")) {
        g_skin = (Skin_t)VA(m, 0x8FCB70);
        for (int k = 0; k < SKIN_KINDS; k++)                 /* push ebx / mov ebx,[esp+8] / mov [ecx+..],bl or the like */
            if (same(m, g_skinVa[k], "538b5c2408")) g_skins[k] = (Skin_t)VA(m, g_skinVa[k]);
        for (int i = 0; i < g_n; i++) if (g_shieldOn[i]) {
            char key[32];
            wsprintfA(key, "OwnShieldSkin%d", g_ids[i]);
            g_skinPick = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
            if (g_skinPick >= SKIN_KINDS) g_skinPick = 0;
            wsprintfA(key, "OwnShieldPulse%d", g_ids[i]);
            g_skinPulse = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        }
        g_takeHitReal = (TakeHit_t)detour(VA(m, 0x76C800), (void*)hk_takeHit);
    }
    /* 9830A0: push 10h / call 912710; 9830BA: call 9154E0; 9830E7: call 914660; 922440: call 914740; 982DA3: mov [esi+10h],0Fh; the slots */
    {
        BYTE* c1 = VA(m, 0x9830A2), *c2 = VA(m, 0x9830BA), *c3 = VA(m, 0x9830E7), *c4 = VA(m, 0x922440);
        DWORD d1 = (DWORD)m.base - 0x400000;
        DWORD* slot = (DWORD*)VA(m, 0x1335A28);
        if (g_takeHitReal && c1[0] == 0xE8 && (DWORD)(c1 + 5 + *(int*)(c1 + 1)) == 0x912710 + d1 && c2[0] == 0xE8 && (DWORD)(c2 + 5 + *(int*)(c2 + 1)) == 0x9154E0 + d1 &&
            c3[0] == 0xE8 && (DWORD)(c3 + 5 + *(int*)(c3 + 1)) == 0x914660 + d1 && c4[0] == 0xE8 && (DWORD)(c4 + 5 + *(int*)(c4 + 1)) == 0x914740 + d1 &&
            same(m, 0x982DA3, "c746100f000000") && slot[3] == 0x982E70 + d1 && slot[4] == 0x982F20 + d1 && slot[5] == 0x982FE0 + d1 && slot[18] == 0x9833E0 + d1) {
            DWORD a[3] = { (DWORD)icon_start, (DWORD)icon_update, (DWORD)icon_end };
            g_st15Start = (St15_t)slot[3];
            g_st15Update = (St15Up_t)slot[4];
            g_st15End = (St15_t)slot[5];
            g_statusChar = (StatusChar_t)VA(m, 0x9154C0);
            DWORD nameHook = (DWORD)buff_name;
            g_st15Name = (BuffName_t)slot[18];
            if (write_mem(slot + 18, &nameHook, 4) && write_mem(slot + 3, a, sizeof a)) {
                g_statusNew = (StatusNew_t)VA(m, 0x912710);
                g_statusTime = (StatusTime_t)VA(m, 0x9154E0);
                g_statusAdd = (StatusList_t)VA(m, 0x914660);
                g_statusOff = (StatusOff_t)VA(m, 0x914740);
                g_statusHasVa = VA(m, 0x913100);
                logf("MageSkills: shield shows the status icon 0xF (vtable 0x1335A28)");
                DWORD* s10 = (DWORD*)VA(m, 0x13465E0);
                if (s10[4] == 0xA5C5D0 + d1 && s10[18] == 0xA5C720 + d1) {
                    DWORD b[3] = { (DWORD)bless_start, (DWORD)bless_update, (DWORD)bless_end }, nm = (DWORD)bless_name;
                    g_st10Start = (St10_t)s10[3]; g_st10Update = (St10Up_t)s10[4]; g_st10End = (St10_t)s10[5]; g_st10Name = (St10Name_t)s10[18];
                    g_blessIcon = write_mem(s10 + 18, &nm, 4) && write_mem(s10 + 3, b, sizeof b);
                }
                logf("MageSkills: blessing icon %s", g_blessIcon ? "on (status 0x10, vtable 0x13465E0)" : "OFF: bytes differ");
            }
        } else logf("MageSkills: shield status icon off: bytes differ");
    }
    logf("MageSkills: shield %s", g_takeHitReal ? "on (0x76C800 detoured)" : "OFF: bytes at 0x76C800/0x8FCB70/0xA5C537 differ");
    if (!g_takeHitReal) for (int i = 0; i < g_n; i++) g_shieldOn[i] = 0;
}

/* 05.11 Holy Smite: OwnSmite<id>=1. Param1 = % of the damage healed on the caster, Param2 = damage, Param3 = metres. Only the enemy the player has targeted, in range: otherwise
   no cast, nothing paid. The hit lands OwnDelay ms after the cast. */
static int g_smiteLogged;
static struct { int on, hero, target, i; float pct, damage; DWORD due; } g_smite;

static void* chosen_enemy(void* stats, float metres)
{
    BYTE* ctrl = readable((BYTE*)stats + 0xFC, 4) ? *(BYTE**)((BYTE*)stats + 0xFC) : 0;
    int chosen = ctrl && readable(ctrl + 0x44, 4) ? *(int*)(ctrl + 0x44) : -1;
    if (chosen == -1) return 0;
    void* st = g_byHandle(*g_statsMgr, 0, &chosen);
    float* p = st && own_enemy(stats, st) ? g_pos(st, 0) : 0, *hp = g_pos(stats, 0);
    if (!p || !hp) return 0;
    float dx = p[0] - hp[0], dy = p[1] - hp[1], dz = p[2] - hp[2];
    return dx * dx + dy * dy + dz * dz < metres * metres ? st : 0;
}

static void smite_tick(DWORD now)
{
    if (!g_smite.on || (int)(now - g_smite.due) < 0) return;
    g_smite.on = 0;
    void* mgr = *g_statsMgr;
    int a = g_smite.hero, b = g_smite.target;
    void* hero = mgr ? g_byHandle(mgr, 0, &a) : 0, *st = mgr ? g_byHandle(mgr, 0, &b) : 0;
    float* hp = hero ? g_pos(hero, 0) : 0;
    if (!hero || !st || !hp || !own_enemy(hero, st)) return;
    float dealt = own_hit(hero, st, g_smite.damage, hp);
    float heal = dealt * g_smite.pct / 100.0f;
    if (heal > 0) (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)hero + 0x24))(hero, 0, heal);
    hit_effect(st, g_smite.i);
    if (g_smiteLogged++ < 30) logf("MageSkills: smite: target %X, damage %d, heals %d", b, (int)dealt, (int)heal);
}

/* 06.11 Blessing: OwnBless<id>=1. Param1 = % of max health healed per second, Param2 = mana per second, Param3 = seconds. Every 500 ms while it lasts, through vtable +24h and
   +28h (0x76CB5C, 0x751DB0); OwnHitFx<id> is renewed on the hero. Casting again starts it over. */
static struct { int on, hero, i; float hp, mana; DWORD until, next, nextFx; } g_bless;
static int g_blessLogged;

static void bless_tick(DWORD now)
{
    if (!g_bless.on || (int)(now - g_bless.next) < 0) return;
    g_bless.next = now + 500;
    void* mgr = *g_statsMgr;
    int h = g_bless.hero;
    void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (!st || (int)(now - g_bless.until) >= 0 || g_health(st, 0) <= 0) { g_bless.on = 0; return; }
    float top = g_maxHealth(st, 0, 0);
    (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)st + 0x24))(st, 0, top * g_bless.hp / 200.0f);
    (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)st + 0x28))(st, 0, g_bless.mana / 2.0f);
}

static void bless_status(void* stats, float secs)
{
    void* list = g_blessIcon ? status_list(stats) : 0;
    if (!list) return;
    g_statusOff(list, 0, 0x10);
    void* st = g_statusNew(0x10);
    if (!st) return;
    g_statusTime(st, 0, secs, 0);
    g_statusAdd(list, 0, st);
}

/* 06.11 Drain: OwnDrain<id>=1. Param1 = magic damage per second, Param2 = seconds, Param3 = % of the damage healed, Param4 = metres. Only the enemy the player has targeted, in range. One
   drain at a time, a new one replaces it; ends with the target. OwnHitFx<id> at every tick. */
static struct { int on, hero, target, i; float dps, pct; DWORD until, next; } g_drain;
static int g_drainLogged;

static void drain_tick(DWORD now)
{
    if (!g_drain.on || (int)(now - g_drain.next) < 0) return;
    g_drain.next = now + 1000;
    void* mgr = *g_statsMgr;
    int a = g_drain.hero, b = g_drain.target;
    void* hero = mgr ? g_byHandle(mgr, 0, &a) : 0, *st = mgr ? g_byHandle(mgr, 0, &b) : 0;
    float* hp = hero ? g_pos(hero, 0) : 0;
    if (!hero || !st || !hp || (int)(now - g_drain.until) >= 0 || !own_enemy(hero, st)) { g_drain.on = 0; return; }
    float dealt = own_hit(hero, st, g_drain.dps, hp);
    float heal = dealt * g_drain.pct / 100.0f;
    if (heal > 0) (*(void (__fastcall **)(void*, void*, float))(*(BYTE**)hero + 0x24))(hero, 0, heal);
    hit_effect(st, g_drain.i);
    if (g_drainLogged++ < 30) logf("MageSkills: drain: target %X, damage %d, heals %d", b, (int)dealt, (int)heal);
}

static void mark_tick(DWORD now)
{
    if (!g_mark.on) return;
    void* mgr = *g_statsMgr;
    int h = g_mark.handle;
    void* st = mgr ? g_byHandle(mgr, 0, &h) : 0;
    if (!st || (int)(now - g_mark.until) >= 0) { g_mark.on = 0; logf("MageSkills: mark over"); return; }
    /* 05.11: a free effect cannot follow (see the marks below), and a long one was left behind: so very short ones where the target is now */
    if ((int)(now - g_mark.nextFx) >= 0) { g_mark.nextFx = now + 40; status_puff(st, &g_ownHitFx[g_mark.i], g_ownHitFxName[g_mark.i], 90); }
}

static BYTE __fastcall own_cast(void* self, void* edx, int id)
{
    if (!own_skill(id)) {
        if (g_volleySkill && id == g_volleySkill) {
            void* who = g_ctrlStats(self, 0);
            if (who) {
                g_volleyHero = *(int*)((BYTE*)who + 0x144);
                g_volleyLeft = (int)g_iceParam(who, 0, id, 2) - 1;
                g_volleyPct = g_iceParam(who, 0, id, 3);
                g_volleyAction = 0;
                g_volleyHits = 0;
                g_volleyCast = GetTickCount() + 3000;
            }
        }
        if (g_iceSkill && id == g_iceSkill) g_iceShot = GetTickCount() + 3000;      /* the arrow's effects may be chosen within the cast */
        BYTE r = g_heroCast(self, edx, id);
        if (r && g_ramSkill && id == g_ramSkill) ram_cast(g_ctrlStats(self, 0));
        if (r && g_curseSkill && id == 0x1A) curse_cast(g_ctrlStats(self, 0));          /* the game's Curse */
        if (!r && g_iceSkill && id == g_iceSkill) g_iceShot = 0;
        if (!r && g_volleySkill && id == g_volleySkill) g_volleyCast = 0;
        if (r && g_iceSkill && id == g_iceSkill) {
            void* who = g_ctrlStats(self, 0);
            void* sk = who ? g_find(who, 0, id) : 0;
            g_iceGi = -1;
            for (int i = 0; i < g_n; i++) if (g_ids[i] == id) g_iceGi = i;
            if (sk && g_iceGi >= 0) {
                g_iceHero = *(int*)((BYTE*)who + 0x144);
                g_iceRank = g_rank(sk, 0);
                g_iceDamage = g_iceParam(who, 0, id, 2);
            }
        }
        if (r && g_markSkill && id == g_markSkill) {
            void* who = g_ctrlStats(self, 0);
            if (who) g_markPct = g_skillParam(who, 0, id, 4);
            if (g_markLogged < 30) { g_markLogged++; logf("MageSkills: mark cast: +%d%%", (int)g_markPct); }
        } else if (r && g_markSkill && id == 20) g_markPct = 0;
        return r;
    }
    void* stats = g_ctrlStats(self, 0);
    if (!stats) return 0;
    if (!g_canUse0(stats, 0, id)) return 0;            /* the game says why (out of mana, cooldown, form) */
    void* skill = g_find(stats, 0, id);
    if (!skill) return 0;
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id && g_drainOn[i]) {
        BYTE* pm = *g_protoMgr;
        BYTE** pb = pm && readable(pm + 0x10, 8) ? *(BYTE***)(pm + 0x10) : 0;
        BYTE* pr = pb && readable(pb + id, 4) ? pb[id] : 0;
        int rk = g_rank(skill, 0);
        void* tg = pr ? chosen_enemy(stats, g_param(pr, 0, rk, 3)) : 0;
        if (!tg) { if (g_drainLogged++ < 30) logf("MageSkills: drain: no target in range"); return 0; }
        g_drain.hero = *(int*)((BYTE*)stats + 0x144);
        g_drain.target = *(int*)((BYTE*)tg + 0x144);
        g_drain.i = i;
        g_drain.dps = g_param(pr, 0, rk, 0);
        g_drain.pct = g_param(pr, 0, rk, 2);
        g_drain.until = GetTickCount() + (DWORD)g_ownDelay[i] + (DWORD)(g_param(pr, 0, rk, 1) * 1000.0f) + 50;
        g_drain.next = GetTickCount() + (DWORD)g_ownDelay[i];
        g_drain.on = 1;
    }
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id && g_chainRange[i] > 0) {     /* 09.11 the user: Chain Lightning only at the chosen enemy; its reach as in chain_tick */
        float far = g_chainRange[i] > 30 ? (float)g_chainRange[i] : 30.0f;
        if (!chosen_enemy(stats, far)) {
            static int n; if (n++ < 30) logf("MageSkills: chain: no chosen enemy within %d m, no cast", (int)far);
            return 0;
        }
    }
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id && g_smiteOn[i]) {
        BYTE* pm = *g_protoMgr;
        BYTE** pb = pm && readable(pm + 0x10, 8) ? *(BYTE***)(pm + 0x10) : 0;
        BYTE* pr = pb && readable(pb + id, 4) ? pb[id] : 0;
        int rk = g_rank(skill, 0);
        void* tg = pr ? chosen_enemy(stats, g_param(pr, 0, rk, 2)) : 0;
        if (!tg) { if (g_smiteLogged++ < 30) logf("MageSkills: smite: no target in range"); return 0; }
        g_smite.hero = *(int*)((BYTE*)stats + 0x144);
        g_smite.target = *(int*)((BYTE*)tg + 0x144);
        g_smite.i = i;
        g_smite.pct = g_param(pr, 0, rk, 0);
        g_smite.damage = g_param(pr, 0, rk, 1);
        g_smite.due = GetTickCount() + (DWORD)g_ownDelay[i];
        g_smite.on = 1;
    }
    {
        int gm = -1;
        for (int i = 0; i < g_n; i++) if (g_ids[i] == id && g_markOn[i]) gm = i;
        if (gm >= 0) {
            BYTE* pm = *g_protoMgr;
            BYTE** pb = pm && readable(pm + 0x10, 8) ? *(BYTE***)(pm + 0x10) : 0;
            BYTE* pr = pb && readable(pb + id, 4) ? pb[id] : 0;
            int rk = g_rank(skill, 0);
            /* only the enemy the player has targeted ([controller+44h], -1 = none), within Param3: otherwise no cast, nothing paid (05.11) */
            void* tg = 0;
            BYTE* ctrl = readable((BYTE*)stats + 0xFC, 4) ? *(BYTE**)((BYTE*)stats + 0xFC) : 0;
            int chosen = ctrl && readable(ctrl + 0x44, 4) ? *(int*)(ctrl + 0x44) : -1;
            if (chosen != -1 && pr) {
                void* st = g_byHandle(*g_statsMgr, 0, &chosen);
                float* p = st && own_enemy(stats, st) ? g_pos(st, 0) : 0, *hp = g_pos(stats, 0);
                float far = g_param(pr, 0, rk, 2);
                if (p && hp && (p[0] - hp[0]) * (p[0] - hp[0]) + (p[1] - hp[1]) * (p[1] - hp[1]) + (p[2] - hp[2]) * (p[2] - hp[2]) < far * far) tg = st;
            }
            if (!tg) { if (g_markLogged < 30) { g_markLogged++; logf("MageSkills: mark: no target in range"); } return 0; }
            g_mark.handle = *(int*)((BYTE*)tg + 0x144);
            g_mark.i = gm;
            g_mark.pct = g_param(pr, 0, rk, 0);
            g_mark.until = GetTickCount() + (DWORD)(g_param(pr, 0, rk, 1) * 1000.0f);
            g_mark.nextFx = GetTickCount();
            g_mark.on = 1;
            logf("MageSkills: mark: target %X, +%d%% for %d ms", g_mark.handle, (int)g_mark.pct, (int)(g_mark.until - GetTickCount()));
        }
    }
    for (int i = 0; i < g_n; i++) if (g_ids[i] == id && g_boomOn[i]) {     /* 07.11 Summon Explosion: a summon or no cast */
        void* sm = nearest_summon(stats);
        if (!sm) { if (g_newLogged++ < 60) logf("MageSkills: summon explosion: no summon within 30 m"); return 0; }
        BYTE* pm = *g_protoMgr;
        BYTE** pb = pm && readable(pm + 0x10, 8) ? *(BYTE***)(pm + 0x10) : 0;
        BYTE* pr = pb && readable(pb + id, 4) ? pb[id] : 0;
        int rk = g_rank(skill, 0);
        g_boom.hero = *(int*)((BYTE*)stats + 0x144);
        g_boom.n = all_summons(stats, g_boom.list);
        g_boom.pos = 0;
        if (!g_boom.n) { g_boom.list[0] = *(int*)((BYTE*)sm + 0x144); g_boom.n = 1; }
        g_boom.i = i;
        g_boom.damage = pr ? g_param(pr, 0, rk, 1) : 0;
        g_boom.radius = pr ? g_param(pr, 0, rk, 2) : 5;
        g_boom.due = GetTickCount() + (DWORD)g_ownDelay[i];
        g_boom.on = 1;
    }
    { BYTE r; if (blink_cast(stats, skill, id, &r)) return r; }
    /* stop running first, as the game's cast does before its switch (0x9243E2..0x92442C): when the hero's current
       action [[stats+C4h]+0Ch] has type 14h, the movement component (type 14F2F44h) gets 0x739770(1) unless 0x739750
       says it already ends. 03.11: cast while running, the running took the hero back at once and the effect vanished */
    if (g_moveType) {
        BYTE* cont = readable((BYTE*)stats + 0xC4, 4) ? *(BYTE**)((BYTE*)stats + 0xC4) : 0;
        BYTE* cur = cont && readable(cont + 0xC, 4) ? *(BYTE**)(cont + 0xC) : 0;
        if (g_ownCasts < 50)
            logf("MageSkills: own skill %d: current action type %d", id, cur && readable(cur + 8, 4) ? *(int*)(cur + 8) : -1);
        if (cur && readable(cur + 8, 4) && *(int*)(cur + 8) == 0x14) {
            void* entry = g_compEntry(cont, 0, g_moveType);
            void* move = entry ? g_compCast(entry, 0, g_moveType) : 0;
            if (move && !g_moveEnds(move, 0)) g_moveStop(move, 0, 1);
            if (g_ownCasts < 50) logf("MageSkills: own skill %d cast while moving: movement %s", id, move ? "stopped" : "not found");
        }
    }
    float before = readable((BYTE*)stats + 0x190, 4) ? *(float*)((BYTE*)stats + 0x190) : -1;
    g_pay0(stats, 0, id);
    g_cooldown(skill, 0);
    if (g_ownCasts++ < 50) {
        float after = readable((BYTE*)stats + 0x190, 4) ? *(float*)((BYTE*)stats + 0x190) : -1;
        logf("MageSkills: own skill %d cast (rank %d): mana %d -> %d, cooldown %d ms", id,
             readable((BYTE*)skill + 0x10, 4) ? *(int*)((BYTE*)skill + 0x10) : -1, (int)before, (int)after,
             (int)(*(float*)((BYTE*)skill + 8) * 1000.0f));
    }
    if (g_pos && g_takeHit) {
        BYTE* mgr2 = *g_protoMgr;
        BYTE** begin = mgr2 && readable(mgr2 + 0x10, 8) ? *(BYTE***)(mgr2 + 0x10) : 0;
        BYTE* proto = begin && readable(begin + id, 4) ? begin[id] : 0;
        int rank = g_rank(skill, 0);
        float damage = proto ? g_param(proto, 0, rank, 1) : 0;     /* Param2 */
        float radius = proto ? g_param(proto, 0, rank, 2) : 0;     /* Param3, metres */
        int gi = -1;
        for (int i = 0; i < g_n; i++) if (g_ids[i] == id) gi = i;
        if (gi >= 0 && g_pushOn[gi]) g_pushMetres[gi] = proto ? g_param(proto, 0, rank, 0) : 0;   /* Param1 */
        if (gi >= 0 && g_drainOn[gi]) {
            /* set before paying, above */
        } else if (gi >= 0 && g_cryOn[gi]) {
            if (proto) {
                g_cry.hero = *(int*)((BYTE*)stats + 0x144);
                g_cry.less = g_param(proto, 0, rank, 0);
                g_cry.more = damage;
                cry_catch(stats, radius);
                g_cry.until = GetTickCount() + (DWORD)(g_param(proto, 0, rank, 3) * 1000.0f);
                g_cry.on = 1;
                if (g_cryLogged++ < 40) logf("MageSkills: battle cry: -%d%% / +%d%%, %d m, caught %d, female %d", (int)g_cry.less, (int)g_cry.more, (int)radius, g_cry.n, g_isFemale ? (int)g_isFemale(stats, 0) : -1);
            }
        } else if (gi >= 0 && g_blessOn[gi]) {
            g_bless.hero = *(int*)((BYTE*)stats + 0x144);
            g_bless.i = gi;
            g_bless.hp = proto ? g_param(proto, 0, rank, 0) : 0;
            g_bless.mana = damage;
            g_bless.until = GetTickCount() + (DWORD)(radius * 1000.0f);
            g_bless.next = g_bless.nextFx = GetTickCount();
            g_bless.on = 1;
            bless_status(stats, radius);
            if (g_blessLogged++ < 20) logf("MageSkills: blessing: %d%% health and %d mana per second for %d s", (int)g_bless.hp, (int)g_bless.mana, (int)radius);
        } else if (gi >= 0 && (g_markOn[gi] || g_smiteOn[gi])) {
            /* set before paying, above */
        } else if (gi >= 0 && g_shieldOn[gi]) {
            shield_start(stats, gi, proto ? g_param(proto, 0, rank, 0) : 0, damage, radius);
        } else if (gi >= 0 && g_chainStep[gi] > 0) {           /* Chain Lightning: Param1 leaps, Param2 damage, Param3 metres */
            int jumps = proto ? (int)(g_param(proto, 0, rank, 0) + 0.5f) : 0;
            chain_start(stats, gi, damage, jumps, radius, (DWORD)g_ownDelay[gi]);
            if (g_ownCasts < 50) logf("MageSkills: own skill %d rank %d: chain of %d leap(s), damage %d, %d m", id, rank,
                                      jumps, (int)damage, (int)radius);
        } else if (gi >= 0 && g_boomOn[gi]) {
            /* set before paying, above; boom_tick */
        } else if (gi >= 0 && g_ownDelay[gi] > 0) {           /* the hit lands on the slam of the animation (own_tick) */
            int k = 0;
            while (k < 8 && g_pend[k].state) k++;
            if (k < 8) {
                g_pend[k].hero = *(int*)((BYTE*)stats + 0x144);
                g_pend[k].i = gi; g_pend[k].id = id; g_pend[k].damage = damage; g_pend[k].radius = radius;
                g_pend[k].due = GetTickCount() + (DWORD)g_ownDelay[gi];
                g_pend[k].state = 1;
            } else logf("MageSkills: own skill %d: too many pending hits -> this one is lost", id);
        } else {
            int n = own_targets(stats, radius, damage, g_ownCasts < 50, gi);
            if (g_ownCasts < 50) logf("MageSkills: own skill %d rank %d: damage %d, radius %d m -> %d target(s)", id, rank,
                                      (int)damage, (int)radius, n);
        }
    }
    for (int i = 0; i < g_n; i++)
        if (g_ids[i] == id) {
            if (g_gameSoundF[i][0] && g_isFemale(stats, 0)) game_sound(g_gameSoundF[i], stats);
            else if (g_ownSound[i] && g_playSound) g_playSound(g_ownSoundF[i] && g_isFemale(stats, 0) ? g_ownSoundF[i] : g_ownSound[i], 0, 0x0004 | 0x0001 | 0x0002);   /* MEMORY|ASYNC|NODEFAULT */
            if (g_castSound[i][0]) game_sound(g_castSound[i], stats);
            own_effect(stats, i, g_rank ? g_rank(skill, 0) : 1);
            if (g_ownHold[i] > 0) {
                g_holdHero = *(int*)((BYTE*)stats + 0x144);
                g_holdFrom = GetTickCount();
                g_holdUntil = g_holdFrom + (DWORD)g_ownHold[i];
            }
        }
    return 1;
}

/* 03.11: OwnTrail<id>=effect[,cm] - the frost trail of a Fireball-based projectile. The bolt's own particles die with
   it at the hit (the projectile and its effect go in the same frame), so a free effect (world_fx) is started where
   the projectile is every <cm> of its way (50 by default) while it flies; each puff lives on and melts away by itself.
   The projectile: 0x969D1A call 0x7D37C0 thiscall(pool [0x14F1714], &desc) ret 4 -> the projectile object; its handle
   [obj+1BACh] (7D3809), set to -1 when it goes (8EFFCE); the pool's table [pool+34h]..[pool+38h] (the lookup 818370
   on pool+4). The position: TODO, not confirmed - the candidates below are logged (TrailTrace=1) and OwnTrailAt<id>
   (offset, 0xB4 by default: 8EF649 copies [obj+98h] there while it flies) is read. */
#define MAX_TRAIL 8
typedef void* (__fastcall *Launch_t)(void* pool, void* edx, void* desc);
static Launch_t g_launch;
static void**   g_projPool;
static char     g_trailName[MAX_GRANT][48];
static char*    g_trailFixed[MAX_GRANT];
static int      g_trailMs[MAX_GRANT], g_trailAt[MAX_GRANT], g_trailTrace, g_trailLogged;
static struct { BYTE* obj; int handle, i, n, has; DWORD next, born; float last[3]; } g_trail[MAX_TRAIL];

static int proj_alive(BYTE* obj, int handle)
{
    BYTE* pool = (BYTE*)*g_projPool;
    if (!pool || handle < 0 || !readable(pool + 0x34, 8)) return 0;
    BYTE** lo = *(BYTE***)(pool + 0x34), **hi = *(BYTE***)(pool + 0x38);
    if (!lo || hi <= lo || handle >= hi - lo || lo[handle] != obj) return 0;
    return readable(obj + 0x1BAC, 4) && *(int*)(obj + 0x1BAC) == handle;
}

static void* __fastcall hk_launch(void* pool, void* edx, void* desc)
{
    BYTE* obj = (BYTE*)g_launch(pool, edx, desc);
    for (int i = 0; i < g_n; i++) {
        if (g_ids[i] != g_fxId || !g_trailName[i][0] || !obj || !readable(obj + 0x1BAC, 4)) continue;
        int k = 0;
        while (k < MAX_TRAIL && g_trail[k].obj) k++;
        if (k == MAX_TRAIL) break;
        g_trail[k].obj = obj; g_trail[k].handle = *(int*)(obj + 0x1BAC); g_trail[k].i = i; g_trail[k].n = 0; g_trail[k].has = 0;
        g_trail[k].born = g_trail[k].next = GetTickCount();
        if (g_trailLogged++ < 4) logf("MageSkills: skill %d projectile %p (handle %d) leaves a trail '%s'", g_fxId, obj,
                                      g_trail[k].handle, g_trailName[i]);
        break;
    }
    return obj;
}

static void trail_tick(DWORD now)
{
    /* TrailTrace=1: the live effect instances every 2 s (the table [14F1430]: [+38h] slots, [+3Eh] word count, as
       0x8103B0 reads it), to see whether the puffs go away (03.11: the fps fell after many casts) */
    static DWORD countAt;
    if (g_trailTrace && g_fxTable && now - countAt > 2000) {
        countAt = now;
        BYTE* t = *g_fxTable;
        if (t && readable(t + 0x38, 8)) {
            int n = *(WORD*)(t + 0x3E), live = 0;
            void** slot = *(void***)(t + 0x38);
            if (slot && readable(slot, n * 4)) for (int j = 0; j < n; j++) live += slot[j] != 0;
            logf("MageSkills: effects alive: %d of %d slots, %d to release%s", live, n, g_expireN, g_expireLost ? " (queue was full)" : "");
        }
    }
    for (int k = 0; k < MAX_TRAIL; k++) {
        if (!g_trail[k].obj) continue;
        BYTE* o = g_trail[k].obj;
        int i = g_trail[k].i;
        if (!proj_alive(o, g_trail[k].handle) || now - g_trail[k].born > 15000) {
            if (g_trailTrace) logf("MageSkills: trail: projectile %p gone after %u ms, %d puffs", o, now - g_trail[k].born, g_trail[k].n);
            g_trail[k].obj = 0;
            continue;
        }
        if (g_trailTrace && g_trail[k].n < 12 && (int)(now - g_trail[k].next) >= 0) {
            g_trail[k].next = now + 40;
            float* a = (float*)(o + 0x20), *b = (float*)(o + 0x98), *c = (float*)(o + 0xB4), *e = (float*)(o + 0x1BA0),
                 *f = (float*)(o + 0x1BB0);
            logf("MageSkills: trail %u ms: +20 %d %d %d | +98 %d %d %d | +B4 %d %d %d | +1BA0 %d %d %d | +1BB0 %d %d %d (cm)",
                 now - g_trail[k].born, (int)(a[0] * 100), (int)(a[1] * 100), (int)(a[2] * 100), (int)(b[0] * 100),
                 (int)(b[1] * 100), (int)(b[2] * 100), (int)(c[0] * 100), (int)(c[1] * 100), (int)(c[2] * 100),
                 (int)(e[0] * 100), (int)(e[1] * 100), (int)(e[2] * 100), (int)(f[0] * 100), (int)(f[1] * 100), (int)(f[2] * 100));
        }
        /* 03.11 (confirmed in game: +B4h moves with the flight, metres): the bolt flies ~2.3 m per 47 ms, so a puff
           every frame left dots; puffs go every <spacing> cm along the way from the last one, up to 8 per frame */
        float* at = (float*)(o + g_trailAt[i]);
        if (at[0] != at[0] || at[0] > 1e6f || at[0] < -1e6f) continue;          /* not a position */
        Mat33 one = {{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
        float step = g_trailMs[i] * 0.01f;
        if (!g_trail[k].has) {
            g_trail[k].last[0] = at[0]; g_trail[k].last[1] = at[1]; g_trail[k].last[2] = at[2]; g_trail[k].has = 1;
            if (world_fx(&g_trailFixed[i], g_trailName[i], at, &one, 950)) g_trail[k].n++;
            continue;
        }
        float dx = at[0] - g_trail[k].last[0], dy = at[1] - g_trail[k].last[1], dz = at[2] - g_trail[k].last[2];
        float len = own_sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 50.0f) { g_trail[k].last[0] = at[0]; g_trail[k].last[1] = at[1]; g_trail[k].last[2] = at[2]; continue; }
        for (int s = 0; s < 8 && len >= step; s++) {
            float f = step / len;
            g_trail[k].last[0] += dx * f; g_trail[k].last[1] += dy * f; g_trail[k].last[2] += dz * f;
            dx -= dx * f; dy -= dy * f; dz -= dz * f; len -= step;
            if (world_fx(&g_trailFixed[i], g_trailName[i], g_trail[k].last, &one, 950)) g_trail[k].n++;
        }
    }
}

static void trail_init(Module m)
{
    DWORD d = (DWORD)m.base - 0x400000;
    int any = 0;
    for (int i = 0; i < g_n; i++) {
        char key[32], v[64];
        wsprintfA(key, "OwnTrail%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", v, sizeof v, g_ini);
        char* c = v;
        int n = 0;
        while (*c && *c != ',' && n < 47) g_trailName[i][n++] = *c++;
        g_trailName[i][n] = 0;
        g_trailMs[i] = 50;                 /* the spacing of the puffs, cm (03.11: was ms) */
        if (*c == ',') {
            int cm = 0;
            c++;
            while (*c >= '0' && *c <= '9') cm = cm * 10 + *c++ - '0';
            if (cm >= 15) g_trailMs[i] = cm;
        }
        wsprintfA(key, "OwnTrailAt%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "0xB4", v, sizeof v, g_ini);
        g_trailAt[i] = 0xB4;
        int x = 0;
        c = v;
        if (c[0] == '0' && (c[1] | 32) == 'x') c += 2;
        for (;; c++) {
            int h = (*c >= '0' && *c <= '9') ? *c - '0' : ((*c | 32) >= 'a' && (*c | 32) <= 'f') ? (*c | 32) - 'a' + 10 : -1;
            if (h < 0) break;
            x = x * 16 + h;
        }
        if (x > 0 && x <= 0x1BC0 && !(x & 3)) g_trailAt[i] = x;
        any |= g_trailName[i][0] != 0;
        wsprintfA(key, "SlowFx%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_slowFxName[i], sizeof g_slowFxName[i], g_ini);
        wsprintfA(key, "FreezeFx%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_freezeFxName[i], sizeof g_freezeFxName[i], g_ini);
        if (g_slowFxName[i][0] || g_freezeFxName[i][0])
            logf("MageSkills: skill %d marks a slowed enemy '%s', a frozen one '%s'%s", g_ids[i], g_slowFxName[i],
                 g_freezeFxName[i], g_fxCreate ? "" : " - no effect manager: off");
    }
    if (!any) return;
    g_trailTrace = GetPrivateProfileIntA("MageSkills", "TrailTrace", 0, g_ini);
    /* 969D0C: lea ecx,[esp+0F0h] / push ecx / mov ecx,[14F1714] / call 7D37C0; 7D37C0: push ecx / push esi / lea esi,[ecx+4] / push 0
       7D3809: mov [edi+1BACh],edx; 8EFFCE: mov dword [esi+1BACh],-1 */
    BYTE* c = VA(m, 0x969D1A);
    int ok = g_fxCreate && g_pump && same(m, 0x969D0C, "8d8c24f000000051") && same(m, 0x969D14, "8b0d") &&
             *(DWORD*)VA(m, 0x969D16) == 0x14F1714 + d && c[0] == 0xE8 && (DWORD)(c + 5 + *(int*)(c + 1)) == 0x7D37C0 + d &&
             same(m, 0x7D37C0, "51568d71046a00") && same(m, 0x7D3809, "8997ac1b0000") &&
             same(m, 0x8EFFCE, "c786ac1b0000ffffffff");
    if (!ok) {
        logf("MageSkills: trail off: %s", !g_fxCreate ? "no effect manager (needs an OwnChain skill's checks)" :
             !g_pump ? "no message loop hook" : "bytes at 0x969D0C/0x7D37C0/0x7D3809/0x8EFFCE differ");
        for (int i = 0; i < g_n; i++) g_trailName[i][0] = 0;
        return;
    }
    g_launch = (Launch_t)VA(m, 0x7D37C0);
    g_projPool = (void**)VA(m, 0x14F1714);
    if (!patch_call(c, VA(m, 0x7D37C0), (void*)hk_launch)) {
        logf("MageSkills: trail off: call site 0x969D1A not patched");
        for (int i = 0; i < g_n; i++) g_trailName[i][0] = 0;
        return;
    }
    for (int i = 0; i < g_n; i++)
        if (g_trailName[i][0])
            logf("MageSkills: skill %d projectile leaves '%s' every %d cm of its way (position at +%Xh, confirmed 03.11)", g_ids[i],
                 g_trailName[i], g_trailMs[i], g_trailAt[i]);
}

/* 09.11.2026: the death was only noticed right after a hit passing through 0x76C800. A cursed enemy killed in another way, or
   whose death flag is raised later, never passed the curse on: the tracked enemies are now also checked 5 times a second. */
static void curse_tick(DWORD now)
{
    static DWORD next; static int goneLogged;
    if (!g_curseSkill || !g_dead || (int)(now - next) < 0) return;
    next = now + 200;
    void* hero = summon_hero();
    if (!hero || !g_hasSkill(hero, 0, g_curseSkill)) return;
    for (int k = 0; k < CURSE_MAX; k++) {
        if (!g_cursed[k].until || (int)(now - g_cursed[k].until) >= 0) continue;
        int h = g_cursed[k].h;
        void* st = g_byHandle(*g_statsMgr, 0, &h);
        if (!st) { g_cursed[k].until = 0; if (goneLogged++ < 10) logf("MageSkills: contagious curse: %X left the world before its death was seen", h); continue; }
        if (!g_dead(st, 0)) continue;
        curse_spread(hero, st, k);
        g_cursed[k].until = 0;
    }
}

static void own_tick(void)
{
    DWORD now = GetTickCount();
    curse_tick(now);
    chain_tick(now);
    status_tick(now);
    push_tick(now);
    skull_tick(now);
    boom_tick(now);
    mark_tick(now);
    smite_tick(now);
    bless_tick(now);
    drain_tick(now);
    volley_tick(now);
    if (g_meleeTrace) trace_tick(now);
    shield_tick(now);
    expire_tick(now);
    trail_tick(now);
    if (g_lockUntil && (int)(now - g_lockUntil) >= 0) {
        g_lockUntil = 0;
        void* mgr = *g_statsMgr;
        void* hero = mgr ? g_byHandle(mgr, 0, &g_lockHero) : 0;
        void* container;
        BYTE* comp = hero ? (BYTE*)fx_component(hero, &container) : 0;
        if (comp && readable(comp + 0xC, 1)) comp[0xC] = 0;
    }
    if (g_holdUntil) {
        void* mgr = *g_statsMgr;
        void* hero = mgr ? g_byHandle(mgr, 0, &g_holdHero) : 0;
        void* container;
        BYTE* comp = hero ? (BYTE*)fx_component(hero, &container) : 0;
        int left = (int)(g_holdUntil - now);
        if (!comp || left <= 0) g_holdUntil = 0;
        else if (readable(comp + 0x18, 4)) {
            float* timer = (float*)(comp + 0x18);
            float want = left / 1000.0f;
            if (g_holdLogged < 40 && ((now - g_holdFrom) / 100) != ((now - g_holdFrom - 16) / 100)) {
                g_holdLogged++;
                logf("MageSkills: hold +%d ms: component timer %d ms, keep %d ms", (int)(now - g_holdFrom),
                     (int)(*timer * 1000), left);
            }
            if (*timer < want) *timer = want;
        }
    }
    for (int k = 0; k < 8; k++) {
        if (!g_pend[k].state || (int)(now - g_pend[k].due) < 0) continue;
        g_pend[k].state = 0;
        void* mgr = *g_statsMgr;
        void* hero = mgr ? g_byHandle(mgr, 0, &g_pend[k].hero) : 0;
        if (!hero) continue;                            /* gone (area change, death) */
        if (hero && g_pushFx[g_pend[k].i][0] && g_pos(hero, 0)) {      /* the wave, a free effect at the feet */
            Mat33 one = {{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
            world_fx(&g_pushFxFixed[g_pend[k].i], g_pushFx[g_pend[k].i], g_pos(hero, 0), &one, 950);
        }
        int n = own_targets(hero, g_pend[k].radius, g_pend[k].damage, g_ownCasts < 50, g_pend[k].i);
        if (g_ownCasts < 50) logf("MageSkills: own skill %d hits after %d ms: damage %d, radius %d m -> %d target(s)",
                                  g_pend[k].id, g_ownDelay[g_pend[k].i], (int)g_pend[k].damage, (int)g_pend[k].radius, n);
    }
}

/* a whole file next to the game (bin) into memory, or 0 */
static BYTE* load_file(const char* rel)
{
    char path[MAX_PATH];
    wsprintfA(path, "%s\\%s", g_dir, rel);
    HANDLE f = CreateFileA(path, 0x80000000, 1, 0, 3, 0x80, 0);           /* GENERIC_READ, share read, OPEN_EXISTING */
    if (f == INVALID_HANDLE_VALUE) { logf("MageSkills: %s not found", path); return 0; }
    DWORD size = GetFileSize(f, 0), got = 0;
    BYTE* buf = size && size < (64u << 20) ? (BYTE*)VirtualAlloc(0, size, 0x3000, 4) : 0;
    if (buf && (!ReadFile(f, buf, size, &got, 0) || got != size)) buf = 0;
    CloseHandle(f);
    return buf;
}

/* ---- SummonLimit=1: more summoned creatures with Summon Mastery (03.11) ----
   0x8CC210 thiscall(handle, duration) adds a summon to the owner's list (vector at [ctrl+48h], 8-byte entries:
   handle, time left) and, when the list holds more than one (0x8CC288: cmp ecx,1 / jbe 8CC2B1), sets the time
   of the oldest to 0: only one summon lives. Summon Mastery's Param2 ("up to [2] creatures") is read nowhere
   (its only reader, 0x960C69, takes Param1). The cmp becomes a jump to summon_stub: the limit is Param2 of the
   owner's Summon Mastery rank (skill 0x2E), 1 without it. The owner's stats as 0x8CC238..0x8CC262 find them:
   0x8C9330([ctrl+20h]) (stdcall, ret 4: 0x818330([14F1758], &handle); 0x8C92C0 called before it takes no argument,
   the push is 0x8C9330's), index [obj+4] & 0xFFF, 0x818370(stats manager, &index). 03.11: the first build called
   them as 0x8C9330(0x8C92C0(handle)) with the wrong conventions: the stack moved by 4 and the game crashed. */
#define ID_SUMMON_MASTERY 0x2E
typedef void* (__fastcall *ByIndex_t)(void* mgr, void* edx, int* index);     /* 0x818370 */
static ByIndex_t g_byIndex;
static BYTE*     g_sumBack;                                                  /* 0x8CC28D: the oldest is expired */
static BYTE*     g_sumSkip;                                                  /* 0x8CC2B1: within the limit */
static int       g_sumLogged;
static int       g_sumCooldown, g_sumCdLogged;                              /* [MageSkills] SummonCooldown */

/* Param2 of the Summon Mastery rank of these stats (1 without the skill), at most 6 */
static int mastery_limit(void* stats)
{
    void* skill = stats ? g_find(stats, 0, ID_SUMMON_MASTERY) : 0;
    BYTE* pm = *g_protoMgr;
    BYTE** begin = pm && readable(pm + 0x10, 8) ? *(BYTE***)(pm + 0x10) : 0;
    BYTE* proto = begin && readable(begin + ID_SUMMON_MASTERY, 4) ? begin[ID_SUMMON_MASTERY] : 0;
    int rank = skill ? g_rank(skill, 0) : 0;
    int n = proto && rank > 0 ? (int)(g_param(proto, 0, rank, 1) + 0.5f) : 1;
    return n < 1 ? 1 : n > 6 ? 6 : n;
}

static int __stdcall summon_limit(BYTE* ctrl, int count)
{
    int limit = 1;
    void* obj = ctrl && readable(ctrl + 0x20, 4) ? g_obj(*(int*)(ctrl + 0x20)) : 0;
    void* mgr = *g_statsMgr;
    if (obj && mgr && readable((BYTE*)obj + 4, 4)) {
        int index = *(int*)((BYTE*)obj + 4) & 0xFFF;
        void* owner = g_byIndex(mgr, 0, &index);
        limit = mastery_limit(owner);
        /* 03.11: the summon action never starts its skill's cooldown (0x960A30 has no 0x89EC50; the game relied on the
           recast dismissing the creature). With SummonRecast the skill could be cast again at once, so start it here:
           the newest entry (0x8D0B90 appends) is the new creature, [creature+1C8h] = its summon skill (0x960BE6) */
        DWORD* e = g_sumCooldown && readable(ctrl + 0x54, 8) ? *(DWORD**)(ctrl + 0x58) : 0;
        int h = e && readable(e - 2, 8) ? (int)e[-2] : 0;
        BYTE* st = h ? (BYTE*)g_byHandle(mgr, 0, &h) : 0;
        int id = st && readable(st + 0x1C8, 4) ? *(int*)(st + 0x1C8) : 0;
        void* skill = owner && id > 0 && id < 0x100 ? g_find(owner, 0, id) : 0;
        if (skill) g_cooldown(skill, 0);
        if (g_sumCdLogged < 20 && e) { g_sumCdLogged++;
            logf("MageSkills: summon %d of skill %d -> %s", h, id, skill ? "cooldown started" : "no skill, no cooldown"); }
    }
    if (g_sumLogged < 40) {
        g_sumLogged++;
        logf("MageSkills: summon list holds %d, limit %d -> %s", count, limit, count > limit ? "oldest dismissed" : "all stay");
        { AddrSpace as; addr_space(&as);
          logf("MageSkills:   memory %u MB used, %u MB free (largest block %u MB), %u MB private", as.usedMB, as.freeMB, as.largestMB, as.privMB); }
        /* 03.11 trace: every entry (handle, time left in ms, does the handle still resolve to a character, active) */
        DWORD* b = ctrl && readable(ctrl + 0x54, 8) ? *(DWORD**)(ctrl + 0x54) : 0;
        void* mgr2 = *g_statsMgr;
        for (int i = 0; b && i < count && i < 16 && readable(b + 2 * i, 8); i++) {
            int h = (int)b[2 * i];
            int ms = (int)(*(float*)(b + 2 * i + 1) * 1000.0f);
            void* st = mgr2 ? g_byHandle(mgr2, 0, &h) : 0;
            logf("MageSkills:   summon %d: handle %d, %d ms left, %s%s", i, h, ms, st ? "exists" : "GONE",
                 st && g_active ? (g_active(st, 0) ? ", active" : ", inactive") : "");
        }
    }
    return limit;
}

/* replaces "cmp ecx,1 / jbe 8CC2B1" at 0x8CC288: ecx = count, edi = the owner's controller; eax/edx are free there */
static __declspec(naked) void summon_stub(void)
{
    __asm {
        push ecx
        push ecx
        push edi
        call summon_limit
        pop ecx
        cmp ecx, eax
        jbe skip
        jmp dword ptr [g_sumBack]
    skip:
        jmp dword ptr [g_sumSkip]
    }
}

/* The recast toggle: the summon action's start 0x960950 asks 0x8CB380 for the owner's summon and, when that one came
   from the same skill ([summon+1C8h] == [comp+28h]), sets [comp+24h] = 1: the action then dismisses every summon
   (0x955370 -> 0x8CB300) instead of summoning (03.11: a second demon took the first away). With Summon Mastery
   allowing more than one, the toggle stays off; the list limit above drops the oldest when there are too many. */
static BYTE* g_togBack;                                                      /* 0x9609BE */
static int   g_togLogged;

static int __stdcall toggle_allowed(void* owner)
{
    int limit = mastery_limit(owner);
    /* the owner's summon list (0x8CC210 is called on [stats+0FCh]): [ctrl+54h]..[ctrl+58h], 8 bytes per summon.
       03.11: a 4th summon at limit 3 made the list drop the oldest, and dropping one of two creatures from the same
       summon action (its one slot was overwritten) crashed (null+524h at 0x95AC09): at the limit the recast is the
       game's toggle again */
    BYTE* ctrl = owner && readable((BYTE*)owner + 0xFC, 4) ? *(BYTE**)((BYTE*)owner + 0xFC) : 0;
    int count = ctrl && readable(ctrl + 0x54, 8) ? (int)((*(DWORD*)(ctrl + 0x58) - *(DWORD*)(ctrl + 0x54)) >> 3) : 99;
    int another = limit > 1 && count < limit;
    if (g_togLogged < 10) { g_togLogged++; logf("MageSkills: recast of a summon: %d of %d -> %s", count, limit, another ? "summon another" : "dismiss (the game's toggle)"); }
    return !another;
}

/* replaces 0x9609AF..0x9609BD (mov edx,[eax+1C8h] / cmp edx,[esi+28h] / jne 9609BE / mov byte [esi+24h],1) and
   returns to 0x9609BE (mov ecx,esi): esi = the action, edi = the owner's stats, eax = the owner's summon.
   03.11: the first version wrote 6 bytes at 0x9609BA, and the game's jne 0x9609BE (a summon of another kind) landed
   inside that jmp: ecx stayed garbage and 0x95AC09 crashed. No jump of the game targets 0x9609B0..0x9609BD. */
static __declspec(naked) void toggle_stub(void)
{
    __asm {
        mov edx, [eax + 1C8h]
        cmp edx, [esi + 28h]
        jne done
        push edi
        call toggle_allowed
        test eax, eax
        je done
        mov byte ptr [esi + 24h], 1
    done:
        jmp dword ptr [g_togBack]
    }
}

static void summon_init(Module m)
{
    if (!GetPrivateProfileIntA("MageSkills", "SummonLimit", 0, g_ini)) return;
    DWORD d = (DWORD)m.base - 0x400000;
    /* 8CC27F: mov ecx,[esi+10h] / sub ecx,[esi+0Ch] / sar ecx,3 / cmp ecx,1 / jbe +24h; 8CC238 mov ecx,[edi+20h];
       8C92C0/8C9330 as called at 8CC23E/8CC245; 818370 as at 8CC262 with [14F13C4] */
    BYTE* at = VA(m, 0x8CC288);
    BYTE* c1 = VA(m, 0x8CC23E), *c2 = VA(m, 0x8CC245), *c3 = VA(m, 0x8CC262);
    if (!(same(m, 0x8CC27F, "8b4e102b4e0cc1f903") && same(m, 0x8CC288, "83f9017624") && same(m, 0x8CC238, "8b4f20") &&
          c1[0] == 0xE8 && (DWORD)(c1 + 5 + *(int*)(c1 + 1)) == 0x8C92C0 + d &&
          c2[0] == 0xE8 && (DWORD)(c2 + 5 + *(int*)(c2 + 1)) == 0x8C9330 + d &&
          c3[0] == 0xE8 && (DWORD)(c3 + 5 + *(int*)(c3 + 1)) == 0x818370 + d &&
          g_find && g_rank && g_param && g_protoMgr && g_statsMgr)) {
        logf("MageSkills: SummonLimit: summon list code differs (or skill code missing) -> off");
        return;
    }
    g_obj = (Obj_t)VA(m, 0x8C9330);
    g_byIndex = (ByIndex_t)VA(m, 0x818370);
    g_sumBack = VA(m, 0x8CC28D);
    g_sumSkip = VA(m, 0x8CC2B1);
    BYTE code[5] = { 0xE9 };
    long rel = (long)((BYTE*)summon_stub - (at + 5));
    memcpy(code + 1, &rel, 4);
    logf("MageSkills: SummonLimit: Summon Mastery Param2 summons at once (0x8CC288) -> %s",
         write_mem(at, code, 5) ? "on" : "write failed");
    /* 960955: mov esi,ecx / call 8FD5A0 / mov edi,eax (owner stats); 9609AF: mov edx,[eax+1C8h] / cmp edx,[esi+28h] /
       jne 9609BE; 9609BA: mov byte [esi+24h],1 / mov ecx,esi; 9609C0 call 95AC00 */
    BYTE* tg = VA(m, 0x9609AF);
    /* 03.11: off unless SummonRecast=1. Two creatures from the same summon action (one creature slot [comp+18h])
       crashed the game (stack overflow in the allocator, after a few summons); creatures of different summon skills
       live together fine with the list limit above */
    g_cooldown = (Cooldown_t)VA(m, 0x89EC50);
    g_sumCooldown = GetPrivateProfileIntA("MageSkills", "SummonCooldown", 1, g_ini) && same(m, 0x89EC50, "51538bd9578b7b04");
    logf("MageSkills: SummonLimit: a summon starts its skill's cooldown -> %s", g_sumCooldown ? "on" : "off");
    if (!GetPrivateProfileIntA("MageSkills", "SummonRecast", 0, g_ini)) {
        logf("MageSkills: SummonLimit: a recast of the same summon dismisses, as in the game (SummonRecast=0)");
        return;
    }
    if (same(m, 0x960955, "8bf1e8") && same(m, 0x96095C, "8bf8") && same(m, 0x9609AF, "8b90c8010000") &&
        same(m, 0x9609B5, "3b56287504") && same(m, 0x9609BA, "c64624018bce") && same(m, 0x9609BE, "8bcee8")) {
        g_togBack = VA(m, 0x9609BE);
        BYTE t[15] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        long r2 = (long)((BYTE*)toggle_stub - (tg + 5));
        memcpy(t + 1, &r2, 4);
        logf("MageSkills: SummonLimit: recasting a summon adds another while Mastery allows (0x9609AF) -> %s",
             write_mem(tg, t, 15) ? "on" : "write failed");
    } else logf("MageSkills: SummonLimit: summon toggle code differs -> a recast still dismisses");
}

/* 03.11: CastWav<id> = a WAV next to the game played instead of the Fireball's cast sound for a skill on Fireball code
   (Ice Bolt sounded like a fireball). 0x95E6E0 (the cast flash, ebp = the cast object, then ebp += 28h at 0x95E87C)
   posts "Skill_Fireball_Cast" on the caster: 0x95E8B6 push 1332814h / 0x95E8BD call 752E20 / 0x95E8C4 call 8F5910
   (thiscall(sound component, event, 0) ret 8); there the skill id is [ebp+4] (= cast object + 2Ch). */
static BYTE*  g_castWav[MAX_GRANT];
static void*  g_castSoundFn;              /* 0x8F5910 */
static int    g_castWavLogged;

static int __cdecl cast_wav(int id)
{
    for (int i = 0; i < g_n; i++)
        if (g_ids[i] == id && g_castWav[i] && g_playSound) {
            g_playSound(g_castWav[i], 0, 0x0004 | 0x0001 | 0x0002);       /* MEMORY|ASYNC|NODEFAULT */
            if (g_castWavLogged++ < 5) logf("MageSkills: skill %d cast sound: own WAV instead of Skill_Fireball_Cast", id);
            return 1;
        }
    return 0;
}

static __declspec(naked) void cast_sound_stub(void)
{
    __asm {
        push ecx
        push dword ptr [ebp + 4]
        call cast_wav
        add esp, 4
        pop ecx
        test eax, eax
        jz game
        mov al, 1
        ret 8
    game:
        jmp dword ptr [g_castSoundFn]
    }
}

static void cast_wav_init(Module m)
{
    int any = 0;
    for (int i = 0; i < g_n; i++) {
        char key[16], wav[MAX_PATH];
        wsprintfA(key, "CastWav%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", wav, sizeof wav, g_ini);
        if (!wav[0]) continue;
        BYTE* b = load_file(wav);
        if (b && !(b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'A')) b = 0;
        g_castWav[i] = b;
        any |= b != 0;
        logf("MageSkills: skill %d cast sound '%s' -> %s", g_ids[i], wav, b ? "loaded" : "NOT loaded");
    }
    if (!any) return;
    if (!g_playSound) {
        HMODULE w = LoadLibraryA("winmm.dll");
        g_playSound = w ? (PlaySound_t)GetProcAddress(w, "PlaySoundA") : 0;
    }
    DWORD d = (DWORD)m.base - 0x400000;
    /* 95E87C: add ebp,28h; 95E8B6: push 1332814h ("Skill_Fireball_Cast"); 95E8C4: call 8F5910 */
    if (same(m, 0x95E87C, "83c528") && same(m, 0x95E8B6, "68") && *(DWORD*)VA(m, 0x95E8B7) == 0x1332814 + d &&
        same(m, 0x8F5910, "5657")) {
        g_castSoundFn = VA(m, 0x8F5910);
        logf("MageSkills: own cast sounds of Fireball-based skills %s (0x95E8C4)",
             patch_call(VA(m, 0x95E8C4), g_castSoundFn, (void*)cast_sound_stub) ? "on" : "NOT patched");
    } else logf("MageSkills: Fireball cast sound code differs -> CastWav off");
}

static void own_init(Module m)
{
    int any = 0;
    for (int i = 0; i < g_n; i++) {
        char key[16]; wsprintfA(key, "Own%d", g_ids[i]);
        g_own[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini) != 0;
        any |= g_own[i];
    }
    if (!any) return;
    /* 923FC0: sub esp,1ACh / mov eax,[14D10B0]; 8FD5A0: mov eax,ecx / mov ecx,[14F13C4]; 763AD0: sub esp,8Ch;
       764330: mov eax,[esp+4] / push ebp / push edi / push eax / mov edi,ecx; 89EC50: push ecx / push ebx / mov ebx,ecx /
       push edi / mov edi,[ebx+4]; 924440..924444: dec eax / cmp eax,39h / ja 92599B (0x39 = last case, default) */
    DWORD d = (DWORD)m.base - 0x400000;
    int ok = same(m, 0x923FC0, "81ecac010000a1") && same(m, 0x8FD5A0, "8bc18b0d") &&
             *(DWORD*)VA(m, 0x8FD5A4) == 0x14F13C4 + d && same(m, 0x763AD0, "81ec8c000000a1") &&
             same(m, 0x764330, "8b4424045557508bf9") && same(m, 0x89EC50, "51538bd9578b7b04") &&
             same(m, 0x924440, "4883f839") && g_find && g_protoMgr;
    if (!ok) { logf("MageSkills: own skills: hero cast code differs -> off"); return; }
    g_ctrlStats = (Stats0_t)VA(m, 0x8FD5A0);
    g_canUse0 = (Skill1b_t)VA(m, 0x763AD0);
    g_pay0 = (Skill1b_t)VA(m, 0x764330);
    g_cooldown = (Cooldown_t)VA(m, 0x89EC50);
    /* 755500/756250/754E50: add ecx,148h / push ecx / mov ecx,[14F1758]; 886BF0: push ebp / mov ebp,esp / and esp,-8;
       724FCE: mov ecx,[14F17E8] / call 886BF0 (the alignment manager as the game's area search uses it) */
    BYTE* rc = VA(m, 0x724FD4);
    if (same(m, 0x755500, "81c148010000518b0d") && same(m, 0x756250, "81c148010000518b0d") &&
        same(m, 0x754E50, "81c148010000518b0d") && same(m, 0x886BF0, "558bec83e4f883ec1c") &&
        same(m, 0x724FCE, "8b0d") && *(DWORD*)VA(m, 0x724FD0) == 0x14F17E8 + d && rc[0] == 0xE8 &&
        (DWORD)(rc + 5 + *(int*)(rc + 1)) == 0x886BF0 + d && g_statsMgr) {
        g_pos = (Pos_t)VA(m, 0x755500);
        g_active = (Flag_t)VA(m, 0x756250);
        g_bit21 = (Flag_t)VA(m, 0x754E50);
        /* 7563B0: add ecx,148h / push ecx / mov ecx,[14F1758] / call 818330 / mov eax,[eax+4] / shr eax,0Ch (IsDead) */
        if (same(m, 0x7563B0, "81c148010000518b0d") && same(m, 0x7563C2, "8b4004c1e80c")) g_dead = (Flag_t)VA(m, 0x7563B0);
        /* 7565C0: add ecx,148h / push ecx / mov ecx,[14F1758] / call 818330 / mov eax,[eax+4] / shr eax,0Fh */
        if (same(m, 0x7565C0, "81c148010000518b0d") && same(m, 0x7565D2, "8b4004c1e80f")) g_bit15 = (Flag_t)VA(m, 0x7565C0);
        else logf("MageSkills: own skills: dead check code differs -> corpses may be struck");
        g_relation = (Relation_t)VA(m, 0x886BF0);
        g_alignMgr = (void**)VA(m, 0x14F17E8);
        /* 7B9C00: sub esp,8 / push esi / push edi / lea eax,[esp+14h]; 9621F9: mov ebp,[14F1828] (formula manager)
           981120: fldz / mov eax,ecx; 76C800: sub esp,0Ch / push ebx / push esi / mov esi,ecx; 89E930: sub esp,8 /
           push ebx / push ebp / push esi / push edi / lea esi,[ecx+4]; 89D540: push ecx / mov eax,[ecx+14h];
           751CF0: mov ecx,[ecx+100h] */
        if (same(m, 0x7B9C00, "83ec0856578d442414") && same(m, 0x9621F9, "8b2d") && *(DWORD*)VA(m, 0x9621FB) == 0x14F1828 + d &&
            same(m, 0x981120, "d9ee8bc1") && same(m, 0x76C800, "83ec0c53568bf1") && same(m, 0x89E930, "83ec08535556578d7104") &&
            same(m, 0x89D540, "518b4114") && same(m, 0x751CF0, "8b8900010000")) {
            g_magicDmg = (MagicDmg_t)VA(m, 0x7B9C00);
            g_formulaMgr = (void**)VA(m, 0x14F1828);
            g_hitCtor = (HitCtor_t)VA(m, 0x981120);
            g_takeHit = (TakeHit_t)VA(m, 0x76C800);
            g_param = (Param_t)VA(m, 0x89E930);
            g_rank = (Rank_t)VA(m, 0x89D540);
            g_level = (Level_t)VA(m, 0x751CF0);
        } else logf("MageSkills: own skills: damage code differs -> no damage");
        /* 751910: mov eax,[ecx+16Ch] / ret (the hero's level, 07.11) */
        if (same(m, 0x751910, "8b816c010000c3")) g_heroLevel = (Level_t)VA(m, 0x751910);
        else logf("MageSkills: hero level code differs at 0x751910 -> the level of the attribute table (0 for the hero)");
        /* 741140: push ecx / push esi / mov esi,ecx / mov ecx,[esi+38h]; 74FD10: mov eax,[esp+8] / push eax /
           mov eax,[esp+8] / lea edx,[ecx+0Ch]; 74FF40: push ebx/ebp/esi/edi / mov edi,[esp+14h]; 69FBA0: push esi /
           mov esi,ecx / mov eax,[esi]; the cast flash code: 95E0D9 push 14F2FACh, 95E85E call [12B712Ch] (add ref),
           95E1B1 mov esi,14CD590h, 95E19A mov edx,[14EF900h], 95E1C9 mov ecx,[14EF5CCh] */
        if (same(m, 0x741140, "51568bf18b4e38") && same(m, 0x74FD10, "8b442408508b442408") &&
            same(m, 0x74FF40, "535556578b7c2414") && same(m, 0x69FBA0, "568bf18b06") &&
            same(m, 0x95E0D9, "68") && *(DWORD*)VA(m, 0x95E0DA) == 0x14F2FAC + d &&
            same(m, 0x95E85E, "ff15") && *(DWORD*)VA(m, 0x95E860) == 0x12B712C + d &&
            same(m, 0x95E1B1, "be") && *(DWORD*)VA(m, 0x95E1B2) == 0x14CD590 + d &&
            same(m, 0x95E19A, "8b15") && *(DWORD*)VA(m, 0x95E19C) == 0x14EF900 + d &&
            same(m, 0x95E1C9, "8b0d") && *(DWORD*)VA(m, 0x95E1CB) == 0x14EF5CC + d && g_fixed) {
            g_compEntry = (CompEntry_t)VA(m, 0x74FF40);
            g_compCast = (CompCast_t)VA(m, 0x69FBA0);
            g_setCasterFx = (SetFx_t2)VA(m, 0x741140);
            g_startComp = (Start_t)VA(m, 0x74FD10);
            g_addRef = *(AddRef_t*)VA(m, 0x12B712C);
            g_fxType = VA(m, 0x14F2FAC);
            g_fxVecA = (Vec3*)VA(m, 0x14EF5CC); g_fxVecB = (Vec3*)VA(m, 0x14EF900); g_fxMat = (Mat33*)VA(m, 0x14CD590);
            /* 739AF0: add ecx,1Ch / jmp [12B7550] (string assign); 739B00: mov al,[esp+4] / mov [ecx+40h],al / ret 4;
               the string import pair as at 0x6E25B2 (ctor from const char*) / 0x6E25C9 (dtor) */
            if (same(m, 0x739AF0, "83c11cff25") && *(DWORD*)VA(m, 0x739AF5) == 0x12B7550 + d &&
                same(m, 0x739B00, "8a442404884140c20400") &&
                same(m, 0x6E25B2, "ff15") && *(DWORD*)VA(m, 0x6E25B4) == 0x12B758C + d &&
                same(m, 0x6E25C9, "ff15") && *(DWORD*)VA(m, 0x6E25CB) == 0x12B7594 + d) {
                g_setAnim = (SetAnim_t)VA(m, 0x739AF0);
                g_setAnimFlag = (SetFlag_t)VA(m, 0x739B00);
                g_animStrCtor = *(StrCtor_t*)VA(m, 0x12B758C);
                g_animStrDtor = *(StrDtor_t*)VA(m, 0x12B7594);
            } else logf("MageSkills: own skills: cast animation code differs -> no animation");
            for (int i = 0; i < g_n; i++) {
                char key[16]; wsprintfA(key, "OwnFx%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", g_ownFxName[i], sizeof g_ownFxName[i], g_ini);
                wsprintfA(key, "OwnAnim%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", g_ownAnim[i], sizeof g_ownAnim[i], g_ini);
                if (g_ownFxName[i][0]) logf("MageSkills: skill %d shows its own effect '%s' on the caster", g_ids[i], g_ownFxName[i]);
                wsprintfA(key, "OwnHitFx%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", g_ownHitFxName[i], sizeof g_ownHitFxName[i], g_ini);
                if (g_ownHitFxName[i][0]) logf("MageSkills: skill %d shows '%s' on every target", g_ids[i], g_ownHitFxName[i]);
            }
        } else logf("MageSkills: own skills: caster effect code differs -> no effect");
    } else logf("MageSkills: own skills: character search code differs -> no targets");
    for (int i = 0; i < g_n; i++) {
        char key[16], wav[MAX_PATH];
        wsprintfA(key, "OwnLock%d", g_ids[i]);
        g_ownLock[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        if (g_ownLock[i] < 0 || g_ownLock[i] > 10000 || !g_pump) g_ownLock[i] = 0;
        wsprintfA(key, "OwnHold%d", g_ids[i]);
        g_ownHold[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        if (g_ownHold[i] < 0 || g_ownHold[i] > 10000 || !g_pump) g_ownHold[i] = 0;
        wsprintfA(key, "OwnDelay%d", g_ids[i]);
        g_ownDelay[i] = GetPrivateProfileIntA("MageSkills", key, 0, g_ini);
        if (g_ownDelay[i] < 0 || g_ownDelay[i] > 5000) g_ownDelay[i] = 0;
        if (g_ownDelay[i] && !g_pump) { logf("MageSkills: skill %d: no message loop hook -> the hit is not delayed", g_ids[i]); g_ownDelay[i] = 0; }
        {
            char v[1024], *c = v;
            wsprintfA(key, "OwnChain%d", g_ids[i]);
            GetPrivateProfileStringA("MageSkills", key, "", v, sizeof v, g_ini);
            int f[3] = { 0, 0, 0 }, nf = 0;
            while (*c && nf < 3) {
                int x = 0;
                while (*c >= '0' && *c <= '9') x = x * 10 + (*c++ - '0');
                f[nf++] = x;
                while (*c && (*c < '0' || *c > '9')) c++;
            }
            if (nf == 3 && f[1] > 0 && g_pump && g_takeHit) {
                g_chainFall[i] = f[0] > 90 ? 90 : f[0]; g_chainStep[i] = f[1]; g_chainRange[i] = f[2] ? f[2] : 15;
                wsprintfA(key, "OwnBeam%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", v, sizeof v, g_ini);
                /* <name>@<cm>,... or <prefix>*@<from>-<to>/<step>: the names <prefix><cm> (03.11: a beam every 50 cm) */
                for (c = v; *c && g_beamN[i] < MAX_BEAMS; ) {
                    char nm[40]; int n = 0, cm = 0, to = 0, step = 0, star = 0;
                    while (*c && *c != '@' && *c != ',' && n < 39) { if (*c == '*') star = 1; else nm[n++] = *c; c++; }
                    nm[n] = 0;
                    if (*c == '@') { c++; while (*c >= '0' && *c <= '9') cm = cm * 10 + (*c++ - '0'); }
                    if (*c == '-') { c++; while (*c >= '0' && *c <= '9') to = to * 10 + (*c++ - '0'); }
                    if (*c == '/') { c++; while (*c >= '0' && *c <= '9') step = step * 10 + (*c++ - '0'); }
                    while (*c && *c != ',') c++;
                    if (*c == ',') c++;
                    if (!n || cm <= 0) continue;
                    if (!star || step <= 0 || to < cm) to = cm, step = 1;
                    for (int x = cm; x <= to && g_beamN[i] < MAX_BEAMS; x += step) {
                        if (star) wsprintfA(g_beamName[i][g_beamN[i]], "%s%d", nm, x);
                        else { int q = 0; while (nm[q]) { g_beamName[i][g_beamN[i]][q] = nm[q]; q++; } g_beamName[i][g_beamN[i]][q] = 0; }
                        g_beamCm[i][g_beamN[i]++] = x;
                    }
                }
                g_chainSwap = GetPrivateProfileIntA("MageSkills", "ChainSwap", 0, g_ini);
                {
                    char hv[32]; int hn[3] = { 55, 20, 135 }, k = 0, neg = 0, val = 0, any = 0;
                    GetPrivateProfileStringA("MageSkills", "ChainHand", "", hv, sizeof hv, g_ini);
                    for (char* q = hv; k < 3; q++) {
                        if (*q == '-') neg = 1;
                        else if (*q >= '0' && *q <= '9') { val = val * 10 + (*q - '0'); any = 1; }
                        else { if (any) hn[k++] = neg ? -val : val; neg = val = any = 0; if (!*q) break; }
                    }
                    for (k = 0; k < 3; k++) g_chainHand[k] = hn[k] / 100.0f;
                    logf("MageSkills: chain lightning starts at the hand: %d cm forward, %d right, %d up", hn[0], hn[1], hn[2]);
                }
                wsprintfA(key, "OwnStrikeSound%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", g_strikeSound[i], sizeof g_strikeSound[i], g_ini);
                wsprintfA(key, "OwnCastSound%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", g_castSound[i], sizeof g_castSound[i], g_ini);
                /* 95E8AB: mov ecx,esi / call 752E20 / ... push 1332814h ("Skill_Fireball_Cast") / ... call 8F5910;
                   752E20: mov eax,[ecx+224h] / ret; 8F5910: push esi / push edi */
                BYTE* cs = VA(m, 0x95E8C4);
                if (!g_charSound && (g_strikeSound[i][0] || g_castSound[i][0])) {
                    if (same(m, 0x752E20, "8b8124020000c3") && same(m, 0x95E8B6, "68") && *(DWORD*)VA(m, 0x95E8B7) == 0x1332814 + d &&
                        cs[0] == 0xE8 && (DWORD)(cs + 5 + *(int*)(cs + 1)) == 0x8F5910 + d && same(m, 0x8F5910, "5657"))
                        g_charSound = (CharSound_t)VA(m, 0x8F5910);
                    else logf("MageSkills: character sound code differs -> no game sounds for skill %d", g_ids[i]);
                }
                if (g_charSound) logf("MageSkills: skill %d plays the game's sounds: cast '%s', strike '%s'", g_ids[i], g_castSound[i], g_strikeSound[i]);
                wsprintfA(key, "OwnStrikeFx%d", g_ids[i]);
                GetPrivateProfileStringA("MageSkills", key, "", v, sizeof v, g_ini);
                for (c = v; *c && g_strikeN[i] < 3; ) {
                    int n = 0;
                    while (*c && *c != ',' && n < 39) g_strikeName[i][g_strikeN[i]][n++] = *c++;
                    g_strikeName[i][g_strikeN[i]][n] = 0;
                    if (*c == ',') c++;
                    if (n) g_strikeN[i]++;
                }
                /* 9827E1: mov ecx,[14F170C] ... 9827F9: call 810BA0; 810BA0: push ebx / mov ebx,[esp+8] / mov eax,[ebx];
                   810060/810270/80FFC0: mov eax,[ecx+8] / test eax,eax / jl (the instance table [14F1430]) */
                BYTE* cc = VA(m, 0x9827F9);
                if (!g_fxCreate && same(m, 0x9827E1, "8b0d") && *(DWORD*)VA(m, 0x9827E3) == 0x14F170C + d && cc[0] == 0xE8 &&
                    (DWORD)(cc + 5 + *(int*)(cc + 1)) == 0x810BA0 + d && same(m, 0x810BA0, "538b5c24088b03") &&
                    same(m, 0x810060, "8b410885c07c") && same(m, 0x810270, "8b410885c07c") && same(m, 0x80FFC0, "8b410885c07c")) {
                    g_fxCreate = (FxCreate_t)VA(m, 0x810BA0);
                    g_fxPos = (FxSet_t)VA(m, 0x810060);
                    g_fxRot = (FxSet_t)VA(m, 0x810270);
                    g_fxFlag = (FxFlag_t)VA(m, 0x80FFC0);
                    g_fxMgr = (void**)VA(m, 0x14F170C);
                    /* 8103B8: mov edx,[14F1430] / movzx esi,word [edx+3Eh] (the table the release check reads) */
                    if (same(m, 0x8103B8, "8b15") && *(DWORD*)VA(m, 0x8103BA) == 0x14F1430 + d && same(m, 0x8103BE, "0fb7723e"))
                        g_fxTable = (BYTE**)VA(m, 0x14F1430);
                    g_fxFreeOk = same(m, 0x810C16, "8b168b4424108b5234508bceffd2");
                    if (!g_fxFreeOk) logf("MageSkills: effect release code at 0x810C16 differs -> free effects stay (fps)");
                } else if (!g_fxCreate) logf("MageSkills: effect manager code differs -> chain without beams");
                logf("MageSkills: skill %d is a chain: %d%% less per leap, %d ms per leap, first enemy within %d m, %d beam(s)",
                     g_ids[i], g_chainFall[i], g_chainStep[i], g_chainRange[i], g_beamN[i]);
            } else if (nf) logf("MageSkills: skill %d: OwnChain needs 3 numbers, the message loop and the damage code -> off", g_ids[i]);
        }
        wsprintfA(key, "OwnSound%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", wav, sizeof wav, g_ini);
        if (wav[0]) {
            if (!g_playSound) {
                HMODULE w = LoadLibraryA("winmm.dll");
                g_playSound = w ? (PlaySound_t)GetProcAddress(w, "PlaySoundA") : 0;
            }
            g_ownSound[i] = load_file(wav);
            BYTE* b = g_ownSound[i];
            if (b && !(b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'A')) g_ownSound[i] = 0;
            logf("MageSkills: skill %d sound '%s' -> %s", g_ids[i], wav, g_ownSound[i] && g_playSound ? "loaded" : "NOT loaded");
            wsprintfA(key, "OwnSoundF%d", g_ids[i]);
            GetPrivateProfileStringA("MageSkills", key, "", wav, sizeof wav, g_ini);
            if (wav[0] && same(m, 0x759820, "5355568bd957")) {
                g_ownSoundF[i] = load_file(wav);
                *(void**)&g_isFemale = VA(m, 0x759820);
                logf("MageSkills: skill %d female sound '%s' -> %s", g_ids[i], wav, g_ownSoundF[i] ? "loaded" : "NOT loaded");
            }
        }
        /* 06.11: OwnGameSoundF<id> = an event of General_Sounds.bnk for a female hero instead of the WAVs
           (the game has no female shout of the hero; first try: Assume_Stance_BattleRage) */
        wsprintfA(key, "OwnGameSoundF%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_gameSoundF[i], sizeof g_gameSoundF[i], g_ini);
        if (g_gameSoundF[i][0]) {
            BYTE* cs = VA(m, 0x95E8C4);
            if (!g_charSound && same(m, 0x752E20, "8b8124020000c3") && same(m, 0x95E8B6, "68") && *(DWORD*)VA(m, 0x95E8B7) == 0x1332814 + d &&
                cs[0] == 0xE8 && (DWORD)(cs + 5 + *(int*)(cs + 1)) == 0x8F5910 + d && same(m, 0x8F5910, "5657"))
                g_charSound = (CharSound_t)VA(m, 0x8F5910);
            if (!g_isFemale && same(m, 0x759820, "5355568bd957")) *(void**)&g_isFemale = VA(m, 0x759820);
            /* A13680: mov eax,[14F13F8] / test / ... / mov ecx,eax / call 6EBE90 (checked in the exe 07.11) */
            BYTE* pc = VA(m, 0xA136A0);
            if (!g_postDirect && same(m, 0xA13680, "a1") && *(DWORD*)VA(m, 0xA13681) == 0x14F13F8 + d && pc[0] == 0xE8 &&
                (DWORD)(pc + 5 + *(int*)(pc + 1)) == 0x6EBE90 + d && same(m, 0x6EBE90, "83ec1c568bf1")) {
                g_postDirect = (PostByName_t)VA(m, 0x6EBE90);
                g_postMgr = (void**)VA(m, 0x14F13F8);
            }
            if (!g_charSound || !g_isFemale) { logf("MageSkills: skill %d: game or gender code differs -> no female game sound", g_ids[i]); g_gameSoundF[i][0] = 0; }
            else logf("MageSkills: skill %d female game sound '%s'", g_ids[i], g_gameSoundF[i]);
        }
        if (g_ownDelay[i]) logf("MageSkills: skill %d hits %d ms after the cast", g_ids[i], g_ownDelay[i]);
    }
    /* 9243F4: cmp eax,14h; 9243FF: push 14F2F44h; 739750: mov eax,[ecx+1Ch] / cmp eax,8; 739770: cmp dword [ecx+1Ch],7 */
    if (g_compEntry && g_compCast && same(m, 0x9243F4, "83f814") && same(m, 0x9243FF, "68") &&
        *(DWORD*)VA(m, 0x924400) == 0x14F2F44 + d && same(m, 0x739750, "8b411c83f808") && same(m, 0x739770, "83791c07")) {
        g_moveType = VA(m, 0x14F2F44);
        g_moveEnds = (MoveEnds_t)VA(m, 0x739750);
        g_moveStop = (MoveStop_t)VA(m, 0x739770);
    } else logf("MageSkills: own skills: movement stop code differs -> casting while running is not stopped");
    g_heroCast = (HeroCast_t)detour(VA(m, 0x923FC0), (void*)own_cast);
    logf("MageSkills: own skills (CodePath Newt) %s at the hero's cast 0x923FC0", g_heroCast ? "taken" : "NOT taken");
}

/* ---- CastTrace=1 (diagnostics, stage 1 of an own skill): who asks for a cast and what changes ----
   0xA709D0 thiscall(a1, a2, a3) ret 0Ch: the cast dispatcher (switch on the behaviour id at 0xA70AC4), called only
   from 0x91E930 thiscall(a1, a2, a3) ret 0Ch. 0x75B870 thiscall(index, delta): adds to a stat (mana is one of them).
   Logs the arguments, the return address and code addresses found on the stack (the call chain), and every stat
   change for 3 s after a cast. */
typedef BYTE (__fastcall *Cast3_t)(void* self, void* edx, int a1, int a2, int a3);
static Cast3_t g_dispatch, g_request;
static AddStat_t g_statTrace;
static DWORD g_textHi;
static int g_traceLogged;

static void stack_chain(const char* what, DWORD* sp)
{
    char line[256]; int n = wsprintfA(line, "MageSkills: %s chain:", what), k = 0;
    for (int i = 0; i < 400 && k < 14 && n < 230; i++)
        if (readable(sp + i, 4) && sp[i] >= g_textLo && sp[i] < g_textHi) { n += wsprintfA(line + n, " %08X", sp[i] - (g_textLo - 0x401000)); k++; }
    logf("%s", line);
}

static BYTE __fastcall trace_dispatch(void* self, void* edx, int a1, int a2, int a3)
{
    if (g_traceLogged++ < 40) {
        logf("MageSkills: cast dispatcher 0xA709D0 self=%08X a1=%d a2=%d a3=%d (return %08X)", (DWORD)self, a1, a2, a3,
             (DWORD)__builtin_return_address(0) - (g_textLo - 0x401000));
        stack_chain("dispatcher", (DWORD*)__builtin_frame_address(0));
    }
    g_castAt = GetTickCount(); g_statLogged = 0;
    BYTE r = g_dispatch(self, edx, a1, a2, a3);
    if (g_traceLogged < 41) logf("MageSkills: cast dispatcher -> %d", r);
    return r;
}

static BYTE __fastcall trace_request(void* self, void* edx, int a1, int a2, int a3)
{
    if (g_traceLogged < 40) {
        logf("MageSkills: cast request 0x91E930 self=%08X a1=%d a2=%d a3=%d (return %08X)", (DWORD)self, a1, a2, a3,
             (DWORD)__builtin_return_address(0) - (g_textLo - 0x401000));
        stack_chain("request", (DWORD*)__builtin_frame_address(0));
    }
    return g_request(self, edx, a1, a2, a3);
}

static void __fastcall trace_stat(void* stats, void* edx, int index, float delta)
{
    if (g_castAt && GetTickCount() - g_castAt < 3000 && g_statLogged++ < 24) {
        int d1000 = (int)(delta * 1000.0f);
        logf("MageSkills: stat %d of %08X changed by %d/1000 %u ms after the cast (return %08X)", index, (DWORD)stats,
             d1000, GetTickCount() - g_castAt, (DWORD)__builtin_return_address(0) - (g_textLo - 0x401000));
    }
    g_statTrace(stats, edx, index, delta);
}

typedef BYTE (__fastcall *Skill1_t)(void* stats, void* edx, int id);
static Skill1_t g_canUse, g_pay;
static int g_canLogged, g_payLogged;

/* 0x763AD0 thiscall(id) ret 4: can the character use the skill (mana [+190h] vs cost, dragon form, "out_of_mana");
   0x764330 thiscall(id) ret 4: pays the skill's mana cost (vtable +28h with -cost) */
static BYTE __fastcall trace_can(void* stats, void* edx, int id)
{
    BYTE r = g_canUse(stats, edx, id);
    if ((ours(id) || id == 0x1B) && g_canLogged++ < 12) {
        logf("MageSkills: can use skill %d (stats %08X) -> %d (return %08X)", id, (DWORD)stats, r,
             (DWORD)__builtin_return_address(0) - (g_textLo - 0x401000));
        stack_chain("can use", (DWORD*)__builtin_frame_address(0));
    }
    return r;
}

static BYTE __fastcall trace_pay(void* stats, void* edx, int id)
{
    if ((ours(id) || id == 0x1B) && g_payLogged++ < 12) {
        logf("MageSkills: pay for skill %d (stats %08X, return %08X)", id, (DWORD)stats,
             (DWORD)__builtin_return_address(0) - (g_textLo - 0x401000));
        stack_chain("pay", (DWORD*)__builtin_frame_address(0));
    }
    return g_pay(stats, edx, id);
}

static void cast_trace_init(Module m)
{
    if (!GetPrivateProfileIntA("MageSkills", "CastTrace", 0, g_ini)) return;
    g_textLo = (DWORD)VA(m, 0x401000); g_textHi = (DWORD)VA(m, 0xC90000);
    /* A709D0: sub esp,90h / push ebx / push ebp; 91E930: cmp dword [esp+4],-1 / push ebx / mov ebx,ecx;
       75B870: push esi / mov esi,ecx / mov ecx,[esi+104h] */
    int ok = same(m, 0xA709D0, "81ec900000005355") && same(m, 0x91E930, "837c2404ff538bd9") &&
             same(m, 0x75B870, "568bf18b8e04010000");
    if (!ok) { logf("MageSkills: cast trace skipped: bytes differ"); return; }
    g_dispatch = (Cast3_t)detour(VA(m, 0xA709D0), (void*)trace_dispatch);
    g_request = (Cast3_t)detour(VA(m, 0x91E930), (void*)trace_request);
    g_statTrace = (AddStat_t)detour(VA(m, 0x75B870), (void*)trace_stat);
    /* 763AD0: sub esp,8Ch / mov eax,[14D10B0]; 764330: mov eax,[esp+4] / push ebp / push edi / push eax / mov edi,ecx */
    if (same(m, 0x763AD0, "81ec8c000000a1") && same(m, 0x764330, "8b4424045557508bf9")) {
        g_canUse = (Skill1_t)detour(VA(m, 0x763AD0), (void*)trace_can);
        g_pay = (Skill1_t)detour(VA(m, 0x764330), (void*)trace_pay);
    }
    logf("MageSkills: cast trace %s/%s/%s, can-use %s, pay %s", g_dispatch ? "on" : "off", g_request ? "on" : "off", g_statTrace ? "on" : "off", g_canUse ? "on" : "off", g_pay ? "on" : "off");
}

/* the slow: stat access as the game's own Lua SetRunningSpeed (0x9BA0B0 -> 0x75BAF0) and the message loop to time it */
static int slow_init(Module m)
{
    DWORD d = (DWORD)m.base - 0x400000;
    /* 818330: mov eax,[esp+4] / push esi / push edi / mov edi,[eax] / mov esi,ecx (index lookup with bounds checks) */
    int okHandle = same(m, 0x818330, "8b44240456578b388bf1");
    /* 75B870: push esi / mov esi,ecx / mov ecx,[esi+104h] / test ecx,ecx; 9858F0: fld [esp+4] / fadd [ecx+8] (adds) */
    int okAdd = same(m, 0x75B870, "568bf18b8e0401000085c9") && same(m, 0x9858F0, "d9442404d84108");
    /* RunningSpeed = record 11: 75BB07 mov ecx,[esi+104h] / 75BB11 add ecx,0E8h / 75BB3B push 0Bh / call 75B870 */
    int okStat = same(m, 0x75BB07, "8b8e04010000") && same(m, 0x75BB11, "81c1e8000000") && same(m, 0x75BB3B, "6a0be8");
    /* B38880: sub esp,1Ch / push esi / push edi / mov edi,[USER32!PeekMessageA] (the main message loop) */
    int okPump = same(m, 0xB38880, "83ec1c56578b3d") && *(DWORD*)VA(m, 0xB38887) == 0x12B7A14 + d;
    if (!okHandle || !okAdd || !okStat || !okPump) {
        logf("MageSkills: slow code mismatch (handle %d, add %d, stat %d, loop %d) -> no slow", okHandle, okAdd, okStat, okPump);
        return 0;
    }
    g_byHandle = (ByHandle_t)VA(m, 0x818330);
    g_addStat = (AddStat_t)VA(m, 0x75B870);
    g_pump = (Pump_t)detour(VA(m, 0xB38880), (void*)hk_pump);
    if (!g_pump) { logf("MageSkills: detour of the message loop refused -> no slow"); return 0; }
    return 1;
}

static void fx_init(Module m)
{
    char names[3][64];
    int any = 0, anySlow = 0;
    for (int i = 0; i < g_n; i++) {
        any |= fx_entry(g_ids[i], names);
        anySlow |= slow_entry(i);
    }
    if (!any && !anySlow) return;
    DWORD d = (DWORD)m.base - 0x400000;
    /* 96998D: push 14F1B60 / push 14F1B50 / lea ecx,[esp+0F8h] / call 8FFB20 */
    int okSite = same(m, 0x96998D, "68") && *(DWORD*)VA(m, 0x96998E) == 0x14F1B60 + d && same(m, 0x969992, "68") &&
                 *(DWORD*)VA(m, 0x969993) == 0x14F1B50 + d && same(m, 0x969997, "8d8c24f8000000");
    /* the game reads the skill id the same way right before: 969689 mov edx,[esi+2Ch] / push edx / call 74D350 */
    int okId = same(m, 0x969689, "8b562c52");
    /* 8FFB20: mov eax,[esp+4] / push esi / mov esi,ecx / push eax / lea ecx,[esi+64h]; 4B34D0: NiFixedString from char* */
    int okSet = same(m, 0x8FFB20, "8b442404568bf1508d4e64");
    int okFixed = same(m, 0x4B34D0, "83ec0c538b5c2414");
    if (!okSite || !okId || !okSet || !okFixed) {
        logf("MageSkills: effect code mismatch (site %d, id %d, setter %d, string %d) -> skills keep their code's effects",
             okSite, okId, okSet, okFixed);
        return;
    }
    g_fixed = (Fixed_t)VA(m, 0x4B34D0);
    int anyCast = 0;
    for (int i = 0; i < g_n; i++) {
        if (!fx_entry(g_ids[i], g_fxName[i])) { g_fxName[i][0][0] = g_fxName[i][2][0] = 0; continue; }
        anyCast |= g_fxName[i][2][0] != 0;
        logf("MageSkills: skill %d effects '%s' / '%s' / cast '%s'", g_ids[i], g_fxName[i][0], g_fxName[i][1],
             g_fxName[i][2][0] ? g_fxName[i][2] : "(Fireball's)");
    }
    if (anySlow && !slow_init(m))
        for (int i = 0; i < g_n; i++) g_slowPct[i][0] = 0;
    g_setFx = (SetFx_t)VA(m, 0x8FFB20);
    if (!patch_call(VA(m, 0x96999E), VA(m, 0x8FFB20), (void*)fx_stub)) {
        logf("MageSkills: effect call site 0x96999E not patched");
        for (int i = 0; i < g_n; i++) g_fxName[i][0][0] = g_slowPct[i][0] = 0;
        return;
    }
    logf("MageSkills: projectile effects of Fireball-based skills replaced at 0x96999E");
    for (int i = 0; i < g_n; i++)
        if (g_slowPct[i][0])
            logf("MageSkills: skill %d slows its target by %d%% (+%d%%/rank) for %d ms (+%d ms/rank), %d ms after the cast",
                 g_ids[i], g_slowPct[i][0], g_slowPct[i][1], g_slowMs[i][0], g_slowMs[i][1], g_slowDelay[i]);
    if (!anyCast) return;

    /* 95E842 and 95E864: mov eax,[14F1B68] (FireBall_Cast); 95E86B mov [esi],eax / call 741140; 95E6F2 mov ebp,ecx */
    DWORD fb = 0x14F1B68 + d;
    int okCast = same(m, 0x95E842, "a1") && *(DWORD*)VA(m, 0x95E843) == fb && same(m, 0x95E864, "a1") &&
                 *(DWORD*)VA(m, 0x95E865) == fb && same(m, 0x95E869, "8bcb8906e8") && same(m, 0x95E6F2, "8be9");
    /* the same object layout: 95E73F cmp dword ptr [ebp+24h],-1 (as 9695E2 cmp [esi+24h],-1 next to [esi+2Ch]) */
    int okLayout = same(m, 0x95E73F, "837d24ff") && same(m, 0x9695E2, "837e24ff");
    if (!okCast || !okLayout) {
        logf("MageSkills: cast flash code mismatch (sites %d, layout %d) -> Fireball's cast flash", okCast, okLayout);
        return;
    }
    g_fbCast = (char**)fb;
    /* both loads or none: the first one takes a reference of the string the second one stores */
    BYTE* at[2] = { VA(m, 0x95E842), VA(m, 0x95E864) };
    BYTE orig[2][5], call[5] = { 0xE8 };
    memcpy(orig[0], at[0], 5);
    memcpy(orig[1], at[1], 5);
    int ok = 1;
    for (int s = 0; s < 2 && ok; s++) {
        long rel = (long)((BYTE*)fx_cast_stub - (at[s] + 5));
        memcpy(call + 1, &rel, 4);
        ok = write_mem(at[s], call, 5);
    }
    if (!ok) {
        write_mem(at[0], orig[0], 5);
        write_mem(at[1], orig[1], 5);
    }
    logf("MageSkills: cast flash of Fireball-based skills %s at 0x95E842/0x95E864", ok ? "replaced" : "NOT replaced");
}

/* 03.11 Name<id> = a skill name for an id the exe has no name for (0x1C, 0x36: holes in 0x89D560's list). 0x89D560
   stdcall(NiFixedString* name) ret 4 is the only place that maps a skill name to its id (the names' strings have no
   other reference in the exe, so nothing maps an id back to a name: the game keeps the prototype's UUID); it compares
   with 0x402580 thiscall(NiFixedString*, const char*) and gives -1 for an unknown name. */
typedef int  (__stdcall *NameId_t)(void* name);
typedef BYTE (__fastcall *StrEq_t)(void* name, void* edx, const char* text);
static NameId_t g_nameId;
static StrEq_t  g_strEq;
static char     g_idName[MAX_GRANT][40];
static int      g_nameLogged;

static int __stdcall name_id(void* name)
{
    for (int i = 0; i < g_n; i++)
        if (g_idName[i][0] && g_strEq(name, 0, g_idName[i])) {
            if (g_nameLogged++ < 4) logf("MageSkills: skill name '%s' -> id %d", g_idName[i], g_ids[i]);
            return g_ids[i];
        }
    return g_nameId(name);
}

static void name_init(Module m)
{
    int any = 0;
    for (int i = 0; i < g_n; i++) {
        char key[16]; wsprintfA(key, "Name%d", g_ids[i]);
        GetPrivateProfileStringA("MageSkills", key, "", g_idName[i], sizeof g_idName[i], g_ini);
        any |= g_idName[i][0] != 0;
    }
    if (!any) return;
    /* 89D560: push esi / mov esi,[esp+8] / push 132A4C4h ("Fireball") / mov ecx,esi / call 402580 */
    DWORD d = (DWORD)m.base - 0x400000;
    BYTE* c = VA(m, 0x89D56C);
    if (!(same(m, 0x89D560, "568b74240868") && *(DWORD*)VA(m, 0x89D566) == 0x132A4C4 + d && same(m, 0x89D56A, "8bce") &&
          c[0] == 0xE8 && (DWORD)(c + 5 + *(int*)(c + 1)) == 0x402580 + d && same(m, 0x402580, "8b018b4c24043bc1"))) {
        logf("MageSkills: skill name table code differs -> Name<id> off");
        return;
    }
    g_strEq = (StrEq_t)VA(m, 0x402580);
    g_nameId = (NameId_t)detour(VA(m, 0x89D560), (void*)name_id);
    for (int i = 0; i < g_n; i++)
        if (g_idName[i][0]) logf("MageSkills: skill id %d gets the name '%s' (0x89D560) -> %s", g_ids[i], g_idName[i],
                                 g_nameId ? "on" : "detour refused");
}

void mageskills_init(Module m)
{
    if (!GetPrivateProfileIntA("MageSkills", "Enable", 0, g_ini)) return;
    char list[128];
    GetPrivateProfileStringA("MageSkills", "Grant", "14", list, sizeof list, g_ini);
    parse_ids(list);
    name_init(m);                        /* before anything reads the skill prototypes */
    if (GetPrivateProfileIntA("MageSkills", "TrainerTrace", 0, g_ini)) {
        /* 89F0D0: push ecx / mov edx,[esp+8] / push esi / mov esi,[14F1474]; B1B200: push ebp / mov ebp,esp / and esp,-8 / sub esp,50h
           B1B284: call 89F0D0 (the record's skill id in the list builder) */
        DWORD d = (DWORD)m.base - 0x400000;
        BYTE* c = VA(m, 0xB1B284);
        if (same(m, 0x89F0D0, "518b542408568b35") && same(m, 0xB1B200, "558bec83e4f883ec50") && c[0] == 0xE8 &&
            (DWORD)(c + 5 + *(int*)(c + 1)) == 0x89F0D0 + d) {
            g_trainerRet = 0xB1B289 + d;
            g_trainerSkillId = (TrainerSkillId_t)detour(VA(m, 0x89F0D0), (void*)trainer_skill_id);
            g_trainerList = g_trainerSkillId ? (TrainerList_t)detour(VA(m, 0xB1B200), (void*)trainer_list_trace) : 0;
            logf("MageSkills: trainer list trace %s", g_trainerList ? "on (0xB1B200)" : "NOT installed");
        } else {
            logf("MageSkills: trainer list trace skipped: bytes at 0x89F0D0/0xB1B200/0xB1B284 differ");
        }
    }
    g_cap = GetPrivateProfileIntA("MageSkills", "RankCap", 5, g_ini);
    if (!g_n) { logf("MageSkills: no skill ids in [MageSkills] Grant -> off"); return; }
    trainer_init(m);
    human_init(m);
    /* TreeSheet=0: the hero's skill tree shows every cell's own button icon instead of the pre-drawn sheet
       characterCreation_I26.dds (10 columns x 6 rows), so cells in rows X >= 6 get a picture too. 0xA54360 passes
       showPlayerSkillIcons(true) unless the character flag bit 19 is set; A543A0: mov byte [esp+28h],1 -> 0 */
    if (!GetPrivateProfileIntA("MageSkills", "TreeSheet", 1, g_ini)) {
        BYTE zero = 0;
        int ok = same(m, 0xA543A0, "c644242801") && same(m, 0xA54390, "885c242868") && write_mem(VA(m, 0xA543A4), &zero, 1);
        logf("MageSkills: skill tree %s", ok ? "shows every cell's own icon (sheet off, 0xA543A4)" : "sheet NOT switched off: bytes at 0xA543A0 differ");
    }

    DWORD d = (DWORD)m.base - 0x400000;
    /* A0BCE0: sub esp,4Ch / push ebx / mov ebx,[esp+54h] / push esi / push edi */
    int okTab = same(m, 0xA0BCE0, "83ec4c538b5c24545657");
    /* 8C9330: mov ecx,[14F1758] / xor eax,eax / test ecx,ecx */
    int okChar = same(m, 0x8C9330, "8b0d") && *(DWORD*)VA(m, 0x8C9332) == 0x14F1758 + d && same(m, 0x8C9336, "33c085c9");
    /* 818370: mov eax,[esp+4] / push esi / mov esi,ecx / mov ecx,[esi+34h] */
    int okStats = same(m, 0x818370, "8b442404568bf18b4e34");
    /* 7692D0: sub esp,7Ch / mov eax,[14D10B0] */
    int okGrant = same(m, 0x7692D0, "83ec7ca1") && *(DWORD*)VA(m, 0x7692D4) == 0x14D10B0 + d;
    /* 753330: push ebx / push ebp / push esi / push edi / mov edi,ecx / mov ebx,[edi+1C0h] */
    int okFind = same(m, 0x753330, "535556578bf98b9fc0010000");
    /* the globals are read where the game reads them: A0BD30 mov ecx,[14F13C4] / 89E1CD mov esi,[14F1474] */
    int okGlobals = same(m, 0xA0BD30, "8b0d") && *(DWORD*)VA(m, 0xA0BD32) == 0x14F13C4 + d &&
                    same(m, 0x89E1CD, "8b35") && *(DWORD*)VA(m, 0x89E1CF) == 0x14F1474 + d;
    if (!okTab || !okChar || !okStats || !okGrant || !okFind || !okGlobals) {
        logf("MageSkills: code mismatch (tab %d, character %d, stats %d, grant %d, find %d, globals %d) -> off",
             okTab, okChar, okStats, okGrant, okFind, okGlobals);
        return;
    }
    g_character = (Character_t)VA(m, 0x8C9330);
    g_stats = (Stats_t)VA(m, 0x818370);
    g_grant = (Grant_t)VA(m, 0x7692D0);
    g_find = (Find_t)VA(m, 0x753330);
    g_statsMgr = (void**)VA(m, 0x14F13C4);
    g_protoMgr = (BYTE**)VA(m, 0x14F1474);
    g_tabList = (TabList_t)detour(VA(m, 0xA0BCE0), (void*)hk_tabList);
    if (!g_tabList) { logf("MageSkills: detour of the skill tree lists refused -> off"); return; }
    logf("MageSkills: on, %d skill(s) given to the hero when the skill tree opens (first id %d, cap %d)", g_n, g_ids[0], g_cap);

    fx_init(m);
    if (!GetPrivateProfileIntA("MageSkills", "OwnIcons", 1, g_ini)) { logf("MageSkills: own icons off (ini)"); return; }
    g_iconTrace = GetPrivateProfileIntA("MageSkills", "IconTrace", 0, g_ini);
    /* BC2775: lea ecx,[esp+10h] / push ecx / mov ecx,eax / call 89D350 / push eax / mov ecx,edi / call 8E07B0 */
    int okSites = same(m, 0xBC2775, "8d4c2410518bc8e8") && same(m, 0xBC2781, "508bcfe8");
    /* 8E07B0: sub esp,8 / push ebx / push ebp / push esi / push edi / lea esi,[ecx+3Ch]; 416FF0: push esi / mov esi,ecx */
    int okName = same(m, 0x8E07B0, "83ec08535556578d713c");
    int okAssign = same(m, 0x416FF0, "568bf18b068b4c2408");
    if (!okSites || !okName || !okAssign) {
        logf("MageSkills: icon code mismatch (sites %d, name %d, assign %d) -> skills show their code skill's icon",
             okSites, okName, okAssign);
        return;
    }
    g_iconName = (IconName_t)VA(m, 0x8E07B0);
    g_assign = (Assign_t)VA(m, 0x416FF0);
    if (!patch_call(VA(m, 0xBC2784), VA(m, 0x8E07B0), (void*)icon_name) ||
        !patch_call(VA(m, 0xBC277C), VA(m, 0x89D350), (void*)icon_behaviour)) {
        logf("MageSkills: icon call sites not patched -> skills show their code skill's icon");
        return;
    }
    logf("MageSkills: own icons on (0xBC277C, 0xBC2784)");
    own_init(m);                         /* before the trace: both check the bytes of 0x763AD0/0x764330 */
    cast_wav_init(m);
    if (GetPrivateProfileIntA("MageSkills", "SoundTrace", 0, g_ini)) {
        /* 8F5910: push esi / push edi / mov edi,ecx / mov ecx,[14F13C4] (checked in the exe 07.11) */
        if (same(m, 0x8F5910, "56578bf98b0d")) {
            g_sndTraceReal = (CharSound_t)detour(VA(m, 0x8F5910), (void*)snd_trace);
            logf("MageSkills: sound trace %s (0x8F5910)", g_sndTraceReal ? "on" : "OFF: detour failed");
        } else logf("MageSkills: sound trace OFF: bytes at 0x8F5910 differ");
        /* 6EBE90: sub esp,1Ch / push esi / mov esi,ecx / cmp byte [esi+31h],0 (checked in the exe 07.11) */
        if (same(m, 0x6EBE90, "83ec1c568bf1807e3100")) {
            g_postReal = (PostByName_t)detour(VA(m, 0x6EBE90), (void*)post_trace);
            logf("MageSkills: sound trace %s (0x6EBE90)", g_postReal ? "on" : "OFF: detour failed");
        } else logf("MageSkills: sound trace OFF: bytes at 0x6EBE90 differ");
    }
    blink_init(m);
    shield_init(m);
    summon_power_init(m, (DWORD)m.base - 0x400000);   /* after shield_init: Pack Hunger's pointers */
    newskills_init(m);
    push_init(m);
    trail_init(m);                       /* after own_init: needs the effect manager of an OwnChain skill */
    summon_init(m);                      /* after own_init: needs its skill, rank and parameter readers */
    cast_trace_init(m);                  /* last: slow_init checks the bytes of 0x75B870 before it is detoured */
    /* BC2380: sub esp,178h / mov eax,[14D10B0] */
    if (g_iconTrace && same(m, 0xBC2380, "81ec78010000a1") && *(DWORD*)VA(m, 0xBC2387) == 0x14D10B0 + d) {
        g_image = (Image_t)detour(VA(m, 0xBC2380), (void*)hk_image);
        logf("MageSkills: image URL trace %s", g_image ? "on" : "refused");
    }
}
