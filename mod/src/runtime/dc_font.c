/* dc_font — растеризация TTF и вывод UTF-8 строк в RGBA-буфер. См. dc_font.h.
 *
 * Растеризатор — stb_truetype, который уже лежит в дереве вместе с Dear ImGui
 * (deps/imgui/imstb_truetype.h). ImGui собирает свою копию со STBTT_STATIC, то
 * есть её символы не покидают imgui_draw.cpp, поэтому вторая статическая копия
 * здесь ни с чем не конфликтует.
 */
#include "dc_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 10.10.2026: объявления для бесплатного компилятора установщика (Clang): здесь вызывались функции без объявления
 * выше по файлу - MSVC подставлял int f() сам, Clang требует объявления. */
static int try_load_bold(const char *path);
static int try_load_fallback(const char *path);

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../deps/stb/stb_truetype.h"   /* ME: stb_truetype v1.26, public domain - not the copy inside ImGui */

/* ---- загруженный шрифт ---------------------------------------------------- */

static unsigned char *s_ttf;             /* содержимое файла, живёт до конца */
static stbtt_fontinfo  s_info;
static int   s_ready;
static int   s_size = 24;                /* кегль в пикселях канваса */
static float s_scale;                    /* юниты шрифта -> пиксели */
static int   s_ascent, s_descent, s_linegap;
static char  s_path[512];
static char  s_family[128];

/* Порог бинаризации: пиксельный шрифт должен остаться пиксельным, без серой
 * каймы. Для векторного шрифта его можно поднять/опустить через DC_FONT_AA. */
static int s_threshold = 128;
static int s_antialias;                  /* 1 — оставить полутона (вектор) */

/* Родной шрифт игры заметно жирнее и идёт плотнее, чем ложится Fusion Pixel
 * из коробки: linegap шрифта даёт 32 пикселя на строку при кегле 24, а штрих
 * тоньше родного. Обе величины настраиваются, чтобы попасть в вид игры. */
static int s_bold;                       /* 1 — второй прогон со сдвигом на пиксель */
static int s_outline_px;                 /* 0 = авто по кеглю */
static int s_linegap_on;                 /* 1 — учитывать linegap шрифта */

/* ---- кэш глифов ----------------------------------------------------------- */

typedef struct Glyph {
    int            cp;                   /* кодовая точка, -1 = слот пуст */
    unsigned char *bmp;                  /* 8bpp, w*h */
    int            w, h, xoff, yoff, adv;
    struct Glyph  *next;                 /* цепочка коллизий */
} Glyph;

#define GLYPH_BUCKETS 2048
static Glyph *s_cache[GLYPH_BUCKETS];

/* Второй шрифт — ЖИРНЫЙ (заголовки, титульные надписи игры; просьба игрока
 * 2026-09-02: «DIN Next LT Pro Regular — обычный текст, Bold/Medium —
 * заголовки и выделения»). Файл: DC_FONT_BOLD_FILE или assets/fonts/
 * dc_ui_bold.ttf. Нет файла — жирный стиль берёт обычный шрифт. */
static unsigned char *s_ttf_b;
static stbtt_fontinfo  s_info_b;
static int   s_ready_b;
static float s_scale_b;
static char  s_path_b[512];
static Glyph *s_cache_b[GLYPH_BUCKETS];
static int   s_aa_env_set;               /* DC_FONT_AA задан явно */

/* ===== СУПЕРСЭМПЛИНГ СЛОЯ ИНТЕРФЕЙСА (2026-09-05, игрок: «примени нормальный
 * фонт») ==================================================================
 * Слой 640×480 растягивался в окно 1920×1080 билинейно (×2.25, а при открытом
 * меню ещё и ×1.33 только по горизонтали) — буквы кегля 24 выходили мыльными
 * и широкими. Теперь слой хранится в S раз крупнее (DC_UI_SS, по умолчанию 3:
 * 1920×1440), а глифы для него растеризуются кеглем 24·S в ОТДЕЛЬНОМ кеше —
 * все внешние координаты dc_ui_* остаются логическими 640×480, метрики
 * (dc_font_line_height / dc_font_text_width) — логическими, атласы шрифта
 * игры и маски подсказок (dc_font_render_cov) работают как прежде своим
 * кеглем. */
static int   s_ss = 0;                   /* 0 = env ещё не прочитан */
static Glyph *s_cache_ui[GLYPH_BUCKETS];
static Glyph *s_cache_ui_b[GLYPH_BUCKETS];
static int ui_ss(void)
{
    if (!s_ss) {
        const char *e = getenv("DC_UI_SS");
        int v = e ? atoi(e) : 3;
        if (v < 1) v = 1;
        if (v > 4) v = 4;
        s_ss = v;
    }
    return s_ss;
}
int dc_ui_scale(void) { return ui_ss(); }

/* ЗАПАСНОЙ шрифт для знаков, которых нет в основном (2026-09-02: игрок выбрал
 * Shippori Mincho — в нём нет кириллицы и греческого, а русский интерфейс и
 * ячейки αβγ атласа игры нужны). Глиф, отсутствующий в основном/жирном файле,
 * берётся отсюда тем же кеглем. Файл: DC_FONT_FALLBACK_FILE или
 * assets/fonts/dc_ui_fallback.ttf (сейчас — Bahnschrift). */
static unsigned char *s_ttf_fb;
static stbtt_fontinfo  s_info_fb;
static int   s_ready_fb;
static float s_scale_fb;
static char  s_path_fb[512];

/* ИЕРОГЛИФЫ (10.09.2026): интерфейс на японском и китайском. Ни в одном нашем
 * шрифте их нет, а класть CJK-шрифт в мод — это 10-20 МБ и чужая лицензия.
 * Поэтому второй запасной шрифт берётся из системы (%WINDIR%\Fonts): для
 * японского Yu Gothic, для остальных языков Microsoft YaHei — он нужен и им,
 * чтобы в списке языков читались «日本語» и «简体中文». DC_FONT_CJK — свой файл. */
static unsigned char *s_ttf_cjk;
static stbtt_fontinfo  s_info_cjk;
static int   s_ready_cjk;
static float s_scale_cjk;
static char  s_path_cjk[512];
/* ⭐ 19.10.2026 (игрок: «запуск - ускорить»): системный шрифт с иероглифами (msyh.ttc ~20 МБ) читался при каждом запуске на
 * любом языке, а нужен он, только когда на экране иероглиф (японский и китайский интерфейс; на остальных языках - имена
 * «日本語»/«简体中文» в списке языков). Теперь на этих языках он загружается при первом знаке, которого нет в своих шрифтах
 * (cjk_load_for_lang из glyph_get_ex), - тем же списком файлов; японский и китайский грузят его сразу, как было.
 * DC_FONT_CJK_EAGER=1 - всегда сразу (как было). */
static int  s_cjk_pending = 0;            /* 1 - шрифт выбран для языка, но ещё не читался */
static int  s_cjk_lang = 0;
static int  cjk_load_for_lang(int lang, int clear);

static void cache_clear_tab(Glyph **tab)
{
    for (int i = 0; i < GLYPH_BUCKETS; i++) {
        Glyph *g = tab[i];
        while (g) {
            Glyph *n = g->next;
            if (g->bmp) stbtt_FreeBitmap(g->bmp, NULL);
            free(g);
            g = n;
        }
        tab[i] = NULL;
    }
}
static void cache_clear(void) { cache_clear_tab(s_cache); cache_clear_tab(s_cache_b);
                                cache_clear_tab(s_cache_ui); cache_clear_tab(s_cache_ui_b); }

