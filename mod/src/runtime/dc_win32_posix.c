/* dc_win32_posix.c - POSIX implementation of the Win32 subset in dc_win32_posix.h. Compiled to nothing on Windows. */
#ifndef _WIN32

#if !defined(_GNU_SOURCE) && defined(__linux__)
#define _GNU_SOURCE
#endif
#include "dc_win32_posix.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wctype.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef __APPLE__
#define DC_MTIME(st) ((st)->st_mtimespec)
#define DC_ATIME(st) ((st)->st_atimespec)
#define DC_CTIME(st) ((st)->st_ctimespec)
#else
#define DC_MTIME(st) ((st)->st_mtim)
#define DC_ATIME(st) ((st)->st_atim)
#define DC_CTIME(st) ((st)->st_ctim)
#endif

/* ------------------------------------------------------------------------------------------------ UTF-8 <-> UTF-32 */
static size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int len = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!len || (size_t)len > n) { *cp = 0xFFFD; return 1; }
    uint32_t c = s[0] & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c;
    return (size_t)len;
}

static int utf8_encode(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c > 0x10FFFF) c = 0xFFFD;
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

int MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int len, wchar_t *dst, int cap)
{
    (void)cp; (void)flags;
    if (!src) return 0;
    const size_t n = len < 0 ? strlen(src) + 1 : (size_t)len;
    const unsigned char *s = (const unsigned char *)src;
    int out = 0;
    for (size_t i = 0; i < n;) {
        uint32_t c;
        i += utf8_decode(s + i, n - i, &c);
        if (cap > 0) {
            if (out >= cap) return 0;          /* Win32: buffer too small - 0 */
            dst[out] = (wchar_t)c;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *src, int len, char *dst, int cap,
                        const char *def, BOOL *used_def)
{
    (void)cp; (void)flags; (void)def;
    if (used_def) *used_def = FALSE;
    if (!src) return 0;
    const size_t n = len < 0 ? wcslen(src) + 1 : (size_t)len;
    int out = 0;
    for (size_t i = 0; i < n; i++) {
        char b[4];
        const int k = utf8_encode((uint32_t)src[i], b);
        if (cap > 0) {
            if (out + k > cap) return 0;
            memcpy(dst + out, b, (size_t)k);
        }
        out += k;
    }
    return out;
}

int dc_wcsicmp(const wchar_t *a, const wchar_t *b)
{
    for (;; a++, b++) {
        const wint_t x = towlower((wint_t)*a), y = towlower((wint_t)*b);
        if (x != y || !x) return (int)x - (int)y;
    }
}

int dc_wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        const wint_t x = towlower((wint_t)*a), y = towlower((wint_t)*b);
        if (x != y || !x) return (int)x - (int)y;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------ paths */
static int exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0;
}

/* p already has '/' separators and does not exist: find every component case-insensitively. 0 - not found. */
static int resolve_case(char *p, size_t cap)
{
    char res[MAX_PATH];
    size_t rn = 0;
    const char *s = p;
    if (*s == '/') { res[rn++] = '/'; while (*s == '/') s++; }
    res[rn] = 0;
    while (*s) {
        const char *e = strchr(s, '/');
        const size_t cl = e ? (size_t)(e - s) : strlen(s);
        char comp[MAX_PATH];
        if (cl >= sizeof comp || rn + cl + 2 >= sizeof res) return 0;
        memcpy(comp, s, cl);
        comp[cl] = 0;
        char trial[MAX_PATH * 2];                 /* res + comp: both shorter than MAX_PATH */
        snprintf(trial, sizeof trial, "%s%s", res, comp);
        if (!exists(trial) && strcmp(comp, ".") && strcmp(comp, "..")) {
            DIR *d = opendir(rn ? res : ".");
            int found = 0;
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL)
                    if (!strcasecmp(de->d_name, comp)) { snprintf(comp, sizeof comp, "%s", de->d_name); found = 1; break; }
                closedir(d);
            }
            if (!found) return 0;
        }
        memcpy(res + rn, comp, strlen(comp));
        rn += strlen(comp);
        s += cl;
        if (*s == '/') {
            res[rn++] = '/';
            while (*s == '/') s++;
        }
        res[rn] = 0;
    }
    if (rn + 1 > cap) return 0;
    memcpy(p, res, rn + 1);
    return 1;
}

