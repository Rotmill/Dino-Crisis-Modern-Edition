/* ==========================================================================
 * bios_hle.c — своё ядро PS1 (High-Level Emulation) вместо ретейл-BIOS.
 *
 * ПОЧЕМУ. Ретейл-образ SCPH1001 — собственность Sony, раздавать его с модом
 * нельзя, и игрок вынужден искать BIOS сам. Здесь ядро написано заново:
 * игра прыгает на вектор 0xA0/0xB0/0xC0, диспетчер отдаёт вызов сюда, и
 * функция выполняется на хосте. Образа BIOS не нужно вообще.
 *
 * ЧТО ИМЕННО НУЖНО ИГРЕ (замер, а не предположение). Скан стабов PsyQ в
 * SLUS_009.22 (`li $tN, 0xA0` + `jr`) дал 55 функций ядра: 18 через A0,
 * 34 через B0, 3 через C0. Функций привода среди них НЕТ — CD-драйвер у
 * Dino Crisis свой, поэтому самый тяжёлый кусок ядра нам не нужен.
 *
 * РАСКЛАДКА В RAM снята с работающей машины на ретейл-BIOS (quicksave →
 * дамп первых 64 КБ): таблицы стоят по тем же адресам, что и раньше, чтобы
 * и игра, и наши отладочные инструменты видели привычную картину.
 *
 * МЕХАНИКА ВОЗВРАТА. Стаб игры делает `jr $t2` — то есть $ra уже указывает
 * на код после вызова. Функция ядра кладёт результат в $v0 и обнуляет
 * cpu->pc: диспетчер выходит из цикла и управление возвращается в
 * рекомпилированную функцию игры, как после обычного вызова.
 * ======================================================================== */

#include "bios_hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);
extern void     psx_check_interrupts(CPUState* cpu);
extern void     psx_dispatch(CPUState* cpu, uint32_t addr);
extern void     psx_exception_longjmp(void);   /* traps.c: выход из обработчика */
extern void     dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);
extern int      psx_get_in_exception(void);
extern void     debug_server_poll(void);

/* --------------------------------------------------------------------------
 * Включение
 * ------------------------------------------------------------------------ */

static int s_enabled = -1;

int hle_bios_enabled(void) {
    if (s_enabled < 0) {
#ifdef PSX_HLE_BIOS
        s_enabled = 1;                    /* сборка psx-runtime-hle */
#else
        const char* e = getenv("DC_HLE_BIOS");
        s_enabled = (e && *e == '1');
#endif
    }
    return s_enabled;
}

/* --------------------------------------------------------------------------
 * Учёт вызовов: что игра позвала и сколько раз. Нереализованное считается
 * отдельно — по этому списку и достраивается ядро.
 * ------------------------------------------------------------------------ */

static uint32_t s_calls[3][256];
static uint32_t s_missing[3][256];
static const char* const VECNAME[3] = { "A0", "B0", "C0" };
static uint64_t s_exc_count = 0;      /* сколько раз ядро обслужило исключение */
static uint64_t s_exc_handled = 0;    /* сколько раз обработчик игры сказал «моё» */
static uint64_t s_deliver_hits = 0;   /* доставок, у которых нашёлся получатель */
static uint64_t s_deliver_miss = 0;   /* событий, которые никто не слушает */
static uint64_t s_hook_calls = 0;     /* вызовов обработчика от HookEntryInt */
static uint64_t s_rfe_calls = 0;      /* сколько раз игра штатно завершила исключение */

/* Таблица событий лежит в гостевой RAM: запись 0x1C байт
 *   +00 класс  +04 состояние  +08 спец  +0C режим  +10 обработчик */
#define EV_COUNT   (HLE_EVCB_SIZE / 0x1C)
#define EV_ADDR(i) (0x80000000u + HLE_EVCB_ADDR + (uint32_t)(i) * 0x1Cu)

/* Имена нужны только для отчёта: без них список номеров нечитаем. */
static const char* hle_name(int vec, uint32_t fn) {
    if (vec == 0) switch (fn) {
        case 0x10: return "atoi";        case 0x13: return "setjmp";
        case 0x14: return "longjmp";     case 0x17: return "strcmp";
        case 0x19: return "strcpy";      case 0x1B: return "strlen";
        case 0x28: return "bzero";       case 0x2A: return "memcpy";
        case 0x2B: return "memset";      case 0x2E: return "memchr";
        case 0x2F: return "rand";        case 0x30: return "srand";
        case 0x33: return "malloc";      case 0x34: return "free";
        case 0x39: return "InitHeap";    case 0x3F: return "printf";
        case 0x44: return "FlushCache";  case 0x49: return "GPU_cw";
        case 0x70: return "_bu_init";    case 0x72: return "CdRemove";
        case 0xAB: return "_card_info";  case 0xAC: return "_card_load_dir";
        default: return "?";
    }
    if (vec == 1) switch (fn) {
        case 0x07: return "DeliverEvent"; case 0x08: return "OpenEvent";
        case 0x09: return "CloseEvent";   case 0x0A: return "WaitEvent";
        case 0x0B: return "TestEvent";    case 0x0C: return "EnableEvent";
        case 0x0D: return "DisableEvent"; case 0x0E: return "OpenTh";
        case 0x0F: return "CloseTh";      case 0x10: return "ChangeTh";
        case 0x12: return "InitPAD";      case 0x13: return "StartPAD";
        case 0x14: return "StopPAD";      case 0x15: return "PAD_init";
        case 0x17: return "ReturnFromException";
        case 0x18: return "ResetEntryInt"; case 0x19: return "HookEntryInt";
        case 0x32: return "open";         case 0x33: return "lseek";
        case 0x34: return "read";         case 0x35: return "write";
        case 0x36: return "close";        case 0x38: return "exit";
        case 0x3F: return "puts";         case 0x42: return "firstfile";
        case 0x43: return "nextfile";     case 0x4A: return "InitCARD";
        case 0x4B: return "StartCARD";    case 0x4C: return "StopCARD";
        case 0x4E: return "_card_write_sector";
        case 0x4F: return "_card_read_sector";
        case 0x50: return "_new_card";    case 0x51: return "Krom2RawAdd";
        case 0x5B: return "ChangeClearPad"; case 0x5C: return "_card_status";
        default: return "?";
    }
    switch (fn) {
        case 0x02: return "SysEnqIntRP";  case 0x03: return "SysDeqIntRP";
        case 0x0A: return "ChangeClearRCnt";
        default: return "?";
    }
}

void hle_bios_report(void) {
    if (!hle_bios_enabled()) return;
    fprintf(stderr,
            "hle: summary (exceptions %llu, served by the game %llu, entries into the game's handler %llu, "
            "normal exits %llu; events: delivered %llu, nobody listening %llu)\n",
            (unsigned long long)s_exc_count, (unsigned long long)s_exc_handled,
            (unsigned long long)s_hook_calls, (unsigned long long)s_rfe_calls,
            (unsigned long long)s_deliver_hits, (unsigned long long)s_deliver_miss);
    for (int v = 0; v < 3; v++) {
        for (int f = 0; f < 256; f++) {
            if (!s_calls[v][f] && !s_missing[v][f]) continue;
            fprintf(stderr, "  %s(%02X) %-22s %8u%s\n",
                    VECNAME[v], f, hle_name(v, (uint32_t)f),
                    s_calls[v][f] + s_missing[v][f],
                    s_missing[v][f] ? "   <- NOT IMPLEMENTED" : "");
        }
    }
    /* Признак «точка входа игры сработала»: от него зависит скорость привода
     * (до него он держит 1x, после — игровую из game.toml) и ускорение
     * загрузок. В сборке со своим ядром бута нет, и если сигнал не пришёл,
     * привод останется на 1x — а это, по опыту проекта, ломает стейт-машину
     * загрузки комнаты и даёт чёрный экран после начала игры. */
    {
        extern int fntrace_is_game_started(void);
        extern int cdrom_streaming(void);
        fprintf(stderr, "  game started: %d, drive on a thread: %d\n",
                fntrace_is_game_started(), cdrom_streaming());
    }

    /* Последние вызванные функции игры. Это единственный надёжный след:
     * рекомпилированный код зовёт функции напрямую (не через диспетчер),
     * поэтому обычная трассировка их не видит, а это кольцо пишется в самом
     * начале каждой функции генкода. */
    {
        extern volatile uint32_t g_dc_fn_ring[32];
        extern volatile uint32_t g_dc_fn_pos;
        fprintf(stderr, "  last game functions:");
        for (int k = 1; k <= 24; k++)
            fprintf(stderr, " %08X", g_dc_fn_ring[(g_dc_fn_pos - (uint32_t)k) & 31u]);
        fprintf(stderr, "\n");
    }

    /* Открытые события: по ним видно, каких прерываний ждёт игра. */
    for (uint32_t i = 0; i < EV_COUNT; i++) {
        uint32_t e = EV_ADDR(i), st = psx_read_word(e + 4);
        if (st == 0) continue;
        fprintf(stderr, "  event %u: class %08X spec %04X mode %04X state %04X handler %08X\n",
                i, psx_read_word(e), psx_read_word(e + 8), psx_read_word(e + 0x0C),
                st, psx_read_word(e + 0x10));
    }
    fflush(stderr);
}

/* --------------------------------------------------------------------------
 * Мелкие помощники доступа к гостевой памяти
 * ------------------------------------------------------------------------ */

static uint8_t rb(CPUState* cpu, uint32_t a) { return cpu->read_byte(a); }
static void    wb(CPUState* cpu, uint32_t a, uint8_t v) { cpu->write_byte(a, v); }
static uint32_t rw(uint32_t a) { return psx_read_word(a); }
static void     ww(uint32_t a, uint32_t v) { psx_write_word(a, v); }

/* Строка гостя → буфер хоста (для printf/puts/open). */
static void guest_str(CPUState* cpu, uint32_t addr, char* out, size_t cap) {
    size_t i = 0;
    if (!addr) { snprintf(out, cap, "(null)"); return; }
    while (i + 1 < cap) {
        uint8_t c = rb(cpu, addr + (uint32_t)i);
        if (!c) break;
        out[i++] = (char)c;
    }
    out[i] = 0;
}

/* --------------------------------------------------------------------------
 * printf ядра. Игра печатает через него свою отладку («ResetGraph:jtb=...»),
 * поэтому форматирование стоит поддержать: без него не видно, что она
 * рассказывает о себе. Первые четыре аргумента в $a0..$a3, дальше стек.
 * ------------------------------------------------------------------------ */
static uint32_t va_arg_n(CPUState* cpu, int n) {
    if (n < 3) return cpu->gpr[5 + n];               /* $a1, $a2, $a3 */
    return rw(cpu->gpr[29] + 16u + (uint32_t)(n - 3) * 4u);
}

static void hle_printf(CPUState* cpu, uint32_t fmt_addr) {
    char fmt[512];
    guest_str(cpu, fmt_addr, fmt, sizeof fmt);
    char out[1024];
    size_t o = 0;
    int argn = 0;
    for (size_t i = 0; fmt[i] && o + 64 < sizeof out; i++) {
        if (fmt[i] != '%') { out[o++] = fmt[i]; continue; }
        char spec[32];
        size_t s = 0;
        spec[s++] = '%';
        i++;
        while (fmt[i] && s + 2 < sizeof spec &&
               !strchr("diouxXcspn%", fmt[i])) spec[s++] = fmt[i++];
        char conv = fmt[i] ? fmt[i] : '%';
        spec[s++] = conv;
        spec[s] = 0;
        if (conv == '%') { out[o++] = '%'; continue; }
        uint32_t v = va_arg_n(cpu, argn++);
        if (conv == 's') {
            char sv[256];
            guest_str(cpu, v, sv, sizeof sv);
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", sv);
        } else if (conv == 'c') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%c", (char)v);
        } else if (conv == 'd' || conv == 'i') {
            spec[s - 1] = 'd';
            o += (size_t)snprintf(out + o, sizeof out - o, spec, (int)v);
        } else {
            o += (size_t)snprintf(out + o, sizeof out - o, spec, (unsigned)v);
        }
    }
    out[o] = 0;
    fprintf(stderr, "hle/game: %s", out);
}