/* style 0 — обычный, 1 — жирный (если жирного файла нет — обычный);
 * ui 1 — глиф для суперсэмплированного слоя интерфейса (кегль ×S, свой кеш). */
static Glyph *glyph_get_ex(int cp, int style, int ui)
{
    int b = (style && s_ready_b) ? 1 : 0;
    Glyph **tab = ui ? (b ? s_cache_ui_b : s_cache_ui) : (b ? s_cache_b : s_cache);
    const stbtt_fontinfo *fi = b ? &s_info_b : &s_info;
    float mul = ui ? (float)ui_ss() : 1.0f;
    float sc = (b ? s_scale_b : s_scale) * mul;

    unsigned h = ((unsigned)cp * 2654435761u) & (GLYPH_BUCKETS - 1);
    for (Glyph *g = tab[h]; g; g = g->next)
        if (g->cp == cp) return g;

    if (!s_ready) return NULL;

    /* знака нет в выбранном файле — запасной шрифт (кириллица и т.п.) */
    if (cp != ' ' && stbtt_FindGlyphIndex(fi, cp) == 0) {
        if (b && stbtt_FindGlyphIndex(&s_info, cp) != 0) { fi = &s_info; sc = s_scale * mul; }
        else if (s_ready_fb && stbtt_FindGlyphIndex(&s_info_fb, cp) != 0) { fi = &s_info_fb; sc = s_scale_fb * mul; }
        else {
            if (s_cjk_pending) { s_cjk_pending = 0; cjk_load_for_lang(s_cjk_lang, 0); }   /* 19.10: шрифт иероглифов - по первому знаку */
            if (s_ready_cjk && stbtt_FindGlyphIndex(&s_info_cjk, cp) != 0) { fi = &s_info_cjk; sc = s_scale_cjk * mul; }
        }
    }

    Glyph *g = (Glyph *)calloc(1, sizeof(Glyph));
    if (!g) return NULL;
    g->cp = cp;

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
    g->adv = (int)(adv * sc + 0.5f);

    int w = 0, hh = 0, xo = 0, yo = 0;
    g->bmp = stbtt_GetCodepointBitmap(fi, sc, sc, cp, &w, &hh, &xo, &yo);
    g->w = w; g->h = hh; g->xoff = xo; g->yoff = yo;

    /* Пиксельный шрифт: убираем полутона, иначе на каждом штрихе появляется
     * серая кайма и буква перестаёт быть пиксельной. */
    if (g->bmp && !s_antialias) {
        int n = w * hh;
        for (int i = 0; i < n; i++)
            g->bmp[i] = (g->bmp[i] >= s_threshold) ? 255 : 0;
    }

    g->next = tab[h];
    tab[h] = g;
    return g;
}
static Glyph *glyph_get_s(int cp, int style) { return glyph_get_ex(cp, style, 0); }
static Glyph *glyph_get(int cp) { return glyph_get_s(cp, 0); }

/* ---- UTF-8 ---------------------------------------------------------------- */