char *dc_posix_path(const char *in, char *out, size_t cap)
{
    if (!cap) return out;
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; i++) out[i] = in[i] == '\\' ? '/' : in[i];
    out[i] = 0;
    if (out[0] && !exists(out)) {
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof tmp, "%s", out);
        if (resolve_case(tmp, sizeof tmp)) snprintf(out, cap, "%s", tmp);
    }
    return out;
}

char *dc_posix_wpath(const wchar_t *in, char *out, size_t cap)
{
    char u[MAX_PATH];
    if (!in || !WideCharToMultiByte(CP_UTF8, 0, in, -1, u, (int)sizeof u, NULL, NULL)) u[0] = 0;
    return dc_posix_path(u, out, cap);
}

FILE *dc_fopen(const char *path, const char *mode)
{
    char p[MAX_PATH];
    return fopen(dc_posix_path(path, p, sizeof p), mode);
}

FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char p[MAX_PATH], m[16];
    if (!WideCharToMultiByte(CP_UTF8, 0, mode, -1, m, (int)sizeof m, NULL, NULL)) return NULL;
    return fopen(dc_posix_wpath(path, p, sizeof p), m);
}

/* ------------------------------------------------------------------------------------------------- attributes/times */
static void to_filetime(struct timespec ts, FILETIME *ft)
{
    const uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static DWORD attrs_of(const struct stat *st, const char *name)
{
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (name && name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

static void fill_data(const struct stat *st, const char *name, WIN32_FILE_ATTRIBUTE_DATA *d)
{
    d->dwFileAttributes = attrs_of(st, name);
    to_filetime(DC_CTIME(st), &d->ftCreationTime);
    to_filetime(DC_ATIME(st), &d->ftLastAccessTime);
    to_filetime(DC_MTIME(st), &d->ftLastWriteTime);
    const uint64_t sz = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
    d->nFileSizeHigh = (DWORD)(sz >> 32);
    d->nFileSizeLow = (DWORD)sz;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

DWORD GetFileAttributesA(const char *path)
{
    char p[MAX_PATH];
    struct stat st;
    if (stat(dc_posix_path(path, p, sizeof p), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return attrs_of(&st, base_name(p));
}

DWORD GetFileAttributesW(const wchar_t *path)
{
    char p[MAX_PATH];
    struct stat st;
    if (stat(dc_posix_wpath(path, p, sizeof p), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return attrs_of(&st, base_name(p));
}

BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    char p[MAX_PATH];
    struct stat st;
    (void)level;
    if (!out || stat(dc_posix_path(path, p, sizeof p), &st) != 0) return FALSE;
    fill_data(&st, base_name(p), (WIN32_FILE_ATTRIBUTE_DATA *)out);
    return TRUE;
}

BOOL GetFileAttributesExW(const wchar_t *path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    char p[MAX_PATH];
    struct stat st;
    (void)level;
    if (!out || stat(dc_posix_wpath(path, p, sizeof p), &st) != 0) return FALSE;
    fill_data(&st, base_name(p), (WIN32_FILE_ATTRIBUTE_DATA *)out);
    return TRUE;
}

BOOL SetFileAttributesW(const wchar_t *path, DWORD attributes)
{
    (void)attributes;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* CreateDirectory on a path whose last component is new: the parent is resolved case-insensitively, the new name kept */
static char *new_entry_path(const char *in, char *out, size_t cap)
{
    char raw[MAX_PATH];
    size_t i = 0;
    for (; in[i] && i + 1 < sizeof raw; i++) raw[i] = in[i] == '\\' ? '/' : in[i];
    raw[i] = 0;
    while (i > 1 && raw[i - 1] == '/') raw[--i] = 0;
    char *sl = strrchr(raw, '/');
    if (!sl) return dc_posix_path(raw, out, cap);
    *sl = 0;
    char parent[MAX_PATH];
    dc_posix_path(raw[0] ? raw : "/", parent, sizeof parent);
    snprintf(out, cap, "%s/%s", strcmp(parent, "/") ? parent : "", sl + 1);
    return out;
}

static char *new_entry_wpath(const wchar_t *in, char *out, size_t cap)
{
    char u[MAX_PATH];
    if (!in || !WideCharToMultiByte(CP_UTF8, 0, in, -1, u, (int)sizeof u, NULL, NULL)) u[0] = 0;
    return new_entry_path(u, out, cap);
}

BOOL CreateDirectoryA(const char *path, void *security)
{
    char p[MAX_PATH];
    (void)security;
    return mkdir(new_entry_path(path, p, sizeof p), 0755) == 0;
}

BOOL CreateDirectoryW(const wchar_t *path, void *security)
{
    char p[MAX_PATH];
    (void)security;
    return mkdir(new_entry_wpath(path, p, sizeof p), 0755) == 0;
}

BOOL RemoveDirectoryW(const wchar_t *path)
{
    char p[MAX_PATH];
    return rmdir(dc_posix_wpath(path, p, sizeof p)) == 0;
}

BOOL DeleteFileW(const wchar_t *path)
{
    char p[MAX_PATH];
    return unlink(dc_posix_wpath(path, p, sizeof p)) == 0;
}

BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists)
{
    char a[MAX_PATH], b[MAX_PATH];
    dc_posix_wpath(from, a, sizeof a);
    new_entry_wpath(to, b, sizeof b);
    const int in = open(a, O_RDONLY);
    if (in < 0) return FALSE;
    const int out = open(b, O_WRONLY | O_CREAT | O_TRUNC | (fail_if_exists ? O_EXCL : 0), 0644);
    if (out < 0) { close(in); return FALSE; }
    char buf[65536];
    BOOL ok = TRUE;
    for (;;) {
        const ssize_t n = read(in, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; ok = FALSE; break; }
        for (ssize_t w = 0; w < n;) {
            const ssize_t k = write(out, buf + w, (size_t)(n - w));
            if (k < 0) { if (errno == EINTR) continue; ok = FALSE; break; }
            w += k;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0) ok = FALSE;
    if (!ok) unlink(b);
    return ok;
}

BOOL CreateHardLinkW(const wchar_t *link_path, const wchar_t *existing, void *security)
{
    char a[MAX_PATH], b[MAX_PATH];
    (void)security;
    return link(dc_posix_wpath(existing, a, sizeof a), new_entry_wpath(link_path, b, sizeof b)) == 0;
}

BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags)
{
    char a[MAX_PATH], b[MAX_PATH];
    (void)flags;
    return rename(dc_posix_wpath(from, a, sizeof a), new_entry_wpath(to, b, sizeof b)) == 0;
}

DWORD GetModuleFileNameA(void *module, char *out, DWORD cap)
{
    (void)module;
    if (!out || !cap) return 0;
#ifdef __APPLE__
    char tmp[MAX_PATH];
    uint32_t sz = sizeof tmp;
    if (_NSGetExecutablePath(tmp, &sz) != 0) return 0;
    char real[MAX_PATH];
    const char *p = realpath(tmp, real) ? real : tmp;
    const size_t n = strlen(p);
#else
    char p[MAX_PATH];
    const ssize_t r = readlink("/proc/self/exe", p, sizeof p - 1);
    if (r <= 0) return 0;
    p[r] = 0;
    const size_t n = (size_t)r;
#endif
    if (n >= cap) { memcpy(out, p, cap - 1); out[cap - 1] = 0; return cap; }   /* Win32: truncated - returns cap */
    memcpy(out, p, n + 1);
    return (DWORD)n;
}

DWORD GetModuleFileNameW(void *module, wchar_t *out, DWORD cap)
{
    char u[MAX_PATH];
    const DWORD n = GetModuleFileNameA(module, u, sizeof u);
    if (!n || n >= sizeof u || !out || !cap) return 0;
    const int w = MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)cap);
    return w > 0 ? (DWORD)(w - 1) : cap;
}

/* ------------------------------------------------------------------------------------------------------ find files */
typedef struct {
    DIR  *dir;
    char  dirpath[MAX_PATH];
    char  mask[MAX_PATH];
    int   single;                   /* exact path: one entry, already returned */
} Finder;

/* case-insensitive '*' / '?' match; Windows also lets "*.*" match names without a dot */
static int wild(const char *m, const char *s)
{
    if (!strcmp(m, "*.*")) return 1;
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*m == '?' || (*m && tolower((unsigned char)*m) == tolower((unsigned char)*s))) { m++; s++; continue; }
        if (*m == '*') { star = m++; ss = s; continue; }
        if (star) { m = star + 1; s = ++ss; continue; }
        return 0;
    }
    while (*m == '*') m++;
    return !*m;
}

static int fill_find(Finder *f, const char *name, WIN32_FIND_DATAA *fd)
{
    char full[MAX_PATH * 2];
    snprintf(full, sizeof full, "%s/%s", f->dirpath, name);
    struct stat st;
    if (stat(full, &st) != 0 && lstat(full, &st) != 0) return 0;   /* a broken link is listed like a file */
    WIN32_FILE_ATTRIBUTE_DATA d;
    fill_data(&st, name, &d);
    memset(fd, 0, sizeof *fd);
    fd->dwFileAttributes = d.dwFileAttributes;
    fd->ftCreationTime = d.ftCreationTime;
    fd->ftLastAccessTime = d.ftLastAccessTime;
    fd->ftLastWriteTime = d.ftLastWriteTime;
    fd->nFileSizeHigh = d.nFileSizeHigh;
    fd->nFileSizeLow = d.nFileSizeLow;
    snprintf(fd->cFileName, sizeof fd->cFileName, "%s", name);
    return 1;
}

static int next_entry(Finder *f, WIN32_FIND_DATAA *fd)
{
    if (!f->dir) return 0;
    struct dirent *de;
    while ((de = readdir(f->dir)) != NULL)
        if (wild(f->mask, de->d_name) && fill_find(f, de->d_name, fd)) return 1;
    return 0;
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd)
{
    if (!pattern || !fd) return INVALID_HANDLE_VALUE;
    char raw[MAX_PATH];
    size_t i = 0;
    for (; pattern[i] && i + 1 < sizeof raw; i++) raw[i] = pattern[i] == '\\' ? '/' : pattern[i];
    raw[i] = 0;
    Finder *f = (Finder *)calloc(1, sizeof *f);
    if (!f) return INVALID_HANDLE_VALUE;
    char *sl = strrchr(raw, '/');
    const char *mask = sl ? sl + 1 : raw;
    if (sl) { *sl = 0; dc_posix_path(raw[0] ? raw : "/", f->dirpath, sizeof f->dirpath); }
    else snprintf(f->dirpath, sizeof f->dirpath, ".");
    snprintf(f->mask, sizeof f->mask, "%s", mask);
    if (!strpbrk(f->mask, "*?")) {                 /* an exact name: one entry, found case-insensitively */
        f->single = 1;
        f->dir = opendir(f->dirpath);
        const int ok = next_entry(f, fd);
        closedir(f->dir);
        f->dir = NULL;
        if (!ok) { free(f); return INVALID_HANDLE_VALUE; }
        return (HANDLE)f;
    }
    f->dir = opendir(f->dirpath);
    if (!f->dir || !next_entry(f, fd)) {
        if (f->dir) closedir(f->dir);
        free(f);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)f;
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    Finder *f = (Finder *)h;
    return f->single ? FALSE : next_entry(f, fd);
}

static void find_a2w(const WIN32_FIND_DATAA *a, WIN32_FIND_DATAW *w)
{
    w->dwFileAttributes = a->dwFileAttributes;
    w->ftCreationTime = a->ftCreationTime;
    w->ftLastAccessTime = a->ftLastAccessTime;
    w->ftLastWriteTime = a->ftLastWriteTime;
    w->nFileSizeHigh = a->nFileSizeHigh;
    w->nFileSizeLow = a->nFileSizeLow;
    if (!MultiByteToWideChar(CP_UTF8, 0, a->cFileName, -1, w->cFileName, MAX_PATH)) w->cFileName[0] = 0;
}

HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd)
{
    char u[MAX_PATH];
    if (!pattern || !fd || !WideCharToMultiByte(CP_UTF8, 0, pattern, -1, u, (int)sizeof u, NULL, NULL))
        return INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAA a;
    HANDLE h = FindFirstFileA(u, &a);
    if (h != INVALID_HANDLE_VALUE) find_a2w(&a, fd);
    return h;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd)
{
    WIN32_FIND_DATAA a;
    if (!FindNextFileA(h, &a)) return FALSE;
    find_a2w(&a, fd);
    return TRUE;
}

BOOL FindClose(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    Finder *f = (Finder *)h;
    if (f->dir) closedir(f->dir);
    free(f);
    return TRUE;
}

/* ---------------------------------------------------------------------------------------------- kernel objects */
enum { K_EVENT = 0x45564E54, K_THREAD = 0x54485244 };
typedef struct {
    uint32_t        kind;
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             signaled, manual;
    int             refs;                       /* the handle, plus the running thread */
    LPTHREAD_START_ROUTINE fn;
    LPVOID          arg;
} KObj;

static KObj *kobj_new(uint32_t kind, int manual, int signaled, int refs)
{
    KObj *k = (KObj *)calloc(1, sizeof *k);
    if (!k) return NULL;
    k->kind = kind; k->manual = manual; k->signaled = signaled; k->refs = refs;
    pthread_mutex_init(&k->m, NULL);
#ifdef __APPLE__
    pthread_cond_init(&k->c, NULL);
#else
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&k->c, &ca);
    pthread_condattr_destroy(&ca);
#endif
    return k;
}

static void kobj_unref(KObj *k)
{
    pthread_mutex_lock(&k->m);
    const int left = --k->refs;
    pthread_mutex_unlock(&k->m);
    if (left > 0) return;
    pthread_cond_destroy(&k->c);
    pthread_mutex_destroy(&k->m);
    free(k);
}

static int is_kobj(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    const uint32_t kind = ((KObj *)h)->kind;
    return kind == K_EVENT || kind == K_THREAD;
}

void GetSystemInfo(SYSTEM_INFO *si)
{
    if (!si) return;
    memset(si, 0, sizeof *si);
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1u;
    const long pg = sysconf(_SC_PAGESIZE);
    si->dwPageSize = pg > 0 ? (DWORD)pg : 4096u;
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= sizeof(DWORD_PTR) * 8 ? ~(DWORD_PTR)0
                                                                                : (((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1);
}

HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name)
{
    (void)security; (void)name;
    return (HANDLE)kobj_new(K_EVENT, manual_reset ? 1 : 0, initial ? 1 : 0, 1);
}

HANDLE CreateEventW(void *security, BOOL manual_reset, BOOL initial, const wchar_t *name)
{
    (void)name;
    return CreateEventA(security, manual_reset, initial, NULL);
}

BOOL SetEvent(HANDLE h)
{
    if (!is_kobj(h)) return FALSE;
    KObj *k = (KObj *)h;
    pthread_mutex_lock(&k->m);
    k->signaled = 1;
    pthread_cond_broadcast(&k->c);
    pthread_mutex_unlock(&k->m);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    if (!is_kobj(h)) return FALSE;
    KObj *k = (KObj *)h;
    pthread_mutex_lock(&k->m);
    k->signaled = 0;
    pthread_mutex_unlock(&k->m);
    return TRUE;
}

static void *thread_tramp(void *p)
{
    KObj *k = (KObj *)p;
    k->fn(k->arg);
    pthread_mutex_lock(&k->m);
    k->signaled = 1;                              /* a thread handle stays signalled (manual) */
    pthread_cond_broadcast(&k->c);
    pthread_mutex_unlock(&k->m);
    kobj_unref(k);
    return NULL;
}

HANDLE CreateThread(void *security, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id)
{
    (void)security; (void)flags;
    if (!fn) return NULL;
    KObj *k = kobj_new(K_THREAD, 1, 0, 2);
    if (!k) return NULL;
    k->fn = fn; k->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (stack) {
        const size_t min = 256u * 1024u;          /* Win32 rounds up too; glibc wants at least PTHREAD_STACK_MIN */
        pthread_attr_setstacksize(&at, stack < min ? min : stack);
    }
    pthread_t t;
    const int r = pthread_create(&t, &at, thread_tramp, k);
    pthread_attr_destroy(&at);
    if (r != 0) { k->refs = 1; kobj_unref(k); return NULL; }
    if (id) *id = (DWORD)((uintptr_t)t & 0xFFFFFFFFu);
    return (HANDLE)k;
}

static void deadline(struct timespec *ts, DWORD ms)
{
#ifdef __APPLE__
    clock_gettime(CLOCK_REALTIME, ts);
#else
    clock_gettime(CLOCK_MONOTONIC, ts);
#endif
    ts->tv_sec += ms / 1000u;
    ts->tv_nsec += (long)(ms % 1000u) * 1000000l;
    if (ts->tv_nsec >= 1000000000l) { ts->tv_sec++; ts->tv_nsec -= 1000000000l; }
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!is_kobj(h)) return WAIT_FAILED;
    KObj *k = (KObj *)h;
    struct timespec ts;
    if (ms != INFINITE) deadline(&ts, ms);
    DWORD r = WAIT_OBJECT_0;
    pthread_mutex_lock(&k->m);
    while (!k->signaled) {
        if (ms == 0) { r = WAIT_TIMEOUT; break; }
        if (ms == INFINITE) pthread_cond_wait(&k->c, &k->m);
        else if (pthread_cond_timedwait(&k->c, &k->m, &ts) == ETIMEDOUT) { if (!k->signaled) r = WAIT_TIMEOUT; break; }
    }
    if (r == WAIT_OBJECT_0 && !k->manual) k->signaled = 0;   /* auto-reset event: one waiter takes it */
    pthread_mutex_unlock(&k->m);
    return r;
}

DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    if (!n || !h) return WAIT_FAILED;
    if (all) {                                    /* the callers wait for worker threads: one after another */
        for (DWORD i = 0; i < n; i++) {
            const DWORD r = WaitForSingleObject(h[i], ms);
            if (r != WAIT_OBJECT_0) return r;
        }
        return WAIT_OBJECT_0;
    }
    uint64_t start = 0;
    { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); start = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
    for (;;) {                                    /* any: short polls, enough for the rare callers */
        for (DWORD i = 0; i < n; i++)
            if (WaitForSingleObject(h[i], 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + i;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        const uint64_t now = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
        if (ms != INFINITE && now - start >= ms) return WAIT_TIMEOUT;
        sched_yield();
        struct timespec nap = {0, 500000};
        nanosleep(&nap, NULL);
    }
}

BOOL CloseHandle(HANDLE h)
{
    if (!is_kobj(h)) return FALSE;
    kobj_unref((KObj *)h);
    return TRUE;
}

HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *process_mask, DWORD_PTR *system_mask)
{
    (void)p;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD_PTR m = si.dwActiveProcessorMask;
#if defined(__linux__) && defined(CPU_COUNT)
    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof cs, &cs) == 0) {
        m = 0;
        for (unsigned i = 0; i < sizeof(DWORD_PTR) * 8 && i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &cs)) m |= (DWORD_PTR)1 << i;
    }