/* --------------------------------------------------------------------------
 * Таблицы ядра A0/B0/C0 в RAM. Игра берёт их адрес через GetB0Table /
 * GetC0Table и подменяет отдельные записи своими обработчиками — обычный
 * приём PsyQ. Поэтому таблицы должны существовать по-настоящему, а вызов
 * функции обязан сначала посмотреть, не подменена ли запись: если да,
 * управление уходит в код игры, а не в нашу реализацию.
 * ------------------------------------------------------------------------ */
#define TBL_A0   0x00000200u
#define TBL_B0   0x00000874u
#define TBL_C0   0x00000674u
/* Метка «запись не тронута». Адрес заведомо не исполняется игрой и своим
 * старшим байтом опознаётся мгновенно. */
#define TBL_MARK 0x5A5A0000u

static uint32_t table_addr(int vec) {
    return 0x80000000u + (vec == 0 ? TBL_A0 : vec == 1 ? TBL_B0 : TBL_C0);
}

static void tables_init(void) {
    for (int v = 0; v < 3; v++)
        for (uint32_t i = 0; i < 256; i++)
            ww(table_addr(v) + i * 4u, TBL_MARK | ((uint32_t)v << 8) | i);
}

/* Не подменила ли игра запись таблицы своим кодом. 0 = запись наша. */
static uint32_t table_override(int vec, uint32_t fn) {
    uint32_t v = rw(table_addr(vec) + (fn & 0xFFu) * 4u);
    return ((v & 0xFFFF0000u) == TBL_MARK) ? 0u : v;
}

/* --------------------------------------------------------------------------
 * События (EvCB). Запись 0x1C байт:
 *   +00 class  +04 status  +08 spec  +0C mode  +10 handler  +14/+18 резерв
 * status: 0 свободно, 0x1000 выключено, 0x2000 включено, 0x4000 доставлено.
 * mode:   0x1000 — вызвать handler, 0x2000 — только поднять флаг.
 * Дескриптор события у игры — 0xF1000000 | index.
 * ------------------------------------------------------------------------ */

#define EV_FREE      0x0000u
#define EV_DISABLED  0x1000u
#define EV_ENABLED   0x2000u
#define EV_DELIVERED 0x4000u

/* Ретейл берёт из дескриптора ТОЛЬКО младшие 16 бит и не проверяет ни тег
 * 0xF1, ни границы таблицы. Наша прежняя проверка тега заворачивала вызовы,
 * которые на консоли проходят. */
static int ev_index(uint32_t desc) {
    uint32_t i = desc & 0xFFFFu;
    return (i < EV_COUNT) ? (int)i : -1;
}

static uint32_t hle_OpenEvent(CPUState* cpu) {
    uint32_t cls = cpu->gpr[4], spec = cpu->gpr[5];
    uint32_t mode = cpu->gpr[6], func = cpu->gpr[7];
    for (uint32_t i = 0; i < EV_COUNT; i++) {
        uint32_t e = EV_ADDR(i);
        if (rw(e + 4) != EV_FREE) continue;
        ww(e + 0x00, cls);
        ww(e + 0x04, EV_DISABLED);
        ww(e + 0x08, spec);
        ww(e + 0x0C, mode);
        ww(e + 0x10, func);
        return 0xF1000000u | i;
    }
    fprintf(stderr, "hle: OpenEvent - the event table is full\n");
    return 0xFFFFFFFFu;
}

/* Закрытие, включение и выключение у ретейла возвращают 1 БЕЗУСЛОВНО. */
static uint32_t hle_CloseEvent(uint32_t desc) {
    int i = ev_index(desc);
    if (i >= 0) ww(EV_ADDR(i) + 4, EV_FREE);
    return 1;
}

static uint32_t hle_EnableEvent(uint32_t desc) {
    int i = ev_index(desc);
    if (i >= 0 && rw(EV_ADDR(i) + 4) != EV_FREE) ww(EV_ADDR(i) + 4, EV_ENABLED);
    return 1;
}

static uint32_t hle_DisableEvent(uint32_t desc) {
    int i = ev_index(desc);
    if (i >= 0 && rw(EV_ADDR(i) + 4) != EV_FREE) ww(EV_ADDR(i) + 4, EV_DISABLED);
    return 1;
}

static uint32_t hle_TestEvent(uint32_t desc) {
    int i = ev_index(desc);
    if (i < 0) return 0;
    if (rw(EV_ADDR(i) + 4) == EV_DELIVERED) {
        ww(EV_ADDR(i) + 4, EV_ENABLED);   /* проверка снимает флаг */
        return 1;
    }
    return 0;
}

/* DeliverEvent(class, spec): у события с таким классом и спецификацией либо
 * поднимаем флаг, либо зовём его обработчик — это код игры, поэтому вызов
 * идёт через диспетчер. */
/* Семантика снята с кода ретейл-BIOS (func_00001B44): обходится ВСЯ таблица;
 * берутся записи со статусом ровно «включено» и совпадением класса и
 * спецификации. Режим 0x2000 — поднять флаг «доставлено». Режим 0x1000 —
 * вызвать обработчик с $a0 = класс, $a1 = спецификация, СТАТУС НЕ МЕНЯТЬ.
 * Любой другой режим — не делать ничего. */
static void hle_DeliverEvent(CPUState* cpu, uint32_t cls, uint32_t spec) {
    int hit = 0;
    for (uint32_t i = 0; i < EV_COUNT; i++) {
        uint32_t e = EV_ADDR(i);
        if (rw(e + 4) != EV_ENABLED) continue;
        if (rw(e + 0x00) != cls || rw(e + 0x08) != spec) continue;
        uint32_t mode = rw(e + 0x0C);
        if (mode == 0x2000u) {
            hit = 1;
            ww(e + 4, EV_DELIVERED);
        } else if (mode == 0x1000u) {
            uint32_t func = rw(e + 0x10);
            if (func) {
                hit = 1;
                uint32_t save_ra = cpu->gpr[31];
                uint32_t save_a0 = cpu->gpr[4], save_a1 = cpu->gpr[5];
                cpu->gpr[4] = cls;
                cpu->gpr[5] = spec;
                psx_dispatch(cpu, func);
                cpu->gpr[31] = save_ra;
                cpu->gpr[4] = save_a0; cpu->gpr[5] = save_a1;
            }
        }
    }
    if (hit) s_deliver_hits++; else s_deliver_miss++;
}

/* WaitEvent крутится, пока прерывание не доставит событие. Ровно так же, как
 * на железе, только шаг «времени» здесь — проверка прерываний рантайма. */
static uint32_t hle_WaitEvent(CPUState* cpu, uint32_t desc) {
    int i = ev_index(desc);
    if (i < 0) return 0;
    uint32_t e = EV_ADDR(i);
    uint32_t st = rw(e + 4);

    /* Ровно три ветки ретейла (func_00001E44):
     *   доставлено   → снять флаг, вернуть 1;
     *   не «включено»→ вернуть 0 НЕМЕДЛЕННО, ничего не меняя;
     *   включено     → крутиться до доставки.
     * Прежняя версия крутилась в любом случае и через 100 млн витков
     * сдавалась — то есть и висла там, где консоль сразу отвечает «нет»,
     * и сдавалась там, где консоль ждёт до конца. */
    if (st == EV_DELIVERED) { ww(e + 4, EV_ENABLED); return 1; }
    if (st != EV_ENABLED) return 0;

    /* Изнутри обработчика прерывания ждать бессмысленно: доставка прерываний
     * там подавлена, событие прийти не может, а сторож рантайма через
     * 25 000 холостых заходов насильно размотает обработчик. */
    if (psx_get_in_exception()) return 0;

    for (unsigned long spins = 0;; spins++) {
        if (rw(e + 4) == EV_DELIVERED) {
            ww(e + 4, EV_ENABLED);
            return 1;
        }
        psx_check_interrupts(cpu);
        if ((spins & 0xFFFu) == 0xFFFu) debug_server_poll();
        if (spins == 30000000UL)
            fprintf(stderr,
                    "hle: WaitEvent waits long for %08X (class %08X spec %04X) from ra=%08X\n",
                    desc, rw(e + 0), rw(e + 8), cpu->gpr[31]);
    }
}

/* --------------------------------------------------------------------------
 * Цепочка обработчиков прерываний (SysEnqIntRP / HookEntryInt).
 * Элемент: +00 next, +04 handler, +08 verifier, +0C пусто.
 * Обработчик исключений идёт по цепочке: verifier решает «моё ли», handler
 * обслуживает. И то и другое — код игры.
 * ------------------------------------------------------------------------ */

#define IRQ_PRIO 4
static uint32_t s_irq_chain[IRQ_PRIO];   /* голова списка на каждый приоритет */
static uint32_t s_entry_hook = 0;        /* обработчик от HookEntryInt */
/* ⭐ 13.09.2026: эти головы и хук живут ЗДЕСЬ, у хозяина, а не в RAM гостя — в
 * savestate их нет. Игрок жал Del прямо на экране запуска: игра ещё не
 * стартовала, SysEnqIntRP/HookEntryInt не звались, после загрузки сейва
 * обработчик vblank libpad не в цепочке → пад никто не опрашивает
 * (sio_tx_writes = 0), «Регина не может ходить». Загрузка ждёт готовности
 * ядра (savestate_dispatch_hook). */
int hle_kernel_ready(void) {
    int chains = 0;
    for (int i = 0; i < IRQ_PRIO; i++) if (s_irq_chain[i]) chains++;
    return chains > 0 && s_entry_hook != 0;
}

static void hle_SysEnqIntRP(uint32_t prio, uint32_t elem) {
    if (prio >= IRQ_PRIO) prio = IRQ_PRIO - 1;
    ww(elem + 0, s_irq_chain[prio]);
    s_irq_chain[prio] = elem;
    fprintf(stderr, "hle: SysEnqIntRP prio=%u entry=%08X (handler=%08X verifier=%08X)\n",
            prio, elem, rw(elem + 4), rw(elem + 8));
}

static void hle_SysDeqIntRP(uint32_t prio, uint32_t elem) {
    if (prio >= IRQ_PRIO) prio = IRQ_PRIO - 1;
    uint32_t cur = s_irq_chain[prio], prev = 0;
    while (cur) {
        uint32_t next = rw(cur + 0);
        if (cur == elem) {
            if (prev) ww(prev + 0, next);
            else      s_irq_chain[prio] = next;
            return;
        }
        prev = cur;
        cur = next;
    }
}

/* Обработчик исключений. Вызывается вместо кода BIOS по адресу 0xC80:
 * рантайм (interrupts.c) уже сохранил регистры и обернул вызов в setjmp,
 * поэтому здесь остаётся только обойти цепочку. */
/* Системная часть обработчика — то, что в ретейл-BIOS делают его собственные
 * обработчики в цепочках (их адреса 0x6D88/0x6D98/0x6DA8 видны в эталонном
 * дампе). Без неё игра ждёт VSync вечно: событие вертикального гашения
 * приходить неоткуда. Классы событий: 0xF2000000+n — счётчик n, где 3 —
 * вертикальное гашение; 0xF0000003 — привод.  */
#define IRQ_VBLANK  (1u << 0)
#define IRQ_CDROM   (1u << 2)
#define IRQ_DMA     (1u << 3)
#define IRQ_RCNT0   (1u << 4)
#define IRQ_RCNT1   (1u << 5)
#define IRQ_RCNT2   (1u << 6)

/* Спецификации событий: у аппаратных классов 0xF00000xx ретейл-BIOS шлёт
 * ровно 0x1000, у классов счётчиков 0xF20000nn — 0x0002. Мы раньше слали
 * 0x0002 всем подряд, и игра не узнавала свои события. */
#define EVS_CNT     0x0002u
#define EVS_HW      0x1000u

/* Флаги автоподтверждения счётчиков (ChangeClearRCnt): ретейл держит их по
 * адресам 0x8600 + n*4 — 0 таймер, 1 таймер, 2 таймер, 3 вертикальное
 * гашение. Взведённый флаг означает «ядро само гасит бит и сразу выходит из
 * исключения» — тогда ни очередь игры, ни её обработчик не работают. Именно
 * поэтому Dino Crisis СНИМАЕТ флаг вертикального гашения. */
