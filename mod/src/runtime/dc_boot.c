/* dc_boot.c — the game starts on its own, without a launcher script.
 *
 * The mod is switched on entirely through environment variables, and the
 * built-in defaults are the framework's, not the mod's: with nothing set,
 * DC_GPU_RASTER, DC_WIDESCREEN and DC_NATIVE_OTS are all 0 and the picture is
 * the plain original one. That is why the release used to ship a .bat whose
 * only job was to export three dozen variables before starting the exe.
 *
 * The exe now does that itself, at the very first line of main():
 *
 *   1. moves the working directory to the data folder next to the exe (or to
 *      the exe's own folder when there is no data folder), so the settings,
 *      the controls and the game's own executable are found no matter where it
 *      was started from (a shortcut, a game launcher, another drive). The
 *      install folder holds only the exe, SDL2.dll and data\ (17.09.2026,
 *      player: "remove every extra file left after setup");
 *   2. applies the startup settings - dc_env.cfg, built into the program
 *      (dc_pak.c) - filling in every value that is NOT already present in the
 *      real environment, so `set DC_WIDESCREEN=0` in a console still wins,
 *      exactly as `if not defined` did in the .bat;
 *   3. writes DinoCrisisME_log.txt next to the exe, anew at every start
 *      (23.10.2026, player, after the first crash reports on GitHub came with
 *      nothing to go by: "let the log be created in the game folder, not in
 *      data - right in the game folder"). Only what a crash report needs goes
 *      in, in English, with no personal data (the same day, after reading the
 *      first one - see the log section below). Until then a release wrote no
 *      log at all (17.09.2026, player: "this is a release of the mod - no debug
 *      and no debug files"); the other diagnostic files stay off (dc_diag_log).
 *      In data\ there are only dc_user.cfg, input.ini and SLUS_009.22, plus the
 *      memory cards and quicksaves the game makes itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#include "dc_pak.h"
#if defined(__has_include)
#if __has_include("dc_version.h")
#include "dc_version.h"   /* DCME_VERSION: the main menu's label, written by rebuild.ps1 from feedback.html */
#endif
#endif

#ifndef DCME_VERSION
#define DCME_VERSION "Dino Crisis ME"
#endif

/* A value already in the environment wins: the settings only fill in blanks. */
static void env_default(const char *key, const char *val)
{
    const char *cur = getenv(key);
    if (cur && *cur) return;
#ifdef _WIN32
    _putenv_s(key, val);
#else
    setenv(key, val, 0);
#endif
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t')) s[--n] = 0;
}

static void read_env_builtin(const char *name)
{
    DcMemFile *f = dc_mf_open(name);
    if (!f) return;
    char line[512];
    while (dc_mf_gets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key); trim(val);
        /* Only our own variables: the settings must not be able to reach into
         * PATH or anything else the process depends on. */
        if (strncmp(key, "DC_", 3) != 0) continue;
        env_default(key, val);
    }
    dc_mf_close(f);
}

/* The diagnostic files of the mod all hang off this one switch. A release
 * writes none of them, and there is no switch to bring them back: the
 * development build is where problems are chased (17.09.2026). */
int dc_diag_log(void)
{
    return 0;
}

#ifdef _WIN32
/* ---- the log (point 3) ---------------------------------------------------
 * 23.10.2026, player, reading the first log of a release (1200 lines, half of
 * them Russian, the game folder in three of them): "make it fully English, and
 * take out all personal data, so nobody gets angry with me later"; "everything
 * unneeded goes out of the logging, of course". The game keeps printing what it
 * prints (the development build has all of it in hle.txt); here stderr and
 * stdout go into a pipe, and a thread of ours writes into the file only
 *   - the header: when, the version of the mod, Windows, processor, memory;
 *   - the lines a report needs, picked by how they begin (s_keep): the graphics
 *     card and the renderer coming up, the shader preparation, the start
 *     screens, the gamepads, the disc, failures. Each pattern has a budget, so
 *     a fault repeated every frame does not fill the file;
 *   - at a fatal error (crash_trace.c: "fatal: ...") the last lines of the game
 *     before it, whatever they were, and what follows;
 *   - "log: the game closed" at a normal exit: a log without it ended some
 *     other way (a crash with no handler, the process killed).
 * Every line is masked before it is written: the game folder and the user's
 * profile folder become <game> and <home>, the user name and the computer name
 * as a folder of a path (\name\, \\name\) <user> and <pc> - not as a word of
 * the text: "User" is a common account name and "user" a common word - and any
 * other drive path is cut down to its file name. */

