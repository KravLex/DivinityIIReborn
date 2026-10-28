/* Div2Fix - skip the startup logo video (Data\Video\intro.wmv).
 *
 * 0xB41BB0 plays intro.wmv and waits until it ends: cmp byte [0x14CEED3],0 (global "play videos" switch,
 * also used for story videos) / je 0xB41C06 (return). Its only caller is 0xB39593. Div2Patch's own
 * PlayIntroVideo switch redirects that call site (0xB39590..0xB39597) to its stub, which still calls
 * 0xB41BB0, so we do not touch the call site: the je inside the function becomes jmp (74 4D -> EB 4D).
 * Story and skill videos are not affected. INI: [Intro] Skip=1.
 */
#include "common.h"

#define VA(m, a) ((m).base + ((DWORD)(a) - 0x400000))

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

void intro_init(Module m)
{
    if (!GetPrivateProfileIntA("Intro", "Skip", 1, g_ini)) { logf("Intro: startup video enabled (ini)"); return; }
    /* B41BB0 cmp byte [14CEED3],0 / je B41C06 / mov ecx,[14FAC0C] / test / je / push 0 / push 0 / push 'intro.wmv' */
    /* the exe can be relocated: compare the opcode bytes and the embedded addresses separately */
    DWORD d = (DWORD)m.base - 0x400000;
    BYTE* p = VA(m, 0xB41BB0);
    if (!same(m, 0xB41BB0, "803d") || !same(m, 0xB41BB6, "00744d8b0d") || !same(m, 0xB41BBF, "85c974436a006a0068") ||
        *(DWORD*)(p + 2) != 0x14CEED3 + d || *(DWORD*)(p + 11) != 0x14FAC0C + d || *(DWORD*)(p + 24) != 0x1362CF4 + d) {
        /* the site may not even be mapped (other exe): only dump bytes that can be read */
        if (readable(p, 9))
            logf("Intro: patch site mismatch -> startup video unchanged (%02X %02X %02X %02X %02X %02X %02X %02X %02X)",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
        else logf("Intro: patch site not mapped -> startup video unchanged");
        return; }
    static const BYTE j = 0xEB;
    if (write_mem(VA(m, 0xB41BB7), &j, 1)) logf("Intro: startup video (intro.wmv) skipped");
}
