/* dc_pak.c - the game's data built into the program (see dc_pak.h).
 *
 * The resource DCME_DATA is dcme_data.pak, written by mod/tools/pack_data.py
 * while setup runs. Little-endian:
 *
 *   "DCMEPAK1"  u32 count
 *   count times: u16 name_len, name (name_len bytes + a zero byte),
 *                u32 offset (from the start of the pack), u32 size
 *   the files, each followed by a zero byte
 *
 * The resource is mapped with the image and never freed, so the pointers
 * handed out stay valid for as long as the game runs.
 *
 * 04.10.2026 (V3), player: "don't forget mod support - a mods folder with
 * subfolders in the folder the game is installed to". A file put into
 * <game folder>\mods\<subfolder>\ under the name the game knows replaces the
 * built-in one (dc_mf_open looks there first). Read from mods today: the rooms
 * (mods\rooms\room_NNN.dcroom, the room library) and the camera collision
 * (mods\collision\STxxx.obj). Setup also makes the folders for what comes
 * next: characters, character_textures, textures, items, sounds, voice. */

#include "dc_pak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct { const char *name; const unsigned char *data; uint32_t size; } PakEntry;

static PakEntry *s_ent   = NULL;
static int       s_count = -1;          /* -1: not read yet */

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void pak_open(void)
{
    if (s_count >= 0) return;
    s_count = 0;
#ifdef _WIN32
    /* 10 = RT_RCDATA; spelled as a number so the A/W flavour of the macro
     * does not matter */
    HRSRC   rs = FindResourceA(NULL, "DCME_DATA", MAKEINTRESOURCEA(10));
    HGLOBAL hg = rs ? LoadResource(NULL, rs) : NULL;
    const unsigned char *p = hg ? (const unsigned char *)LockResource(hg) : NULL;
    uint32_t n = rs ? (uint32_t)SizeofResource(NULL, rs) : 0;
    if (!p || n < 12 || memcmp(p, "DCMEPAK1", 8) != 0) return;
    uint32_t count = rd32(p + 8);
    if (count == 0 || count > 65536) return;
    PakEntry *ent = (PakEntry *)calloc(count, sizeof *ent);
    if (!ent) return;
    uint32_t at = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (at + 2 > n) { free(ent); return; }
        uint32_t nl = rd16(p + at);
        if (at + 2 + nl + 1 + 8 > n || p[at + 2 + nl] != 0) { free(ent); return; }
        uint32_t off = rd32(p + at + 2 + nl + 1);
        uint32_t sz  = rd32(p + at + 2 + nl + 5);
        if (off > n || sz > n - off || off + sz >= n || p[off + sz] != 0) { free(ent); return; }
        ent[i].name = (const char *)(p + at + 2);
        ent[i].data = p + off;
        ent[i].size = sz;
        at += 2 + nl + 1 + 8;
    }
    s_ent = ent;
    s_count = (int)count;
#endif
}

int dc_pak_norm(const char *path, char *out, size_t cap)
{
    if (!path || !out || cap == 0) return 0;
    const char *s = path;
    while (s[0] == '.' && (s[1] == '/' || s[1] == '\\')) s += 2;
    size_t o = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '\\') c = '/';
        if (c == '/' && (o == 0 || out[o - 1] == '/')) continue;   /* no leading or doubled separators */
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (o + 1 >= cap) { out[0] = 0; return 0; }
        out[o++] = c;
    }
    out[o] = 0;
    return 1;
}

const unsigned char *dc_pak_find(const char *path, size_t *size)
{
    pak_open();
    char key[512];
    if (!dc_pak_norm(path, key, sizeof key)) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_ent[i].name, key) != 0) continue;
        if (size) *size = s_ent[i].size;
        return s_ent[i].data;
    }
    return NULL;
}

int dc_pak_count(void)
{
    pak_open();
    return s_count;
}

const char *dc_pak_name(int i)
{
    pak_open();
    return (i >= 0 && i < s_count) ? s_ent[i].name : "";
}

struct DcMemFile { const unsigned char *p; size_t n, pos; unsigned char *own; };

/* pack folder -> mods subfolder; only files lying directly in the folder */
static const struct { const char *pak; const char *mods; } k_mod_dirs[] = {
    { "exported_rooms/library/",   "rooms" },
    { "exported_rooms/collision/", "collision" },
};

