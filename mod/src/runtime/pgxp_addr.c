/*
 * pgxp_addr.c - sub-pixel vertex tracking by memory address (see pgxp_addr.h). Our implementation, 08.10.2026.
 *
 * Three places hold cells: guest memory words (g_px_cell, with the g_px_live bitmap in front), the 32 CPU
 * registers (g_px_reg) and the three GTE screen registers SXY0..2 (s_fifo). A cell always carries the exact
 * 32-bit word it was made for; every read compares that word with the real one, so a value the tracker did not
 * follow (arithmetic, untracked stores) only loses precision, it never moves a vertex.
 */
#include "pgxp_addr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int       g_pgxp_addr_on = 0;
int       g_px_reseed = 0;
int       g_px_arena = 0;
PxCell*   g_px_cell = NULL;
PxCell    g_px_reg[32];
uint32_t* g_px_live = NULL;
long      g_pgxp_addr_hit = 0, g_pgxp_addr_zhit = 0;
int       g_pgxp_last_actor = 0;
int       g_pgxp_rtp_vi = 0;

#define PX_CELLS (PX_RAM_CELLS + PX_SPAD_CELLS + PX_ARENA_CELLS)

static PxCell   s_fifo[3];            /* GTE SXY0 / SXY1 / SXY2 */
static uint32_t s_reg_from[32];       /* address each register was last loaded from (VXY origin, debug) */
static uint32_t s_vxy_from[3];        /* 12.09.2026: where VXY0/1/2 came from (lwc2 / mtc2 into data regs 0/2/4) */

static int env_on(const char* name, int dflt) {
    const char* e = getenv(name);
    if (!e || !*e) return dflt;
    return *e != '0';
}
static int env_one(const char* name) {     /* statistics switches: only "1" turns them on */
    const char* e = getenv(name);
    return e && *e == '1';
}

void pgxp_addr_boot(void) {
    g_px_arena = env_on("DC_PGXP_ARENA", 1);
    g_px_reseed = env_on("DC_PGXP_RESEED", 1);   /* room repack coverage needs it (pgxp_sw_v) */
    if (!env_on("DC_PGXP_ADDR", 1)) { g_pgxp_addr_on = 0; return; }
    g_px_cell = (PxCell*)calloc(PX_CELLS, sizeof(PxCell));
    g_px_live = (uint32_t*)calloc((PX_CELLS + 31) / 32, sizeof(uint32_t));
    g_pgxp_addr_on = g_px_cell != NULL && g_px_live != NULL;
}

/* After a savestate load every cell belongs to the old timeline; one whose word happens to match the restored
 * RAM would give the renderer a wrong position. Start empty - the next frame of projections fills them again. */
void pgxp_addr_reset(void) {
    if (!g_pgxp_addr_on) return;
    memset(g_px_live, 0, ((PX_CELLS + 31) / 32) * sizeof(uint32_t));
    memset(g_px_reg, 0, sizeof g_px_reg);
    memset(s_fifo, 0, sizeof s_fifo);
}

/* ---- memory cells ------------------------------------------------------------------------------------------ */

static PxCell* cell_at(uint32_t addr) {
    const int32_t i = px_index(addr);
    return i < 0 ? NULL : g_px_cell + i;
}

static void mark_live(uint32_t addr, int live) {
    const int32_t i = px_index(addr);
    if (i < 0) return;
    if (live) g_px_live[i >> 5] |= 1u << (i & 31);
    else g_px_live[i >> 5] &= ~(1u << (i & 31));
}

/* depth that arrived with one half is lost with it; a depth no half vouches for any more is no depth */
static uint32_t depth_settle(uint32_t bits) {
    if ((bits & PX_ZOK) && !(bits & (PX_ZLO | PX_ZHI))) bits &= ~PX_ZOK;
    return bits;
}

/* The whole word goes to memory: the cell becomes a copy of the source; a known depth now stands behind both
 * halves; a full x/y pair stored at once is render grade. */
static void put_word(uint32_t addr, const PxCell* from) {
    PxCell* c = cell_at(addr);
    if (!c) return;
    uint32_t b = from->bits & ~(PX_ZLO | PX_ZHI);
    if (b & PX_ZOK) b |= PX_ZLO | PX_ZHI;
    if ((b & PX_XY) == PX_XY) b |= PX_WHOLE;
    *c = *from;
    c->bits = b;
    mark_live(addr, b != 0);
}

/* One halfword goes to memory: the low 16 bits of the source register land in the addressed half. The source's
 * own x is what that half now means (x for the low half, y for the high one). The cell keeps its word in step
 * with memory even when it had nothing tracked: a later whole-word read is checked against it. */
static void put_half(uint32_t addr, const PxCell* from, uint32_t half16) {
    PxCell* c = cell_at(addr);
    if (!c) return;
    const int high = (addr & 2u) != 0;
    uint32_t b = px_is_live(addr) ? c->bits : 0u;
    const uint32_t xok = from->bits & PX_XOK;
    if (high) {
        c->y = from->x;
        b = (b & ~PX_YOK) | (xok ? PX_YOK : 0u);
        c->raw = (c->raw & 0x0000FFFFu) | (half16 << 16);
    } else {
        c->x = from->x;
        b = (b & ~PX_XOK) | xok;
        c->raw = (c->raw & 0xFFFF0000u) | half16;
    }
    b &= ~PX_WHOLE;
    if (from->bits & PX_ZOK) {
        c->z = from->z;
        b |= PX_ZOK | (high ? PX_ZHI : PX_ZLO);
    } else {
        b = depth_settle(b & ~(high ? PX_ZHI : PX_ZLO));
    }
    c->bits = b;
    mark_live(addr, b != 0);
}

/* A halfword comes from memory into a register (the cell is live). The half must still be the one the cell was
 * made for, or that coordinate is forgotten in memory too. The register then holds the half as its low part;
 * its high part is the extension the load produced: all ones only for a sign-extending load of a negative
 * coordinate. */
static void get_half(PxCell* to, uint32_t addr, uint32_t v, int sign_ext) {
    PxCell* c = cell_at(addr);
    if (!c) { to->bits = 0; return; }
    const int high = (addr & 2u) != 0;
    const uint32_t had = high ? (c->raw >> 16) & 0xFFFFu : c->raw & 0xFFFFu;
    if (had != (v & 0xFFFFu)) c->bits &= high ? ~PX_YOK : ~PX_XOK;
    PxCell r = *c;
    if (high) {
        r.x = c->y;
        r.bits = (r.bits & ~PX_XOK) | ((c->bits & PX_YOK) ? PX_XOK : 0u);
    }
    if (r.bits & PX_XOK) {
        r.y = (sign_ext && r.x < 0.f) ? -1.0f : 0.0f;
        r.bits |= PX_YOK;
    } else {
        r.y = 0.0f;
        r.bits &= ~PX_YOK;
    }
    r.raw = v;
    *to = r;
}

/* Untracked halfword store into a live cell (memory.c, fast write): that half's coordinate and any depth it
 * brought are gone, the word follows memory. */
void px_forget_half(uint32_t addr, uint16_t newval) {
    PxCell* c = cell_at(addr);
    if (!c) return;
    uint32_t b = c->bits & ~PX_WHOLE;
    if (addr & 2u) {
        b &= ~(PX_YOK | PX_ZHI);
        c->raw = (c->raw & 0x0000FFFFu) | ((uint32_t)newval << 16);
    } else {
        b &= ~(PX_XOK | PX_ZLO);
        c->raw = (c->raw & 0xFFFF0000u) | newval;
    }
    c->bits = depth_settle(b);
    if (!c->bits) mark_live(addr, 0);
}

/* ---- register <-> memory ------------------------------------------------------------------------------------ */

void px_store_word_from_reg(uint32_t addr, int r) { put_word(addr, &g_px_reg[r]); }

void px_store_half_from_reg(uint32_t addr, int r, uint32_t v) { put_half(addr, &g_px_reg[r], v & 0xFFFFu); }

/* Is this RAM word in the character vertex pool? The fixed window of the double-buffered pool, then (12.09.2026,
 * door scenes put the pool elsewhere in 4 frames of 169) the live range pt_renderer keeps;
 * DC_PGXP_ACTOR_POOLRANGE=0 - the fixed window only. */
static int in_actor_pool(uint32_t addr) {
    if (((addr & 0x1FFFFFFu) - 0xC6800u) < 0x2000u) return 1;
    static int live_range = -1;
    if (live_range < 0) live_range = env_on("DC_PGXP_ACTOR_POOLRANGE", 1);
    if (!live_range) return 0;
    extern int pt_poolgeom_is_pool(uint32_t addr);
    return pt_poolgeom_is_pool(addr);
}

/* Whole word into a register (the cell is live). A cell that no longer matches memory is emptied. The class
 * mark is set at the source - the split character pipeline pre-projects body vertices into the pool
 * (func_80073F10) and the assemblers read them back with lw; the mark then rides the whole chain to the packet. */
