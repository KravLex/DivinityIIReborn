/* Div2Fix - camera zoom range (mouse wheel).
 *
 * Mouse-wheel zoom in this setup is Div2Patch's feature: distance = base (human 3.75, dragon 7.0)
 * - level * 0.25, level clamped to [iZoomRangeMin, iZoomRangeMax] = [-10, 10] (constants in Div2Patch.dll
 * .rdata, followed by the wide string "(LC)+"). Div2Fix widens that range in memory at start-up; the
 * Div2Patch.dll file is not changed. Without Div2Patch nothing is done.
 * INI: [Camera] ZoomOutSteps (default 30 = up to +7.5 units, Div2Patch default 10), ZoomInSteps (default 10).
 * ZoomStep (0.25), HumanDistance (3.75, used by Div2Patch only with DynamicCameraZoom=0), DragonDistance (7.0):
 * Div2Patch doubles in .rdata right after its FOV range ints (60, 120); patched in memory the same way.
 */
#include "common.h"

static int g_out, g_in;
static double g_step = 0.25, g_human = 3.75, g_dragon = 7.0;
static double ini_d(const char* key, double def, double lo, double hi)
{
    char v[32]; GetPrivateProfileStringA("Camera", key, "", v, sizeof v, g_ini);
    if (!v[0]) return def;
    double f = 0, div = 1; int frac = 0; const char* p = v;
    while (*p >= '0' && *p <= '9') f = f * 10 + (*p++ - '0');
    if (*p == '.' || *p == ',') { p++; while (*p >= '0' && *p <= '9' && div < 1e6) { f += (*p++ - '0') / (div *= 10); frac = 1; } }
    (void)frac;
    return f < lo ? lo : f > hi ? hi : f;
}

/* Div2Patch.dll is loaded by the game's patched entry code, i.e. after our DllMain: wait for it */
static DWORD WINAPI zoom_thread(void* p)
{
    (void)p;
    HMODULE dp = 0;
    for (int t = 0; t < 600 && !dp; t++) { dp = GetModuleHandleA("Div2Patch.dll"); if (!dp) Sleep(100); }
    if (!dp) { logf("Camera: Div2Patch not loaded within 60 s -> zoom range unchanged"); return 0; }
    Sleep(200);
    int out = g_out, in = g_in;
    BYTE* b = (BYTE*)dp;
    if (!readable(b, 0x40)) return 0;
    DWORD pe = *(DWORD*)(b + 0x3C);
    if (!readable(b + pe, 0x60)) return 0;
    DWORD size = *(DWORD*)(b + pe + 0x50);
    static const BYTE pat[] = { 0xF6,0xFF,0xFF,0xFF, 0x0A,0,0,0, '(',0,'L',0,'C',0,')',0,'+',0 };
    int cnt = 0; BYTE* hit = 0;
    for (DWORD i = 0; i + sizeof pat <= size; i++) {                /* constants are not 4-aligned (offset 0x538B) */
        if ((i & 0xFFF) == 0 && !readable(b + i, 0x1000)) { i += 0x1000 - 1; continue; }   /* page by page */
        if ((i & 0xFFF) > 0x1000 - sizeof pat && !readable(b + i, sizeof pat)) continue;  /* pattern crossing a section edge */
        int ok = 1; for (DWORD k = 0; k < sizeof pat; k++) if (b[i + k] != pat[k]) { ok = 0; break; }
        if (ok) { cnt++; hit = b + i; }
    }
    if (cnt != 1) { logf("Camera: Div2Patch zoom constants not found (%d matches) -> unchanged (image %08X, %u bytes)", cnt, (DWORD)b, size); return 0; }
    /* step / human base / dragon base: doubles 0.25, 3.75, 7.0 right after the FOV range ints 60, 120 */
    static const BYTE pat2[] = { 0x3C,0,0,0, 0x78,0,0,0,  0,0,0,0,0,0,0xD0,0x3F,  0,0,0,0,0,0,0x0E,0x40,  0,0,0,0,0,0,0x1C,0x40 };
    int cnt2 = 0; BYTE* hit2 = 0;
    for (DWORD i = 0; i + sizeof pat2 <= size; i++) {
        if ((i & 0xFFF) == 0 && !readable(b + i, 0x1000)) { i += 0x1000 - 1; continue; }
        if ((i & 0xFFF) > 0x1000 - sizeof pat2 && !readable(b + i, sizeof pat2)) continue;
        int ok = 1; for (DWORD k = 0; k < sizeof pat2; k++) if (b[i + k] != pat2[k]) { ok = 0; break; }
        if (ok) { cnt2++; hit2 = b + i; }
    }
    double step = 0.25;
    if (cnt2 == 1) {
        double d[3]; d[0] = g_step; d[1] = g_human; d[2] = g_dragon;
        if (d[0] == 0.25 && d[1] == 3.75 && d[2] == 7.0) logf("Camera: step/distance constants found, left at Div2Patch defaults");
        else {
            if (write_mem(hit2 + 8, d, 24)) { step = g_step;
                logf("Camera: zoom step %d.%03d, default distance human %d.%02d (only with DynamicCameraZoom=0), dragon %d.%02d",
                     (int)g_step, (int)(g_step * 1000) % 1000, (int)g_human, (int)(g_human * 100) % 100, (int)g_dragon, (int)(g_dragon * 100) % 100); }
        }
    } else if (g_step != 0.25 || g_human != 3.75 || g_dragon != 7.0)
        logf("Camera: Div2Patch step/distance constants not found (%d matches) -> unchanged", cnt2);
    { int maxin = (int)((g_human - 0.25) / step); if (in > maxin) in = maxin; if (in < 1) in = 1; }   /* never through the hero */
    int v[2] = { -out, in };
    if (write_mem(hit, v, 8)) { double mx = out * step;
        logf("Camera: zoom range %d..%d steps (Div2Patch default -10..10), max extra distance %d.%02d", -out, in, (int)mx, (int)(mx * 100) % 100); }
    return 0;
}

void camera_init(Module m)
{
    (void)m;
    if (!GetPrivateProfileIntA("Camera", "Enable", 1, g_ini)) { logf("Camera: disabled in ini"); return; }
    g_out = GetPrivateProfileIntA("Camera", "ZoomOutSteps", 30, g_ini);
    g_in = GetPrivateProfileIntA("Camera", "ZoomInSteps", 10, g_ini);
    if (g_out < 1) g_out = 1; if (g_out > 200) g_out = 200;
    if (g_in < 1) g_in = 1; if (g_in > 14) g_in = 14;      /* 14 * 0.25 = 3.5 < 3.75 base: never through the hero */
    g_step = ini_d("ZoomStep", 0.25, 0.05, 2.0);
    g_human = ini_d("HumanDistance", 3.75, 1.5, 15.0);
    g_dragon = ini_d("DragonDistance", 7.0, 3.0, 30.0);
    HANDLE th = CreateThread(0, 0, zoom_thread, 0, 0, 0);
    if (th) CloseHandle(th); else logf("Camera: cannot start helper thread");
}
