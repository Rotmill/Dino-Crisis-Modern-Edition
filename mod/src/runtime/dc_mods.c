/* dc_mods — моды: файл из папки mods подменяет файл игры с тем же именем.
 *
 * ⭐ 04.10.2026 (вечер, 7), игрок: «не забудь про поддержку модов — нужна папка mods с подпапками в директории, куда игра
 * установится»; «ещё папки sounds (со своим разделением), characters, videos. В общем, весь контент игры надо сделать
 * доступным для подмены и моддинга. Всё это — подпапки mods»; «всё, что туда положишь с правильным именем, будет подменять
 * контент игры»; «никаких опций по моддингу внутри игры не должно быть… приложение отдельное должно быть для моддинга —
 * там вьювер и по минимуму инструменты» (выгрузка, просмотр и конвертер — в нём, не в игре).
 *
 * ПОДМЕНА — на уровне файлов диска. Игра читает свои файлы секторами через привод (cdrom.c); если в
 * <папка игры>\mods\<подпапка>\ лежит файл с именем файла игры, сектора этого файла приходят из него — для любого
 * контента сразу, логика игры (её таблицы, скрипты, коллизия) не меняется. Подпапки — по тому, что в файле:
 *   rooms\            ST*.DAT — комнаты (с врагами, которые в них живут) и стадии ST1..ST9
 *   doors\            DOOR*, UP*, DOWN*, WIRE* — сцены дверей, лестниц и тросов
 *   characters\       CORE*.DAT — Регина и костюмы; P10..P33 / H10..H33 — её движения и оружие
 *   items\            ITEM*.DAT — картинки предметов
 *   screens\          заставки и экраны: TITLE, CAPLOGO, WARNING, OPTION, LOAD, OVER, CLEAR, ENDING, OMAKE, WIPE…
 *   movie\            MOVIE\*.STR — ролики                                  (V3: videos\)
 *   sound\bgm\        ME*, MF*, MR*, MS*, MT*, M__* — музыкальные банки     (V3: sounds\effects\, sounds\stage\)
 *   sound\se\         STAGE00.XAS — шумы и гул                              (V3: sounds\speech\)
 *   sound\voice\      STAGE01..06.XAS — речь                                (V3: sounds\speech\)
 *   other\            DEMO*.TRG — записи демо-режима
 * (textures — под обычные форматы следующих версий; collision\ и наши комнаты rooms\room_*.dcroom подменяет в релизе
 * dc_pak.c тем же правилом.)
 * Форматы — родные: файлы данных (DAT, TRG) — как есть; потоки (XAS, STR) — сырыми секторами по 2352 байта (или 2336, без
 * синхронизации и заголовка). Файл не может быть длиннее места, которое он занимает на диске: игра читает его по своей
 * таблице — длиннее пропускается со строкой в журнале. Обычные форматы (PNG, WAV, glTF/GLB, FBX, OBJ, видео) — следующими
 * версиями, по категориям (что обязательно конвертировать в формат игры — запись памяти dc_modding_plan).
 *
 * DC_MODS=0 — без подмены; DC_MODS_DIR — своя папка модов (автотест).
 *
 * ⭐ 06.10.2026, player: "the mods must be picked up by themselves... think about optimization, nothing must cost the game".
 * MODS PICKED UP ON THE FLY (the end of this file). A thread waits on Windows for changes in the mods folder (it sleeps while
 * nothing changes); the folder of a changed file says what to read again; 0.7 s after the last change the work is handed
 * over. The emulation thread looks at it with one read per drive step; when no command is in flight and no data file was
 * read for 0.5 s (a room is never taken half old, half new), dc_mods_reload reads again only what changed:
 *   game files - by size and time of the mod file (the rest are not touched);
 *   other\SLUS_009.22 and the file table entries of bigger rooms - only bytes that differ from RAM are written;
 *   meshes (dc_meshrep.cpp) - read in their own thread, swapped in with a new frame; models whose folder did not change stay;
 *   textures and screens PNG (gpu_vk_raster.cpp) - a changed picture is decoded again into the same place;
 *   sounds WAV and movies MP4 - a file with the same size and time keeps its decoded sound.
 * The game reads a room, a door scene or a sound bank when it gets there, so a change shows at the next entry.
 * DC_MODS_LIVE=0 - off (the mods are read once, at the start). */
#include "dc_mods.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif

#ifndef MAX_PATH
#  define MAX_PATH 260
#endif

extern int iso_read_sector(void *handle, uint32_t lba, uint8_t *buffer, int size);
extern uint8_t *memory_get_ram_ptr(void);

enum { M_NONE = 0, M_DATA = 1, M_RAW2352 = 2, M_RAW2336 = 3 };

typedef struct {
    char        name[32];          /* имя на диске без ";1" */
    const char *cat;               /* подпапка mods / data */
    const char *old_cat;           /* подпапка V3 (запасная), NULL — та же */
    uint32_t    lba, nsec, size;
    uint8_t     stream;            /* XA / STR: сектора целиком */
    uint8_t     mode;              /* M_* — подменён ли и как читать мод */
    FILE       *fp;
    uint32_t    msize;
    uint32_t    vlba, vnsec;       /* 06.10.2026: a room bigger than its place on the disc - read from virtual sectors */
    uint32_t    vcap;              /* 06.10.2026: virtual sectors kept for it (a later save of the same room reuses them) */
    const char *cat0;              /* 06.10.2026: its own folder (cat may point at the V3 one it was found in) */
    uint64_t    stamp;             /* 06.10.2026: time and size of the mod file when opened (0 - none): what changed */
} DiscFile;

/* 06.10.2026: virtual sectors for room files bigger than their place on the disc: past the end of the disc (~177500
 * sectors with the audio track), below the 99-minute limit of SetLoc (449849) */
static uint32_t s_vnext = 300000u;
static void     *s_iso;
static DiscFile *s_f;
static int       s_n, s_cap, s_active;
static char      s_root[MAX_PATH];        /* папка exe с '\' в конце */

static int env_on(const char *k, int def)
{
    const char *e = getenv(k);
    return e ? (*e != '0') : def;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* 06.10.2026: a mod file is held open while the game runs - with every sharing, so the Studio or any editor can write it or
 * put a new file in its place (picked up on the fly: it is opened again) */
static FILE *open_shared(const char *p)
{
#ifdef _WIN32
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    const int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return NULL; }
    FILE *fp = _fdopen(fd, "rb");
    if (!fp) _close(fd);
    return fp;
#else
    return fopen(p, "rb");
#endif
}

/* 06.10.2026: size and time of a file as its folder lists it (0 - no such file): what "changed" means on the fly */
#ifdef _WIN32
static uint64_t fd_stamp(const WIN32_FIND_DATAA *fd)
{
    const uint64_t t = (uint64_t)fd->ftLastWriteTime.dwHighDateTime << 32 | fd->ftLastWriteTime.dwLowDateTime;
    const uint64_t s = (uint64_t)fd->nFileSizeHigh << 32 | fd->nFileSizeLow;
    return (t ^ (s * 0x9E3779B97F4A7C15ull)) | 1u;
}
#endif
static uint64_t file_stamp(const char *p)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : fd_stamp(&fd);
#else
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
#endif
}

static void root_dir(void)
{
    if (s_root[0]) return;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, s_root, (DWORD)sizeof s_root);
    if (n == 0 || n >= sizeof s_root) { strcpy(s_root, ".\\"); return; }
    char *sl = strrchr(s_root, '\\');
    if (sl) sl[1] = 0; else strcpy(s_root, ".\\");
#else
    strcpy(s_root, "./");
#endif
}

/* 05.10.2026 (игрок: «посмотри для примера на структуру папок в PC-версии… может поменяешь где-то нейминг, иерархию»,
 * «всё это касается mods и source»): по образцу PC (Movie, Sound\BGM, Sound\SE, Sound\VOICE) — movie, sound\bgm,
 * sound\se, sound\voice. Все ME*, MF*, MR*, MS*, MT*, M__* — музыкальные банки (у PC это треки Sound\BGM me_XX, mf_XX…),
 * STAGE00.XAS — шумы и гул (не речь), STAGE01..06.XAS — речь. Прежние папки V3 (videos, sounds\speech, sounds\effects,
 * sounds\stage) читаются запасными — old_cat_of. Та же раскладка — в приложении Dino Crisis ME Studio (папка source). */
static const char *cat_of(const char *dir, const char *n, uint8_t *stream)
{
    *stream = 0;
    if (strcmp(dir, "/PSX/MOVIE/") == 0)   { *stream = 1; return "movie"; }
    if (strcmp(dir, "/PSX/DATA_XA/") == 0) { *stream = 1; return strcmp(n, "STAGE00.XAS") == 0 ? "sound\\se" : "sound\\voice"; }
    /* 05.10: оверлеи этапов (ростер динозавров — данные; код оверлея исполняет интерпретатор из RAM) */
    if (strcmp(dir, "/PSX/BIN/") == 0)
        return (n[0] == 'S' && n[1] == 'T' && n[2] >= '1' && n[2] <= '9' && strcmp(n + 3, ".BIN") == 0) ? "other" : NULL;
    if (strcmp(dir, "/PSX/DATA/") != 0) return NULL;
    if (n[0] == 'S' && n[1] == 'T' && isxdigit((unsigned char)n[2])) return "rooms";
    if (!strncmp(n, "DOOR", 4) || !strncmp(n, "UP", 2) || !strncmp(n, "DOWN", 4) || !strncmp(n, "WIRE", 4)) return "doors";
    if (!strncmp(n, "CORE", 4) ||
        ((n[0] == 'P' || n[0] == 'H') && isdigit((unsigned char)n[1]) && isdigit((unsigned char)n[2]) && n[3] == '.'))
        return "characters";
    if (!strncmp(n, "ITEM", 4)) return "items";
    if (n[0] == 'M' && n[1] && strchr("EFRST_", n[1]) && (n[2] == '_' || isxdigit((unsigned char)n[2]))) return "sound\\bgm";
    if (!strncmp(n, "DEMO", 4)) return "other";
    return "screens";
}

/* папка V3 (04.10) для того же файла — если в новой его нет */
static const char *old_cat_of(const char *dir, const char *n)
{
    if (strcmp(dir, "/PSX/MOVIE/") == 0)   return "videos";
    if (strcmp(dir, "/PSX/DATA_XA/") == 0) return "sounds\\speech";
    if (strcmp(dir, "/PSX/DATA/") != 0) return NULL;
    if (n[0] == 'M' && (n[1] == 'E' || n[1] == 'F') && isxdigit((unsigned char)n[2])) return "sounds\\effects";
    if (n[0] == 'M' && n[1] && n[2] == '_') return "sounds\\stage";
    return NULL;
}

