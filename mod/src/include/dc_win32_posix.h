/* dc_win32_posix.h - the small part of Win32 the mod runtime uses, implemented on POSIX (Linux, also builds on macOS).
 *
 * The mod runtime was written for Windows: folders are listed with FindFirstFile, paths are built with '\' and, in the
 * model and package code, kept as UTF-16 wide strings. Rewriting all of that would fork the Windows code; instead the
 * same calls exist here with POSIX behaviour, and the Windows build never sees this header.
 *
 * Path rules on POSIX (every function below that takes a path follows them):
 *   - '\' is a separator, exactly like '/' (a real backslash in a Linux file name is not supported);
 *   - if the path does not exist as written, it is looked up again case-insensitively, component by component, so
 *     "mods\Textures\Index.txt" finds mods/textures/index.txt like NTFS would;
 *   - wide strings are UTF-32 (wchar_t on Linux) and are converted to UTF-8; CP_ACP is UTF-8 too.
 */
#ifndef DC_WIN32_POSIX_H
#define DC_WIN32_POSIX_H

#ifndef _WIN32

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DC_WIN32_BASE_TYPES
#define DC_WIN32_BASE_TYPES
typedef uint32_t DWORD;
typedef int BOOL;
#endif
typedef void *HANDLE;
typedef wchar_t WCHAR;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef void *LPVOID;
typedef const wchar_t *LPCWSTR;
#ifndef WINAPI
#define WINAPI
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFFu)
#define FILE_ATTRIBUTE_READONLY  0x01u
#define FILE_ATTRIBUTE_HIDDEN    0x02u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL    0x80u
#define CP_ACP  0u
#define CP_UTF8 65001u

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    char     cFileName[MAX_PATH];
} WIN32_FIND_DATAA;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    wchar_t  cFileName[MAX_PATH];
} WIN32_FIND_DATAW;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

typedef enum { GetFileExInfoStandard = 0 } GET_FILEEX_INFO_LEVELS;

/* Paths: '\' -> '/', then a case-insensitive lookup when the exact path is missing. Returns out (always terminated). */
char *dc_posix_path(const char *in, char *out, size_t cap);
/* The same for a wide path (UTF-32 -> UTF-8). */
char *dc_posix_wpath(const wchar_t *in, char *out, size_t cap);
/* fopen with the path rules above; dc_fopen is plain fopen on Windows (dc_platform.h). */
FILE *dc_fopen(const char *path, const char *mode);
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);

/* Folder listing: "dir\*", "dir\*.png" or an exact path (one entry). The mask is case-insensitive like NTFS. */
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd);
BOOL   FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd);
BOOL   FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL   FindClose(HANDLE h);

DWORD GetFileAttributesA(const char *path);
DWORD GetFileAttributesW(const wchar_t *path);
BOOL  GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out);
BOOL  GetFileAttributesExW(const wchar_t *path, GET_FILEEX_INFO_LEVELS level, void *out);
BOOL  SetFileAttributesW(const wchar_t *path, DWORD attributes);   /* no hidden bit on POSIX: accepted, nothing to do */
BOOL  CreateDirectoryA(const char *path, void *security);
BOOL  CreateDirectoryW(const wchar_t *path, void *security);
BOOL  RemoveDirectoryW(const wchar_t *path);
BOOL  DeleteFileW(const wchar_t *path);
BOOL  CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists);
BOOL  CreateHardLinkW(const wchar_t *link_path, const wchar_t *existing, void *security);
BOOL  MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags);

/* The running executable with '/' separators (readlink /proc/self/exe; _NSGetExecutablePath on macOS). */
DWORD GetModuleFileNameA(void *module, char *out, DWORD cap);
DWORD GetModuleFileNameW(void *module, wchar_t *out, DWORD cap);
/* the folder of the running executable with a trailing '/', 0 when unknown. The game works in data/ (dc_boot.c), so
 * anything that lies next to the executable (dcme_data.pak, mods/) is found from here, not from the working directory. */
int dc_exe_dir(char *out, size_t cap);

/* UTF-8 <-> UTF-32 with the Win32 contract: len -1 includes the terminator, cap 0 returns the needed size. */
int MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int len, wchar_t *dst, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *src, int len, char *dst, int cap,
                        const char *def, BOOL *used_def);

/* the last path separator of either kind (NULL - none) */
static inline char *dc_last_sep(char *p)
{
    char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    return a > b ? a : b;
}

#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp  dc_wcsicmp
#define _wcsnicmp dc_wcsnicmp
#define _snwprintf swprintf
int dc_wcsicmp(const wchar_t *a, const wchar_t *b);
int dc_wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);