#define DC_LOG_LINE  1024        /* a longer line is cut */
#define DC_LOG_RING  24          /* lines kept for the context of a fatal error */
#define DC_LOG_CAP   (2 << 20)   /* bytes: past them nothing more is written */
#define DC_LOG_AFTER 300         /* lines written after a fatal error */

typedef struct { const char *pat; int budget; } LogKeep;
static const LogKeep s_keep[] = {
    /* the graphics card, the renderer, the output */
    { "render: *", 24 }, { "raster: ready*", 2 }, { "raster: bring-up*", 4 }, { "[VKR] shared:*", 6 },
    { "renderer: *", 2 },              /* 25.10.2026: the modern graphics did not start - the reason shown to the player */
    { "present thread: *", 4 },        /* 26.10.2026: frames between ticks on their own thread, or why not (main.cpp) */
    { "dpi: *", 2 },                   /* 26.10.2026: the Windows scale, the window in points and in pixels (main.cpp) */
    { "raster: options in the shader*", 4 }, { "raster: pipelines for*", 4 },   /* 25.10.2026: lean pipelines by the options */
    { "[VKR] post: MSAA up to*", 1 }, { "canvas: *", 4 },
    { "hz: *", 12 },                   /* 28.10.2026: was 4 - the lines on G-Sync / FreeSync and on V-Sync with no refresh grid come 3rd to 6th (main.cpp) */
    { "shaders: *", 12 }, { "pipecache: *", 4 }, { "post: SSAO *", 2 }, { "post: MSAA *", 4 },
    { "graphics: *", 4 },              /* 24.10.2026: the first start check - lowered for a weak graphics card or not (main.cpp) */
    { "vram: *", 12 },                 /* 24.10.2026: video memory in use and what the system gives (gpu_vk_raster.cpp, main.cpp) */
    { "mods: textures - all*", 1 },    /* 24.10.2026: the places for mod textures are fewer on Intel (gpu_vk_raster.cpp) */
    /* the game and its data */
    { "disc: *", 2 }, { "hle: own kernel*", 2 }, { "room library: * rooms*", 1 }, { "mods: game files *", 1 },
    { "dc_settings: dc_user.cfg - *", 1 }, { "psxrecomp: *", 6 },
    { "cdrom: subchannel*", 4 }, { "game: stopped for good*", 2 },   /* 23.10.2026: LibCrypt discs (cdrom.c, main.cpp) */
    /* 23.10.2026: does the machine keep up - frames per second, frames skipped to stay in real time, the sound running dry */
    { "perf: *", 30 }, { "audio: queue*", 12 }, { "preload: *", 1 },
    /* the gamepads */
    { "pad: SDL sees *", 2 }, { "pad:   [*", 8 }, { "pad: the chosen gamepad*", 4 }, { "pad: gamecontrollerdb*", 1 },
    /* where the run is */
    { "splash: language selection*", 2 }, { "splash: about the mod*", 2 }, { "splash: controls layout*", 2 },
    { "splash: closed*", 4 }, { "quit: leaving the game*", 1 },
    { "titlequit: *", 4 },             /* 24.10.2026: the exit item of the main menu - which line of which version, or why none */
    { "talk2: speech *", 6 },          /* 24.10.2026: is the speech on the disc the recording the faces were made for */
    /* failures, wherever they come from */
    { "*FAILED*", 16 }, { "*failed*", 16 }, { "*VK_ERROR*", 8 }, { "*device lost*", 4 }, { "*not found*", 8 },
    { "*cannot *", 8 }, { "* error*", 8 }, { "*ERROR*", 8 },
};
#define DC_LOG_KEEP_N ((int)(sizeof s_keep / sizeof s_keep[0]))