#define RCNT_CLEAR_FLAG(n) (0x80000000u + 0x8600u + (uint32_t)(n) * 4u)

static const struct { uint32_t bit; uint32_t cls; } RCNT_EVENTS[4] = {
    { 0x0010u, 0xF2000000u },   /* таймер 0 */
    { 0x0020u, 0xF2000001u },   /* таймер 1 */
    { 0x0040u, 0xF2000002u },   /* таймер 2 */
    { 0x0001u, 0xF2000003u },   /* вертикальное гашение */
};

/* Первая половина системной части — очередь ретейла с приоритетом 1: только
 * счётчики. Идёт ДО очереди игры (та висит на приоритете 2).
 * Возвращает 1, если сработало автоподтверждение: у ретейла в этом случае
 * обработка исключения на этом и заканчивается. */
static int hle_sys_counters(CPUState* cpu) {
    uint32_t act = rw(0x1F801070u) & rw(0x1F801074u);
    int stop = 0;
    for (int i = 0; i < 4; i++) {
        if (!(act & RCNT_EVENTS[i].bit)) continue;
        hle_DeliverEvent(cpu, RCNT_EVENTS[i].cls, EVS_CNT);
        if (rw(RCNT_CLEAR_FLAG(i))) {
            ww(0x1F801070u, ~RCNT_EVENTS[i].bit);   /* запись в I_STAT — это И */
            stop = 1;
        }
    }
    return stop;
}

/* Вторая половина — очередь ретейла с приоритетом 3: штатный раздатчик
 * аппаратных прерываний. Идёт ПОСЛЕ очереди игры. Таблица «бит → класс»
 * снята с func_00002458; заметь, что таймеры 1 и 2 делят один класс —
 * это особенность ретейла, а не опечатка. */
/* Отложенное завершение операции карты (см. card_finish ниже): на консоли
 * обмен с картой занимает десятки миллисекунд, и игра успевает встать в
 * ожидание события. Мы делали всё мгновенно — событие взводилось ДО того,
 * как игра начинала ждать, она его сбрасывала своей же проверкой и потом
 * ждала уже несуществующего. Поэтому событие созревает через несколько
 * вертикальных гашений. */
static struct { int vbl; uint32_t spec; int armed; } s_card_late;
static void card_deliver_now(CPUState* cpu, uint32_t spec);
static void card_late_tick(CPUState* cpu) {
    if (!s_card_late.armed) return;
    if (--s_card_late.vbl > 0) return;
    s_card_late.armed = 0;
    card_deliver_now(cpu, s_card_late.spec);
}

static void hle_sys_dispatcher(CPUState* cpu) {
    uint32_t act = rw(0x1F801070u) & rw(0x1F801074u);
    if (act & 0x0001u) card_late_tick(cpu);      /* по вертикальному гашению */
    static const struct { uint32_t bit; uint32_t cls; } HW[] = {
        { 0x0004u, 0xF0000003u },   /* привод */
        { 0x0200u, 0xF0000009u },   /* звук */
        { 0x0002u, 0xF0000002u },   /* графика */
        { 0x0400u, 0xF000000Au },   /* параллельный порт */
        { 0x0100u, 0xF000000Bu },   /* последовательный порт */
        { 0x0001u, 0xF0000001u },   /* вертикальное гашение */
        { 0x0010u, 0xF0000005u },   /* таймер 0 */
        { 0x0020u, 0xF0000006u },   /* таймер 1 */
        { 0x0040u, 0xF0000006u },   /* таймер 2 — тот же класс */
    };
    for (unsigned i = 0; i < sizeof HW / sizeof HW[0]; i++)
        if (act & HW[i].bit) hle_DeliverEvent(cpu, HW[i].cls, EVS_HW);
}

/* --------------------------------------------------------------------------
 * Контроллер. На консоли игра не читает порт сама: она отдаёт ядру два буфера
 * через InitPAD и включает опрос через StartPAD, а ядро на каждом вертикальном
 * гашении кладёт туда свежее состояние. Формат буфера (34 байта):
 *   +00 признак подключения (0 — есть, 0xFF — нет)
 *   +01 тип устройства (0x41 — цифровой)
 *   +02..+03 кнопки, 16 бит, активный уровень — НОЛЬ
 * Без этого игра видит «ничего не нажато»: на титуле Start не срабатывал и
 * она уходила в демонстрационный ролик.
 * ------------------------------------------------------------------------ */

extern uint16_t sio_get_pad_buttons(void);

static uint32_t s_pad_buf[2];
static uint32_t s_pad_len[2];
static int      s_pad_started = 0;

static void hle_pad_update(CPUState* cpu) {
    /* DC_HLE_PAD=0 - не подкладывать состояние контроллера в буферы игры:
     * у игры есть свой опрос по SIO, наша подкладка может с ним спорить. */
    { static int en = -1;
      if (en < 0) { const char* e = getenv("DC_HLE_PAD"); en = (e && *e == '0') ? 0 : 1; }
      if (!en) return; }
    if (!s_pad_started) return;
    uint16_t btn = sio_get_pad_buttons();
    for (int i = 0; i < 2; i++) {
        uint32_t b = s_pad_buf[i];
        if (!b || s_pad_len[i] < 4) continue;
        if (i == 0) {
            wb(cpu, b + 0, 0x00);            /* первый порт: пад на месте */
            wb(cpu, b + 1, 0x41);            /* цифровой контроллер */
            wb(cpu, b + 2, (uint8_t)(btn >> 8));
            wb(cpu, b + 3, (uint8_t)(btn & 0xFF));
        } else {
            wb(cpu, b + 0, 0xFF);            /* второй порт пуст */
            wb(cpu, b + 1, 0x41);
            wb(cpu, b + 2, 0xFF);
            wb(cpu, b + 3, 0xFF);
        }
    }
}

static CPUState* s_cpu = NULL;

/* Конец передачи по DMA ретейл-BIOS событием НЕ объявляет: событие звука
 * 0xF0000009/0x0020 игра шлёт себе сама, синхронно, по опросу регистра
 * управления звуком. Поэтому здесь ничего не делаем — оставлено, потому что
 * dma.c зовёт эту точку. */
void hle_bios_dma_completed(int ch) {
    (void)ch;
}

/* Очередь обработчиков, зарегистрированных игрой через SysEnqIntRP.
 * Проверяющая функция говорит «это моё прерывание», рабочая обслуживает —
 * обе принадлежат игре. Именно здесь растёт её счётчик кадров. */
static void hle_irq_chains(CPUState* cpu) {
    for (int p = 0; p < IRQ_PRIO; p++) {
        uint32_t cur = s_irq_chain[p];
        while (cur) {
            uint32_t handler  = rw(cur + 4);
            uint32_t verifier = rw(cur + 8);
            /* Узел без проверяющей функции ретейл пропускает целиком —
             * рабочую он в этом случае не зовёт никогда. */
            if (!verifier) { cur = rw(cur + 0); continue; }
            uint32_t take;
            {
                uint32_t save_ra = cpu->gpr[31];
                psx_dispatch(cpu, verifier);
                cpu->gpr[31] = save_ra;
                take = cpu->gpr[2];
            }
            if (take && handler) {
                s_exc_handled++;
                uint32_t save_ra = cpu->gpr[31];
                /* Ретейл-BIOS передаёт рабочей функции ОТВЕТ проверяющей:
                 * $a0 = то, что вернула проверка (обычно номер источника). */
                cpu->gpr[4] = take;
                psx_dispatch(cpu, handler);
                cpu->gpr[31] = save_ra;
            }
            /* Следующий узел читается ПОСЛЕ вызовов — обработчик может снять
             * себя с очереди или переписать связь, ретейл читает так же.
             * Обход не прерывается досрочно ни при каком ответе. */
            cur = rw(cur + 0);
        }
    }
}

/* ⭐ Сохранение контекста прерванного кода в TCB текущего потока — ровно то,
 * что делает ретейл-BIOS на входе в обработчик (регистры по TCB+8+4*N, EPC на
 * +136, HI/LO на +140/+144, SR на +148).
 * Это не формальность: переключение потоков в рантайме (psx_change_thread)
 * восстанавливает состояние потока ИЗ TCB. Пока ядро туда ничего не писало,
 * поток, прерванный во время работы, при возврате получал устаревший снимок —
 * в мире, где игра переключает потоки тысячи раз, это уводило её не в тот
 * поток и сцена не строилась. */
static void hle_save_context(CPUState* cpu) {
    /* DC_HLE_SAVECTX=0 — не трогать TCB на входе в обработчик. Переключение
     * потоков в рантайме восстанавливает состояние ИЗ TCB, и наша запись
     * может затирать контекст спящего потока (он паркуется в ChangeTh, а мы
     * пишем туда регистры на момент прерывания). Оставлено флагом, чтобы
     * сравнивать поведение честным A/B. */
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_HLE_SAVECTX"); on = (e && *e == '0') ? 0 : 1; }
    if (!on) return;
    uint32_t pcb = rw(0x80000108u);
    if (!pcb) return;
    uint32_t tcb = rw(pcb);
    if (!tcb) return;
    uint32_t base = (tcb & 0x001FFFFFu) | 0x80000000u;   /* TCB лежит в KSEG1 */
    for (int i = 1; i < 32; i++)
        ww(base + 8u + (uint32_t)i * 4u, cpu->gpr[i]);
    ww(base + 140, cpu->hi);
    ww(base + 144, cpu->lo);
    ww(base + 148, cpu->cop0[12]);   /* SR */
    /* ⛔ EPC здесь НЕ пишем: к моменту вызова обработчика рантайм уже подменил
     * его сторожевым значением 0x80000048 (interrupts.c), и запись затирала бы
     * потоку точку возврата. Настоящий адрес прерывания в этой точке недоступен. */
}

/* СТЕК ЯДРА НА ВРЕМЯ ОБРАБОТЧИКА (DC_HLE_KSTACK=0 — старое поведение).
 *
 * Ретейл первым делом уводит $sp на свой стек: `lui $sp,0` + `lw $sp,0x6CF0($sp)`
 * (SCPH1001_full.c:64485-64490, ПЗУ 0xBFC108C8/0xBFC108D0), и только потом
 * разбирает источники. Мы этого не делали — и все вызовы из обработчика
 * (колбэки событий, цепочки прерываний, обработчик игры) шли на стеке
 * ПРЕРВАННОГО потока, затирая его локальные переменные ниже его же $sp.
 * Регистры прерванного кода рантайм потом восстанавливает (interrupts.c),
 * а память — нет: поток просыпался с чужими данными в своём кадре.
 * Отсюда и плавающая точка зависания: каждый раз портилось разное. */
#define HLE_KSTACK_PTR 0x80006CF0u   /* ячейка ретейла с вершиной стека ядра */

