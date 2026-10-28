/* Minimal Win32 declarations (no Windows SDK in build env). x86 only. */
#pragma once
typedef unsigned long DWORD; typedef int BOOL; typedef unsigned char BYTE; typedef unsigned short WORD;
typedef void* HANDLE; typedef void* HMODULE; typedef void* HKL; typedef long LONG; typedef unsigned int UINT;
typedef unsigned long SIZE_T; typedef long HRESULT; typedef const char* LPCSTR; typedef char* LPSTR;
typedef int (__stdcall *FARPROC)(void);
#define WINAPI __stdcall
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define PAGE_EXECUTE_READWRITE 0x40
#define MAX_PATH 260
#define FILE_APPEND_DATA 4
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define GENERIC_WRITE 0x40000000
#define CREATE_ALWAYS 2
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
typedef struct { void* BaseAddress; void* AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State; DWORD Protect; DWORD Type; } MEMORY_BASIC_INFORMATION;
#define MEM_COMMIT 0x1000
#define MEM_FREE 0x10000
#define PAGE_NOACCESS 0x01
#define PAGE_GUARD 0x100
__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) HMODULE WINAPI LoadLibraryA(LPCSTR);
__declspec(dllimport) FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR);
__declspec(dllimport) HANDLE WINAPI LoadImageA(HMODULE, LPCSTR, UINT, int, int, UINT);
__declspec(dllimport) int WINAPI MultiByteToWideChar(UINT, DWORD, LPCSTR, int, unsigned short*, int);
__declspec(dllimport) int WINAPI WideCharToMultiByte(UINT, DWORD, const unsigned short*, int, LPSTR, int, LPCSTR, int*);
__declspec(dllimport) UINT WINAPI GetSystemDirectoryA(LPSTR, UINT);
__declspec(dllimport) DWORD WINAPI GetModuleFileNameA(HMODULE, LPSTR, DWORD);
__declspec(dllimport) BOOL WINAPI VirtualProtect(void*, SIZE_T, DWORD, DWORD*);
__declspec(dllimport) SIZE_T WINAPI VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, SIZE_T);
__declspec(dllimport) BOOL WINAPI FlushInstructionCache(HANDLE, const void*, SIZE_T);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
__declspec(dllimport) UINT WINAPI GetPrivateProfileIntA(LPCSTR, LPCSTR, int, LPCSTR);
__declspec(dllimport) DWORD WINAPI GetPrivateProfileStringA(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
__declspec(dllimport) void WINAPI InitializeCriticalSection(void*);
__declspec(dllimport) void WINAPI EnterCriticalSection(void*);
__declspec(dllimport) void WINAPI LeaveCriticalSection(void*);
__declspec(dllimport) HKL WINAPI GetKeyboardLayout(DWORD);
__declspec(dllimport) int WINAPI GetKeyboardLayoutList(int, HKL*);
__declspec(dllimport) int __cdecl wsprintfA(LPSTR, LPCSTR, ...);
__declspec(dllimport) int WINAPI wvsprintfA(LPSTR, LPCSTR, __builtin_va_list);
typedef struct { long long QuadPart; } LARGE_INTEGER_;
__declspec(dllimport) BOOL WINAPI QueryPerformanceCounter(long long*);
__declspec(dllimport) BOOL WINAPI QueryPerformanceFrequency(long long*);
__declspec(dllimport) void WINAPI Sleep(DWORD);
__declspec(dllimport) HANDLE WINAPI CreateThread(void*, SIZE_T, DWORD (WINAPI*)(void*), void*, DWORD, DWORD*);
__declspec(dllimport) short WINAPI GetAsyncKeyState(int);
__declspec(dllimport) void* WINAPI GetForegroundWindow(void);
__declspec(dllimport) int WINAPI MessageBoxW(void*, const unsigned short*, const unsigned short*, UINT);
__declspec(dllimport) DWORD WINAPI GetWindowThreadProcessId(void*, DWORD*);
__declspec(dllimport) int WINAPI PostMessageA(void*, UINT, DWORD, long);
__declspec(dllimport) DWORD WINAPI GetCurrentProcessId(void);
__declspec(dllimport) void* WINAPI VirtualAlloc(void*, SIZE_T, DWORD, DWORD);
__declspec(dllimport) BOOL WINAPI VirtualFree(void*, SIZE_T, DWORD);
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
__declspec(dllimport) void WINAPI GetLocalTime(SYSTEMTIME*);
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
  QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage; } PROCESS_MEMORY_COUNTERS_EX;
__declspec(dllimport) BOOL WINAPI K32GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS_EX*, DWORD);
__declspec(dllimport) DWORD WINAPI GetCurrentThreadId(void);
__declspec(dllimport) BOOL WINAPI ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
__declspec(dllimport) DWORD WINAPI GetFileSize(HANDLE, DWORD*);
__declspec(dllimport) BOOL WINAPI WritePrivateProfileStringA(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
__declspec(dllimport) DWORD WINAPI TlsAlloc(void);
__declspec(dllimport) void* WINAPI TlsGetValue(DWORD);
__declspec(dllimport) BOOL WINAPI TlsSetValue(DWORD, void*);
#define TLS_OUT_OF_INDEXES 0xFFFFFFFF
/* sampling profiler */
typedef struct { void* lpBaseOfDll; DWORD SizeOfImage; void* EntryPoint; } MODULEINFO;
__declspec(dllimport) HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD);
__declspec(dllimport) DWORD WINAPI SuspendThread(HANDLE);
__declspec(dllimport) DWORD WINAPI ResumeThread(HANDLE);
__declspec(dllimport) BOOL WINAPI GetThreadContext(HANDLE, void*);
__declspec(dllimport) BOOL WINAPI SetThreadPriority(HANDLE, int);
__declspec(dllimport) HANDLE WINAPI GetCurrentThread(void);
__declspec(dllimport) BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE*, DWORD, DWORD*);
__declspec(dllimport) BOOL WINAPI K32GetModuleInformation(HANDLE, HMODULE, MODULEINFO*, DWORD);
__declspec(dllimport) HANDLE WINAPI CreateWaitableTimerExW(void*, const unsigned short*, DWORD, DWORD);
__declspec(dllimport) BOOL WINAPI SetWaitableTimer(HANDLE, const long long*, long, void*, void*, BOOL);
__declspec(dllimport) DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x1F0003
#define GENERIC_READ 0x80000000
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;
