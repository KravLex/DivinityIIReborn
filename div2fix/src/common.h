#pragma once
#include "win.h"
typedef struct { BYTE* base; DWORD size; } Module;
extern char g_dir[MAX_PATH], g_ini[MAX_PATH], g_log[MAX_PATH];
extern int g_logOn;
void logf(const char* fmt, ...);
int readable(const void* p, SIZE_T n);
int write_mem(void* dst, const void* src, SIZE_T n);
Module main_module(void);
BYTE* find_pattern(Module m, const BYTE* pat, const char* mask, int* count);
int s_len(const char* s);
char lc(char c);
void* __cdecl memcpy(void* d, const void* s, SIZE_T n);
void* __cdecl memset(void* d, int c, SIZE_T n);
/* diagnostics module */
void diag_init(Module m);
void gfx_init(Module m);
/* parallel texture-level copy (parcopy.c) */
void parcopy_init(Module m);
void crashguard_init(Module m);
void crashguard_exit(void);
/* address space of the process in MB (diag.c): used, free, largest free block, private bytes */
typedef struct { DWORD usedMB, freeMB, largestMB, privMB; } AddrSpace;
void addr_space(AddrSpace* a);
int parcopy_pool(int threads);
void parcopy_run(void* dst, const void* src, DWORD n, int workers);
int __cdecl parcopy_memcpy_s(void* dst, SIZE_T dstSize, const void* src, SIZE_T count);
void parcopy_stats(DWORD* parCalls, DWORD* serCalls, DWORD* parMB);
void mindread_init(Module m);
void camera_init(Module m);
void icons_init(Module m);
void intro_init(Module m);
void overlay_init(Module m);
void skilltrace_init(Module m);   /* Mage Skills Expansion: skill registration trace ([Diagnostics] SkillTrace=1) */
void mageskills_init(Module m);   /* Mage Skills Expansion: gives the hero the new skills ([MageSkills] Enable=1) */
int version_check(Module m);
void shadow_on_device(void* dev);
int shadow_wanted(void);
void uitext_init(Module m);
void window_init(Module m, HMODULE self);
void input_on_dinput(void* di);
/* gamepad (gamepad.c): XInput -> DirectInput keyboard/mouse (window.c) and Windows cursor in menus */
void gamepad_init(Module m);
int gamepad_on(void);
void gamepad_take_motion(long* dx, long* dy, long* dz);
void gamepad_ui_hooked(void);
void gamepad_ui_movie(const char* name);
void gamepad_ui_call(const char* name, int kind);   /* 0 NotifyMouseState, 1 mouse event, 2 SetViewport */
void gamepad_give_back(long dx, long dy, long dz);
/* d-pad menu focus (uinav.c) */
void uinav_install(void** rootVt, void* probeOrig18);
void uinav_frame(void* root, const char* movieName);
int uinav_active(void);
void uinav_request(int dir);
void uinav_take_move(long* dx, long* dy);
void uinav_release(void);
int uinav_accept(void);
int uinav_journal(void);
int gamepad_hud_binding(WORD action, int* layer, int* face, unsigned short* glyph);
void uinav_scroll(int steps);
void diag_on_dinput(void* idi8);
void* detour(void* target, void* hook);
/* texture statistics module (texmgr.c), driven by diag.c */
int texmgr_wanted(void);
void texmgr_init(Module m);
void texmgr_on_device(void* dev);
void texmgr_frame(int recording);
void texmgr_rec_start(int maxFrames);
void texmgr_report(const char* txt, const char* csv, const float* dt, int n);
DWORD texmgr_frame_work_us(int i);
/* sampling profiler (profiler.c), driven by diag.c */
void prof_init(void);
void prof_rec_start(void);
void prof_frame(int idx, float ms);
void prof_report(const char* txt, const char* csv, const float* dt, int n);