/* '*' stands for any run of characters, everything else for itself */
static int glob_match(const char *p, const char *s)
{
    const char *star = NULL, *back = s;
    while (*s) {
        if (*p == '*') { star = p++; back = s; continue; }
        if (*p == *s) { p++; s++; continue; }
        if (!star) return 0;
        p = star + 1;
        s = ++back;
    }
    while (*p == '*') p++;
    return *p == 0;
}

typedef struct { char text[MAX_PATH * 3]; size_t n; const char *as; } LogMask;
static LogMask s_mask[8];
static int s_mask_n = 0;
/* folder-name masks: the user name and the computer name, each in UTF-8 and in the ANSI code page */
static char s_word[4][128];
static const char *const s_word_as[4] = { "<user>", "<user>", "<pc>", "<pc>" };

static int path_eq(char a, char b)
{
    if (a == '/') a = '\\';
    if (b == '/') b = '\\';
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int is_word(char c)
{
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

/* A folder to mask, in both code pages the game can print it in (A-functions give the ANSI one). */
static void mask_add(const wchar_t *w, const char *as)
{
    static const UINT cps[2] = { CP_UTF8, CP_ACP };
    if (!w || !w[0]) return;
    for (int i = 0; i < 2 && s_mask_n < (int)(sizeof s_mask / sizeof s_mask[0]); i++) {
        LogMask *m = &s_mask[s_mask_n];
        int n = WideCharToMultiByte(cps[i], 0, w, -1, m->text, (int)sizeof m->text, NULL, NULL);
        if (n <= 1) continue;
        m->n = (size_t)n - 1;
        while (m->n && (m->text[m->n - 1] == '\\' || m->text[m->n - 1] == '/')) m->text[--m->n] = 0;
        if (m->n < 3) continue;
        int dup = 0;
        for (int k = 0; k < s_mask_n; k++)
            if (s_mask[k].n == m->n && _strnicmp(s_mask[k].text, m->text, m->n) == 0) dup = 1;
        if (dup) continue;
        m->as = as;
        s_mask_n++;
    }
}

static void word_set(int slot, const wchar_t *w)
{
    static const UINT cps[2] = { CP_UTF8, CP_ACP };
    for (int i = 0; i < 2; i++) {
        char *d = s_word[slot * 2 + i];
        d[0] = 0;
        if (!w || !w[0]) continue;
        int n = WideCharToMultiByte(cps[i], 0, w, -1, d, (int)sizeof s_word[0], NULL, NULL);
        if (n <= 3) d[0] = 0;      /* two letters or fewer would be hit by chance */
    }
}

static void mask_line(const char *s, char *out, size_t cap)
{
    size_t o = 0, i = 0;
    while (s[i] && o + 8 < cap) {
        int hit = 0;
        for (int k = 0; k < s_mask_n && !hit; k++) {
            const LogMask *m = &s_mask[k];
            size_t j = 0;
            while (j < m->n && s[i + j] && path_eq(s[i + j], m->text[j])) j++;
            if (j < m->n) continue;
            size_t a = strlen(m->as);
            if (o + a + 1 >= cap) break;
            memcpy(out + o, m->as, a);
            o += a; i += j; hit = 1;
        }
        if (!hit && i > 0 && (s[i - 1] == '\\' || s[i - 1] == '/')) {
            for (int k = 0; k < 4 && !hit; k++) {
                size_t n = strlen(s_word[k]);
                if (!n || _strnicmp(s + i, s_word[k], n) != 0 || is_word(s[i + n])) continue;
                size_t a = strlen(s_word_as[k]);
                if (o + a + 1 >= cap) break;
                memcpy(out + o, s_word_as[k], a);
                o += a; i += n; hit = 1;
            }
        }
        if (!hit) out[o++] = s[i++];
    }
    out[o] = 0;
    /* any other drive path: only what follows its last separator stays */
    static const char tag[] = "<path>";
    for (i = 0; out[i]; i++) {
        if (!isalpha((unsigned char)out[i]) || out[i + 1] != ':' || (out[i + 2] != '\\' && out[i + 2] != '/')) continue;
        if (i > 0 && is_word(out[i - 1])) continue;
        size_t last = i + 2;
        for (size_t j = i + 3; out[j]; j++) if (out[j] == '\\' || out[j] == '/') last = j;
        if (last - i < sizeof tag - 1) continue;
        memmove(out + i + sizeof tag - 1, out + last, strlen(out + last) + 1);
        memcpy(out + i, tag, sizeof tag - 1);
        i += sizeof tag - 2;
    }
}

static HANDLE s_log_file = INVALID_HANDLE_VALUE;
static HANDLE s_log_read = NULL;
static HANDLE s_log_event = NULL;      /* set by the reader at one of our marks (log_mark) */
static long long s_log_bytes = 0;
static int s_log_capped = 0;
static DWORD s_log_t0 = 0;

static void log_raw(const char *s, size_t n)
{
    DWORD put = 0;
    if (s_log_file == INVALID_HANDLE_VALUE || s_log_capped) return;
    if (s_log_bytes + (long long)n > DC_LOG_CAP) {
        static const char note[] = "log: 2 MB reached - the rest of this run is not written\r\n";
        s_log_capped = 1;
        WriteFile(s_log_file, note, (DWORD)(sizeof note - 1), &put, NULL);
        return;
    }
    if (WriteFile(s_log_file, s, (DWORD)n, &put, NULL)) s_log_bytes += put;
}

/* one line of the log: masked, with the seconds since the start in front */
static void log_put(DWORD ms, const char *line)
{
    static char masked[DC_LOG_LINE + 64], out[DC_LOG_LINE + 96];
    mask_line(line, masked, sizeof masked);
    int n = snprintf(out, sizeof out, "[%5lu.%02lu] %s\r\n", (unsigned long)(ms / 1000u),
                     (unsigned long)(ms % 1000u / 10u), masked);
    if (n > 0) log_raw(out, (size_t)n < sizeof out ? (size_t)n : sizeof out - 1);
}

typedef struct { DWORD ms; char text[DC_LOG_LINE]; } LogRingLine;
static LogRingLine s_ring[DC_LOG_RING];
static int s_ring_n = 0, s_ring_at = 0;
static int s_keep_used[DC_LOG_KEEP_N];
static unsigned char s_keep_noted[DC_LOG_KEEP_N];
static int s_fatal_seen = 0, s_after = 0;

/* 23.10.2026 (the player: "убери такие повторы" - 24 of the 56 lines of his run were the same two lines, the frame size of the
 * 480-line screens and back): a kept line that is word for word one already written in this run is not written again; how
 * many were left out is said once at the end */
#define DC_LOG_SEEN 2048                 /* hashes of the written lines (open addressing, filled up to half) */
static unsigned long long s_seen[DC_LOG_SEEN];
static int s_seen_n = 0;
static long s_repeats = 0;
static unsigned long long log_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    return h ? h : 1ull;
}
static int log_seen(unsigned long long h, int remember)   /* 1 - a line with this hash is already in the log */
{
    unsigned i = (unsigned)(h ^ (h >> 32)) & (DC_LOG_SEEN - 1);
    for (int probe = 0; probe < DC_LOG_SEEN; probe++, i = (i + 1) & (DC_LOG_SEEN - 1)) {
        if (s_seen[i] == h) return 1;
        if (!s_seen[i]) {
            if (remember && s_seen_n < DC_LOG_SEEN / 2) { s_seen[i] = h; s_seen_n++; }
            return 0;
        }
    }
    return 0;
}

static void log_line(char *s)
{
    const DWORD ms = GetTickCount() - s_log_t0;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    if (!n) return;
    if (s[0] == '\x01') {                                  /* our marks (log_mark) */
        if (strcmp(s + 1, "end") == 0) {
            if (s_repeats > 0) {
                char note[96];
                snprintf(note, sizeof note, "log: %ld repeats of lines already above were left out", s_repeats);
                log_put(ms, note);
            }
            log_put(ms, "log: the game closed");
        }
        SetEvent(s_log_event);
        return;
    }
    if (!s_fatal_seen && strncmp(s, "fatal: ", 7) == 0) {  /* crash_trace.c: an error the game cannot go on from */
        s_fatal_seen = 1;
        log_put(ms, "--- the last lines of the game before the error ---");
        for (int i = 0; i < s_ring_n; i++) {
            const LogRingLine *r = &s_ring[(s_ring_at - s_ring_n + i + DC_LOG_RING) % DC_LOG_RING];
            log_put(r->ms, r->text);
        }
        log_put(ms, "---");
        log_put(ms, s);
        return;
    }
    if (s_fatal_seen) {
        if (s_after++ < DC_LOG_AFTER) log_put(ms, s);
        return;
    }
    LogRingLine *r = &s_ring[s_ring_at];
    r->ms = ms;
    snprintf(r->text, sizeof r->text, "%s", s);
    s_ring_at = (s_ring_at + 1) % DC_LOG_RING;
    if (s_ring_n < DC_LOG_RING) s_ring_n++;
    int spent = -1;
    unsigned long long h = 0;
    for (int k = 0; k < DC_LOG_KEEP_N; k++) {
        if (!glob_match(s_keep[k].pat, s)) continue;
        if (!h) { h = log_hash(s); if (log_seen(h, 0)) { s_repeats++; return; } }   /* the same line is already above */
        if (s_keep_used[k] < s_keep[k].budget) {
            s_keep_used[k]++;
            log_seen(h, 1);
            log_put(ms, s);
            return;
        }
        if (spent < 0) spent = k;
    }
    if (spent >= 0 && !s_keep_noted[spent]) {               /* said once: the budget of this kind is used up */
        char note[160];
        s_keep_noted[spent] = 1;
        snprintf(note, sizeof note, "log: no more lines like '%s' in this run", s_keep[spent].pat);
        log_put(ms, note);
    }
}

static DWORD WINAPI log_reader(LPVOID unused)
{
    static char buf[1 << 16], line[DC_LOG_LINE];
    size_t ln = 0;
    (void)unused;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(s_log_read, buf, (DWORD)sizeof buf, &got, NULL) || got == 0) break;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n') { line[ln] = 0; log_line(line); ln = 0; }
            else if (ln < sizeof line - 1) line[ln++] = buf[i];
        }
    }
    if (ln) { line[ln] = 0; log_line(line); }
    return 0;
}

