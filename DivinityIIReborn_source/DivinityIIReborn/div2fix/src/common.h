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
void mindread_init(Module m);
void camera_init(Module m);
void icons_init(Module m);
void intro_init(Module m);
int version_check(Module m);
void shadow_on_device(void* dev);
int shadow_wanted(void);
void uitext_init(Module m);
void window_init(Module m, HMODULE self);
void input_on_dinput(void* di);
void diag_on_dinput(void* idi8);
void* detour(void* target, void* hook);
