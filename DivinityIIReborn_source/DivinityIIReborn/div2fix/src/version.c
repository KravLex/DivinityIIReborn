/* Div2Fix - game version check.
 * Every Div2Fix patch was made for Divinity2.exe v1.4.700.56 (the "Y-axis hotfix" build that Div2Patch installs;
 * the plain Steam build is v1.4.700.49). Each patch verifies its bytes anyway and switches itself off on another
 * build, so nothing breaks - but the player would silently lose most features. Tell them once at start-up.
 * The version string is at 0x1476B70 in v.56 (0x1476B20 in Steam v.49, 0x1476AA0 in GOG v.38).
 * INI: [General] VersionWarning=1.
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))
typedef unsigned short WCHAR_T;
static WCHAR_T g_msg[1024];
static int str_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int is_ver(Module m, DWORD va, char* out)
{
    const char* s = (const char*)VA(m, va);
    if (!readable(s, 16)) return 0;
    if (s[0] != 'v' || s[1] != '1' || s[2] != '.' || s[3] != '4' || s[4] != '.') return 0;
    int i = 0; for (; i < 15 && s[i] > 32 && s[i] < 127; i++) out[i] = s[i]; out[i] = 0;
    return 1;
}
static const WCHAR_T k_title[] = { 'D','i','v','i','n','i','t','y',' ','I','I',' ','R','e','b','o','r','n',0 };
static DWORD WINAPI msg_thread(void* p)
{
    (void)p;
    MessageBoxW(0, (const WCHAR_T*)g_msg, k_title, 0x30 /*MB_ICONWARNING*/ | 0x40000 /*MB_TOPMOST*/ | 0x10000 /*MB_SETFOREGROUND*/);
    return 0;
}
int version_check(Module m)
{
    char v[16] = "";
    int force = GetPrivateProfileIntA("General", "VersionWarningTest", 0, g_ini);   /* 1 = show the warning anyway (testing) */
    if (is_ver(m, 0x1476B70, v) && v[0] && str_eq(v, "v1.4.700.56") && !force) { logf("Game version %s (Div2Patch build): OK", v); return 1; }
    if (!v[0] && !is_ver(m, 0x1476B20, v) && !is_ver(m, 0x1476AA0, v) && !is_ver(m, 0x147EA50, v)) { v[0] = '?'; v[1] = 0; }
    logf("Game version %s: Div2Fix needs v1.4.700.56 (installed by Div2Patch) - most features will switch themselves off", v);
    if (!GetPrivateProfileIntA("General", "VersionWarning", 1, g_ini)) return 0;
    char lang[16]; char li[MAX_PATH]; wsprintfA(li, "%s..\\lang.ini", g_dir);
    GetPrivateProfileStringA("general", "Language", "en", lang, sizeof lang, li);
    int ru = (lc(lang[0]) == 'r' && lc(lang[1]) == 'u');
    const char* txt = ru
        ? "Для Divinity II Reborn нужен Div2Patch.\n\nНайдена версия игры %s, а мод рассчитан на v1.4.700.56, которую устанавливает Div2Patch. "
          "Большинство улучшений мода сейчас отключено (игра работает как обычно).\n\n"
          "Установите Div2Patch: https://github.com/BlanknameES/Div2Patch\nи запустите игру снова."
        : "Divinity II Reborn requires Div2Patch.\n\nFound game version %s, but the mod is made for v1.4.700.56, which Div2Patch installs. "
          "Most of the mod's features are switched off for now (the game itself runs normally).\n\n"
          "Install Div2Patch: https://github.com/BlanknameES/Div2Patch\nand start the game again.";
    char a[1024]; wsprintfA(a, txt, v);
    int n = MultiByteToWideChar(65001, 0, a, -1, (WCHAR_T*)g_msg, 1023); if (n <= 0) return 0;
    HANDLE th = CreateThread(0, 0, msg_thread, 0, 0, 0); if (th) CloseHandle(th);
    return 0;
}
