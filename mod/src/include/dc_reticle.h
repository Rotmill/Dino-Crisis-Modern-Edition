/* dc_reticle.h — перекрестие на SDL (см. dc_reticle.c).
 *
 * Отрисовка отделена от решения «рисовать ли и что именно»: решение остаётся
 * в main.cpp рядом с состоянием игры, сюда приходит уже готовое описание.
 * Так прицел перестал зависеть от Dear ImGui, а отладочные окна F2/F8/F9
 * можно выключать из релиза целиком, не трогая прицел.
 */

#ifndef DC_RETICLE_H
#define DC_RETICLE_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DC_RET_CROSS = 0,   /* пистолет и всё неопознанное */
    DC_RET_DOT,         /* точка */
    DC_RET_ARCS,        /* дробовик: две дуги */
    DC_RET_RING,        /* гранатомёт: четыре диагонали */
    DC_RET_DART         /* транквилизатор: три штриха */
};

typedef struct {
    int   shape;        /* DC_RET_* */
    int   red;          /* красный вид: опция «красное перекрестие» или цель под прицелом */
    int   on_target;    /* под прицелом существо: штрих толще */
    int   locked;       /* сведение дошло до конца: ярче */
    float size;         /* hud.reticle_size */
    float open;         /* 1 - сведение: насколько разведён прицел */
    float focus_raw;    /* сырое сведение для дробовика (без ограничения 70 %) */
    float sg_park;      /* DC_SG_RETICLE: где припаркованы дуги дробовика */
    float sg_anim;      /* DC_SG_ANIM: размах их колебания */
} DcReticle;

void dc_reticle_draw(SDL_Renderer *r, const DcReticle *o);

/* 13.10.2026: строка сообщения (квиксейвы) прежним системным шрифтом (Bahnschrift), размер и место - как в деве:
 * x, y - левый верхний угол строки в логическом пространстве рендерера, цвет 0..255 */
void dc_osd_text(SDL_Renderer *r, float x, float y, const char *s, int cr, int cg, int cb, int ca);

#ifdef __cplusplus
}
#endif

#endif /* DC_RETICLE_H */