/* <folder of the exe>\mods\<subfolder>\<name> for a normalised pack name, 0 when the name has no mods folder.
 * 10.10.2026: the mods folder the loaders read - mods itself or, with mod packages on, their view data\mods_view (dc_mods.c) */
static int mods_path(const char *key, char *out, size_t cap)
{
#ifdef _WIN32
    extern const char *dc_mods_dir(void);
    char exe[MAX_PATH];
    const char *base = dc_mods_dir();
    if (!base) {                                  /* DC_MODS=0: the mods folder next to the exe, as before */
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
        if (n == 0 || n >= sizeof exe) return 0;
        char *slash = strrchr(exe, '\\');
        if (!slash) return 0;
        slash[1] = 0;
        if (strlen(exe) + 6 >= sizeof exe) return 0;
        strcat(exe, "mods\\");
        base = exe;
    }
    for (size_t i = 0; i < sizeof k_mod_dirs / sizeof k_mod_dirs[0]; i++) {
        size_t L = strlen(k_mod_dirs[i].pak);
        if (strncmp(key, k_mod_dirs[i].pak, L) != 0) continue;
        const char *rest = key + L;
        if (!*rest || strchr(rest, '/')) return 0;
        int k = snprintf(out, cap, "%s%s\\%s", base, k_mod_dirs[i].mods, rest);
        return k > 0 && (size_t)k < cap;
    }
#else
    (void)key; (void)out; (void)cap;
#endif
    return 0;
}

/* the whole file of a mod, followed by a zero byte like the pack entries */
static DcMemFile *mods_open(const char *path)
{
    char key[512], mp[1024];
    if (!dc_pak_norm(path, key, sizeof key) || !mods_path(key, mp, sizeof mp)) return NULL;
    FILE *fp = fopen(mp, "rb");
    if (!fp) return NULL;
    DcMemFile *f = NULL;
    unsigned char *buf = NULL;
    long n = -1;
    if (fseek(fp, 0, SEEK_END) == 0) n = ftell(fp);
    if (n >= 0 && fseek(fp, 0, SEEK_SET) == 0 && (buf = (unsigned char *)malloc((size_t)n + 1)) != NULL &&
        fread(buf, 1, (size_t)n, fp) == (size_t)n && (f = (DcMemFile *)malloc(sizeof *f)) != NULL) {
        buf[n] = 0;
        f->p = buf; f->n = (size_t)n; f->pos = 0; f->own = buf;
    } else {
        free(buf);
    }
    fclose(fp);
    return f;
}

DcMemFile *dc_mf_open(const char *path)
{
    DcMemFile *m = mods_open(path);
    if (m) return m;
    size_t n = 0;
    const unsigned char *p = dc_pak_find(path, &n);
    if (!p) return NULL;
    DcMemFile *f = (DcMemFile *)malloc(sizeof *f);
    if (!f) return NULL;
    f->p = p; f->n = n; f->pos = 0; f->own = NULL;
    return f;
}

char *dc_mf_gets(char *buf, int n, DcMemFile *f)
{
    if (!f || !buf || n <= 0 || f->pos >= f->n) return NULL;
    int o = 0;
    while (o < n - 1 && f->pos < f->n) {
        unsigned char c = f->p[f->pos];
        if (c == '\r' && f->pos + 1 < f->n && f->p[f->pos + 1] == '\n') { f->pos++; continue; }
        buf[o++] = (char)c;
        f->pos++;
        if (c == '\n') break;
    }
    buf[o] = 0;
    return buf;
}

size_t dc_mf_read(void *dst, size_t size, size_t count, DcMemFile *f)
{
    if (!f || !dst || size == 0 || count == 0) return 0;
    size_t items = (f->n - f->pos) / size;
    if (items > count) items = count;
    memcpy(dst, f->p + f->pos, items * size);
    f->pos += items * size;
    return items;
}

void dc_mf_close(DcMemFile *f)
{
    if (f) free(f->own);
    free(f);
}