#endif
    if (process_mask) *process_mask = m;
    if (system_mask) *system_mask = si.dwActiveProcessorMask;
    return TRUE;
}

LONG CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    const uint64_t x = ((uint64_t)a->dwHighDateTime << 32) | a->dwLowDateTime;
    const uint64_t y = ((uint64_t)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}

HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }   /* pseudo handle, as on Windows */
BOOL SetThreadPriority(HANDLE h, int priority) { (void)h; (void)priority; return TRUE; }
int GetThreadPriority(HANDLE h) { (void)h; return THREAD_PRIORITY_NORMAL; }
HMODULE GetModuleHandleW(const wchar_t *name) { (void)name; return NULL; }
HMODULE GetModuleHandleA(const char *name) { (void)name; return NULL; }
FARPROC GetProcAddress(HMODULE m, const char *name) { (void)m; (void)name; return NULL; }

/* --------------------------------------------------------------------------------------------- CRT stat and find */
int dc_stat64(const char *path, struct _stat64 *st)
{
    char p[MAX_PATH];
    struct stat s;
    if (!path || !st || stat(dc_posix_path(path, p, sizeof p), &s) != 0) return -1;
    memset(st, 0, sizeof *st);
    st->st_size = (__int64)s.st_size;
    st->st_mode = (unsigned)s.st_mode;
    st->st_mtime = s.st_mtime;
    return 0;
}

