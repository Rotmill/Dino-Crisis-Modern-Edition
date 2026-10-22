/* dc_binds — назначения кнопок: шаблоны Original / Modern и своя раскладка. См. dc_binds.h. */
#include "dc_binds.h"
#include "dc_splash.h"            /* DCS_* — значки кнопок схемы геймпада */

#include <SDL.h>

#include <stdlib.h>
#include <string.h>

extern int g_dc_orig_mode;        /* memory.c */

/* ⭐ 10.10.2026 (игрок: «сделай нормальное современное управление на клавиатуре с мышью в оригинальном режиме - как в
 * современных играх с танковым управлением и фиксированными камерами… настроить раскладку клавиатуры с мышью
 * оригинального режима отдельно, современного - отдельно, и то же самое для геймпада»): шаблон теперь у КАЖДОЙ пары
 * «тип игры + устройство» свой, своя раскладка - тоже. По умолчанию: наш режим - Modern на обоих устройствах;
 * оригинальный - клавиатура и мышь Modern (танковая, см. tpl_get), геймпад Original (PS1 один в один).
 * DC_ORIG_KB_MODERN=0 - клавиатура оригинального режима по умолчанию Original (PC-версия 2000 года), как было. */
/* ⭐ 15.10.2026 (игрок: «приведи в порядок раскладки для клавиатуры с мышью: то, что мы расписали в типе игры Original при
 * Button Layout Modern, нужно считать правильной раскладкой»): у клавиатуры в ОБОИХ типах игры Original - эта классическая
 * раскладка (WASD и стрелки, Shift, Caps Lock, Q/X, мышь; см. tpl_get), Modern - современная (WASD, Shift/пробел, X, мышь,
 * Q - предметы; в оригинальном типе ещё и смена цели колесом, его кнопкой и B). Клавиши PC-версии 2000 года убраны.
 * По умолчанию: наш тип - Modern, оригинальный - Original (строка хранения клавиатуры оригинального типа переименована:
 * у всех игроков там стоял прежний Modern = эта же раскладка, main.cpp inp.layout.o.kb2). Геймпад - как был.
 * DC_KB_LAYOUT_1015=0 - шаблоны клавиатуры и умолчание как до 15.10. */
static int      s_layout[2][2];                                   /* [тип игры][устройство] — DCB_TPL_* */
static int      s_have[2][2];                                     /* своя раскладка [тип игры][устройство] заведена */
static int      s_cust[2][2][DCB_COUNT][DCB_MAX];                 /* [тип игры][устройство][действие][вход] */
static unsigned s_serial = 1;

