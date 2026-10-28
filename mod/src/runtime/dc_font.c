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

/* АРАБСКИЕ БУКВЫ (27.10.2026, игрок: «добавь новый язык, арабский»): готовые формы букв, которые подставляет ar_vis,
 * есть в системных Segoe UI, Tahoma и Arial, но не в Bahnschrift. Шрифт читается при первой арабской букве на экране
 * (ar_font из glyph_get_ex) на любом языке: в списке языков есть и имя арабского. DC_FONT_AR - свой файл. */
static stbtt_fontinfo  s_info_ar;        /* файл в памяти до конца: на него смотрит s_info_ar.data */
static int   s_ready_ar;                  /* 0 - ещё не искали, 1 - загружен, -1 - нет ни одного */
static float s_scale_ar;
static int   ar_font(void);
static int   ar_cp(int c)                 /* знак арабского письма */
{
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0x08A0 && c <= 0x08FF) ||
           (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

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
        else if (ar_cp(cp) && ar_font() && stbtt_FindGlyphIndex(&s_info_ar, cp) != 0) { fi = &s_info_ar; sc = s_scale_ar * mul; }
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
static int utf8_put(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* ---- арабский: формы букв и порядок справа налево (27.10.2026) -------------
 * stb_truetype рисует знаки по одному слева направо и не знает ни связывания арабских букв, ни письма справа
 * налево. Поэтому внешние функции строк (dc_font_text_width, dc_ui_text и остальные) сначала переписывают строку с
 * арабскими буквами (ar_vis):
 *  - связывание: буква берёт форму по соседям (отдельная, конечная, начальная, средняя) из блока готовых форм
 *    Юникода, лам с алифом - одна лигатура; огласовки отбрасываются - над буквой их stb_truetype не поставит;
 *  - порядок: алгоритм двунаправленного текста Юникода (UAX 9) для одной строки без явных вставок. Строка идёт
 *    справа налево, если в ней есть арабская буква (как direction: rtl у шаблонов экранов); латиница и числа внутри
 *    остаются слева направо, парные скобки - по правилу N0, зеркальные знаки справа налево отражаются.
 * Итог - знаки в порядке вывода слева направо. Переписывать строго ОДИН раз (второй проход развернёт строку
 * обратно): внутренние draw_run_ex, text_width_ui и font_draw_ex получают уже готовую. Строка без арабских букв
 * не меняется. Эталон - Edge с direction: rtl. */
#define AR_MAX 384                        /* знаков в строке; длиннее - выводится как есть */
#define AR_BUF (AR_MAX * 4 + 1)           /* переписанная строка в UTF-8 */

/* формы букв от хамзы до йа по порядку кодов: отдельная, конечная, начальная, средняя (0 - формы нет) */
static const unsigned short s_ar_form[42][4] = {
    { 0xFE80, 0, 0, 0 }, { 0xFE81, 0xFE82, 0, 0 }, { 0xFE83, 0xFE84, 0, 0 }, { 0xFE85, 0xFE86, 0, 0 },
    { 0xFE87, 0xFE88, 0, 0 }, { 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C }, { 0xFE8D, 0xFE8E, 0, 0 }, { 0xFE8F, 0xFE90, 0xFE91, 0xFE92 },
    { 0xFE93, 0xFE94, 0, 0 }, { 0xFE95, 0xFE96, 0xFE97, 0xFE98 }, { 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C }, { 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },
    { 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 }, { 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 }, { 0xFEA9, 0xFEAA, 0, 0 }, { 0xFEAB, 0xFEAC, 0, 0 },
    { 0xFEAD, 0xFEAE, 0, 0 }, { 0xFEAF, 0xFEB0, 0, 0 }, { 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 }, { 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 },
    { 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC }, { 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 }, { 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 }, { 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },
    { 0xFEC9, 0xFECA, 0xFECB, 0xFECC }, { 0xFECD, 0xFECE, 0xFECF, 0xFED0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
    { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
    { 0xFED1, 0xFED2, 0xFED3, 0xFED4 }, { 0xFED5, 0xFED6, 0xFED7, 0xFED8 }, { 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC }, { 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },
    { 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 }, { 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 }, { 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC }, { 0xFEED, 0xFEEE, 0, 0 },
    { 0xFEEF, 0xFEF0, 0xFBE8, 0xFBE9 }, { 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },
};
enum { AJ_U, AJ_R, AJ_D, AJ_C };          /* связь: никакой, только с предыдущей буквой, с обеих сторон, связка */
static int ar_join(int c)
{
    if (c == 0x0640 || c == 0x200D) return AJ_C;        /* растяжка и ZWJ */
    if (c < 0x0621 || c > 0x064A) return AJ_U;
    const unsigned short *f = s_ar_form[c - 0x0621];
    return f[2] ? AJ_D : f[1] ? AJ_R : AJ_U;
}
static int ar_mark(int c)                 /* огласовки и знаки чтения */
{
    return (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
           (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) || c == 0x06E7 || c == 0x06E8 ||
           (c >= 0x06EA && c <= 0x06ED);
}

/* классы UAX 9; явные вставки и изоляторы (LRE ... PDI) - как BN: выбрасываются, вложенных уровней нет */
enum { AB_L, AB_R, AB_AL, AB_EN, AB_ES, AB_ET, AB_AN, AB_CS, AB_NSM, AB_BN, AB_B, AB_S, AB_WS, AB_ON };
static int ar_class(int c)
{
    /* ASCII и Latin-1 - точно: буква - номер класса по порядку выше (a - L, b - R, ...) */
    static const char lat1[] =
        "jjjjjjjjjlklmkjjjjjjjjjjjjjjkkklmnnfffnnnnnehehhddddddddddhnnnnn"
        "naaaaaaaaaaaaaaaaaaaaaaaaaannnnnnaaaaaaaaaaaaaaaaaaaaaaaaaannnnj"
        "jjjjjkjjjjjjjjjjjjjjjjjjjjjjjjjjhnffffnnnnannjnnffddnannndannnnn"
        "aaaaaaaaaaaaaaaaaaaaaaanaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaanaaaaaaaa";
    /* дальше - отрезки (начало, конец, класс) по возрастанию: точно - арабский блок, общая пунктуация и арабские
     * готовые формы, прочее - близко; чего нет в списке - L */
    static const unsigned short rng[][3] = {
        { 0x0300, 0x036F, AB_NSM }, { 0x0483, 0x0489, AB_NSM }, { 0x0590, 0x05FF, AB_R },
        { 0x0600, 0x0605, AB_AN }, { 0x0606, 0x0607, AB_ON }, { 0x0608, 0x0608, AB_AL }, { 0x0609, 0x060A, AB_ET },
        { 0x060B, 0x060B, AB_AL }, { 0x060C, 0x060C, AB_CS }, { 0x060D, 0x060D, AB_AL }, { 0x060E, 0x060F, AB_ON },
        { 0x0610, 0x061A, AB_NSM }, { 0x061B, 0x064A, AB_AL }, { 0x064B, 0x065F, AB_NSM }, { 0x0660, 0x0669, AB_AN },
        { 0x066A, 0x066A, AB_ET }, { 0x066B, 0x066C, AB_AN }, { 0x066D, 0x066F, AB_AL }, { 0x0670, 0x0670, AB_NSM },
        { 0x0671, 0x06D5, AB_AL }, { 0x06D6, 0x06DC, AB_NSM }, { 0x06DD, 0x06DD, AB_AN }, { 0x06DE, 0x06DE, AB_ON },
        { 0x06DF, 0x06E4, AB_NSM }, { 0x06E5, 0x06E6, AB_AL }, { 0x06E7, 0x06E8, AB_NSM }, { 0x06E9, 0x06E9, AB_ON },
        { 0x06EA, 0x06ED, AB_NSM }, { 0x06EE, 0x06EF, AB_AL }, { 0x06F0, 0x06F9, AB_EN }, { 0x06FA, 0x07BF, AB_AL },
        { 0x07C0, 0x085F, AB_R }, { 0x0860, 0x08FF, AB_AL },
        { 0x2000, 0x200A, AB_WS }, { 0x200B, 0x200D, AB_BN }, { 0x200E, 0x200E, AB_L }, { 0x200F, 0x200F, AB_R },
        { 0x2010, 0x2027, AB_ON }, { 0x2028, 0x2028, AB_WS }, { 0x2029, 0x2029, AB_B }, { 0x202A, 0x202E, AB_BN },
        { 0x202F, 0x202F, AB_CS }, { 0x2030, 0x2034, AB_ET }, { 0x2035, 0x2043, AB_ON }, { 0x2044, 0x2044, AB_CS },
        { 0x2045, 0x205E, AB_ON }, { 0x205F, 0x205F, AB_WS }, { 0x2060, 0x206F, AB_BN }, { 0x2070, 0x2070, AB_EN },
        { 0x2074, 0x2079, AB_EN }, { 0x207A, 0x207B, AB_ES }, { 0x207C, 0x207E, AB_ON }, { 0x2080, 0x2089, AB_EN },
        { 0x208A, 0x208B, AB_ES }, { 0x208C, 0x208E, AB_ON }, { 0x20A0, 0x20CF, AB_ET }, { 0x20D0, 0x20FF, AB_NSM },
        { 0x2190, 0x2211, AB_ON }, { 0x2212, 0x2212, AB_ES }, { 0x2213, 0x2213, AB_ET }, { 0x2214, 0x2BFF, AB_ON },
        { 0x3000, 0x3000, AB_WS }, { 0x3001, 0x3004, AB_ON }, { 0x3008, 0x3020, AB_ON },
        { 0xFB1D, 0xFB4F, AB_R }, { 0xFB50, 0xFDFF, AB_AL }, { 0xFE00, 0xFE0F, AB_NSM }, { 0xFE10, 0xFE6F, AB_ON },
        { 0xFE70, 0xFEFE, AB_AL }, { 0xFEFF, 0xFEFF, AB_BN }, { 0xFF01, 0xFF0F, AB_ON }, { 0xFF10, 0xFF19, AB_EN },
        { 0xFF1A, 0xFF20, AB_ON }, { 0xFF3B, 0xFF40, AB_ON }, { 0xFF5B, 0xFF65, AB_ON }, { 0xFFE0, 0xFFFF, AB_ON },
    };
    if (c >= 0 && c < 0x100) return lat1[c] - 'a';
    int lo = 0, hi = (int)(sizeof rng / sizeof rng[0]) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (c < rng[mid][0]) hi = mid - 1;
        else if (c > rng[mid][1]) lo = mid + 1;
        else return rng[mid][2];
    }
    return AB_L;
}
static int ar_rtl(int c)                  /* знак письма справа налево: строку надо переписать */
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFE) || c == 0x200F;
}
static int ar_strong(int t)               /* направление для N0 и N1 (числа - как R); -1 - нейтральный */
{
    return t == AB_L ? AB_L : (t == AB_R || t == AB_EN || t == AB_AN) ? AB_R : -1;
}
static int ar_ni(int t) { return t == AB_ON || t == AB_WS || t == AB_S || t == AB_B; }   /* нейтральный для N1 */
static int ar_mirror(int c)               /* L4: знак, отражённый для письма справа налево */
{
    static const unsigned short mp[][2] = {
        { '(', ')' }, { '<', '>' }, { '[', ']' }, { '{', '}' }, { 0x00AB, 0x00BB }, { 0x2039, 0x203A }, { 0x2264, 0x2265 },
    };
    for (size_t i = 0; i < sizeof mp / sizeof mp[0]; i++) {
        if (c == mp[i][0]) return mp[i][1];
        if (c == mp[i][1]) return mp[i][0];
    }
    return c;
}
static void ar_rev(int *cp, short *src, unsigned char *lvl, int a, int b)   /* развернуть отрезок [a, b) */
{
    for (b--; a < b; a++, b--) {
        const int t = cp[a]; cp[a] = cp[b]; cp[b] = t;
        const short s = src[a]; src[a] = src[b]; src[b] = s;
        const unsigned char l = lvl[a]; lvl[a] = lvl[b]; lvl[b] = l;
    }
}

/* строка в логическом порядке (in, n знаков) -> знаки вывода слева направо (out); idx - номер исходного знака
 * у каждого (NULL - не нужен). Возвращает число знаков вывода. */
static int ar_layout(const int *in, int n, int *out, short *idx)
{
    int c0[AR_MAX], cp[AR_MAX];
    short s0[AR_MAX], src[AR_MAX], po[AR_MAX / 2], pc[AR_MAX / 2];
    unsigned char cls[AR_MAX], org[AR_MAX], lvl[AR_MAX];
    int n0 = 0, m = 0, rtl = 0;
    if (n > AR_MAX) n = AR_MAX;
    /* X9: без огласовок и невидимых знаков; ZWNJ и ZWJ остаются до связывания */
    for (int i = 0; i < n; i++) {
        const int c = in[i];
        if (ar_mark(c) || (c != 0x200C && c != 0x200D && ar_class(c) == AB_BN)) continue;
        c0[n0] = c; s0[n0] = (short)i; n0++;
    }
    /* связывание: форма буквы по соседям */
    for (int i = 0; i < n0; i++) {
        const int c = c0[i], t = ar_join(c);
        const short si = s0[i];
        int v = c;
        if (c == 0x200C || c == 0x200D) continue;       /* ZWNJ и ZWJ разорвали или связали соседей - их не выводим */
        if (t == AJ_D || t == AJ_R) {
            const int pj = i > 0 && (ar_join(c0[i - 1]) == AJ_D || ar_join(c0[i - 1]) == AJ_C);   /* с предыдущей */
            const int a = (i + 1 < n0) ? c0[i + 1] : 0;
            if (c == 0x0644 && (a == 0x0622 || a == 0x0623 || a == 0x0625 || a == 0x0627)) {     /* лам с алифом */
                v = (a == 0x0622 ? 0xFEF5 : a == 0x0623 ? 0xFEF7 : a == 0x0625 ? 0xFEF9 : 0xFEFB) + pj;
                i++;                                    /* алиф - уже в лигатуре */
            } else {
                const int nj = t == AJ_D && ar_join(a) != AJ_U;                                   /* со следующей */
                const unsigned short *f = s_ar_form[c - 0x0621];
                v = f[pj ? (nj ? 3 : 1) : (nj ? 2 : 0)];
                if (!v) v = f[0];
            }
        }
        cp[m] = v; src[m] = si; m++;
    }
    for (int i = 0; i < m; i++) {
        cls[i] = org[i] = (unsigned char)ar_class(cp[i]);
        rtl |= cls[i] == AB_R || cls[i] == AB_AL;
    }
    if (rtl) {
        /* W1-W7; уровень строки 1, по краям (sos, eos) - R */
        for (int i = 0; i < m; i++) if (cls[i] == AB_NSM) cls[i] = i ? cls[i - 1] : AB_R;
        for (int i = 0, last = AB_R; i < m; i++) {                                            /* W2 */
            if (cls[i] == AB_L || cls[i] == AB_R || cls[i] == AB_AL) last = cls[i];
            else if (cls[i] == AB_EN && last == AB_AL) cls[i] = AB_AN;    /* число после арабской буквы - арабское */
        }
        for (int i = 0; i < m; i++) if (cls[i] == AB_AL) cls[i] = AB_R;                       /* W3 */
        for (int i = 1; i + 1 < m; i++) {                                                     /* W4 */
            const int a = cls[i - 1], b = cls[i + 1];
            if (cls[i] == AB_ES && a == AB_EN && b == AB_EN) cls[i] = AB_EN;
            else if (cls[i] == AB_CS && a == b && (a == AB_EN || a == AB_AN)) cls[i] = (unsigned char)a;
        }
        for (int i = 0; i < m; ) {                                                            /* W5 */
            int j = i;
            while (j < m && cls[j] == AB_ET) j++;
            if (j == i) { i++; continue; }
            if ((i > 0 && cls[i - 1] == AB_EN) || (j < m && cls[j] == AB_EN))
                for (int k = i; k < j; k++) cls[k] = AB_EN;
            i = j;
        }
        for (int i = 0; i < m; i++) if (cls[i] == AB_ES || cls[i] == AB_ET || cls[i] == AB_CS) cls[i] = AB_ON;   /* W6 */
        for (int i = 0, last = AB_R; i < m; i++) {                                            /* W7 */
            if (cls[i] == AB_L || cls[i] == AB_R) last = cls[i];
            else if (cls[i] == AB_EN && last == AB_L) cls[i] = AB_L;
        }
        /* N0: парные скобки ( ) [ ] { } - по направлению внутри, а если внутри только обратное - по тексту перед ними */
        {
            int st_c[63], st_i[63], ns = 0, np = 0;
            for (int i = 0; i < m && np < AR_MAX / 2; i++) {
                if (cls[i] != AB_ON) continue;
                const int c = cp[i], cl = c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : 0;
                if (cl) {
                    if (ns == 63) break;
                    st_c[ns] = cl; st_i[ns] = i; ns++;
                } else if (c == ')' || c == ']' || c == '}') {
                    for (int k = ns - 1; k >= 0; k--)
                        if (st_c[k] == c) { po[np] = (short)st_i[k]; pc[np] = (short)i; np++; ns = k; break; }
                }
            }
            for (int a = 1; a < np; a++)                                    /* пары - по порядку открывающих */
                for (int b = a; b > 0 && po[b - 1] > po[b]; b--) {
                    short t = po[b]; po[b] = po[b - 1]; po[b - 1] = t;
                    t = pc[b]; pc[b] = pc[b - 1]; pc[b - 1] = t;
                }
            for (int p = 0; p < np; p++) {
                int in_r = 0, in_l = 0, d = -1;
                for (int k = po[p] + 1; k < pc[p]; k++) {
                    const int s = ar_strong(cls[k]);
                    if (s == AB_R) in_r = 1; else if (s == AB_L) in_l = 1;
                }
                if (in_r) d = AB_R;
                else if (in_l) {
                    d = AB_R;
                    for (int k = po[p] - 1; k >= 0; k--) { const int s = ar_strong(cls[k]); if (s >= 0) { d = s; break; } }
                }
                if (d >= 0) cls[po[p]] = cls[pc[p]] = (unsigned char)d;
            }
        }
        for (int i = 0; i < m; ) {                       /* N1, N2: нейтральные - как соседи с обеих сторон, иначе R */
            if (!ar_ni(cls[i])) { i++; continue; }
            int j = i;
            while (j < m && ar_ni(cls[j])) j++;
            const int a = i ? ar_strong(cls[i - 1]) : AB_R, b = (j < m) ? ar_strong(cls[j]) : AB_R;
            for (int k = i; k < j; k++) cls[k] = (unsigned char)(a == b ? a : AB_R);
            i = j;
        }
        for (int i = 0; i < m; i++) lvl[i] = cls[i] == AB_R ? 1 : 2;         /* I2: L и числа - уровнем выше */
        for (int i = m - 1, tail = 1; i >= 0; i--) {                          /* L1: пробелы в конце и у табуляции */
            if (org[i] == AB_S || org[i] == AB_B) { lvl[i] = 1; tail = 1; }
            else if (org[i] == AB_WS && tail) lvl[i] = 1;
            else tail = 0;
        }
        for (int i = 0; i < m; ) {                       /* L2: отрезки уровня 2 разворачиваются, потом вся строка */
            if (lvl[i] < 2) { i++; continue; }
            int j = i;
            while (j < m && lvl[j] == 2) j++;
            ar_rev(cp, src, lvl, i, j);
            i = j;
        }
        ar_rev(cp, src, lvl, 0, m);
        for (int i = 0; i < m; i++) if (lvl[i] == 1) cp[i] = ar_mirror(cp[i]);                /* L4 */
    }
    int k = 0;
    for (int i = 0; i < m; i++) {
        if (cp[i] == 0x200E || cp[i] == 0x200F || cp[i] == 0x061C) continue;   /* LRM, RLM, ALM - без глифа */
        out[k] = cp[i];
        if (idx) idx[k] = src[i];
        k++;
    }
    return k;
}
/* строка к выводу: s как есть или переписанная в buf (AR_BUF байт), только до перевода строки - дальше функции
 * вывода и так не идут */
static const char *ar_vis(const char *s, char *buf)
{
    if (!s) return s;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && *u != '\n' && *u < 0x80) u++;
    if (*u < 0x80) return s;                             /* одна латиница */
    int cp[AR_MAX], vis[AR_MAX], n = 0, rtl = 0;
    for (const char *p = s; *p && *p != '\n'; n++) {
        if (n == AR_MAX) return s;
        p = utf8_next(p, &cp[n]);
        rtl |= ar_rtl(cp[n]);
    }
    if (!rtl) return s;
    const int m = ar_layout(cp, n, vis, NULL);
    char *o = buf;
    for (int i = 0; i < m; i++) o += utf8_put(o, vis[i]);
    *o = 0;
    return buf;
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

    fprintf(stderr, "dc_font: font not found "
                    "(put a ttf into assets/fonts/dc_ui.ttf or set DC_FONT)\n");
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
    fprintf(stderr, "dc_font: loaded %s%s%s, size %d px, line height %d, antialiasing %s, bold: %s, fallback: %s\n",
            s_path, s_family[0] ? " - " : "", s_family, s_size, dc_font_line_height(),
            s_antialias ? "on" : "off", s_ready_b ? s_path_b : "no file (the regular one is used)",
            s_ready_fb ? s_path_fb : "none");
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
    if (cached) fprintf(stderr, "dc_font: pixel grid - %d px per em\n", cached);
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
    fprintf(stderr, "dc_font: main font -> %s%s%s (size %d, antialiasing %s)\n",
            s_path, s_family[0] ? " - " : "", s_family, s_size, s_antialias ? "on" : "off");
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
        fprintf(stderr, "dc_font: bold -> %s\n", s_ready_b ? s_path_b : "no file (the regular one thickened)");
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
            fprintf(stderr, "dc_font: CJK glyphs -> at the first character (DC_FONT_CJK_EAGER=1 - at once)\n");
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
    fprintf(stderr, "dc_font: CJK glyphs -> %s\n", got ? s_path_cjk : "no system font");
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
    if (s_ready_ar > 0) s_scale_ar = stbtt_ScaleForMappingEmToPixels(&s_info_ar, (float)px);
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

/* 27.10: системный шрифт с арабскими буквами (s_info_ar) - DC_FONT_AR или Segoe UI, Tahoma, Arial из %WINDIR%\Fonts;
 * ищется один раз, при первой арабской букве. Кеш не сбрасывается: арабских глифов в нём ещё нет. */
static int ar_font(void)
{
    if (s_ready_ar) return s_ready_ar > 0;
    static const char *const names[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
    const char *env = getenv("DC_FONT_AR");
    const char *win = getenv("WINDIR");
    char path[512];
    s_ready_ar = -1;
    for (int i = -1; i < (int)(sizeof names / sizeof names[0]) && s_ready_ar < 0; i++) {
        if (i < 0) { if (!env || !*env) continue; snprintf(path, sizeof path, "%s", env); }
        else snprintf(path, sizeof path, "%s/Fonts/%s", (win && *win) ? win : "C:/Windows", names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = (n > 0 && n <= (64 << 20)) ? (unsigned char *)malloc((size_t)n) : NULL;
        size_t rd = buf ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        int off = (buf && rd == (size_t)n) ? stbtt_GetFontOffsetForIndex(buf, 0) : -1;
        stbtt_fontinfo fi;                   /* целым или никак (try_load) */
        if (off < 0 || !stbtt_InitFont(&fi, buf, off)) { free(buf); continue; }
        s_info_ar = fi;
        s_ready_ar = 1;
        s_scale_ar = stbtt_ScaleForMappingEmToPixels(&s_info_ar, (float)s_size);
    }
    fprintf(stderr, "dc_font: Arabic glyphs -> %s\n", s_ready_ar > 0 ? path : "no system font");
    return s_ready_ar > 0;
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
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
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
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
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
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
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
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
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
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
    int ss = ui_ss();
    font_draw_ex(s_ui, s_ui_w, s_ui_h, x * ss, y * ss, utf8, rgba, outline, 0, 1);
    s_ui_dirty = 1;
}

void dc_ui_text_centered(int y, const char *utf8, uint32_t rgba, int outline)
{
    if (!dc_font_ready() || !ui_buf()) return;
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: один раз на ширину и вывод */
    int ss = ui_ss();
    int w = text_width_ui(utf8, 0);
    font_draw_ex(s_ui, s_ui_w, s_ui_h, (s_ui_w - w) / 2, y * ss, utf8, rgba, outline, 0, 1);
    s_ui_dirty = 1;
}

/* Жирным (заголовки, вкладки): отдельный жирный файл, иначе утолщение. */
void dc_ui_text_b(int x, int y, const char *utf8, uint32_t rgba, int outline)
{
    if (!dc_font_ready() || !ui_buf()) return;
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
    int ss = ui_ss();
    font_draw_ex(s_ui, s_ui_w, s_ui_h, x * ss, y * ss, utf8, rgba, outline, 1, 1);
    s_ui_dirty = 1;
}

int dc_font_text_width_b(const char *utf8)
{
    if (!s_ready || !utf8) return 0;
    char ab[AR_BUF]; utf8 = ar_vis(utf8, ab);   /* 27.10: арабский - формы и порядок (ar_vis) */
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