static void find_to_crt(const WIN32_FIND_DATAA *a, struct _finddata_t *fd)
{
    memset(fd, 0, sizeof *fd);
    fd->attrib = (a->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _A_SUBDIR : 0;
    const uint64_t t = ((uint64_t)a->ftLastWriteTime.dwHighDateTime << 32) | a->ftLastWriteTime.dwLowDateTime;
    fd->time_write = (time_t)(t / 10000000ull - 11644473600ull);
    fd->time_create = fd->time_access = fd->time_write;
    fd->size = a->nFileSizeLow;
    snprintf(fd->name, sizeof fd->name, "%.259s", a->cFileName);   /* the CRT's 260 */
}

intptr_t dc_findfirst(const char *pattern, struct _finddata_t *fd)
{
    WIN32_FIND_DATAA a;
    HANDLE h = FindFirstFileA(pattern, &a);
    if (h == INVALID_HANDLE_VALUE) return -1;
    find_to_crt(&a, fd);
    return (intptr_t)h;
}

int dc_findnext(intptr_t h, struct _finddata_t *fd)
{
    WIN32_FIND_DATAA a;
    if (h == -1 || !FindNextFileA((HANDLE)h, &a)) return -1;
    find_to_crt(&a, fd);
    return 0;
}

int dc_findclose(intptr_t h)
{
    return h != -1 && FindClose((HANDLE)h) ? 0 : -1;
}

#endif /* !_WIN32 */
