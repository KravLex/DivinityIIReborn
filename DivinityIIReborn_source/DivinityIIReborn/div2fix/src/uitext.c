/* Div2Fix - menu text encoding fix (boxes instead of letters).
 *
 * The game passes values to its Scaleform movies through its own value type; 0x9B3550 copies one into a
 * GFxValue: type 1 (char*) -> GFx string (UTF-8!), type 2 (wchar_t*) -> GFx wide string. Some game code
 * passes ANSI text (Windows-1251 for Russian) as type 1; GFx decodes it as UTF-8 and every letter becomes
 * a "missing glyph" box (seen above user flags on the world map). Hook the type-1 branch (0x9B355F:
 * mov eax,[ecx] / mov ecx,[esp+8] / push eax / call 0x9A24B0) and, only for strings that are NOT valid
 * UTF-8, convert them from the game language's ANSI code page to UTF-8 before GFx copies them.
 * ASCII and valid UTF-8 strings pass unchanged. INI: [UIText] Enable=1, Log=1 (first 30 conversions).
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
static int g_cp, g_utlog, g_n;
static char g_out[4][2048];
static unsigned short g_w[1024];
static int g_slot;

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

static int valid_utf8(const BYTE* s, int* hi)
{
    *hi = 0;
    while (*s) {
        BYTE c = *s;
        if (c < 0x80) { s++; continue; }
        *hi = 1;
        int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || c == 0xC0 || c == 0xC1) return 0;
        for (int k = 1; k <= n; k++) if ((s[k] & 0xC0) != 0x80) return 0;
        s += n + 1;
    }
    return 1;
}

/* wide string made of widened bytes (all chars < 0x100, some >= 0x80): rebuild the real text */
static unsigned short g_wo[4][1024]; static int g_wslot, g_wn;
static const unsigned short* __cdecl ut_wfix(const unsigned short* w)
{
    if (!w || !readable(w, 2)) return w;
    int n = 0, hi = 0, c0 = 0;
    /* bytes widened as signed char come out as 0xFF80..0xFFFF (sign-extended), unsigned ones as 0x80..0xFF */
    while (n < 1000 && readable(w + n, 2) && w[n]) {
        unsigned short ch = w[n];
        if (ch >= 0x100 && ch < 0xFF80) return w;
        if ((ch & 0xFF) >= 0x80) { hi++; if ((ch & 0xFF) >= 0xC0) c0++; }
        n++; }
    if (!hi || n >= 1000) return w;
    char bytes[1001]; for (int i = 0; i < n; i++) bytes[i] = (char)(w[i] & 0xFF); bytes[n] = 0;
    int h2; int cp = 0;
    if (valid_utf8((const BYTE*)bytes, &h2)) cp = 65001;
    else if (g_cp == 1251 && c0 * 2 >= hi) cp = 1251;          /* Cyrillic letters are 0xC0..0xFF in 1251 */
    if (!cp) return w;
    unsigned short* o = g_wo[g_wslot++ & 3];
    int on = MultiByteToWideChar(cp, 0, bytes, n, o, 1023);
    if (on <= 0) return w;
    o[on] = 0;
    if (g_utlog && g_wn < 30) { g_wn++; char u[1200]; int un = WideCharToMultiByte(65001, 0, o, on, u, sizeof u - 1, 0, 0); if (un < 0) un = 0; u[un] = 0;
        logf("UIText: rebuilt widened text (%s, %d chars): %.200s", cp == 65001 ? "UTF-8" : "1251", n, u); }
    return o;
}
static const char* __cdecl ut_fix(const char* s)
{
    if (!s || !readable(s, 1)) return s;
    int hi; if (valid_utf8((const BYTE*)s, &hi) || !hi) return s;
    int len = 0; while (len < 1000 && readable(s + len, 1) && s[len]) len++;
    if (len >= 1000) return s;
    int wn = MultiByteToWideChar(g_cp, 0, s, len, g_w, 1023);
    if (wn <= 0) return s;
    char* o = g_out[g_slot++ & 3];
    int on = WideCharToMultiByte(65001, 0, g_w, wn, o, sizeof g_out[0] - 1, 0, 0);
    if (on <= 0) return s;
    o[on] = 0;
    if (g_utlog && g_n < 30) { g_n++; logf("UIText: converted ANSI text (%d bytes, code page %d): %.120s", len, g_cp, o); }
    return o;
}