static int kb_1015(void) {
    static int v = -1;
    if (v < 0) { const char *e = SDL_getenv("DC_KB_LAYOUT_1015"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
int dcb_kb_layout_1015(void) { return kb_1015(); }

int dcb_default_tpl(int mode, int dev) {
    if (mode != DCB_ORIG) return DCB_TPL_MODERN;
    if (dev == DCB_PAD) return DCB_TPL_ORIG;
    if (kb_1015()) return DCB_TPL_ORIG;                           /* 15.10: Original клавиатуры = классическая раскладка */
    static int okb = -1;
    if (okb < 0) { const char *e = SDL_getenv("DC_ORIG_KB_MODERN"); okb = (e && *e == '0') ? 0 : 1; }
    return okb ? DCB_TPL_MODERN : DCB_TPL_ORIG;
}
static void lay_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    for (int m = 0; m < 2; m++)
        for (int d = 0; d < 2; d++) s_layout[m][d] = dcb_default_tpl(m ? DCB_ORIG : DCB_MODERN, d);
}

int dcb_mode(void) { return g_dc_orig_mode ? DCB_ORIG : DCB_MODERN; }
static int s_edit_mode = -1;
int  dcb_edit_mode(void) { return s_edit_mode < 0 ? dcb_mode() : s_edit_mode; }
void dcb_set_edit_mode(int mode) { s_edit_mode = (mode < 0) ? -1 : (mode == DCB_ORIG ? DCB_ORIG : DCB_MODERN); }
unsigned dcb_serial(void) { return s_serial; }

/* бамперы повторяют курки (15.09 ночь, игрок; DC_SHOULDER_DUP=0 — бампер L1 = смена цели, как было до того) */
static int shoulder_dup(void) {
    static int v = -1;
    if (v < 0) { const char *e = SDL_getenv("DC_SHOULDER_DUP"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

#define K(sc)   SDL_SCANCODE_##sc
#define PB(b)   DCB_PADB(SDL_CONTROLLER_BUTTON_##b)
#define ML      DCB_MOUSE(SDL_BUTTON_LEFT)
#define MR      DCB_MOUSE(SDL_BUTTON_RIGHT)

/* Шаблон типа игры mode: tpl — DCB_TPL_ORIG / DCB_TPL_MODERN, dev — устройство, act — действие. */
static int tpl_get(int mode, int tpl, int dev, int act, int *o) {
    int n = 0;
#define ADD(c) do { if (n < DCB_MAX) o[n++] = (c); } while (0)
    if (dev == DCB_KB && kb_1015()) {
        /* ⭐ 15.10.2026: клавиатура - два шаблона на оба типа игры (см. выше). Смена цели (L1 игры) - только в оригинальном
         * типе: в нашем цель - под перекрестием, а колесо там двигает камеру (dc_nots_zoom) */
        const int orig = (mode == DCB_ORIG);
        if (tpl == DCB_TPL_ORIG) {                             /* классическая: прежний Modern оригинального типа */
            switch (act) {
            case DCB_FWD: ADD(K(W)); ADD(K(UP)); break;          case DCB_BACK: ADD(K(S)); ADD(K(DOWN)); break;
            case DCB_LEFT: ADD(K(A)); ADD(K(LEFT)); break;       case DCB_RIGHT: ADD(K(D)); ADD(K(RIGHT)); break;
            case DCB_RUN: ADD(DCB_SHIFT); break;                  case DCB_RUN_TOGGLE: ADD(K(CAPSLOCK)); break;
            case DCB_QUICKTURN: ADD(K(Q)); ADD(K(X)); break;
            case DCB_AIM: ADD(MR); break;                         case DCB_FIRE: ADD(ML); break;
            case DCB_TARGET: if (orig) { ADD(DCB_WHEEL_UP); ADD(DCB_WHEEL_DOWN); ADD(DCB_MOUSE(SDL_BUTTON_MIDDLE)); ADD(K(B)); } break;
            case DCB_ACTION: ADD(K(E)); ADD(K(F)); ADD(K(RETURN)); break;
            case DCB_INVENTORY: ADD(K(TAB)); ADD(K(I)); ADD(K(M)); break;
            case DCB_PAUSE: ADD(K(P)); break;                     case DCB_OPTIONS: ADD(K(BACKSPACE)); break;
            default: break;                                       /* отмена (Esc - всегда) */
            }
        } else {                                               /* современная: ремейки RE (как у нашего типа до 15.10) */
            switch (act) {
            case DCB_FWD: ADD(K(W)); break;        case DCB_BACK: ADD(K(S)); break;
            case DCB_LEFT: ADD(K(A)); break;       case DCB_RIGHT: ADD(K(D)); break;
            case DCB_RUN: ADD(DCB_SHIFT); ADD(K(SPACE)); break;
            case DCB_QUICKTURN: ADD(K(X)); break;
            case DCB_AIM: ADD(MR); break;          case DCB_FIRE: ADD(ML); break;
            case DCB_TARGET: if (orig) { ADD(DCB_WHEEL_UP); ADD(DCB_WHEEL_DOWN); ADD(DCB_MOUSE(SDL_BUTTON_MIDDLE)); ADD(K(B)); } break;
            case DCB_ACTION: ADD(K(E)); ADD(K(F)); ADD(K(RETURN)); break;
            case DCB_INVENTORY: ADD(K(TAB)); ADD(K(I)); ADD(K(M)); ADD(K(Q)); break;
            case DCB_PAUSE: ADD(K(P)); break;      case DCB_OPTIONS: ADD(K(BACKSPACE)); break;
            default: break;                         /* бег переключением, отмена (Esc - всегда) */
            }
        }
        return n;
    }
    if (tpl == DCB_TPL_MODERN && dev == DCB_KB && mode == DCB_ORIG) {
        /* ⭐ 10.10.2026: ОРИГИНАЛЬНЫЙ ТИП, КЛАВИАТУРА И МЫШЬ - современная танковая раскладка, как в нынешних играх с
         * танковым управлением и неподвижными камерами (RE HD Remaster, Tormented Souls, Crow Country; разбор - в
         * памяти dc_controls_orig_modern): WASD и стрелки - вперёд/назад и повороты, Shift - бег, Caps Lock - бег
         * переключением, ПКМ держать - прицел, ЛКМ - выстрел (без прицела - действие, как ✕ у PS1), смена цели - колесо
         * мыши (Crow Country), его кнопка и B (PC-версия 2000), разворот - Q (ремейки Capcom) и X (как в нашем режиме),
         * действие - E/F/Enter, инвентарь - Tab/I/M, пауза - P, меню OPTION - Backspace (и Esc - всегда) */
        switch (act) {
        case DCB_FWD: ADD(K(W)); ADD(K(UP)); break;          case DCB_BACK: ADD(K(S)); ADD(K(DOWN)); break;
        case DCB_LEFT: ADD(K(A)); ADD(K(LEFT)); break;       case DCB_RIGHT: ADD(K(D)); ADD(K(RIGHT)); break;
        case DCB_RUN: ADD(DCB_SHIFT); break;                  case DCB_RUN_TOGGLE: ADD(K(CAPSLOCK)); break;
        case DCB_QUICKTURN: ADD(K(Q)); ADD(K(X)); break;
        case DCB_AIM: ADD(MR); break;                         case DCB_FIRE: ADD(ML); break;
        case DCB_TARGET: ADD(DCB_WHEEL_UP); ADD(DCB_WHEEL_DOWN); ADD(DCB_MOUSE(SDL_BUTTON_MIDDLE)); ADD(K(B)); break;
        case DCB_ACTION: ADD(K(E)); ADD(K(F)); ADD(K(RETURN)); break;
        case DCB_INVENTORY: ADD(K(TAB)); ADD(K(I)); ADD(K(M)); break;
        case DCB_PAUSE: ADD(K(P)); break;                     case DCB_OPTIONS: ADD(K(BACKSPACE)); break;
        default: break;                                       /* бег переключением, отмена (Esc - всегда) */
        }
    } else if (tpl == DCB_TPL_MODERN && dev == DCB_PAD && mode == DCB_ORIG) {
        /* 10.10: оригинальный тип, геймпад Modern - как наш Modern, но камеры правого стика нет, а смена цели нужна:
         * левый курок и бампер - прицел, правый курок - выстрел, правый бампер - смена цели */
        switch (act) {
        case DCB_FWD: ADD(PB(DPAD_UP)); break;     case DCB_BACK: ADD(PB(DPAD_DOWN)); break;
        case DCB_LEFT: ADD(PB(DPAD_LEFT)); break;  case DCB_RIGHT: ADD(PB(DPAD_RIGHT)); break;
        case DCB_RUN: ADD(PB(X)); break;           case DCB_RUN_TOGGLE: ADD(PB(LEFTSTICK)); break;
        case DCB_QUICKTURN: ADD(PB(RIGHTSTICK)); break;
        case DCB_AIM: ADD(DCB_PADT(0)); ADD(PB(LEFTSHOULDER)); break;
        case DCB_FIRE: ADD(DCB_PADT(1)); break;
        case DCB_TARGET: ADD(PB(RIGHTSHOULDER)); break;
        case DCB_ACTION: ADD(PB(A)); break;
        case DCB_INVENTORY: ADD(PB(Y)); ADD(PB(B)); break;
        case DCB_CANCEL: ADD(PB(B)); break;
        case DCB_PAUSE: ADD(PB(START)); break;     case DCB_OPTIONS: ADD(PB(BACK)); break;
        default: break;
        }
    } else if (tpl == DCB_TPL_MODERN && dev == DCB_KB) {      /* раскладка до 04.10: ремейки RE */
        switch (act) {
        case DCB_FWD: ADD(K(W)); break;        case DCB_BACK: ADD(K(S)); break;
        case DCB_LEFT: ADD(K(A)); break;       case DCB_RIGHT: ADD(K(D)); break;
        case DCB_RUN: ADD(DCB_SHIFT); ADD(K(SPACE)); break;
        case DCB_QUICKTURN: ADD(K(X)); break;
        case DCB_AIM: ADD(MR); break;          case DCB_FIRE: ADD(ML); break;
        case DCB_ACTION: ADD(K(E)); ADD(K(F)); ADD(K(RETURN)); break;
        case DCB_INVENTORY: ADD(K(TAB)); ADD(K(I)); ADD(K(M)); ADD(K(Q)); break;
        case DCB_PAUSE: ADD(K(P)); break;      case DCB_OPTIONS: ADD(K(BACKSPACE)); break;
        default: break;                         /* бег переключением, смена цели, отмена (Esc — всегда) */
        }
    } else if (tpl == DCB_TPL_MODERN) {                        /* геймпад до 04.10 */
        switch (act) {
        case DCB_FWD: ADD(PB(DPAD_UP)); break;     case DCB_BACK: ADD(PB(DPAD_DOWN)); break;
        case DCB_LEFT: ADD(PB(DPAD_LEFT)); break;  case DCB_RIGHT: ADD(PB(DPAD_RIGHT)); break;
        case DCB_RUN: ADD(PB(X)); break;           case DCB_RUN_TOGGLE: ADD(PB(LEFTSTICK)); break;
        case DCB_QUICKTURN: ADD(PB(RIGHTSTICK)); break;
        case DCB_AIM: ADD(DCB_PADT(0)); if (shoulder_dup()) ADD(PB(LEFTSHOULDER)); break;
        case DCB_FIRE: ADD(DCB_PADT(1)); if (shoulder_dup()) ADD(PB(RIGHTSHOULDER)); break;
        case DCB_TARGET: if (!shoulder_dup()) ADD(PB(LEFTSHOULDER)); break;
        case DCB_ACTION: ADD(PB(A)); break;
        case DCB_INVENTORY: ADD(PB(Y)); ADD(PB(B)); break;   /* B: в мире — инвентарь, на экранах игры — назад */
        case DCB_CANCEL: ADD(PB(B)); break;
        case DCB_PAUSE: ADD(PB(START)); break;     case DCB_OPTIONS: ADD(PB(BACK)); break;
        default: break;
        }
    } else if (dev == DCB_KB) {                                /* PC-версия 2000 года */
        switch (act) {
        case DCB_FWD: ADD(K(UP)); ADD(K(KP_8)); break;       case DCB_BACK: ADD(K(DOWN)); ADD(K(KP_2)); break;
        case DCB_LEFT: ADD(K(LEFT)); ADD(K(KP_4)); break;    case DCB_RIGHT: ADD(K(RIGHT)); ADD(K(KP_6)); break;
        case DCB_RUN: ADD(K(V)); break;
        case DCB_QUICKTURN: ADD(K(D)); break;
        case DCB_AIM: ADD(K(X)); if (mode == DCB_MODERN) ADD(MR); break;     /* в нашем режиме — и мышью */
        case DCB_FIRE: ADD(K(C)); if (mode == DCB_MODERN) ADD(ML); break;
        case DCB_TARGET: if (mode == DCB_ORIG) ADD(K(B)); break;            /* в нашем режиме цель — под перекрестием */
        case DCB_ACTION: ADD(K(C)); ADD(K(RETURN)); break;
        case DCB_INVENTORY: ADD(K(Z)); break;
        case DCB_PAUSE: ADD(K(A)); break;          case DCB_OPTIONS: ADD(DCB_CTRL); break;
        default: break;
        }
    } else {                                                   /* геймпад PS1 один в один */
        switch (act) {
        case DCB_FWD: ADD(PB(DPAD_UP)); break;     case DCB_BACK: ADD(PB(DPAD_DOWN)); break;
        case DCB_LEFT: ADD(PB(DPAD_LEFT)); break;  case DCB_RIGHT: ADD(PB(DPAD_RIGHT)); break;
        case DCB_RUN: ADD(PB(X)); break;                       /* □ */
        case DCB_QUICKTURN: ADD(DCB_PADT(1)); break;           /* R2 */
        case DCB_AIM: ADD(PB(RIGHTSHOULDER)); break;           /* R1 */
        case DCB_FIRE: ADD(PB(A)); break;                      /* ✕ при прицеле */
        case DCB_TARGET: if (mode == DCB_ORIG) ADD(PB(LEFTSHOULDER)); break;   /* L1 */
        case DCB_ACTION: ADD(PB(A)); break;                    /* ✕ */
        case DCB_INVENTORY: ADD(PB(B)); break;                 /* ○ */
        case DCB_CANCEL: ADD(PB(Y)); break;                    /* △ */
        case DCB_PAUSE: ADD(PB(START)); break;     case DCB_OPTIONS: ADD(PB(BACK)); break;
        default: break;
        }
    }
#undef ADD
    return n;
}

int dcb_layout_dev(int mode, int dev) { lay_init(); return s_layout[mode == DCB_ORIG ? 1 : 0][dev == DCB_PAD ? 1 : 0]; }
int dcb_layout(int mode) { return dcb_layout_dev(mode, DCB_KB); }

static const char *tpl_name(int t) { return t == DCB_TPL_ORIG ? "Original" : t == DCB_TPL_MODERN ? "Modern" : "Custom"; }

/* своя раскладка типа игры и устройства — копией шаблона, с которого начинали (если её ещё нет) */
static void cust_init(int m, int d, int from_tpl) {
    if (s_have[m][d]) return;
    for (int a = 0; a < DCB_COUNT; a++) {
        int c[DCB_MAX] = { 0 };
        tpl_get(m, from_tpl, d, a, c);
        memcpy(s_cust[m][d][a], c, sizeof c);
    }
    s_have[m][d] = 1;
}

void dcb_set_layout_dev(int mode, int dev, int tpl) {
    lay_init();
    const int m = (mode == DCB_ORIG) ? 1 : 0, d = (dev == DCB_PAD) ? 1 : 0;
    if (tpl < DCB_TPL_ORIG || tpl > DCB_TPL_CUSTOM) tpl = dcb_default_tpl(mode, dev);
    if (tpl == DCB_TPL_CUSTOM) cust_init(m, d, s_layout[m][d] == DCB_TPL_CUSTOM ? dcb_default_tpl(mode, dev) : s_layout[m][d]);
    if (s_layout[m][d] != tpl) {
        s_layout[m][d] = tpl; s_serial++;
        fprintf(stderr, "binds: %s mode %s layout -> %s\n", m ? "original" : "modern", d ? "gamepad" : "keyboard", tpl_name(tpl));
    }
}
void dcb_set_layout(int mode, int tpl) { dcb_set_layout_dev(mode, DCB_KB, tpl); dcb_set_layout_dev(mode, DCB_PAD, tpl); }

int dcb_get(int mode, int dev, int act, int *out) {
    lay_init();
    const int m = (mode == DCB_ORIG) ? 1 : 0;
    if (act < 0 || act >= DCB_COUNT || dev < 0 || dev > 1) return 0;
    if (s_layout[m][dev] == DCB_TPL_CUSTOM && s_have[m][dev]) {
        int n = 0;
        for (int k = 0; k < DCB_MAX; k++) if (s_cust[m][dev][act][k]) out[n++] = s_cust[m][dev][act][k];
        return n;
    }
    return tpl_get(m, s_layout[m][dev] == DCB_TPL_CUSTOM ? dcb_default_tpl(mode, dev) : s_layout[m][dev], dev, act, out);
}

int dcb_norm(int code) {
    switch (code) {
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return DCB_SHIFT;
    case SDL_SCANCODE_LCTRL:  case SDL_SCANCODE_RCTRL:  return DCB_CTRL;
    case SDL_SCANCODE_LALT:   case SDL_SCANCODE_RALT:   return DCB_ALT;
    default: return code;
    }
}

/* пары, которые шаблоны делят намеренно: ✕ — действие и выстрел, B — инвентарь в мире и «назад» на экранах */
static int shared_pair(int a, int b) {
    return (a == DCB_ACTION && b == DCB_FIRE) || (a == DCB_FIRE && b == DCB_ACTION) ||
           (a == DCB_INVENTORY && b == DCB_CANCEL) || (a == DCB_CANCEL && b == DCB_INVENTORY);
}

/* правка: тип игры и устройство уходят в свою раскладку (копия шаблона, с которого начинали) */
static int cust_begin(int m, int d) {
    lay_init();
    if (s_layout[m][d] != DCB_TPL_CUSTOM) {
        s_have[m][d] = 0;
        cust_init(m, d, s_layout[m][d]);
        s_layout[m][d] = DCB_TPL_CUSTOM;
        fprintf(stderr, "binds: %s mode %s layout -> Custom (copied from the template)\n", m ? "original" : "modern",
                d ? "gamepad" : "keyboard");
    } else cust_init(m, d, dcb_default_tpl(m ? DCB_ORIG : DCB_MODERN, d));
    return m;
}

int dcb_assign(int mode, int dev, int act, int code, int add) {
    if (act < 0 || act >= DCB_COUNT || dev < 0 || dev > 1 || !dcb_code_ok(dev, code)) return -1;
    const int m = cust_begin(mode == DCB_ORIG ? 1 : 0, dev);
    code = dcb_norm(code);
    int moved = -1;
    for (int a = 0; a < DCB_COUNT; a++) {                 /* тот же вход у другого действия — снять */
        if (a == act || shared_pair(a, act)) continue;
        int *c = s_cust[m][dev][a];
        for (int k = 0; k < DCB_MAX; k++) if (c[k] == code) {
            for (int j = k; j < DCB_MAX - 1; j++) c[j] = c[j + 1];
            c[DCB_MAX - 1] = 0; moved = a; k--;
        }
    }
    int *c = s_cust[m][dev][act];
    if (!add) {
        memset(c, 0, sizeof(int) * DCB_MAX); c[0] = code;
    } else {
        int have = 0, n = 0;
        for (int k = 0; k < DCB_MAX; k++) { if (c[k] == code) have = 1; if (c[k]) n++; }
        if (!have) {
            if (n >= DCB_MAX) { for (int j = 0; j < DCB_MAX - 1; j++) c[j] = c[j + 1]; n = DCB_MAX - 1; }
            c[n] = code;
        }
    }
    s_serial++;
    fprintf(stderr, "binds: %s %s %s %s 0x%03X%s\n", m ? "original" : "modern", dev ? "pad" : "kb", dcb_act_key(act),
            add ? "+" : "=", code, moved >= 0 ? " (taken from another action)" : "");
    return moved;
}

void dcb_clear(int mode, int dev, int act) {
    if (act < 0 || act >= DCB_COUNT || dev < 0 || dev > 1) return;
    const int m = cust_begin(mode == DCB_ORIG ? 1 : 0, dev);
    memset(s_cust[m][dev][act], 0, sizeof(int) * DCB_MAX);
    s_serial++;
}

/* 10.10: «Restore Defaults» — назад к шаблону этого типа игры и устройства по умолчанию (а не в копию шаблона) */
void dcb_restore(int mode, int dev) {
    if (dev < 0 || dev > 1) return;
    dcb_set_layout_dev(mode, dev, dcb_default_tpl(mode, dev));
    s_serial++;
    fprintf(stderr, "binds: %s mode %s back to the default layout (%s)\n", mode == DCB_ORIG ? "original" : "modern",
            dev ? "gamepad" : "keyboard", tpl_name(dcb_default_tpl(mode, dev)));
}

/* ---- входы: что можно назначить, подписи -------------------------------------------------------- */

typedef struct { int code; const char *label; } CodeLbl;
static const CodeLbl k_lbl[] = {
    { K(A), "A" }, { K(B), "B" }, { K(C), "C" }, { K(D), "D" }, { K(E), "E" }, { K(F), "F" }, { K(G), "G" },
    { K(H), "H" }, { K(I), "I" }, { K(J), "J" }, { K(K), "K" }, { K(L), "L" }, { K(M), "M" }, { K(N), "N" },
    { K(O), "O" }, { K(P), "P" }, { K(Q), "Q" }, { K(R), "R" }, { K(S), "S" }, { K(T), "T" }, { K(U), "U" },
    { K(V), "V" }, { K(W), "W" }, { K(X), "X" }, { K(Y), "Y" }, { K(Z), "Z" },
    { K(1), "1" }, { K(2), "2" }, { K(3), "3" }, { K(4), "4" }, { K(5), "5" }, { K(6), "6" }, { K(7), "7" },
    { K(8), "8" }, { K(9), "9" }, { K(0), "0" },
    { K(SPACE), "Space" }, { K(RETURN), "Enter" }, { K(TAB), "Tab" }, { K(BACKSPACE), "Backspace" },
    { DCB_SHIFT, "Shift" }, { DCB_CTRL, "Ctrl" }, { DCB_ALT, "Alt" }, { K(CAPSLOCK), "Caps Lock" },
    { K(UP), "\xE2\x86\x91" }, { K(DOWN), "\xE2\x86\x93" }, { K(LEFT), "\xE2\x86\x90" }, { K(RIGHT), "\xE2\x86\x92" },
    { K(HOME), "Home" }, { K(END), "End" },
    { K(KP_0), "Num 0" }, { K(KP_1), "Num 1" }, { K(KP_2), "Num 2" }, { K(KP_3), "Num 3" }, { K(KP_4), "Num 4" },
    { K(KP_5), "Num 5" }, { K(KP_6), "Num 6" }, { K(KP_7), "Num 7" }, { K(KP_8), "Num 8" }, { K(KP_9), "Num 9" },
    { K(KP_PLUS), "Num +" }, { K(KP_MINUS), "Num -" }, { K(KP_MULTIPLY), "Num *" }, { K(KP_DIVIDE), "Num /" },
    { K(KP_PERIOD), "Num ." }, { K(KP_ENTER), "Num Enter" },
    { K(MINUS), "-" }, { K(EQUALS), "=" }, { K(LEFTBRACKET), "[" }, { K(RIGHTBRACKET), "]" }, { K(SEMICOLON), ";" },
    { K(APOSTROPHE), "'" }, { K(COMMA), "," }, { K(PERIOD), "." }, { K(SLASH), "/" }, { K(BACKSLASH), "\\" },
    { K(GRAVE), "`" },
    { ML, "LMB" }, { MR, "RMB" }, { DCB_MOUSE(SDL_BUTTON_MIDDLE), "MMB" },
    { DCB_MOUSE(SDL_BUTTON_X1), "Mouse 4" }, { DCB_MOUSE(SDL_BUTTON_X2), "Mouse 5" },
    { DCB_WHEEL_UP, "Wheel Up" }, { DCB_WHEEL_DOWN, "Wheel Down" },          /* 10.10 */
    /* геймпад: значки схемы, где они есть (dcb_code_pad_icon); подпись — для крестовины рядом со значком и для Select */
    { PB(A), "A" }, { PB(B), "B" }, { PB(X), "X" }, { PB(Y), "Y" },
    { PB(LEFTSHOULDER), "L1" }, { PB(RIGHTSHOULDER), "R1" }, { DCB_PADT(0), "L2" }, { DCB_PADT(1), "R2" },
    { PB(LEFTSTICK), "L3" }, { PB(RIGHTSTICK), "R3" }, { PB(START), "Start" }, { PB(BACK), "Select" },
    { PB(DPAD_UP), "\xE2\x86\x91" }, { PB(DPAD_DOWN), "\xE2\x86\x93" }, { PB(DPAD_LEFT), "\xE2\x86\x90" },
    { PB(DPAD_RIGHT), "\xE2\x86\x92" }, { PB(TOUCHPAD), "Touchpad" },
};

const char *dcb_code_label(int code) {
    for (size_t i = 0; i < sizeof k_lbl / sizeof k_lbl[0]; i++) if (k_lbl[i].code == code) return k_lbl[i].label;
    return NULL;
}

int dcb_code_ok(int dev, int code) {
    code = dcb_norm(code);
    if (dev == DCB_PAD ? !DCB_IS_PAD(code) : DCB_IS_PAD(code)) return 0;
    return dcb_code_label(code) != NULL;            /* только входы из списка: у них есть подпись */
}

int dcb_code_pad_icon(int code) {
    switch (code) {
    case PB(A): return DCS_CRS;       case PB(B): return DCS_CIR;
    case PB(X): return DCS_SQR;       case PB(Y): return DCS_TRI;
    case PB(LEFTSHOULDER): return DCS_L1;   case PB(RIGHTSHOULDER): return DCS_R1;
    case DCB_PADT(0): return DCS_L2;  case DCB_PADT(1): return DCS_R2;
    case PB(LEFTSTICK): return DCS_L3;      case PB(RIGHTSTICK): return DCS_R3;
    case PB(START): return DCS_OPTIONS;     case PB(TOUCHPAD): return DCS_TOUCH;
    case PB(DPAD_UP): case PB(DPAD_DOWN): case PB(DPAD_LEFT): case PB(DPAD_RIGHT): return DCS_DPAD;
    default: return -1;
    }
}

static const char *const k_act_title[DCB_COUNT] = {
    "Move Forward", "Move Backward", "Move Left", "Move Right",
    "Run (Hold)", "Run (Toggle)", "Quick Turn",
    "Aim", "Fire", "Switch Target",
    "Action / Confirm", "Inventory", "Cancel", "Pause", "OPTION Menu",   /* = подписи строк подстраниц (атлас меню) */
};
static const char *const k_act_key[DCB_COUNT] = {
    "fwd", "back", "left", "right", "run", "runtoggle", "quickturn",
    "aim", "fire", "target", "action", "inventory", "cancel", "pause", "options",
};
const char *dcb_act_title(int act) { return (act >= 0 && act < DCB_COUNT) ? k_act_title[act] : ""; }
const char *dcb_act_key(int act)   { return (act >= 0 && act < DCB_COUNT) ? k_act_key[act] : "?"; }

/* ---- dc_user.cfg: bind.<m|o>.<kb|pad>.<действие>=0x1A,0x2C (коды через запятую) ------------------- */

int dcb_cfg_read(const char *key, const char *val) {
    if (strncmp(key, "bind.", 5) != 0) return 0;
    const char *p = key + 5;
    int m;
    if (p[0] == 'm' && p[1] == '.') m = 0; else if (p[0] == 'o' && p[1] == '.') m = 1; else return 1;
    p += 2;
    int dev;
    if (strncmp(p, "kb.", 3) == 0) { dev = DCB_KB; p += 3; }
    else if (strncmp(p, "pad.", 4) == 0) { dev = DCB_PAD; p += 4; }
    else return 1;
    int act = -1;
    for (int a = 0; a < DCB_COUNT; a++) if (strcmp(p, k_act_key[a]) == 0) act = a;
    if (act < 0) return 1;
    if (!s_have[m][dev]) { memset(s_cust[m][dev], 0, sizeof s_cust[m][dev]); s_have[m][dev] = 1; }   /* своя раскладка — из файла целиком */
    int *c = s_cust[m][dev][act], n = 0;
    memset(c, 0, sizeof(int) * DCB_MAX);
    const char *q = val;
    while (*q && n < DCB_MAX) {
        char *e = NULL;
        const long v = strtol(q, &e, 0);
        if (e == q) break;
        if (v > 0 && dcb_code_ok(dev, (int)v)) c[n++] = (int)v;
        q = e;
        while (*q == ',' || *q == ' ') q++;
    }
    s_serial++;
    return 1;
}

void dcb_cfg_write(FILE *f) {
    for (int m = 0; m < 2; m++) {
        for (int d = 0; d < 2; d++) {
            if (!s_have[m][d]) continue;
            for (int a = 0; a < DCB_COUNT; a++) {
                fprintf(f, "bind.%c.%s.%s=", m ? 'o' : 'm', d ? "pad" : "kb", k_act_key[a]);
                int first = 1;
                for (int k = 0; k < DCB_MAX; k++) if (s_cust[m][d][a][k]) {
                    fprintf(f, "%s0x%03X", first ? "" : ",", s_cust[m][d][a][k]); first = 0;
                }
                fprintf(f, "\n");
            }
        }
    }
}
