/* dc_font — собственный текстовый рендерер для интерфейса Dino Crisis PT.
 *
 * Родной шрифт игры — битмап в VRAM, только заглавная латиница: ни кириллицы,
 * ни иероглифов в нём нет. Этот модуль растеризует произвольный TTF (по умолчанию
 * Fusion Pixel 12px, SIL OFL) и рисует UTF-8 строки в RGBA-буфер, поэтому текст
 * интерфейса может быть на русском, китайском, французском и итальянском.
 *
 * Шрифт МЕНЯЕТСЯ ФАЙЛОМ, без пересборки: DC_FONT=<путь к ttf>. Кегль — DC_FONT_PX.
 * Пиксельный шрифт растеризуется без сглаживания (порог), чтобы глиф лёг ровно
 * в сетку; кегль, кратный базовому (12 -> 24 -> 36), даёт точное увеличение.
 */
#ifndef DC_FONT_H
#define DC_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Грузит шрифт (DC_FONT или встроенный список путей). 1 = готов, 0 = нет файла.
 * Вызывать один раз при старте; повторные вызовы безвредны. */
int  dc_font_init(void);
int  dc_font_use_file(const char *path);   /* перезагрузить основной шрифт */
int  dc_font_apply_language(int lang);
int  dc_font_pixel_em(void);   /* пикселей в em у пиксельного шрифта, 0 — вектор */
void dc_font_pixel_em_reset(void);     /* 0 = English (dc_ui_en.ttf), иначе dc_ui.ttf */
int  dc_font_ready(void);

/* Кегль в пикселях канваса. По умолчанию 24 = базовые 12px на канвасе 640x480,
 * то есть визуально размер родного шрифта игры. */
void dc_font_set_size(int px);

int  dc_font_line_height(void);            /* высота строки (без linegap; DC_FONT_LINEGAP=1 вернёт его) */
int  dc_font_text_width(const char *utf8); /* ширина строки в пикселях */

/* Растеризовать строку в 8-битную маску покрытия w×h (0..255, сглаживание) кеглем px
 * в ПИКСЕЛЯХ ИГРЫ, с утолщением bold (доп. проходов на пиксель вправо), от левого края x0
 * и базовой линии baseline; style 0 - обычный, 1 - жирный (assets/fonts/dc_ui_bold.ttf,
 * иначе обычный). Временно меняет кегль и очищает кэш глифов - звать редко.
 * Возвращает правый край набранной строки. */
int  dc_font_render_cov(const char *utf8, int px, int style, int bold,
                        unsigned char *cov, int w, int h, int x0, int baseline);
int  dc_font_text_width_px(const char *utf8, int px, int style, int bold);
int  dc_font_cap_height(int px, int style);      /* высота заглавной H в пикселях */
int  dc_font_fit_px(int cap_px, int style);      /* кегль под заданную высоту заглавных */
int  dc_font_has_bold(void);                     /* загружен ли отдельный жирный файл */
void dc_ui_text_b(int x, int y, const char *utf8, uint32_t rgba, int outline); /* жирным в наш слой */
int  dc_font_text_width_b(const char *utf8);     /* ширина строки жирным */

/* Атлас шрифта игры (страница 4bpp 256×256 = 32768 байт) + таблица ширин
 * (256, 0xFF = ячейку не трогать) из загруженного TTF; cell_cp[i] — код
 * символа ячейки i (0 — не трогать). Возвращает число нарисованных ячеек. */
int  dc_font_build_game_atlas(const int *cell_cp, int ncells, int px, int baseline, int cap_px,
                              const unsigned char *grey16, unsigned char *out, unsigned char *widths);
/* Индекс палитры для строки y штриха: яркость 1.0 у ytop -> 0.7 у ybot,
 * ближайший по реальным серым палитры grey16[16] (0 не используется). */
int  dc_font_grad_index(const unsigned char *grey16, int y, int ytop, int ybot);
/* hi-res RGBA8-атлас (256·S)² для растеризатора: те же ячейки/кегль, что у 4bpp */
int  dc_font_build_hires_atlas(const int *cell_cp, int ncells, int px, int baseline, int cap_px,
                               int S, const unsigned char *grey16, unsigned char *out,
                               unsigned char *drawn /* [256] или NULL: какие ячейки нарисованы */);
/* hi-res RGBA-полоска (w·S)×(h·S) с текстом — титульные надписи игры */
int  dc_font_build_hires_strip(const char *utf8, int px, int style, int bold, int w, int h,
                               int baseline, int cap_px, int S, const unsigned char *grey16,
                               unsigned char *out);

const char *dc_font_family(void); /* имя семейства из таблицы name, если есть */

/* --- слой интерфейса поверх кадра ------------------------------------------
 * Буфер логического канваса (640x480), который present копирует последним.
 * Очищается каждый кадр; рисовать между dc_ui_begin() и present. */
uint32_t *dc_ui_buffer(int *w, int *h);   /* ФИЗИЧЕСКИЙ размер = 640·S × 480·S, см. dc_ui_scale */
int       dc_ui_scale(void);       /* S: суперсэмплинг слоя (DC_UI_SS, по умолчанию 3); все
                                    * координаты dc_ui_* — логические 640×480, слой хранится
                                    * в S раз крупнее, чтобы текст на 1080p был резким */
void      dc_ui_begin(void);       /* очистить прозрачным */
/* 17.09.2026 вечер: прямоугольник нарисованного за кадр (физические координаты слоя) — чистка и заливка только
 * его; dc_ui_mark — для прямой записи в буфер мимо dc_ui_*; dc_ui_dirty_rect: 1 — есть, mode 0/1/2 = DC_UI_DIRTYRECT */
void      dc_ui_mark(int x0, int y0, int x1, int y1);
int       dc_ui_dirty_rect(int *x, int *y, int *w, int *h, int *mode);
int       dc_ui_dirty(void);       /* 1, если в этом кадре что-то нарисовано */
void      dc_ui_text(int x, int y, const char *utf8, uint32_t rgba, int outline);
void      dc_ui_text_centered(int y, const char *utf8, uint32_t rgba, int outline);
void      dc_ui_rect(int x, int y, int w, int h, uint32_t rgba);

#ifdef __cplusplus
}
#endif
#endif /* DC_FONT_H */