void px_load_word_to_reg(uint32_t addr, int r, uint32_t v) {
    PxCell* c = cell_at(addr);
    if (!c) { g_px_reg[r].bits = 0; return; }
    if (c->raw != v) { c->bits = 0; mark_live(addr, 0); }
    g_px_reg[r] = *c;
    if (in_actor_pool(addr)) g_px_reg[r].bits |= PX_ACTOR;
    s_reg_from[r] = addr;
    if (r == 0) g_px_reg[0].bits = 0;
}

void px_load_half_to_reg(uint32_t addr, int r, uint32_t v, int sign) {
    get_half(&g_px_reg[r], addr, v, sign);
    if (r == 0) g_px_reg[0].bits = 0;
}

/* ---- GTE screen registers ------------------------------------------------------------------------------------ */

static int sxy_slot(int reg) { return (reg >= 12 && reg <= 14) ? reg - 12 : -1; }

/* RTPS / RTPT: the FIFO moves on and SXY2 gets the precise result (valid = 0 still moves it on). */
void pgxp_addr_rtps_push(float fx, float fy, float w, uint32_t packed, int valid) {
    if (!g_pgxp_addr_on) return;
    s_fifo[0] = s_fifo[1];
    s_fifo[1] = s_fifo[2];
    PxCell* n = &s_fifo[2];
    n->x = fx; n->y = fy; n->z = w;
    n->raw = packed;
    n->bits = valid ? (PX_XY | PX_ZOK) : 0u;
    n->origin = (g_pgxp_rtp_vi >= 0 && g_pgxp_rtp_vi < 3) ? s_vxy_from[g_pgxp_rtp_vi] : 0u;
}

/* mfc2 from SXYn: the register takes the slot (a slot that no longer matches its register is emptied). */
void px_gte_to_reg(int r, int reg, uint32_t v) {
    const int k = sxy_slot(reg);
    if (r == 0 || k < 0) return;
    if (s_fifo[k].raw != v) s_fifo[k].bits = 0;
    g_px_reg[r] = s_fifo[k];
    g_px_reg[r].raw = v;
}

/* mtc2: into VXY0/1/2 only the origin is noted; into SXYn the register's cell goes in if it is for this value,
 * otherwise only the class mark survives (the debug colour and the back-face bias must not blink). */
void px_reg_to_gte(int reg, int r, uint32_t v) {
    if (reg == 0 || reg == 2 || reg == 4) { s_vxy_from[reg >> 1] = r ? s_reg_from[r] : 0u; return; }
    const int k = sxy_slot(reg);
    if (k < 0) return;
    const PxCell* g = &g_px_reg[r];
    if (r == 0 || g->raw != v) {
        s_fifo[k].bits = r ? (g->bits & PX_ACTOR) : 0u;
        s_fifo[k].raw = v;
        return;
    }
    {   /* DC_PGXP_STAT: every 2000th source address feeding mtc2 (where the pool is) */
        static int st = -1; static long n = 0;
        if (st < 0) st = env_one("DC_PGXP_STAT");
        if (st && (n++ % 2000) == 0)
            fprintf(stderr, "mtc2src: reg=%d src=0x%08X val=0x%08X%c", reg, s_reg_from[r], v, 10);
    }
    s_fifo[k] = *g;
    s_fifo[k].raw = v;
}

/* swc2 from SXYn: memory takes the slot; a body vertex heading into the character pool is also handed to
 * pt_renderer with its exact word and position (pt_poolgeom_vertex), which tells body triangles from world
 * geometry in NCLIP without guessing. */
void px_gte_to_mem(uint32_t addr, int reg, uint32_t v) {
    const int k = sxy_slot(reg);
    if (k < 0) return;
    PxCell* s = &s_fifo[k];
    if (s->raw != v) s->bits = 0;
    put_word(addr, s);
    extern int  pt_poolgeom_is_pool(uint32_t addr);
    extern void pt_poolgeom_vertex(uint32_t packed, float fx, float fy);
    if ((s->bits & PX_XY) == PX_XY && pt_poolgeom_is_pool(addr)) pt_poolgeom_vertex(v, s->x, s->y);
}

