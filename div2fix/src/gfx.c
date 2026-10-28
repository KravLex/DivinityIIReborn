/* Div2Fix - graphics options: object LOD / draw distance and cascaded shadows.
 *
 * Every patch site is verified byte-for-byte before it is touched; a mismatch (other game
 * build, other mod already patched it) disables only that option and is written to the log.
 * Addresses are for Divinity2.exe of Divinity II: Developer's Cut (Steam, v1.4.700.x).
 *
 *  LOD:     CLODNode::SelectLevel (0x5855E0) computes  d = |cam - centre| * cameraLODScale
 *           and picks the level whose [near,far) range contains d, or culls when d > max.
 *           We multiply d by 1/LODDistanceMultiplier -> detail levels and draw distance grow.
 *  Shadows: CShadowManager quality 3 map size (0xB9673F, 4096), CCascadedShadowMap range
 *           constant (200.0 shared with unrelated code -> operands are redirected to our own
 *           values instead of changing the constant), split weight (0.75) and the per-cascade
 *           caster mask (all objects cast shadows into every cascade).
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))

static float  g_invLod = 1.0f;
static float  g_fRange = 200.0f, g_fSplit = 0.75f;
static float  g_treeMul = 1.0f;
static double g_dRange = 200.0, g_dRangeM1 = 199.0;

static int same(Module m, DWORD va, const char* hex)
{
    BYTE* p = VA(m, va);
    int n = s_len(hex) / 2;
    if (!readable(p, (SIZE_T)n)) return 0;
    for (int i = 0; i < n; i++) {
        int hi = hex[2*i], lo = hex[2*i+1];
        hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10; lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
        if (p[i] != (BYTE)(hi * 16 + lo)) return 0;
    }
    return 1;
}

/* instruction "op1 op2 [abs32]" whose absolute operand was relocated by the loader (ASLR) */
static int same_ptr(Module m, DWORD instr, BYTE op1, BYTE op2, DWORD ptrva)
{
    BYTE* q = VA(m, instr);
    if (!readable(q, 6) || q[0] != op1 || q[1] != op2) return 0;
    return *(DWORD*)(q + 2) == (DWORD)VA(m, ptrva);
}

/* "3.5" -> 3.5 (no CRT) */
static float ini_float(const char* sec, const char* key, float def)
{
    char b[32]; GetPrivateProfileStringA(sec, key, "", b, sizeof b, g_ini);
    if (!b[0]) return def;
    float v = 0, frac = 0, scale = 1; int i = 0, neg = 0, any = 0, dot = 0;
    while (b[i] == ' ') i++;
    if (b[i] == '-') { neg = 1; i++; }
    for (; b[i]; i++) {
        if (b[i] >= '0' && b[i] <= '9') { any = 1; if (dot) { scale *= 0.1f; frac += (b[i] - '0') * scale; } else v = v * 10 + (b[i] - '0'); }
        else if ((b[i] == '.' || b[i] == ',') && !dot) dot = 1;
        else break;
    }
    if (!any) return def;
    v += frac; return neg ? -v : v;
}