static void hle_exception(CPUState* cpu) {
    s_exc_count++;
    hle_save_context(cpu);

    static int kstack = -1;
    if (kstack < 0) { const char* e = getenv("DC_HLE_KSTACK"); kstack = (e && *e == '0') ? 0 : 1; }
    uint32_t saved_sp = cpu->gpr[29], saved_fp = cpu->gpr[30];
    if (kstack) {
        uint32_t ks = rw(HLE_KSTACK_PTR);
        if (ks) { cpu->gpr[29] = ks; cpu->gpr[30] = ks; }
    }
#define HLE_EXC_RESTORE() do { cpu->gpr[29] = saved_sp; cpu->gpr[30] = saved_fp; } while (0)
    if (s_exc_count <= 5)
        fprintf(stderr, "hle: exception #%llu, I_STAT=%08X I_MASK=%08X, chains %08X/%08X/%08X/%08X\n",
                (unsigned long long)s_exc_count, rw(0x1F801070u), rw(0x1F801074u),
                s_irq_chain[0], s_irq_chain[1], s_irq_chain[2], s_irq_chain[3]);
    /* Порядок ровно как у ретейла: его очередь приоритета 1 (счётчики) →
     * очередь игры (она висит на приоритете 2) → его очередь приоритета 3
     * (раздатчик аппаратных событий) → выход через hook. */
    /* Состояние контроллера кладём в буферы игры перед обработкой — ядро
     * консоли делает это в своей части обработчика прерывания. */
    if ((rw(0x1F801070u) & 0x0001u)) hle_pad_update(cpu);

    if (hle_sys_counters(cpu)) { HLE_EXC_RESTORE(); cpu->pc = 0; return; }
    hle_irq_chains(cpu);
    hle_sys_dispatcher(cpu);

    /* ⭐ ВЫХОД ЧЕРЕЗ HookEntryInt — не вызов, а longjmp.
     * Ретейл-BIOS завершает обработчик исключений не возвратом, а прыжком в
     * буфер, зарегистрированный игрой: аргумент HookEntryInt — это 12-словный
     * буфер SetJmp (A0 0x13), а не четвёрка указателей:
     *   +00 ra  +04 sp  +08 fp  +0C..+28 s0..s7  +2C gp
     * Хвост обработчика грузит их все, ставит $a0 = адрес буфера, $a1 = 1 и
     * $v0 = 1, после чего делает jr $ra. Единица в $v0 — не мелочь: по адресу
     * возврата 0x80087B00 у Dino Crisis стоит `beq $v0, $zero`, и при нуле
     * игра уходит в ветку переустановки хука, ни разу не зайдя в собственный
     * обработчик прерываний. Отсюда и были холостые вызовы без подтверждения
     * привода: мы читали буфер как {pc, gp, sp, fp} и звали его функцией. */
    HLE_EXC_RESTORE();   /* дальше стек задаёт либо буфер игры, либо прерванный код */
    if (s_entry_hook) {
        uint32_t buf = s_entry_hook;
        uint32_t ra = rw(buf + 0x00);
        if (ra) {
            s_hook_calls++;
            cpu->gpr[31] = ra;
            cpu->gpr[29] = rw(buf + 0x04);              /* sp */
            cpu->gpr[30] = rw(buf + 0x08);              /* fp */
            for (int i = 0; i < 8; i++)
                cpu->gpr[16 + i] = rw(buf + 0x0Cu + (uint32_t)i * 4u);  /* s0..s7 */
            cpu->gpr[28] = rw(buf + 0x2C);              /* gp */
            cpu->gpr[4]  = buf;                         /* a0 = сам буфер */
            cpu->gpr[5]  = 1;                           /* a1 = 1 */
            cpu->gpr[2]  = 1;                           /* v0 = 1 — ключ */
            cpu->pc = ra;   /* продолжаем с адреса возврата, как longjmp */
            return;
        }
    }

    cpu->pc = 0;
}

/* --------------------------------------------------------------------------
 * Потоки. Раскладка TCB снята с эталонной машины (запись 0xC0 байт):
 *   +00 состояние (0x4000 занят, 0x1000 свободен)  +04 режим
 *   +08 регистры r0..r31   +136 EPC   +140 HI   +144 LO   +148 SR
 * Само переключение делает рантайм на файберах (psx_change_thread в traps.c) —
 * тот же путь, что и при вызове через syscall, поэтому поведение совпадает
 * со сборкой на ретейл-BIOS.
 * ------------------------------------------------------------------------ */

extern int  psx_change_thread(CPUState* cpu, uint32_t target_tcb);
extern void psx_thread_retire(uint32_t tcb);   /* списать хост-файбер потока */

#define TCB_STRIDE 0xC0u
#define TCB_COUNT  (HLE_TCB_SIZE / TCB_STRIDE)
#define TCB_ADDR(i) (0x80000000u + HLE_TCB_ADDR + (uint32_t)(i) * TCB_STRIDE)
#define TCB_USED   0x4000u
#define TCB_FREE   0x1000u

static int th_index(uint32_t handle) {
    if ((handle & 0xFF000000u) != 0xFF000000u) return -1;
    uint32_t i = handle & 0xFFFFu;
    return (i < TCB_COUNT) ? (int)i : -1;
}

static uint32_t hle_OpenTh(uint32_t pc, uint32_t sp, uint32_t gp) {
    for (uint32_t i = 1; i < TCB_COUNT; i++) {     /* 0 — главный поток */
        uint32_t t = TCB_ADDR(i);
        if (rw(t) == TCB_USED) continue;
        for (uint32_t o = 0; o < TCB_STRIDE; o += 4) ww(t + o, 0);
        ww(t + 0x00, TCB_USED);
        ww(t + 0x04, 0x1000u);
        ww(t + 8 + 28 * 4, gp);
        ww(t + 8 + 29 * 4, sp);
        ww(t + 8 + 30 * 4, sp);
        ww(t + 136, pc);                            /* EPC — точка входа */
        /* SR как у ретейла (сверено с дампом рабочей машины: 0x40000404):
         * прерывания разрешены + бит 30 — доступ к сопроцессору геометрии.
         * Без него поток формально не имеет права на GTE-команды. */
        ww(t + 148, 0x40000404u);
        /* Новый поток в старом слоте: списать прежний хост-файбер, иначе
         * рантайм возобновит его со старого места и точка входа пропадёт. */
        psx_thread_retire(0xA0000000u + HLE_TCB_ADDR + i * TCB_STRIDE);
        return 0xFF000000u | i;
    }
    fprintf(stderr, "hle: OpenTh - no free threads\n");
    return 0xFFFFFFFFu;
}

static uint32_t hle_CloseTh(uint32_t handle) {
    int i = th_index(handle);
    if (i <= 0) return 0;
    ww(TCB_ADDR(i), TCB_FREE);
    psx_thread_retire(0xA0000000u + HLE_TCB_ADDR + (uint32_t)i * TCB_STRIDE);
    return 1;
}

static uint32_t hle_ChangeTh(CPUState* cpu, uint32_t handle) {
    int i = th_index(handle);
    if (i < 0) {
        fprintf(stderr, "hle: ChangeTh with a bad handle %08X\n", handle);
        return 0;
    }
    uint32_t target = 0xA0000000u + HLE_TCB_ADDR + (uint32_t)i * TCB_STRIDE;

    /* DC_HLE_THDBG=1 — кто, куда и с чем переключается, и вернулось ли
     * управление. Без этого не видно, застряло ли переключение: обычная
     * трассировка вызовов не ловит путь через интерпретируемый блок. */
    static int thdbg = -1;
    if (thdbg < 0) { const char* e = getenv("DC_HLE_THDBG"); thdbg = (e && *e == '1') ? 60 : ((e && *e == '2') ? 1000000000 : 0); }
    int log = (thdbg > 0);
    if (log) {
        thdbg--;
        fprintf(stderr, "hle: ChangeTh -> thread %d (TCB %08X), ra=%08X, target state %08X\n",
                i, target, cpu->gpr[31], rw(TCB_ADDR(i)));
    }

    uint32_t ok = psx_change_thread(cpu, target) ? 1u : 0u;

    if (log)
        fprintf(stderr, "hle: ChangeTh returned, result %u, current TCB %08X\n",
                ok, rw(0x80000000u + HLE_PCB_ADDR));
    if (!ok)
        fprintf(stderr, "hle: ChangeTh %08X failed (TCB %08X)\n", handle, target);
    return ok;
}

/* --------------------------------------------------------------------------
 * Карта памяти. Ядро работает прямо с нашими файлами card1.mcd / card2.mcd
 * (memcard.c), минуя протокол SIO: игре важен только результат операции,
 * о котором она узнаёт из события класса 0xF4000001.
 * Канал: 0x00 — первый слот, 0x10 — второй.
 * ------------------------------------------------------------------------ */

extern int memcard_read_sector(int card, int sector, uint8_t* buf);
extern int memcard_write_sector(int card, int sector, const uint8_t* buf);
extern int memcard_is_present(int card);
extern void memcard_flush(int card);
extern int memcard_flush_checked(int card);   /* memcard.c: 0 = в файле, -1 = файл не записан */

#define CARD_EVENT_CLASS 0xF4000001u      /* SwCARD — файловая система BIOS */
#define CARD_EVENT_HW    0xF0000011u      /* HwCARD — драйвер SIO (железо)   */
#define CARD_SPEC_DONE   0x0004u
#define CARD_SPEC_ERROR  0x8000u

/* Канал карты: 0x00 — слот 1, 0x10 — слот 2. Всё остальное (0x01..0x0F и
 * 0x2x..0xFx) — карты мультитапа, которых у нас нет.
 * ⭐ 09.09: игра ПЕРЕБИРАЕТ каналы (в её структуре менеджера поле +22 растёт:
 * 0,1,...,5 → каналы 0x00,0x10,...,0x50), и прежняя формула `(chan>>4)&1`
 * отвечала «карта на месте» на каждый из них — на 0x50 в том числе. Автомат
 * менеджера от такого ответа уходил не туда. Теперь чужой канал = нет карты. */
static int card_of(uint32_t chan) {
    if (chan & 0x0Fu) return -1;
    uint32_t slot = chan >> 4;
    return (slot <= 1u) ? (int)slot : -1;
}

/* --- ЖУРНАЛ КАРТЫ (2026-09-09, игрок: «сейвы игры не работают») ------------
 * Без него не видно ни одной стороны сделки: что игра позвала, чем ядро
 * ответило и нашёлся ли получатель у события. DC_CARD_LOG=0 — выключить. */
static int card_log_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_CARD_LOG"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* Все события класса карты, какие игра открыла: класс, спец, режим, состояние.
 * Именно по паре (класс, спец) DeliverEvent ищет получателя — если игра ждёт
 * другую пару, наш DONE уходит в никуда, и она сообщает об ошибке. */
static void card_log_events(const char* when) {
    if (!card_log_on()) return;
    fprintf(stderr, "card: events at '%s':\n", when);
    int n = 0;
    for (uint32_t i = 0; i < EV_COUNT; i++) {
        uint32_t e = EV_ADDR(i);
        uint32_t st = rw(e + 4);
        if (st == EV_FREE) continue;
        uint32_t cls = rw(e + 0x00);
        /* карточные классы: SwCARD 0xF4000001, HwCARD 0xF0000011, плюс всё,
         * что игра открыла с «ошибочным» спецом 0x8000 */
        uint32_t spec = rw(e + 0x08);
        if (cls != 0xF4000001u && cls != 0xF0000011u && spec != 0x8000u) continue;
        fprintf(stderr, "card:   ev%02u class=%08X spec=%04X mode=%04X state=%04X handler=%08X\n",
                i, cls, spec, rw(e + 0x0C), st, rw(e + 0x10));
        n++;
    }
    if (!n) fprintf(stderr, "card:   (no memory card event is open)\n");
}
/* Доставка с отчётом: нашёлся ли получатель.
 * ⭐ 09.09: шлём ОБА класса — и «железный» HwCARD (0xF0000011), и
 * «программный» SwCARD (0xF4000001). На консоли операцию завершают оба:
 * сначала драйвер SIO поднимает аппаратное, следом файловая система BIOS —
 * программное. Игра открыла обе четвёрки событий (спецы 0004/8000/0100/2000
 * у каждого класса) и ждала именно HwCARD: наше единственное SwCARD-событие
 * так и висело доставленным-непрочитанным (состояние 4000 в сводке ядра), а
 * сохранение объявлялось неудавшимся. */
static void card_deliver_now(CPUState* cpu, uint32_t spec) {
    static const uint32_t CLS[2] = { CARD_EVENT_HW, CARD_EVENT_CLASS };
    for (int k = 0; k < 2; k++) {
        uint64_t before_hit = s_deliver_hits, before_miss = s_deliver_miss;
        hle_DeliverEvent(cpu, CLS[k], spec);
        if (card_log_on())
            fprintf(stderr, "card:   -> event class=%08X spec=%04X %s\n",
                    CLS[k], spec,
                    (s_deliver_hits > before_hit) ? "DELIVERED" :
                    (s_deliver_miss > before_miss) ? "NOBODY LISTENING" : "?");
    }
}
/* Завершение операции карты: не мгновенно, а через DC_CARD_DELAY вертикальных
 * гашений (по умолчанию 2 — примерно столько же занимает обмен на консоли).
 * DC_CARD_DELAY=0 возвращает прежнюю мгновенную доставку. */
