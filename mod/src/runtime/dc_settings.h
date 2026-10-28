/* dc_settings — экранное меню PC-настроек Dino Crisis PT.
 *
 * План: DinoCrisis_PT_v0823/docs/pc_options_plan.md. Меню рисуется НАШИМ слоем
 * (dc_font.c: dc_ui_text / dc_ui_rect, буфер 640x480 поверх кадра), поэтому
 * подписи могут быть на любом языке, а логика игры не трогается вовсе.
 *
 * Таблицу опций объявляет main.cpp — там под рукой и окно SDL, и ручки камеры,
 * и рендерер; здесь только описание типов, файл настроек, навигация и вывод.
 */
#ifndef DC_SETTINGS_H
#define DC_SETTINGS_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DC_OPT_BOOL,     /* Вкл / Выкл */
    DC_OPT_ENUM,     /* список значений (choices) */
    DC_OPT_INT,      /* целое с шагом */
    DC_OPT_FLOAT,    /* дробное с шагом */
    DC_OPT_ACTION,   /* строка-кнопка: подтверждение вызывает set(1) */
    DC_OPT_HEADER,   /* заголовок группы, не выбирается */
    DC_OPT_BIND,     /* 04.10 (6): назначение действия (dc_binds.h): lo — действие DCB_*, hi — устройство DCB_KB/DCB_PAD;
                      * подтверждение — ждать новую клавишу/кнопку (она заменит прежние), добавить, очистить */
    DC_OPT_DEVICE    /* 04.10 (6): выбор геймпада: 0 — Auto, 1..n — подключённые (число и подписи — dc_menu_set_dev_hooks);
                      * choices — все подписи, что строка может показать (для атласа меню) */
} DcOptType;

/* Когда изменение вступает в силу. Опции с DC_APPLY_CONFIRM трогают ресурсы
 * рендера, поэтому применяются не на каждый шаг ползунка, а по подтверждению. */
typedef enum { DC_APPLY_LIVE, DC_APPLY_CONFIRM, DC_APPLY_RESTART } DcOptApply;

typedef struct DcOpt {
    const char  *id;          /* ключ в dc_user.cfg, например "cam.distance"; NULL — строка
                               * не сохраняется (её значение хранит скрытая строка с ключом) */
    const char  *title;       /* подпись в меню */
    const char  *hint;        /* строка-пояснение внизу экрана */
    int          page;        /* номер вкладки */
    DcOptType    type;
    DcOptApply   apply;
    float        lo, hi, step, def;
    const char **choices;     /* для DC_OPT_ENUM, NULL-терминированный список */
    float      (*get)(void);
    void       (*set)(float);
    float        pct;         /* INT/FLOAT: показывать процентами — значение × pct и «%»
                               * (15.09: «1.00», «18.00», «1700» игроку ничего не говорили); 0 — число */
    const char **hints;       /* 13.10.2026: ENUM - пояснение к каждому значению (NULL-терминированный, как choices):
                               * есть - внизу пояснение выбранного значения вместо hint (режимы ходьбы в прицеле) */
} DcOpt;

/* Регистрация таблицы (массив и его длина) и названий вкладок. */
void dc_menu_init(const DcOpt *opts, int count,
                  const char **pages, int page_count);

/* Язык интерфейса (ui.lang): 0 English (строки таблицы как есть), 1 Русский,
 * 2 Deutsch, 3 Español, 4 Français, 5 Português (Brasil), 6 日本語, 7 简体中文, 8 Italiano (20.10.2026),
 * 9 арабский (27.10.2026, справа налево) -
 * тот же набор и порядок, что у установщика мода. dc_tr() переводит строку
 * меню; нет перевода — возвращает английскую. */
#define DC_LANG_COUNT 10
#define DC_LANG_AR 9   /* 27.10.2026: арабский - справа налево (dc_font.c связывает буквы, dc_splash.cpp - плашка подсказки) */
extern int g_dc_menu_lang;
const char *dc_tr(const char *en);

/* Файл настроек: dc_user.cfg рядом с exe. Загрузка применяет значения через
 * set() каждой опции; сохранение пишет текущие get(). */
void dc_menu_load(void);
extern int g_dc_menu_loading;   /* 13.10.2026: 1 - пока dc_menu_load применяет dc_user.cfg */
void dc_menu_save(void);
void dc_menu_save_quiet(void);   /* без строки «Настройки сохранены» в меню */

/* Состояние меню. */
int  dc_menu_is_open(void);
void dc_menu_toggle(void);
void dc_menu_close(void);

/* Навигация. Коды — DC_MENU_UP и ниже. Возвращает 1, если событие съедено. */
enum {
    DC_MENU_UP = 1, DC_MENU_DOWN, DC_MENU_LEFT, DC_MENU_RIGHT,
    DC_MENU_OK, DC_MENU_BACK, DC_MENU_PAGE_PREV, DC_MENU_PAGE_NEXT,
    DC_MENU_BIND_ADD, DC_MENU_BIND_CLEAR            /* 04.10 (6): на строке назначения — добавить вход, очистить */
};
int  dc_menu_input(int action);