void uitext_init(Module m)
{
    if (!GetPrivateProfileIntA("UIText", "Enable", 1, g_ini)) { logf("UIText: disabled in ini"); return; }
    g_utlog = GetPrivateProfileIntA("UIText", "Log", 1, g_ini);
    char lang[16]; char li[MAX_PATH]; wsprintfA(li, "%s..\\lang.ini", g_dir);
    GetPrivateProfileStringA("general", "Language", "en", lang, sizeof lang, li);
    char a = lc(lang[0]), b = lc(lang[1]);
    g_cp = (a == 'r' && b == 'u') ? 1251 : (a == 'p' && b == 'l') ? 1250 : (a == 'j' && b == 'p') ? 932 : 1252;
    if (!same(m, 0x9B355F, "8b018b4c240850e8") || !same(m, 0x9B3550, "8b411483f803") || !same(m, 0x9A24B0, "538b5c2408558be9")) {
        logf("UIText: hook site mismatch -> off"); return; }
    BYTE* cave = (BYTE*)VirtualAlloc(0, 160, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return;
    BYTE* c = cave;
    *c++ = 0x8B; *c++ = 0x01;                                                    /* mov eax,[ecx]          */
    *c++ = 0x60; *c++ = 0x9C;                                                    /* pushad / pushfd        */
    *c++ = 0x50;                                                                 /* push eax               */
    *c++ = 0xE8; *(DWORD*)c = (DWORD)((BYTE*)ut_fix - (c + 4)); c += 4;          /* call ut_fix            */
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;                                       /* add esp,4              */
    *c++ = 0x89; *c++ = 0x44; *c++ = 0x24; *c++ = 0x20;                          /* mov [esp+20h],eax (saved eax) */
    *c++ = 0x9D; *c++ = 0x61;                                                    /* popfd / popad          */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x08;                          /* mov ecx,[esp+8]        */
    *c++ = 0x50;                                                                 /* push eax               */
    *c++ = 0xE8; *(DWORD*)c = (DWORD)(VA(m, 0x9A24B0) - (c + 4)); c += 4;        /* call 9A24B0            */
    *c++ = 0xE9; *(DWORD*)c = (DWORD)(VA(m, 0x9B356B) - (c + 4)); c += 4;        /* jmp 9B356B             */
    BYTE j[12] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(DWORD*)(j + 1) = (DWORD)(cave - (VA(m, 0x9B355F) + 5));
    if (write_mem(VA(m, 0x9B355F), j, 12)) logf("UIText: ANSI->UTF-8 fix for menu text active (code page %d)", g_cp);
    /* wide strings (type 2): 9B357B mov ecx,[ecx+4] / push ecx / mov ecx,[esp+0Ch] / call 9A2510 / 9B3588 */
    if (same(m, 0x9B357B, "8b4904518b4c240ce8") && same(m, 0x9A2510, "538b5c2408558be9")) {
        BYTE* c2 = c;
        *c++ = 0x8B; *c++ = 0x49; *c++ = 0x04;                                   /* mov ecx,[ecx+4]          */
        *c++ = 0x60; *c++ = 0x9C; *c++ = 0x51;                                   /* pushad / pushfd / push ecx */
        *c++ = 0xE8; *(DWORD*)c = (DWORD)((BYTE*)ut_wfix - (c + 4)); c += 4;     /* call ut_wfix             */
        *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;                                   /* add esp,4                */
        *c++ = 0x89; *c++ = 0x44; *c++ = 0x24; *c++ = 0x1C;                      /* mov [esp+1Ch],eax (saved ecx) */
        *c++ = 0x9D; *c++ = 0x61;                                                /* popfd / popad            */
        *c++ = 0x51; *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x0C;         /* push ecx / mov ecx,[esp+0Ch] */
        *c++ = 0xE8; *(DWORD*)c = (DWORD)(VA(m, 0x9A2510) - (c + 4)); c += 4;    /* call 9A2510              */
        *c++ = 0xE9; *(DWORD*)c = (DWORD)(VA(m, 0x9B3588) - (c + 4)); c += 4;    /* jmp 9B3588               */
        BYTE j2[13] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        *(DWORD*)(j2 + 1) = (DWORD)(c2 - (VA(m, 0x9B357B) + 5));
        if (write_mem(VA(m, 0x9B357B), j2, 13)) logf("UIText: widened-bytes fix for wide menu text active");
    } else logf("UIText: wide-string site mismatch -> that part off");
}