static void card_deliver(CPUState* cpu, uint32_t spec) {
    static int delay = -1;
    if (delay < 0) { const char* e = getenv("DC_CARD_DELAY"); delay = e ? atoi(e) : 2; }
    if (delay <= 0) { card_deliver_now(cpu, spec); return; }
    /* Если предыдущее завершение ещё не созрело — отдаём его сразу, порядок
     * операций для игры не должен меняться. */
    if (s_card_late.armed) { s_card_late.armed = 0; card_deliver_now(cpu, s_card_late.spec); }
    s_card_late.spec = spec; s_card_late.vbl = delay; s_card_late.armed = 1;
}

/* ⭐ КОРЕНЬ «Save failed. Please try again.» (найден 09.09.2026).
 * memcard_read_sector/memcard_write_sector возвращают 0 при УСПЕХЕ и -1 при
 * ошибке (memcard.c), а прежнее условие ошибки читалось как
 *     if (!memcard_is_present(card) || !memcard_write_sector(...))
 * то есть «!0» = успех попадал в ветку ОШИБКИ. Ядро отвечало игре ошибкой на
 * КАЖДУЮ операцию карты — и на запись, и на чтение, — хотя сектор ложился в
 * образ карты правильно. Отсюда и «сохранение не удалось», и пустой список
 * сохранений. Ниже — правильная проверка: успех = возврат 0. */
static uint32_t hle_card_read(CPUState* cpu, uint32_t chan, uint32_t sector, uint32_t dst) {
    uint8_t buf[128];
    int card = card_of(chan);
    int ok = memcard_is_present(card) && memcard_read_sector(card, (int)sector, buf) == 0;
    if (card_log_on())
        fprintf(stderr, "card: read  slot=%d sector=%u -> %s (ra=%08X)\n",
                card, sector, ok ? "ok" : "ERROR", cpu->gpr[31]);
    if (!ok) { card_deliver(cpu, CARD_SPEC_ERROR); return 0; }
    for (int i = 0; i < 128; i++) wb(cpu, dst + (uint32_t)i, buf[i]);
    card_deliver(cpu, CARD_SPEC_DONE);
    return 1;
}

static uint32_t hle_card_write(CPUState* cpu, uint32_t chan, uint32_t sector, uint32_t src) {
    uint8_t buf[128];
    int card = card_of(chan);
    for (int i = 0; i < 128; i++) buf[i] = rb(cpu, src + (uint32_t)i);
    int ok = memcard_is_present(card) && memcard_write_sector(card, (int)sector, buf) == 0;
    if (card_log_on()) {
        static int first = 1;
        if (first) { first = 0; card_log_events("first sector write"); }
        fprintf(stderr, "card: write slot=%d sector=%u -> %s (ra=%08X, first bytes %02X%02X%02X%02X)\n",
                card, sector, ok ? "ok" : "ERROR", cpu->gpr[31],
                buf[0], buf[1], buf[2], buf[3]);
    }
    if (!ok) { card_deliver(cpu, CARD_SPEC_ERROR); return 0; }
    /* Сектор — сразу в файл карты, как это делает путь через SIO (sio.c):
     * иначе сохранение живёт только в памяти процесса и теряется, если игру
     * закрыли не через штатный выход. 18.10.2026: файл не записался (полный
     * диск, нет прав) - игре ошибка записи, как от сбойной карты, а не «готово». */
    if (memcard_flush_checked(card) != 0) {
        fprintf(stderr, "card: memory card file %d not written - the game gets a write error\n", card);
        card_deliver(cpu, CARD_SPEC_ERROR);
        return 0;
    }
    card_deliver(cpu, CARD_SPEC_DONE);
    return 1;
}

/* --------------------------------------------------------------------------
 * ФАЙЛОВАЯ СИСТЕМА КАРТЫ ПАМЯТИ: open / lseek / read / write / close
 *
 * 09.09.2026, игрок: «сейвы игры не работают». Замер сводкой ядра показал,
 * ЧЕМ именно сохраняется Dino Crisis:
 *      B0(32) open 6   B0(33) lseek 5   B0(35) write 30   B0(36) close 6
 *      ← все НЕ РЕАЛИЗОВАНО
 * то есть игра пишет сейв как ФАЙЛ на «bu00:», а не посекторно через
 * _card_write_sector, который ядро умело. Отсюда и «Save failed. Please try
 * again.», и шесть секунд ожидания: игра повторяла попытку за попыткой.
 *
 * Раскладка карты (та же, что формирует memcard.c и понимает DuckStation):
 *   сектор 128 байт, блок = 64 сектора = 8 КБ, всего 16 блоков;
 *   блок 0 служебный: сектор 0 — заголовок «MC», сектора 1..15 — записи
 *   директории для блоков данных 1..15;
 *   запись: +00 состояние (0x51 первый блок файла, 0x52 средний, 0x53
 *   последний, 0xA0 свободный), +04 размер файла в байтах (у первого блока),
 *   +08 индекс СЛЕДУЮЩЕЙ записи (0xFFFF — последняя), +0A имя (20 байт),
 *   +7F XOR-контроль.
 * ------------------------------------------------------------------------ */

#define CARD_BLK_SECTORS 64
#define CARD_BLK_BYTES   (CARD_BLK_SECTORS * 128)
#define CARD_DIR_N       15

static uint32_t crd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void cwr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int card_dir_get(int card, int idx, uint8_t* fr) {
    return memcard_read_sector(card, idx + 1, fr) == 0;
}
static void card_dir_put(int card, int idx, uint8_t* fr) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= fr[i];
    fr[127] = x;
    memcard_write_sector(card, idx + 1, fr);
}
/* Индекс записи первого блока файла с таким именем, иначе -1. */
static int card_find(int card, const char* name) {
    uint8_t fr[128];
    for (int i = 0; i < CARD_DIR_N; i++) {
        if (!card_dir_get(card, i, fr)) continue;
        if (crd32(fr) != 0x51u) continue;              /* только первый блок файла */
        if (strncmp((const char*)fr + 0x0A, name, 20) == 0) return i;
    }
    return -1;
}
/* Создать файл на blocks блоков; вернуть индекс первой записи или -1. */
static int card_create(int card, const char* name, int blocks) {
    uint8_t fr[128];
    int freeidx[CARD_DIR_N], nfree = 0;
    if (blocks < 1) blocks = 1;
    for (int i = 0; i < CARD_DIR_N && nfree < blocks; i++) {
        if (!card_dir_get(card, i, fr)) continue;
        if ((crd32(fr) & 0xF0u) == 0xA0u) freeidx[nfree++] = i;   /* 0xA0..0xA3 — свободна */
    }
    if (nfree < blocks) return -1;                      /* карта заполнена */
    for (int b = 0; b < blocks; b++) {
        int next = (b + 1 < blocks) ? freeidx[b + 1] : 0xFFFF;
        memset(fr, 0, sizeof fr);
        cwr32(fr, b == 0 ? 0x51u : (b == blocks - 1 ? 0x53u : 0x52u));
        if (b == 0) {
            cwr32(fr + 4, (uint32_t)blocks * CARD_BLK_BYTES);
            strncpy((char*)fr + 0x0A, name, 20);
        }
        fr[8] = (uint8_t)(next & 0xFF);
        fr[9] = (uint8_t)((next >> 8) & 0xFF);
        card_dir_put(card, freeidx[b], fr);
    }
    return freeidx[0];
}
/* Сектор карты, на который попадает позиция pos в файле. */
static int card_seek_sector(int card, int first, int pos, int* sec_out) {
    uint8_t fr[128];
    int idx = first;
    for (int k = pos / CARD_BLK_BYTES; k > 0; k--) {
        if (!card_dir_get(card, idx, fr)) return 0;
        int nx = (int)fr[8] | ((int)fr[9] << 8);
        if (nx < 0 || nx >= CARD_DIR_N) return 0;       /* цепочка кончилась */
        idx = nx;
    }
    *sec_out = (idx + 1) * CARD_BLK_SECTORS + (pos % CARD_BLK_BYTES) / 128;
    return 1;
}

/* ⭐ КЭШ ДИРЕКТОРИИ КАРТЫ — то, из чего игра строит список сохранений.
 * 09.09.2026: экран LOAD/SAVE показывал «NO DATA» на карте с готовым сейвом,
 * причём в момент показа игра НЕ обращалась ни к одному файлу — значит имена
 * она берёт откуда-то ещё. Замер на эталоне (сборка с настоящим BIOS, где
 * список показывается правильно): в области ядра по адресу 0x8000BA88 лежат
 * 30 записей по 32 байта — по 15 на карту, — и это ПЕРВЫЕ 30 БАЙТ записи
 * директории один в один (состояние, размер, ссылка на следующий блок, имя).
 * Ретейл наполняет их внутри _card_load_dir; наше ядро только делало вид,
 * что загрузило директорию. Теперь наполняет по-настоящему. */
#define CARD_DIRCACHE 0x8000BA88u
static void hle_dcb_ensure(void);          /* таблица устройств, см. ниже */
static void card_cache_dir(CPUState* cpu, int card) {
    uint8_t fr[128];
    if (card < 0 || card > 1) return;
    hle_dcb_ensure();                      /* переживает загрузку savestate */
    for (int i = 0; i < CARD_DIR_N; i++) {
        uint32_t e = CARD_DIRCACHE + (uint32_t)((card * CARD_DIR_N + i) * 32);
        if (!memcard_is_present(card) || !card_dir_get(card, i, fr)) {
            memset(fr, 0, sizeof fr);
            fr[0] = 0xA0;                       /* свободная запись */
            fr[8] = 0xFF; fr[9] = 0xFF;
        }
        for (int k = 0; k < 30; k++) wb(cpu, e + (uint32_t)k, fr[k]);
        wb(cpu, e + 30, 0); wb(cpu, e + 31, 0);
    }
    {   /* лог однократный на слот: кэш обновляется на каждом опросе карты */
        static int said[2] = { 0, 0 };
        if (card_log_on() && !said[card]) {
            said[card] = 1;
            fprintf(stderr, "card: slot %d directory placed in the kernel cache 0x%08X\n",
                    card, CARD_DIRCACHE + (uint32_t)(card * CARD_DIR_N * 32));
        }
    }
}

/* ⭐⭐ ТАБЛИЦА УСТРОЙСТВ ЯДРА (DCB) — без неё список сохранений пуст всегда.
 *
 * 09.09.2026. Список сейвов игра строит не из кэша директории и не файловыми
 * вызовами: в ней слинкована libapi-версия firstfile (func_80094064), которая
 * САМА обходит таблицу устройств BIOS — читает указатель по адресу 0x150 и
 * размер по 0x154, делит размер на 80 и сравнивает имя каждого устройства с
 * префиксом пути («bu» из «bu00:BASLUS-00922-DINO*»). У нас этой таблицы не
 * было вовсе, поэтому firstfile не находил устройство и возвращал «файлов
 * нет» — nextfile после него уже не звался (замер fn_hist: func_80094064
 * вызвана 1 раз, B0(0x42) — ни разу, и в сводке ядра его нет).
 * Раскладка снята с эталона (сборка с настоящим BIOS): 10 записей по 80 байт,
 * начиная с 0x80006EE0; у карты памяти размер сектора 128 и флаги 0x14. */
#define DCB_TABLE   0x80006EE0u
#define DCB_COUNT   10
#define DCB_ENTSZ   80
#define DCB_NAME_BU 0x80006EC0u        /* строка "bu" перед таблицей */

static int card_path(const char* path, int* card, const char** name);   /* ниже */

