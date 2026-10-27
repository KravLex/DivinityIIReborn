/*
 * Div2Fix — minimal x86 inline detour.
 * Copies >= 5 bytes of whole instructions from the target into an executable trampoline
 * (relocating rel32 JMP/CALL), then writes JMP rel32 to the hook at the target.
 * The length decoder only accepts instruction forms commonly found in function prologues
 * and hook stubs; anything else makes the detour refuse (returns 0) instead of guessing.
 */
#include "common.h"

/* length of one instruction at p, or 0 if not understood. *rel = offset of rel32 to relocate (or -1) */
static int insn_len(const BYTE* p, int* rel)
{
    int n = 0, opsize = 0; *rel = -1;
    for (;;) {                                   /* prefixes */
        BYTE b = p[n];
        if (b == 0x66) { opsize = 1; n++; continue; }
        if (b == 0x64 || b == 0x65 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0xF2 || b == 0xF3) { n++; continue; }
        break;
    }
    BYTE op = p[n++];
    int hasModrm = 0, imm = 0;
    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x90: case 0xC3: case 0xCC: case 0x9C: case 0x9D: case 0x60: case 0x61:
        return n;
    case 0x6A: return n + 1;                     /* push imm8 */
    case 0x68: return n + 4;                     /* push imm32 */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        return n + (opsize ? 2 : 4);             /* mov reg, imm */
    case 0xA1: case 0xA3: return n + 4;          /* mov eax,[moffs] / mov [moffs],eax */
    case 0xE8: case 0xE9: *rel = n; return n + 4;/* call/jmp rel32 */
    case 0x89: case 0x8B: case 0x8D: case 0x31: case 0x33: case 0x01: case 0x03: case 0x29: case 0x2B:
    case 0x85: case 0x39: case 0x3B: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x87:
        hasModrm = 1; break;
    case 0x83: hasModrm = 1; imm = 1; break;
    case 0x81: case 0xC7: hasModrm = 1; imm = opsize ? 2 : 4; break;
    case 0xFF: hasModrm = 1; break;              /* inc/dec/call/jmp/push r/m (absolute) */
    case 0x0F: {
        BYTE op2 = p[n++];
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) { hasModrm = 1; break; } /* movzx/movsx */
        return 0;
    }
    default: return 0;
    }
    if (hasModrm) {
        BYTE m = p[n++]; int mod = m >> 6, rm = m & 7;
        if (op == 0xFF) { int reg = (m >> 3) & 7; if (reg == 7) return 0; }
        if (mod != 3 && rm == 4) {               /* SIB */
            BYTE sib = p[n++];
            if (mod == 0 && (sib & 7) == 5) n += 4;
        }
        if (mod == 0 && rm == 5) n += 4;         /* [disp32] (absolute on x86) */
        else if (mod == 1) n += 1;
        else if (mod == 2) n += 4;
        n += imm;
    }
    return n;
}

/* Install detour. Returns trampoline (call it as the original), or 0 on refusal. */
void* detour(void* target, void* hook)
{
    BYTE* t = (BYTE*)target;
    if (!readable(t, 16)) return 0;
    int total = 0, rels[8], nrel = 0;
    while (total < 5) {
        int rel; int l = insn_len(t + total, &rel);
        if (!l || nrel >= 8) { logf("  detour %08X refused: unknown instruction %02X %02X %02X at +%d", (DWORD)t, t[total], t[total + 1], t[total + 2], total); return 0; }
        if (rel >= 0) rels[nrel++] = total + rel;
        total += l;
    }
    BYTE* tr = (BYTE*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, t, (SIZE_T)total);
    for (int i = 0; i < nrel; i++) {             /* relocate rel32 */
        int o = rels[i];
        long dst = (long)(t + o + 4) + *(long*)(t + o);
        *(long*)(tr + o) = dst - (long)(tr + o + 4);
    }
    tr[total] = 0xE9; *(long*)(tr + total + 1) = (long)(t + total) - (long)(tr + total + 5);
    BYTE patch[16]; patch[0] = 0xE9; *(long*)(patch + 1) = (long)hook - (long)(t + 5);
    for (int i = 5; i < total; i++) patch[i] = 0x90;
    if (!write_mem(t, patch, (SIZE_T)total)) { VirtualFree(tr, 0, MEM_RELEASE); return 0; }
    logf("  detour %08X -> %08X (stole %d bytes, trampoline %08X)", (DWORD)t, (DWORD)hook, total, (DWORD)tr);
    return tr;
}
