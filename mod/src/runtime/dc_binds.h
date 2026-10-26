/* dc_binds — назначения кнопок: шаблоны раскладок Original / Modern и своя раскладка (Custom).
 *
 * ⭐ 04.10.2026 (вечер, 6), игрок: «Добавь в опции controls настройку кастомного управления и выбор из шаблонов
 * раскладок (оригинальный и модерн. Модерн сейчас у нас в обоих режимах стоит. Оригинальную раскладку игры в
 * оригинальном режиме не меняешь, а в нашем режиме адаптируй). Всё сделай как под клава мышь, так и под геймпад…
 * По дефолту поставь оригинальную раскладку игры в оригинальном режиме и нашу текущую модерн раскладку в нашем».
 *
 * Действие (DCB_*) — то, что делает кнопка; у каждого типа игры (наш / оригинальный) — своя раскладка: шаблон
 * Original, Modern или своя. На действие — до DCB_MAX входов клавиатуры и мыши и отдельно геймпада.
 *   Modern   — раскладка, что стояла до 04.10 (ремейки RE): WASD, мышь, E/F, Shift, Tab…; L2/R2, A, B, X, L3, R3.
 *   Original — игра 1999 года: геймпад PS1 один в один (✕ действие/выстрел, □ бег, ○ инвентарь, △ отмена, R1 прицел,
 *              R2 разворот, L1 смена цели), клавиатура — как в PC-версии 2000 года (стрелки, C, X, V, Z, D, B, A, Ctrl).
 *              В нашем режиме — она же, но движение относительно камеры, камера на мыши и правом стике, прицел и
 *              выстрел ещё и на кнопках мыши, смены цели нет (цель — под перекрестием).
 * Что из этого получает игра — main.cpp (pad_from_keyboard_b, merge_controller_pad_b, слой apply_ots_movement).
 * Хранение — dc_user.cfg: inp.layout.m / inp.layout.o (строки меню) и bind.<m|o>.<kb|pad>.<действие>=коды (своя).
 * 20.10.2026: прежний ввод (DC_BINDS=0, раскладка до 04.10) вырезан. */
#ifndef DC_BINDS_H
#define DC_BINDS_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DCB_FWD, DCB_BACK, DCB_LEFT, DCB_RIGHT,           /* движение (в оригинальном режиме — танковое) */
    DCB_RUN, DCB_RUN_TOGGLE, DCB_QUICKTURN,           /* бег, пока держишь / переключением, разворот на 180° */
    DCB_AIM, DCB_FIRE, DCB_TARGET,                    /* прицел, выстрел, смена цели */
    DCB_ACTION, DCB_INVENTORY, DCB_CANCEL,            /* действие / подтвердить, инвентарь, отмена */
    DCB_PAUSE, DCB_OPTIONS,                           /* пауза (Start), меню OPTION игры (Select) */
    /* 25.10.2026 вечер (игрок: «добавь для переназначения эти клавиши. То есть эти действия»): быстрое сохранение,
     * загрузка, слот вперёд / назад - не кнопки игры, их делает main.cpp (dc_qs_do) */
    DCB_QSAVE, DCB_QLOAD, DCB_QSLOT_NEXT, DCB_QSLOT_PREV,
    /* 25.10.2026 ночь (игрок: «нету кнопки O, которую можно переназначить... переключение между режимами оригинальным и
     * modern»): смена типа игры - тоже не кнопка игры, была клавишей O мимо раскладки; делает main.cpp (dc_game_type_toggle) */
    DCB_GAMETYPE,
    DCB_COUNT
};
#define DCB_IS_QS(a) ((a) >= DCB_QSAVE && (a) <= DCB_QSLOT_PREV)
#define DCB_IS_PROG(a) ((a) >= DCB_QSAVE && (a) <= DCB_GAMETYPE)   /* действия программы: быстрые сохранения и тип игры */
enum { DCB_KB = 0, DCB_PAD = 1 };                     /* устройство: клавиатура + мышь / геймпад */
enum { DCB_MODERN = 0, DCB_ORIG = 1 };                /* тип игры: g_dc_orig_mode */
enum { DCB_TPL_ORIG = 0, DCB_TPL_MODERN = 1, DCB_TPL_CUSTOM = 2 };
#define DCB_MAX 4                                     /* входов на действие у одного устройства */