/* main() gives stderr a buffer flushed once a frame (main.cpp, g_log_buffered), and the shader preparation and the
 * start screens come before the game's first frame - pushed out ten times a second here, so a crash there still
 * finds the last lines in the log, and the seconds in front of a line are not off by half a second. */
static DWORD WINAPI log_flusher(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(100);
        fflush(stderr);
        fflush(stdout);
    }
}

/* A mark through the pipe; back when the reader has handled everything before it (or after a second). */
static void log_mark(const char *mark)
{
    if (!s_log_event) return;
    ResetEvent(s_log_event);
    fprintf(stderr, "\x01%s\n", mark);
    fflush(stderr);
    WaitForSingleObject(s_log_event, 1000);
}

static void log_sync(void) { log_mark("sync"); }
static void log_at_exit(void) { log_mark("end"); }

extern void (*psx_log_sync_fn)(void);     /* crash_trace.c: called after "fatal: ..." */

/* What the log needs before the game says anything. The graphics card comes from the renderer itself (pt_vulkan.cpp,
 * the "render: Vulkan" lines), the disc from game.toml (main.cpp, "disc: game id"). No path: where the game is
 * installed is nobody's business. */
static void log_header(void)
{
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(line, sizeof line, "%s - log of this run (written anew at every start), %04u-%02u-%02u %02u:%02u:%02u\r\n",
             DCME_VERSION, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    log_raw(line, strlen(line));
    {   /* GetVersionEx answers what the manifest declares; RtlGetVersion - the real build (Windows 11 is 10.0.22000 and up) */
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        OSVERSIONINFOW v;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        RtlGetVersionFn f = nt ? (RtlGetVersionFn)(void (*)(void))GetProcAddress(nt, "RtlGetVersion") : NULL;
        if (f && f(&v) == 0) {
            snprintf(line, sizeof line, "Windows %lu.%lu build %lu%s\r\n", v.dwMajorVersion, v.dwMinorVersion,
                     v.dwBuildNumber, (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) ? " (Windows 11)" : "");
            log_raw(line, strlen(line));
        }
    }
    {
        char cpu[128] = "";
        DWORD sz = sizeof cpu - 1, type = 0;
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "ProcessorNameString", NULL, &type, (BYTE *)cpu, &sz) != ERROR_SUCCESS || type != REG_SZ)
                cpu[0] = 0;
            RegCloseKey(k);
        }
        cpu[sizeof cpu - 1] = 0;
        trim(cpu);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        MEMORYSTATUSEX ms;
        memset(&ms, 0, sizeof ms);
        ms.dwLength = sizeof ms;
        GlobalMemoryStatusEx(&ms);
        snprintf(line, sizeof line, "processor: %s, %lu threads; memory %.1f GB\r\n", cpu[0] ? cpu : "?",
                 (unsigned long)si.dwNumberOfProcessors, (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        log_raw(line, strlen(line));
    }
}

