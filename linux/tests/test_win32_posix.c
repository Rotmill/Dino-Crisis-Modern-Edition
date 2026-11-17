/* The Win32 subset over POSIX (mod/src/runtime/dc_win32_posix.c) behaves the way the mod runtime expects. */
#include "dc_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static volatile LONG s_hits;
static HANDLE s_go;

static DWORD WINAPI worker(LPVOID arg)
{
    WaitForSingleObject(s_go, INFINITE);
    InterlockedOr(&s_hits, (LONG)(intptr_t)arg);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *root = argv[1];
    char p[MAX_PATH], q[MAX_PATH];

    /* a tree like mods/: Textures/Index.txt, sound/BGM/a.wav */
    snprintf(p, sizeof p, "%s/Textures", root); CHECK(mkdir(p, 0755) == 0);
    snprintf(p, sizeof p, "%s/Textures/Index.txt", root);
    FILE *f = fopen(p, "wb"); CHECK(f); fputs("hello", f); fclose(f);
    snprintf(p, sizeof p, "%s\\sound", root); CHECK(CreateDirectoryA(p, NULL));
    snprintf(p, sizeof p, "%s\\SOUND\\BGM", root); CHECK(CreateDirectoryA(p, NULL));   /* parent found ignoring case */
    snprintf(p, sizeof p, "%s/sound/BGM", root); CHECK(GetFileAttributesA(p) & FILE_ATTRIBUTE_DIRECTORY);

    /* '\' separators and the case of every component */
    snprintf(p, sizeof p, "%s\\textures\\index.TXT", root);
    f = dc_fopen(p, "rb"); CHECK(f);
    char buf[16] = {0}; CHECK(fread(buf, 1, 15, f) == 5); fclose(f);
    CHECK(!strcmp(buf, "hello"));
    dc_posix_path(p, q, sizeof q);
#ifdef __linux__
    CHECK(strstr(q, "/Textures/Index.txt") != NULL);   /* the real names (a case-insensitive disk keeps what it got) */
#endif
    CHECK(GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES);
    snprintf(p, sizeof p, "%s\\nothing\\here.txt", root);
    CHECK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES);

    /* listing: a mask, an exact name, sizes */
    snprintf(p, sizeof p, "%s\\TEXTURES\\*.txt", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(!strcmp(fd.cFileName, "Index.txt") && fd.nFileSizeLow == 5);
    CHECK(!FindNextFileA(h, &fd));
    FindClose(h);
    snprintf(p, sizeof p, "%s\\textures\\INDEX.txt", root);
    h = FindFirstFileA(p, &fd);
    CHECK(h != INVALID_HANDLE_VALUE && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    FindClose(h);
    struct _stat64 st;
    CHECK(_stat64(p, &st) == 0 && st.st_size == 5);

    /* wide strings: UTF-8 <-> UTF-32, _wfopen, FindFirstFileW */
    const char *u8 = "Регина_é";
    wchar_t w[64];
    const int nw = MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 64);
    CHECK(nw == 9 && w[0] == 0x0420 && w[7] == 0x00E9);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0) == nw);
    char back[64];
    CHECK(WideCharToMultiByte(CP_UTF8, 0, w, -1, back, 64, NULL, NULL) == (int)strlen(u8) + 1 && !strcmp(back, u8));
    wchar_t wp[MAX_PATH];
    snprintf(p, sizeof p, "%s\\textures\\index.txt", root);
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wp, MAX_PATH);
    f = _wfopen(wp, L"rb"); CHECK(f); fclose(f);
    CHECK(!_wcsicmp(L"Index.TXT", L"index.txt") && _wcsicmp(L"a", L"b") < 0);
    WIN32_FIND_DATAW fw;
    snprintf(p, sizeof p, "%s\\sound\\*", root);
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wp, MAX_PATH);
    h = FindFirstFileW(wp, &fw);
    CHECK(h != INVALID_HANDLE_VALUE);
    int found = 0;
    do found |= !wcscmp(fw.cFileName, L"BGM"); while (FindNextFileW(h, &fw));
    FindClose(h);
    CHECK(found);

    /* the executable path */
    CHECK(GetModuleFileNameA(NULL, p, sizeof p) > 0 && p[0] == '/');

    /* events and waitable threads */
    s_go = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(s_go);
    CHECK(WaitForSingleObject(s_go, 10) == WAIT_TIMEOUT);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) { th[i] = CreateThread(NULL, 64 * 1024, worker, (LPVOID)(intptr_t)(1 << i), 0, NULL); CHECK(th[i]); }
    SetEvent(s_go);
    CHECK(WaitForMultipleObjects(4, th, TRUE, 5000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);
    CHECK(s_hits == 15);
    HANDLE ev = CreateEvent(NULL, FALSE, TRUE, NULL);                 /* auto-reset: one wait takes it */
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0 && WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
    CloseHandle(ev);
    CloseHandle(s_go);
    SYSTEM_INFO si; GetSystemInfo(&si);
    CHECK(si.dwNumberOfProcessors >= 1);

    puts("win32 over POSIX test: ok");
    return 0;
}