static const char *utf8_next(const char *s, int *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned c = *p++;
    if (c < 0x80) { *cp = (int)c; return (const char *)p; }
    int extra, min;
    if      ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; min = 0x10000; }
    else { *cp = 0xFFFD; return (const char *)p; }   /* мусорный байт */
    while (extra--) {
        if ((*p & 0xC0) != 0x80) { *cp = 0xFFFD; return (const char *)p; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *cp = (c < (unsigned)min) ? 0xFFFD : (int)c;
    return (const char *)p;
}

/* ---- загрузка ------------------------------------------------------------- */

static int try_load(const char *path)
{
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (200 << 20)) { fclose(f); return 0; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return 0; }

    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    /* 18.10.2026 (разбор кода): шрифт разбирается в своей структуре и встаёт на место только целым. stbtt_InitFont
     * пишет в структуру до проверок - битый файл оставлял s_info на уже освобождённом буфере, а шрифт считался
     * готовым (смена языка с таким файлом - обращение к освобождённой памяти). То же у трёх загрузчиков ниже. */
    stbtt_fontinfo fi;
    if (off < 0 || !stbtt_InitFont(&fi, buf, off)) { free(buf); return 0; }

    if (s_ttf) { cache_clear(); free(s_ttf); }
    s_info = fi;
    s_ttf = buf;
    snprintf(s_path, sizeof s_path, "%s", path);

    /* имя семейства (name id 1), UTF-16BE -> грубо в ASCII, только для лога */
    s_family[0] = 0;
    {
        int len = 0;
        const char *nm = stbtt_GetFontNameString(&s_info, &len,
                            STBTT_PLATFORM_ID_MICROSOFT,
                            STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
        if (nm && len > 0) {
            int o = 0;
            for (int i = 1; i < len && o < (int)sizeof(s_family) - 1; i += 2)
                s_family[o++] = nm[i];
            s_family[o] = 0;
        }
    }

    stbtt_GetFontVMetrics(&s_info, &s_ascent, &s_descent, &s_linegap);
    s_ready = 1;
    dc_font_set_size(s_size);
    return 1;
}

/* ME (17.09.2026): the interface font is the system Bahnschrift (Windows 10
 * and 11), read straight from the Windows font folder - nothing is copied next
 * to the game, and it is a Microsoft font that cannot travel with the mod
 * anyway. Segoe UI, then Arial, on a Windows without it. 0 when none is there. */
static int sys_ui_font(char *out, size_t n)
{
    static const char *const names[] = { "bahnschrift.ttf", "segoeui.ttf", "arial.ttf" };
    const char *win = getenv("WINDIR");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(out, n, "%s/Fonts/%s", (win && *win) ? win : "C:/Windows", names[i]);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

int dc_font_init(void)
{
    if (s_ready) return 1;

    const char *e = getenv("DC_FONT_PX");
    if (e) { int v = atoi(e); if (v >= 6 && v <= 128) s_size = v; }
    e = getenv("DC_FONT_AA");
    s_aa_env_set = (e != NULL);
    s_antialias = (e && *e == '1') ? 1 : 0;
    e = getenv("DC_FONT_THRESHOLD");
    if (e) { int v = atoi(e); if (v > 0 && v < 256) s_threshold = v; }
    e = getenv("DC_FONT_BOLD");
    s_bold = (e && *e == '1') ? 1 : 0;
    e = getenv("DC_FONT_OUTLINE");
    if (e) { int v = atoi(e); if (v >= 0 && v <= 4) s_outline_px = v; }
    e = getenv("DC_FONT_LINEGAP");
    s_linegap_on = (e && *e == '1') ? 1 : 0;

    const char *env = getenv("DC_FONT");
    if (try_load(env)) goto done;

    /* Обычные места: рядом с exe, в assets проекта, в дереве сборки. */
    {
        /* ME: the system font (sys_ui_font), no font files */
        char sys[512];
        if (sys_ui_font(sys, sizeof sys) && try_load(sys)) goto done;
    }

    fprintf(stderr, "dc_font: шрифт не найден "
                    "(положи ttf в assets/fonts/dc_ui.ttf или задай DC_FONT)\n");
    return 0;

done:
    /* Сглаживание: пиксельный шрифт (Fusion Pixel) — порог, чтобы глиф лёг в
     * сетку; любой векторный (DIN Next, Bahnschrift) — полутона, иначе контур
     * рвётся. DC_FONT_AA=0/1 задаёт явно. */
    if (!s_aa_env_set) {
        s_antialias = (strstr(s_family, "Pixel") == NULL && strstr(s_path, "pixel") == NULL) ? 1 : 0;
        cache_clear();
    }
    /* Жирный шрифт для заголовков и титульных надписей игры. */
    {
        const char *b = getenv("DC_FONT_BOLD_FILE");
        (void)try_load_bold(b);   /* ME: no bold file - bold is the regular face thickened, as in V1 with Bahnschrift */
    }
    /* Запасной шрифт для знаков, которых нет в основном. */
    {
        const char *fb = getenv("DC_FONT_FALLBACK_FILE");
        char sys[512];
        if (!try_load_fallback(fb) && sys_ui_font(sys, sizeof sys))
            try_load_fallback(sys);   /* ME: the same system font - V1 copied Bahnschrift into fonts\dc_ui_fallback.ttf too */
    }
    fprintf(stderr, "dc_font: загружен %s%s%s, кегль %d px, высота строки %d, сглаживание %s, жирный: %s, запасной: %s\n",
            s_path, s_family[0] ? " — " : "", s_family, s_size, dc_font_line_height(),
            s_antialias ? "вкл" : "выкл", s_ready_b ? s_path_b : "нет файла (берётся обычный)",
            s_ready_fb ? s_path_fb : "нет");
    return 1;
}

int dc_font_ready(void) { return s_ready; }

/* ШРИФТ ПОД ЯЗЫК (2026-09-03). Игрок: «шрифтов будет несколько, английский —
 * вот этим». Основной файл перезагружается на лету: try_load сам освобождает
 * прежний буфер и чистит кеш глифов, остаётся вернуть кегль и заново решить
 * про сглаживание (у пиксельного шрифта оно должно быть выключено, иначе
 * ступеньки замыливаются). Для языка ищем dc_ui_<код>.ttf, и если файла нет —
 * молча остаёмся на текущем. */
/* ПИКСЕЛЬНЫЙ ШРИФТ: размер его собственной сетки в кеглях (2026-09-03).
 * Шрифты, собранные из растрового листа (PE2), состоят из целых пикселей —
 * все координаты контуров кратны одному шагу. Находим этот шаг наибольшим
 * общим делителем координат нескольких глифов: тогда кегль, кратный
 * upm/шаг, кладёт каждый исходный пиксель ровно, без «плавающей» толщины
 * штрихов. 0 — шрифт не пиксельный (обычный векторный). */
static int dc_font_gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a < 0 ? -a : a; }
static int s_pixel_em = -2;
void dc_font_pixel_em_reset(void) { s_pixel_em = -2; }
int dc_font_pixel_em(void)
{
    int cached = s_pixel_em;
    if (cached != -2) return cached;
    cached = 0;
    if (s_ready) {
        int step = 0;
        static const int probe[] = { 'A', 'E', 'M', 'o', '0' };
        for (size_t i = 0; i < sizeof(probe)/sizeof(probe[0]); i++) {
            stbtt_vertex *v = NULL;
            int gi = stbtt_FindGlyphIndex(&s_info, probe[i]);
            if (!gi) continue;
            int n = stbtt_GetGlyphShape(&s_info, gi, &v);
            for (int k = 0; k < n; k++) {
                step = dc_font_gcd(step, v[k].x);
                step = dc_font_gcd(step, v[k].y);
            }
            if (v) stbtt_FreeShape(&s_info, v);
        }
        int upm = (int)(1.0f / stbtt_ScaleForMappingEmToPixels(&s_info, 1.0f) + 0.5f);
        if (step >= 8 && upm > 0 && upm % step == 0 && upm / step >= 6 && upm / step <= 64)
            cached = upm / step;          /* сколько исходных пикселей в em */
    }
    s_pixel_em = cached;
    if (cached) fprintf(stderr, "dc_font: пиксельная сетка — %d px на em\n", cached);
    return cached;
}
int dc_font_use_file(const char *path)
{
    if (!path || !*path) return 0;
    int saved = s_size;
    if (!try_load(path)) return 0;
    { extern void dc_font_pixel_em_reset(void); dc_font_pixel_em_reset(); }
    s_size = 0;                       /* заставить пересчитать масштаб */
    dc_font_set_size(saved);
    if (!s_aa_env_set) {
        s_antialias = (strstr(s_family, "Pixel") == NULL && strstr(s_path, "pixel") == NULL) ? 1 : 0;
        cache_clear();
    }
    fprintf(stderr, "dc_font: основной шрифт -> %s%s%s (кегль %d, сглаживание %s)\n",
            s_path, s_family[0] ? " — " : "", s_family, s_size, s_antialias ? "вкл" : "выкл");
    return 1;
}
static int try_load_bold(const char *path);      /* ниже по файлу */
static int try_load_cjk(const char *path, int clear);   /* ниже по файлу */
/* ME (17.09.2026): the menu and main font names map onto the system font
 * (sys_ui_font). There are no font files next to the game - and no bold
 * files, so bold stays the thickened regular face. */
static int font_find(const char *name, char *out, size_t n)
{
    if (strcmp(name, "dc_ui_menu.ttf") != 0 && strcmp(name, "dc_ui.ttf") != 0) return 0;
    return sys_ui_font(out, n);
}
int dc_font_apply_language(int lang)
{
    char buf[512];
    /* ОДИН ШРИФТ МЕНЮ ДЛЯ ОБОИХ ЯЗЫКОВ (2026-09-05, игрок: «примени нормальный
     * фонт»): в английском меню стоял dc_ui_en.ttf — пиксельный шрифт из PE2,
     * порогованный, без полутонов. Если рядом лежит assets/fonts/dc_ui_menu.ttf
     * (положен Bahnschrift — гротеск в духе DIN, как в меню ремейков; латиница
     * и кириллица в одном файле), он берётся для интерфейса независимо от
     * языка. DC_MENU_FONT=<файл> — свой; DC_MENU_FONT=0 — прежний выбор по языку. */
    { const char *mf = getenv("DC_MENU_FONT");
      int picked = 0;
      if (mf && *mf && !(mf[0] == '0' && mf[1] == 0)) picked = font_find(mf, buf, sizeof buf) || (snprintf(buf, sizeof buf, "%s", mf), 1);
      else if (!(mf && mf[0] == '0')) picked = font_find("dc_ui_menu.ttf", buf, sizeof buf);
      if (!picked && !font_find((lang == 0) ? "dc_ui_en.ttf" : "dc_ui.ttf", buf, sizeof buf)) return 0; }
    if (strcmp(buf, s_path) != 0 && !dc_font_use_file(buf)) return 0;
    /* Жирное начертание того же языка. Файла нет — отключаем жирный шрифт,
     * тогда заголовки и титульные надписи игры рисуются утолщением обычного.
     * Без этого они оставались бы прежним шрифтом: заголовки берут ОТДЕЛЬНЫЙ
     * файл, и подмена только основного их не касается. */
    {
        char bb[512];
        int got = 0;
        /* 10.09: отдельный жирный файл (Shippori Mincho Bold) — только у русского,
         * как было; остальные языки — как английский (утолщение основного).
         * Иначе японские и китайские заголовки собирались бы вперемешку из
         * антиквы Shippori и системного гротеска — знак за знаком. */
        if (font_find((lang == 1) ? "dc_ui_bold.ttf" : "dc_ui_en_bold.ttf", bb, sizeof bb))
            got = (strcmp(bb, s_path_b) == 0) ? 1 : try_load_bold(bb);
        if (!got) { s_ready_b = 0; s_path_b[0] = 0; }
        int saved = s_size; s_size = 0; dc_font_set_size(saved);
        cache_clear();
        fprintf(stderr, "dc_font: жирный -> %s\n", s_ready_b ? s_path_b : "нет файла (утолщение обычного)");
    }
    /* Иероглифы — системный шрифт Windows под язык (см. s_info_cjk). 19.10: на языках без иероглифов - при первом знаке. */
    {
        static int eager = -1;
        if (eager < 0) { const char *e = getenv("DC_FONT_CJK_EAGER"); eager = (e && *e == '1') ? 1 : 0; }
        s_cjk_lang = lang;
        const int cjk_ui = (lang == 6 || lang == 7);
        const char *want = (lang == 6) ? "YuGoth" : "msyh";        /* сменился набор (японский <-> прочие) - заново */
        if (eager || cjk_ui) { s_cjk_pending = 0; cjk_load_for_lang(lang, 1); }
        else if (s_ready_cjk && strstr(s_path_cjk, want)) s_cjk_pending = 0;   /* уже загружен свой */
        else {
            s_cjk_pending = 1;
            fprintf(stderr, "dc_font: иероглифы -> при первом знаке (DC_FONT_CJK_EAGER=1 - сразу)\n");
        }
    }
    return 1;
}
/* загрузка системного шрифта иероглифов для языка: DC_FONT_CJK или список файлов %WINDIR%\Fonts (как было) */
static int cjk_load_for_lang(int lang, int clear)
{
    static const char *ja[] = { "YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc", NULL };
    static const char *zh[] = { "msyh.ttc", "simsun.ttc", "YuGothM.ttc", NULL };
    const char **names = (lang == 6) ? ja : zh;
    const char *env = getenv("DC_FONT_CJK");
    const char *win = getenv("WINDIR");
    char cj[512];
    int got = 0;
    if (env && *env) got = (strcmp(env, s_path_cjk) == 0) || try_load_cjk(env, clear);
    for (int i = 0; !got && names[i]; i++) {
        snprintf(cj, sizeof cj, "%s/Fonts/%s", (win && *win) ? win : "C:/Windows", names[i]);
        got = (strcmp(cj, s_path_cjk) == 0) || try_load_cjk(cj, clear);
    }
    fprintf(stderr, "dc_font: иероглифы -> %s\n", got ? s_path_cjk : "нет системного шрифта");
    return got;
}

const char *dc_font_family(void) { return s_family; }

void dc_font_set_size(int px)
{
    if (px < 6) px = 6;
    if (px > 128) px = 128;
    if (s_ready && px == s_size && s_scale > 0.0f) return;
    s_size = px;
    if (!s_ready) return;
    /* Пиксельный шрифт нарисован по сетке em: масштабируем em -> кегль, тогда
     * при кегле, кратном базовому, каждый пиксель исходника ложится ровно. */
    s_scale = stbtt_ScaleForMappingEmToPixels(&s_info, (float)px);
    if (s_ready_b) s_scale_b = stbtt_ScaleForMappingEmToPixels(&s_info_b, (float)px);
    if (s_ready_fb) s_scale_fb = stbtt_ScaleForMappingEmToPixels(&s_info_fb, (float)px);
    if (s_ready_cjk) s_scale_cjk = stbtt_ScaleForMappingEmToPixels(&s_info_cjk, (float)px);
    cache_clear();
}

static int try_load_fallback(const char *path)
{
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (200 << 20)) { fclose(f); return 0; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return 0; }
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    stbtt_fontinfo fi;                   /* 18.10: целым или никак (try_load) */
    if (off < 0 || !stbtt_InitFont(&fi, buf, off)) { free(buf); return 0; }
    if (s_ttf_fb) free(s_ttf_fb);
    s_info_fb = fi;
    s_ttf_fb = buf;
    s_ready_fb = 1;
    snprintf(s_path_fb, sizeof s_path_fb, "%s", path);
    s_scale_fb = stbtt_ScaleForMappingEmToPixels(&s_info_fb, (float)s_size);
    cache_clear();
    return 1;
}