static void hle_dcb_init(void) {
    ww(DCB_NAME_BU, 0x00007562u);      /* строка "bu" */
    for (uint32_t i = 0; i < DCB_COUNT; i++) {
        uint32_t e = DCB_TABLE + i * DCB_ENTSZ;
        for (uint32_t k = 0; k < DCB_ENTSZ; k += 4) ww(e + k, 0);
    }
    /* Запись 2 — карта памяти, как у ретейла (там же лежит и у него). */
    uint32_t bu = DCB_TABLE + 2u * DCB_ENTSZ;
    ww(bu + 0x00, DCB_NAME_BU);        /* имя устройства */
    ww(bu + 0x04, 0x14u);              /* флаги */
    ww(bu + 0x08, 128u);               /* размер сектора */
    ww(bu + 0x0C, DCB_NAME_BU);        /* описание — та же строка, игре хватает */
    ww(0x80000150u, DCB_TABLE);
    ww(0x80000154u, DCB_COUNT * DCB_ENTSZ);
}
/* ⭐ ТАБЛИЦА ДОЛЖНА ПЕРЕЖИВАТЬ ПОДМЕНУ ВСЕЙ ПАМЯТИ (09.09.2026).
 * Она живёт в гостевой RAM, а её туда кладёт ядро один раз при загрузке.
 * Загрузка быстрого сохранения (savestate) восстанавливает RAM целиком — и
 * если слепок снят до появления таблицы, указатель по 0x150 обнуляется, а
 * список сейвов после этого ВСЕГДА пуст: libapi-шная firstfile обходит
 * таблицу сама и, не найдя устройства «bu», до нашего ядра даже не доходит.
 * Ровно так это и выглядело в замере: 0x150 = 0 при двух годных сейвах на
 * карте. Проверяем указатель на каждом опросе карты и, если он не наш,
 * выкладываем таблицу заново. */
static void hle_dcb_ensure(void) {
    uint32_t had = rw(0x80000150u);
    if (had == DCB_TABLE && rw(0x80000154u) == DCB_COUNT * DCB_ENTSZ
        && rw(DCB_TABLE + 2u * DCB_ENTSZ) == DCB_NAME_BU)
        return;
    hle_dcb_init();
    if (card_log_on())
        fprintf(stderr, "card: device table restored (0x150 was 0x%08X)\n", had);
}

/* ---- firstfile / nextfile (B0 0x42 / 0x43) -------------------------------
 * Отдают DIRENTRY (40 байт): имя[20] + атрибут(4) + размер(4) + next(4) +
 * начальный сектор(4) + system[4]. Значения атрибута (0x50) и сектора
 * ((номер записи + 1) * 64) сверены с эталоном. */
static struct { int active, card, idx; char pat[24]; uint32_t entry; } s_ff;

static int card_name_match(const char* pat, const char* name) {
    /* Шаблон вида «BASLUS-00922-DINO*»; поддержаны '*' (до конца) и '?'. */
    for (;;) {
        if (*pat == '*') return 1;
        if (!*pat) return !*name;
        if (!*name) return 0;
        if (*pat != '?' && *pat != *name) return 0;
        pat++; name++;
    }
}
static int card_dir_next(CPUState* cpu, int card, int from, uint32_t entry, const char* pat) {
    uint8_t fr[128];
    for (int i = from; i < CARD_DIR_N; i++) {
        if (!memcard_is_present(card) || !card_dir_get(card, i, fr)) break;
        if (crd32(fr) != 0x51u) continue;                 /* только первый блок файла */
        char nm[21];
        memcpy(nm, fr + 0x0A, 20); nm[20] = 0;
        if (!card_name_match(pat, nm)) continue;
        for (int k = 0; k < 20; k++) wb(cpu, entry + (uint32_t)k, (uint8_t)nm[k]);
        ww(entry + 20, 0x50u);                            /* атрибут — как у ретейла */
        ww(entry + 24, crd32(fr + 4));                    /* размер файла */
        ww(entry + 28, 0);                                /* next — не используется */
        ww(entry + 32, (uint32_t)((i + 1) * CARD_BLK_SECTORS));  /* начальный сектор */
        ww(entry + 36, 0);
        if (card_log_on())
            fprintf(stderr, "card: found file '%s' (entry %d, %u bytes)\n",
                    nm, i, crd32(fr + 4));
        return i;
    }
    return -1;
}
static uint32_t hle_firstfile(CPUState* cpu, uint32_t path_addr, uint32_t entry) {
    char path[64]; guest_str(cpu, path_addr, path, sizeof path);
    int card; const char* name;
    if (!card_path(path, &card, &name) || !memcard_is_present(card)) {
        if (card_log_on()) fprintf(stderr, "card: firstfile '%s' - no such device\n", path);
        s_ff.active = 0;
        return 0;
    }
    snprintf(s_ff.pat, sizeof s_ff.pat, "%s", name);
    s_ff.card = card; s_ff.entry = entry; s_ff.active = 1;
    int i = card_dir_next(cpu, card, 0, entry, s_ff.pat);
    if (i < 0) { s_ff.active = 0; return 0; }
    s_ff.idx = i;
    return entry;
}
static uint32_t hle_nextfile(CPUState* cpu, uint32_t entry) {
    if (!s_ff.active) return 0;
    int i = card_dir_next(cpu, s_ff.card, s_ff.idx + 1, entry, s_ff.pat);
    if (i < 0) { s_ff.active = 0; return 0; }
    s_ff.idx = i;
    return entry;
}

#define CARD_FD_N 8
static struct { int used, card, first, pos, size; char name[24]; } s_cfd[CARD_FD_N];

/* «bu00:ИМЯ» → слот 0, «bu10:ИМЯ» → слот 1. Возвращает 1, если путь наш. */
static int card_path(const char* path, int* card, const char** name) {
    if (!path || (path[0] != 'b' && path[0] != 'B') || (path[1] != 'u' && path[1] != 'U')) return 0;
    const char* c = strchr(path, ':');
    if (!c) return 0;
    *card = (path[3] == '1' || path[2] == '1') ? 1 : 0;   /* bu00 / bu10 */
    *name = c + 1;
    return 1;
}
static int hle_file_open(CPUState* cpu, uint32_t name_addr, uint32_t mode) {
    char path[64]; guest_str(cpu, name_addr, path, sizeof path);
    int card; const char* name;
    if (!card_path(path, &card, &name)) {
        if (card_log_on()) fprintf(stderr, "card: open '%s' - not a memory card path\n", path);
        return -1;
    }
    if (!memcard_is_present(card)) return -1;
    int first = card_find(card, name);
    /* O_CREAT = 0x0200, число блоков — в старших 16 битах (PsyQ:
     * open(name, O_CREAT | (blocks << 16))). */
    if (first < 0 && (mode & 0x0200u)) {
        int blocks = (int)((mode >> 16) & 0xFFFFu);
        first = card_create(card, name, blocks ? blocks : 1);
        if (card_log_on())
            fprintf(stderr, "card: created '%s', %d block(s) -> entry %d\n",
                    name, blocks ? blocks : 1, first);
    }
    if (first < 0) {
        if (card_log_on()) fprintf(stderr, "card: open '%s' mode %08X - no such file\n", name, mode);
        return -1;
    }
    for (int i = 0; i < CARD_FD_N; i++) {
        if (s_cfd[i].used) continue;
        uint8_t fr[128];
        int size = CARD_BLK_BYTES;
        if (card_dir_get(card, first, fr)) size = (int)crd32(fr + 4);
        if (size <= 0) size = CARD_BLK_BYTES;
        s_cfd[i].used = 1; s_cfd[i].card = card; s_cfd[i].first = first;
        s_cfd[i].pos = 0;  s_cfd[i].size = size;
        snprintf(s_cfd[i].name, sizeof s_cfd[i].name, "%s", name);
        if (card_log_on())
            fprintf(stderr, "card: open '%s' mode %08X -> fd=%d (block %d, %d bytes)\n",
                    name, mode, i + 1, first, size);
        return i + 1;                                   /* дескриптор 1..8 */
    }
    return -1;
}
static int fd_ok(int fd) { return fd >= 1 && fd <= CARD_FD_N && s_cfd[fd - 1].used; }
static int hle_file_rw(CPUState* cpu, int fd, uint32_t addr, int len, int writing) {
    if (!fd_ok(fd) || len < 0) return -1;
    int i = fd - 1, done = 0, io_fail = 0;
    uint8_t sec[128];
    while (done < len) {
        int sector, off = s_cfd[i].pos % 128;
        int n = 128 - off; if (n > len - done) n = len - done;
        /* ХВОСТ ЗА КОНЦОМ ФАЙЛА. Игра пишет сейв кусками по 2432 байта и
         * НЕ подгоняет последний кусок под размер файла (8192 = 3×2432 + 896),
         * а потом отправляет ещё два таких же куска. Пока мы возвращали ей
         * «896 из 2432», она считала запись неудавшейся и повторяла весь
         * цикл — три захода и «Save failed» на полностью записанном сейве.
         * Байтам за концом файла лечь некуда (блок один), поэтому они просто
         * отбрасываются, а в результате учитываются: ровно так операция
         * выглядит для игры на консоли. DC_CARD_TAIL_OK=0 — прежнее строгое
         * поведение (короткий результат). */
        if (s_cfd[i].pos >= s_cfd[i].size) {
            static int tail = -1;
            if (tail < 0) { const char* e = getenv("DC_CARD_TAIL_OK"); tail = (e && *e == '0') ? 0 : 1; }
            if (tail) done = len;
            break;
        }
        if (!card_seek_sector(s_cfd[i].card, s_cfd[i].first, s_cfd[i].pos, &sector)) break;
        if (memcard_read_sector(s_cfd[i].card, sector, sec) != 0) { io_fail = 1; break; }
        if (writing) {
            for (int k = 0; k < n; k++) sec[off + k] = rb(cpu, addr + (uint32_t)(done + k));
            if (memcard_write_sector(s_cfd[i].card, sector, sec) != 0) { io_fail = 1; break; }
        } else {
            for (int k = 0; k < n; k++) wb(cpu, addr + (uint32_t)(done + k), sec[off + k]);
        }
        s_cfd[i].pos += n; done += n;
    }
    /* сразу в файл карты; 18.10.2026: не записался - игре ошибка носителя («Save failed»), а не «сохранено» */
    if (writing && done && memcard_flush_checked(s_cfd[i].card) != 0) {
        fprintf(stderr, "card: memory card file %d not written - the game gets a write error\n", s_cfd[i].card);
        io_fail = 1;
    }
    if (card_log_on())
        fprintf(stderr, "card: %s fd=%d %d/%d bytes (position %d)%s\n",
                writing ? "write" : "read ", fd, done, len, s_cfd[i].pos,
                io_fail ? "  MEDIA ERROR" : "");
    /* ⭐ 09.09: событие «ошибка» шлём ТОЛЬКО при отказе носителя. Игра пишет
     * сейв кусками по 2432 байта, и последний кусок упирается в конец файла
     * (8192 = 3×2432 + 896): раньше мы на этом слали спец 8000, игра его
     * слушает (событие открыто) и показывала «Save failed» на полностью
     * записанном сейве. На консоли короткая запись в конце файла — норма. */
    card_deliver(cpu, io_fail ? CARD_SPEC_ERROR : CARD_SPEC_DONE);
    return io_fail ? -1 : done;
}
static int hle_file_seek(int fd, int off, int whence) {
    if (!fd_ok(fd)) return -1;
    int i = fd - 1;
    int p = (whence == 1) ? s_cfd[i].pos + off
          : (whence == 2) ? s_cfd[i].size + off
                          : off;
    if (p < 0) return -1;
    s_cfd[i].pos = p;
    return p;
}
static int hle_file_close_cpu(CPUState* cpu, int fd) {
    if (!fd_ok(fd)) return -1;
    memcard_flush(s_cfd[fd - 1].card);
    /* После записи директория могла измениться (новый файл) — кэш ядра, из
     * которого игра строит список сохранений, обновляем сразу. */
    card_cache_dir(cpu, s_cfd[fd - 1].card);
    if (card_log_on())
        fprintf(stderr, "card: close fd=%d '%s'\n", fd, s_cfd[fd - 1].name);
    s_cfd[fd - 1].used = 0;
    return fd;
}

/* --------------------------------------------------------------------------
 * Вызов функции ядра
 * ------------------------------------------------------------------------ */

