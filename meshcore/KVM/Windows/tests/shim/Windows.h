/* Test shim: just enough of <windows.h> for kvm_video.cpp and the headers it includes to compile on Linux in kvm_video_harness_test.cpp (a fake clock, no real API). */
#ifndef KVM_TEST_WINDOWS_SHIM_H
#define KVM_TEST_WINDOWS_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef long long LONGLONG;
typedef void *HANDLE;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { int unused; } BITMAPINFO;
#define WINAPI
#define __inline inline
#define UNREFERENCED_PARAMETER(x) (void)(x)
typedef void *HDESK;
#define UOI_NAME 2
extern "C" {
extern uint32_t gTestNowMs;
extern long long gTestQpcTicks;
DWORD timeGetTime(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *p);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *p);
DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD size);
HDESK GetThreadDesktop(DWORD tid);
DWORD GetCurrentThreadId(void);
BOOL GetUserObjectInformationA(HDESK d, int index, void *info, DWORD len, DWORD *needed);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentProcess(void);
BOOL TerminateProcess(HANDLE p, unsigned code);
LONG InterlockedIncrement(volatile LONG *p);
LONG InterlockedExchange(volatile LONG *p, LONG v);
LONG InterlockedCompareExchange(volatile LONG *p, LONG ex, LONG cmp);
}
#endif
