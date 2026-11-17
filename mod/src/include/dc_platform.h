#ifndef DC_PLATFORM_H
#define DC_PLATFORM_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
/* fopen with POSIX path rules on Linux (dc_win32_posix.h); the plain one on Windows */
#define dc_fopen fopen
#else
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef struct { int64_t QuadPart; } LARGE_INTEGER;
#ifndef DC_WIN32_BASE_TYPES
#define DC_WIN32_BASE_TYPES
typedef uint32_t DWORD;
typedef int BOOL;
#endif

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_WRITE_THROUGH    0x8u

static inline int QueryPerformanceFrequency(LARGE_INTEGER *out)
{
    if (!out) return 0;
    out->QuadPart = 1000000000ll;
    return 1;
}

static inline int QueryPerformanceCounter(LARGE_INTEGER *out)
{
    struct timespec ts;
    if (!out || clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    out->QuadPart = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    return 1;
}

static inline uint64_t GetTickCount64(void)
{
    LARGE_INTEGER now;
    return QueryPerformanceCounter(&now) ? (uint64_t)(now.QuadPart / 1000000ll) : 0;
}

static inline uint32_t GetTickCount(void)
{
    return (uint32_t)GetTickCount64();
}

static inline void Sleep(DWORD milliseconds)
{
    struct timespec req;
    req.tv_sec = milliseconds / 1000u;
    req.tv_nsec = (long)(milliseconds % 1000u) * 1000000l;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {}
}

static inline DWORD GetCurrentThreadId(void)
{
    return (DWORD)((uintptr_t)pthread_self() & 0xffffffffu);
}

static inline int MoveFileExA(const char *source, const char *destination, unsigned flags)
{
    (void)flags;
    return rename(source, destination) == 0;
}

#include "dc_win32_posix.h"
#endif

#endif