static uint32_t s_rand_seed = 1;
static uint32_t s_heap_base = 0, s_heap_size = 0;

static int hle_call_a0(CPUState* cpu, uint32_t fn) {
    uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5], a2 = cpu->gpr[6];
    char buf[512];
    switch (fn) {
    case 0x10: {                                    /* atoi */
        guest_str(cpu, a0, buf, sizeof buf);
        cpu->gpr[2] = (uint32_t)atoi(buf);
        return 1;
    }
    case 0x13: {                                    /* setjmp */
        static const int reg[] = { 31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28 };
        for (int i = 0; i < 12; i++) ww(a0 + (uint32_t)i * 4, cpu->gpr[reg[i]]);
        cpu->gpr[2] = 0;
        return 1;
    }
    case 0x14: {                                    /* longjmp */
        static const int reg[] = { 31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28 };
        for (int i = 0; i < 12; i++) cpu->gpr[reg[i]] = rw(a0 + (uint32_t)i * 4);
        cpu->gpr[2] = a1;
        cpu->pc = cpu->gpr[31];
        return 1;
    }
    case 0x17: {                                    /* strcmp */
        int r = 0;
        for (uint32_t i = 0;; i++) {
            uint8_t x = rb(cpu, a0 + i), y = rb(cpu, a1 + i);
            if (x != y) { r = (int)x - (int)y; break; }
            if (!x) break;
        }
        cpu->gpr[2] = (uint32_t)r;
        return 1;
    }
    case 0x19: {                                    /* strcpy */
        uint32_t i = 0; uint8_t c;
        do { c = rb(cpu, a1 + i); wb(cpu, a0 + i, c); i++; } while (c);
        cpu->gpr[2] = a0;
        return 1;
    }
    case 0x1B: {                                    /* strlen */
        uint32_t i = 0;
        while (rb(cpu, a0 + i)) i++;
        cpu->gpr[2] = i;
        return 1;
    }
    case 0x28:                                      /* bzero */
        for (uint32_t i = 0; i < a1; i++) wb(cpu, a0 + i, 0);
        cpu->gpr[2] = a0;
        return 1;
    case 0x2A:                                      /* memcpy */
        for (uint32_t i = 0; i < a2; i++) wb(cpu, a0 + i, rb(cpu, a1 + i));
        cpu->gpr[2] = a0;
        return 1;
    case 0x2B:                                      /* memset */
        for (uint32_t i = 0; i < a2; i++) wb(cpu, a0 + i, (uint8_t)(a1 & 0xFF));
        cpu->gpr[2] = a0;
        return 1;
    case 0x2E: {                                    /* memchr */
        cpu->gpr[2] = 0;
        for (uint32_t i = 0; i < a2; i++)
            if (rb(cpu, a0 + i) == (uint8_t)(a1 & 0xFF)) { cpu->gpr[2] = a0 + i; break; }
        return 1;
    }
    case 0x2F:                                      /* rand */
        s_rand_seed = s_rand_seed * 1103515245u + 12345u;
        cpu->gpr[2] = (s_rand_seed >> 16) & 0x7FFFu;
        return 1;
    case 0x30:                                      /* srand */
        s_rand_seed = a0;
        cpu->gpr[2] = 0;
        return 1;
    case 0x39:                                      /* InitHeap */
        s_heap_base = a0; s_heap_size = a1;
        cpu->gpr[2] = 0;
        return 1;
    case 0x3F:                                      /* printf */
        hle_printf(cpu, a0);
        cpu->gpr[2] = 0;
        return 1;
    case 0x44:                                      /* FlushCache */
        cpu->gpr[2] = 0;
        return 1;
    case 0x49:                                      /* GPU_cw */
        ww(0x1F801810u, a0);
        cpu->gpr[2] = 0;
        return 1;
    /* Те же файловые функции доступны и через вектор A0 (FileOpen и т.д.) —
     * DC зовёт версии B0, но пусть работают обе. */
    case 0x00:                                      /* FileOpen */
        cpu->gpr[2] = (uint32_t)hle_file_open(cpu, a0, a1);
        return 1;
    case 0x01:                                      /* FileSeek */
        cpu->gpr[2] = (uint32_t)hle_file_seek((int)a0, (int)a1, (int)a2);
        return 1;
    case 0x02:                                      /* FileRead */
        cpu->gpr[2] = (uint32_t)hle_file_rw(cpu, (int)a0, a1, (int)a2, 0);
        return 1;
    case 0x03:                                      /* FileWrite */
        cpu->gpr[2] = (uint32_t)hle_file_rw(cpu, (int)a0, a1, (int)a2, 1);
        return 1;
    case 0x04:                                      /* FileClose */
        cpu->gpr[2] = (uint32_t)hle_file_close_cpu(cpu, (int)a0);
        return 1;
    case 0x70:                                      /* _bu_init */
        cpu->gpr[2] = 0;
        return 1;
    case 0x72:                                      /* CdRemove */
        cpu->gpr[2] = 0;
        return 1;
    case 0xAB:                                      /* _card_info */
        /* Ответ идёт обоими классами (HwCARD + SwCARD) — см. card_deliver:
         * игра опрашивает карту этим вызовом полсотни раз за сеанс и по
         * ответу решает, готова ли она вообще работать с сохранениями. */
        if (memcard_is_present(card_of(a0))) card_cache_dir(cpu, card_of(a0));
        card_deliver(cpu, memcard_is_present(card_of(a0)) ? CARD_SPEC_DONE
                                                          : CARD_SPEC_ERROR);
        cpu->gpr[2] = 1;
        return 1;
    case 0xAC:                                      /* _card_load_dir */
        card_cache_dir(cpu, card_of(a0));           /* реальная загрузка директории */
        card_deliver(cpu, CARD_SPEC_DONE);
        cpu->gpr[2] = 1;
        return 1;
    default:
        return 0;
    }
}

static int hle_call_b0(CPUState* cpu, uint32_t fn) {
    uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5];
    char buf[512];
    switch (fn) {
    case 0x07: hle_DeliverEvent(cpu, a0, a1); cpu->gpr[2] = 0; return 1;
    case 0x08: cpu->gpr[2] = hle_OpenEvent(cpu);      return 1;
    case 0x09: cpu->gpr[2] = hle_CloseEvent(a0);      return 1;
    case 0x0A: cpu->gpr[2] = hle_WaitEvent(cpu, a0);  return 1;
    case 0x0B: cpu->gpr[2] = hle_TestEvent(a0);       return 1;
    case 0x0C: cpu->gpr[2] = hle_EnableEvent(a0);     return 1;
    case 0x0D: cpu->gpr[2] = hle_DisableEvent(a0);    return 1;
    case 0x0E:                                        /* OpenTh(pc, sp, gp) */
        cpu->gpr[2] = hle_OpenTh(a0, a1, cpu->gpr[6]);
        return 1;
    case 0x0F: cpu->gpr[2] = hle_CloseTh(a0);         return 1;
    case 0x10: cpu->gpr[2] = hle_ChangeTh(cpu, a0);   return 1;
    case 0x17:                                        /* ReturnFromException */
        s_rfe_calls++;
        cpu->pc = 0;
        psx_exception_longjmp();                      /* не возвращается */
        return 1;
    case 0x18:                                        /* ResetEntryInt */
        s_entry_hook = 0;
        cpu->gpr[2] = 0;
        return 1;
    case 0x19:                                        /* HookEntryInt */
        /* Аргумент — не элемент цепочки, а четвёрка { pc, gp, sp, fp }:
         * обработчик со своим контекстом. У Dino Crisis это pc=0x80087B00
         * (её CD-драйвер), проверено по дампу RAM ретейл-машины. */
        /* Игра переустанавливает hook постоянно (libcd делает это вокруг
         * каждой критической секции) — печатаем только смену адреса. */
        if (s_entry_hook != a0) {
            fprintf(stderr, "hle: HookEntryInt %08X (pc=%08X gp=%08X sp=%08X)\n",
                    a0, rw(a0), rw(a0 + 4), rw(a0 + 8));
            s_entry_hook = a0;
        }
        cpu->gpr[2] = 0;
        return 1;
    case 0x3F:                                        /* puts */
        guest_str(cpu, a0, buf, sizeof buf);
        fprintf(stderr, "hle/game: %s\n", buf);
        cpu->gpr[2] = 0;
        return 1;
    case 0x51:                                        /* Krom2RawAdd */
        cpu->gpr[2] = 0;                              /* японский шрифт не нужен */
        return 1;
    case 0x12:                                        /* InitPAD */
        s_pad_buf[0] = a0; s_pad_len[0] = a1;
        s_pad_buf[1] = cpu->gpr[6]; s_pad_len[1] = cpu->gpr[7];
        fprintf(stderr, "hle: InitPAD buffers %08X(%u) and %08X(%u)\n",
                s_pad_buf[0], s_pad_len[0], s_pad_buf[1], s_pad_len[1]);
        cpu->gpr[2] = 0;
        return 1;
    case 0x13:                                        /* StartPAD */
        s_pad_started = 1;
        cpu->gpr[2] = 0;
        return 1;
    case 0x14:                                        /* StopPAD */
        s_pad_started = 0;
        cpu->gpr[2] = 0;
        return 1;
    case 0x15:                                        /* PAD_init */
        /* Второй, «низкоуровневый» вход: буфер приходит в $a1. */
        s_pad_buf[0] = a1; s_pad_len[0] = 34;
        s_pad_started = 1;
        cpu->gpr[2] = 1;
        return 1;
    /* Файлы карты памяти: именно так сохраняется игра (см. разбор выше). */
    case 0x32:                                        /* open(name, mode) */
        cpu->gpr[2] = (uint32_t)hle_file_open(cpu, a0, a1);
        return 1;
    case 0x33:                                        /* lseek(fd, off, whence) */
        cpu->gpr[2] = (uint32_t)hle_file_seek((int)a0, (int)a1, (int)cpu->gpr[6]);
        return 1;
    case 0x34:                                        /* read(fd, dst, len) */
        cpu->gpr[2] = (uint32_t)hle_file_rw(cpu, (int)a0, a1, (int)cpu->gpr[6], 0);
        return 1;
    case 0x35:                                        /* write(fd, src, len) */
        cpu->gpr[2] = (uint32_t)hle_file_rw(cpu, (int)a0, a1, (int)cpu->gpr[6], 1);
        return 1;
    case 0x36:                                        /* close(fd) */
        cpu->gpr[2] = (uint32_t)hle_file_close_cpu(cpu, (int)a0);
        return 1;
    case 0x42:                                        /* firstfile */
        cpu->gpr[2] = hle_firstfile(cpu, a0, a1);
        return 1;
    case 0x43:                                        /* nextfile */
        cpu->gpr[2] = hle_nextfile(cpu, a0);
        return 1;
    case 0x4A:                                        /* InitCARD */
    case 0x4C:                                        /* StopCARD */
        card_cache_dir(cpu, 0); card_cache_dir(cpu, 1);
        cpu->gpr[2] = 0;
        return 1;
    case 0x4B:                                        /* StartCARD */
        /* Кэш директории держим актуальным с самого включения карты: игра
         * может прочитать его в любой момент, а не только сразу после
         * _card_load_dir. */
        card_cache_dir(cpu, 0); card_cache_dir(cpu, 1);
        cpu->gpr[2] = 1;
        return 1;
    case 0x4E:                                        /* _card_write_sector */
        cpu->gpr[2] = hle_card_write(cpu, a0, a1, cpu->gpr[6]);
        return 1;
    case 0x4F:                                        /* _card_read_sector */
        cpu->gpr[2] = hle_card_read(cpu, a0, a1, cpu->gpr[6]);
        return 1;
    case 0x50:                                        /* _new_card */
        cpu->gpr[2] = 0;
        return 1;
    case 0x5C:                                        /* _card_status */
        cpu->gpr[2] = memcard_is_present(card_of(a0)) ? 1u : 0u;
        return 1;
    case 0x54:                                        /* _get_errno */
    case 0x55:                                        /* _get_error */
        cpu->gpr[2] = 0;
        return 1;
    case 0x56:                                        /* GetC0Table */
        cpu->gpr[2] = 0x80000000u + TBL_C0;
        return 1;
    case 0x57:                                        /* GetB0Table */
        cpu->gpr[2] = 0x80000000u + TBL_B0;
        return 1;
    case 0x5B:                                        /* ChangeClearPad */
        cpu->gpr[2] = 0;
        return 1;
    default:
        return 0;
    }
}

