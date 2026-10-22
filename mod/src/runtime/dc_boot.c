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
 *   3. sends stdout and stderr to the null device. A windowed program has no
 *      console, and a release writes no log (17.09.2026, player: "this is a
 *      release of the mod - no debug and no debug files"). In data\ there are
 *      only dc_user.cfg, input.ini and SLUS_009.22, plus the memory cards and
 *      quicksaves the game makes itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include "dc_pak.h"

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

    /* No log. The streams still have to point somewhere writable: this is the
     * same redirection the log file used (tested in the V1 release), only onto
     * NUL. _IONBF, not _IOLBF: MSVC's ucrtbase rejects a line-buffered stream
     * with a NULL buffer through the invalid-parameter handler, and that kills
     * the process before main() runs. */
    FILE *r = freopen("NUL", "w", stderr);
    (void)r;
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef _WIN32
    /* stdout joins stderr through the SAME descriptor rather than a second
     * freopen: two streams over one target keep two positions. */
    _dup2(_fileno(stderr), _fileno(stdout));
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
}