static void add_file(const char *dir, const char *name, uint32_t lba, uint32_t size)
{
    uint8_t stream = 0;
    const char *cat = cat_of(dir, name, &stream);
    if (!cat || size == 0) return;
    if (s_n == s_cap) {
        int nc = s_cap ? s_cap * 2 : 512;
        DiscFile *nf = (DiscFile *)realloc(s_f, (size_t)nc * sizeof *nf);
        if (!nf) return;
        s_f = nf; s_cap = nc;
    }
    DiscFile *f = &s_f[s_n++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->cat = cat; f->cat0 = cat; f->lba = lba; f->size = size; f->nsec = (size + 2047u) / 2048u; f->stream = stream;
    f->old_cat = old_cat_of(dir, name);
}

static void walk(uint32_t lba, uint32_t size, const char *prefix, int depth)
{
    if (depth > 4 || size == 0 || size > 1u << 20) return;
    const uint32_t nsec = (size + 2047u) / 2048u;
    uint8_t *d = (uint8_t *)malloc((size_t)nsec * 2048u);
    if (!d) return;
    for (uint32_t i = 0; i < nsec; i++)
        if (!iso_read_sector(s_iso, lba + i, d + (size_t)i * 2048u, 2048)) { free(d); return; }
    uint32_t off = 0;
    while (off < size) {
        const uint32_t rl = d[off];
        if (rl == 0) { off = (off / 2048u + 1u) * 2048u; continue; }
        if (off + rl > nsec * 2048u || rl < 34) break;
        const uint8_t *r = d + off;
        const uint32_t elba = le32(r + 2), elen = le32(r + 10);
        const uint8_t flags = r[25], nl = r[32];
        off += rl;
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;            /* . и .. */
        char nm[40]; int k = 0;
        for (int i = 0; i < nl && k < 39 && r[33 + i] != ';'; i++) nm[k++] = (char)r[33 + i];
        nm[k] = 0;
        if (flags & 0x02) {
            char sub[96];
            snprintf(sub, sizeof sub, "%s%s/", prefix, nm);
            walk(elba, elen, sub, depth + 1);
        } else {
            add_file(prefix, nm, elba, elen);
        }
    }
    free(d);
}

static int cmp_lba(const void *a, const void *b)
{
    const DiscFile *x = (const DiscFile *)a, *y = (const DiscFile *)b;
    return x->lba < y->lba ? -1 : x->lba > y->lba ? 1 : 0;
}

/* настоящая папка mods (DC_MODS_DIR - своя, автотест) */
static void mods_src(char *out, size_t cap)
{
    const char *e = getenv("DC_MODS_DIR");
    if (e && *e) snprintf(out, cap, "%s\\", e);
    else { root_dir(); snprintf(out, cap, "%smods\\", s_root); }
}

/* ======================================================================================================================
 * ПАКЕТЫ МОДОВ (10.10.2026, игрок: «моды пакетами: каждый мод в своей папке, менеджер в студии - включать, выключать, порядок»).
 * Пакет - подпапка mods с файлом mod.json (пишет студия; игре важно только, что он есть) и той же раскладкой внутри
 * (textures, characters, rooms, sound, other...). Порядок и выключенные - mods\load_order.txt (пишет менеджер студии): строка -
 * имя папки пакета, «-» в начале - выключен; пакеты, которых в списке нет, включены и идут после перечисленных, по алфавиту.
 * Слои: свои файлы mods (вне пакетов) - первый, дальше включённые пакеты по порядку; файл с тем же путём берётся из последнего
 * слоя. Списки сливаются по строкам: textures\index.txt (ключ - 8 полей до пути), screens\index.txt (3), names.txt (3),
 * прочие index.txt моделей (2); строка с тем же ключом - из последнего слоя. other\SLUS_009.22 - байты, которыми каждый слой
 * отличается от программы игры, по порядку (slus_apply). Игра читает собранный вид <папка игры>\data\mods_view\ (жёсткие
 * ссылки на файлы, копия - если ссылка не вышла): загрузчики (комнаты, звуки, ролики, текстуры, модели) читают его как mods и о
 * пакетах не знают. Вид пересобирается при старте и на ходу (сторож mods), если что-то поменялось (отпечаток .stamp). Нет ни
 * одного включённого пакета - вида нет, всё как было. Отчёт - data\mods_view\view.txt (слои, чей файл взят). DC_MODS_PACKS=0 -
 * пакеты не читаются.
 * ====================================================================================================================== */
#ifdef _WIN32
#define VP 1024                                   /* WCHAR в пути вида и пакетов */
typedef struct { WCHAR *rel, *abs; int layer, kind; uint64_t size, mt; } VFile;
enum { VK_PLAIN = 0, VK_LIST = 1, VK_EXE = 2 };
static VFile  *s_vf;
static int     s_nvf, s_cvf;
static WCHAR   s_vlay[65][96];                    /* имена слоёв: [0] - «mods», дальше папки пакетов (до 64) */
static int     s_nvlay;
static WCHAR   s_vexe[65][VP];                    /* other\SLUS_009.22 каждого слоя, по порядку (slus_apply) */
static int     s_nvexe;
static int     s_npack_off;                       /* выключенных пакетов */

static int vlist_fields(const WCHAR *rel)         /* сколько полей - ключ строки списка (0 - не список) */
{
    if (!_wcsicmp(rel, L"names.txt")) return 3;
    if (!_wcsicmp(rel, L"textures\\index.txt")) return 8;
    if (!_wcsicmp(rel, L"screens\\index.txt")) return 3;
    const size_t n = wcslen(rel);
    if (n >= 10 && !_wcsicmp(rel + n - 10, L"\\index.txt")) return 2;
    return 0;
}

static void vf_add(const WCHAR *rel, const WCHAR *abs, int layer, const WIN32_FIND_DATAW *fd)
{
    if (s_nvf == s_cvf) {
        const int nc = s_cvf ? s_cvf * 2 : 256;
        VFile *nv = (VFile *)realloc(s_vf, (size_t)nc * sizeof *nv);
        if (!nv) return;
        s_vf = nv; s_cvf = nc;
    }
    VFile *f = &s_vf[s_nvf++];
    f->rel = _wcsdup(rel); f->abs = _wcsdup(abs);
    f->layer = layer;
    f->kind = !_wcsicmp(rel, L"other\\SLUS_009.22") ? VK_EXE : (vlist_fields(rel) ? VK_LIST : VK_PLAIN);
    f->size = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    f->mt = ((uint64_t)fd->ftLastWriteTime.dwHighDateTime << 32) | fd->ftLastWriteTime.dwLowDateTime;
}

static void vf_free(void)
{
    for (int i = 0; i < s_nvf; i++) { free(s_vf[i].rel); free(s_vf[i].abs); }
    s_nvf = 0;
}

static int is_pack_dir(const WCHAR *dir)
{
    WCHAR p[VP];
    _snwprintf(p, VP, L"%ls\\mod.json", dir); p[VP - 1] = 0;
    const DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* файлы слоя: top - сама папка слоя (там пропускаются пакеты у mods и служебные файлы) */
static void vwalk(const WCHAR *dir, const WCHAR *rel, int layer, int top)
{
    WCHAR pat[VP];
    _snwprintf(pat, VP, L"%ls\\*", dir); pat[VP - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const WCHAR *nm = fd.cFileName;
        if (nm[0] == L'.') continue;              /* «.», «..» и скрытое своё (.stamp) */
        WCHAR sub[VP], srel[VP];
        _snwprintf(sub, VP, L"%ls\\%ls", dir, nm); sub[VP - 1] = 0;
        if (rel[0]) { _snwprintf(srel, VP, L"%ls\\%ls", rel, nm); srel[VP - 1] = 0; }
        else { wcsncpy(srel, nm, VP - 1); srel[VP - 1] = 0; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (top && layer == 0 && is_pack_dir(sub)) continue;   /* пакеты - свои слои */
            vwalk(sub, srel, layer, 0);
        } else {
            if (top && !_wcsicmp(nm, L"load_order.txt")) continue;
            if (top && layer > 0 && (!_wcsicmp(nm, L"mod.json") || !_wcsicmp(nm, L"preview.png") ||
                                     !_wcsicmp(nm, L"readme.txt"))) continue;
            vf_add(srel, sub, layer, &fd);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int vf_cmp(const void *a, const void *b)
{
    const VFile *x = (const VFile *)a, *y = (const VFile *)b;
    const int c = _wcsicmp(x->rel, y->rel);
    return c ? c : (x->layer - y->layer);
}

static void vdel_tree(const WCHAR *dir)
{
    WCHAR pat[VP];
    _snwprintf(pat, VP, L"%ls\\*", dir); pat[VP - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            WCHAR sub[VP];
            _snwprintf(sub, VP, L"%ls\\%ls", dir, fd.cFileName); sub[VP - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) vdel_tree(sub);
            else { SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL); DeleteFileW(sub); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void vmkdirs(const WCHAR *path)           /* все папки пути (последняя часть - тоже папка) */
{
    WCHAR p[VP];
    wcsncpy(p, path, VP - 1); p[VP - 1] = 0;
    for (WCHAR *s = p + 3; *s; s++)
        if (*s == L'\\') { *s = 0; CreateDirectoryW(p, NULL); *s = L'\\'; }
    CreateDirectoryW(p, NULL);
}

static void vparent_dirs(const WCHAR *file)
{
    WCHAR p[VP];
    wcsncpy(p, file, VP - 1); p[VP - 1] = 0;
    WCHAR *sl = wcsrchr(p, L'\\');
    if (sl) { *sl = 0; vmkdirs(p); }
}

static uint64_t vfnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

/* текст файла целиком (UTF-8 как есть) */
static char *vread_all(const WCHAR *p, size_t *n)
{
    FILE *fp = _wfopen(p, L"rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    const long L = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *b = (L >= 0) ? (char *)malloc((size_t)L + 1) : NULL;
    if (b && fread(b, 1, (size_t)L, fp) != (size_t)L) { free(b); b = NULL; }
    fclose(fp);
    if (b) { b[L] = 0; if (n) *n = (size_t)L; }
    return b;
}

/* ключ строки списка: первые nf полей через пробел или табуляцию */
static size_t vline_key(const char *s, int nf)
{
    size_t i = 0;
    for (int f = 0; f < nf; f++) {
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (!s[i] || s[i] == '\r' || s[i] == '\n') return 0;
        while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') i++;
    }
    return i;
}

/* список из всех слоёв: строки в порядке первого появления, строка с тем же ключом - из последнего слоя */
static int vlist_write(const VFile *g, int ng, const WCHAR *dst, int nf)
{
    typedef struct { char *line; size_t klen; } Ln;
    Ln *L = NULL;
    int nl = 0, cl = 0;
    for (int k = 0; k < ng; k++) {
        char *t = vread_all(g[k].abs, NULL);
        if (!t) continue;
        for (char *s = t; *s; ) {
            char *e = s;
            while (*e && *e != '\n') e++;
            const size_t len = (size_t)(e - s) + (*e == '\n' ? 1 : 0);
            if (s[0] != '#' && s[0] != '\r' && s[0] != '\n') {
                const size_t kl = vline_key(s, nf);
                if (kl) {
                    int at = -1;
                    for (int i = 0; i < nl; i++)
                        if (L[i].klen == kl && !memcmp(L[i].line, s, kl)) { at = i; break; }
                    char *ln = (char *)malloc(len + 2);
                    if (ln) {
                        memcpy(ln, s, len); ln[len] = 0;
                        if (len == 0 || ln[len - 1] != '\n') { ln[len] = '\n'; ln[len + 1] = 0; }
                        if (at >= 0) { free(L[at].line); L[at].line = ln; }
                        else {
                            if (nl == cl) { cl = cl ? cl * 2 : 256; L = (Ln *)realloc(L, (size_t)cl * sizeof *L); }
                            L[nl].line = ln; L[nl].klen = kl; nl++;
                        }
                    }
                }
            }
            s += len ? len : 1;
            if (!len) break;
        }
        free(t);
    }
    FILE *fp = _wfopen(dst, L"wb");
    if (fp) {
        for (int i = 0; i < nl; i++) fputs(L[i].line, fp);
        fclose(fp);
    }
    for (int i = 0; i < nl; i++) free(L[i].line);
    free(L);
    return fp != NULL;
}

/* порядок пакетов: load_order.txt, потом не перечисленные - по алфавиту; out - имена включённых */
static int vpack_order(const WCHAR *src, WCHAR (*out)[96], int cap)
{
    static WCHAR names[64][96];
    int nn = 0, no = 0;
    s_npack_off = 0;
    {   /* пакеты на диске */
        WCHAR pat[VP];
        _snwprintf(pat, VP, L"%ls\\*", src); pat[VP - 1] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                WCHAR sub[VP];
                _snwprintf(sub, VP, L"%ls\\%ls", src, fd.cFileName); sub[VP - 1] = 0;
                if (!is_pack_dir(sub) || nn >= 64 || wcslen(fd.cFileName) >= 95) continue;
                wcscpy(names[nn++], fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!nn) return 0;
    int used[64] = { 0 };
    {   /* load_order.txt (UTF-8): имя папки в строке, «-» - выключен, «#» - пометка */
        WCHAR p[VP];
        _snwprintf(p, VP, L"%ls\\load_order.txt", src); p[VP - 1] = 0;
        char *t = vread_all(p, NULL);
        if (t) {
            for (char *s = t; *s; ) {
                char *e = s;
                while (*e && *e != '\n') e++;
                const char save = *e; *e = 0;
                char *b = s;
                while (*b == ' ' || *b == '\t' || (unsigned char)*b == 0xEF || (unsigned char)*b == 0xBB || (unsigned char)*b == 0xBF) b++;
                size_t L = strlen(b);
                while (L && (b[L - 1] == '\r' || b[L - 1] == ' ' || b[L - 1] == '\t')) b[--L] = 0;
                if (L && b[0] != '#') {
                    const int off = b[0] == '-';
                    WCHAR w[96];
                    if (MultiByteToWideChar(CP_UTF8, 0, b + off, -1, w, 96) > 0)
                        for (int i = 0; i < nn; i++)
                            if (!used[i] && !_wcsicmp(names[i], w)) {
                                used[i] = 1;
                                if (off) s_npack_off++;
                                else if (no < cap) wcscpy(out[no++], names[i]);
                                break;
                            }
                }
                *e = save;
                s = *e ? e + 1 : e;
            }
            free(t);
        }
    }
    for (;;) {   /* не перечисленные - по алфавиту */
        int best = -1;
        for (int i = 0; i < nn; i++)
            if (!used[i] && (best < 0 || _wcsicmp(names[i], names[best]) < 0)) best = i;
        if (best < 0) break;
        used[best] = 1;
        if (no < cap) wcscpy(out[no++], names[best]);
    }
    return no;
}

static int  s_view_on;                            /* загрузчики читают вид */
static char s_view_dir[MAX_PATH];                 /* <папка игры>\data\mods_view\ */
static void mods_src(char *out, size_t cap);

/* собрать (или оставить прежним) вид из mods и пакетов; 1 - вид включён */
static int view_prepare(void)
{
    s_nvexe = 0;
    if (!env_on("DC_MODS", 1) || !env_on("DC_MODS_PACKS", 1)) { s_view_on = 0; return 0; }
    char srcA[MAX_PATH];
    mods_src(srcA, sizeof srcA);
    WCHAR src[VP];
    if (!MultiByteToWideChar(CP_ACP, 0, srcA, -1, src, VP)) { s_view_on = 0; return 0; }
    size_t sl = wcslen(src);
    if (sl && (src[sl - 1] == L'\\' || src[sl - 1] == L'/')) src[--sl] = 0;
    static WCHAR packs[64][96];
    const int np = vpack_order(src, packs, 64);
    if (!np) {
        if (s_view_on) fprintf(stderr, "mods: no packages on - the mods folder itself again\n");
        s_view_on = 0;
        return 0;
    }
    root_dir();
    snprintf(s_view_dir, sizeof s_view_dir, "%sdata\\mods_view\\", s_root);
    WCHAR view[VP];
    MultiByteToWideChar(CP_ACP, 0, s_view_dir, -1, view, VP);
    size_t vl = wcslen(view);
    if (vl && view[vl - 1] == L'\\') view[--vl] = 0;
    /* слои и их файлы */
    vf_free();
    s_nvlay = 0;
    wcscpy(s_vlay[s_nvlay++], L"mods");
    vwalk(src, L"", 0, 1);
    for (int i = 0; i < np; i++) {
        WCHAR d[VP];
        _snwprintf(d, VP, L"%ls\\%ls", src, packs[i]); d[VP - 1] = 0;
        wcscpy(s_vlay[s_nvlay], packs[i]);
        vwalk(d, L"", s_nvlay, 1);
        s_nvlay++;
    }
    qsort(s_vf, (size_t)s_nvf, sizeof *s_vf, vf_cmp);
    /* программа игры: файлы слоёв по порядку - их правки накладывает slus_apply */
    for (int i = 0; i < s_nvf; i++)
        if (s_vf[i].kind == VK_EXE && s_nvexe < 65) wcscpy(s_vexe[s_nvexe++], s_vf[i].abs);
    /* отпечаток: слои, пути, размеры, время */
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < s_nvlay; i++) h = vfnv(h, s_vlay[i], wcslen(s_vlay[i]) * sizeof(WCHAR) + sizeof(WCHAR));
    for (int i = 0; i < s_nvf; i++) {
        h = vfnv(h, s_vf[i].rel, wcslen(s_vf[i].rel) * sizeof(WCHAR));
        h = vfnv(h, &s_vf[i].layer, sizeof s_vf[i].layer);
        h = vfnv(h, &s_vf[i].size, sizeof s_vf[i].size);
        h = vfnv(h, &s_vf[i].mt, sizeof s_vf[i].mt);
    }
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%016llX", (unsigned long long)h);
    WCHAR sp[VP];
    _snwprintf(sp, VP, L"%ls\\.stamp", view); sp[VP - 1] = 0;
    {
        char *old = vread_all(sp, NULL);
        const int same = old && !strcmp(old, stamp);
        free(old);
        if (same) {
            if (!s_view_on) fprintf(stderr, "mods: packages - %d on, %d off; the view %s is up to date (%d files)\n",
                                    np, s_npack_off, s_view_dir, s_nvf);
            s_view_on = 1;
            return 1;                              /* как был */
        }
    }
    /* заново: прежний вид удаляется целиком (это ссылки и копии - сами файлы модов не трогаются) */
    const ULONGLONG t0 = GetTickCount64();
    if (vl < 12 || _wcsicmp(view + vl - 10, L"\\mods_view") != 0) { s_view_on = 0; return 0; }   /* защита: удаляем только вид */
    vdel_tree(view);
    vmkdirs(view);
    int linked = 0, copied = 0, failed = 0, lists = 0, conflicts = 0;
    FILE *rep = NULL;
    {
        WCHAR rp[VP];
        _snwprintf(rp, VP, L"%ls\\view.txt", view); rp[VP - 1] = 0;
        rep = _wfopen(rp, L"wb");
        if (rep) {
            fprintf(rep, "\xEF\xBB\xBF# Dino Crisis ME - mods as the game reads them (the game builds this folder, do not edit it)\r\n# layers, later wins:");
            for (int i = 0; i < s_nvlay; i++) {
                char u[300];
                WideCharToMultiByte(CP_UTF8, 0, s_vlay[i], -1, u, sizeof u, NULL, NULL);
                fprintf(rep, " %s%s", i ? "> " : "", u);
            }
            fprintf(rep, "\r\n");
        }
    }
    for (int i = 0; i < s_nvf; ) {
        int j = i + 1;
        while (j < s_nvf && !_wcsicmp(s_vf[j].rel, s_vf[i].rel)) j++;
        const VFile *w = &s_vf[j - 1];            /* последний слой */
        WCHAR dst[VP];
        _snwprintf(dst, VP, L"%ls\\%ls", view, w->rel); dst[VP - 1] = 0;
        vparent_dirs(dst);
        if (w->kind == VK_LIST && j - i > 1) {
            if (vlist_write(&s_vf[i], j - i, dst, vlist_fields(w->rel))) lists++; else failed++;
        } else {
            if (CreateHardLinkW(dst, w->abs, NULL)) linked++;
            else if (CopyFileW(w->abs, dst, FALSE)) copied++;
            else failed++;
            if (j - i > 1 && w->kind == VK_PLAIN) {
                conflicts++;
                if (rep) {
                    char u[VP * 3];
                    WideCharToMultiByte(CP_UTF8, 0, w->rel, -1, u, sizeof u, NULL, NULL);
                    fprintf(rep, "%s:", u);
                    for (int k = i; k < j; k++) {
                        char n8[300];
                        WideCharToMultiByte(CP_UTF8, 0, s_vlay[s_vf[k].layer], -1, n8, sizeof n8, NULL, NULL);
                        fprintf(rep, " %s%s", k > i ? "> " : "", n8);
                    }
                    fprintf(rep, "\r\n");
                }
            }
        }
        i = j;
    }
    if (rep) fclose(rep);
    {
        FILE *fp = _wfopen(sp, L"wb");
        if (fp) { fputs(stamp, fp); fclose(fp); }
        SetFileAttributesW(sp, FILE_ATTRIBUTE_HIDDEN);
    }
    s_view_on = 1;
    fprintf(stderr, "mods: packages - %d on, %d off; the view %s rebuilt: %d linked, %d copied, %d lists merged, %d files "
            "taken from a later layer, %d failed, %u ms\n", np, s_npack_off, s_view_dir, linked, copied, lists, conflicts, failed,
            (unsigned)(GetTickCount64() - t0));
    return 2;                                      /* пересобран */
}
#else
static int  s_view_on;
static char s_view_dir[MAX_PATH];
static int  s_nvexe;
static int view_prepare(void) { return 0; }
#endif

/* папка, которую читают загрузчики: mods, а с включёнными пакетами (10.10) - их вид data\mods_view */
static void mods_base(char *out, size_t cap)
{
    if (s_view_on) snprintf(out, cap, "%s", s_view_dir);
    else mods_src(out, cap);
}

/* 05.10.2026: папка mods с '\' в конце для растра (текстуры PNG, gpu_vk_raster.cpp); NULL — моды выключены (DC_MODS=0).
 * 10.10: каждый раз заново - вид пакетов появляется и пропадает на ходу */
const char *dc_mods_dir(void)
{
    static char base[MAX_PATH];
    if (!env_on("DC_MODS", 1)) return NULL;
    mods_base(base, sizeof base);
    return base;
}

/* подпапки mods с файлами игры (cat_of, old_cat_of) */
static const char *const cats[] = { "rooms", "doors", "characters", "items", "screens", "movie", "sound\\bgm",
                                    "sound\\se", "sound\\voice", "other",
                                    /* папки V3 */ "sounds\\effects", "sounds\\stage", "sounds\\speech", "videos" };

/* файлы в подпапках mods, которых нет среди файлов игры (наши комнаты и коллизию разбирает dc_pak.c) — в журнал */
static void report_strangers(const char *base)
{
#ifdef _WIN32
    for (size_t c = 0; c < sizeof cats / sizeof cats[0]; c++) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s%s\\*", base, cats[c]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const char *nm = fd.cFileName;
            const char *dot = strrchr(nm, '.');
            if (dot && (!_stricmp(dot, ".dcroom") || !_stricmp(dot, ".obj") || !_stricmp(nm, "manifest.txt") ||
                        !_stricmp(nm, "rooms_lba.txt"))) continue;
            if (!_stricmp(nm, "SLUS_009.22")) continue;   /* данные программы — dc_mods_exe_patch */
            if (!_stricmp(nm, "index.txt")) continue;     /* 05.10: список картинок экранов из PNG (растр) */
            int found = 0;
            for (int i = 0; i < s_n && !found; i++)
                if ((!strcmp(s_f[i].cat, cats[c]) || (s_f[i].old_cat && !strcmp(s_f[i].old_cat, cats[c]))) &&
                    !_stricmp(s_f[i].name, nm)) found = 1;
            if (!found) fprintf(stderr, "mods: %s\\%s - no game file of that name in this folder, ignored\n", cats[c], nm);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    (void)base;
#endif
}

static void audio_movie_load(const char *base);   /* звуки WAV и ролики MP4 — ниже */
static void live_start(const char *base);         /* 06.10.2026: подхват на лету — в конце файла */

/* мод файла игры (06.10.2026: и снова, при подхвате на лету): открыт, как читать, виртуальные сектора большой комнаты;
 * 1 — подменяет файл игры */
static int mod_open(DiscFile *f, const char *base)
{
    if (f->fp) { fclose(f->fp); f->fp = NULL; }
    f->mode = M_NONE; f->msize = 0; f->vnsec = 0; f->stamp = 0;   /* vlba и vcap остаются: новая версия комнаты берёт их же */
    f->cat = f->cat0;
    char p[MAX_PATH + 64];
    snprintf(p, sizeof p, "%s%s\\%s", base, f->cat, f->name);
    FILE *fp = open_shared(p);
    if (!fp && f->old_cat) {                     /* папка V3 */
        snprintf(p, sizeof p, "%s%s\\%s", base, f->old_cat, f->name);
        fp = open_shared(p);
        if (fp) f->cat = f->old_cat;             /* в журнале — где нашёлся */
    }
    if (!fp) return 0;
    f->stamp = file_stamp(p);
    long n = -1;
    if (fseek(fp, 0, SEEK_END) == 0) n = ftell(fp);
    if (n < 0) { fclose(fp); return 0; }
    const uint32_t ms = (uint32_t)n;
    uint8_t mode = M_NONE;
    if (!f->stream) mode = (ms > 0 && ms <= f->nsec * 2048u) ? M_DATA : M_NONE;
    /* ⭐ 06.10.2026 (player: "any enemy in any room"): a room file that grew - another kind's model and motions added by
     * the Studio - is read from virtual sectors past the end of the disc; the game's file table entry is pointed there
     * when the EXE loads (dc_mods_exe_patch). DC_MODS_BIG=0 - such a file is ignored, as before. The sectors are kept for
     * the file with 16 to spare: a later save of the same room reuses them. */
    const uint32_t need = (ms + 2047u) / 2048u;
    if (!f->stream && mode == M_NONE && ms > 0 && !strcmp(f->cat, "rooms") && env_on("DC_MODS_BIG", 1) &&
        (need <= f->vcap || s_vnext + need + 16u < 449000u)) {
        if (need > f->vcap) { f->vlba = s_vnext; f->vcap = need + 16u; s_vnext += f->vcap; }
        f->vnsec = need;
        mode = M_DATA;
        fprintf(stderr, "mods: %s\\%s is %u bytes, more than its %u sectors on the disc - read from sectors %u..%u past the end "
                        "of the disc\n", f->cat, f->name, ms, f->nsec, f->vlba, f->vlba + f->vnsec - 1);
    }
    else if (ms == f->nsec * 2352u) mode = M_RAW2352;
    else if (ms == f->nsec * 2336u) mode = M_RAW2336;
    if (mode == M_NONE) {
        if (!f->stream)
            fprintf(stderr, "mods: %s\\%s is %u bytes, the game has room for %u (%u sectors) - ignored\n",
                    f->cat, f->name, ms, f->nsec * 2048u, f->nsec);
        else
            fprintf(stderr, "mods: %s\\%s is %u bytes - a stream must be %u raw sectors: %u bytes (2352 each) or %u (2336) - ignored\n",
                    f->cat, f->name, ms, f->nsec, f->nsec * 2352u, f->nsec * 2336u);
        fclose(fp);
        return 0;
    }
    f->fp = fp; f->msize = ms; f->mode = mode;
    fprintf(stderr, "mods: %s\\%s replaces the game's file (%u bytes, %s)\n", f->cat, f->name, ms,
            mode == M_DATA ? "data" : mode == M_RAW2352 ? "raw sectors 2352" : "raw sectors 2336");
    return 1;
}

void dc_mods_init(void *iso_handle)
{
    s_iso = iso_handle;
    for (int i = 0; i < s_n; i++) if (s_f[i].fp) fclose(s_f[i].fp);
    s_n = 0; s_active = 0;
    if (!s_iso) return;
    uint8_t pvd[2048];
    if (!iso_read_sector(s_iso, 16, pvd, 2048) || memcmp(pvd + 1, "CD001", 5) != 0) {
        fprintf(stderr, "mods: no ISO9660 volume on the disc image\n");
        return;
    }
    walk(le32(pvd + 156 + 2), le32(pvd + 156 + 10), "/", 0);
    qsort(s_f, (size_t)s_n, sizeof *s_f, cmp_lba);
    if (!env_on("DC_MODS", 1)) {
        fprintf(stderr, "mods: game files %d, replacing is off (DC_MODS=0)\n", s_n);
        return;
    }
    char base[MAX_PATH];
    view_prepare();                               /* 10.10: пакеты модов - вид data\mods_view */
    mods_base(base, sizeof base);
    for (int i = 0; i < s_n; i++) s_active += mod_open(&s_f[i], base);
    report_strangers(base);
    fprintf(stderr, "mods: game files %d, replaced by mods %d\n", s_n, s_active);   /* 23.10.2026: no folder - the log must not carry the player's paths */
    audio_movie_load(base);
    { extern void dc_meshrep_boot(const char *mods); dc_meshrep_boot(base); }   /* 05.10 ночь (2): меши из glTF (dc_meshrep.cpp) */
    { char src[MAX_PATH]; mods_src(src, sizeof src); live_start(src); }      /* 10.10: сторож - на настоящей папке mods */
}

/* ======================================================================================================================
 * ДАННЫЕ ПРОГРАММЫ ИГРЫ (05.10.2026): mods\other\SLUS_009.22 — копия EXE с правленными таблицами (сцены дверей, предметы,
 * тексты EXE), её собирает «Сконвертировать всё» приложения. Код игры перекомпилирован, поэтому файл целиком не нужен:
 * при загрузке EXE в RAM (bios_hle.c) берутся байты, которыми мод отличается от EXE игрока, и пишутся поверх; те же байты
 * пишутся снова после быстрой загрузки (savestate.c) — сейв мог быть сделан без мода. DC_MODS=0 — без правок.
 * 06.10.2026 (подхват на лету): тело EXE с диска хранится; правки собираются от него заново (мод, затем записи таблицы
 * файлов больших комнат — прежде мод возвращал их назад), в RAM пишутся только байты, которые поменялись; для быстрой
 * загрузки — каждый байт, тронутый модами за сеанс, с его нынешним значением (сейв мог быть сделан до правки).
 * ====================================================================================================================== */
typedef struct { uint32_t addr, n; uint8_t *b; } ExeRun;
static ExeRun *s_exe;
static int     s_nexe, s_capexe;
static uint8_t *s_exe_orig, *s_exe_cur, *s_exe_touch;   /* тело EXE: с диска, как сейчас в RAM, байты, тронутые модами */
static uint32_t s_exe_taddr, s_exe_size;

/* 06.10.2026: a room read from virtual sectors - its entry in the game's file table ({u32 lba, u32 size, u32 ...} in the
 * EXE data) is pointed there with the new size */
static void big_table_patch(uint8_t *body)
{
    for (int i = 0; i < s_n; i++) {
        DiscFile *f = &s_f[i];
        if (!f->vnsec || !f->fp) continue;
        int hits = 0;
        uint32_t at = 0;
        for (uint32_t k = 0; k + 8 <= s_exe_size; k += 4)
            if (le32(body + k) == f->lba) {
                const uint32_t sz = le32(body + k + 4);
                if (sz + 2048u > f->nsec * 2048u && sz <= f->nsec * 2048u) { hits++; at = k; }
            }
        if (hits != 1) {
            fprintf(stderr, "mods: rooms\\%s - its entry in the game's file table not found (%d), the bigger file is not read\n", f->name, hits);
            continue;
        }
        uint8_t nb[8];
        nb[0] = (uint8_t)f->vlba; nb[1] = (uint8_t)(f->vlba >> 8); nb[2] = (uint8_t)(f->vlba >> 16); nb[3] = (uint8_t)(f->vlba >> 24);
        nb[4] = (uint8_t)f->msize; nb[5] = (uint8_t)(f->msize >> 8); nb[6] = (uint8_t)(f->msize >> 16); nb[7] = (uint8_t)(f->msize >> 24);
        memcpy(body + at, nb, 8);
        fprintf(stderr, "mods: rooms\\%s - the game's file table entry at %08X points to sector %u, %u bytes\n", f->name,
                (unsigned)(s_exe_taddr + at), f->vlba, f->msize);
    }
}

/* mods\other\SLUS_009.22 поверх тела: байты, которыми мод отличается от EXE с диска */
static int slus_apply_fp(uint8_t *body, FILE *fp);
static int slus_apply(uint8_t *body)
{
#ifdef _WIN32
    if (s_view_on && s_nvexe > 0) {               /* 10.10: пакеты - правки каждого слоя по порядку (позже - главнее) */
        int ch = 0;
        for (int k = 0; k < s_nvexe; k++) {
            FILE *fk = _wfopen(s_vexe[k], L"rb");
            if (fk) ch += slus_apply_fp(body, fk);
        }
        return ch;
    }
#endif
    char base[MAX_PATH], p[MAX_PATH + 64];
    mods_base(base, sizeof base);
    snprintf(p, sizeof p, "%sother\\SLUS_009.22", base);
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    return slus_apply_fp(body, fp);
}
static int slus_apply_fp(uint8_t *body, FILE *fp)
{
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *m = (n > 0x800) ? (uint8_t *)malloc((size_t)n) : NULL;
    size_t got = m ? fread(m, 1, (size_t)n, fp) : 0;
    fclose(fp);
    if (!m || got != (size_t)n || memcmp(m, "PS-X EXE", 8) != 0 || le32(m + 0x18) != s_exe_taddr || (uint32_t)n - 0x800u != s_exe_size) {
        fprintf(stderr, "mods: other\\SLUS_009.22 is not this game's program (another size or address) - ignored\n");
        free(m);
        return 0;
    }
    int changed = 0;
    for (uint32_t i = 0; i < s_exe_size; i++)
        if (m[0x800 + i] != s_exe_orig[i]) { body[i] = m[0x800 + i]; changed++; }
    free(m);
    return changed;
}

/* тело с модами (от тела с диска): мод, затем записи больших комнат; куски для быстрой загрузки — все тронутые байты */
static int exe_build(uint8_t *body)
{
    memcpy(body, s_exe_orig, s_exe_size);
    const int changed = slus_apply(body);
    big_table_patch(body);
    for (uint32_t i = 0; i < s_exe_size; i++) if (body[i] != s_exe_orig[i]) s_exe_touch[i] = 1;
    for (int i = 0; i < s_nexe; i++) free(s_exe[i].b);
    s_nexe = 0;
    for (uint32_t i = 0; i < s_exe_size; ) {
        if (!s_exe_touch[i]) { i++; continue; }
        uint32_t j = i;
        while (j < s_exe_size && s_exe_touch[j]) j++;
        if (s_nexe == s_capexe) {
            const int nc = s_capexe ? s_capexe * 2 : 64;
            ExeRun *nr = (ExeRun *)realloc(s_exe, (size_t)nc * sizeof *nr);
            if (!nr) break;
            s_exe = nr; s_capexe = nc;
        }
        ExeRun *r = &s_exe[s_nexe++];
        r->addr = s_exe_taddr + i; r->n = j - i;
        r->b = (uint8_t *)malloc(r->n);
        if (r->b) memcpy(r->b, body + i, r->n);
        i = j;
    }
    return changed;
}

int dc_mods_exe_patch(uint32_t t_addr, uint8_t *body, uint32_t size)
{
    for (int i = 0; i < s_nexe; i++) free(s_exe[i].b);
    s_nexe = 0;
    free(s_exe_orig); free(s_exe_cur); free(s_exe_touch);
    s_exe_orig = s_exe_cur = s_exe_touch = NULL;
    if (!env_on("DC_MODS", 1) || !body || !size) return 0;
    s_exe_orig = (uint8_t *)malloc(size);
    s_exe_cur = (uint8_t *)malloc(size);
    s_exe_touch = (uint8_t *)calloc(size, 1);
    if (!s_exe_orig || !s_exe_cur || !s_exe_touch) {
        free(s_exe_orig); free(s_exe_cur); free(s_exe_touch);
        s_exe_orig = s_exe_cur = s_exe_touch = NULL;
        return 0;
    }
    memcpy(s_exe_orig, body, size);
    s_exe_taddr = t_addr; s_exe_size = size;
    const int changed = exe_build(s_exe_cur);
    memcpy(body, s_exe_cur, size);
    if (changed) fprintf(stderr, "mods: other\\SLUS_009.22 - %d bytes of the game's data replaced (%d places)\n", changed, s_nexe);
    return changed;
}

/* 06.10.2026: подхват на лету — тело собрано заново, в RAM — только поменявшиеся байты */
static void exe_reapply(void)
{
    if (!s_exe_orig) return;                     /* EXE ещё не загружен: при загрузке возьмётся всё */
    uint8_t *nb = (uint8_t *)malloc(s_exe_size);
    if (!nb) return;
    exe_build(nb);
    uint8_t *ram = memory_get_ram_ptr();
    int n = 0;
    for (uint32_t i = 0; i < s_exe_size; i++)
        if (nb[i] != s_exe_cur[i]) { if (ram) ram[(s_exe_taddr + i) & 0x1FFFFFu] = nb[i]; n++; }
    free(s_exe_cur);
    s_exe_cur = nb;
    if (n) fprintf(stderr, "mods: the game's program data - %d bytes changed in memory\n", n);
}

int dc_mods_exe_run(int i, uint32_t *addr, const uint8_t **b, uint32_t *n)
{
    if (i < 0 || i >= s_nexe || !s_exe[i].b) return 0;
    *addr = s_exe[i].addr; *b = s_exe[i].b; *n = s_exe[i].n;
    return 1;
}

/* ======================================================================================================================
 * ЗВУКИ WAV (05.10.2026, игрок: «доделай всё»; решение 04.10: «звуки WAV (эффекты, речь, музыка)» подменяются на нашей
 * стороне, игра о них не знает). Раскладка — та же, что выгружает приложение Dino Crisis ME Studio в source:
 *   sound\se\{room,door,weapon,common}\<ФАЙЛ>\NNN.wav (bankK_NNN.wav)   эффекты из звуковых банков <ФАЙЛ>.DAT
 *   sound\bgm\<ФАЙЛ>\instruments\NNN.wav                                  инструменты музыки
 *   sound\voice\STAGE0N\chNN.wav, sound\se\STAGE00\chNN.wav               каналы XA (речь, шумы)
 * Эффект и инструмент узнаются по СОДЕРЖИМОМУ: на старте банки файла читаются с диска (или из его мода) тем же разбором,
 * что в приложении (уникальные адреса тонов по возрастанию), и для каждого сэмпла с WAV берётся хеш ADPCM от начала до
 * блока конца; на KEYON голоса (spu.c) хеш данных в SPU RAM с его адреса сравнивается — совпал, голос играет WAV: высота —
 * от регистра pitch голоса (как играл бы сэмпл игры на своей «родной» частоте), огибающая — его ADSR. Петля — из чанка
 * smpl WAV, иначе с начала, если петлял сэмпл игры. Канал XA узнаётся по сектору: номер сектора канала в файле → место
 * в WAV (выгрузка кладёт сектора канала подряд). DC_MODS_AUDIO=0 — выключить.
 * ====================================================================================================================== */
extern int iso_read_raw_sector(void *handle, uint32_t lba, uint8_t *buffer, int size);
static DiscFile *find(uint32_t lba);

typedef struct {
    uint64_t hash; uint32_t nbytes;          /* ADPCM сэмпла игры: от начала до блока с флагом конца включительно */
    int16_t *pcm; uint32_t n;                /* WAV, моно */
    int rate, orig_rate;                     /* частота WAV и «родная» частота сэмпла (как в выгрузке) */
    int32_t loop;                            /* начало петли в WAV, −1 — без */
    uint64_t pkey, stamp; int32_t wloop;     /* 06.10.2026: путь и отметка WAV, петля из smpl — для подхвата на лету */
} DcSnd;
static DcSnd *s_snd;
static int    s_nsnd, s_capsnd;

typedef struct {
    uint32_t flba, fnsec;                    /* файл XA на диске */
    uint8_t  chan;
    uint32_t *lba; int nlba;                 /* сектора канала по порядку */
    int16_t  *pcm; uint32_t frames; int rate;  /* WAV: стерео (L,R), частота XA канала */
    uint64_t pkey, stamp;                    /* 06.10.2026: путь и отметка WAV / MP4 */
} DcXa;
static DcXa *s_xa;
static int   s_nxa;

typedef struct { uint64_t key; int16_t *pcm; uint32_t frames; int32_t loop_start, loop_end; uint64_t pkey, stamp; } DcSong;   /* мелодии (ниже) */
static DcSong *s_song;
static int     s_nsong;
static int song_scan_file(DiscFile *f, const char *dir);

static uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

/* 06.10.2026 (подхват на лету): звуки, декодированные до перечитывания; WAV / MP4 с той же отметкой (размер и время) забирает
 * свой звук назад — ничего не декодируется дважды; остальное освобождается, а то, что ещё играет голос или мелодия
 * (spu.c), — при следующем перечитывании */
typedef struct { uint64_t key, stamp; int16_t *pcm; uint32_t frames; int rate; int32_t loop; uint8_t taken; } KeepPcm;
static KeepPcm  *s_keep;
static int       s_nkeep, s_capkeep;
static int16_t **s_grave;
static int       s_ngrave, s_capgrave;
extern int  spu_mods_pcm_busy(const int16_t *p);
extern void spu_mods_changed(void);

static uint64_t path_key(const char *p) { return fnv64((const uint8_t *)p, strlen(p), 0xCBF29CE484222325ull); }

static void keep_put(uint64_t key, uint64_t stamp, int16_t *pcm, uint32_t frames, int rate, int32_t loop)
{
    if (!pcm) return;
    if (s_nkeep == s_capkeep) {
        const int nc = s_capkeep ? s_capkeep * 2 : 64;
        KeepPcm *nk = (KeepPcm *)realloc(s_keep, (size_t)nc * sizeof *nk);
        if (!nk) { if (spu_mods_pcm_busy(pcm)) return; free(pcm); return; }
        s_keep = nk; s_capkeep = nc;
    }
    KeepPcm *k = &s_keep[s_nkeep++];
    k->key = key; k->stamp = stamp; k->pcm = pcm; k->frames = frames; k->rate = rate; k->loop = loop; k->taken = 0;
}

static int16_t *keep_take(uint64_t key, uint64_t stamp, uint32_t *frames, int *rate, int32_t *loop)
{
    if (!stamp) return NULL;
    for (int i = 0; i < s_nkeep; i++) {
        KeepPcm *k = &s_keep[i];
        if (k->taken || k->key != key || k->stamp != stamp) continue;
        k->taken = 1;
        *frames = k->frames; *rate = k->rate; *loop = k->loop;
        return k->pcm;
    }
    return NULL;
}

static void keep_retire(void)
{
    int kept = 0, freed = 0, busy = 0;
    for (int i = 0; i < s_nkeep; i++) {
        KeepPcm *k = &s_keep[i];
        if (k->taken) kept++;
        if (k->taken || !k->pcm) continue;
        if (!spu_mods_pcm_busy(k->pcm)) { free(k->pcm); freed++; continue; }
        busy++;
        if (s_ngrave == s_capgrave) {
            const int nc = s_capgrave ? s_capgrave * 2 : 16;
            int16_t **ng = (int16_t **)realloc(s_grave, (size_t)nc * sizeof *ng);
            if (!ng) continue;                   /* не освободить — лучше, чем освободить играющее */
            s_grave = ng; s_capgrave = nc;
        }
        s_grave[s_ngrave++] = k->pcm;
    }
    if (s_nkeep) fprintf(stderr, "mods: sounds picked up - %d kept, %d freed, %d still playing (freed later)\n", kept, freed, busy);
    s_nkeep = 0;
    int n = 0;
    for (int i = 0; i < s_ngrave; i++) {
        if (spu_mods_pcm_busy(s_grave[i])) s_grave[n++] = s_grave[i];
        else free(s_grave[i]);
    }
    s_ngrave = n;
}

static uint16_t le16m(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* WAV → int16 с чередованием каналов; 8/16/24 бит PCM и 32 бит float; петля из smpl */
static int wav_load(const char *path, int16_t **out, uint32_t *frames, int *ch, int *rate, int32_t *loop)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 44 || n > (256l << 20)) { fclose(fp); return 0; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(fp); return 0; }
    size_t got = fread(b, 1, (size_t)n, fp);
    fclose(fp);
    int ok = 0, fmt = 0, nch = 0, bits = 0, sr = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    *loop = -1;
    if (got == (size_t)n && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        size_t o = 12;
        while (o + 8 <= (size_t)n) {
            uint32_t cl = le32(b + o + 4);
            const uint8_t *c = b + o + 8;
            if (o + 8 + cl > (size_t)n) cl = (uint32_t)((size_t)n - o - 8);
            if (!memcmp(b + o, "fmt ", 4) && cl >= 16) {
                fmt = le16m(c); nch = le16m(c + 2); sr = (int)le32(c + 4); bits = le16m(c + 14);
                if (fmt == 0xFFFE && cl >= 26) fmt = le16m(c + 24);   /* WAVE_FORMAT_EXTENSIBLE: подформат */
            } else if (!memcmp(b + o, "data", 4)) {
                data = c; dlen = cl;
            } else if (!memcmp(b + o, "smpl", 4) && cl >= 36 + 24 && le32(c + 28) > 0) {
                *loop = (int32_t)le32(c + 36 + 8);
            }
            o += 8 + cl + (cl & 1);
        }
        if (data && nch >= 1 && nch <= 8 && sr > 0 && ((fmt == 1 && (bits == 8 || bits == 16 || bits == 24)) || (fmt == 3 && bits == 32))) {
            const uint32_t bps = (uint32_t)bits / 8u, fr = dlen / (bps * (uint32_t)nch);
            int16_t *o16 = (int16_t *)malloc((size_t)fr * (size_t)nch * 2 + 2);
            if (o16) {
                for (uint32_t i = 0; i < fr * (uint32_t)nch; i++) {
                    const uint8_t *q = data + (size_t)i * bps;
                    int v;
                    if (fmt == 3) { float f; memcpy(&f, q, 4); f = f < -1 ? -1 : f > 1 ? 1 : f; v = (int)(f * 32767.0f); }
                    else if (bits == 8) v = ((int)q[0] - 128) << 8;
                    else if (bits == 16) v = (int16_t)le16m(q);
                    else v = (int)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 16);
                    o16[i] = (int16_t)v;
                }
                *out = o16; *frames = fr; *ch = nch; *rate = sr;
                ok = 1;
            }
        }
    }
    free(b);
    return ok;
}

/* каналы и частота: в ch_out каналов на частоте to (линейно) */
static int16_t *pcm_convert(const int16_t *in, uint32_t frames, int ch, int from, int ch_out, int to, uint32_t *out_frames)
{
    if (ch == ch_out && from == to) {            /* 06.10.2026: менять нечего — копия (песня 44.1 кГц стерео) */
        int16_t *o = (int16_t *)malloc((size_t)frames * (size_t)ch * 2 + 4);
        if (!o) return NULL;
        memcpy(o, in, (size_t)frames * (size_t)ch * 2);
        *out_frames = frames;
        return o;
    }
    uint32_t on = (uint32_t)((double)frames * to / from);
    int16_t *o = (int16_t *)malloc((size_t)on * (size_t)ch_out * 2 + 4);
    if (!o) return NULL;
    const double step = (double)from / to;
    for (uint32_t i = 0; i < on; i++) {
        double s = i * step;
        uint32_t a = (uint32_t)s, b = a + 1 < frames ? a + 1 : a;
        double f = s - a;
        for (int c = 0; c < ch_out; c++) {
            double va = 0, vb = 0;
            if (ch_out == 1 && ch > 1) {        /* в моно: среднее */
                for (int k = 0; k < ch; k++) { va += in[(size_t)a * ch + k]; vb += in[(size_t)b * ch + k]; }
                va /= ch; vb /= ch;
            } else {
                int cc = c < ch ? c : ch - 1;
                va = in[(size_t)a * ch + cc]; vb = in[(size_t)b * ch + cc];
            }
            double v = va + (vb - va) * f;
            o[(size_t)i * ch_out + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
    }
    *out_frames = on;
    return o;
}

/* файл игры так, как его прочтёт игра (с подменой из mods) */
static uint8_t *read_game_file(DiscFile *f)
{
    uint8_t *d = (uint8_t *)malloc((size_t)f->nsec * 2048u);
    if (!d) return NULL;
    for (uint32_t i = 0; i < f->nsec; i++) {
        if (!iso_read_sector(s_iso, f->lba + i, d + (size_t)i * 2048u, 2048)) memset(d + (size_t)i * 2048u, 0, 2048);
        dc_mods_cooked(f->lba + i, d + (size_t)i * 2048u);
    }
    return d;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

static DiscFile *find_stem(const char *stem, const char *ext)
{
    char nm[48];
    snprintf(nm, sizeof nm, "%s%s", stem, ext);
    for (int i = 0; i < s_n; i++) if (same_name(s_f[i].name, nm)) return &s_f[i];
    return NULL;
}


/* сэмплы банков файла — как parse_vab приложения; для каждого с WAV в dir — запись в таблицу */
static int snd_scan_file(DiscFile *f, const char *dir, int sfx)
{
    uint8_t *d = read_game_file(f);
    if (!d) return 0;
    const uint32_t size = f->nsec * 2048u;
    uint32_t et[128], es[128], ed[128], eo[128];
    int ne = 0;
    for (uint32_t off = 0; off + 16 <= 0x800 && ne < 128; off += 16) {
        uint32_t t = le32(d + off), sz = le32(d + off + 4);
        if (t > 20 || sz == 0 || sz > size) break;
        et[ne] = t; es[ne] = sz; ed[ne] = le32(d + off + 8); ne++;
    }
    uint32_t pos = 0x800;
    for (int i = 0; i < ne; i++) {
        if (pos + es[i] > size) { ne = i; break; }
        eo[i] = pos;
        pos = (pos + es[i] + 2047u) & ~2047u;
    }
    int hb[32], bb[32], nb = 0;
    for (int i = 0; i < ne && nb < 32; i++) {
        if (et[i] != 3) continue;
        for (int k = i + 1; k < ne; k++)
            if (et[k] == 4 && ed[k] == ed[i]) {
                const uint8_t *h = d + eo[i];
                const uint32_t np = es[i] >= 0x90 ? le16m(h + 6) : 0;
                if (es[i] >= 0x90 && !memcmp(h, "Gian", 4) && np <= 128 && 0x90u + np * 384u <= es[i]) { hb[nb] = i; bb[nb] = k; nb++; }
                break;
            }
    }
    int added = 0;
    for (int bi = 0; bi < nb; bi++) {
        const uint8_t *h = d + eo[hb[bi]], *body = d + eo[bb[bi]];
        const uint32_t bn = es[bb[bi]], dest = ed[bb[bi]], np = le16m(h + 6);
        /* сэмплы — уникальные адреса тонов (тон < числа тонов программы) по возрастанию; центр, нота min и shift — у ПЕРВОГО
         * тона с этим сэмплом (как в приложении: по нему частота выгрузки) */
        uint16_t addr[2048]; uint8_t ctr[2048], mn[2048], shf[2048];
        int na = 0;
        for (uint32_t p = 0; p < np; p++) {
            const int ntones = p < 16 ? h[0x10 + p * 8] : 16;
            for (int t = 0; t < 16 && t < ntones; t++) {
                const uint8_t *q = h + 0x90 + p * 384 + t * 24;
                const uint16_t a = le16m(q + 22);
                if (!a) continue;
                int k = 0;
                while (k < na && addr[k] != a) k++;
                if (k == na && na < 2048) { addr[na] = a; ctr[na] = q[4]; mn[na] = q[6]; shf[na] = q[5]; na++; }
            }
        }
        for (int i = 1; i < na; i++)                          /* по возрастанию адреса */
            for (int j = i; j > 0 && addr[j - 1] > addr[j]; j--) {
                uint16_t ta = addr[j]; addr[j] = addr[j - 1]; addr[j - 1] = ta;
                uint8_t tc = ctr[j]; ctr[j] = ctr[j - 1]; ctr[j - 1] = tc;
                tc = mn[j]; mn[j] = mn[j - 1]; mn[j - 1] = tc;
                tc = shf[j]; shf[j] = shf[j - 1]; shf[j - 1] = tc;
            }
        int idx = 0;
        for (int i = 0; i < na; i++) {
            const int64_t off = (int64_t)addr[i] * 8 - (int64_t)dest;
            if (off < 0 || (uint64_t)off >= bn) continue;
            char wp[MAX_PATH + 96];
            if (nb > 1) snprintf(wp, sizeof wp, "%s\\bank%d_%03d.wav", dir, bi, idx);
            else snprintf(wp, sizeof wp, "%s\\%03d.wav", dir, idx);
            idx++;
            const uint64_t st = file_stamp(wp);
            if (!st) continue;
            uint32_t nbytes = 0;
            for (uint32_t b = (uint32_t)off; b + 16 <= bn; b += 16) {
                nbytes += 16;
                if (body[b + 1] & 1) break;
            }
            int16_t *pcm = NULL; uint32_t fr = 0; int ch = 1, rate = 0; int32_t loop = -1;
            const uint64_t pk = path_key(wp);
            pcm = keep_take(pk, st, &fr, &rate, &loop);           /* 06.10.2026: тот же файл — его звук, без декода */
            if (!pcm) {
                if (!wav_load(wp, &pcm, &fr, &ch, &rate, &loop)) { fprintf(stderr, "mods: %s - not a WAV I can read, ignored\n", wp); continue; }
                if (ch != 1) {                                     /* голос SPU — моно */
                    uint32_t mf = 0;
                    int16_t *m = pcm_convert(pcm, fr, ch, rate, 1, rate, &mf);
                    free(pcm);
                    if (!m) continue;
                    pcm = m; fr = mf;
                }
            }
            /* частота, на которой сэмпл выгружен: эффект — нота min тона (44100·2^((min + shift/128 − center)/12), разбор
             * драйвера 05.10), инструмент — 44.1 кГц */
            const int c = ctr[i];
            double semis = mn[i] + shf[i] / 128.0 - c;
            if (semis > 24) semis = 24;
            const int orig_rate = (sfx && c >= 24 && c <= 120) ? (int)(44100.0 * pow(2.0, semis / 12.0) + 0.5) : 44100;
            if (s_nsnd == s_capsnd) {
                int nc = s_capsnd ? s_capsnd * 2 : 64;
                DcSnd *ns = (DcSnd *)realloc(s_snd, (size_t)nc * sizeof *ns);
                if (!ns) { keep_put(pk, st, pcm, fr, rate, loop); continue; }
                s_snd = ns; s_capsnd = nc;
            }
            DcSnd *s = &s_snd[s_nsnd++];
            s->pkey = pk; s->stamp = st; s->wloop = loop;
            s->hash = fnv64(body + off, nbytes, 0xCBF29CE484222325ull);
            if (env_on("DC_MODS_AUDIO_LOG", 0))
                fprintf(stderr, "modsnd: %s sample %d at SPU %05X bytes %u hash %016llX\n", f->name, idx - 1,
                        (unsigned)(addr[i] * 8u), (unsigned)nbytes, (unsigned long long)s->hash);
            s->nbytes = nbytes;
            s->pcm = pcm; s->n = fr; s->rate = rate; s->orig_rate = orig_rate;
            s->loop = (loop >= 0 && (uint32_t)loop < fr) ? loop : -1;
            if (s->loop < 0) {                                     /* петлял ли сэмпл игры — с начала */
                for (uint32_t b = (uint32_t)off; b + 16 <= (uint32_t)off + nbytes; b += 16)
                    if ((body[b + 1] & 3) == 3) { s->loop = 0; break; }
            }
            added++;
        }
    }
    free(d);
    return added;
}

/* каналы XA: сектора канала по порядку и WAV на частоте канала */
static int xa_scan_file(DiscFile *f, const char *dir)
{
    int want[32] = {0}, any = 0;
    uint64_t wst[32] = {0};
    for (int c = 0; c < 32; c++) {
        char wp[MAX_PATH + 64];
        snprintf(wp, sizeof wp, "%s\\ch%02d.wav", dir, c);
        wst[c] = file_stamp(wp);
        if (wst[c]) { want[c] = 1; any = 1; }
    }
    if (!any) return 0;
    uint32_t *lbas[32] = {0}; int nl[32] = {0}, rate[32] = {0};
    uint8_t raw[2352];
    for (uint32_t i = 0; i < f->nsec; i++) {
        if (!iso_read_raw_sector(s_iso, f->lba + i, raw, 2352)) continue;
        const uint8_t ch = raw[17], sub = raw[18], cod = raw[19];
        if (!(sub & 0x04) || ch >= 32 || !want[ch]) continue;
        uint32_t *nl2 = (uint32_t *)realloc(lbas[ch], (size_t)(nl[ch] + 1) * 4u);
        if (!nl2) continue;
        lbas[ch] = nl2;
        lbas[ch][nl[ch]++] = f->lba + i;
        rate[ch] = ((cod >> 2) & 3) == 1 ? 18900 : 37800;
    }
    int added = 0;
    for (int c = 0; c < 32; c++) {
        if (!want[c]) continue;
        char wp[MAX_PATH + 64];
        snprintf(wp, sizeof wp, "%s\\ch%02d.wav", dir, c);
        if (!nl[c]) { free(lbas[c]); continue; }
        const uint64_t pk = path_key(wp) ^ (uint64_t)rate[c];
        uint32_t of = 0; int r0 = 0; int32_t l0 = 0;
        int16_t *st = keep_take(pk, wst[c], &of, &r0, &l0);   /* 06.10.2026: тот же файл — его звук, без декода */
        if (!st) {
            int16_t *pcm = NULL; uint32_t fr = 0; int ch = 0, sr = 0; int32_t loop;
            if (!wav_load(wp, &pcm, &fr, &ch, &sr, &loop)) { free(lbas[c]); continue; }
            st = pcm_convert(pcm, fr, ch, sr, 2, rate[c], &of);
            free(pcm);
        }
        if (!st) { free(lbas[c]); continue; }
        DcXa *nx = (DcXa *)realloc(s_xa, (size_t)(s_nxa + 1) * sizeof *nx);
        if (!nx) { keep_put(pk, wst[c], st, of, rate[c], 0); free(lbas[c]); continue; }
        s_xa = nx;
        DcXa *x = &s_xa[s_nxa++];
        x->flba = f->lba; x->fnsec = f->nsec; x->chan = (uint8_t)c;
        x->lba = lbas[c]; x->nlba = nl[c];
        x->pcm = st; x->frames = of; x->rate = rate[c];
        x->pkey = pk; x->stamp = wst[c];
        added++;
    }
    return added;
}

/* ======================================================================================================================
 * РОЛИКИ MP4 (05.10.2026): mods\movie\<ИМЯ>\<ИМЯ>.mp4 (как выгружает приложение, любое разрешение) — вместо ролика игры.
 * Игра читает свой STR как всегда; по номеру кадра из заголовка видеосектора (магия 0x0160) задаётся время MP4
 * (dc_movie.cpp: (кадр − первый) / 30 с — как выгрузка), показ рисует его кадр. Звук MP4 — на канал XA ролика (первый канал
 * звука в файле), как речь из WAV. DC_MODS_MOVIE=0 — без роликов из mods.
 * ====================================================================================================================== */
#ifdef _WIN32
extern int  dc_movie_audio(const wchar_t *path, int16_t **pcm, uint32_t *frames, int *ch, int *rate);
extern void dc_movie_at(int id, const wchar_t *path, double t);
#endif
typedef struct { DiscFile *f; wchar_t path[MAX_PATH]; uint32_t first; } DcMovie;
static DcMovie *s_mov;
static int      s_nmov;
static int      s_mov_gen;                /* 06.10.2026: растёт на каждом перечитывании — dc_movie.cpp открывает ролик заново */

static void movie_init(const char *base)
{
    free(s_mov); s_mov = NULL; s_nmov = 0;
    s_mov_gen++;
#ifdef _WIN32
    if (!env_on("DC_MODS", 1) || !env_on("DC_MODS_MOVIE", 1)) return;
    for (int i = 0; i < s_n; i++) {
        DiscFile *f = &s_f[i];
        if (strcmp(f->cat0, "movie") != 0) continue;
        char stem[32];
        snprintf(stem, sizeof stem, "%s", f->name);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        char p[MAX_PATH + 96];
        snprintf(p, sizeof p, "%smovie\\%s\\%s.mp4", base, stem, stem);
        const uint64_t stp = file_stamp(p);
        if (!stp) continue;
        uint32_t first = 0xFFFFFFFFu;                      /* номер первого кадра: заголовок первого видеосектора */
        uint8_t u[2048];
        for (uint32_t k = 0; k < f->nsec && k < 64 && first == 0xFFFFFFFFu; k++)
            if (iso_read_sector(s_iso, f->lba + k, u, 2048) && u[0] == 0x60 && u[1] == 0x01 && u[2] == 0x01 && u[3] == 0x80)
                first = (uint32_t)(u[8] | (u[9] << 8) | (u[10] << 16) | ((uint32_t)u[11] << 24));
        if (first == 0xFFFFFFFFu) { fprintf(stderr, "mods: movie\\%s - no video in the game's %s, ignored\n", stem, f->name); continue; }
        DcMovie *nm = (DcMovie *)realloc(s_mov, (size_t)(s_nmov + 1) * sizeof *nm);
        if (!nm) break;
        s_mov = nm;
        DcMovie *m = &s_mov[s_nmov++];
        m->f = f; m->first = first;
        /* 18.10.2026: путь целиком из корня mods (GetModuleFileNameA - кодировка ANSI) и имени файла диска (латиница):
         * переводится как ANSI. Как UTF-8 он ломался на русских буквах в пути к игре - ролик молча не находился. */
        MultiByteToWideChar(CP_ACP, 0, p, -1, m->path, MAX_PATH);
        /* звук: дорожка MP4 на первый канал XA ролика */
        int nxa_before = s_nxa;
        if (env_on("DC_MODS_AUDIO", 1)) {
            uint8_t raw[2352];
            int chan = -1, rate = 37800, nl = 0;
            uint32_t *lbas = NULL;
            for (uint32_t k = 0; k < f->nsec; k++) {
                if (!iso_read_raw_sector(s_iso, f->lba + k, raw, 2352)) continue;
                const uint8_t c = raw[17], sub = raw[18], cod = raw[19];
                if (!(sub & 0x04) || c >= 32) continue;
                if (chan < 0) { chan = c; rate = ((cod >> 2) & 3) == 1 ? 18900 : 37800; }
                if (c != chan) continue;
                uint32_t *n2 = (uint32_t *)realloc(lbas, (size_t)(nl + 1) * 4u);
                if (!n2) break;
                lbas = n2; lbas[nl++] = f->lba + k;
            }
            if (chan >= 0 && nl) {
                const uint64_t pk = path_key(p) ^ (uint64_t)rate;
                uint32_t of = 0; int r0 = 0; int32_t l0 = 0;
                int16_t *st = keep_take(pk, stp, &of, &r0, &l0);   /* 06.10.2026: тот же ролик — его звук, без декода */
                if (!st) {
                    int16_t *pcm = NULL; uint32_t fr = 0; int ch = 0, sr = 0;
                    if (dc_movie_audio(m->path, &pcm, &fr, &ch, &sr) && fr) st = pcm_convert(pcm, fr, ch, sr, 2, rate, &of);
                    free(pcm);
                }
                DcXa *nx = st ? (DcXa *)realloc(s_xa, (size_t)(s_nxa + 1) * sizeof *nx) : NULL;
                if (nx) {
                    s_xa = nx;
                    DcXa *x = &s_xa[s_nxa++];
                    x->flba = f->lba; x->fnsec = f->nsec; x->chan = (uint8_t)chan;
                    x->lba = lbas; x->nlba = nl;
                    x->pcm = st; x->frames = of; x->rate = rate;
                    x->pkey = pk; x->stamp = stp;
                    lbas = NULL;
                } else if (st) keep_put(pk, stp, st, of, rate, 0);
            }
            free(lbas);
        }
        fprintf(stderr, "mods: movie\\%s\\%s.mp4 - instead of %s (first frame %u, sound %s)\n", stem, stem, f->name, first,
                s_nxa > nxa_before ? "from the MP4" : "the game's");
    }
#else
    (void)base;
#endif
}

/* сектор данных прочитан приводом (cdrom.c): видеосектор подменённого ролика — время его кадра в dc_movie.cpp */
void dc_mods_movie_sector(uint32_t lba, const uint8_t *u)
{
#ifdef _WIN32
    if (!s_nmov || !u || u[0] != 0x60 || u[1] != 0x01 || u[2] != 0x01 || u[3] != 0x80) return;
    for (int i = 0; i < s_nmov; i++) {
        const DcMovie *m = &s_mov[i];
        if (lba < m->f->lba || lba >= m->f->lba + m->f->nsec) continue;
        const uint32_t fr = (uint32_t)(u[8] | (u[9] << 8) | (u[10] << 16) | ((uint32_t)u[11] << 24));
        dc_movie_at(i + s_mov_gen * 4096, m->path, fr >= m->first ? (double)(fr - m->first) / 30.0 : 0.0);
        return;
    }
#else
    (void)lba; (void)u;
#endif
}

/* таблицы звуков пусты (audio_movie_load отдала прежние в список keep) */
static void audio_init(const char *base)
{
    if (!env_on("DC_MODS", 1) || !env_on("DC_MODS_AUDIO", 1)) return;
#ifdef _WIN32
    static const char *const se_groups[] = {"sound\\se\\room", "sound\\se\\door", "sound\\se\\weapon", "sound\\se\\common", "sound\\bgm"};
    int nsmp = 0, nxa = 0, nsong = 0;
    for (size_t g = 0; g < sizeof se_groups / sizeof se_groups[0]; g++) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s%s\\*", base, se_groups[g]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            const int bgm = g == 4;
            DiscFile *f = find_stem(fd.cFileName, ".DAT");
            if (!f) continue;
            char dir[MAX_PATH + 64];
            snprintf(dir, sizeof dir, "%s%s\\%s%s", base, se_groups[g], fd.cFileName, bgm ? "\\instruments" : "");
            nsmp += snd_scan_file(f, dir, !bgm);
            if (bgm) {
                snprintf(dir, sizeof dir, "%s%s\\%s", base, se_groups[g], fd.cFileName);
                nsong += song_scan_file(f, dir);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* XA: речь — sound\voice\STAGE0N, шумы — sound\se\STAGE00 */
    static const char *const xa_dirs[] = {"sound\\voice", "sound\\se"};
    for (size_t g = 0; g < 2; g++) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s%s\\*", base, xa_dirs[g]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            DiscFile *f = find_stem(fd.cFileName, ".XAS");
            if (!f) continue;
            char dir[MAX_PATH + 64];
            snprintf(dir, sizeof dir, "%s%s\\%s", base, xa_dirs[g], fd.cFileName);
            nxa += xa_scan_file(f, dir);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (nsmp || nxa || nsong) fprintf(stderr, "mods: sounds from WAV - %d samples, %d XA channels, %d songs\n", nsmp, nxa, nsong);
#else
    (void)base;
#endif
}

/* звуки WAV и ролики MP4 (06.10.2026: и снова, при подхвате на лету): декодированное прежде — в список keep, таблицы — заново
 * (файл с той же отметкой забирает свой звук), голоса и мелодии spu.c — на новые номера, остальное освобождается */
static void audio_movie_load(const char *base)
{
    for (int i = 0; i < s_nsnd; i++) keep_put(s_snd[i].pkey, s_snd[i].stamp, s_snd[i].pcm, s_snd[i].n, s_snd[i].rate, s_snd[i].wloop);
    s_nsnd = 0;
    for (int i = 0; i < s_nxa; i++) { keep_put(s_xa[i].pkey, s_xa[i].stamp, s_xa[i].pcm, s_xa[i].frames, s_xa[i].rate, 0); free(s_xa[i].lba); }
    s_nxa = 0;
    for (int i = 0; i < s_nsong; i++) keep_put(s_song[i].pkey, s_song[i].stamp, s_song[i].pcm, s_song[i].frames, 44100, s_song[i].loop_start);
    s_nsong = 0;
    audio_init(base);
    movie_init(base);
    spu_mods_changed();
    keep_retire();
}

int dc_mods_snd_count(void) { return s_nsnd; }

/* ---- мелодии: sound\bgm\<ФАЙЛ>\song_N.wav (N — номер записи секвенции в файле). Мелодия узнаётся по содержимому
 * (одна и та же лежит во многих файлах): ключ — FNV-1a 64 байтов записи до FF 2F 00 включительно (как song_key приложения).
 * Формат секвенции (re/sound.md): 8 Б заголовка, события «статус, данные, [delta, если бит 7]». */
static uint64_t song_key_c(const uint8_t *d, uint32_t n)
{
    uint32_t pc = 8, end = n;
    for (int g = 0; g < 1000000 && pc < n; g++) {
        const uint8_t st = d[pc];
        const int type = (st >> 4) & 7;
        uint32_t len = 1;
        int last = 0;
        if (type == 0 || type == 1 || type == 3 || type == 6) len += 2;
        else if (type == 4) len += 1;
        else if (type == 7) {
            if (pc + 1 >= n) break;
            if (d[pc + 1] == 0x51) len += 4;
            else { len += 1; last = d[pc + 1] == 0x2F; }
        }
        if (st & 0x80) len += 1;
        if (pc + len > n) break;
        pc += len;
        if (last) { end = pc; break; }
    }
    return fnv64(d, end < n ? end : n, 0xCBF29CE484222325ull);
}

static int song_scan_file(DiscFile *f, const char *dir)
{
    uint8_t *d = read_game_file(f);
    if (!d) return 0;
    const uint32_t size = f->nsec * 2048u;
    int added = 0, seq = 0;
    uint32_t pos = 0x800;
    for (uint32_t off = 0; off + 16 <= 0x800; off += 16) {
        uint32_t t = le32(d + off), sz = le32(d + off + 4);
        if (t > 20 || sz == 0 || sz > size || pos + sz > size) break;
        if (t == 5) {
            char wp[MAX_PATH + 64];
            snprintf(wp, sizeof wp, "%s\\song_%d.wav", dir, seq);
            const uint64_t stp = file_stamp(wp), pk = path_key(wp);
            uint32_t of = 0; int r0 = 0; int32_t ls = 0;
            int16_t *st = stp ? keep_take(pk, stp, &of, &r0, &ls) : NULL;   /* 06.10.2026: тот же файл — его звук, без декода */
            if (!st && stp) {
                int16_t *pcm = NULL; uint32_t fr = 0; int ch = 0, rate = 0; int32_t loop = -1;
                if (wav_load(wp, &pcm, &fr, &ch, &rate, &loop)) {
                    st = pcm_convert(pcm, fr, ch, rate, 2, 44100, &of);
                    free(pcm);
                    /* петля из smpl; без неё — с начала (одноразовую мелодию останавливает сам драйвер) */
                    ls = (loop >= 0 && rate > 0) ? (int32_t)((double)loop * 44100 / rate) : 0;
                }
            }
            if (st) {
                DcSong *ns = (DcSong *)realloc(s_song, (size_t)(s_nsong + 1) * sizeof *ns);
                if (ns) {
                    s_song = ns;
                    DcSong *s = &s_song[s_nsong++];
                    s->key = song_key_c(d + pos, sz);
                    s->pcm = st; s->frames = of;
                    s->loop_start = ls;
                    s->loop_end = (int32_t)of;
                    s->pkey = pk; s->stamp = stp;
                    added++;
                } else keep_put(pk, stp, st, of, 44100, ls);
            }
            seq++;
        }
        pos = (pos + sz + 2047u) & ~2047u;
    }
    free(d);
    return added;
}

int dc_mods_song_count(void) { return s_nsong; }

int dc_mods_song_find(const uint8_t *seq, uint32_t maxlen)
{
    if (!s_nsong || !seq) return -1;
    const uint64_t k = song_key_c(seq, maxlen);
    for (int i = 0; i < s_nsong; i++) if (s_song[i].key == k) return i;
    return -1;
}

const int16_t *dc_mods_song(int id, uint32_t *frames, int32_t *loop_start)
{
    if (id < 0 || id >= s_nsong) return NULL;
    *frames = s_song[id].frames;
    *loop_start = s_song[id].loop_start;
    return s_song[id].pcm;
}

/* KEYON: сэмпл в SPU RAM с адреса addr — подменён ли WAV; −1 нет */
int dc_mods_snd_match(const uint8_t *ram, uint32_t ram_size, uint32_t addr)
{
    if (!s_nsnd) return -1;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t nbytes = 0, a = addr & (ram_size - 1u);
    for (uint32_t k = 0; k < ram_size / 16u; k++) {
        h = fnv64(ram + a, 16, h);
        nbytes += 16;
        if (ram[a + 1u] & 1u) break;
        a = (a + 16u) & (ram_size - 1u);
    }
    int found = -1;
    for (int i = 0; i < s_nsnd && found < 0; i++)
        if (s_snd[i].hash == h && s_snd[i].nbytes == nbytes) found = i;
    {   /* DC_MODS_AUDIO_LOG=1 — каждый узнаваемый KEYON в журнал (первые 300) */
        static int lg = -1; static int nlog = 0;
        if (lg < 0) lg = env_on("DC_MODS_AUDIO_LOG", 0);
        if (lg && nlog < 300) {
            nlog++;
            fprintf(stderr, "modsnd: keyon %05X bytes %u hash %016llX -> %d\n", (unsigned)addr, (unsigned)nbytes,
                    (unsigned long long)h, found);
        }
    }
    return found;
}

const int16_t *dc_mods_snd(int id, uint32_t *n, double *ratio, int32_t *loop)
{
    if (id < 0 || id >= s_nsnd) return NULL;
    const DcSnd *s = &s_snd[id];
    *n = s->n;
    *ratio = (double)s->rate / (double)s->orig_rate;
    *loop = s->loop;
    return s->pcm;
}

/* сектор XA декодирован (native — стерео L,R, frames кадров): если канал подменён WAV — переписать */
int dc_mods_xa(int lba, int channel, int16_t *native, int frames)
{
    {   /* DC_MODS_AUDIO_LOG=1 — смена канала XA в журнал: какой файл и канал звучит (имя WAV для мода) */
        static int lg = -1, last_ch = -1; static const DiscFile *last_f = NULL;
        if (lg < 0) lg = env_on("DC_MODS_AUDIO_LOG", 0);
        if (lg) {
            const DiscFile *f = find((uint32_t)lba);
            if (f != last_f || channel != last_ch)
                fprintf(stderr, "modxa: %s ch%02d (lba %d)\n", f ? f->name : "?", channel, lba);
            last_f = f; last_ch = channel;
        }
    }
    for (int i = 0; i < s_nxa; i++) {
        const DcXa *x = &s_xa[i];
        if (x->chan != channel || (uint32_t)lba < x->flba || (uint32_t)lba >= x->flba + x->fnsec) continue;
        int lo = 0, hi = x->nlba - 1, k = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (x->lba[mid] == (uint32_t)lba) { k = mid; break; }
            if (x->lba[mid] < (uint32_t)lba) lo = mid + 1; else hi = mid - 1;
        }
        if (k < 0) return 0;
        const uint64_t start = (uint64_t)k * (uint64_t)frames;
        for (int f = 0; f < frames; f++) {
            const uint64_t s = start + (uint64_t)f;
            native[f * 2] = s < x->frames ? x->pcm[s * 2] : 0;
            native[f * 2 + 1] = s < x->frames ? x->pcm[s * 2 + 1] : 0;
        }
        return 1;
    }
    return 0;
}

static DiscFile *find(uint32_t lba)
{
    int lo = 0, hi = s_n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        DiscFile *f = &s_f[mid];
        if (lba < f->lba) hi = mid - 1;
        else if (lba >= f->lba + f->nsec) lo = mid + 1;
        else return f;
    }
    return NULL;
}

static void read_at(DiscFile *f, uint32_t off, uint8_t *dst, uint32_t n)
{
    size_t got = 0;
    if (off < f->msize && fseek(f->fp, (long)off, SEEK_SET) == 0) got = fread(dst, 1, n, f->fp);
    if (got < n) memset(dst + got, 0, n - got);
}

void dc_mods_raw(uint32_t lba, uint8_t *raw)
{
    if (!s_active || !raw) return;
    DiscFile *f = find(lba);
    if (!f || f->mode == M_NONE) return;
    const uint32_t k = lba - f->lba;
    if (f->mode == M_DATA) {
        read_at(f, k * 2048u, raw + 24, 2048u);
    } else if (f->mode == M_RAW2352) {
        uint8_t t[2352];
        read_at(f, k * 2352u, t, 2352u);
        memcpy(raw + 16, t + 16, 2336u);       /* синхронизация и адрес — родные */
    } else {
        read_at(f, k * 2336u, raw + 16, 2336u);
    }
}

/* 06.10.2026: a virtual sector of a bigger room - a data sector made from the file's first sector on the disc (sync, mode 2,
 * data sub-header) with its own address and the mod's bytes; 1 - it was one */
static uint8_t bcd8(uint32_t v) { return (uint8_t)(((v / 10u) << 4) | (v % 10u)); }
int dc_mods_virtual_raw(uint32_t lba, uint8_t *raw)
{
    if (!s_active || !raw || lba < 300000u) return 0;
    for (int i = 0; i < s_n; i++) {
        DiscFile *f = &s_f[i];
        if (!f->vnsec || lba < f->vlba || lba >= f->vlba + f->vnsec) continue;
        if (!s_iso || !iso_read_raw_sector(s_iso, f->lba, raw, 2352)) memset(raw, 0, 2352);
        const uint32_t a = lba + 150u;
        raw[12] = bcd8(a / 4500u); raw[13] = bcd8((a / 75u) % 60u); raw[14] = bcd8(a % 75u);
        read_at(f, (lba - f->vlba) * 2048u, raw + 24, 2048u);
        return 1;
    }
    return 0;
}

/* a virtual sector -> the same sector of the file's own place on the disc (the room the player is in is known by it) */
uint32_t dc_mods_virtual_home(uint32_t lba)
{
    if (lba < 300000u) return lba;
    for (int i = 0; i < s_n; i++) {
        const DiscFile *f = &s_f[i];
        if (f->vnsec && lba >= f->vlba && lba < f->vlba + f->vnsec) {
            const uint32_t k = lba - f->vlba;
            return f->lba + (k < f->nsec ? k : f->nsec - 1u);
        }
    }
    return lba;
}

void dc_mods_cooked(uint32_t lba, uint8_t *buf)
{
    if (!s_active || !buf) return;
    DiscFile *f = find(lba);
    if (!f || f->mode == M_NONE) return;
    const uint32_t k = lba - f->lba;
    if (f->mode == M_DATA) read_at(f, k * 2048u, buf, 2048u);
    else if (f->mode == M_RAW2352) read_at(f, k * 2352u + 24u, buf, 2048u);
    else read_at(f, k * 2336u + 8u, buf, 2048u);
}

/* ======================================================================================================================
 * ПОДХВАТ НА ЛЕТУ (06.10.2026) — шапка файла. Поток-сторож спит в ReadDirectoryChangesW; что перечитать — по первой папке
 * пути изменённого файла; через 0.7 с после последнего изменения — маска для потока эмуляции.
 * ====================================================================================================================== */
enum { RL_FILES = 1, RL_EXE = 2, RL_MESH = 4, RL_TEX = 8, RL_SND = 16, RL_MOV = 32, RL_VIEW = 64 };
#define RL_ALL (RL_FILES | RL_EXE | RL_MESH | RL_TEX | RL_SND | RL_MOV | RL_VIEW)
#ifdef _WIN32
static volatile LONG     s_live_mask;          /* изменения ждут простоя привода */
static volatile LONGLONG s_data_tick;          /* когда привод последний раз читал файл данных (не XA / STR) */
static int               s_live;               /* сторож запущен */
static char              s_live_base[MAX_PATH];

/* путь изменённого файла от папки mods -> что перечитать */
/* 10.10: что перечитать по папке категории (без RL_VIEW); -1 - не категория */
static LONG live_cat(const WCHAR *top)
{
    if (!_wcsicmp(top, L"rooms") || !_wcsicmp(top, L"doors") || !_wcsicmp(top, L"characters") || !_wcsicmp(top, L"items"))
        return RL_FILES | RL_MESH;                 /* файлы игры и модели (index.txt, папки под names.txt) */
    if (!_wcsicmp(top, L"weapons")) return RL_MESH;
    if (!_wcsicmp(top, L"screens")) return RL_FILES | RL_TEX;
    if (!_wcsicmp(top, L"textures")) return RL_TEX;
    if (!_wcsicmp(top, L"other")) return RL_FILES | RL_EXE;
    if (!_wcsicmp(top, L"movie") || !_wcsicmp(top, L"videos")) return RL_FILES | RL_MOV;
    if (!_wcsicmp(top, L"sound") || !_wcsicmp(top, L"sounds")) return RL_FILES | RL_SND;
    if (!_wcsicmp(top, L"collision") || !_wcsicmp(top, L"source")) return 0;   /* на ходу не читаются */
    return -1;
}

/* путь изменённого файла от папки mods -> что перечитать. 10.10: RL_VIEW - с пакетами вид собран из ссылок на файлы mods
 * (новый или удалённый файл - пересобрать вид); путь внутри пакета (mods\<пакет>\<категория>\...) будит свою категорию */
static LONG live_bits(const WCHAR *n, int len)
{
    int k = 0;
    while (k < len && n[k] != L'\\') k++;
    if (k == len) {                                /* файл в самой папке mods */
        if (len == 9 && !_wcsnicmp(n, L"names.txt", 9)) return RL_MESH | RL_VIEW;
        if (len == 14 && !_wcsnicmp(n, L"load_order.txt", 14)) return RL_ALL;   /* 10.10: порядок пакетов (студия) */
        return 0;
    }
    WCHAR top[96];
    if (k >= 96) return RL_ALL;
    memcpy(top, n, (size_t)k * sizeof(WCHAR));
    top[k] = 0;
    const LONG c = live_cat(top);
    if (c >= 0) return c | RL_VIEW;
    /* 10.10: пакет: сама папка (появилась, пропала) - всё; mod.json и прочее о пакете - ничего; категория внутри - её */
    const WCHAR *r = n + k + 1;
    const int rl = len - k - 1;
    if (rl <= 0) return RL_ALL;
    int k2 = 0;
    while (k2 < rl && r[k2] != L'\\') k2++;
    if (k2 == rl) {
        if ((rl == 8 && !_wcsnicmp(r, L"mod.json", 8)) || (rl == 11 && !_wcsnicmp(r, L"preview.png", 11)) ||
            (rl == 10 && !_wcsnicmp(r, L"readme.txt", 10))) return 0;
        return RL_VIEW;                            /* файл в корне пакета (names.txt и т. п.) */
    }
    WCHAR cat[32];
    if (k2 >= 32) return RL_VIEW;
    memcpy(cat, r, (size_t)k2 * sizeof(WCHAR));
    cat[k2] = 0;
    const LONG c2 = live_cat(cat);
    return (c2 >= 0 ? c2 : 0) | RL_VIEW;
}

static DWORD WINAPI live_thread(LPVOID unused)
{
    (void)unused;
    WCHAR wb[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, s_live_base, -1, wb, MAX_PATH)) return 0;
    HANDLE dir = CreateFileW(wb, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (dir == INVALID_HANDLE_VALUE) { fprintf(stderr, "mods: cannot watch %s - changes are read at the next start\n", s_live_base); return 0; }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    static DWORD buf[16384];                       /* 64 КБ, выровнено по DWORD */
    LONG pend = 0;
    ULONGLONG last = 0;
    int armed = 0;
    while (ov.hEvent) {
        if (!armed) {
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(dir, buf, sizeof buf, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                       FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, NULL, &ov, NULL)) {
                fprintf(stderr, "mods: Windows does not report changes in %s - they are read at the next start\n", s_live_base);
                break;
            }
            armed = 1;
        }
        DWORD wait = INFINITE;                     /* тихо — спим, пока Windows не скажет */
        if (pend) {
            const ULONGLONG el = GetTickCount64() - last;
            if (el >= 700) { InterlockedOr(&s_live_mask, pend); pend = 0; continue; }   /* 0.7 с без изменений — отдать */
            wait = (DWORD)(700 - el);
        }
        const DWORD w = WaitForSingleObject(ov.hEvent, wait);
        if (w == WAIT_TIMEOUT) continue;
        if (w != WAIT_OBJECT_0) break;
        armed = 0;
        DWORD got = 0;
        if (!GetOverlappedResult(dir, &ov, &got, FALSE)) break;
        if (got == 0) { pend |= RL_ALL; last = GetTickCount64(); continue; }   /* не влезло — всё */
        const uint8_t *p = (const uint8_t *)buf;
        for (;;) {
            const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)p;
            const LONG b = live_bits(fi->FileName, (int)(fi->FileNameLength / sizeof(WCHAR)));
            if (b) { pend |= b; last = GetTickCount64(); }
            if (!fi->NextEntryOffset) break;
            p += fi->NextEntryOffset;
        }
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(dir);
    return 0;
}
#endif

static void live_start(const char *base)
{
#ifdef _WIN32
    if (s_live || !env_on("DC_MODS_LIVE", 1)) return;
    snprintf(s_live_base, sizeof s_live_base, "%s", base);
    const size_t n = strlen(s_live_base);
    if (n > 3 && (s_live_base[n - 1] == '\\' || s_live_base[n - 1] == '/')) s_live_base[n - 1] = 0;
    const DWORD a = GetFileAttributesA(s_live_base);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "mods: no folder %s - nothing to watch\n", s_live_base);
        return;
    }
    HANDLE t = CreateThread(NULL, 0, live_thread, NULL, 0, NULL);
    if (!t) return;
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    s_live = 1;
    fprintf(stderr, "mods: changes in the mods folder are picked up while the game runs (DC_MODS_LIVE=0 - off)\n");
#else
    (void)base;
#endif
}

/* сектор прочитан приводом (cdrom.c): файл данных — подхват подождёт, пока игра не дочитает его (поток XA / STR — не ждёт) */
void dc_mods_note_read(uint32_t lba)
{
#ifdef _WIN32
    if (!s_live) return;
    const DiscFile *f = find(dc_mods_virtual_home(lba));
    if (f && f->stream) return;
    s_data_tick = (LONGLONG)GetTickCount64();
#else
    (void)lba;
#endif
}

int dc_mods_changed(void)
{
#ifdef _WIN32
    if (!s_live_mask) return 0;                    /* обычный случай: одно чтение */
    return GetTickCount64() - (ULONGLONG)s_data_tick >= 500u;
#else
    return 0;
#endif
}

/* файлы игры, чей мод изменился (размер или время), появился или исчез, открываются заново; остальные не трогаются.
 * *big — у большой комнаты поменялись сектора или размер (запись таблицы файлов в RAM) */
static int files_rescan(const char *base, int *big)
{
    *big = 0;
#ifdef _WIN32
    typedef struct { const char *cat; char name[32]; uint64_t stamp; } Seen;
    Seen *sv = NULL;
    int ns = 0, cs = 0;
    for (size_t c = 0; c < sizeof cats / sizeof cats[0]; c++) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s%s\\*", base, cats[c]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strlen(fd.cFileName) >= 32) continue;
            if (ns == cs) {
                const int nc = cs ? cs * 2 : 256;
                Seen *nv = (Seen *)realloc(sv, (size_t)nc * sizeof *nv);
                if (!nv) break;
                sv = nv; cs = nc;
            }
            sv[ns].cat = cats[c];
            snprintf(sv[ns].name, sizeof sv[ns].name, "%s", fd.cFileName);
            sv[ns].stamp = fd_stamp(&fd);
            ns++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    int changed = 0;
    for (int i = 0; i < s_n; i++) {
        DiscFile *f = &s_f[i];
        uint64_t st = 0;
        for (int k = 0; k < ns && !st; k++)
            if (!strcmp(sv[k].cat, f->cat0) && !_stricmp(sv[k].name, f->name)) st = sv[k].stamp;
        for (int k = 0; k < ns && !st && f->old_cat; k++)
            if (!strcmp(sv[k].cat, f->old_cat) && !_stricmp(sv[k].name, f->name)) st = sv[k].stamp;
        if (st == f->stamp) continue;
        changed++;
        const int was = f->mode != M_NONE;
        const uint32_t ov = f->vnsec ? f->vlba : 0, osz = f->vnsec ? f->msize : 0;
        mod_open(f, base);
        if ((f->vnsec ? f->vlba : 0) != ov || (f->vnsec ? f->msize : 0) != osz) *big = 1;
        if (was && f->mode == M_NONE && !f->stamp) fprintf(stderr, "mods: %s\\%s is gone - the game's own file again\n", f->cat0, f->name);
    }
    free(sv);
    s_active = 0;
    for (int i = 0; i < s_n; i++) if (s_f[i].mode != M_NONE) s_active++;
    return changed;
#else
    (void)base;
    return 0;
#endif
}

void dc_mods_reload(void)
{
#ifdef _WIN32
    LONG m = InterlockedExchange(&s_live_mask, 0);
    if (!m) return;
    const ULONGLONG t0 = GetTickCount64();
    if (m & RL_VIEW) {                             /* 10.10: пакеты - сначала вид, потом загрузчики читают его */
        char was[MAX_PATH];
        mods_base(was, sizeof was);
        const int vr = view_prepare();
        char now[MAX_PATH];
        mods_base(now, sizeof now);
        if (strcmp(was, now) != 0 || vr == 2) m |= RL_ALL;   /* вид появился, пропал или пересобран: перечитать всё */
    }
    char base[MAX_PATH];
    mods_base(base, sizeof base);
    int nf = 0, big = 0;
    if (m & RL_FILES) nf = files_rescan(base, &big);
    if ((m & RL_EXE) || big) exe_reapply();      /* other\SLUS_009.22; запись таблицы файлов большой комнаты */
    if (m & (RL_SND | RL_MOV)) audio_movie_load(base);
    if (m & RL_MESH) { extern void dc_meshrep_reload(const char *mods); dc_meshrep_reload(base); }
    if (m & RL_TEX) { extern void dcvk_mods_reload(void); dcvk_mods_reload(); }
    fprintf(stderr, "mods: picked up on the fly:%s%s%s%s%s%s%s - %d game file(s) changed, %u ms\n",
            (s_view_on && (m & RL_VIEW)) ? " packages" : "",
            (m & RL_FILES) ? " files" : "", (m & RL_EXE) ? " program" : "", (m & RL_MESH) ? " meshes" : "",
            (m & RL_TEX) ? " textures" : "", (m & RL_SND) ? " sounds" : "", (m & RL_MOV) ? " movies" : "", nf,
            (unsigned)(GetTickCount64() - t0));
#endif
}

/* ======================================================================================================================
 * SOUNDS OF AN ENEMY OF ANOTHER STAGE (06.10.2026). Player: "the dinosaurs I added sound like other dinosaurs"; "make the sound
 * memory bigger". A sound effect is func_8005C9A4(actor, id): the entry 0x801FE900 + id*4 (b0: bank slot in bits 0-5, kind
 * in 6-7; b1 program; b2 priority / first tone; b3 voice / extra tones) and the header of that slot's bank from
 * 0x8009E538[slot] (tones of 24 bytes from +144, the sample's SPU address /8 at +22). Ids 0x30..0x8F are filled by the files
 * of the stage (bank slot 2, samples from 0x4D190), 0x90..0xDF by the room's (slot 3, from 0x65830). An enemy of another
 * stage calls the ids of ITS stage and room, and here they name ours. For a call from it - its code runs from a bank in the
 * memory for mods (the return address), or the actor is of a kind whose code does - the entry of its own place and its
 * bank's header go into the tables for the length of the call: the game picks the voice, pitch, envelope, volume and the
 * position itself; the voices whose start address is set meanwhile read their samples from an extended sound page (spu.c),
 * where its stage's and room's banks lie at their own addresses. The header copies live past the end of the bank's code
 * (bank + 0x18000.., the window is 0x18000). Where it came from: the Studio's note at bank + 0x1FF00 ("DCFKSND1", kind ->
 * room), else the room's kinds folders (mods\rooms\<ROOM>\kinds\<actor>_<SRCROOM>). A room without a bank of slot 2 (ST407)
 * takes its stage's from the nearest room of the stage before it that has one. DC_FK_SOUND=0 - off. */
#include "cpu_state.h"
extern void func_8005C9A4(CPUState *cpu);
extern const uint8_t *memory_mod_ptr(uint32_t phys, uint32_t len);
extern uint8_t *spu_xpage(int page);
extern void spu_xpage_capture(int page);
extern int dc_room_current(void);
extern const char *dc_room_name(int room);

#define FKS_BANK0   0x02E00000u           /* phys of bank 0; 128 KB each */
#define FKS_BANKSZ  0x20000u
#define FKS_NBANK   8
#define FKS_TAIL    0x18000u              /* past the window: the header copies */
#define FKS_NOTE    0x1FF00u              /* the Studio's note */
#define FKS_ROOMS   8                     /* source rooms at once = extended sound pages */

typedef struct {
    char     room[8];                     /* "ST603" */
    uint8_t  ent[256][4], have[256];      /* its entries 0x30..0xDF */
    uint32_t hdr[8];                      /* the game's address of its bank headers by slot (0 - none) */
} FkRoom;
static FkRoom s_fkr[FKS_ROOMS];           /* index + 1 = its extended sound page */
static int    s_nfkr;
static int    s_fkr_room = -1;            /* the room these were read for */

typedef struct {
    uint8_t sig[32];                      /* the bank's first bytes when it was read */
    int     state;                        /* 0 to read, 1 ready, -1 nothing usable */
    int     room_at;                      /* the room it was read in */
    int     nk;
    uint8_t kind[16];
    int8_t  ridx[16];                     /* kind -> s_fkr */
    int8_t  ridx0;                        /* any other caller of this bank */
} FkBank;
static FkBank s_fkb[FKS_NBANK];

typedef struct { uint32_t type, size, dest, param, off; } FkRec;
static int fks_records(const uint8_t *d, uint32_t n, FkRec *out, int cap)
{
    int k = 0;
    uint32_t pos = 0x800;
    for (uint32_t o = 0; o + 16u <= 0x800u && o + 16u <= n && k < cap; o += 16) {
        const uint32_t t = le32(d + o), sz = le32(d + o + 4), dest = le32(d + o + 8), par = le32(d + o + 12);
        if (t > 20 || sz == 0 || sz > n || pos + sz > n) break;
        out[k].type = t; out[k].size = sz; out[k].dest = dest; out[k].param = par; out[k].off = pos;
        k++;
        pos = (pos + sz + 2047u) & ~2047u;
    }
    return k;
}

/* a room file as the game reads it (with its mod); NULL - none */
static uint8_t *fks_file(const char *stem, uint32_t *n)
{
    DiscFile *f = find_stem(stem, ".DAT");
    if (!f) return NULL;
    uint8_t *d = read_game_file(f);
    if (d) *n = f->nsec * 2048u;
    return d;
}

/* the sound records of one file into R: what = 1 the room's own (every slot 2/3 and every entry), 2 only the stage's part
 * (slot 2 bank and the entries 0x30..0x8F); 1 - it had a slot 2 bank */
static int fks_take(FkRoom *R, int page, const uint8_t *d, uint32_t n, uint32_t tail_phys, int what)
{
    FkRec rec[64];
    const int nr = fks_records(d, n, rec, 64);
    uint8_t *pg = spu_xpage(page);
    uint8_t *tail = (uint8_t *)memory_mod_ptr(tail_phys, 0x1C00u);
    int had2 = 0;
    for (int i = 0; i < nr; i++) {
        const FkRec *r = &rec[i];
        const uint32_t slot = (r->param >> 16) & 0xFFu;
        if (r->type == 0 && r->dest >= 0x801FE9C0u && r->dest < 0x801FEC80u) {
            const uint32_t id0 = (r->dest - 0x801FE900u) / 4u;
            for (uint32_t k = 0; k < r->size / 4u; k++) {
                const uint32_t id = id0 + k;
                if (id < 0x30u || id > 0xDFu || (what == 2 && id > 0x8Fu)) continue;
                memcpy(R->ent[id], d + r->off + k * 4u, 4);
                R->have[id] = 1;
            }
        } else if (r->type == 3 && (slot == 2 || (slot == 3 && what == 1)) && r->size <= 0xE00u && tail) {
            memcpy(tail + (slot == 2 ? 0u : 0xE00u), d + r->off, r->size);
            R->hdr[slot] = 0x80000000u | (tail_phys + (slot == 2 ? 0u : 0xE00u));
            if (slot == 2) had2 = 1;
        } else if (r->type == 4 && (slot == 2 || (slot == 3 && what == 1)) && pg && r->dest + r->size <= 0x80000u) {
            memcpy(pg + r->dest, d + r->off, r->size);
        }
    }
    return had2;
}

/* s_fkr index of a source room (read now if new); -1 - cannot */
static int fks_room(const char *room, uint32_t tail_phys)
{
    for (int i = 0; i < s_nfkr; i++) if (same_name(s_fkr[i].room, room)) return i;
    if (s_nfkr >= FKS_ROOMS || strlen(room) != 5) return -1;
    uint32_t n = 0;
    uint8_t *d = fks_file(room, &n);
    if (!d) { fprintf(stderr, "fksound: %s is not on the disc\n", room); return -1; }
    FkRoom *R = &s_fkr[s_nfkr];
    memset(R, 0, sizeof *R);
    snprintf(R->room, sizeof R->room, "%s", room);
    uint8_t *pg = spu_xpage(s_nfkr + 1);
    if (pg) memset(pg, 0, 0x80000u);
    int had2 = fks_take(R, s_nfkr + 1, d, n, tail_phys, 1);
    free(d);
    char from[8] = "";
    if (!had2) {                                  /* its stage's bank is loaded by another room of the stage */
        const int me = (int)strtol(room + 3, NULL, 16);
        for (int step = 1; step < 0x40 && !had2; step++)
            for (int dir = -1; dir <= 1 && !had2; dir += 2) {
                const int o = me + dir * step;
                if (o < 0 || o > 0xFF) continue;
                char cand[8];
                snprintf(cand, sizeof cand, "ST%c%02X", room[2], o);
                uint32_t m = 0;
                uint8_t *e = fks_file(cand, &m);
                if (!e) continue;
                FkRec rec[64];
                const int nr = fks_records(e, m, rec, 64);
                int has = 0;
                for (int i = 0; i < nr; i++) if (rec[i].type == 3 && ((rec[i].param >> 16) & 0xFFu) == 2u) has = 1;
                if (has) { had2 = fks_take(R, s_nfkr + 1, e, m, tail_phys, 2); snprintf(from, sizeof from, "%s", cand); }
                free(e);
            }
    }
    int ne = 0;
    for (int id = 0x30; id <= 0xDF; id++) ne += R->have[id];
    fprintf(stderr, "fksound: sounds of %s - %d entries, banks: stage %s, room %s; extended sound page %d\n", room, ne,
            R->hdr[2] ? (from[0] ? from : "its own") : "none", R->hdr[3] ? "its own" : "none", s_nfkr + 1);
    return s_nfkr++;
}

/* the stage of a bank: its first bytes are STn.BIN's */
static int fks_bank_stage(const uint8_t *bank)
{
    for (int s = 1; s <= 9; s++) {
        char nm[8];
        snprintf(nm, sizeof nm, "ST%d", s);
        DiscFile *f = find_stem(nm, ".BIN");
        if (!f) continue;
        uint8_t *d = read_game_file(f);
        if (!d) continue;
        const int same = f->size >= 0x800u && !memcmp(d, bank, 0x800);
        free(d);
        if (same) return s;
    }
    return 0;
}

/* read the sounds of bank k for this room */
static void fks_bank_read(int k)
{
    FkBank *B = &s_fkb[k];
    const uint32_t bphys = FKS_BANK0 + (uint32_t)k * FKS_BANKSZ;
    const uint8_t *bank = memory_mod_ptr(bphys, FKS_BANKSZ);
    B->state = -1; B->nk = 0; B->ridx0 = -1;
    if (!bank) return;
    const int cur = dc_room_current();
    if (cur != s_fkr_room) { s_nfkr = 0; s_fkr_room = cur; }
    B->room_at = cur;
    int local = 0;                                /* header copies of this bank: 0x1C00 each past its window */
    const uint8_t *note = bank + FKS_NOTE;
    if (!memcmp(note, "DCFKSND1", 8)) {
        const int n = note[8] > 15 ? 15 : note[8];
        for (int i = 0; i < n && local < 4; i++) {
            const uint8_t *e = note + 16 + i * 16;
            char room[16];
            memcpy(room, e + 2, 14); room[14] = 0;
            int ri = -1;
            for (int j = 0; j < B->nk; j++) if (B->ridx[j] >= 0 && same_name(s_fkr[B->ridx[j]].room, room)) ri = B->ridx[j];
            if (ri < 0) ri = fks_room(room, bphys + FKS_TAIL + (uint32_t)(local++) * 0x1C00u);
            if (ri < 0) continue;
            B->kind[B->nk] = e[0]; B->ridx[B->nk] = (int8_t)ri; B->nk++;
            if (B->ridx0 < 0) B->ridx0 = (int8_t)ri;
        }
    } else {                                      /* built before the note: the room's kinds folders say where from */
        const char *mods = dc_mods_dir();
        const int st = fks_bank_stage(bank);
        if (mods && st) {
#ifdef _WIN32
            char pat[MAX_PATH];
            snprintf(pat, sizeof pat, "%srooms\\%s\\kinds\\*", mods, dc_room_name(cur));
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    const char *u = strrchr(fd.cFileName, '_');
                    if (!u || strlen(u + 1) != 5 || toupper((unsigned char)u[1]) != 'S' || u[3] != '0' + st) continue;
                    B->ridx0 = (int8_t)fks_room(u + 1, bphys + FKS_TAIL);
                } while (B->ridx0 < 0 && FindNextFileA(h, &fd));
                FindClose(h);
            }
#endif
        }
        if (B->ridx0 < 0) fprintf(stderr, "fksound: bank %d (stage %d) - no note and no kinds folder, its sounds stay the room's\n", k, st);
    }
    if (B->ridx0 >= 0) B->state = 1;
}

static int fks_on(void) { static int on = -1; if (on < 0) on = env_on("DC_FK_SOUND", 1); return on; }

/* does the room's stage table run a kind from bank k */
static int fks_bank_used(const uint8_t *ram, int k)
{
    const uint32_t tab = le32(ram + 0xB56F0u) & 0x1FFFFFu;
    if (tab < 0x10000u || tab + 128u > 0x200000u) return 0;
    const uint32_t lo = FKS_BANK0 + (uint32_t)k * FKS_BANKSZ;
    for (int i = 0; i < 32; i++) {
        const uint32_t fn = le32(ram + tab + (uint32_t)i * 4u) & 0x1FFFFFFFu;
        if (fn >= lo && fn < lo + FKS_BANKSZ) return 1;
    }
    return 0;
}

/* ===== ENEMIES STAND DOWN IN A SCENE OF A ROOM OF MODS (07.10.2026) ===================================================
 * Player: "in my room the skip of the scene skips one shot and stops; without the dinosaurs it works". A sequence puts its
 * own bits into GS+64 (func_8001E2D4 / E558 / F860, the mask of the sequence): the Gail scene of ST10D puts 0x804 - no bit
 * 1 ("the enemies stand down", tested by the raptor func_8002FAAC, func_80022688 and the kinds of the stages), as that room
 * had no enemies. The pteranodon the player added dived at her mid-dialogue (measured: her mode 4 -> 2 by its code), the
 * sequence broke off (0x800 gone) and the skip ended on it. In a room read from mods, while the script leads her (her mode
 * 4) the enemies read bit 1 as set - only what they read (dirty_ram_interp.c LW, hooks at 0x8002269C and
 * 0x8002FAEC), RAM keeps the game's value, so a quick save in the middle keeps nothing of it. DC_SCENE_STANDDOWN=0 - off. */
volatile int g_dc_standdown = 0;
static void standdown_tick(void)
{
    static int on = -1, room = -2, modded = 0;
    if (on < 0) on = env_on("DC_SCENE_STANDDOWN", 0);   /* 07.10: off - not the game's own rule (player: only the game's logic) */
    const uint8_t *ram = memory_get_ram_ptr();
    if (!on || !ram) { g_dc_standdown = 0; return; }
    const int cur = dc_room_current();
    if (cur != room) {
        room = cur;
        DiscFile *f = cur >= 0 ? find_stem(dc_room_name(cur), ".DAT") : NULL;
        modded = f && f->mode != M_NONE;
    }
    const uint32_t gs64 = le32(ram + 0xB7890u);
    /* her mode 4 - the script leads her (the Gail scene: 0x804 and mode 4). Not GS+64 0x800: the game puts it for every
     * hit reaction too (mode 2), and the enemies froze after each hit */
    const int seq = ram[0xB92C8] == 4;
    const int was = g_dc_standdown;
    g_dc_standdown = modded && seq && !(gs64 & 2u);
    if (g_dc_standdown != was) {
        static int n = 0;
        if (++n <= 40)
            fprintf(stderr, "standdown: %s - the enemies %s (room of mods, GS+64 %08X, her mode %d)\n", g_dc_standdown ? "a sequence runs" :
                    "the sequence is over", g_dc_standdown ? "stand down" : "are free again", (unsigned)gs64, ram[0xB92C8]);
    }
}

void dc_fk_sound_tick(void)
{
    standdown_tick();
    {   /* 07.10.2026: the handler slots of Regina's special modes - their per-family copies are of one room */
        extern void dc_fk_gsv_reset(void);
        static int gs_room = -2;
        const int rm = dc_room_current();
        if (rm != gs_room) { gs_room = rm; dc_fk_gsv_reset(); }
    }
    if (!fks_on()) return;
    const uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    static int quiet[FKS_NBANK];
    const int cur = dc_room_current();
    for (int k = 0; k < FKS_NBANK; k++) {
        FkBank *B = &s_fkb[k];
        const uint8_t *p = memory_mod_ptr(FKS_BANK0 + (uint32_t)k * FKS_BANKSZ, 32);
        if (!p) return;
        if (memcmp(p, B->sig, 32) || (B->state != 0 && B->room_at != cur)) {
            memcpy(B->sig, p, 32);
            B->state = 0;
            quiet[k] = 0;
            continue;
        }
        /* read while the room settles: the bank and the table are written by the room's load */
        if (B->state == 0 && ++quiet[k] >= 30 && fks_bank_used(ram, k)) fks_bank_read(k);
    }
}

int dc_fk_sound(CPUState *cpu)
{
    static int in = 0;
    if (in || !fks_on()) return 0;
    const uint32_t id = cpu->gpr[5] & 0xFFu;
    if (id < 0x30u || id > 0xDFu) return 0;
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return 0;
    int k = -1, kind = -1;
    const uint32_t a0 = cpu->gpr[4] & 0x1FFFFFFFu;
    if (a0 >= 0xB7ACCu && a0 < 0xB7ACCu + 10u * 608u && (a0 - 0xB7ACCu) % 608u == 0u) {
        kind = ram[a0 + 39];
        const uint32_t tab = le32(ram + 0xB56F0u) & 0x1FFFFFu;
        if (tab >= 0x10000u && tab + 128u <= 0x200000u && kind < 32) {
            const uint32_t fn = le32(ram + tab + (uint32_t)kind * 4u) & 0x1FFFFFFFu;
            if (fn >= FKS_BANK0 && fn < FKS_BANK0 + FKS_NBANK * FKS_BANKSZ) k = (int)((fn - FKS_BANK0) / FKS_BANKSZ);
        }
    }
    if (k < 0) {
        const uint32_t ra = cpu->gpr[31] & 0x1FFFFFFFu;
        if (ra >= FKS_BANK0 && ra < FKS_BANK0 + FKS_NBANK * FKS_BANKSZ) k = (int)((ra - FKS_BANK0) / FKS_BANKSZ);
    }
    if (k < 0) return 0;
    FkBank *B = &s_fkb[k];
    if (B->state == 0 || B->room_at != dc_room_current()) {
        const uint8_t *p = memory_mod_ptr(FKS_BANK0 + (uint32_t)k * FKS_BANKSZ, 32);
        if (p) memcpy(B->sig, p, 32);
        fks_bank_read(k);
    }
    if (B->state != 1) return 0;
    int ri = B->ridx0;
    for (int i = 0; i < B->nk; i++) if (B->kind[i] == kind) ri = B->ridx[i];
    if (ri < 0 || ri >= s_nfkr) return 0;
    const FkRoom *R = &s_fkr[ri];
    if (!R->have[id]) return 0;                   /* its place has no such sound: as the room has it */
    const uint8_t *e = R->ent[id];
    const uint32_t slot = e[0] & 0x3Fu;
    if ((e[0] >> 6) == 2u && (slot >= 8u || !R->hdr[slot])) return 0;
    uint8_t keep_e[4], keep_t[4];
    const uint32_t ea = 0x1FE900u + id * 4u, ta = 0x9E538u + (slot & 7u) * 4u;
    memcpy(keep_e, ram + ea, 4);
    memcpy(keep_t, ram + ta, 4);
    memcpy(ram + ea, e, 4);
    if (slot < 8u && R->hdr[slot]) { const uint32_t h = R->hdr[slot]; memcpy(ram + ta, &h, 4); }
    {   static int lg = -1; static int nlog = 0;
        if (lg < 0) lg = env_on("DC_FK_SOUND_LOG", 0);
        if (lg && nlog < 400) { nlog++;
            fprintf(stderr, "fksound: actor %05X kind %d bank %d id %02X -> %s entry %02X%02X%02X%02X\n", (unsigned)a0, kind, k,
                    (unsigned)id, R->room, e[0], e[1], e[2], e[3]); } }
    spu_xpage_capture(ri + 1);
    in = 1;
    func_8005C9A4(cpu);
    in = 0;
    spu_xpage_capture(0);
    memcpy(ram + ea, keep_e, 4);
    memcpy(ram + ta, keep_t, 4);
    return 1;
}