/* stderr and stdout into the pipe, the reader and the flusher started. 1 - the log is on. The working directory is
 * data\ by now - hence the full path of the file, taken in UTF-16 (a folder name the ANSI code page cannot spell
 * still opens). */
static int log_open(void)
{
    wchar_t dir[MAX_PATH + 1], path[MAX_PATH + 40], v[MAX_PATH + 1];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = (n > 0 && n < MAX_PATH) ? wcsrchr(dir, L'\\') : NULL;
    if (!slash) return 0;
    *slash = 0;
    _snwprintf(path, MAX_PATH + 39, L"%ls\\DinoCrisisME_log.txt", dir);
    path[MAX_PATH + 39] = 0;
    s_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_log_file == INVALID_HANDLE_VALUE) return 0;

    mask_add(dir, "<game>");
    n = GetEnvironmentVariableW(L"USERPROFILE", v, MAX_PATH);
    if (n > 0 && n < MAX_PATH) mask_add(v, "<home>");
    n = GetEnvironmentVariableW(L"USERNAME", v, MAX_PATH);
    if (n > 0 && n < MAX_PATH) word_set(0, v);
    n = GetEnvironmentVariableW(L"COMPUTERNAME", v, MAX_PATH);
    if (n > 0 && n < MAX_PATH) word_set(1, v);

    s_log_t0 = GetTickCount();
    log_header();

    HANDLE wr = NULL;
    if (!CreatePipe(&s_log_read, &wr, NULL, 1u << 20)) {
        static const char note[] = "log: no pipe for the game's lines - nothing more is written\r\n";
        log_raw(note, sizeof note - 1);
        return 0;
    }
    int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY);
    if (fd < 0) {
        CloseHandle(wr);
        return 0;
    }
    if (_dup2(fd, _fileno(stderr)) != 0 || _dup2(fd, _fileno(stdout)) != 0) {
        _close(fd);
        return 0;
    }
    _close(fd);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    s_log_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 64 * 1024, log_reader, NULL, 0, NULL);
    if (t) CloseHandle(t);
    t = CreateThread(NULL, 64 * 1024, log_flusher, NULL, 0, NULL);
    if (t) CloseHandle(t);
    psx_log_sync_fn = log_sync;
    atexit(log_at_exit);
    return 1;
}
#endif