/* Системный шрифт с иероглифами. Файлы Windows — сборники .ttc: берём первый
 * шрифт сборника (stbtt_GetFontOffsetForIndex, как и у остальных файлов). */
static int try_load_cjk(const char *path, int clear)
{
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64 << 20)) { fclose(f); return 0; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return 0; }
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    stbtt_fontinfo fi;                   /* 18.10: целым или никак (try_load) */
    if (off < 0 || !stbtt_InitFont(&fi, buf, off)) { free(buf); return 0; }
    if (s_ttf_cjk) free(s_ttf_cjk);
    s_info_cjk = fi;
    s_ttf_cjk = buf;
    s_ready_cjk = 1;
    snprintf(s_path_cjk, sizeof s_path_cjk, "%s", path);
    s_scale_cjk = stbtt_ScaleForMappingEmToPixels(&s_info_cjk, (float)s_size);
    if (clear) cache_clear();   /* 19.10: при загрузке по первому знаку - без сброса: прежние глифы взяты из своих шрифтов */
    return 1;
}

static int try_load_bold(const char *path)
{
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (200 << 20)) { fclose(f); return 0; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return 0; }
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    stbtt_fontinfo fi;                   /* 18.10: целым или никак (try_load) */
    if (off < 0 || !stbtt_InitFont(&fi, buf, off)) { free(buf); return 0; }
    if (s_ttf_b) free(s_ttf_b);
    s_info_b = fi;
    s_ttf_b = buf;
    s_ready_b = 1;
    snprintf(s_path_b, sizeof s_path_b, "%s", path);
    s_scale_b = stbtt_ScaleForMappingEmToPixels(&s_info_b, (float)s_size);
    cache_clear();
    return 1;
}
int dc_font_has_bold(void) { return s_ready_b; }

int dc_font_line_height(void)
{
    if (!s_ready) return s_size;
    /* Без linegap: у Fusion Pixel он добавляет к кеглю 24 ещё восемь пикселей,
     * и меню разъезжается заметно шире родного. DC_FONT_LINEGAP=1 возвращает
     * типографскую высоту строки. */
    int units = s_ascent - s_descent + (s_linegap_on ? s_linegap : 0);
    int h = (int)(units * s_scale + 0.5f);
    return h > 0 ? h : s_size;
}

int dc_font_text_width(const char *utf8)
{
    if (!s_ready || !utf8) return 0;
    int bold = s_bold ? 1 : 0;
    int x = 0, cp;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get(cp);
        if (g) x += g->adv + bold;
    }
    return x;
}

/* ---- вывод ---------------------------------------------------------------- */