/* lwc2: into VXY0/1/2 the address is the vertex origin; into SXYn the memory cell goes in when it is live. */
void px_mem_to_gte(uint32_t addr, int reg, uint32_t v) {
    if (reg == 0 || reg == 2 || reg == 4) { s_vxy_from[reg >> 1] = addr; return; }
    const int k = sxy_slot(reg);
    if (k < 0) return;
    PxCell* c = px_is_live(addr) ? cell_at(addr) : NULL;
    if (!c) { s_fifo[k].bits = 0; s_fifo[k].raw = v; return; }
    if (c->raw != v) c->bits = 0;
    s_fifo[k] = *c;
    s_fifo[k].raw = v;
}

void pgxp_track_lwc2_sxy(uint32_t addr, int reg, uint32_t v) { px_mem_to_gte(addr, reg, v); }

/* An SXY write that went around the wrappers (the generator writes SXY2 / SXYP directly because of the FIFO).
 * The slot must follow the real register or it keeps the previous triangle's floats. The value was just loaded
 * by the game into some register (tracked lw from the pool): take that register's cell by the exact word. At
 * range several registers can hold the same word for different vertices - prefer one whose point differs from
 * the other two slots (a copy would give an area of exactly 0 and drop a real polygon), a copy only if nothing
 * else; with no coordinates anywhere keep at least the class mark of a register carrying this word. */
static void slot_from_regs(int k, uint32_t v) {
    PxCell* slot = &s_fifo[k];
    const PxCell* fallback = NULL;
    for (int r = 1; r < 32; r++) {
        const PxCell* g = &g_px_reg[r];
        if (g->raw != v || (g->bits & PX_XY) != PX_XY) continue;
        int same_point = 0;
        for (int j = 0; j < 3 && !same_point; j++)
            if (j != k && (s_fifo[j].bits & PX_XY) == PX_XY && s_fifo[j].x == g->x && s_fifo[j].y == g->y) same_point = 1;
        if (!same_point) { *slot = *g; slot->raw = v; return; }
        if (!fallback) fallback = g;
    }
    if (fallback) { *slot = *fallback; slot->raw = v; return; }
    for (int r = 1; r < 32; r++)
        if (g_px_reg[r].raw == v && (g_px_reg[r].bits & PX_ACTOR)) { slot->bits = PX_ACTOR; slot->raw = v; return; }
    slot->bits = 0;
    slot->raw = v;
}

void pgxp_addr_gte_sxy_raw(int reg, uint32_t v) {
    if (!g_pgxp_addr_on) return;
    int k = sxy_slot(reg);
    if (reg == 15) {                      /* SXYP: pushes the FIFO */
        s_fifo[0] = s_fifo[1];
        s_fifo[1] = s_fifo[2];
        k = 2;
    }
    if (k < 0) return;
    slot_from_regs(k, v);
    s_fifo[k].origin = 0;
}

/* ---- questions from the GTE, the GPU and pt_renderer ------------------------------------------------------- */

/* the FIFO speaks for the three words with known x/y in every slot */
static int fifo_matches(uint32_t s0, uint32_t s1, uint32_t s2, int* why) {
    const uint32_t want[3] = { s0, s1, s2 };
    for (int k = 0; k < 3; k++) {
        if ((s_fifo[k].bits & PX_XY) != PX_XY) { if (why) *why = 1; return 0; }
        if (s_fifo[k].raw != want[k]) { if (why) *why = 2; return 0; }
    }
    return 1;
}

static double fifo_cross(void) {
    const double ax = (double)s_fifo[1].x - s_fifo[0].x, ay = (double)s_fifo[1].y - s_fifo[0].y;
    const double bx = (double)s_fifo[2].x - s_fifo[1].x, by = (double)s_fifo[2].y - s_fifo[1].y;
    return ax * by - ay * bx;
}

/* DC_NCLIP_STAT=1: how often the precise winding was available */
static void nclip_count(int kind) {
    static int on = -1; static long calls = 0, precise = 0, no_bits = 0, stale = 0;
    if (on < 0) on = env_one("DC_NCLIP_STAT");
    if (!on) return;
    calls++;
    if (kind == 0) precise++; else if (kind == 1) no_bits++; else stale++;
    if (calls % 50000 == 0)
        fprintf(stderr, "nclipcov: calls=%ld precise=%ld flagmiss=%ld valmiss=%ld%c", calls, precise, no_bits, stale, 10);
}

/* NCLIP from the precise points. A winding that rounds below one whole unit but is not tiny keeps its sign as
 * +-1, so it is not dropped by rounding alone; below 0.1 it is dropped like the console's integer result (the
 * game relies on sub-pixel back shells disappearing - kept, they smeared as dark wedges across faces). */
