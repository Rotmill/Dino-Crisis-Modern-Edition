/* ==========================================================================
 * bios_hle.h — своё ядро PS1 вместо ретейл-BIOS.
 *
 * Зачем: чтобы мод можно было раздавать без дампа SCPH1001 (образ Sony
 * распространять нельзя). Ядро реализовано нативно: игра зовёт его через
 * векторы 0xA0/0xB0/0xC0, мы перехватываем вызов в диспетчере и выполняем
 * функцию на стороне хоста.
 *
 * Замер, с которого всё начиналось: Dino Crisis зовёт 55 функций ядра
 * (18 через A0, 34 через B0, 3 через C0) и НИ ОДНОЙ функции привода —
 * приводом игра управляет сама. Список снят сканом стабов в SLUS_009.22.
 * ======================================================================== */
#ifndef PSX_BIOS_HLE_H
#define PSX_BIOS_HLE_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Раскладка ядра в гостевой RAM. Адреса взяты НЕ с потолка: они сняты с
 * работающей машины на ретейл-BIOS (quicksave → дамп RAM), чтобы игра и
 * наши отладочные инструменты видели ровно то же, что видели всегда. */
#define HLE_VEC_A0        0x000000A0u
#define HLE_VEC_B0        0x000000B0u
#define HLE_VEC_C0        0x000000C0u
#define HLE_STUB_A0       0x000005C4u   /* куда прыгает трамплин вектора A0 */
#define HLE_STUB_B0       0x000005E0u
#define HLE_STUB_C0       0x00000600u
#define HLE_EXC_HANDLER   0x00000C80u   /* обработчик исключений */

#define HLE_EXCB_ADDR     0x0000E004u   /* таблица обработчиков прерываний */
#define HLE_EXCB_SIZE     0x00000020u
#define HLE_PCB_ADDR      0x0000E294u   /* текущий процесс */
#define HLE_PCB_SIZE      0x00000004u
#define HLE_TCB_ADDR      0x0000E29Cu   /* потоки, 4 шт. по 0xC0 */
#define HLE_TCB_SIZE      0x00000300u
#define HLE_EVCB_ADDR     0x0000E028u   /* события, 22 шт. по 0x1C */
#define HLE_EVCB_SIZE     0x00000268u

/* Включено ли своё ядро (сборка psx-runtime-hle либо DC_HLE_BIOS=1). */
int  hle_bios_enabled(void);

/* Разложить ядро в RAM и загрузить PS-X EXE игры. Возвращает точку входа
 * (0 = не удалось). После этого рантайм исполняет игру напрямую, без бута. */
uint32_t hle_bios_boot(CPUState* cpu, const char* exe_path, uint32_t stack_base);

/* Перехват диспетчера. Возвращает 1, если адрес принадлежит ядру и вызов
 * выполнен (результат уже в $v0, cpu->pc обнулён). */
int  hle_bios_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);

/* Сводка вызовов за сессию — что игра звала и сколько раз, включая то,
 * что ещё не реализовано. Печатается по DC_HLE_REPORT=1 и при выходе. */
void hle_bios_report(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BIOS_HLE_H */