static void blit_glyph(uint32_t *dst, int dw, int dh,
                       Glyph *g, int px, int py, uint32_t rgba)
{
    if (!g || !g->bmp) return;
    const unsigned a0 = (rgba >> 24) & 0xFF;
    for (int y = 0; y < g->h; y++) {
        int ty = py + g->yoff + y;
        if (ty < 0 || ty >= dh) continue;
        const unsigned char *row = g->bmp + (size_t)y * g->w;
        uint32_t *out = dst + (size_t)ty * dw;
        for (int x = 0; x < g->w; x++) {
            unsigned cov = row[x];
            if (!cov) continue;
            int tx = px + g->xoff + x;
            if (tx < 0 || tx >= dw) continue;
            if (cov == 255 && a0 == 255) { out[tx] = rgba; continue; }
            /* смешивание по покрытию (нужно только для векторного шрифта) */
            unsigned a = (a0 * cov) / 255u;
            uint32_t d = out[tx];
            uint32_t r = (((rgba)       & 0xFF) * a + ((d)       & 0xFF) * (255 - a)) / 255;
            uint32_t gg= (((rgba >> 8)  & 0xFF) * a + ((d >> 8)  & 0xFF) * (255 - a)) / 255;
            uint32_t b = (((rgba >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            uint32_t da = ((d >> 24) & 0xFF);
            uint32_t na = a + da * (255 - a) / 255;
            out[tx] = (na << 24) | (b << 16) | (gg << 8) | r;
        }
    }
}

/* ui 1 — координаты и кегль физические (×S), глифы из кеша слоя */
static void ui_touch_if(const uint32_t *dst, int x0, int y0, int x1, int y1);   /* 17.09 вечер: прямоугольник слоя, ниже */
static void draw_run_ex(uint32_t *dst, int dw, int dh, int x, int y,
                        const char *utf8, uint32_t rgba, int style, int ui)
{
    int mul = ui ? ui_ss() : 1;
    int baseline = y + (int)(s_ascent * s_scale * (float)mul + 0.5f);
    /* утолщение штриха: явное (DC_FONT_BOLD=1) или для жирного стиля без
     * отдельного жирного файла; в слое — на S физических пикселей */
    int bold = (s_bold || (style && !s_ready_b)) ? mul : 0;
    int pen = x, cp;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get_ex(cp, style, ui);
        if (!g) continue;
        blit_glyph(dst, dw, dh, g, pen, baseline, rgba);
        if (bold) blit_glyph(dst, dw, dh, g, pen + bold, baseline, rgba);
        if (g->bmp) ui_touch_if(dst, pen + g->xoff, baseline + g->yoff, pen + g->xoff + g->w + bold, baseline + g->yoff + g->h);
        pen += g->adv + bold;
    }
}
/* ширина строки в ФИЗИЧЕСКИХ пикселях слоя */
static int text_width_ui(const char *utf8, int style)
{
    if (!s_ready || !utf8) return 0;
    int bold = (s_bold || (style && !s_ready_b)) ? ui_ss() : 0;
    int x = 0, cp;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get_ex(cp, style, 1);
        if (g) x += g->adv + bold;
    }
    return x;
}

static void font_draw_ex(uint32_t *dst, int dw, int dh, int x, int y,
                         const char *utf8, uint32_t rgba, int outline, int style, int ui)
{
    if (!s_ready || !dst || !utf8) return;
    if (outline) {
        const uint32_t black = 0xFF000000u;
        static const int dx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
        static const int dy[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
        int t = s_outline_px ? s_outline_px : ((s_size >= 20) ? 2 : 1);
        if (ui) t *= ui_ss();
        for (int k = 0; k < 8; k++)
            draw_run_ex(dst, dw, dh, x + dx[k] * t, y + dy[k] * t, utf8, black, style, ui);
    }
    draw_run_ex(dst, dw, dh, x, y, utf8, rgba, style, ui);
}

/* ---- маска для подмены надписей игры в VRAM (2026-09-02) ------------------
 * Титульные NEW GAME / LOAD GAME / OPTION / PUSH START BUTTON у игры — не
 * глифы, а готовые картинки 4bpp; gpu.c перерисовывает их нашим шрифтом и
 * ему нужна просто бинарная маска штриха в пикселях игры. Кегль здесь — уже
 * пиксели игры (12 = как в атласе шрифта), а не пиксели канваса 640×480. */
int dc_font_render_cov(const char *utf8, int px, int style, int bold,
                       unsigned char *cov, int w, int h, int x0, int baseline)
{
    if (!cov || w <= 0 || h <= 0) return 0;
    memset(cov, 0, (size_t)w * (size_t)h);
    if (!s_ready || !utf8) return 0;
    int saved = s_size;
    dc_font_set_size(px);
    int pen = x0, right = x0, cp;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get_s(cp, style);
        if (!g) continue;
        if (g->bmp) {
            for (int dx = 0; dx <= bold; dx++)
                for (int y = 0; y < g->h; y++) {
                    int ty = baseline + g->yoff + y;
                    if (ty < 0 || ty >= h) continue;
                    for (int x = 0; x < g->w; x++) {
                        unsigned c = g->bmp[y * g->w + x];
                        if (!c) continue;
                        int tx = pen + g->xoff + x + dx;
                        if (tx < 0 || tx >= w) continue;
                        if (c > cov[ty * w + tx]) cov[ty * w + tx] = (unsigned char)c;
                    }
                }
        }
        pen += g->adv + bold;
        right = pen;
    }
    dc_font_set_size(saved);
    return right;
}

int dc_font_text_width_px(const char *utf8, int px, int style, int bold)
{
    if (!s_ready || !utf8) return 0;
    int saved = s_size;
    dc_font_set_size(px);
    int x = 0, cp;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get_s(cp, style);
        if (g) x += g->adv + bold;
    }
    dc_font_set_size(saved);
    return x;
}

int dc_font_cap_height(int px, int style)
{
    if (!s_ready) return 0;
    int saved = s_size;
    dc_font_set_size(px);
    Glyph *g = glyph_get_s('H', style);
    int h = g ? g->h : 0;
    dc_font_set_size(saved);
    return h;
}

/* Кегль (в пикселях игры), при котором высота заглавной H равна cap_px или
 * чуть меньше: так подбирается размер под ячейку 16×16 атласа игры и под
 * полоски титула (у родного шрифта заглавные ~10 px). */
int dc_font_fit_px(int cap_px, int style)
{
    int best = 12;
    for (int px = 8; px <= 24; px++) {
        int h = dc_font_cap_height(px, style);
        if (h <= 0) continue;
        if (h <= cap_px) best = px; else break;
    }
    return best;
}

/* ---- атлас шрифта игры из TTF (2026-09-02) --------------------------------
 * Страница 4bpp 256×256 (32768 байт, младший полубайт — левый пиксель), ячейка
 * 16×16, 16 в ряд; cell_cp[i] — код символа ячейки i (0 — не трогать).
 * Индексы палитры (768,504): 0 прозрачный, 1 — обводка (у родного шрифта
 * такая же), тело 2..body_max по покрытию (сглаживание); widths[i] — шаг
 * ячейки для таблицы ширин игры (0xFF — не трогать). Обводка считается внутри
 * ячейки. Возвращает число нарисованных ячеек. */
