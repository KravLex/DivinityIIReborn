/* Div2Fix - skill registration trace (Mage Skills Expansion, stage 2a diagnostics). Changes nothing in the game.
 *
 * MageExpansion.dv2 adds the prototype UUID="Chain Hit" (exe skill id 0x0E), a tree cell and a DefaultSkills entry,
 * but the skill does not show in the skill tree. This module logs every step of that chain (MageExpansion/docs/RESEARCH.md):
 *     0x89D560  thiscall(char** name) ret 4      skill name -> id (-1 unknown): prototype load, CodePath
 *     0x8A4380  thiscall(proto, id, name) ret 0Ch registers a prototype under its id
 *     0x6F3040  thiscall(a, b) ret 8               creates the hero ('Player'): Episodes\<ep>\DefaultSkills.xml -> skills
 *     0x6FEEB0  thiscall(char** name, int* id) ret 8  prototype name -> id (used by DefaultSkills)
 *     0x7692D0  thiscall(id, level, x) ret 0Ch     gives a skill to a character's stats
 *     0x753330  thiscall(id) ret 4                 the character's skill object with this id, or 0 (called, not hooked)
 * Not 0x6F2F70: it ends right before 0x6F3040 without int3 padding and takes no arguments (plain ret); the first
 * build hooked it as ret 8 and broke the stack of its caller 0x723E40 (crash on New Game, 01.11 03:22).
 * Only names/ids of interest are logged (Chain Hit, Newt, Magic Missile as the control), unknown names (first 40) and
 * every grant while DefaultSkills runs. INI: [Diagnostics] SkillTrace=1 (default 0).
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
#define ID_CHAINHIT 0x0E
#define ID_NEWT     0x39
#define ID_MISSILE  0x1B

typedef int  (__fastcall *Lookup_t)(void* self, void* edx, char** name);
typedef BYTE (__fastcall *Register_t)(void* self, void* edx, void* proto, int id, char** name);
typedef BYTE (__fastcall *Defaults_t)(void* self, void* edx, int a, int b);
typedef BYTE (__fastcall *ByName_t)(void* self, void* edx, char** name, int* id);
typedef int  (__fastcall *Grant_t)(void* stats, void* edx, int id, int level, int x);
typedef void* (__fastcall *Find_t)(void* stats, void* edx, int id);

static Lookup_t   g_lookup;
static Register_t g_register;
static Defaults_t g_defaults;
static ByName_t   g_byName;
static Grant_t    g_grant;
static Find_t     g_find;
static int        g_inDefaults, g_unknown;

static const char* text(char** name)
{
    if (!name || !readable(name, 4) || !*name || !readable(*name, 1)) return "?";
    return *name;
}

static int eq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) if (*a != *b) return 0;
    return *a == *b;
}

static int watched(int id) { return id == ID_CHAINHIT || id == ID_NEWT || id == ID_MISSILE; }

static int __fastcall hk_lookup(void* self, void* edx, char** name)
{
    int id = g_lookup(self, edx, name);
    const char* t = text(name);
    if (eq(t, "Chain Hit") || eq(t, "Newt") || eq(t, "Ice Bolt"))
        logf("SkillTrace: name '%s' -> id %d", t, id);
    else if (id == -1 && g_unknown < 40) {
        g_unknown++;
        logf("SkillTrace: unknown skill name '%s' -> -1", t);
    }
    return id;
}

static BYTE __fastcall hk_register(void* self, void* edx, void* proto, int id, char** name)
{
    BYTE ok = g_register(self, edx, proto, id, name);
    if (watched(id)) logf("SkillTrace: prototype '%s' registered as id 0x%02X -> %d", text(name), id, ok);
    return ok;
}

static BYTE __fastcall hk_byname(void* self, void* edx, char** name, int* id)
{
    BYTE ok = g_byName(self, edx, name, id);
    const char* t = text(name);
    if (g_inDefaults || eq(t, "Chain Hit") || eq(t, "Ice Bolt"))
        logf("SkillTrace: prototype by name '%s' -> %d, id %d%s", t, ok, (ok && id && readable(id, 4)) ? *id : -1,
             g_inDefaults ? " [DefaultSkills]" : "");
    return ok;
}

static BYTE __fastcall hk_defaults(void* self, void* edx, int a, int b)
{
    logf("SkillTrace: DefaultSkills.xml -> hero skills: start");
    g_inDefaults = 1;
    BYTE r = g_defaults(self, edx, a, b);
    g_inDefaults = 0;
    logf("SkillTrace: DefaultSkills.xml -> hero skills: end (%d)", r);
    return r;
}

static int __fastcall hk_grant(void* stats, void* edx, int id, int level, int x)
{
    int r = g_grant(stats, edx, id, level, x);
    if (g_inDefaults || watched(id)) {
        void* s = g_find(stats, 0, id);
        logf("SkillTrace: grant id 0x%02X level %d (%d) to stats %08X -> skill object %08X%s", id, level, x,
             (DWORD)stats, (DWORD)s, g_inDefaults ? " [DefaultSkills]" : "");
    }
    return r;
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

void skilltrace_init(Module m)
{
    if (!GetPrivateProfileIntA("Diagnostics", "SkillTrace", 0, g_ini)) return;
    DWORD d = (DWORD)m.base - 0x400000;
    /* 89D560: push esi / mov esi,[esp+8] / push imm32 */
    int okLookup = same(m, 0x89D560, "568b74240868");
    /* 8A4380: sub esp,20h / push ebx / mov ebx,[esp+2Ch] */
    int okReg = same(m, 0x8A4380, "83ec20538b5c242c");
    /* 6F3040: sub esp,50h / mov [esp+0Ch],ecx (the only caller 0x848538 pushes 2 args, ret 8) */
    int okDef = same(m, 0x6F3040, "83ec50894c240c");
    /* 6FEEB0: mov eax,[esp+4] / sub esp,8 / push ebx */
    int okByName = same(m, 0x6FEEB0, "8b44240483ec0853");
    /* 7692D0: sub esp,7Ch / mov eax,[14D10B0] (security cookie) */
    int okGrant = same(m, 0x7692D0, "83ec7ca1") && *(DWORD*)VA(m, 0x7692D4) == 0x14D10B0 + d;
    /* 753330: push ebx / push ebp / push esi / push edi / mov edi,ecx / mov ebx,[edi+1C0h] */
    int okFind = same(m, 0x753330, "535556578bf98b9fc0010000");
    if (!okLookup || !okReg || !okDef || !okByName || !okGrant || !okFind) {
        logf("SkillTrace: code mismatch (lookup %d, register %d, defaults %d, byname %d, grant %d, find %d) -> off",
             okLookup, okReg, okDef, okByName, okGrant, okFind);
        return;
    }
    g_find = (Find_t)VA(m, 0x753330);
    g_lookup = (Lookup_t)detour(VA(m, 0x89D560), (void*)hk_lookup);
    g_register = (Register_t)detour(VA(m, 0x8A4380), (void*)hk_register);
    g_defaults = (Defaults_t)detour(VA(m, 0x6F3040), (void*)hk_defaults);
    g_byName = (ByName_t)detour(VA(m, 0x6FEEB0), (void*)hk_byname);
    g_grant = (Grant_t)detour(VA(m, 0x7692D0), (void*)hk_grant);
    logf("SkillTrace: on (lookup %d, register %d, defaults %d, byname %d, grant %d)",
         g_lookup != 0, g_register != 0, g_defaults != 0, g_byName != 0, g_grant != 0);
}
