/* Div2Fix - mind-read advisor: adds a spoiler-free verdict line to the game's mind-read confirmation popup.
 * Nothing is written to the game or to saves; the cost and rewards stay as in the original.
 *
 * When the game builds the "Mindread costs [1] xp" confirmation (mindreadPressed handler 0x936DA0), right after
 * it computed the cost (call 0x75DB20 at 0x936E19, result in eax, player in edi) we log:
 *   - the current dialog (same getter the handler itself calls: 0x702640 on [0x14F13EC]),
 *   - its name (std::string at dialog+0x8C, filled from the dialog's "name" attribute by the loader at 0xA8AE84),
 *   - mindreadlvl (dialog+0x08, atoi of attribute "mindreadlvl" at 0xA8AF1A..0xA8AF3E),
 *   - the cost the game computed and the player float at +0x14 used as a multiplier (TODO: what it is),
 *   - the category from Div2Fix_mindread.txt (built from game data by tools/mindread/mr_build.py).
 * Every patched byte is verified first; on mismatch the module stays off and says so in the log.
 * INI: [MindRead] Enable=1, Dump=0 (1 = hex dump of the dialog object once per dialog, for the TODO items).
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))

static Module g_m;
typedef unsigned short WCHAR;
static int g_dump, g_show, g_pend, g_lang;              /* g_pend: 0 none, 1 green, 2 yellow, 3 gray, 4 unknown */
static WCHAR g_text[1024];
/* advisor line per language (index = g_lang) and category */
static const WCHAR* const HINT[][4] = {
    { L"Советник: Можно узнать что-то полезное", L"Советник: Можно узнать кое-что, но немного", L"Советник: Похоже, тут только мысли", L"Советник: Неизвестно, что это даст" },
    { L"Advisor: You may learn something useful", L"Advisor: You may learn something, but not much", L"Advisor: Looks like just thoughts here", L"Advisor: Unknown what this will give" },
    { L"Berater: Hier lässt sich etwas Nützliches erfahren", L"Berater: Etwas zu erfahren, aber nicht viel", L"Berater: Scheinbar nur Gedanken", L"Berater: Unklar, was das bringt" },
    { L"Conseiller : vous pourriez apprendre quelque chose d'utile", L"Conseiller : quelque chose à apprendre, mais peu", L"Conseiller : on dirait de simples pensées", L"Conseiller : impossible de dire ce que cela apportera" },
    { L"Consejero: puedes descubrir algo útil", L"Consejero: puedes descubrir algo, pero poco", L"Consejero: parece que solo hay pensamientos", L"Consejero: no se sabe qué aportará" },
    { L"Consigliere: potresti scoprire qualcosa di utile", L"Consigliere: qualcosa da scoprire, ma poco", L"Consigliere: sembrano solo pensieri", L"Consigliere: non si sa cosa porterà" },
    { L"Doradca: można dowiedzieć się czegoś przydatnego", L"Doradca: można coś odkryć, ale niewiele", L"Doradca: wygląda na same myśli", L"Doradca: nie wiadomo, co to da" },
};
static char* g_tab;                 /* table file contents (NUL-terminated lines) */
static DWORD g_tabLen;
typedef void* (__thiscall* GetDlgFn)(void* self);

static int same(Module m, DWORD va, const char* hex)
{
    BYTE* p = VA(m, va); int n = s_len(hex) / 2;
    if (!readable(p, (SIZE_T)n)) return 0;
    for (int i = 0; i < n; i++) {
        int hi = hex[2*i], lo = hex[2*i+1];
        hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10; lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
        if (p[i] != (BYTE)(hi * 16 + lo)) return 0;
    }
    return 1;
}

/* MSVC9 std::string somewhere in [p, p+8): try the two layouts (with/without a leading allocator word) */
static int read_std_string(const BYTE* p, char* out, int cap)
{
    out[0] = 0;
    for (int k = 0; k <= 4; k += 4) {
        const BYTE* s = p + k;
        if (!readable(s, 24)) return 0;
        DWORD size = *(const DWORD*)(s + 16), res = *(const DWORD*)(s + 20);
        if (res < 15 || res > 4096 || size == 0 || size > res || size >= (DWORD)cap) continue;
        const char* t = res < 16 ? (const char*)s : *(const char* const*)s;
        if (!readable(t, size)) continue;
        int ok = 1; for (DWORD i = 0; i < size; i++) if (t[i] < 32 || t[i] > 126) { ok = 0; break; }
        if (!ok) continue;
        memcpy(out, t, size); out[size] = 0; return k + 1;
    }
    return 0;
}

static int s_ieq(const char* a, const char* b, int n)   /* first n chars of a == b (b NUL-terminated, length n) */
{
    for (int i = 0; i < n; i++) if (lc(a[i]) != lc(b[i]) || !b[i]) return 0;
    return b[n] == 0;
}