/* Стиль текста игры (просьба игрока 2026-09-02, «текст выглядит грязно»):
 * штрих БЕЗ полутонов — чисто белый, обводка чёрная (индекс 1 — самый тёмный
 * непрозрачный), и вертикальный градиент яркости 1.0 у верха заглавных → 0.7
 * у базовой линии (выносные ниже базовой остаются 0.7, надстрочные знаки —
 * 1.0). Индекс палитры подбирается по РЕАЛЬНЫМ серым палитры (grey16 — 16
 * значений 0..255), потому что палитра (768,504) не линейна. */
#define DCF_BODY_THRESHOLD 110     /* покрытие, с которого пиксель считается штрихом */

int dc_font_grad_index(const unsigned char *grey16, int y, int ytop, int ybot)
{
    float t = 0.0f;
    if (ybot > ytop) {
        t = (float)(y - ytop) / (float)(ybot - ytop);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    float b = 1.0f - 0.3f * t;                        /* 1.0 сверху -> 0.7 снизу */
    int target = (int)(b * (float)grey16[15] + 0.5f);
    int best = 15, bd = 1 << 30;
    for (int i = 1; i < 16; i++) {                    /* 0 — прозрачный, не берём */
        int d = (int)grey16[i] - target; if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int dc_font_build_game_atlas(const int *cell_cp, int ncells, int px, int baseline, int cap_px,
                             const unsigned char *grey16, unsigned char *out, unsigned char *widths)
{
    if (!out || !widths) return 0;
    memset(out, 0, 32768);
    memset(widths, 0xFF, 256);
    if (!s_ready || !cell_cp || !grey16) return 0;
    int saved = s_size;
    dc_font_set_size(px);
    unsigned char cell[16 * 16], idx[16 * 16];
    int drawn = 0;
    const int ytop = baseline - cap_px, ybot = baseline;   /* градиент по высоте заглавных */
    /* У ПИКСЕЛЬНОГО ШРИФТА ТЕЛО РОВНОЕ (2026-09-03, сверка со скриншотами
     * Parasite Eve 2): там буквы залиты одним белым, а не вертикальным
     * градиентом — с градиентом заглавные казались «из другого набора»,
     * потому что у высокой буквы перепад яркости куда заметнее, чем у
     * строчной. Обводка остаётся. */
    const int flat_body = (dc_font_pixel_em() > 0);
    for (int i = 0; i < ncells && i < 256; i++) {
        int cp = cell_cp[i];
        if (cp <= 0) continue;
        Glyph *g = glyph_get_s(cp, 0);
        if (!g) continue;
        if (cp != ' ' && (!g->bmp || g->w <= 0 || g->h <= 0)) continue;   /* нет глифа — оставить родной */
        memset(cell, 0, sizeof cell);
        if (g->bmp)
            for (int y = 0; y < g->h; y++) {
                int ty = baseline + g->yoff + y;
                if (ty < 0 || ty >= 16) continue;
                for (int x = 0; x < g->w; x++) {
                    int tx = 1 + g->xoff + x;
                    if (tx < 0 || tx >= 16) continue;
                    if (g->bmp[y * g->w + x] >= DCF_BODY_THRESHOLD) cell[ty * 16 + tx] = 255;
                }
            }
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int v = 0;
                if (cell[y * 16 + x]) {
                    v = flat_body ? 15 : dc_font_grad_index(grey16, y, ytop, ybot);
                } else {
                    for (int dy = -1; dy <= 1 && !v; dy++)
                        for (int dx = -1; dx <= 1 && !v; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || xx >= 16 || yy < 0 || yy >= 16) continue;
                            if (cell[yy * 16 + xx]) v = 1;                 /* обводка */
                        }
                }
                idx[y * 16 + x] = (unsigned char)v;
            }
        int cx = i % 16, cy = i / 16;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x += 2) {
                size_t o = ((size_t)(cy * 16 + y) * 256 + (size_t)(cx * 16 + x)) / 2;
                out[o] = (unsigned char)(idx[y * 16 + x] | (idx[y * 16 + x + 1] << 4));
            }
        /* Ширина ячейки для игры. У векторного шрифта advance не включает
         * боковой отступ, поэтому к нему прибавлялся пиксель. У пиксельного
         * шрифта отступ уже заложен в advance при сборке, и это «+1» давало
         * лишний пробел между каждой парой букв — текст шёл вразрядку
         * (игрок 2026-09-03, «Equip and check your weapons»). */
        int adv = g->adv + (dc_font_pixel_em() > 0 ? 0 : 1);
        if (adv < 2) adv = 2;
        if (adv > 15) adv = 15;
        widths[i] = (unsigned char)adv;
        drawn++;
    }
    dc_font_set_size(saved);
    return drawn;
}

/* Hi-res атлас шрифта игры (RGBA8, (256·S)²): те же ячейки и кегль, что у
 * 4bpp-атласа выше, но с плотностью S текселей на родной тексель — растеризатор
 * подставляет его вместо VRAM (DC_FONT_HIRES). Каналы текселя: a — покрытие
 * (тело + обводка), r — доля тела в покрытии, g — индекс серого тела /15
 * (вертикальный градиент 1.0 → 0.7, как в 4bpp-атласе), b = 255 — ячейка НЕ
 * наша (шейдер берёт VRAM). Цвета берёт шейдер из CLUT примитива, поэтому
 * палитровые варианты текста (подсветка, мигание) сохраняются. */
/* Сглаживание hi-res текста игры: по умолчанию ВКЛ — край тела смешивается с
 * обводкой по покрытию. Без него штрих TTF без хинтинга на сетке 2× рвётся на
 * ступеньки разной ширины («шрифт хуже оригинального», игрок 2026-09-02);
 * с ним при S=2 буквы гладкие, но по-прежнему вдвое крупнее пикселя канваса,
 * то есть «низкое разрешение» сохраняется. DC_FONT_HIRES_AA=0 — порог. */
static int hires_aa(void) {
    static int aa = -1;
    if (aa < 0) { const char *e = getenv("DC_FONT_HIRES_AA"); aa = (e && *e == '0') ? 0 : 1; }
    return aa;
}
static void hires_threshold(unsigned char *cov, size_t n) {
    if (hires_aa()) return;
    for (size_t i = 0; i < n; i++) cov[i] = (cov[i] >= 128) ? 255 : 0;
}