int pgxp_addr_nclip(uint32_t s0, uint32_t s1, uint32_t s2, int32_t* out) {
    if (!g_pgxp_addr_on) return 0;
    int why = 0;
    if (!fifo_matches(s0, s1, s2, &why)) { nclip_count(why); return 0; }
    nclip_count(0);
    double n = fifo_cross();
    const double mag = n < 0 ? -n : n;
    if (mag > 0.1 && mag < 1.0) n = n < 0 ? -1.0 : 1.0;
    *out = (int32_t)n;
    return 1;
}

/* Same, only when all three vertices came from the character pool: the game's own NCLIP from rounded points
 * drops body polygons under a pixel and the room shows through; world geometry is not touched. */
int pgxp_addr_actor_area(uint32_t s0, uint32_t s1, uint32_t s2, double* area) {
    if (!g_pgxp_addr_on || !area || !fifo_matches(s0, s1, s2, NULL)) return 0;
    for (int k = 0; k < 3; k++)
        if (!(s_fifo[k].bits & PX_ACTOR)) return 0;
    *area = fifo_cross();
    return 1;
}

int pgxp_addr_sxy_actor(void) {
    return ((s_fifo[0].bits | s_fifo[1].bits | s_fifo[2].bits) & PX_ACTOR) != 0;
}

/* 15.09.2026: where the triangle's vertices were loaded from (NCLIP, gte.cpp): VXY0 of a polygon record (or of
 * its copy in the scratchpad), 0 when the FIFO words differ or the origins are not 8 bytes apart. */
uint32_t pgxp_addr_sxy_src(uint32_t s0, uint32_t s1, uint32_t s2) {
    if (!g_pgxp_addr_on) return 0;
    if (s_fifo[0].raw != s0 || s_fifo[1].raw != s1 || s_fifo[2].raw != s2) return 0;
    const uint32_t a = s_fifo[0].origin;
    return (a && s_fifo[1].origin == a + 8u && s_fifo[2].origin == a + 16u) ? a : 0u;
}

/* 12.09.2026: origin of a packet word's vertex (gpu.c recognises room polygon records by it). */
uint32_t pgxp_addr_vertex_src(uint32_t addr, uint32_t value) {
    if (!g_pgxp_addr_on || addr == 0xFFFFFFFFu || !px_is_live(addr)) return 0;
    const PxCell* c = cell_at(addr);
    if (!c || c->raw != value || (c->bits & PX_XY) != PX_XY) return 0;
    return c->origin;
}

/* GPU: the precise point of a packet word. The class is reported even for a cell without coordinates (a dead
 * pool slot, see pgxp_lw). DC_PGXP_STAT=1 prints the hit rate. */
int pgxp_addr_get_vertex(uint32_t addr, uint32_t value, float* fx, float* fy, float* w) {
    static int stat = -1; static long calls = 0, actor_hits = 0;
    if (stat < 0) stat = env_one("DC_PGXP_STAT");
    g_pgxp_last_actor = 0;
    if (!g_pgxp_addr_on || addr == 0xFFFFFFFFu) return 0;
    if (stat && ++calls % 200000 == 0)
        fprintf(stderr, "pgxpaddr: calls=%ld hit=%ld (%.2f%%) zhit=%ld actor=%ld\n", calls, g_pgxp_addr_hit,
                100.0 * (double)g_pgxp_addr_hit / (double)calls, g_pgxp_addr_zhit, actor_hits);
    if (!px_is_live(addr)) return 0;
    const PxCell* c = cell_at(addr);
    if (!c || c->raw != value) return 0;
    g_pgxp_last_actor = (c->bits & PX_ACTOR) != 0;
    actor_hits += g_pgxp_last_actor;
    if ((c->bits & PX_XY) != PX_XY) return 0;
    *fx = c->x;
    *fy = c->y;
    g_pgxp_addr_hit++;
    if (!(c->bits & PX_ZOK)) { *w = 1.0f; return 1; }
    *w = c->z;
    g_pgxp_addr_zhit++;
    return 2;
}

/* Display-list word rebuilt from the value-keyed GTE cache (pgxp_sw_v). */
extern int pgxp_lookup(int sx, int sy, float* fx, float* fy, float* w);

void px_rebuild_from_cache(uint32_t addr, uint32_t v) {
    float fx, fy, w;
    if (!pgxp_lookup((int16_t)v, (int16_t)(v >> 16), &fx, &fy, &w)) return;
    PxCell c;
    c.x = fx; c.y = fy; c.z = w;
    c.raw = v;
    c.bits = PX_XY | PX_ZOK;
    c.origin = 0;
    put_word(addr, &c);
}