static int hle_call_c0(CPUState* cpu, uint32_t fn) {
    uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5];
    switch (fn) {
    case 0x02: hle_SysEnqIntRP(a0, a1); cpu->gpr[2] = 0; return 1;
    case 0x03: hle_SysDeqIntRP(a0, a1); cpu->gpr[2] = 0; return 1;
    case 0x0A: {                                      /* ChangeClearRCnt */
        /* Ставит флаг автоподтверждения счётчика n и возвращает прежний.
         * Игра снимает флаг вертикального гашения, чтобы прерывание доходило
         * до её собственного обработчика, а не гасилось ядром. */
        uint32_t n = a0 & 3u;
        uint32_t old = rw(RCNT_CLEAR_FLAG(n));
        ww(RCNT_CLEAR_FLAG(n), a1);
        cpu->gpr[2] = old;
        return 1;
    }
    default:
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * Перехват диспетчера
 * ------------------------------------------------------------------------ */

int hle_bios_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys) {
    (void)addr;
    if (!hle_bios_enabled()) return 0;

    if (phys == HLE_EXC_HANDLER) { hle_exception(cpu); return 1; }

    int vec;
    if      (phys == HLE_VEC_A0 || phys == HLE_STUB_A0) vec = 0;
    else if (phys == HLE_VEC_B0 || phys == HLE_STUB_B0) vec = 1;
    else if (phys == HLE_VEC_C0 || phys == HLE_STUB_C0) vec = 2;
    else return 0;

    uint32_t fn = cpu->gpr[9] & 0xFFu;      /* номер функции лежит в $t1 */

    /* Запись таблицы подменена игрой — значит эту функцию ядра она решила
     * обслуживать сама; отдаём управление её коду. */
    uint32_t own = table_override(vec, fn);
    if (own) {
        cpu->pc = own;
        return 1;
    }

    /* Периодическая сводка: видно, что игра успела позвать и на чём стоит,
     * не останавливая процесс. DC_HLE_REPORT=0 — выключить. */
    {
        static unsigned long s_since = 0;
        /* 17.09.2026 вечер: getenv на КАЖДОМ вызове ядра (в профиле он прятался под именем strchr) — теперь один раз */
        static int s_rep_on = -1;
        if (s_rep_on < 0) { const char* r = getenv("DC_HLE_REPORT"); s_rep_on = (!r || *r != '0') ? 1 : 0; }
        if (s_rep_on && ++s_since >= 20000UL) {
            s_since = 0;
            hle_bios_report();
        }
    }

    /* DC_HLE_FNRING=1 — часто печатать кольцо последних вызванных функций
     * игры. Это единственный способ увидеть, какой код она реально крутит:
     * рекомпилированные функции зовут друг друга напрямую, мимо диспетчера. */
    {
        static int fnring = -1;
        static unsigned long s_fn_since = 0;
        if (fnring < 0) { const char* e = getenv("DC_HLE_FNRING"); fnring = (e && *e == '1') ? 1 : 0; }
        if (fnring && ++s_fn_since >= 3000UL) {
            s_fn_since = 0;
            extern volatile uint32_t g_dc_fn_ring[32];
            extern volatile uint32_t g_dc_fn_pos;
            fprintf(stderr, "fnring:");
            for (int k = 1; k <= 32; k++)
                fprintf(stderr, " %08X", g_dc_fn_ring[(g_dc_fn_pos - (uint32_t)k) & 31u]);
            fprintf(stderr, "\n");
        }
    }

    /* DC_HLE_TRACE=1 — печатать КАЖДЫЙ вызов ядра с аргументами. Нужен, когда
     * надо увидеть точную последовательность игры (09.09: чем именно она
     * читает список сохранений). Шумно: строка на вызов. */
    {
        static int tr = -1;
        if (tr < 0) { const char* e = getenv("DC_HLE_TRACE"); tr = (e && *e == '1') ? 1 : 0; }
        if (tr && !(vec == 1 && (fn == 0x0B || fn == 0x10)))   /* без TestEvent/ChangeTh — их миллионы */
            fprintf(stderr, "hle> %s(%02X) %-16s a0=%08X a1=%08X a2=%08X ra=%08X\n",
                    VECNAME[vec], fn, hle_name(vec, fn),
                    cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[31]);
    }
    int done = (vec == 0) ? hle_call_a0(cpu, fn)
             : (vec == 1) ? hle_call_b0(cpu, fn)
                          : hle_call_c0(cpu, fn);
    if (done) {
        s_calls[vec][fn]++;
    } else {
        if (!s_missing[vec][fn])
            fprintf(stderr, "hle: NOT IMPLEMENTED %s(%02X) %s - returned 0 (ra=%08X)\n",
                    VECNAME[vec], fn, hle_name(vec, fn), cpu->gpr[31]);
        s_missing[vec][fn]++;
        cpu->gpr[2] = 0;
    }
    cpu->pc = 0;
    return 1;
}

/* --------------------------------------------------------------------------
 * Бут без BIOS: разложить ядро и загрузить PS-X EXE
 * ------------------------------------------------------------------------ */

/* Трамплин `lui rN,hi / addiu rN,rN,lo / jr rN / nop` — тот же вид, что
 * пишет ретейл-BIOS: его узнаёт и наш резолвер трамплинов в traps.c. */
static void write_trampoline(uint32_t at, uint32_t target, int reg) {
    uint32_t hi = (target >> 16) & 0xFFFFu;
    uint32_t lo = target & 0xFFFFu;
    if (lo & 0x8000u) hi++;                       /* компенсация знака addiu */
    ww(at + 0, 0x3C000000u | ((uint32_t)reg << 16) | hi);
    ww(at + 4, 0x24000000u | ((uint32_t)reg << 21) | ((uint32_t)reg << 16) | lo);
    ww(at + 8, 0x00000008u | ((uint32_t)reg << 21));
    ww(at + 12, 0x00000000u);
}

uint32_t hle_bios_boot(CPUState* cpu, const char* exe_path, uint32_t stack_base) {
    s_cpu = cpu;
    /* 23.10.2026: the log names the file, not the folder - no player's path in it */
    const char* exe_name = exe_path;
    for (const char* q = exe_path; *q; q++) if (*q == '\\' || *q == '/') exe_name = q + 1;
    FILE* f = fopen(exe_path, "rb");
    if (!f) {
        fprintf(stderr, "hle: cannot open the game's EXE: %s\n", exe_name);
        return 0;
    }
    uint8_t hdr[0x800];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fprintf(stderr, "hle: PS-X EXE header too short: %s\n", exe_name);
        fclose(f);
        return 0;
    }
    if (memcmp(hdr, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "hle: not a PS-X EXE: %s\n", exe_name);
        fclose(f);
        return 0;
    }
    uint32_t pc0    = *(uint32_t*)(hdr + 0x10);
    uint32_t gp0    = *(uint32_t*)(hdr + 0x14);
    uint32_t t_addr = *(uint32_t*)(hdr + 0x18);
    uint32_t t_size = *(uint32_t*)(hdr + 0x1C);
    uint32_t s_addr = *(uint32_t*)(hdr + 0x30);
    uint32_t s_size = *(uint32_t*)(hdr + 0x34);

    /* Тело EXE в RAM. */
    uint8_t* body = (uint8_t*)malloc(t_size);
    if (!body) { fclose(f); return 0; }
    size_t got = fread(body, 1, t_size, f);
    fclose(f);
    { extern int dc_mods_exe_patch(uint32_t t_addr, uint8_t *body, uint32_t size);   /* 05.10: таблицы и тексты из mods */
      if (got == t_size) dc_mods_exe_patch(t_addr, body, (uint32_t)got); }
    for (uint32_t i = 0; i < got; i++) cpu->write_byte(t_addr + i, body[i]);
    free(body);

    /* Пометить текст игры как исполняемый код в RAM. Иначе функции, которых
     * нет в статической таблице (их зовут только по указателю — например
     * обработчик прерываний CD-драйвера 0x80087B00), не выполнит никто:
     * интерпретатор берётся лишь за страницы, помеченные исполняемыми. */
    dirty_ram_mark_executable_range(t_addr & 0x1FFFFFFFu, t_size);

    /* Векторы и трамплины — как их ставит ретейл-BIOS. */
    write_trampoline(0x80000080u, 0x80000000u + HLE_EXC_HANDLER, 26); /* $k0 */
    write_trampoline(0x800000A0u, 0x80000000u + HLE_STUB_A0, 8);      /* $t0 */
    write_trampoline(0x800000B0u, 0x80000000u + HLE_STUB_B0, 8);
    write_trampoline(0x800000C0u, 0x80000000u + HLE_STUB_C0, 8);

    /* Таблица таблиц: адрес и размер каждой области ядра. */
    ww(0x80000100u, 0xA0000000u + HLE_EXCB_ADDR); ww(0x80000104u, HLE_EXCB_SIZE);
    ww(0x80000108u, 0xA0000000u + HLE_PCB_ADDR);  ww(0x8000010Cu, HLE_PCB_SIZE);
    ww(0x80000110u, 0xA0000000u + HLE_TCB_ADDR);  ww(0x80000114u, HLE_TCB_SIZE);

    /* Вершина стека ядра там же, где её держит ретейл — в ячейке 0x6CF0.
     * Область 0xE600..0xFF00 у нас свободна: EvCB кончается на 0xE290,
     * TCB — на 0xE59C, код игры начинается с 0x10000. */
    ww(HLE_KSTACK_PTR, 0x8000FF00u);
    ww(0x80000120u, 0xA0000000u + HLE_EVCB_ADDR); ww(0x80000124u, HLE_EVCB_SIZE);

    for (uint32_t i = 0; i < HLE_EVCB_SIZE; i += 4) ww(0x80000000u + HLE_EVCB_ADDR + i, 0);
    for (uint32_t i = 0; i < HLE_TCB_SIZE;  i += 4) ww(0x80000000u + HLE_TCB_ADDR  + i, 0);
    ww(0x80000000u + HLE_TCB_ADDR, 0x4000u);        /* поток 0 занят */
    ww(0x80000000u + HLE_PCB_ADDR, 0xA0000000u + HLE_TCB_ADDR);
    for (int p = 0; p < IRQ_PRIO; p++) s_irq_chain[p] = 0;
    tables_init();
    hle_dcb_init();   /* таблица устройств: по ней игра находит «bu» (см. выше) */

    /* Автоподтверждение счётчиков включено по умолчанию — так ядро ретейла
     * ведёт себя после инициализации (проверено по дампу рабочей машины:
     * таймеры 0/1/2 = 1, вертикальное гашение = 0, потому что его игра
     * снимает сама через ChangeClearRCnt). Без этого прерывание таймера 2
     * никто не гасил: обработчик игры трогает только те источники, на
     * которые у неё есть обратный вызов, а остальное считает зависшим —
     * отсюда её жалоба «intr timeout(0040:004d)». */
    for (int i = 0; i < 4; i++) ww(RCNT_CLEAR_FLAG(i), 1);

    /* Регистры на входе в игру. */
    for (int i = 1; i < 32; i++) cpu->gpr[i] = 0;
    cpu->gpr[28] = gp0;                                     /* $gp */
    cpu->gpr[29] = cpu->gpr[30] = stack_base ? stack_base
                 : (s_addr ? s_addr + s_size : 0x801FFF00u); /* $sp/$fp */
    cpu->pc = pc0;
    cpu->cop0[12] = 0x00000000u;   /* SR: BEV=0, прерывания включит игра */

    fprintf(stderr,
            "hle: own kernel - loaded %s (pc=%08X gp=%08X text=%08X..%08X sp=%08X)\n",
            exe_name, pc0, gp0, t_addr, t_addr + t_size, cpu->gpr[29]);
    return pc0;
}