/* CRT stat/find types for the loaders that read packed data (dc_pak.h stand-ins): only the fields they use */
#ifndef __int64
#define __int64 long long
#endif
#define _S_IFREG S_IFREG
#define _S_IFDIR S_IFDIR
#define _A_SUBDIR 0x10u
typedef uint32_t _fsize_t;
struct _stat64 {
    __int64  st_size;
    unsigned st_mode;
#if defined(st_mtime) && defined(__APPLE__)
    struct timespec st_mtimespec;   /* <sys/stat.h> maps st_mtime onto it */
#elif defined(st_mtime)
    struct timespec st_mtim;
#else
    time_t   st_mtime;
#endif
};
struct _finddata_t {
    unsigned attrib;
    time_t   time_create, time_access, time_write;
    _fsize_t size;
    char     name[260];
};

/* ------------------------------------------------------------------ kernel objects: events and waitable threads */
typedef uintptr_t DWORD_PTR;
typedef long HRESULT;
typedef const wchar_t *PCWSTR;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
typedef void *HMODULE;
typedef void (*FARPROC)(void);
#define INFINITE        0xFFFFFFFFu
#define WAIT_OBJECT_0   0u
#define WAIT_TIMEOUT    258u
#define WAIT_FAILED     0xFFFFFFFFu
#define THREAD_PRIORITY_LOWEST       (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL       0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST      2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define FILE_NOTIFY_CHANGE_FILE_NAME  0x01u
#define FILE_NOTIFY_CHANGE_DIR_NAME   0x02u
#define FILE_NOTIFY_CHANGE_SIZE       0x08u
#define FILE_NOTIFY_CHANGE_LAST_WRITE 0x10u

typedef struct {
    DWORD     dwNumberOfProcessors;
    DWORD     dwPageSize;
    DWORD_PTR dwActiveProcessorMask;
} SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);

HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name);
HANDLE CreateEventW(void *security, BOOL manual_reset, BOOL initial, const wchar_t *name);
#define CreateEvent CreateEventA
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
/* a thread handle is signalled when the thread returns; the thread runs detached either way */
HANDLE CreateThread(void *security, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms);
BOOL   CloseHandle(HANDLE h);
HANDLE GetCurrentThread(void);
BOOL   SetThreadPriority(HANDLE h, int priority);   /* accepted; Linux threads keep the process priority */
int    GetThreadPriority(HANDLE h);
HMODULE GetModuleHandleW(const wchar_t *name);      /* NULL: no system DLLs to look into */
HMODULE GetModuleHandleA(const char *name);
FARPROC GetProcAddress(HMODULE m, const char *name);

HANDLE GetCurrentProcess(void);                       /* pseudo handle */
BOOL   GetProcessAffinityMask(HANDLE p, DWORD_PTR *process_mask, DWORD_PTR *system_mask);
LONG   CompareFileTime(const FILETIME *a, const FILETIME *b);
#define SwitchToThread() (sched_yield() == 0)
#define _wcsdup  wcsdup
#define _strtoui64 strtoull

/* folder change notifications: not offered - the callers fall back to their periodic rescan */
static inline HANDLE FindFirstChangeNotificationA(const char *p, BOOL sub, DWORD f) { (void)p; (void)sub; (void)f; return INVALID_HANDLE_VALUE; }
static inline BOOL FindNextChangeNotification(HANDLE h) { (void)h; return FALSE; }
static inline BOOL FindCloseChangeNotification(HANDLE h) { (void)h; return TRUE; }

#define InterlockedCompareExchange(p, x, cmp) __sync_val_compare_and_swap((p), (cmp), (x))
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)

/* the CRT calls themselves (path rules above); dc_pak.h redefines them over packed data where it reads our data */
int      dc_stat64(const char *path, struct _stat64 *st);
intptr_t dc_findfirst(const char *pattern, struct _finddata_t *fd);
int      dc_findnext(intptr_t h, struct _finddata_t *fd);
int      dc_findclose(intptr_t h);
#define _stat64(p, st)    dc_stat64((p), (st))
#define _findfirst(p, fd) dc_findfirst((p), (fd))
#define _findnext(h, fd)  dc_findnext((h), (fd))
#define _findclose(h)     dc_findclose(h)

#if defined(__x86_64__) || defined(__i386__)
#define YieldProcessor() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define YieldProcessor() __asm__ __volatile__("yield")
#else
#define YieldProcessor() ((void)0)
#endif

#define InterlockedOr(p, v)       __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif
