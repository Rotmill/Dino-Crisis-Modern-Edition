/*
 * pgxp_addr.h - sub-pixel vertex tracking by memory address (our implementation, 08.10.2026).
 *
 * The idea (known as PGXP) is to let the precise result of every GTE projection travel together with the
 * 16-bit screen word the game copies around: GTE SXY registers -> CPU registers -> RAM (the display list) ->
 * GP0 packet. Each tracked place keeps a small "cell" with the precise x, y and depth plus the exact 32-bit word
 * it describes; whoever reads a cell checks that word against the real one, so anything the tracker did not
 * see simply misses (the renderer then uses the integer position) instead of handing out a wrong vertex.
 *
 * Producer: gte.cpp (pgxp_addr_rtps_push). Movers: the inline wrappers below, called by the generated code
 * (tools/pgxp_transform.py rewrites the matching loads, stores and moves). Untracked stores: memory.c calls
 * pgxp_addr_inval_word / pgxp_addr_inval_half. Consumer: gpu.c (pgxp_addr_get_vertex with the RAM address of
 * every vertex word of a GP0 command).
 *
 * Env: DC_PGXP_ADDR=0 off (the value-keyed cache in pgxp.h stays), DC_PGXP_ARENA=0 no cells for the packet
 * arena, DC_PGXP_RESEED=0 no rebuild of display-list words from the value-keyed cache.
 */
#ifndef PSXRECOMP_PGXP_ADDR_H
#define PSXRECOMP_PGXP_ADDR_H


#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_fastmem.h"   /* page map + fast reads */

#ifdef __cplusplus
extern "C" {
#endif

/* What a cell knows (PxCell.bits). */
#define PX_XOK    0x01u   /* low halfword: precise x known */
#define PX_YOK    0x02u   /* high halfword: precise y known */
#define PX_ZOK    0x04u   /* depth known */
#define PX_ZLO    0x08u   /* depth came in with a store of the low halfword */
#define PX_ZHI    0x10u   /* ... of the high halfword */
#define PX_WHOLE  0x20u   /* written as one whole word (sw / swc2); two halfword stores can join an x of this frame
                           * with a y of the last one - the word still matches, the point is garbage; the renderer
                           * wants WHOLE */
#define PX_ACTOR  0x40u   /* the vertex came from the character vertex pool (classification, debug colour) */
#define PX_XY     (PX_XOK | PX_YOK)

typedef struct PxCell {
    float    x, y;        /* precise sub-pixel screen position */
    float    z;           /* precise depth (w for the rasterizer) */
    uint32_t raw;         /* the 32-bit word this cell describes */
    uint32_t bits;        /* PX_* */
    uint32_t origin;      /* 12.09.2026: RAM address of the VXY word the vertex entered the GTE from (lwc2 / mtc2 ->
                           * RTPS / RTPT); 0 = unknown. Rides along to the GP0 packet; gpu.c recognises room polygon
                           * records by it (z-fight, F8 / G). */
} PxCell;

/* source address of a packet word's vertex (0 = none), see pgxp_addr.c */
uint32_t pgxp_addr_vertex_src(uint32_t addr, uint32_t value);
extern int g_pgxp_rtp_vi;    /* which vertex (V0/V1/V2) is being projected right now: gte.cpp */

/* Cells: main RAM (2 MB), scratchpad (1 KB), packet arena (memory.c PSX_EXT_BASE, 12 MB) - one per 32-bit word. */
#define PX_RAM_CELLS   (0x200000u >> 2)
#define PX_SPAD_CELLS  (0x400u >> 2)
#define PX_ARENA_PHYS  0x00400000u     /* 07.10.2026: since 06.10 the frame packets live there (DC_ARENA_EXT); without
                                        * cells the hand, weapon and hair took their depth from the whole-pixel cache and
                                        * two vertices in one pixel swapped places (the hand flickered over the weapon) */
#define PX_ARENA_CELLS ((12u << 20) >> 2)
extern int g_px_arena;                  /* DC_PGXP_ARENA (1) */

extern int      g_pgxp_addr_on;         /* pgxp_addr_boot: DC_PGXP_ADDR */
extern int      g_px_reseed;            /* DC_PGXP_RESEED */
extern PxCell*  g_px_cell;              /* RAM, scratchpad, arena cells */
extern PxCell   g_px_reg[32];           /* CPU register cells ($0 never valid) */
extern uint32_t* g_px_live;             /* one bit per cell, set = worth looking at; clear = treat the cell as empty.
                                         * Keeps the untracked fast paths in 64 KB instead of the 10 MB of cells
                                         * (touching the cells there cost ~11 % of the frame rate). */

/* 17.09.2026 evening: force-inline the tiny memory/PGXP helpers - inside the huge generated functions the compiler
 * left them as calls even with /Ob2. -DDC_NO_FORCE_INLINE - plain static inline. */
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

void pgxp_addr_boot(void);        /* env + allocation, once at startup */

/* Cell number of a guest address, -1 outside RAM / scratchpad / arena (same mapping as memory.c). */
PSX_HOT_INLINE int32_t px_index(uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x200000u) return (int32_t)(phys >> 2);
    if (phys - 0x1F800000u < 0x400u) return (int32_t)(PX_RAM_CELLS + ((phys - 0x1F800000u) >> 2));
    if (g_px_arena && phys - PX_ARENA_PHYS < (PX_ARENA_CELLS << 2))
        return (int32_t)(PX_RAM_CELLS + PX_SPAD_CELLS + ((phys - PX_ARENA_PHYS) >> 2));
    return -1;
}

PSX_HOT_INLINE int px_is_live(uint32_t addr) {
    const int32_t i = px_index(addr);
    return i >= 0 && ((g_px_live[i >> 5] >> (i & 31)) & 1u);
}

/* GTE: one call per projected vertex with its precise result (valid = 0 keeps the shadow FIFO in step with the
 * real SXY FIFO when there is nothing precise to say). */
void pgxp_addr_rtps_push(float fx, float fy, float w, uint32_t packed, int valid);

/* GPU: addr = RAM address of a vertex word, value = the word. 0 = nothing, 1 = precise x/y (w = 1),
 * 2 = precise x/y and depth. */
int pgxp_addr_get_vertex(uint32_t addr, uint32_t value, float* fx, float* fy, float* w);

extern long g_pgxp_addr_hit, g_pgxp_addr_zhit;   /* counters (gte.cpp DC_PGXP_STAT) */
/* set by every pgxp_addr_get_vertex call: 1 when the cell is a character pool vertex (gpu.c DC_DBG_PRIM=2) */
extern int g_pgxp_last_actor;

/* Stores the tracker did not route (memory.c and the fast writes below): a word or byte store empties the cell,
 * a halfword store forgets that half. */
void px_forget_half(uint32_t addr, uint16_t newval);

PSX_HOT_INLINE void pgxp_addr_inval_word(uint32_t addr) {
    const int32_t i = px_index(addr);
    if (i >= 0) g_px_live[i >> 5] &= ~(1u << (i & 31));
}

static inline void pgxp_addr_inval_half(uint32_t addr, uint16_t newval) {
    if (px_is_live(addr)) px_forget_half(addr, newval);
}

/* Fast guest writes (09.09.2026): main RAM pages nobody watches are written here directly; everything watched and
 * everything outside main RAM goes the full way (memory.c). They live here, not in psx_fastmem.h, because the fast
 * path must empty the PGXP cells exactly like the full one does. */
PSX_HOT_INLINE void psx_ww_f(uint32_t addr, uint32_t val) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!psx_fm_wr_ok(phys)) { psx_write_word(addr, val); return; }
    if (g_pgxp_addr_on) pgxp_addr_inval_word(phys);
    psx_fm_mark_dirty(phys);
    memcpy(g_psx_ram + phys, &val, 4);
}
PSX_HOT_INLINE void psx_wh_f(uint32_t addr, uint16_t val) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!psx_fm_wr_ok(phys)) { psx_write_half(addr, val); return; }
    if (g_pgxp_addr_on) pgxp_addr_inval_half(phys, val);
    psx_fm_mark_dirty(phys);
    memcpy(g_psx_ram + phys, &val, 2);
}
PSX_HOT_INLINE void psx_wb_f(uint32_t addr, uint8_t val) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!psx_fm_wr_ok(phys)) { psx_write_byte(addr, val); return; }
    if (g_pgxp_addr_on) pgxp_addr_inval_word(phys);
    psx_fm_mark_dirty(phys);
    g_psx_ram[phys] = val;
}

/* Slow paths of the wrappers (the wrappers already did the real access and checked the cheap conditions). */
void px_store_word_from_reg(uint32_t addr, int r);
void px_store_half_from_reg(uint32_t addr, int r, uint32_t v);
void px_load_word_to_reg(uint32_t addr, int r, uint32_t v);
void px_load_half_to_reg(uint32_t addr, int r, uint32_t v, int sign);
void px_gte_to_reg(int r, int reg, uint32_t v);
void px_reg_to_gte(int reg, int r, uint32_t v);
void px_gte_to_mem(uint32_t addr, int reg, uint32_t v);
void px_mem_to_gte(uint32_t addr, int reg, uint32_t v);
void pgxp_track_lwc2_sxy(uint32_t addr, int reg, uint32_t v);   /* = px_mem_to_gte; called by the generated code
                                                                * (zf_vxy_transform.py: the VXY address goes with the word) */
void px_rebuild_from_cache(uint32_t addr, uint32_t v);
void pgxp_addr_gte_sxy_raw(int reg, uint32_t v);   /* SXY write that bypassed the wrappers */
int  pgxp_addr_nclip(uint32_t s0, uint32_t s1, uint32_t s2, int32_t* out);
int  pgxp_addr_sxy_actor(void);   /* any SXY cell from the character pool (stats) */
/* Signed sub-pixel area of the SXY FIFO triangle (native 320-space pixels); 0 = no trustworthy cells. */
int  pgxp_addr_area(uint32_t s0, uint32_t s1, uint32_t s2, double* area);
/* A precise position computed outside the GTE chain, installed for one packet word. */
void pgxp_addr_store_precise(uint32_t addr, uint32_t value, float fx, float fy, float w, int actor);
/* Exact area of the SXY triangle only when all three vertices came from the character pool. */
int  pgxp_addr_actor_area(uint32_t s0, uint32_t s1, uint32_t s2, double* area);
/* 15.09: address of VXY0 of the SXY triangle (start of a polygon record), 0 - not a room record */
uint32_t pgxp_addr_sxy_src(uint32_t s0, uint32_t s1, uint32_t s2);

/* ---- wrappers used by the transformed generated code ------------------------------------------------------
 * The guest access itself is done first and exactly as before; only then the cells follow. A store from a
 * register without a cell costs nothing more than the emptying the write already did; a load from a word
 * without a cell only empties the destination register's cell. */

/* Direct guest reads (09.09.2026): the memory pointers in CPUState always point at psx_read_* / psx_write_*;
 * main RAM is read here without the indirect call (all watchers sit on WRITES). */
extern uint8_t *const g_psx_ram;
extern uint32_t psx_read_word(uint32_t addr);
extern uint16_t psx_read_half(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t value);
extern void     psx_write_half(uint32_t addr, uint16_t value);
#define PX_RAM_BYTES 0x200000u

PSX_HOT_INLINE uint32_t pgxp_rd_word(uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= PX_RAM_BYTES) return psx_read_word(addr);
    uint32_t v; memcpy(&v, g_psx_ram + phys, 4); return v;
}
PSX_HOT_INLINE uint16_t pgxp_rd_half(uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= PX_RAM_BYTES) return psx_read_half(addr);
    uint16_t v; memcpy(&v, g_psx_ram + phys, 2); return v;
}

/* A register cell speaks for the value v when it knows something and was made for exactly that word. */
PSX_HOT_INLINE int px_reg_holds(int r, uint32_t v) { return g_px_reg[r].bits != 0 && g_px_reg[r].raw == v; }

/* The guest registers are locals of the generated function (code_generator.cpp): the value comes as an argument. */
static inline void pgxp_sw_v(CPUState* cpu, uint32_t addr, int r, uint32_t v) {
    (void)cpu;
    psx_ww_f(addr, v);
    if (!g_pgxp_addr_on) return;
    if (px_reg_holds(r, v)) { px_store_word_from_reg(addr, r); return; }
    /* 16.08.2026 ("polygons merge / vanish with distance"): the room renderer (func_80064268 family) repacks
     * projected vertices through plain arithmetic before storing them into the display-list arena, so nothing
     * follows them there and the raster falls back to whole pixels - distant triangles collapse. An untracked
     * store into that arena that looks like a packed screen vertex gets its cell back from the value-keyed GTE
     * cache; a collided entry still lies in the same whole pixel, never worse than the integer fallback. */
    if (g_px_reseed && ((addr & 0x1FFFFFFu) - 0x150000u) < 0x80000u && v != 0) {
        const int32_t sx = (int16_t)v, sy = (int16_t)(v >> 16);
        if (sx > -1024 && sx < 1024 && sy > -512 && sy < 512) px_rebuild_from_cache(addr, v);
    }
}

static inline uint32_t pgxp_lw(CPUState* cpu, uint32_t addr, int r) {
    (void)cpu;
    const uint32_t v = pgxp_rd_word(addr);
    if (!g_pgxp_addr_on) return v;
    if (px_is_live(addr)) { px_load_word_to_reg(addr, r, v); return v; }
    g_px_reg[r].bits = 0;
    /* A pool word whose cell was emptied by an untracked store: the precise position is gone, but the vertex is
     * still a character vertex - keep that alone, tied to the word, so the polygon does not flip between actor
     * and world for a frame (the class drives the debug colour and the back-face depth bias, never positions). */
    if (r != 0 && ((addr & 0x1FFFFFFu) - 0xC6800u) < 0x2000u) {
        g_px_reg[r].bits = PX_ACTOR;
        g_px_reg[r].raw = v;
    }
    return v;
}

static inline void pgxp_sh_v(CPUState* cpu, uint32_t addr, int r, uint32_t v) {
    (void)cpu;
    psx_wh_f(addr, (uint16_t)v);
    if (g_pgxp_addr_on && px_reg_holds(r, v)) px_store_half_from_reg(addr, r, v);
}

static inline uint32_t pgxp_lhu(CPUState* cpu, uint32_t addr, int r) {
    (void)cpu;
    const uint32_t v = (uint32_t)pgxp_rd_half(addr);
    if (g_pgxp_addr_on) {
        if (px_is_live(addr)) px_load_half_to_reg(addr, r, v, 0);
        else g_px_reg[r].bits = 0;
    }
    return v;
}

static inline uint32_t pgxp_lh(CPUState* cpu, uint32_t addr, int r) {
    (void)cpu;
    const uint32_t v = (uint32_t)(int32_t)(int16_t)pgxp_rd_half(addr);
    if (g_pgxp_addr_on) {
        if (px_is_live(addr)) px_load_half_to_reg(addr, r, v, 1);
        else g_px_reg[r].bits = 0;
    }
    return v;
}

static inline uint32_t pgxp_move_v(CPUState* cpu, int rd, int rs, uint32_t v) {
    (void)cpu;
    if (!g_pgxp_addr_on || rd == 0) return v;
    if (rs != 0 && px_reg_holds(rs, v)) g_px_reg[rd] = g_px_reg[rs];
    else g_px_reg[rd].bits = 0;
    return v;
}

static inline uint32_t pgxp_mfc2_sxy(CPUState* cpu, int r, int reg) {
    const uint32_t v = cpu->gte_data[reg];
    if (g_pgxp_addr_on) px_gte_to_reg(r, reg, v);
    return v;
}

static inline void pgxp_mtc2_sxy_v(CPUState* cpu, int reg, int r, uint32_t v) {
    cpu->gte_data[reg] = v;
    if (g_pgxp_addr_on) px_reg_to_gte(reg, r, v);
}

static inline void pgxp_swc2_sxy(CPUState* cpu, uint32_t addr, int reg) {
    const uint32_t v = cpu->gte_data[reg];
    psx_ww_f(addr, v);
    if (g_pgxp_addr_on) px_gte_to_mem(addr, reg, v);
}

static inline void pgxp_lwc2_sxy(CPUState* cpu, uint32_t addr, int reg) {
    const uint32_t v = pgxp_rd_word(addr);
    cpu->gte_data[reg] = v;
    if (g_pgxp_addr_on) px_mem_to_gte(addr, reg, v);
}

/* Older three-argument forms (the value from CPUState) - generated code from before the registers became locals. */
static inline void pgxp_sw(CPUState* cpu, uint32_t addr, int r) { pgxp_sw_v(cpu, addr, r, cpu->gpr[r]); }
static inline void pgxp_sh(CPUState* cpu, uint32_t addr, int r) { pgxp_sh_v(cpu, addr, r, cpu->gpr[r]); }
static inline uint32_t pgxp_move(CPUState* cpu, int rd, int rs) { return pgxp_move_v(cpu, rd, rs, cpu->gpr[rs]); }
static inline void pgxp_mtc2_sxy(CPUState* cpu, int reg, int r) { pgxp_mtc2_sxy_v(cpu, reg, r, cpu->gpr[r]); }

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PGXP_ADDR_H */