/* ---- V3 (04.10.2026): the program's own data and the stand-ins of the loaders -------------------------------------
 *
 * Our data that used to be compiled in as C arrays (dc_embedded.c: shaders, aiming poses, screen pictures) is in the
 * pack too, under embed/<name> - a resource costs nothing to build, a 60 MB array source did. The loaders that the dev
 * tree grew after V1 (texture-mark lists, faces, camera curves, post and shadow settings) call the file functions below
 * instead of the C runtime's in a release (release_edits.py, v3_data_inside). The packed files never change while the
 * game runs: their "time" is always 1, so a loader that reloads changed files reads each one once. */

/* By file name, without the folder: the dev tree asks for "shaders\\dc_raster.frag.spv" in places a text edit cannot
 * reach (a name picked by ?:, names handed to pp_mkPipe / fx_mkPost), and V3 first shipped with exactly those shaders
 * not found - no raster at all: the native 320x240 picture, and no room of ours (04.10.2026, the player's first try). */
const unsigned char *dc_embedded(const char *name, unsigned *size)
{
    char key[320];
    if (!name) return NULL;
    for (const char *s = name; *s; s++)
        if (*s == '/' || *s == '\\') name = s + 1;
    snprintf(key, sizeof key, "embed/%s", name);
    size_t n = 0;
    const unsigned char *p = dc_pak_find(key, &n);
    if (p && size) *size = (unsigned)n;
    return p;
}

#ifdef _WIN32
int dcme_file_attr(const char *path, void *wfad)
{
    size_t n = 0;
    if (!dc_pak_find(path, &n)) return 0;
    WIN32_FILE_ATTRIBUTE_DATA *fa = (WIN32_FILE_ATTRIBUTE_DATA *)wfad;
    memset(fa, 0, sizeof *fa);
    fa->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    fa->nFileSizeLow = (DWORD)n;
    fa->ftLastWriteTime.dwLowDateTime = 1;
    return 1;
}

int dcme_stat64(const char *path, struct _stat64 *st)
{
    char key[512];
    if (!path || !st || !dc_pak_norm(path, key, sizeof key)) return -1;
    memset(st, 0, sizeof *st);
    size_t n = 0;
    if (dc_pak_find(key, &n)) { st->st_size = (__int64)n; st->st_mtime = 1; st->st_mode = _S_IFREG; return 0; }
    pak_open();
    const size_t L = strlen(key);
    for (int i = 0; i < s_count; i++)
        if (!strncmp(s_ent[i].name, key, L) && s_ent[i].name[L] == '/') { st->st_mode = _S_IFDIR; st->st_mtime = 1; return 0; }
    return -1;
}

/* _findfirst over the packed names, for a pattern "<folder>/*<end>" (one search at a time - the only caller is the
 * faces folder scan) */
static char s_fd_dir[400], s_fd_end[64];
static int  s_fd_pos = -1;

int dcme_findnext(intptr_t h, struct _finddata_t *fd)
{
    (void)h;
    if (s_fd_pos < 0 || !fd) return -1;
    pak_open();
    const size_t ld = strlen(s_fd_dir), le = strlen(s_fd_end);
    for (int i = s_fd_pos; i < s_count; i++) {
        const char *nm = s_ent[i].name;
        if (strncmp(nm, s_fd_dir, ld) != 0) continue;
        const char *rest = nm + ld;
        const size_t lr = strlen(rest);
        if (!*rest || strchr(rest, '/') || lr < le || strcmp(rest + lr - le, s_fd_end) != 0) continue;
        memset(fd, 0, sizeof *fd);
        snprintf(fd->name, sizeof fd->name, "%s", rest);
        fd->size = (_fsize_t)s_ent[i].size;
        fd->time_write = 1;
        s_fd_pos = i + 1;
        return 0;
    }
    s_fd_pos = -1;
    return -1;
}

intptr_t dcme_findfirst(const char *pattern, struct _finddata_t *fd)
{
    char key[512];
    if (!pattern || !dc_pak_norm(pattern, key, sizeof key)) return -1;
    char *star = strrchr(key, '*');
    char *slash = strrchr(key, '/');
    if (!star || !slash || star < slash) return -1;
    slash[1] = 0;
    snprintf(s_fd_dir, sizeof s_fd_dir, "%s", key);
    snprintf(s_fd_end, sizeof s_fd_end, "%s", star + 1);
    s_fd_pos = 0;
    return dcme_findnext(1, fd) == 0 ? 1 : -1;
}

int dcme_findclose(intptr_t h)
{
    (void)h;
    s_fd_pos = -1;
    return 0;
}
#endif
