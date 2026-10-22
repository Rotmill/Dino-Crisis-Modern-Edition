/* ==========================================================================
 * hle_dispatch.c — диспетчер для сборки со своим ядром (без генкода BIOS).
 *
 * В обычной сборке psx_dispatch приходит из generated/SCPH1001_dispatch.c
 * вместе с таблицей из 5687 рекомпилированных функций ретейл-BIOS. Здесь
 * этой таблицы нет вовсе: вызовы ядра обслуживает bios_hle.c, всё
 * остальное — рекомпилированный код игры и интерпретатор.
 *
 * Порядок разбора цели (важен именно такой):
 *   1) функция игры          — статически рекомпилирована, самый частый путь;
 *   2) ядро                  — векторы A0/B0/C0 и обработчик исключений;
 *   3) «грязная» RAM         — оверлеи и код, записанный игрой на ходу;
 *   4) psx_unknown_dispatch  — резолвер трамплинов и аварийная диагностика.
 * ======================================================================== */

#include "cpu_state.h"
#include "bios_hle.h"

#include <stdint.h>
#include <stdio.h>

extern int  psx_game_address_in_text(uint32_t addr);
extern int  psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr);
extern int  dirty_ram_dispatch(CPUState* cpu, uint32_t addr);
extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);
extern void psx_check_interrupts(CPUState* cpu);
extern void fntrace_record(CPUState* cpu, uint32_t target);
/* На этой паре держится ВСЯ отладочная трассировка вызовов (fntrace_arm,
 * счётчики функций, кольца отладчика). В штатном диспетчере из генкода BIOS
 * они вызываются перед каждой функцией; пока их не было здесь, любой замер
 * «сколько раз вызвана функция игры» давал ноль — и это увело диагностику
 * чёрного экрана по ложному следу «поток не исполняется». */
extern void debug_server_trace_dispatch(uint32_t func_addr);

extern uint64_t g_dispatch_static_hits;      /* debug_server.c */
extern uint32_t g_debug_current_func_addr;   /* debug_server.c */
int             g_psx_dispatch_depth = 0;

static void dispatch_impl(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    int outermost = (g_psx_dispatch_depth++ == 0);
    for (;;) {
        fntrace_record(cpu, addr);
        cpu->pc = 0;
        uint32_t phys = addr & 0x1FFFFFFFu;
        int found = 0;

        if (psx_game_address_in_text(addr)) {
            g_debug_current_func_addr = phys;
            debug_server_trace_dispatch(phys);
            found = psx_dispatch_game_compiled(cpu, addr);
            if (found) g_dispatch_static_hits++;
        }
        if (!found) found = hle_bios_dispatch(cpu, addr, phys);
        if (!found) found = dirty_ram_dispatch(cpu, addr);
        if (!found) psx_unknown_dispatch(cpu, addr, phys);

        if (cpu->pc == 0) {
            --g_psx_dispatch_depth;
            if (outermost) psx_check_interrupts(cpu);
            return;
        }
        if (stop_addr != 0 && cpu->pc == stop_addr) {
            cpu->pc = 0;
            --g_psx_dispatch_depth;
            if (outermost) psx_check_interrupts(cpu);
            return;
        }
        addr = cpu->pc;   /* хвостовой вызов: разбираем цель заново */
    }
}

void psx_dispatch(CPUState* cpu, uint32_t addr) {
    dispatch_impl(cpu, addr, 0);
}

void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr) {
    dispatch_impl(cpu, addr, return_addr);
}