static void lod_patch(Module m, float mult)
{
    /* 58563B: fld [esp+18h] / fmul dword [edi+0C8h] / (585645) fstp [esp+18h] */
    if (!same(m, 0x58563B, "d9442418d88fc8000000d95c2418")) { logf("Graphics: LOD site mismatch -> LOD option skipped"); return; }
    BYTE* cave = (BYTE*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return;
    g_invLod = 1.0f / mult;
    BYTE* c = cave;
    *c++ = 0xD9; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18;                        /* fld  dword [esp+18h]   */
    *c++ = 0xD8; *c++ = 0x8F; *c++ = 0xC8; *c++ = 0; *c++ = 0; *c++ = 0;        /* fmul dword [edi+0C8h]  */
    *c++ = 0xD8; *c++ = 0x0D; *(DWORD*)c = (DWORD)&g_invLod; c += 4;           /* fmul dword [g_invLod]  */
    *c++ = 0xE9; *(DWORD*)c = (DWORD)(VA(m, 0x585645) - (c + 4)); c += 4;      /* jmp  585645            */
    BYTE j[10] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(DWORD*)(j + 1) = (DWORD)(cave - (VA(m, 0x58563B) + 5));
    if (write_mem(VA(m, 0x58563B), j, 10)) logf("Graphics: LOD/draw distance x%d.%02d", (int)mult, (int)(mult * 100) % 100);
}

static void redirect(Module m, DWORD instr, BYTE op1, BYTE op2, DWORD ptrva, void* var, int* ok, int* bad)
{
    if (!same_ptr(m, instr, op1, op2, ptrva)) { (*bad)++; return; }
    DWORD p = (DWORD)var;
    if (write_mem(VA(m, instr) + 2, &p, 4)) (*ok)++; else (*bad)++;
}

void gfx_init(Module m)
{
    const char* S = "Graphics";
    if (!GetPrivateProfileIntA(S, "Enable", 1, g_ini)) { logf("Graphics: disabled in ini"); return; }

    /* ---- LOD / draw distance ---- */
    float lod = ini_float(S, "LODDistanceMultiplier", 1.0f);
    if (lod > 8.0f) lod = 8.0f;
    if (lod > 1.01f) lod_patch(m, lod); else logf("Graphics: LOD unchanged");

    /* ---- shadow map resolution (ShadowQuality 3) ---- */
    int sms = GetPrivateProfileIntA(S, "ShadowMapSize", 4096, g_ini);
    if (sms == 8192) {
        if (same(m, 0xB9673F, "be00100000")) { DWORD v = 8192; if (write_mem(VA(m, 0xB96740), &v, 4)) logf("Graphics: shadow map 8192 (ShadowQuality 3)"); }
        else logf("Graphics: shadow map site mismatch -> skipped");
    }

    /* ---- shadow distance ---- */
    float range = ini_float(S, "ShadowDistance", 200.0f);
    if (range < 50.0f) range = 50.0f; if (range > 1000.0f) range = 1000.0f;
    if (range < 199.5f || range > 200.5f) {
        g_fRange = range; g_dRange = range; g_dRangeM1 = range - 1.0;
        int ok = 0, bad = 0;
        /* verify all first: half-applied range would mismatch the cascade split math */
        int all = same_ptr(m, 0xC32DCC, 0xD9, 0x05, 0x1320278) && same_ptr(m, 0xC3202F, 0xD9, 0x05, 0x1320278) && same_ptr(m, 0x81A702, 0xD9, 0x05, 0x1320278)
               && same_ptr(m, 0x81A270, 0xD9, 0x05, 0x1320278) && same_ptr(m, 0xC31D7C, 0xDD, 0x05, 0x1320270) && same_ptr(m, 0xC32020, 0xDC, 0x15, 0x1320270)
               && same_ptr(m, 0xC31DA2, 0xDC, 0x0D, 0x1370D88);
        if (all) {
            redirect(m, 0xC32DCC, 0xD9, 0x05, 0x1320278, &g_fRange, &ok, &bad);   /* CCascadedShadowMap ctor: range      */
            redirect(m, 0xC3202F, 0xD9, 0x05, 0x1320278, &g_fRange, &ok, &bad);   /* range clamp                          */
            redirect(m, 0x81A702, 0xD9, 0x05, 0x1320278, &g_fRange, &ok, &bad);   /* light shadow defaults                */
            redirect(m, 0x81A270, 0xD9, 0x05, 0x1320278, &g_fRange, &ok, &bad);
            redirect(m, 0xC31D7C, 0xDD, 0x05, 0x1320270, &g_dRange, &ok, &bad);   /* split computation (double)           */
            redirect(m, 0xC32020, 0xDC, 0x15, 0x1320270, &g_dRange, &ok, &bad);
            redirect(m, 0xC31DA2, 0xDC, 0x0D, 0x1370D88, &g_dRangeM1, &ok, &bad);
            logf("Graphics: shadow distance %d (%d sites)", (int)range, ok);
        } else {
            static const DWORD va[7] = { 0xC32DCC, 0xC3202F, 0x81A702, 0x81A270, 0xC31D7C, 0xC32020, 0xC31DA2 };
            for (int i = 0; i < 7; i++) { BYTE* q = VA(m, va[i]); if (readable(q, 6)) logf("  site %08X: %02X %02X %02X %02X %02X %02X", va[i], q[0], q[1], q[2], q[3], q[4], q[5]); else logf("  site %08X: unreadable", va[i]); }
            logf("Graphics: shadow distance sites mismatch -> skipped");
        }
    }

    /* ---- cascade split weight (0..1, higher = more resolution near the camera) ---- */
    float split = ini_float(S, "ShadowSplitWeight", 0.75f);
    if (split < 0.05f) split = 0.05f; if (split > 0.98f) split = 0.98f;
    if (split < 0.745f || split > 0.755f) {
        int ok = 0, bad = 0; g_fSplit = split;
        redirect(m, 0xC32D35, 0xD9, 0x05, 0x13200D0, &g_fSplit, &ok, &bad);
        logf(ok ? "Graphics: shadow split weight changed" : "Graphics: split weight site mismatch -> skipped");
    }

    /* ---- character shadow map resolution ----
       CCharacterShadowMap::Init (0xC3E800) hard-codes a 1024x1024 R32F map with a hard depth compare (VSM forced off):
       0xC3E840 mov dword [esi+28h],400h (size field) / 0xC3E85E push 400h / 0xC3E863 push 400h (CreateRenderedTexture
       w, h). Over a dragon this gives large stair steps. No other code of the class uses the literal size. */
    int css = GetPrivateProfileIntA(S, "CharacterShadowSize", 1024, g_ini);
    if (css != 1024) {
        if (css != 2048 && css != 4096) logf("Graphics: CharacterShadowSize=%d not supported (1024/2048/4096) -> unchanged", css);
        else if (same(m, 0xC3E840, "c74628") && same(m, 0xC3E843, "00040000") && same(m, 0xC3E85E, "6800040000") && same(m, 0xC3E863, "6800040000")) {
            DWORD v = (DWORD)css;
            if (write_mem(VA(m, 0xC3E843), &v, 4) && write_mem(VA(m, 0xC3E85F), &v, 4) && write_mem(VA(m, 0xC3E864), &v, 4))
                logf("Graphics: character shadow map %d (game: 1024)", css);
        } else logf("Graphics: character shadow map site mismatch -> unchanged");
    }

    /* ---- tree (SpeedTree) draw distance ----
       The graphics settings object keeps "TreeQualityMultiplier" (graphicoptions.xml, menu max 3) at +0x68; its only
       reader is the getter 0x6A2C20 (fld [ecx+68h]; ret), called from the SpeedTree code at 0xC8C92A and 0xC8CE3E.
       Raising the value to 6 in graphicoptions.xml moved the tree pop-in much farther (tested 28.10). The setter
       would reset it to the menu value, so instead the getter returns our value:
       0x6A2C20: fld dword [g_treeMul]; ret  (7 bytes; 12 bytes of int3 padding follow the original 4). */
    float tm = ini_float(S, "TreeDistanceMultiplier", 0.0f);
    if (tm > 0.0f) {
        if (tm < 1.0f) tm = 1.0f; if (tm > 10.0f) tm = 10.0f;
        if (same(m, 0x6A2C20, "d94168c3cccccccccccc") && same(m, 0xC8C92A, "e8") && same(m, 0xC8CE3E, "e8")) {
            g_treeMul = tm;
            BYTE c[7] = { 0xD9, 0x05, 0, 0, 0, 0, 0xC3 }; *(DWORD*)(c + 2) = (DWORD)&g_treeMul;
            if (write_mem(VA(m, 0x6A2C20), c, 7)) logf("Graphics: tree distance multiplier %d.%01d (game menu: up to 3)", (int)tm, (int)(tm * 10) % 10);
        } else logf("Graphics: tree distance site mismatch -> unchanged");
    }

    /* ---- every object category casts shadows into every cascade ---- */
    if (GetPrivateProfileIntA(S, "ShadowAllObjects", 0, g_ini)) {
        if (same(m, 0xC3198F, "8b4504")) { BYTE p[3] = { 0x83, 0xC8, 0xFF }; if (write_mem(VA(m, 0xC3198F), p, 3)) logf("Graphics: all objects cast shadows"); }
        else logf("Graphics: shadow caster site mismatch -> skipped");
    }
}