/* ---- подстраницы и назначения кнопок (04.10 вечер, 6) ---------------------------------------
 * Подстраница — строки с номером страницы за видимыми вкладками (6 — клавиатура и мышь, 7 — геймпад); открывает её
 * строка-кнопка вкладки CONTROLS, «назад» возвращает на вкладку. Лента вкладок при этом подсвечивает родителя. */
void dc_menu_set_subpages(const char **titles, int first_page, int count);
void dc_menu_open_sub(int page);
int  dc_menu_page_parent(void);                     /* вкладка для ленты: на подстранице — родитель, иначе текущая */
const char *dc_menu_sub_title(void);                /* заголовок подстраницы (ключ) или NULL */
/* Ожидание новой клавиши/кнопки для строки назначения: 0 — не ждём, иначе 1 + устройство (DCB_KB / DCB_PAD).
 * Вызывающий отдаёт пойманный вход в dc_menu_capture_input (0 — отмена: Esc или ожидание вышло). */
int  dc_menu_capturing(void);
int  dc_menu_capture_add(void);                     /* ждём «добавить», а не «заменить» */
void dc_menu_capture_input(int code);
/* свои строки dc_user.cfg (назначения кнопок): чтение — строки, которых нет в таблице опций; запись — после опций */
void dc_menu_set_cfg_ext(int (*read)(const char *key, const char *val), void (*write)(FILE *f));
/* строка DC_OPT_DEVICE: число подключённых устройств и подпись устройства slot (0 — Auto) — ключ атласа */
void dc_menu_set_dev_hooks(int (*count)(void), const char *(*label)(int slot));

/* Рисует меню в наш слой. Вызывать раз в кадр перед выводом слоя. */
void dc_menu_draw(void);

/* Мышь. Координаты — в системе нашего слоя (640x480), их считает вызывающий.
 * click: 0 — только навели, 1 — щёлкнули левой. wheel: прокрутка (+вверх).
 * Возвращает 1, если событие обработано меню. */
int  dc_menu_mouse(int x, int y, int click, int wheel);

/* Сообщение в нижней строке меню на ~3 с (переводится через dc_tr). */
void dc_menu_note(const char *text_en);

/* ---- меню в стиле экранов запуска (10.09.2026) ------------------------------
 * Игрок: «сделай стиль меню опций наших полностью таким же, как окна раскладки
 * и выбора языка». Рисует его dc_splash.cpp из атласа строк (ui_menu_<код>.png):
 * отсюда он берёт состояние меню, сюда отдаёт мышь. Строки — АНГЛИЙСКИЕ ключи,
 * как в таблице опций: по ним ищется картинка строки на текущем языке. */
typedef struct {
    int          opt;           /* индекс в таблице опций */
    DcOptType    type;
    const char  *title;         /* ключ подписи */
    const char  *hint;          /* ключ пояснения или NULL */
    const char  *value_key;     /* ключ значения: выбор ENUM, "On"/"Off"; NULL — число или действие */
    char         value_num[24]; /* текст числа (INT/FLOAT) */
    const char **choices;       /* ENUM: все значения — колонка держится под самое широкое */
    int          bind_dev;      /* BIND: устройство */
    int          bind_n;        /* BIND: входов назначено */
    int          bind_code[4];  /* BIND: коды входов (dc_binds.h) — для значков геймпада */
    const char  *bind_lbl[4];   /* BIND: подписи входов — ключи атласа */
    int          capturing;     /* BIND: эта строка ждёт новую клавишу/кнопку */
    float        num_v, num_lo, num_hi;   /* 11.10: INT/FLOAT - значение и пределы для ползунка */
} DcMenuRow;
int  dc_menu_page(void);
int  dc_menu_page_count(void);
const char *dc_menu_page_key(int page);
int  dc_menu_rows(DcMenuRow *out, int max);   /* строки текущей вкладки по порядку */
int  dc_menu_sel_opt(void);                   /* выбранная строка — индекс в таблице */
int  dc_menu_sel_type(void);                  /* 04.10 (6): тип выбранной строки (DcOptType) или -1 */
const char *dc_menu_note_key(void);           /* сообщение (ключ) или NULL; звать раз в кадр */
void dc_menu_hover(int opt);                  /* мышь навели на строку */
void dc_menu_click(int opt, int dir);         /* щелчок: dir -1/+1 — стрелки, 0 — по подписи */
/* 11.10.2026 (игрок: «настройки камеры все ползунками; у каждого раздела кнопку - сбросить до дефолта»):
 * значение ползунка (мышь по полосе: шаг строки, пределы lo..hi) и сброс вкладки к умолчаниям установки */
void dc_menu_set_value(int opt, float v);
void dc_menu_reset_page(int page);
void dc_menu_set_page(int page);
void dc_menu_set_dev(int pad);                /* чем игрок пользуется — под это подсказки внизу */
int  dc_menu_dev(void);

#ifdef __cplusplus
}
#endif
#endif /* DC_SETTINGS_H */
