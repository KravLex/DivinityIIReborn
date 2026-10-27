/* Div2Fix - HD icons: draw upscaled menu icons at their original on-screen size.
 *
 * Menu icons ("img://itm/...", "ach/...", "skl/...", markers, cursor) come from the game's own GFx image class
 * (vtable 0x13C6494 static). Its GetWidth/GetHeight (0xF124B0 / 0xF124F0) return the stored size at +0x20/+0x24
 * (the icon's original size) when a Gamebryo texture is attached at +0x28 - that is what the menus lay out.
 * Its GetTexture (0xF12920) wraps the D3D texture for Scaleform: GTexture::InitTexture(d3dtex, w, h) with
 * w/h = NiTexture GetWidth/GetHeight (vtable +0x48/+0x4C) = the REAL texture size. With the HD texture pack
 * the real size is 4x the stored size, Scaleform maps texture pixels 1:1 and only the top-left quarter shows.
 * Fix: pass the stored size (+0x20/+0x24) instead, i.e. the same size the menu uses; for unmodified
 * textures both are equal, so nothing changes there. Four in-place patches, every byte verified first:
 *   F12972 mov eax,[edx+4Ch]; mov ecx,edi; call eax -> mov eax,[esi+24h]  (height)
 *   F1297C mov eax,[edx+48h]; mov ecx,edi; call eax -> mov eax,[esi+20h]  (width)
 *   F129CD mov ecx,edi; call eax                    -> mov eax,[esi+24h]  (second path, height)
 *   F129D4 mov eax,[edx+48h]; mov ecx,edi; call eax -> mov eax,[esi+20h]  (second path, width)
 * INI: [Icons] Enable=1.
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

void icons_init(Module m)
{
    if (!GetPrivateProfileIntA("Icons", "Enable", 1, g_ini)) { logf("Icons: disabled in ini"); return; }
    if (!same(m, 0xF12972, "8b424c8bcfffd08b17508b42488bcfffd0") ||      /* F12972..F12982 */
        !same(m, 0xF129C6, "8b424c894c24588bcfffd08b17508b42488bcfffd0") ||  /* F129C6..F129DA */
        !same(m, 0xF124B0, "8bc18378280074048b4020c3") ||               /* GetWidth: [+28] ? [+20] */
        !same(m, 0xF124F0, "8bc18378280074048b4024c3")) {               /* GetHeight: [+28] ? [+24] */
        logf("Icons: patch site mismatch -> module off"); return; }
    static const BYTE h7[7] = { 0x8B, 0x46, 0x24, 0x90, 0x90, 0x90, 0x90 };   /* mov eax,[esi+24h] */
    static const BYTE w7[7] = { 0x8B, 0x46, 0x20, 0x90, 0x90, 0x90, 0x90 };   /* mov eax,[esi+20h] */
    static const BYTE h4[4] = { 0x8B, 0x46, 0x24, 0x90 };
    int ok = write_mem(VA(m, 0xF12972), h7, 7) && write_mem(VA(m, 0xF1297C), w7, 7)
          && write_mem(VA(m, 0xF129CD), h4, 4) && write_mem(VA(m, 0xF129D4), w7, 7);
    logf(ok ? "Icons: HD icons drawn at original size (4 patches)" : "Icons: write failed");
}