/* find "name<TAB>file<TAB>ep<TAB>lvl<TAB>cat<TAB>rewards" by name (+lvl when names repeat across episodes) */
static const char* lookup(const char* name, int lvl, int* outLen)
{
    const char* best = 0; int bestLen = 0;
    for (DWORD i = 0; i < g_tabLen; ) {
        const char* l = g_tab + i; int n = 0; while (i + n < g_tabLen && l[n] != '\n' && l[n] != '\r') n++;
        i += n; while (i < g_tabLen && (g_tab[i] == '\n' || g_tab[i] == '\r')) i++;
        if (!n || l[0] == '#') continue;
        int f0 = 0; while (f0 < n && l[f0] != '\t') f0++;
        if (!s_ieq(l, name, f0)) continue;
        int t = 0, c = 0; for (; c < n && t < 3; c++) if (l[c] == '\t') t++;          /* -> lvl field */
        int v = 0, neg = 0; if (c < n && l[c] == '-') { neg = 1; c++; } while (c < n && l[c] >= '0' && l[c] <= '9') v = v * 10 + (l[c++] - '0');
        if (neg) v = -v;
        if (!best || v == lvl) { best = l; bestLen = n; if (v == lvl) break; }
    }
    *outLen = bestLen; return best;
}

static void load_table(void)
{
    char path[MAX_PATH]; wsprintfA(path, "%sDiv2Fix_mindread.txt", g_dir);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { logf("MindRead: Div2Fix_mindread.txt not found -> categories unknown"); return; }
    DWORD sz = GetFileSize(h, 0);
    if (sz && sz < (4u << 20)) {
        g_tab = (char*)VirtualAlloc(0, sz + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD r = 0; if (g_tab && ReadFile(h, g_tab, sz, &r, 0)) g_tabLen = r;
    }
    CloseHandle(h);
    int lines = 0; for (DWORD i = 0; i < g_tabLen; i++) if (g_tab[i] == '\n') lines++;
    logf("MindRead: table loaded, %d lines", lines);
}

#define MAXSEEN 64
static void* g_seen[MAXSEEN]; static int g_nseen;

/* called from the cave: cost = value the game will show, player = edi */
void __cdecl mr_on_cost(int cost, BYTE* player)
{
    void** pmgr = (void**)VA(g_m, 0x14F13EC);
    if (!readable(pmgr, 4) || !*pmgr) { logf("MindRead: no dialog manager"); return; }
    BYTE* dlg = (BYTE*)((GetDlgFn)VA(g_m, 0x702640))(*pmgr);
    if (!dlg || !readable(dlg, 0x190)) { logf("MindRead: no current dialog"); return; }
    int lvl = *(int*)(dlg + 8);
    char name[96]; int lay = read_std_string(dlg + 0x8C, name, sizeof name);
    int mul100 = -1;
    if (player && readable(player + 0x14, 4)) { float f = *(float*)(player + 0x14); mul100 = (int)(f * 100.0f + 0.5f); }
    char row[200] = "(not in table)"; int rl = 0; const char* r = lay ? lookup(name, lvl, &rl) : 0;
    if (r) { if (rl > 199) rl = 199; memcpy(row, r, (SIZE_T)rl); row[rl] = 0; for (int i = 0; i < rl; i++) if (row[i] == '\t') row[i] = '|'; }
    g_pend = 4;                                            /* unknown unless the table says otherwise */
    if (r) { int t = 0, c = 0; for (; c < rl && t < 4; c++) if (r[c] == '\t') t++;
             if (c < rl) g_pend = r[c] == 'G' && r[c+1] == 'R' && r[c+2] == 'E' ? 1 : r[c] == 'Y' ? 2 : r[c] == 'G' ? 3 : 4; }
    logf("MindRead: dialog '%s' (layout %d, obj %08X) mindreadlvl=%d  game cost=%d  player[+14]=%d.%02d  formula lvl*int(p14)=%d  -> %s",
         lay ? name : "?", lay, (DWORD)dlg, lvl, cost, mul100 / 100, mul100 % 100, mul100 >= 0 ? lvl * (mul100 / 100) : -1, row);
    if (g_dump) {
        int seen = 0; for (int i = 0; i < g_nseen; i++) if (g_seen[i] == dlg) seen = 1;
        if (!seen && g_nseen < MAXSEEN) {
            g_seen[g_nseen++] = dlg;
            for (int o = 0; o < 0x190; o += 32) {
                char b[140]; int n = wsprintfA(b, "  +%03X:", o);
                for (int j = 0; j < 32; j += 4) n += wsprintfA(b + n, " %08X", *(DWORD*)(dlg + o + j));
                logf("%s", b);
            }
        } else if (seen) logf("  (dialog object already dumped this session; compare mindreadlvl/bytes after a read)");
    }
}

/* called after the game produced the final popup text (wchar_t* in *holder): append the advisor line */
void __cdecl mr_on_text(const WCHAR** holder)
{
    int cat = g_pend; g_pend = 0;
    if (!g_show || !cat || !holder || !readable(holder, 4) || !*holder) return;
    const WCHAR* src = *holder; int n = 0;
    while (n < 900 && readable(src + n, 2) && src[n]) n++;
    if (n >= 900) return;
    for (int i = 0; i < n; i++) g_text[i] = src[i];
    g_text[n++] = L'\n';
    const WCHAR* h = HINT[g_lang][cat - 1];
    for (int i = 0; h[i] && n < 1020; i++) g_text[n++] = h[i];
    g_text[n] = 0;
    *holder = g_text;
}

void mindread_init(Module m)
{
    g_m = m;
    if (!GetPrivateProfileIntA("MindRead", "Enable", 0, g_ini)) { logf("MindRead: disabled in ini"); return; }
    g_dump = GetPrivateProfileIntA("MindRead", "Dump", 0, g_ini);
    /* 936E19: call 75DB20 / 936E1E: mov [esp+14h],eax / push 1 / lea eax,[esp+0Ch] */
    if (!same(m, 0x936E1E, "894424146a01") || !same(m, 0x936E19, "e8")) { logf("MindRead: hook site mismatch -> module off"); return; }
    BYTE* cave = (BYTE*)VirtualAlloc(0, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return;
    BYTE* c = cave;
    *c++ = 0x60; *c++ = 0x9C;                                                    /* pushad / pushfd          */
    *c++ = 0x57; *c++ = 0x50;                                                    /* push edi / push eax      */
    *c++ = 0xE8; *(DWORD*)c = (DWORD)((BYTE*)mr_on_cost - (c + 4)); c += 4;      /* call mr_on_cost          */
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;                                       /* add esp,8                */
    *c++ = 0x9D; *c++ = 0x61;                                                    /* popfd / popad            */
    *c++ = 0x89; *c++ = 0x44; *c++ = 0x24; *c++ = 0x14;                          /* mov [esp+14h],eax        */
    *c++ = 0x6A; *c++ = 0x01;                                                    /* push 1                   */
    *c++ = 0xE9; *(DWORD*)c = (DWORD)(VA(m, 0x936E24) - (c + 4)); c += 4;        /* jmp 936E24               */
    BYTE j[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(DWORD*)(j + 1) = (DWORD)(cave - (VA(m, 0x936E1E) + 5));
    load_table();
    char lang[16]; char li[MAX_PATH]; wsprintfA(li, "%s..\\lang.ini", g_dir);
    GetPrivateProfileStringA("general", "Language", "en", lang, sizeof lang, li);
    static const char* langs[] = { "ru", "en", "de", "fr", "es", "it", "pl", 0 };
    g_lang = 1; for (int i = 0; langs[i]; i++) if (lc(lang[0]) == langs[i][0] && lc(lang[1]) == langs[i][1]) g_lang = i;
    g_show = GetPrivateProfileIntA("MindRead", "Show", 1, g_ini);
    if (!write_mem(VA(m, 0x936E1E), j, 6)) return;
    logf("MindRead: cost hook active (game language '%s')", lang);
    if (!g_show) { logf("MindRead: Show=0 -> log only"); return; }
    /* 936E9F: call 8E57F0 (holder = c_str of the formatted text, holder at [esp+0Ch])
       936EA4: mov edx,[esi] / 936EA6: mov edx,[edx+90h] / 936EAC: push 1 ... call edx (show popup) */
    if (!same(m, 0x936E9F, "e8") || !same(m, 0x936EA4, "8b168b9290000000") || !same(m, 0x936E9B, "8d4c2410")) { logf("MindRead: text hook site mismatch -> advisor line off (log still on)"); return; }
    BYTE* cv = c;                                                                /* second cave right after the first */
    *c++ = 0x60; *c++ = 0x9C;                                                    /* pushad / pushfd          */
    *c++ = 0x8D; *c++ = 0x44; *c++ = 0x24; *c++ = 0x30;                          /* lea eax,[esp+30h] = holder */
    *c++ = 0x50;                                                                 /* push eax                 */
    *c++ = 0xE8; *(DWORD*)c = (DWORD)((BYTE*)mr_on_text - (c + 4)); c += 4;      /* call mr_on_text          */
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;                                       /* add esp,4                */
    *c++ = 0x9D; *c++ = 0x61;                                                    /* popfd / popad            */
    *c++ = 0x8B; *c++ = 0x16;                                                    /* mov edx,[esi]            */
    *c++ = 0x8B; *c++ = 0x92; *c++ = 0x90; *c++ = 0; *c++ = 0; *c++ = 0;         /* mov edx,[edx+90h]        */
    *c++ = 0xE9; *(DWORD*)c = (DWORD)(VA(m, 0x936EAC) - (c + 4)); c += 4;        /* jmp 936EAC               */
    BYTE j2[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(DWORD*)(j2 + 1) = (DWORD)(cv - (VA(m, 0x936EA4) + 5));
    if (write_mem(VA(m, 0x936EA4), j2, 8)) logf("MindRead: advisor line in the confirmation popup active");
}