int dc_font_build_hires_atlas(const int *cell_cp, int ncells, int px, int baseline, int cap_px,
                              int S, const unsigned char *grey16, unsigned char *out,
                              unsigned char *drawn)
{
    if (drawn) memset(drawn, 0, 256);
    if (!out || S < 1 || S > 8) return 0;
    const int W = 256 * S, CS = 16 * S;
    for (size_t i = 0; i < (size_t)W * W; i++) {
        out[i * 4 + 0] = 0; out[i * 4 + 1] = 0; out[i * 4 + 2] = 255; out[i * 4 + 3] = 0;
    }
    if (!s_ready || !cell_cp || !grey16) return 0;
    unsigned char *cov = (unsigned char *)malloc((size_t)CS * CS);
    unsigned char *dil = (unsigned char *)malloc((size_t)CS * CS);
    unsigned char *tmp = (unsigned char *)malloc((size_t)CS * CS);
    if (!cov || !dil || !tmp) { free(cov); free(dil); free(tmp); return 0; }
    int saved = s_size;
    dc_font_set_size(px * S);
    const float ytop = (float)(baseline - cap_px) * (float)S, ybot = (float)baseline * (float)S;
    int ndrawn = 0;
    for (int i = 0; i < ncells && i < 256; i++) {
        int cp = cell_cp[i];
        if (cp <= 0) continue;
        Glyph *g = glyph_get_s(cp, 0);
        if (!g) continue;
        if (cp != ' ' && (!g->bmp || g->w <= 0 || g->h <= 0)) continue;   /* нет глифа — оставить родной */
        memset(cov, 0, (size_t)CS * CS);
        if (g->bmp)
            for (int y = 0; y < g->h; y++) {
                int ty = baseline * S + g->yoff + y;
                if (ty < 0 || ty >= CS) continue;
                for (int x = 0; x < g->w; x++) {
                    int tx = S + g->xoff + x;                 /* 1 родной тексель отступа, как в 4bpp */
                    if (tx < 0 || tx >= CS) continue;
                    cov[ty * CS + tx] = g->bmp[y * g->w + x];
                }
            }
        hires_threshold(cov, (size_t)CS * CS);
        /* обводка = расширение тела на S текселей (один родной), раздельно по осям */
        for (int y = 0; y < CS; y++)
            for (int x = 0; x < CS; x++) {
                int m = 0;
                for (int dx = -S; dx <= S; dx++) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= CS) continue;
                    if (cov[y * CS + xx] > m) m = cov[y * CS + xx];
                }
                tmp[y * CS + x] = (unsigned char)m;
            }
        for (int y = 0; y < CS; y++)
            for (int x = 0; x < CS; x++) {
                int m = 0;
                for (int dy = -S; dy <= S; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= CS) continue;
                    if (tmp[yy * CS + x] > m) m = tmp[yy * CS + x];
                }
                dil[y * CS + x] = (unsigned char)m;
            }
        int cx = i % 16, cy = i / 16;
        for (int y = 0; y < CS; y++) {
            float t = (ybot > ytop) ? ((float)y + 0.5f - ytop) / (ybot - ytop) : 1.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float bright = 1.0f - 0.3f * t;                      /* 1.0 сверху -> 0.7 снизу */
            int target = (int)(bright * (float)grey16[15] + 0.5f);
            int best = 15, bd = 1 << 30;
            for (int k = 1; k < 16; k++) {
                int d = (int)grey16[k] - target; if (d < 0) d = -d;
                if (d < bd) { bd = d; best = k; }
            }
            for (int x = 0; x < CS; x++) {
                int b = cov[y * CS + x], o = dil[y * CS + x];
                size_t p = ((size_t)(cy * CS + y) * (size_t)W + (size_t)(cx * CS + x)) * 4;
                out[p + 0] = (unsigned char)(o ? (b * 255 + o / 2) / o : 0);   /* доля тела */
                out[p + 1] = (unsigned char)(best * 17);                          /* индекс серого */
                out[p + 2] = 0;                                                    /* ячейка наша */
                out[p + 3] = (unsigned char)o;                                     /* покрытие */
            }
        }
        if (drawn) drawn[i] = 1;
        ndrawn++;
    }
    dc_font_set_size(saved);
    free(cov); free(dil); free(tmp);
    return ndrawn;
}

/* hi-res RGBA-полоска титульной надписи (кодировка та же, что у hi-res атласа
 * выше): строка utf8 кеглем px·S в прямоугольник (w·S)×(h·S), базовая линия
 * baseline·S, тело по градиенту 1.0 → 0.7 на высоте заглавных cap_px, обводка —
 * расширение на S текселей. Игрок 2026-09-02: «буквы PUSH START BUTTON
 * выглядят криво» — полоски игра растягивает впятеро, а мы рисовали их в
 * родные 16 текселей. Возвращает 1, если штрих есть. */
int dc_font_build_hires_strip(const char *utf8, int px, int style, int bold, int w, int h,
                              int baseline, int cap_px, int S, const unsigned char *grey16,
                              unsigned char *out)
{
    if (!out || S < 1 || S > 8 || w <= 0 || h <= 0) return 0;
    const int W = w * S, H = h * S;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        out[i * 4 + 0] = 0; out[i * 4 + 1] = 0; out[i * 4 + 2] = 255; out[i * 4 + 3] = 0;
    }
    if (!s_ready || !utf8 || !grey16) return 0;
    unsigned char *cov = (unsigned char *)calloc((size_t)W * H, 1);
    unsigned char *dil = (unsigned char *)malloc((size_t)W * H);
    unsigned char *tmp = (unsigned char *)malloc((size_t)W * H);
    if (!cov || !dil || !tmp) { free(cov); free(dil); free(tmp); return 0; }
    int saved = s_size;
    dc_font_set_size(px * S);
    int pen = S, cp, any = 0;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get_s(cp, style);
        if (!g) continue;
        if (g->bmp)
            for (int dx = 0; dx <= bold * S; dx++)
                for (int y = 0; y < g->h; y++) {
                    int ty = baseline * S + g->yoff + y;
                    if (ty < 0 || ty >= H) continue;
                    for (int x = 0; x < g->w; x++) {
                        unsigned c = g->bmp[y * g->w + x];
                        if (!c) continue;
                        int tx = pen + g->xoff + x + dx;
                        if (tx < 0 || tx >= W) continue;
                        if (c > cov[ty * W + tx]) cov[ty * W + tx] = (unsigned char)c;
                        any = 1;
                    }
                }
        pen += g->adv + bold * S;
    }
    dc_font_set_size(saved);
    hires_threshold(cov, (size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int m = 0;
            for (int dx = -S; dx <= S; dx++) {
                int xx = x + dx;
                if (xx < 0 || xx >= W) continue;
                if (cov[y * W + xx] > m) m = cov[y * W + xx];
            }
            tmp[y * W + x] = (unsigned char)m;
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int m = 0;
            for (int dy = -S; dy <= S; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= H) continue;
                if (tmp[yy * W + x] > m) m = tmp[yy * W + x];
            }
            dil[y * W + x] = (unsigned char)m;
        }
    const float ytop = (float)(baseline - cap_px) * (float)S, ybot = (float)baseline * (float)S;
    for (int y = 0; y < H; y++) {
        float t = (ybot > ytop) ? ((float)y + 0.5f - ytop) / (ybot - ytop) : 1.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float bright = 1.0f - 0.3f * t;
        int target = (int)(bright * (float)grey16[15] + 0.5f);
        int best = 15, bd = 1 << 30;
        for (int k = 1; k < 16; k++) {
            int d = (int)grey16[k] - target; if (d < 0) d = -d;
            if (d < bd) { bd = d; best = k; }
        }
        for (int x = 0; x < W; x++) {
            int b = cov[y * W + x], o = dil[y * W + x];
            size_t q = ((size_t)y * (size_t)W + (size_t)x) * 4;
            out[q + 0] = (unsigned char)(o ? (b * 255 + o / 2) / o : 0);
            out[q + 1] = (unsigned char)(best * 17);
            out[q + 2] = 0;
            out[q + 3] = (unsigned char)o;
        }
    }
    free(cov); free(dil); free(tmp);
    return any;
}

/* ---- слой интерфейса ------------------------------------------------------ */

#define UI_W 640
#define UI_H 480