/* Коды входов. Клавиатура — SDL_Scancode (1..511); Shift, Ctrl и Alt — любой из двух. */
#define DCB_SHIFT     0x200
#define DCB_CTRL      0x201
#define DCB_ALT       0x202
#define DCB_MOUSE(b)  (0x300 + (b))                   /* SDL_BUTTON_LEFT (1) .. SDL_BUTTON_X2 (5) */
#define DCB_WHEEL_UP   0x306                          /* 10.10: колесо мыши - короткое нажатие на шаг колеса (в мире) */
#define DCB_WHEEL_DOWN 0x307
#define DCB_PADB(b)   (0x400 + (b))                   /* SDL_GameControllerButton */
#define DCB_PADT(t)   (0x480 + (t))                   /* курок: 0 левый (L2/LT), 1 правый (R2/RT) */
#define DCB_IS_MOUSE(c) ((c) >= 0x301 && (c) <= 0x307)
#define DCB_IS_WHEEL(c) ((c) == DCB_WHEEL_UP || (c) == DCB_WHEEL_DOWN)
#define DCB_IS_PAD(c)   ((c) >= 0x400 && (c) <= 0x481)
#define DCB_IS_KB(c)    ((c) > 0 && (c) < 0x300)

int  dcb_mode(void);                                  /* тип игры сейчас: DCB_MODERN / DCB_ORIG */
/* 10.10.2026: шаблон - у каждой пары «тип игры + устройство» свой (оригинальный режим: клавиатура Modern, геймпад Original) */
int  dcb_layout_dev(int mode, int dev);               /* DCB_TPL_* */
void dcb_set_layout_dev(int mode, int dev, int tpl);
int  dcb_default_tpl(int mode, int dev);              /* шаблон по умолчанию (DC_ORIG_KB_MODERN=0 - клавиатура O: Original) */
int  dcb_kb_layout_1015(void);                         /* 15.10: шаблоны клавиатуры Original = классическая (DC_KB_LAYOUT_1015=0 - как было) */
int  dcb_layout(int mode);                            /* = dcb_layout_dev(mode, DCB_KB) */
void dcb_set_layout(int mode, int tpl);               /* оба устройства */
/* 10.10: чью раскладку правит меню назначений (строка «Для типа игры»): -1 - текущий тип игры */
int  dcb_edit_mode(void);
void dcb_set_edit_mode(int mode);
/* Назначения действия в раскладке типа игры mode (шаблон или своя): число входов, коды — в out[DCB_MAX]. */
int  dcb_get(int mode, int dev, int act, int *out);
/* Правка своей раскладки. Первая правка типа игры копирует его шаблон в свою раскладку (обоих устройств) и ставит
 * Custom. add=0 — вход становится единственным у действия, add=1 — добавляется (старые вытесняются по очереди).
 * Тот же вход у другого действия того же устройства снимается (кроме пар, которые шаблоны делят намеренно:
 * действие/выстрел и инвентарь/отмена); возвращает действие, у которого его сняли, или -1. */
int  dcb_assign(int mode, int dev, int act, int code, int add);
void dcb_clear(int mode, int dev, int act);
void dcb_restore(int mode, int dev);                  /* 10.10: назад к шаблону по умолчанию этого типа игры и устройства */
int  dcb_norm(int code);                              /* правый/левый Shift, Ctrl, Alt -> «любой» */
int  dcb_code_ok(int dev, int code);                  /* вход можно назначить на этом устройстве */
const char *dcb_code_label(int code);                 /* подпись входа — английская строка (ключ атласа меню) */
int  dcb_code_pad_icon(int code);                     /* значок схемы геймпада DCS_* или -1 */
const char *dcb_act_title(int act);                   /* подпись действия — ключ атласа меню */
const char *dcb_act_key(int act);                     /* имя действия в dc_user.cfg */
unsigned dcb_serial(void);                            /* растёт при любой смене назначений — кэшам ввода */
/* dc_user.cfg: своя раскладка пишется строками bind.*, читается обратно (1 — строка наша). */
int  dcb_cfg_read(const char *key, const char *val);
void dcb_cfg_write(FILE *f);

#ifdef __cplusplus
}
#endif
#endif /* DC_BINDS_H */
