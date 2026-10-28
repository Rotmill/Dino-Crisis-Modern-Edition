/* dc_splash — экраны запуска мода: выбор языка и «Раскладка управления».
 *
 * Дизайн — проект Claude Design «Раскладка управления» (10.09.2026). Чтобы на
 * экране было ровно то, что нарисовано, экраны рендерит браузер в PNG
 * (design/controls_layout/render/render.py), а здесь — только показ этих
 * картинок поверх кадра и навигация. Подробности — в dc_splash.cpp.
 *
 * При каждом запуске: выбор языка -> раскладка управления -> игра. Всё
 * пропускается одной кнопкой. Из меню настроек (CONTROLS -> Controls Layout)
 * раскладка открывается поверх меню.
 */
#ifndef DC_SPLASH_H
#define DC_SPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Renderer;

/* Действия кнопок геймпада — подписи на схеме «Раскладки управления». Схема
 * собирается по input.ini (main.cpp, splash_pad_slot_actions): порядок здесь =
 * порядок в подписи, «Бег, пока держишь / Назад» так и складывается. */
enum {
    DCA_AIM, DCA_FIRE, DCA_ACTION, DCA_MOVEMENT, DCA_MOVE, DCA_CAMERA, DCA_INVENTORY,
    DCA_RUN_HOLD, DCA_TOGGLE_RUN, DCA_BACK, DCA_SWITCH_TARGET, DCA_PAUSE, DCA_OPTION,
    DCA_QUICK_TURN,                 /* 15.09 ночь: разворот на 180° — R3 (прежде отдельный блок «L3 + B») */
    DCA_ACTION_FIRE,                /* 04.10 (6): ✕ раскладки Original — «Действие / Выстрел» одной короткой подписью */
    DCA_COUNT
};
/* Кнопки на схеме — места подписей из дизайна. У Back (Share/Create) места нет. */
enum {
    DCS_L2, DCS_L1, DCS_DPAD, DCS_LSTICK, DCS_R2, DCS_R1, DCS_TRI, DCS_SQR, DCS_CIR,
    DCS_CRS, DCS_RSTICK, DCS_TOUCH, DCS_OPTIONS, DCS_L3, DCS_R3, DCS_COUNT
};

/* Ручки main.cpp: язык живёт в таблице опций меню, геймпад — там же. */
typedef struct {
    int  (*get_lang)(void);         /* язык интерфейса: ui.lang 0..7 */
    void (*set_lang)(int lang);     /* сменить язык так же, как пункт меню Language */
    void (*lang_chosen)(void);      /* игрок выбрал язык: записать dc_user.cfg */
    int  (*pad_connected)(void);    /* есть геймпад — стартовая вкладка и подсказки под него */
    unsigned (*pad_slot_actions)(int slot);   /* маска 1<<DCA_* для кнопки DCS_* */
    /* 04.10 (6): какая вкладка клавиатуры: 0 — шаблон Modern в нашем типе игры (ui_ctrl_<код>_kb.png), 1 — Modern в
     * оригинальном (_kb_mo), 2 — Original в нашем (_kb_om), 3 — Original в оригинальном (_kb_oo), 4 — своя раскладка
     * (список действий собирается из атласа меню) */
    int  (*kb_layout_kind)(void);
    /* 04.10 (6): подписи кнопок по типу пада: 0 — PlayStation / Xbox (фигура и буква Xbox), 1 — Nintendo (буквы на
     * тех же местах другие: снизу B, справа A, слева Y, сверху X) */
    int  (*pad_style)(void);
} DcSplashHooks;

void dc_splash_init(const DcSplashHooks *h);

/* Очередь экранов при запуске; show_lang — начать с выбора языка. */
void dc_splash_boot(int show_lang);
/* Раскладка поверх меню настроек (from_menu=1: «назад» возвращает в меню). */
void dc_splash_open_controls(int from_menu);
void dc_splash_open_lang(void);     /* отдельно — для отладочного порта */
void dc_splash_open_disc(void);     /* 04.10 (7): экран «о моде» — тоже для порта */

int  dc_splash_active(void);
/* Закрыть всё сразу (например, игрок нажал быструю загрузку). */
void dc_splash_close(const char *why);
/* SDL_GetTicks() последнего закрытия: второй KEYDOWN одного нажатия Esc (так
 * шлёт PostMessage-харнес) не должен закрыть ещё и меню под экраном. */
unsigned dc_splash_closed_tick(void);

/* Навигация кодами DC_MENU_* (dc_settings.h). from_pad — нажато на геймпаде:
 * подсказки внизу переключаются на его кнопки. Возвращает 1, если съедено. */
int  dc_splash_action(int act, int from_pad);
/* Мышь в логических координатах рендерера (как у SDL_RenderWindowToLogical). */
void dc_splash_mouse(float lx, float ly, int lclick, int rclick, int logical_w, int logical_h);

/* Нарисовать поверх готового кадра. Вызывать последним перед отладочными
 * панелями; логический размер — тот, что стоит у рендерера. */
void dc_splash_present(struct SDL_Renderer *r, int logical_w, int logical_h);

/* 18.10.2026: экран «Подготовка шейдеров» (main.cpp dc_shader_prep): progress 0..1, < 0 - только фон; lang = ui.lang 0..7 */
void dc_splash_shader_present(struct SDL_Renderer *r, int logical_w, int logical_h, int lang, float progress);
void dc_splash_shader_done(void);

/* 20.10.2026: главное меню титула - справа снизу «Please leave feedback:» со значком Discord и «Made by ROTMILL.» с версией
 * (картинки ui_feedback_off/on.png). show - меню на экране (появление и уход плавные), hover - мышь над ссылкой, kx/ky -
 * правый верх строки копирайта Capcom в логических координатах (0 - не мешает). hit: логическая точка над ссылкой. */
void dc_splash_feedback_present(struct SDL_Renderer *r, int logical_w, int logical_h, int show, int hover, float kx, float ky);
int  dc_splash_feedback_hit(float lx, float ly);
/* 26.10.2026: кадр между тактами - тот же блок, что нарисовал прошлый dc_splash_feedback_present (ничего не считает заново) */
void dc_splash_feedback_again(struct SDL_Renderer *r, int logical_w, int logical_h);

/* 24.10.2026: счётчик кадров (SCREEN -> FPS Counter): число картинок в секунду в левом верхнем углу окна; fps < 0 - ещё не
 * сосчитано, ничего не рисуется */
void dc_splash_fps_present(struct SDL_Renderer *r, int fps);

/* Поля JSON для отладочного порта ({"cmd":"splash_state"}). */
int  dc_splash_state(char *buf, int n);

/* Меню настроек в стиле этих экранов (10.09.2026, игрок: «сделай стиль меню
 * опций полностью таким же, как окна раскладки и выбора языка»). Логика,
 * таблица и переводы — dc_settings.c, здесь только вид и мышь.
 * dc_menu_ui_ready(): меню открыто и все его строки есть в атласе текущего
 * языка — рисуем новое; иначе main.cpp рисует прежнее (dc_menu_draw).
 * DC_MENU_STYLE=0 — всегда прежнее. */
int  dc_menu_ui_ready(void);
void dc_menu_ui_present(struct SDL_Renderer *r, int logical_w, int logical_h);   /* звать каждый кадр */
void dc_menu_ui_mouse(float lx, float ly, int lclick, int logical_w, int logical_h);

#ifdef __cplusplus
}
#endif
#endif /* DC_SPLASH_H */