/* Физический буфер слоя: 640·S × 480·S (S = dc_ui_scale), выделяется при
 * первом обращении. Все функции dc_ui_* принимают ЛОГИЧЕСКИЕ координаты
 * 640×480 и умножают их на S сами. */
static uint32_t *s_ui;
static int       s_ui_w, s_ui_h;
static int       s_ui_dirty;
static uint32_t *ui_buf(void)
{
    if (!s_ui) {
        s_ui_w = UI_W * ui_ss(); s_ui_h = UI_H * ui_ss();
        s_ui = (uint32_t *)calloc((size_t)s_ui_w * (size_t)s_ui_h, sizeof(uint32_t));
    }
    return s_ui;
}

uint32_t *dc_ui_buffer(int *w, int *h)
{
    uint32_t *b = ui_buf();
    if (w) *w = s_ui_w;
    if (h) *h = s_ui_h;
    return b;
}

/* 17.09.2026 вечер, пункт 2: ПРЯМОУГОЛЬНИК НАРИСОВАННОГО. Слой 1920×1440 (S=3) чистился memset-ом целиком и заливался
 * в видеокарту целиком на КАЖДОЙ развёртке, пока в нём хоть что-то нарисовано (подсказка «Пропустить» 3 с после
 * нажатия в сцене): 11 МБ + 11 МБ. Теперь все писатели слоя (глифы draw_run_ex, dc_ui_rect, skp_blit через
 * dc_ui_mark) расширяют прямоугольник кадра; dc_ui_begin чистит только его, main.cpp заливает объединение с прошлой
 * заливкой. DC_UI_DIRTYRECT=0 — целиком, как было; =2 — ещё и сверка: вне прямоугольника буфер пуст (uidirty:). */
static int s_ui_bx0 = 1 << 30, s_ui_by0 = 1 << 30, s_ui_bx1 = -1, s_ui_by1 = -1;
static int ui_dirtyrect_mode(void)
{
    static int m = -1;
    if (m < 0) { const char *e = getenv("DC_UI_DIRTYRECT"); m = e ? atoi(e) : 1; if (m < 0 || m > 2) m = 1; }
    return m;
}
void dc_ui_mark(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_ui_w) x1 = s_ui_w;
    if (y1 > s_ui_h) y1 = s_ui_h;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < s_ui_bx0) s_ui_bx0 = x0;
    if (y0 < s_ui_by0) s_ui_by0 = y0;
    if (x1 > s_ui_bx1) s_ui_bx1 = x1;
    if (y1 > s_ui_by1) s_ui_by1 = y1;
}
static void ui_touch_if(const uint32_t *dst, int x0, int y0, int x1, int y1)
{
    if (dst && dst == s_ui) dc_ui_mark(x0, y0, x1, y1);
}
/* прямоугольник этого кадра; 0 — пусто. mode: 0 — целиком (как было) */
int dc_ui_dirty_rect(int *x, int *y, int *w, int *h, int *mode)
{
    int m = ui_dirtyrect_mode();
    if (mode) *mode = m;
    if (s_ui_bx1 <= s_ui_bx0 || s_ui_by1 <= s_ui_by0) { *x = *y = *w = *h = 0; return 0; }
    *x = s_ui_bx0; *y = s_ui_by0; *w = s_ui_bx1 - s_ui_bx0; *h = s_ui_by1 - s_ui_by0;
    return 1;
}

void dc_ui_begin(void)
{
    const int m = ui_dirtyrect_mode();
    if (s_ui_dirty && ui_buf()) {
        if (m == 0 || s_ui_bx1 <= s_ui_bx0 || s_ui_by1 <= s_ui_by0) {
            memset(s_ui, 0, (size_t)s_ui_w * (size_t)s_ui_h * sizeof(uint32_t));
        } else {
            const int w = s_ui_bx1 - s_ui_bx0;
            for (int yy = s_ui_by0; yy < s_ui_by1; yy++)
                memset(s_ui + (size_t)yy * (size_t)s_ui_w + (size_t)s_ui_bx0, 0, (size_t)w * sizeof(uint32_t));
        }
    }
    s_ui_dirty = 0;
    s_ui_bx0 = s_ui_by0 = 1 << 30; s_ui_bx1 = s_ui_by1 = -1;
}

int dc_ui_dirty(void) { return s_ui_dirty; }

void dc_ui_text(int x, int y, const char *utf8, uint32_t rgba, int outline)
{
    if (!dc_font_ready() || !ui_buf()) return;
    int ss = ui_ss();
    font_draw_ex(s_ui, s_ui_w, s_ui_h, x * ss, y * ss, utf8, rgba, outline, 0, 1);
    s_ui_dirty = 1;
}

void dc_ui_text_centered(int y, const char *utf8, uint32_t rgba, int outline)
{
    if (!dc_font_ready() || !ui_buf()) return;
    int ss = ui_ss();
    int w = text_width_ui(utf8, 0);
    font_draw_ex(s_ui, s_ui_w, s_ui_h, (s_ui_w - w) / 2, y * ss, utf8, rgba, outline, 0, 1);
    s_ui_dirty = 1;
}

/* Жирным (заголовки, вкладки): отдельный жирный файл, иначе утолщение. */
void dc_ui_text_b(int x, int y, const char *utf8, uint32_t rgba, int outline)
{
    if (!dc_font_ready() || !ui_buf()) return;
    int ss = ui_ss();
    font_draw_ex(s_ui, s_ui_w, s_ui_h, x * ss, y * ss, utf8, rgba, outline, 1, 1);
    s_ui_dirty = 1;
}

int dc_font_text_width_b(const char *utf8)
{
    if (!s_ready || !utf8) return 0;
    int bold = (s_bold || !s_ready_b) ? 1 : 0;
    int x = 0, cp;
    const char *p = utf8;
    while (*p) {
        p = utf8_next(p, &cp);
        if (cp == '\n') break;
        Glyph *g = glyph_get_s(cp, 1);
        if (g) x += g->adv + bold;
    }
    return x;
}

void dc_ui_rect(int x, int y, int w, int h, uint32_t rgba)
{
    if (!ui_buf()) return;
    { int ss = ui_ss(); x *= ss; y *= ss; w *= ss; h *= ss; }   /* логические -> физические */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_ui_w) w = s_ui_w - x;
    if (y + h > s_ui_h) h = s_ui_h - y;
    if (w <= 0 || h <= 0) return;
    dc_ui_mark(x, y, x + w, y + h);
    unsigned a = (rgba >> 24) & 0xFF;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = s_ui + (size_t)(y + yy) * s_ui_w + x;
        if (a == 255) {
            for (int xx = 0; xx < w; xx++) row[xx] = rgba;
        } else {
            for (int xx = 0; xx < w; xx++) {
                uint32_t d = row[xx];
                uint32_t r = (((rgba)       & 0xFF) * a + ((d)       & 0xFF) * (255 - a)) / 255;
                uint32_t g = (((rgba >> 8)  & 0xFF) * a + ((d >> 8)  & 0xFF) * (255 - a)) / 255;
                uint32_t b = (((rgba >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
                uint32_t da = ((d >> 24) & 0xFF);
                uint32_t na = a + da * (255 - a) / 255;
                row[xx] = (na << 24) | (b << 16) | (g << 8) | r;
            }
        }
    }
    s_ui_dirty = 1;
}
