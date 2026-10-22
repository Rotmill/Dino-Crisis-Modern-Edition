#ifndef PSXRECOMP_PSX_FASTMEM_H
#define PSXRECOMP_PSX_FASTMEM_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ⭐⭐ ОБРАЩЕНИЯ К ПАМЯТИ БЕЗ ВЫЗОВОВ (09.09.2026, «ускорить кратно»).
 *
 * Как было. Каждое обращение рекомпилированного кода к памяти уходило в
 * memory.c: чтение байта и запись байта — через УКАЗАТЕЛЬ НА ФУНКЦИЮ в
 * CPUState (5006 и 3895 мест в генкоде), то есть косвенный переход, который
 * процессор не предсказывает. А на КАЖДОЙ записи в основную память висела
 * очередь наблюдателей — до десяти межмодульных вызовов подряд:
 *   dc_wwatch, dc_drawlist_watch, dc_speed_spike, dc_cam_watch, dc_light_watch,
 *   dc_ammo_write_note, трассировщик записей, слежение за картой памяти,
 *   пометка грязных страниц, инвалидация теней PGXP.
 * Почти все они либо выключены (отладочные ключи), либо смотрят на считаные
 * адреса — но проверялись на каждой записи, потому что жили внутри одной
 * функции psx_write_word.
 *
 * Как стало. Кому нужен медленный путь, решает КАРТА СТРАНИЦ по 256 байт
 * (g_psx_wslow, 8 КБ на все 2 МБ гостевой памяти): её раз в кадр
 * пересобирает psx_wslow_refresh() из тех же условий, что стояли внутри
 * наблюдателей. Быстрый путь — три проверки и одна запись; всё, что попало
 * под наблюдение, идёт по прежней дороге БЕЗ ИЗМЕНЕНИЙ. Чтения наблюдателей
 * не имели никогда, поэтому чтение из основной памяти — просто загрузка.
 *
 * ⛔ Добавляешь новый наблюдатель за записью в memory.c — обязан отметить его
 * адреса в psx_wslow_refresh(), иначе он замолчит на быстром пути.
 * Выключить целиком: DC_FASTMEM=0 (тогда g_psx_wslow_all=1 и всё идёт как
 * раньше) — это же и способ проверить, что баг не от этой правки. */

/* 17.09.2026 вечер (пункт 5): принудительное встраивание крошечных помощников памяти/PGXP — в огромных функциях генкода
 * компилятор даже с /Ob2 оставлял их вызовами. -DDC_NO_FORCE_INLINE — обычный static inline, как было. */
#ifndef PSX_HOT_INLINE
#  if defined(DC_NO_FORCE_INLINE)
#    define PSX_HOT_INLINE static inline
#  elif defined(_MSC_VER)
#    define PSX_HOT_INLINE static __forceinline
#  elif defined(__GNUC__)
#    define PSX_HOT_INLINE static inline __attribute__((always_inline))
#  else
#    define PSX_HOT_INLINE static inline
#  endif
#endif
#define PSX_FM_RAM_BYTES  0x200000u          /* 2 МБ основной памяти */
#define PSX_FM_PAGE_SHIFT 8                  /* страница карты — 256 байт */
#define PSX_FM_PAGES      (PSX_FM_RAM_BYTES >> PSX_FM_PAGE_SHIFT)

extern uint8_t *const g_psx_ram;             /* memory.c: основная память */
extern uint8_t        g_psx_wslow[PSX_FM_PAGES];  /* 1 = странице нужен полный путь */
extern int            g_psx_wslow_all;       /* 1 = быстрый путь записи отключён */
extern const uint32_t *g_psx_sr_pub;         /* cop0 SR — бит IsC (изоляция кэша) */
extern uint32_t       *g_psx_dirty_bm;       /* карта грязных страниц (4 КБ) */

/* полные пути (memory.c) — сюда уходит всё, что не попало в быстрый */
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint8_t  psx_read_byte(uint32_t addr);
void     psx_write_word(uint32_t addr, uint32_t val);
void     psx_write_half(uint32_t addr, uint16_t val);
void     psx_write_byte(uint32_t addr, uint8_t val);
void     psx_wslow_refresh(void);            /* пересобрать карту (раз в кадр) */

/* --- чтение: наблюдателей нет, основная память — прямая загрузка --------- */
PSX_HOT_INLINE uint32_t psx_rw_f(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_FM_RAM_BYTES) { uint32_t v; memcpy(&v, g_psx_ram + phys, 4); return v; }
    return psx_read_word(addr);
}
PSX_HOT_INLINE uint16_t psx_rh_f(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_FM_RAM_BYTES) { uint16_t v; memcpy(&v, g_psx_ram + phys, 2); return v; }
    return psx_read_half(addr);
}
PSX_HOT_INLINE uint8_t psx_rb_f(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_FM_RAM_BYTES) return g_psx_ram[phys];
    return psx_read_byte(addr);
}

/* --- запись ------------------------------------------------------------- */
/* Пометка грязных страниц: тот же закон, что в memory.c
 * (низкие 64 КБ — заглушки ядра, выше конца текста игры — оверлеи). */
#define PSX_FM_KERNEL_BYTES 0x10000u
#define PSX_FM_TEXT_END     0xB2000u
PSX_HOT_INLINE void psx_fm_mark_dirty(uint32_t phys) {
    if (phys < PSX_FM_KERNEL_BYTES || phys >= PSX_FM_TEXT_END) {
        uint32_t page = phys >> 12;
        g_psx_dirty_bm[page >> 5] |= (1u << (page & 31u));
    }
}
PSX_HOT_INLINE int psx_fm_wr_ok(uint32_t phys) {
    return phys < PSX_FM_RAM_BYTES
        && !g_psx_wslow_all
        && !g_psx_wslow[phys >> PSX_FM_PAGE_SHIFT]
        && !(g_psx_sr_pub && (*g_psx_sr_pub & 0x10000u));
}

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_FASTMEM_H */