void dc_boot_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

#ifdef _WIN32
    /* Work from the folder the exe lives in. The launcher script did this with
     * `cd /d "%~dp0"`; without it a shortcut with the wrong "Start in" leaves
     * the game looking for its files in someone else's directory. */
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = 0;
            /* data\ first: that is where the game keeps everything it reads
             * and writes. The exe folder only when there is no data folder. */
            char data[MAX_PATH + 8];
            snprintf(data, sizeof data, "%s\\data", exe);
            DWORD attr = GetFileAttributesA(data);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                SetCurrentDirectoryA(data);
            else
                SetCurrentDirectoryA(exe);
        }
    }
#endif

    read_env_builtin("dc_env.cfg");

    /* The log (point 3). Both streams first go onto NUL: a GUI program starts
     * with no stderr at all, and a write to a closed stream would end the
     * process. When the game folder is not writable, they stay there, as
     * before. _IONBF, not _IOLBF: MSVC's ucrtbase rejects a line-buffered
     * stream with a NULL buffer through the invalid-parameter handler, and that
     * kills the process before main() runs. */
    FILE *r = freopen("NUL", "w", stderr);
    FILE *r2 = freopen("NUL", "w", stdout);
    (void)r; (void)r2;
#ifdef _WIN32
    log_open();
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}
