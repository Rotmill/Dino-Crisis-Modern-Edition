/* ---------------------------------------------------------------------------
 * Animation cross-fade for the player character (DC_ANIM_SMOOTH=1).
 *
 * The game swaps motions outright: idle -> walk -> run replace the pose in one
 * frame and read as a snap. It does own a cross-fade (func_80050D60, which
 * unpacks two frames and mixes them), but the movement state machine
 * (func_800607FC) does not route through it — it just unpacks the new motion's
 * frame straight into the scratchpad.
 *
 * This hook sits right after that unpack. The buffer at 0x1F80002C holds the
 * frame's BONE ANGLES; blending those and letting the game build the skeleton
 * from them is a real animation cross-fade. (Blending world matrices instead
 * drags the body through space — a second copy sliding into place.)
 *
 * Angles are 12-bit, 4096 = 360 degrees, so blending takes the shortest arc,
 * the same +-2048 rule the game's own interpolator (func_80066630) uses.
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cpu_state.h"
/* dc_embedded.c: our own data, built into the program */
const unsigned char *dc_embedded(const char *name, unsigned *size);
/* 10.10.2026: объявления для бесплатного компилятора установщика (Clang): здесь вызывались функции без объявления
 * выше по файлу - MSVC подставлял int f() сам, Clang требует объявления. */
static int ao_bone_ours(int b);
extern uint8_t *memory_get_ram_ptr(void);   /* memory.c */

#define ANIM_MAX_HALF 256          /* angle halfwords we track */

static int16_t s_prev[ANIM_MAX_HALF];   /* pose shown last frame */
static int16_t s_src[ANIM_MAX_HALF];    /* pose the blend starts from */
static int     s_have = 0, s_n = 0;
static int     s_left = 0;              /* frames left in the blend */
static uint32_t s_blk_lo = 0xFFFFFFFFu, s_blk_hi = 0;
static long    s_switches = 0;

int pt_anim_smooth(void);               /* pt_renderer.c */

static int anim_frames(void) {
    static int n = -1;
    if (n < 0) { const char *e = getenv("DC_ANIM_FRAMES");
                 /* OFF by default: pose blending is parked while the movement
                  * and camera problems are investigated, so nothing of ours
                  * touches her pose. DC_ANIM_FRAMES=15 restores it. */
                 n = (e && *e) ? atoi(e) : 0;
                 /* 0 means OFF (instant swap, as the console does). It used to
                  * be clamped up to 2, so blending could never be compared
                  * against no blending at all. */
                 if (n != 0 && n < 2) n = 2;
                 if (n > 60) n = 60; }
    return n;
}
static int anim_stat(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("DC_ANIM_STAT"); s = (e && *e == '1') ? 1 : 0; }
    return s;
}

/* Shortest-arc blend of two 12-bit angles (4096 = 360 degrees), the same
 * +-2048 rule the game's own interpolator uses. Returns `b` untouched for
 * anything that is not a 12-bit angle: the unpacked frame buffer also carries
 * values outside that range, and forcing them through the angle path (or
 * masking them to 12 bits) corrupts the pose. */
static int blend_is_angle(int v) { return v >= 0 && v < 4096; }

static int16_t blend_angle(int a, int b, float t) {
    if (!blend_is_angle(a) || !blend_is_angle(b)) return (int16_t)b;
    int d = b - a;
    if (d >  2048) d -= 4096;
    if (d < -2048) d += 4096;
    int r = a + (int)(d * t + (d < 0 ? -0.5f : 0.5f));
    r &= 0xFFF;
    return (int16_t)r;
}

/* Для харнеса (debug-команда anim_src): адрес последнего распакованного кадра и
 * счётчик вызовов — обновляются на ВХОДЕ, до любых ранних выходов ниже. */
static uint32_t s_seen_src = 0; static uint64_t s_seen_n = 0;
uint32_t pt_anim_last_src(void) { return s_seen_src; }
uint64_t pt_anim_last_seq(void) { return s_seen_n; }
void pt_anim_frame(CPUState *cpu, uint32_t src, uint32_t dst, uint32_t nbones) {
    { uint32_t s0 = cpu->gpr[16] & 0x1FFFFFu;              /* только контекст Регины (0xB9280..0xB9300) */
      if (s0 >= 0xB9280u && s0 < 0xB9300u) { s_seen_src = src; s_seen_n++; } }
    /* who unpacks through here: one line per distinct actor context (s0) */
    { static int ph = -1;
      if (ph < 0) { const char *e = getenv("DC_PHASEDBG"); ph = (e && *e == '1') ? 1 : 0; }
      if (ph) { static uint32_t seen[8]; static int ns = 0;
          uint32_t s0 = cpu->gpr[16]; int k;
          for (k = 0; k < ns; k++) if (seen[k] == s0) break;
          if (k == ns && ns < 8) { seen[ns++] = s0;
              fprintf(stderr, "phN s0=%08X s8=%08X src=%08X dst=%08X n=%u%c",
                      cpu->gpr[16], cpu->gpr[30], src, dst, nbones, 10); } } }
    if (!pt_anim_smooth()) return;
    int nh = (int)nbones * 3;                 /* three angles per bone */
    if (nh <= 0 || nh > ANIM_MAX_HALF) return;

    /* The unpacker writes SIX bytes per bone: a word holding two 12-bit
     * angles unpacked to 16 bits, then a halfword with the third. So the
     * angles are a flat array of nbones*3 int16 starting at offset 0.
     * Starting at +6 shifted every angle onto the next bone — which is what
     * tilted her instead of blending the transition. */
    const uint32_t base = dst;
    int16_t cur[ANIM_MAX_HALF];
    for (int i = 0; i < nh; i++)
        cur[i] = (int16_t)cpu->read_half(base + (uint32_t)i * 2u);

    if (s_have && s_n == nh) {
        /* A motion swap shows up as the frame pointer leaving the block it was
         * walking through. Stepping to the next frame of the same motion moves
         * it by one frame size; a loop back to the start moves it backwards
         * within the same motion, and blending that is harmless (the poses
         * match). */
        /* One motion's frames live in one contiguous block, and playback walks
         * inside it (forward frame by frame, or back to its start on a loop).
         * A motion SWAP lands outside that block. Track the block's extent as
         * playback reveals it, and treat leaving it as the swap. */
        uint32_t fsz = (uint32_t)(12 + ((nbones * 3 * 12 + 7) / 8));
        fsz = (fsz + 3u) & ~3u;                       /* frame size, 80B at 15 bones */
        uint32_t pad = fsz * 4u;
        if (src + pad < s_blk_lo || src > s_blk_hi + pad) {
            if (s_left == 0) {
                memcpy(s_src, s_prev, sizeof(int16_t) * nh);
                s_left = anim_frames();
                if (s_left == 0) { s_have = 0; }   /* blending disabled */
                s_switches++;
            }
            s_blk_lo = s_blk_hi = src;                /* new motion's block */
        } else {
            if (src < s_blk_lo) s_blk_lo = src;
            if (src > s_blk_hi) s_blk_hi = src;
        }
        if (s_left > 0) {
            int n = anim_frames();
            float t = (float)(n - s_left + 1) / (float)n;
            t = t * t * (3.f - 2.f * t);          /* ease in/out */
            for (int i = 0; i < nh; i++) {
                int16_t v = blend_angle(s_src[i], cur[i], t);
                cur[i] = v;
                cpu->write_half(base + (uint32_t)i * 2u, (uint16_t)v);
            }
            s_left--;
        }
    }
    memcpy(s_prev, cur, sizeof(int16_t) * nh);
    s_have = 1; s_n = nh;

    /* diagnostic: how much each half-word moved this frame, and what the
     * words around the angle block look like */
    if (anim_stat()) {
        static int16_t dbg_prev[ANIM_MAX_HALF + 8];
        int16_t now[8];
        for (int i = 0; i < 8; i++) now[i] = (int16_t)cpu->read_half(dst + (uint32_t)i * 2u);
        int biggest = 0, at = -1;
        for (int i = 0; i < nh; i++) {
            if (!blend_is_angle(cur[i]) || !blend_is_angle(dbg_prev[i])) continue;
            int d = cur[i] - dbg_prev[i];
            if (d >  2048) d -= 4096;
            if (d < -2048) d += 4096;
            if (d < 0) d = -d;
            if (d > biggest) { biggest = d; at = i; }
        }
        for (int i = 0; i < nh; i++) dbg_prev[i] = cur[i];
        static long q = 0;
        if (biggest > 200 || (q++ % 200) == 0)
            fprintf(stderr, "anim: src=%08X sw=%ld bl=%d maxstep=%d(bone%d) head=[%d %d %d %d %d %d]%c",
                    src, s_switches, s_left, biggest, at < 0 ? -1 : at / 3,
                    now[0], now[1], now[2], now[3], now[4], now[5], 10);
    }
}

/* ---------------------------------------------------------------------------
 * Camera collision against the GAME's own collision records.
 *
 * Found by logging what the movement module reads while she walks into a wall:
 * the room's collision section (ctx+340) carries 16-byte records from +0x11C,
 * four int16 each, in world units — the same data that stops the character.
 * Using it means the camera is blocked by exactly what blocks Regina, with no
 * mesh, no room library and no matrices.
 * ------------------------------------------------------------------------- */
#define COLLREC_MAX 128
static float s_coll[COLLREC_MAX][4];   /* x0,z0,x1,z1 (min/max) */
static int   s_coll_n = 0;

int dc_coll_count(void) { return s_coll_n; }

void dc_coll_refresh(CPUState *cpu)
{
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_CAM_GAMECOLL"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) { s_coll_n = 0; return; }
    uint32_t ctx = cpu->read_word(0x1F800000u) & 0x1FFFFFu;
    if (!ctx || ctx + 400u >= 0x200000u) { s_coll_n = 0; return; }
    uint32_t sec = cpu->read_word(ctx + 340u) & 0x1FFFFFu;
    if (!sec || sec + 0x400u >= 0x200000u) { s_coll_n = 0; return; }
    static uint32_t off = 0;
    if (!off) { const char *e = getenv("DC_COLL_OFF"); off = e ? (uint32_t)strtoul(e,0,0) : 0x114u; }
    int n = 0;
    for (int r = 0; r < COLLREC_MAX; r++) {
        uint32_t a = sec + off + (uint32_t)r * 16u;
        int v0 = (int16_t)cpu->read_half(a);
        int v1 = (int16_t)cpu->read_half(a + 2);
        int v2 = (int16_t)cpu->read_half(a + 4);
        int v3 = (int16_t)cpu->read_half(a + 6);
        if (!v0 && !v1 && !v2 && !v3) break;              /* end of list */
        float x0 = (float)v0, z0 = (float)v1, x1 = (float)v2, z1 = (float)v3;
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("DC_COLL_FMT"); mode = e ? atoi(e) : 0; }
        if (mode == 1) { x1 = x0 + x1; z1 = z0 + z1; }    /* x,z,w,h */
        s_coll[n][0] = x0 < x1 ? x0 : x1;
        s_coll[n][1] = z0 < z1 ? z0 : z1;
        s_coll[n][2] = x0 < x1 ? x1 : x0;
        s_coll[n][3] = z0 < z1 ? z1 : z0;
        n++;
    }
    s_coll_n = n;
    { static int st = -1, k = 0;
      if (st < 0) { const char *e = getenv("DC_CAM_MESH_STAT"); st = (e && *e=='1') ? 1 : 0; }
      if (st && (k++ % 300) == 0)
        fprintf(stderr, "gamecoll: %d records from %06X%c", n, sec + off, 10); }
}

/* Nearest blocking distance along the horizontal ray, or maxt if clear. */
float dc_coll_raycast(float ox, float oz, float dx, float dz, float maxt)
{
    float best = maxt;
    for (int i = 0; i < s_coll_n; i++) {
        const float *b = s_coll[i];
        /* slab test in 2D */
        float t0 = 0.f, t1 = maxt;
        for (int ax = 0; ax < 2; ax++) {
            float o = ax ? oz : ox, d = ax ? dz : dx;
            float lo = ax ? b[1] : b[0], hi = ax ? b[3] : b[2];
            if (d > -1e-6f && d < 1e-6f) { if (o < lo || o > hi) { t0 = 1.f; t1 = 0.f; break; } continue; }
            float ta = (lo - o) / d, tb = (hi - o) / d;
            if (ta > tb) { float s = ta; ta = tb; tb = s; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) break;
        }
        if (t0 <= t1 && t0 > 1.f && t0 < best) best = t0;
    }
    return best;
}

/* ===== КОЛЬЦО ВСЕХ РАСПАКОВОК КАДРОВ (2026-09-05, вопрос игрока «используются
 * ли стойки прицела в нашем аиме?») =========================================
 * Настоящий распаковщик — func_80060CEC(a0 = кадр-источник, a1 = буфер углов,
 * a2 = число костей). Хук pt_anim_frame выше стоит только на двух его вызовах
 * из машины состояний движения (func_800607FC); позу прицела игра строит
 * другим путём (func_80050D60 — смесь двух кадров — тоже зовёт распаковщик).
 * Этот хук стоит на ВХОДЕ func_80060CEC (генкод) и пишет каждый вызов в
 * кольцо; debug-команда anim_ring отдаёт хвост, харнес сопоставляет адреса
 * источников с таблицами моушенов (CORE @0x180000, оружейный набор @0x160000). */
typedef struct { uint32_t src, dst, n, ra; uint32_t frame; } AnimRingEnt;
#define ANIM_RING 256
static AnimRingEnt s_aring[ANIM_RING]; static uint32_t s_aring_head = 0; static uint64_t s_aring_total = 0;
static void ap_tick(CPUState *cpu, uint32_t src);   /* aim patch, ниже */
void pt_anim_unpack_note(CPUState *cpu) {
    extern uint64_t s_frame_count;
    AnimRingEnt *e = &s_aring[s_aring_head];
    e->src = cpu->gpr[4]; e->dst = cpu->gpr[5]; e->n = cpu->gpr[6]; e->ra = cpu->gpr[31];
    e->frame = (uint32_t)s_frame_count;
    s_aring_head = (s_aring_head + 1) % ANIM_RING; s_aring_total++;
    ap_tick(cpu, cpu->gpr[4]);       /* до распаковки: переписанный кадр игра прочитает сразу */
}
/* хвост кольца: count последних записей, от старой к новой; возвращает число */
int pt_anim_ring_tail(int count, uint32_t *src, uint32_t *dst, uint32_t *n, uint32_t *ra, uint32_t *frame) {
    if (count > ANIM_RING) count = ANIM_RING;
    if ((uint64_t)count > s_aring_total) count = (int)s_aring_total;
    for (int i = 0; i < count; i++) {
        const AnimRingEnt *e = &s_aring[(s_aring_head + ANIM_RING - count + i) % ANIM_RING];
        src[i] = e->src; dst[i] = e->dst; n[i] = e->n; ra[i] = e->ra; frame[i] = e->frame;
    }
    return count;
}

/* ===== СНЯТИЕ ИТОГОВОЙ ПОЗЫ (2026-09-05 ночь, аддитивный выстрел) ===========
 * Формат выхода распаковщика func_80060CEC (по коду): на кость 8 байт —
 * три полуслова углов 12-бит (x, y, z) и полуслово-заполнитель; кости идут
 * подряд с a1. Смеситель func_80050D60 распаковывает две стойки в стек
 * (скретчпад — читать снаружи бесполезно, стек затирается) и сводит их
 * func_80066630(a0 = текущая поза, a1 = цель, ...). Хуки ниже пишут:
 *  - вход интерполятора: a0..a3, ra (где лежит итоговая поза);
 *  - выход распаковщика: снимок буфера a1 (n костей × 8 байт) — сами углы. */
typedef struct { uint32_t a0, a1, a2, a3, ra, frame; } InterpEnt;
#define INTERP_RING 64
static InterpEnt s_iring[INTERP_RING]; static uint32_t s_iring_head = 0; static uint64_t s_iring_total = 0;
void pt_anim_interp_note(CPUState *cpu) {
    extern uint64_t s_frame_count;
    InterpEnt *e = &s_iring[s_iring_head];
    e->a0 = cpu->gpr[4]; e->a1 = cpu->gpr[5]; e->a2 = cpu->gpr[6]; e->a3 = cpu->gpr[7]; e->ra = cpu->gpr[31];
    e->frame = (uint32_t)s_frame_count;
    s_iring_head = (s_iring_head + 1) % INTERP_RING; s_iring_total++;
}
int pt_anim_interp_tail(int count, uint32_t *a0, uint32_t *a1, uint32_t *a2, uint32_t *a3, uint32_t *ra, uint32_t *frame) {
    if (count > INTERP_RING) count = INTERP_RING;
    if ((uint64_t)count > s_iring_total) count = (int)s_iring_total;
    for (int i = 0; i < count; i++) {
        const InterpEnt *e = &s_iring[(s_iring_head + INTERP_RING - count + i) % INTERP_RING];
        a0[i] = e->a0; a1[i] = e->a1; a2[i] = e->a2; a3[i] = e->a3; ra[i] = e->ra; frame[i] = e->frame;
    }
    return count;
}
/* снимки выхода распаковщика: последние 8 вызовов, до 16 костей */
typedef struct { uint32_t src, dst, n, ra, frame; uint16_t q[16][4]; } UnpackOut;
#define UOUT_RING 8
static UnpackOut s_uout[UOUT_RING]; static uint32_t s_uout_head = 0; static uint64_t s_uout_total = 0;
void pt_anim_unpack_done(CPUState *cpu) {
    /* последняя запись кольца входов = этот вызов (функция не рекурсивна) */
    const AnimRingEnt *e = &s_aring[(s_aring_head + ANIM_RING - 1) % ANIM_RING];
    UnpackOut *o = &s_uout[s_uout_head];
    o->src = e->src; o->dst = e->dst; o->n = e->n; o->ra = e->ra; o->frame = e->frame;
    uint32_t n = e->n > 16 ? 16 : e->n;
    for (uint32_t b = 0; b < n; b++)
        for (int k = 0; k < 4; k++) o->q[b][k] = cpu->read_half(e->dst + b * 8u + (uint32_t)k * 2u);
    s_uout_head = (s_uout_head + 1) % UOUT_RING; s_uout_total++;
}
int pt_anim_unpack_out(int idx_from_newest, uint32_t *src, uint32_t *dst, uint32_t *n, uint32_t *ra, uint32_t *frame, uint16_t q[16][4]) {
    if ((uint64_t)idx_from_newest >= s_uout_total || idx_from_newest >= UOUT_RING) return 0;
    const UnpackOut *o = &s_uout[(s_uout_head + UOUT_RING - 1 - idx_from_newest) % UOUT_RING];
    *src = o->src; *dst = o->dst; *n = o->n; *ra = o->ra; *frame = o->frame;
    for (int b = 0; b < 16; b++) for (int k = 0; k < 4; k++) q[b][k] = o->q[b][k];
    return 1;
}

/* ===== AIM PATCH: ПРАВЛЕНЫЕ СТОЙКИ ПРИЦЕЛА + АДДИТИВНЫЙ ВЫСТРЕЛ (2026-09-05 ночь) ====
 * Игрок правит стойки прицела в Maya (export/regina_fbx/edited), tools/dc_extract/
 * make_aim_patch.py переводит их обратно в 12-бит углы игры и пишет assets/aim_patch.bin.
 * Здесь: (1) пока оружейный набор лежит в RAM по 0x160000 (MOTION_BASE), кадры его
 * стоек заменяются на правленые — смешивание по углу прицела делает сама игра
 * (func_80050D60: две стойки -> вес -> углы узлов скелета 0xB94EC+172k+40);
 * (2) выстрел аддитивный: на первом кадре анимации выстрела (любой из трёх) её
 * кадры в RAM переписываются как (текущая смешанная поза из узлов) + (кадр_j −
 * кадр_0 эталона), т.е. одна и та же отдача поверх любого направления прицела —
 * «Регина стреляет туда, куда мы смотрим». Хук стоит на входе распаковщика, так
 * что переписанный кадр 0 игра читает сразу. DC_AIM_PATCH=0 — всё выключить,
 * DC_SHOT_ADDITIVE=0 — только стойки. */
#define AP_BLOCK 0x160000u
#define AP_NODES 0xB94ECu
#define AP_NODE_STRIDE 172u
#define AP_NODE_ANG 40u
typedef struct { uint16_t motion, kind; uint32_t off; uint16_t nframes; const uint8_t *frames;
                 uint32_t hdr_off; uint8_t hdr[8]; int has_hdr;
                 uint8_t *orig;   /* orig: кадры из RAM до нашей первой записи (стойка) — для выключения на лету */
                 /* v3 (17.09.2026, игрок: «aim_patch.bin хранить разницей с оригинальными кадрами, а сигнатуры —
                  * хешами»): в файле нет байтов игры. frames == NULL, пока кадры не собраны из RAM: оригинал
                  * берётся из блока набора в памяти (хеш orig_hash сверяет, что это кадры с диска) и к нему
                  * прибавляется delta (NULL — моушен не правлен, кадры = оригинал). */
                 int v3; uint64_t hdr_hash, orig_hash; const uint8_t *delta; uint32_t mat_try; } ApMotion;
typedef struct { char name[5]; uint32_t sig_off; uint8_t sig[64]; int v3; uint64_t sig_hash; int nmot; ApMotion mot[16]; } ApSet;
static ApSet s_ap[8]; static int s_ap_n = -1; static uint8_t *s_ap_blob = 0; static int s_ap_on = 1, s_ap_shot = 1;
static int s_ap_cur = -1;   /* (07.09) какой набор патча лежит в RAM (индекс s_ap) — ao_enter узнаёт по нему выстрел (kind 1) */
static struct { uint16_t base[45]; int motion, set, nframes, from_aim; uint32_t frame, seq; } s_ap_last;   /* последний переписанный выстрел (debug aim_shot_last) */
static struct { uint16_t pose[45]; uint32_t frame; int valid; } s_ap_aim;   /* поза спокойного прицела: снимок узлов при распаковке стойки */

/* ---- v3: хеши вместо байтов игры, кадры = оригинал из RAM + наша разница (17.09.2026) ---- */
static uint64_t ap_fnv64(const uint8_t *p, size_t n) {   /* FNV-1a 64 */
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}
static int ap_sig_match(const ApSet *S, const uint8_t *ram) {
    return S->v3 ? ap_fnv64(ram + AP_BLOCK + S->sig_off, 64) == S->sig_hash
                 : memcmp(ram + AP_BLOCK + S->sig_off, S->sig, 64) == 0;
}
static int ap_hdr_match(const ApMotion *M, const uint8_t *ram) {
    if (!M->has_hdr) return 1;
    return M->v3 ? ap_fnv64(ram + AP_BLOCK + M->hdr_off, 8) == M->hdr_hash
                 : memcmp(ram + AP_BLOCK + M->hdr_off, M->hdr, 8) == 0;
}
/* Кадр (80 байт) +/- разница: 6 полуслов корня — сложение с переносом, 45 углов по 12 бит — по модулю 4096.
 * Угол i лежит в битах 12*i от байта 12 и умещается в два байта (сдвиг 0 или 4); старшие 4 бита байта 79
 * после последнего угла не трогаются. */
static void ap_delta(uint8_t *fr, const uint8_t *d, int sign) {
    for (int i = 0; i < 12; i += 2) {
        uint16_t a = (uint16_t)(fr[i] | (fr[i + 1] << 8)), b = (uint16_t)(d[i] | (d[i + 1] << 8));
        uint16_t r = (uint16_t)(sign > 0 ? a + b : a - b);
        fr[i] = (uint8_t)r; fr[i + 1] = (uint8_t)(r >> 8);
    }
    for (int i = 0; i < 45; i++) {
        int bo = i * 12, at = 12 + (bo >> 3), sh = bo & 7;
        uint32_t w  = (uint32_t)fr[at] | ((uint32_t)fr[at + 1] << 8);
        uint32_t dw = (uint32_t)d[at]  | ((uint32_t)d[at + 1] << 8);
        uint32_t v = (w >> sh) & 0xFFF, dv = (dw >> sh) & 0xFFF;
        v = (sign > 0 ? v + dv : v + 4096u - dv) & 0xFFF;
        w = (w & ~(0xFFFu << sh)) | (v << sh);
        fr[at] = (uint8_t)w; fr[at + 1] = (uint8_t)(w >> 8);
    }
}
/* Собрать кадры моушена v3 из RAM. Блок набора в памяти держит либо кадры с диска (хеш совпал сразу),
 * либо уже наши (квиклоад сейва, снятого с правлеными кадрами): тогда разница вычитается и хеш сверяется
 * ещё раз. Не сошлось — данные чужие, моушен не трогаем (повтор не чаще раза в 30 кадров). */
static int ap_materialize(ApMotion *M, const uint8_t *ram) {
    size_t len = (size_t)M->nframes * 80u;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return 0;
    memcpy(buf, ram + AP_BLOCK + M->off, len);
    int ok = (ap_fnv64(buf, len) == M->orig_hash);
    if (!ok && M->delta) {
        for (uint32_t j = 0; j < M->nframes; j++) ap_delta(buf + j * 80u, M->delta + j * 80u, -1);
        ok = (ap_fnv64(buf, len) == M->orig_hash);
    }
    if (!ok) { free(buf); return 0; }
    if (!M->orig) { M->orig = (uint8_t *)malloc(len); if (M->orig) memcpy(M->orig, buf, len); }   /* 04.10 (6): кадры с диска — для оригинального режима */
    if (M->delta) for (uint32_t j = 0; j < M->nframes; j++) ap_delta(buf + j * 80u, M->delta + j * 80u, +1);
    M->frames = buf;
    return 1;
}
/* ⭐ 04.10 (6): кадры с диска для файла v2 (дев) — из файла v3 того же патча (assets/aim_patch_v3.bin, собран
 * make_aim_patch_v3.py из этого v2 и диска: разница и хеш оригинала, байтов игры нет): оригинал = правленый − разница,
 * сверка хешем. Нужен оригинальному режиму: сейвы сняты в нашем режиме — в их RAM правленые стойки, и снять копию
 * с диска «до первой записи» (M->orig в ap_tick) рантайм может так и не успеть. */
static void ap_v2_orig_from_v3(void) {
    const char *paths[] = { getenv("DC_AIM_PATCH_ORIG"), "assets/aim_patch_v3.bin", "../../assets/aim_patch_v3.bin", "../../../assets/aim_patch_v3.bin" };
    FILE *f = 0;
    for (int i = 0; i < 4 && !f; i++) if (paths[i] && *paths[i]) f = fopen(paths[i], "rb");
    if (!f) { fprintf(stderr, "aimpatch: нет assets/aim_patch_v3.bin — кадры с диска для оригинального режима только из RAM\n"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)(n > 0 ? n : 1));
    if (!b || n < 12 || fread(b, 1, (size_t)n, f) != (size_t)n || memcmp(b, "DCAP", 4) != 0 || *(uint32_t *)(b + 4) < 3) {
        fclose(f); free(b); fprintf(stderr, "aimpatch: aim_patch_v3.bin не v3 — пропуск\n"); return; }
    fclose(f);
    const uint8_t *p = b + 12, *end = b + n;
    const uint32_t nsets = *(uint32_t *)(b + 8);
    int got = 0, same = 0, bad = 0;
    for (uint32_t s = 0; s < nsets && p + 20 <= end; s++) {
        char name[5]; memcpy(name, p, 4); name[4] = 0; p += 4 + 4 + 8;
        const uint32_t nm = *(uint32_t *)p; p += 4;
        for (uint32_t m = 0; m < nm && p + 32 <= end; m++) {
            const uint16_t motion = *(uint16_t *)p, nfr = *(uint16_t *)(p + 8), flags = *(uint16_t *)(p + 10);
            const uint32_t off = *(uint32_t *)(p + 4);
            uint64_t oh; memcpy(&oh, p + 24, 8);
            p += 32;
            const uint8_t *delta = (flags & 1) ? p : NULL;
            if (flags & 1) p += (size_t)nfr * 80u;
            if (p > end) break;
            for (int k = 0; k < s_ap_n; k++) {
                if (strcmp(s_ap[k].name, name) != 0) continue;
                for (int q = 0; q < s_ap[k].nmot; q++) {
                    ApMotion *M = &s_ap[k].mot[q];
                    if (M->motion != motion || M->off != off || M->nframes != nfr || !M->frames || M->orig) continue;
                    const size_t len = (size_t)nfr * 80u;
                    uint8_t *o = (uint8_t *)malloc(len); if (!o) continue;
                    memcpy(o, M->frames, len);
                    if (delta) for (uint32_t j = 0; j < nfr; j++) ap_delta(o + j * 80u, delta + j * 80u, -1);
                    if (ap_fnv64(o, len) != oh) { free(o); bad++; continue; }
                    M->orig = o; if (delta) got++; else same++;
                }
            }
        }
    }
    free(b);
    fprintf(stderr, "aimpatch: кадры с диска из aim_patch_v3.bin — правленых %d, без правки %d, не сошлось %d\n", got, same, bad);
}
static int ap_frames_ready(ApMotion *M, const uint8_t *ram) {
    if (M->frames) return 1;
    if (!M->v3) return 0;
    extern uint64_t s_frame_count;
    uint32_t now = (uint32_t)s_frame_count;
    if (M->mat_try && now - M->mat_try < 30) return 0;
    M->mat_try = now ? now : 1;
    return ap_materialize(M, ram);
}

static void ap_load(void) {
    s_ap_n = 0;
    const char *e = getenv("DC_AIM_PATCH"); if (e && *e == '0') { s_ap_on = 0; return; }
    e = getenv("DC_SHOT_ADDITIVE"); if (e && *e == '0') s_ap_shot = 0;
    /* The edited stances are built into the program (dc_embedded.c) - they
     * do not belong next to the game as a loose file. DC_AIM_PATCH_FILE and
     * the paths below stay for a quick test of a fresh pose build. */
    long n = 0;
    const char *env_file = getenv("DC_AIM_PATCH_FILE");
    if (!env_file || !*env_file) {
        unsigned nb = 0;
        const unsigned char *blob = dc_embedded("aim_patch.bin", &nb);
        if (blob && nb) {
            s_ap_blob = (uint8_t *)malloc(nb);
            if (!s_ap_blob) return;
            memcpy(s_ap_blob, blob, nb);
            n = (long)nb;
        }
    }
    if (!n) {
        const char *paths[] = { env_file, "assets/aim_patch.bin", "../../assets/aim_patch.bin", "../../../assets/aim_patch.bin" };
        FILE *f = 0;
        for (int i = 0; i < 4 && !f; i++) if (paths[i] && *paths[i]) f = fopen(paths[i], "rb");
        if (!f) { fprintf(stderr, "aimpatch: aim_patch.bin not found - no edited stances\n"); return; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        s_ap_blob = (uint8_t *)malloc((size_t)n); if (!s_ap_blob || fread(s_ap_blob, 1, (size_t)n, f) != (size_t)n) { fclose(f); return; }
        fclose(f);
    }
    const uint8_t *p = s_ap_blob, *end = s_ap_blob + n;
    if (n < 12 || memcmp(p, "DCAP", 4) != 0) { fprintf(stderr, "aimpatch: плохой заголовок\n"); return; }
    uint32_t ver = *(uint32_t *)(p + 4), nsets = *(uint32_t *)(p + 8); p += 12;
    /* v1 брала сигнатурой первые 64 байта блока — а они у ВСЕХ наборов одинаковы (общий M00
     * idle): с гранатомётом в RAM рантайм «узнавал» P20 и писал стойки пистолета в кадры P30 M10
     * (выстрел гранатомёта) — «меш ломается на пару кадров при входе в прицел» (игрок, 05.09).
     * v2: сигнатура со смещения sig_off (96 — там наборы различаются) + заголовок каждого
     * моушена (dptr,cnt) сверяется перед записью. v1-файл больше не принимаем. */
    if (ver < 2) { fprintf(stderr, "aimpatch: файл версии %u устарел (сигнатура не различает наборы) — пересобери make_aim_patch.py, патч выключен\n", ver); return; }
    if (ver >= 3) {
        /* v3 (17.09.2026; релиз, _source/tools/make_aim_patch_v3.py из v2 и диска): байтов игры нет.
         *   набор:  char name[4], u32 sig_off, u64 sig_hash (FNV-1a 64 байт блока с sig_off), u32 n_motions
         *   моушен: u16 motion, u16 kind, u32 off, u16 nframes, u16 flags, u32 hdr_off, u64 hdr_hash (8 байт
         *           заголовка), u64 orig_hash (кадры с диска); flags bit0 — дальше nframes*80 байт разницы
         *           (корень: полуслова, углы: 12 бит по модулю 4096 в раскладке кадра). */
        for (uint32_t s = 0; s < nsets && s < 8 && p + 4 + 4 + 8 + 4 <= end; s++) {
            ApSet *S = &s_ap[s]; memcpy(S->name, p, 4); S->name[4] = 0; p += 4;
            S->sig_off = *(uint32_t *)p; p += 4;
            memcpy(&S->sig_hash, p, 8); p += 8; S->v3 = 1;
            uint32_t nm = *(uint32_t *)p; p += 4; S->nmot = 0;
            for (uint32_t m = 0; m < nm && p + 32 <= end; m++) {
                ApMotion M; memset(&M, 0, sizeof M);
                M.motion = *(uint16_t *)p; M.kind = *(uint16_t *)(p + 2); M.off = *(uint32_t *)(p + 4);
                M.nframes = *(uint16_t *)(p + 8);
                uint16_t flags = *(uint16_t *)(p + 10);
                M.hdr_off = *(uint32_t *)(p + 12); memcpy(&M.hdr_hash, p + 16, 8); memcpy(&M.orig_hash, p + 24, 8);
                M.has_hdr = 1; M.v3 = 1; p += 32;
                if (flags & 1) { M.delta = p; p += (size_t)M.nframes * 80u; }
                if (p > end) break;
                if (S->nmot < 16) S->mot[S->nmot++] = M;
            }
            s_ap_n = (int)s + 1;
        }
    } else
    for (uint32_t s = 0; s < nsets && s < 8 && p + 4 + 4 + 64 + 4 <= end; s++) {
        ApSet *S = &s_ap[s]; memcpy(S->name, p, 4); S->name[4] = 0; p += 4;
        S->sig_off = *(uint32_t *)p; p += 4;
        memcpy(S->sig, p, 64); p += 64;
        uint32_t nm = *(uint32_t *)p; p += 4; S->nmot = 0;
        for (uint32_t m = 0; m < nm && p + 24 <= end; m++) {
            ApMotion M; memset(&M, 0, sizeof M);
            M.motion = *(uint16_t *)p; M.kind = *(uint16_t *)(p + 2); M.off = *(uint32_t *)(p + 4);
            M.nframes = *(uint16_t *)(p + 8); p += 12;
            M.hdr_off = *(uint32_t *)p; memcpy(M.hdr, p + 4, 8); M.has_hdr = 1; p += 12;
            M.frames = p; p += (size_t)M.nframes * 80u;
            if (p > end) break;
            if (S->nmot < 16) S->mot[S->nmot++] = M;
        }
        s_ap_n = (int)s + 1;
    }
    fprintf(stderr, "aimpatch: загружено наборов %d (стойки правленые/оригинал + эталоны выстрелов), аддитивный выстрел %s\n",
            s_ap_n, s_ap_shot ? "вкл" : "выкл");
    if (ver == 2 && s_ap_n > 0) ap_v2_orig_from_v3();
}
/* 12-бит углы кадра (80 байт) <-> 45 значений */
/* 18.10.2026 (разбор кода): 45 углов по 12 бит - ровно байты 12..79 кадра; угол со сдвигом 0 или 4 целиком в двух
 * байтах. Третий байт брался лишним, и у последнего угла (байт 66) он за концом кадра: чтение за последним кадром
 * набора (за концом выделенной памяти) и запись ap_pack45 на байт за своим буфером. */
static void ap_unpack45(const uint8_t *fr, uint16_t *out) {
    const uint8_t *bits = fr + 12;
    for (int i = 0; i < 45; i++) {
        int bo = i * 12, byte = bo >> 3, sh = bo & 7;
        uint32_t w = bits[byte] | ((uint32_t)bits[byte + 1] << 8) | (byte + 2 < 68 ? ((uint32_t)bits[byte + 2] << 16) : 0u);
        out[i] = (uint16_t)((w >> sh) & 0xFFF);
    }
}
static void ap_pack45(uint8_t *fr, const uint16_t *ang) {   /* только биты углов, корень не трогаем */
    uint8_t bits[68]; memset(bits, 0, sizeof bits);
    for (int i = 0; i < 45; i++) {
        int bo = i * 12, byte = bo >> 3, sh = bo & 7;
        uint32_t w = (uint32_t)(ang[i] & 0xFFF) << sh;
        bits[byte] |= (uint8_t)(w & 0xFF); bits[byte + 1] |= (uint8_t)((w >> 8) & 0xFF);
        if (byte + 2 < 68) bits[byte + 2] |= (uint8_t)((w >> 16) & 0xFF);
    }
    bits[67] |= (uint8_t)(fr[12 + 67] & 0xF0);   /* 18.10: старшая тетрада байта 79 - не угол, остаётся как была (как в ap_delta) */
    memcpy(fr + 12, bits, 68);
}
static int s_ao_byte; static uint32_t s_ao_byte_frame;   /* определены ниже (аим-оффсет) */
static long s_ao_byte_writes = 0; static int s_ao_byte_last = -1;
static void ap_restore_disk(uint8_t *ram, const char *why);
static void ap_tick(CPUState *cpu, uint32_t src) {
    (void)cpu;
    if (s_ap_n < 0) ap_load();
    if (!s_ap_on || s_ap_n <= 0) return;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    /* ⭐ 04.10 (6): в оригинальном режиме стойки прицела и выстрелы — родные с диска (игрок: «отключай при переключении на
     * оригинальный режим все нововведения… прицеливания»). На входе в режим кадры возвращаются к диску, пока режим идёт —
     * ни стоек, ни аддитивного выстрела, ни байта высоты от нашего прицела; на выходе правленые кадры пишутся сами (ниже,
     * memcmp). DC_ORIG_PURE=0 — как было. */
    { extern int dc_orig_pure(void);
      static int was_pure = 0;
      const int pure = dc_orig_pure();
      if (pure && !was_pure) ap_restore_disk(ram, "оригинальный режим");
      if (!pure && was_pure) fprintf(stderr, "aimpatch: наш режим — правленые стойки вернутся с первой распаковкой\n");
      was_pure = pure;
      if (pure) {
          /* и дальше держать диск: быстрая загрузка сейва, снятого в нашем режиме, приносит в RAM правленые кадры */
          static long nk = 0;
          for (int s = 0; s < s_ap_n; s++) {
              if (!ap_sig_match(&s_ap[s], ram)) continue;
              for (int m = 0; m < s_ap[s].nmot; m++) {
                  ApMotion *M = &s_ap[s].mot[m];
                  const uint8_t *disk = (M->kind == 1) ? M->frames : M->orig;
                  if (!disk || !ap_hdr_match(M, ram)) continue;
                  uint8_t *dst = ram + AP_BLOCK + M->off; const size_t len = (size_t)M->nframes * 80u;
                  if (memcmp(dst, disk, len) != 0) {
                      memcpy(dst, disk, len);
                      if (++nk <= 20) fprintf(stderr, "aimpatch: оригинальный режим — %s M%02d снова с диска (правленые кадры пришли из сейва)\n", s_ap[s].name, M->motion);
                  }
              }
          }
          return;
      } }
    /* какой набор сейчас лежит по 0x160000 */
    /* какой набор сейчас лежит по 0x160000: сигнатура со смещения sig_off (у v1 — первые 64
     * байта — наборы не различались, см. ap_load). Пересчитывать при любой смене этих байт. */
    static int cur = -1; static uint32_t chk = 0; static int chk_valid = 0;
    uint32_t so = s_ap[0].sig_off;
    uint32_t head = *(uint32_t *)(ram + AP_BLOCK + so) ^ *(uint32_t *)(ram + AP_BLOCK + so + 60) ^ *(uint32_t *)(ram + AP_BLOCK + so + 28);
    if (!chk_valid || head != chk) {
        cur = -1; chk = head; chk_valid = 1;
        for (int s = 0; s < s_ap_n; s++) if (ap_sig_match(&s_ap[s], ram)) { cur = s; break; }
        static int lastlog = -2;
        if (cur != lastlog) { lastlog = cur; fprintf(stderr, "aimpatch: в RAM набор %s\n", cur >= 0 ? s_ap[cur].name : "(неизвестный)"); }
        s_ap_cur = cur;
    }
    { extern uint64_t s_frame_count; uint32_t sp0 = src & 0x1FFFFFu;
    /* миксер игры (func_80050D60) распаковывает пару кадров и ПОСЛЕ этого читает байт высоты
     * прицела ctx+102 = 0xB94B2 как вес смеси: пишем сюда наш байт — он переживает и запись игрой
     * 128 / её шаг 16/тик к «прямо» при зажатой кнопке выстрела («стреляет просто прямо», рука
     * уезжает вниз на старте выстрела — кадры 54–56 игрока, 06.09). Любой моушен оружейного
     * блока: блок @0x160000 распаковывает только Регина, наборы вне патча тоже. */
      if (sp0 >= AP_BLOCK && sp0 < AP_BLOCK + 0x20000u && s_ao_byte >= 0 && (uint32_t)s_frame_count - s_ao_byte_frame <= 30 && ram[0xB92C9] == 6)
          { ram[0xB94B2] = (uint8_t)s_ao_byte; s_ao_byte_writes++; s_ao_byte_last = s_ao_byte; } }
    if (cur < 0) return;
    ApSet *S = &s_ap[cur];
    /* (1) стойки: держать правленые кадры в RAM — только если заголовок моушена на месте.
     * Попутно: распаковка кадра СТОЙКИ = тик спокойного прицела → снимок углов узлов (поза
     * предыдущего тика, тоже спокойная) — это база для аддитивного выстрела. Брать базу из
     * узлов в момент распаковки кадра 0 выстрела НЕЛЬЗЯ (игрок 06.09: «при выстреле вниз на 5°
     * выше, вверх — на 5° ниже»; замер node_ring: в первый тик выстрела игра ДО распаковки
     * пишет в узлы переходную позу, смещённую к «прямо» — плечо 5.1°, кисть 2.5°, — в
     * оригинале это одно-тиковый рывок с возвратом за ~3 тика, а у нас он становился базой
     * всего выстрела). */
    extern uint64_t s_frame_count;
    uint32_t sp = src & 0x1FFFFFu;
    for (int m = 0; m < S->nmot; m++) {
        ApMotion *M = &S->mot[m];
        if (M->kind != 0) continue;
        if (!ap_hdr_match(M, ram)) continue;   /* не тот набор/моушен — не трогать */
        uint32_t f0 = AP_BLOCK + M->off;
        if (M->nframes <= 2 && sp >= f0 + 12 && sp < f0 + (uint32_t)M->nframes * 80u) {   /* стойка = 1–2 кадра; выстрелы (тоже kind 0) не считаются */
            for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++)
                s_ap_aim.pose[b*3+c] = (uint16_t)(*(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)b + AP_NODE_ANG + 2u * (uint32_t)c) & 0xFFF);
            s_ap_aim.frame = (uint32_t)s_frame_count; s_ap_aim.valid = 1;
        }
        if (!ap_frames_ready(M, ram)) continue;   /* v3: кадры ещё не собраны из RAM */
        uint8_t *dst = ram + AP_BLOCK + M->off; size_t len = (size_t)M->nframes * 80u;
        if (memcmp(dst, M->frames, len) != 0) {
            if (!M->orig) { M->orig = (uint8_t *)malloc(len); if (M->orig) memcpy(M->orig, dst, len); }   /* первый раз: запомнить диск */
            memcpy(dst, M->frames, len);
            static long np = 0; if (++np <= 12) fprintf(stderr, "aimpatch: %s M%02d — кадры стойки записаны в RAM\n", S->name, M->motion);
        }
    }
    /* (2) аддитивный выстрел: на ПЕРВОМ кадре 0 нового проигрывания анимации выстрела
     * переписать её кадры. Игра смешивает ДВЕ анимации выстрела (соседние стойки по
     * углу) и зовёт распаковщик для обеих по очереди, кадр 0 может держаться
     * несколько тиков — поэтому «новый экземпляр» считаем по-моушенно: k==0 после
     * k!=0 или после паузы >15 кадров хоста. */
    if (!s_ap_shot) return;
    for (int m = 0; m < S->nmot; m++) {
        ApMotion *M = &S->mot[m];
        if (M->kind != 1) continue;
        uint32_t f0 = AP_BLOCK + M->off;
        if (sp < f0 + 12 || sp >= f0 + (uint32_t)M->nframes * 80u) continue;
        if (!ap_hdr_match(M, ram)) return;   /* заголовок моушена не совпал — чужие данные */
        if (!ap_frames_ready(M, ram)) return;   /* v3: эталон ещё не собран из RAM */
        uint32_t k = (sp - 12 - f0) / 80u;
        static uint32_t lastk[8][16], lastfr[8][16];
        uint32_t pk = lastk[cur][m], pf = lastfr[cur][m], now = (uint32_t)s_frame_count;
        lastk[cur][m] = k; lastfr[cur][m] = now;
        if (k != 0) return;                          /* середина выстрела */
        if (pk == 0 && pf && now - pf <= 15) return; /* тот же кадр 0 держится ещё тик */
        uint16_t base[45], o0[45], oj[45], nw[45];
        int from_aim = (s_ap_aim.valid && now - s_ap_aim.frame <= 8);   /* стойка распаковывалась ≤4 тика назад */
        if (from_aim) memcpy(base, s_ap_aim.pose, sizeof base);
        else for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++)       /* выстрел не из спокойного прицела — текущие узлы */
            base[b*3+c] = (uint16_t)(*(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)b + AP_NODE_ANG + 2u * (uint32_t)c) & 0xFFF);
        s_ap_last.from_aim = from_aim;
        memcpy(s_ap_last.base, base, sizeof base); s_ap_last.motion = M->motion; s_ap_last.set = cur;
        s_ap_last.frame = now; s_ap_last.nframes = M->nframes; s_ap_last.seq++;
        ap_unpack45(M->frames, o0);
        for (uint32_t j = 0; j < M->nframes; j++) {
            ap_unpack45(M->frames + j * 80u, oj);
            for (int i = 0; i < 45; i++) {
                int d = ((int)oj[i] - (int)o0[i] + 2048) & 0xFFF; d -= 2048;   /* кратчайшая дуга */
                nw[i] = (uint16_t)(((int)base[i] + d) & 0xFFF);
            }
            uint8_t *fr = ram + f0 + j * 80u;
            memcpy(fr, M->frames + j * 80u, 12);        /* корень — эталонный */
            ap_pack45(fr, nw);
        }
        static long ns = 0; if (++ns <= 20) fprintf(stderr, "aimpatch: выстрел %s M%02d — %u кадров переписаны аддитивно поверх текущей позы (base0=%u,%u,%u)\n",
                                                   S->name, M->motion, M->nframes, base[0], base[1], base[2]);
        return;
    }
}
/* debug: последний аддитивно переписанный выстрел — база (углы узлов на кадре 0), моушен, набор */
int pt_anim_aim_shot_last(uint16_t *base45, int *motion, char *setname, int *nframes, uint32_t *frame, uint32_t *seq) {
    if (!s_ap_last.seq) return 0;
    memcpy(base45, s_ap_last.base, sizeof s_ap_last.base); *motion = s_ap_last.motion;
    strcpy(setname, s_ap_n > 0 && s_ap_last.set >= 0 ? s_ap[s_ap_last.set].name : "?");
    *nframes = s_ap_last.nframes; *frame = s_ap_last.frame; *seq = s_ap_last.seq; return 1;
}
/* debug aim_patch {"on":0|1}: переключение на лету. При выключении кадры выстрелов (kind 1)
 * резидентного набора возвращаются к эталону — иначе «выкл» играл бы прошлую аддитивную
 * перезапись. Стойки (kind 0) при выключении к диску не возвращаются (сейчас они и так
 * оригинальные; правленые вернутся сами при включении). on<0 — только спросить. */
/* кадры резидентного набора — к диску: выстрелы (kind 1) — эталон, стойки (kind 0) — снятая при первой записи копия */
static void ap_restore_disk(uint8_t *ram, const char *why) {
    if (!ram || s_ap_n <= 0) return;
    for (int s = 0; s < s_ap_n; s++) {
        if (!ap_sig_match(&s_ap[s], ram)) continue;
        for (int m = 0; m < s_ap[s].nmot; m++) {
            ApMotion *M = &s_ap[s].mot[m];
            if (!ap_hdr_match(M, ram)) continue;
            if (M->kind == 1) { if (M->frames) memcpy(ram + AP_BLOCK + M->off, M->frames, (size_t)M->nframes * 80u); }   /* эталон выстрела */
            else if (M->orig) memcpy(ram + AP_BLOCK + M->off, M->orig, (size_t)M->nframes * 80u);     /* стойка — как на диске */
        }
        fprintf(stderr, "aimpatch: %s — кадры выстрелов и стоек %s возвращены к диску\n", why, s_ap[s].name);
    }
}
int pt_anim_aim_patch_toggle(int on) {
    if (s_ap_n < 0) ap_load();
    if (on < 0) return s_ap_on;
    uint8_t *ram = memory_get_ram_ptr();
    if (!on && ram && s_ap_n > 0) ap_restore_disk(ram, "выключен");
    s_ap_on = on ? 1 : 0;
    if (s_ap_on) fprintf(stderr, "aimpatch: включен\n");
    return s_ap_on;
}

/* ===== ПРОЦЕДУРНЫЙ АИМ-ОФФСЕТ (2026-09-06, план «1 и 2», игрок: «не так высоко задирает
 * оружие и не так низко опускает») =====================================================
 * 1) dc_aimoff_blend(): байт высоты прицела 0xB94B2 (0 = руки вверх, 128 = прямо, 255 = вниз;
 *    вес смеси стоек = |b-128|/128) считается не «камера x 110», а так, чтобы ОСЬ ОРУЖИЯ смотрела
 *    на высоту перекрестия: угол крайних стоек резидентного набора берётся из RAM (таблица
 *    моушенов блока @0x160000: три подряд 1-2-кадровых = стойки вверх/прямо/вниз) прямой
 *    кинематикой по тем же 15 костям, что и игра. Ось: пистолет (байт оружия 0x20) - плечо->кисть,
 *    дробовик/гранатомёт - правая кисть->левая (ствол). Работает для всех состояний здоровья.
 * 2) pt_anim_aimoff_enter/exit(): на входе в сборку скелета игры (func_80073F10) для Регины
 *    остаток (высота перекрестия - текущая высота оси) доворачивается ЖЁСТКИМ поворотом вокруг
 *    ЛИНИИ ПЛЕЧ (обе руки при двуручном хвате - кисти не расходятся, FK-проверка 06.09),
 *    долей на корпус (спина +s*th, таз -s*th - ноги на месте) и на голову; на выходе углы узлов
 *    возвращаются, чтобы кроссфейды игры доворот не видели. DC_AIMOFF=0 выкл, DC_AIMOFF_MAX
 *    (25 град), DC_AIMOFF_TORSO (0.25), DC_AIMOFF_HEAD (0.6), DC_AIMOFF_GAIN (1.0), DC_AIM_REMAP=0
 *    - старая формула байта. debug: aimoff. */
#include <math.h>
typedef struct { const char *name; int parent; float off[3]; } AoBone;
static const AoBone AO_BONES[15] = {
    {"spine", -1, {0, -1179, 0}}, {"arm_r_upper", 0, {145, -282, -49}}, {"arm_r_lower", 1, {50, 251, 1}},
    {"hand_r", 2, {42, 223, 36}}, {"arm_l_upper", 0, {-145, -282, -49}}, {"arm_l_lower", 4, {-50, 251, 1}},
    {"hand_l", 5, {-42, 223, 36}}, {"head", 0, {0, -291, -41}}, {"pelvis", 0, {0, -7, 0}},
    {"leg_r_upper", 8, {87, 198, -20}}, {"leg_r_lower", 9, {-9, 433, 0}}, {"foot_r", 10, {-7, 444, -49}},
    {"leg_l_upper", 8, {-87, 198, -20}}, {"leg_l_lower", 12, {9, 433, 0}}, {"foot_l", 13, {7, 444, -49}} };
enum { AO_SPINE = 0, AO_ARM_R = 1, AO_ELB_R = 2, AO_HAND_R = 3, AO_ARM_L = 4, AO_HAND_L = 6, AO_HEAD = 7, AO_PELVIS = 8 };
#define AO_ANG (6.283185307f / 4096.f)
static void ao_mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        o[i*3+j] = a[i*3]*b[j] + a[i*3+1]*b[3+j] + a[i*3+2]*b[6+j];
}
static void ao_tr(const float *a, float *o) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o[i*3+j] = a[j*3+i]; }
static void ao_mv(const float *m, const float *v, float *o) { for (int i = 0; i < 3; i++) o[i] = m[i*3]*v[0] + m[i*3+1]*v[1] + m[i*3+2]*v[2]; }
static void ao_euler(int ax, int ay, int az, float *m) {      /* libgte RotMatrix: Rx*Ry*Rz */
    float cx = cosf(ax * AO_ANG), sx = sinf(ax * AO_ANG), cy = cosf(ay * AO_ANG), sy = sinf(ay * AO_ANG), cz = cosf(az * AO_ANG), sz = sinf(az * AO_ANG);
    float rx[9] = {1,0,0, 0,cx,-sx, 0,sx,cx}, ry[9] = {cy,0,sy, 0,1,0, -sy,0,cy}, rz[9] = {cz,-sz,0, sz,cz,0, 0,0,1}, t[9];
    ao_mul(rx, ry, t); ao_mul(t, rz, m);
}
static void ao_decompose(const float *m, int *q) {           /* M = Rx Ry Rz -> 12-бит углы */
    float sy = m[2]; if (sy > 1.f) sy = 1.f; if (sy < -1.f) sy = -1.f;
    float ay = asinf(sy), ax, az;
    if (fabsf(m[2]) < 0.999999f) { ax = atan2f(-m[5], m[8]); az = atan2f(-m[1], m[0]); }
    else { ax = atan2f(m[7], m[4]); az = 0.f; }
    q[0] = (int)lroundf(ax / AO_ANG) & 0xFFF; q[1] = (int)lroundf(ay / AO_ANG) & 0xFFF; q[2] = (int)lroundf(az / AO_ANG) & 0xFFF;
}
static void ao_axis_rot(const float *axis, float deg, float *m) {   /* Родригес */
    float n = sqrtf(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]); if (n < 1e-6f) n = 1.f;
    float x = axis[0]/n, y = axis[1]/n, z = axis[2]/n, a = deg * 3.14159265f / 180.f, c = cosf(a), s = sinf(a), C = 1.f - c;
    float r[9] = { c + x*x*C, x*y*C - z*s, x*z*C + y*s,  y*x*C + z*s, c + y*y*C, y*z*C - x*s,  z*x*C - y*s, z*y*C + x*s, c + z*z*C };
    memcpy(m, r, sizeof r);
}
/* прямая кинематика: углы 45 -> мировые повороты W[15][9] и позиции P[15][3] (система персонажа) */
static void ao_fk(const int *ang, float W[15][9], float P[15][3]) {
    for (int b = 0; b < 15; b++) {
        float L[9]; ao_euler(ang[b*3], ang[b*3+1], ang[b*3+2], L);
        int p = AO_BONES[b].parent;
        if (p < 0) { memcpy(W[b], L, sizeof L); P[b][0] = AO_BONES[b].off[0]; P[b][1] = AO_BONES[b].off[1]; P[b][2] = AO_BONES[b].off[2]; }
        else { ao_mul(W[p], L, W[b]); float o[3]; ao_mv(W[p], AO_BONES[b].off, o); for (int i = 0; i < 3; i++) P[b][i] = P[p][i] + o[i]; }
    }
}
/* ось оружия -> угол возвышения, градусы, вверх положительно (Y игры вниз).
 * (06.09 вечер) Раньше: пистолет — плечо->кисть, двуручные — кисть->кисть. Для гранатомёта линия
 * кисть->кисть идёт ПОПЕРЁК тела (левая держит трубу сбоку-снизу: Δ по оси плеч 225, вперёд 5), и
 * поворот рук вокруг линии плеч её возвышения не менял вовсе (замер node_ring: руки поднялись на
 * 26–29°, ось «кисть->кисть» осталась +32°). Теперь ось = фиксированный вектор u в системе правой
 * кисти, откалиброванный по стойке «прямо»: там ствол горизонтален и смотрит вперёд (перпендикуляр
 * к линии плеч в сторону кистей). Жёсткий поворот руки на θ поднимает такой вектор ровно на θ. */
static void ao_unpack45(const uint8_t *fr, int *out) {
    const uint8_t *bits = fr + 12;
    for (int i = 0; i < 45; i++) { int bo = i * 12, by = bo >> 3, sh = bo & 7;
        uint32_t w = bits[by] | ((uint32_t)bits[by+1] << 8) | (by + 2 < 68 ? ((uint32_t)bits[by+2] << 16) : 0u); out[i] = (int)((w >> sh) & 0xFFF); }   /* 18.10: см. ap_unpack45 */
}
/* крайние стойки резидентного набора: три подряд моушена <=2 кадров в таблице блока @0x160000 */
static struct { uint32_t key; int valid; float e_up, e_level, e_down; int pistol; int mot[3]; int st_ang[3][45]; float u[3]; } s_ao_ext;   /* u = ствол в системе правой кисти (калибровка по стойке «прямо») */
static struct { uint32_t data; int nf; } s_ao_mots[64]; static int s_ao_nmot = 0;   /* все моушены набора: для прогресса вскидывания */
static int s_ao_axis_hand = 0;   /* DC_AIMOFF_AXIS=hand: ось = вектор в системе кисти (ствол); по умолчанию — прежняя мера */
static float ao_elev2(const float W[15][9], const float P[15][3]) {
    /* (06.09 ~18:00, игрок: «пистолет не направлен в сторону прицела, раньше прям хорошо направлен был»)
     * ПРЕЖНЯЯ мера возвращена по умолчанию: пистолет — линия плечо->кисть, двуручные — кисть->кисть.
     * Именно её игрок видел как «направлен на прицел»: в OTS-виде читается направление РУКИ, а не ствола
     * (ствол в стойке «вверх» на ~13° положе). Калиброванный вектор кисти (DC_AIMOFF_AXIS=hand) при том же
     * перекрестии поднимал руку на те 13° выше — «рука на 90°». */
    if (!s_ao_axis_hand) {
        float v[3];
        if (s_ao_ext.pistol) for (int i = 0; i < 3; i++) v[i] = P[AO_HAND_R][i] - P[AO_ARM_R][i];
        else                 for (int i = 0; i < 3; i++) v[i] = P[AO_HAND_L][i] - P[AO_HAND_R][i];
        return atan2f(-v[1], sqrtf(v[0]*v[0] + v[2]*v[2])) * 180.f / 3.14159265f;
    }
    float d[3]; ao_mv(W[AO_HAND_R], s_ao_ext.u, d);
    return atan2f(-d[1], sqrtf(d[0]*d[0] + d[2]*d[2])) * 180.f / 3.14159265f;
}
static void ao_calib_u(const float W[15][9], const float P[15][3]) {   /* по стойке «прямо» */
    float ax0 = P[AO_ARM_R][0] - P[AO_ARM_L][0], ax2 = P[AO_ARM_R][2] - P[AO_ARM_L][2];   /* линия плеч, горизонталь */
    float f[3] = { -ax2, 0.f, ax0 };                                                        /* перпендикуляр к ней */
    float fw0 = P[AO_HAND_R][0] - P[AO_SPINE][0], fw2 = P[AO_HAND_R][2] - P[AO_SPINE][2];   /* кисть — впереди тела */
    if (f[0]*fw0 + f[2]*fw2 < 0.f) { f[0] = -f[0]; f[2] = -f[2]; }
    float n = sqrtf(f[0]*f[0] + f[2]*f[2]); if (n < 1e-6f) n = 1.f; f[0] /= n; f[2] /= n;
    float WT[9]; ao_tr(W[AO_HAND_R], WT); ao_mv(WT, f, s_ao_ext.u);
}
static int ao_extremes(uint8_t *ram) {
    uint32_t key = *(uint32_t *)(ram + AP_BLOCK + 96) ^ *(uint32_t *)(ram + AP_BLOCK + 128) ^ ((uint32_t)ram[0xB94CC] << 24);
    if (s_ao_ext.valid && s_ao_ext.key == key) return 1;
    if (!s_ao_ext.valid && s_ao_ext.key == key) return 0;      /* уже пробовали этот набор */
    s_ao_ext.key = key; s_ao_ext.valid = 0;
    uint32_t pos = 0, n = 0x20000; int idx = 0, run = 0; uint32_t f0s[3] = {0,0,0}; int mots[3] = {0,0,0};
    while (pos + 8 <= n && idx < 64) {
        uint32_t dptr = *(uint32_t *)(ram + AP_BLOCK + pos), cnt = *(uint32_t *)(ram + AP_BLOCK + pos + 4);
        if (dptr < 0x80160000u || dptr >= 0x80160000u + n || cnt > 500) break;
        uint32_t nk = cnt + 1; if (dptr - 0x80160000u != pos + 8 + nk * 4) break;
        int nf = 0; for (uint32_t k = 0; k < nk; k++) { int fi = ram[AP_BLOCK + pos + 8 + k*4 + 1]; if (fi + 1 > nf) nf = fi + 1; }
        uint32_t data = dptr - 0x80160000u; uint32_t end = data + (uint32_t)nf * 80u; if (end > n) break;
        if (idx < 64) { s_ao_mots[idx].data = data; s_ao_mots[idx].nf = nf; }
        if (nf <= 2) { if (run < 3) { f0s[run] = data; mots[run] = idx; } run++; } else if (run < 3) run = 0;
        pos = end; idx++;
    }
    s_ao_nmot = idx;
    if (run < 3) { fprintf(stderr, "aimoff: стойки в наборе не найдены (перекарта выкл)\n"); return 0; }
    int pistol = ((ram[0xB94CC] & 0xF0) == 0x20); float e[3]; s_ao_ext.pistol = pistol;   /* до ao_elev2: мера зависит от хвата */
    for (int j = 0; j < 3; j++) { int i = (j == 0) ? 1 : (j == 1 ? 0 : 2);            /* «прямо» первой: по ней калибруется ось */
                                  int ang[45]; float W[15][9], P[15][3]; ao_unpack45(ram + AP_BLOCK + f0s[i], ang); ao_fk(ang, W, P);
                                  if (i == 1) ao_calib_u(W, P);
                                  e[i] = ao_elev2(W, P); memcpy(s_ao_ext.st_ang[i], ang, sizeof ang); }
    s_ao_ext.e_up = e[0]; s_ao_ext.e_level = e[1]; s_ao_ext.e_down = e[2]; s_ao_ext.pistol = pistol;
    memcpy(s_ao_ext.mot, mots, sizeof mots); s_ao_ext.valid = (e[0] > e[1] && e[1] > e[2]);
    fprintf(stderr, "aimoff: набор в RAM: стойки M%02d/M%02d/M%02d, ось %s: вверх %+.0f прямо %+.0f вниз %+.0f град%s\n",
            mots[0], mots[1], mots[2], s_ao_axis_hand ? "вперёд правой кисти (калибровка по стойке прямо)" : (pistol ? "плечо->кисть" : "кисть->кисть"), e[0], e[1], e[2], s_ao_ext.valid ? "" : " (порядок не вверх/прямо/вниз - перекарта выкл)");
    return s_ao_ext.valid;
}
/* ось оружия при данном байте высоты: та же смесь стоек, что делает миксер игры (прямо -> крайняя,
 * вес |b-128|/128, кратчайшая дуга по углам), прямой кинематикой */
static float ao_elev_for_byte(int b) {
    int up = (b < 128); float w = (b < 128 ? (128 - b) : (b - 128)) / 128.f; if (w > 1.f) w = 1.f;
    const int *A = s_ao_ext.st_ang[1], *B = up ? s_ao_ext.st_ang[0] : s_ao_ext.st_ang[2];
    int ang[45];
    for (int i = 0; i < 45; i++) { int d = ((B[i] - A[i] + 2048) & 0xFFF) - 2048; ang[i] = (A[i] + (int)lroundf(d * w)) & 0xFFF; }
    float W[15][9], P[15][3]; ao_fk(ang, W, P); return ao_elev2(W, P);
}
/* углы смеси стоек при байте b (та же интерполяция, что у миксера) */
static void ao_blend_stances(int b, int *out) {
    int up = (b < 128); float w = (b < 128 ? (128 - b) : (b - 128)) / 128.f; if (w > 1.f) w = 1.f;
    const int *A = s_ao_ext.st_ang[1], *B = up ? s_ao_ext.st_ang[0] : s_ao_ext.st_ang[2];
    for (int i = 0; i < 45; i++) { int d = ((B[i] - A[i] + 2048) & 0xFFF) - 2048; out[i] = (A[i] + (int)lroundf(d * w)) & 0xFFF; }
}
static float s_ao_torso_g = 0.0f, s_ao_head_g = 0.6f;   /* копии параметров для ao_offset_pose (определены ниже) */
/* три поворота доводки: наклон th вокруг линии плеч + поворот side вокруг вертикали персонажа (Y вниз →
 * ось (0,−1,0)); доли: корпус, руки (остаток), голова (сверх корпуса) */
static void ao_rot3(const float axis[3], float th, float side, float Rs[9], float Ra[9], float Rh[9]) {
    const float up[3] = { 0.f, 1.f, 0.f }; float A[9], B[9];   /* ось (0,+1,0): замер 07.09 — так + = вправо (с (0,−1,0) уходило влево) */
    ao_axis_rot(axis, th * s_ao_torso_g, A);         ao_axis_rot(up, side * s_ao_torso_g, B);         ao_mul(B, A, Rs);
    ao_axis_rot(axis, th * (1.f - s_ao_torso_g), A); ao_axis_rot(up, side * (1.f - s_ao_torso_g), B); ao_mul(B, A, Ra);
    ao_axis_rot(axis, th * s_ao_head_g, A);          ao_axis_rot(up, side * s_ao_head_g, B);          ao_mul(B, A, Rh);
}
/* доворот на th градусов вокруг линии плеч (+ side вокруг вертикали) поверх позы ang -> out (спина, таз, руки, голова) */
static void ao_offset_pose(const int *ang, float th, float side, int two_hands, int *out) {
    memcpy(out, ang, 45 * sizeof(int));
    if (fabsf(th) < 0.05f && fabsf(side) < 0.05f) return;
    float W[15][9], P[15][3]; ao_fk(ang, W, P);
    float axis[3] = { P[AO_ARM_R][0] - P[AO_ARM_L][0], P[AO_ARM_R][1] - P[AO_ARM_L][1], P[AO_ARM_R][2] - P[AO_ARM_L][2] };
    float Rs[9], Ra[9], Rh[9], Ws[9], WsT[9], tmp[9], Ln[9]; int q[3];
    ao_rot3(axis, th, side, Rs, Ra, Rh);
    ao_mul(Rs, W[AO_SPINE], Ws); ao_tr(Ws, WsT);
    int list[5] = { AO_SPINE, AO_PELVIS, AO_ARM_R, AO_ARM_L, AO_HEAD };
    for (int k = 0; k < 5; k++) {
        int b = list[k]; if (b == AO_ARM_L && !two_hands) continue;
        if ((b == AO_SPINE || b == AO_PELVIS) && !ao_bone_ours(b)) continue;   /* корень/таз — никогда (дрейф) */
        if (b == AO_SPINE) memcpy(Ln, Ws, sizeof Ln);
        else if (b == AO_PELVIS) ao_mul(WsT, W[AO_PELVIS], Ln);
        else { float Wd[9]; ao_mul(Rs, W[b], tmp); ao_mul(b == AO_HEAD ? Rh : Ra, tmp, Wd); ao_mul(WsT, Wd, Ln); }
        ao_decompose(Ln, q); out[b*3] = q[0]; out[b*3+1] = q[1]; out[b*3+2] = q[2];
    }
}
static float s_ao_slew;   /* определена ниже */
/* (06.09 вечер, игрок: «сейчас ~90°, а было 60–70 — верни прежний угол») s_ao_gain = 0.62: цель = высота
 * перекрестия × 0.62 — и для перекарты байта, и для остатка. Прежняя картинка (до того, как доворот стал
 * доходить до рендера) = смесь стоек по мере плечо→кисть, которая круче ствола на ~13°: при 34° камеры
 * ствол был на ~21°, при 57° — 40 (стойка «вверх»), при −57° — −39; при 72° (предел камеры) — та же
 * стойка 40. Коэффициент 0.62 повторяет это с точностью до 2–5° по всему диапазону (72° → 45).
 * DC_AIMOFF_GAIN=1 — ствол ровно на перекрестии (игроку показалось слишком высоко). */
/* (06.09 ~18:00) ОТКАТ К ПРЕЖНЕМУ ВИДУ (игрок: гранатомёт/дробовик «сгибается неестественно при крайних
 * углах — откати эти позы тоже, как с пистолетом»; пистолет «раньше прям хорошо направлен был»):
 * остаток сверх стоек ВЫКЛЮЧЕН (s_ao_max = 0: доворот рук/корпуса/головы не применяется; включить —
 * DC_AIMOFF_MAX=60), gain = 1, мера оси прежняя. Остаются: перекарта байта по стойкам из RAM, морф
 * вскидывания в смесь стоек и кроссфейд стыка (переходы игрок подтвердил), хуки на отрисовке. */
static int s_ao_on = -1; static float s_ao_max = 0.f, s_ao_torso = 0.0f, s_ao_head = 0.6f, s_ao_gain = 1.f; static int s_ao_remap = 1;   /* torso 0 (07.09): спина/таз не трогаются — дрейф */
static float s_ao_force = -999.f;   /* DC_AIMOFF_FORCE=<град>: отладка — доворот фиксирован, чтобы видеть, доходит ли он до рендера */
/* (06.09 ~18:00, игрок: «гранатомёт и дробовик немного доведи, чтобы было похоже, что целится вперёд, куда
 * прицел смотрит») ПОСТОЯННАЯ ПОПРАВКА для двуручных стволов: в их стойках ствол смотрит чуть ниже линии
 * взгляда. Поправка = тот же жёсткий поворот обеих рук вокруг линии плеч (кисти на оружии), не зависит от
 * перекрестия, добавляется во всех фазах прицела (при опускании затухает). DC_AIMOFF_BIAS_SG (0x10 дробовик),
 * DC_AIMOFF_BIAS_GL (0x30 гранатомёт), градусы, + = выше; debug aimoff_bias {sg,gl} — живо. */
/* Подбор 06.09 по скринам (tools/aimoff_bias_gl0: ствол гранатомёта проходит через перекрестие при +10;
 * дробовик в OTS-виде почти не виден — линия кисть→кисть у него вдоль ствола, поправка мягкая +6,
 * tools/aimoff_bias_sg_down). */
/* (07.09, игрок: «в дебаг-окне параметры: оффсет оружия по forward и side оси аим-позы, вкл/выкл доводки
 * на конкретном оружии») ПАРАМЕТРЫ ПО ОРУЖИЮ: индекс 0 пистолет (0x20), 1 дробовик (0x10), 2 гранатомёт
 * (0x30). on — вся наша доводка позы на этом оружии (морф вскидывания, кроссфейд стыка, поправки);
 * fwd — наклон вперёд, град, + = ствол выше (поворот рук вокруг линии плеч); side — поворот вбок, град,
 * + = вправо (поворот рук вокруг вертикали). Меню (вкладка WEAPON AIM, dc_user.cfg aim.*), debug aimoff_w,
 * env DC_AIMOFF_BIAS_SG/GL — только пока меню не задало своё. */
/* ДЕФОЛТЫ = ПОДГОНКА ИГРОКА (F8, 07.09 ~10:00, dc_user.cfg): пистолет вкл 4.0/1.1, дробовик ВЫКЛ 6.0/0.0,
 * гранатомёт вкл 8.9/-1.9; режим clamp у всех. */
/* ⭐ 30.09.2026 (игрок, слот 0: «надо в дебаге так же добавить параметры для пистолета в двойном хвате; и подогнать его аим —
 * сейчас быстрее вверх поднимается, чем нужно, и недостаточно вниз опускается»). Индекс 3 — ПИСТОЛЕТ ДВУМЯ РУКАМИ: байт оружия
 * 0x22/0x23 (набор P22/P23, костюмы 2–3; 0x20/0x21 — одной рукой, P20/P21). До этого у него были параметры пистолета одной
 * руки. DC_AIM_PISTOL2=0 — как было (общие с пистолетом). */
#define AO_NW 4
static int s_ao_w_on[AO_NW] = { 1, 0, 1, 1 }; static float s_ao_w_fwd[AO_NW] = { 4.0f, 6.0f, 8.9f, 4.0f }, s_ao_w_side[AO_NW] = { 1.1f, 0.0f, -1.9f, 1.1f }; static int s_ao_w_menu[AO_NW] = { 0, 0, 0, 0 };
/* (07.09, игрок: «сделай, чтобы можно было заскейлить clamp: к крайним позам оружие поднимается раньше
 * перекрестия») усиление по оружию: цель = перекрестие × gain (в clamp — до упора в стойки; в scale —
 * доля хода камеры × gain). gain < 1 — рука доходит до крайней стойки позже, синхронно с перекрестием. */
static float s_ao_w_gain[AO_NW] = { 0.98f, 1.0f, 0.80f, 0.80f };   /* подгонка игрока 07.09 ~10:40 (dc_user.cfg): пистолет 0.98, дробовик 1.00, гранатомёт 0.80;
                                                                     * 01.10: двумя руками 0.80 (игрок: «вверх поднимается быстрее, чем нужно») */
/* 30.09: усиление ВНИЗ отдельно от вверх (gain — вверх, gain_dn — вниз; у прежних стволов по умолчанию одинаковые) и запас
 * ВНИЗ сверх нижней стойки (градусы доворота рук вокруг линии плеч, когда перекрестие ниже стойки «вниз»; 0 — упор в стойку,
 * как было: «докрутка сверх стоек» у остальных стволов отвергнута 06.09) */
static float s_ao_w_gain_dn[AO_NW] = { 0.98f, 1.0f, 0.80f, 0.98f };
static float s_ao_w_over_dn[AO_NW] = { 0.f, 0.f, 0.f, 18.f };   /* 01.10: двумя руками ниже стойки −39 ещё до 18° (игрок: «недостаточно вниз») */
/* (07.09) режим диапазона по оружию: 0 = clamp — перекрестие 1:1, у пределов стоек рука упирается;
 * 1 = scale — весь ход камеры (пределы DC_CAM_PITCH_UP/DOWN) растягивается на диапазон стоек, рука доходит
 * до крайней стойки ровно у предела камеры */
static int s_ao_w_range[AO_NW] = { 0, 0, 0, 0 };
static void ao_cam_limits(float *up_rad, float *dn_rad) {
    static float up = -1.f, dn = -1.f;
    if (up < 0.f) { const char *e = getenv("DC_CAM_PITCH_UP"); up = e ? (float)atof(e) : 1.26f; e = getenv("DC_CAM_PITCH_DOWN"); dn = e ? (float)atof(e) : 1.31f;
                    if (up < 0.1f) up = 1.26f; if (dn < 0.1f) dn = 1.31f; }
    *up_rad = up; *dn_rad = dn;
}
static int ao_pistol2_env(void) { static int v = -1; if (v < 0) { const char *e = getenv("DC_AIM_PISTOL2"); v = (e && *e == '0') ? 0 : 1; } return v; }
int dc_aimoff_widx_of(int wb) {                  /* 20.09: без битов модификаций; 30.09: 0x22/0x23 — пистолет двумя руками (3) */
    int w = wb & 0xF0;
    if (w == 0x10) return 1;
    if (w == 0x30) return 2;
    if (w == 0x20 && (wb & 0x0F) >= 2 && ao_pistol2_env()) return 3;
    return 0;
}
static int ao_widx(const uint8_t *ram) { return dc_aimoff_widx_of(ram[0xB94CC]); }
static float ao_bias(const uint8_t *ram) { return s_ao_w_fwd[ao_widx(ram)]; }
static float ao_side(const uint8_t *ram) { return s_ao_w_side[ao_widx(ram)]; }
/* what: 0 on, 1 fwd, 2 side, 3 range, 4 gain (вверх), 5 gain вниз, 6 запас вниз сверх стойки (град) */
float dc_aimoff_weapon_get(int w, int what) {
    if (w < 0 || w >= AO_NW) return 0.f;
    switch (what) { case 0: return (float)s_ao_w_on[w]; case 1: return s_ao_w_fwd[w]; case 2: return s_ao_w_side[w];
                    case 3: return (float)s_ao_w_range[w]; case 4: return s_ao_w_gain[w]; case 5: return s_ao_w_gain_dn[w];
                    case 6: return s_ao_w_over_dn[w]; default: return 0.f; }
}
void dc_aimoff_weapon_set(int w, int what, float v) {
    if (w < 0 || w >= AO_NW) return;
    if (what == 0) s_ao_w_on[w] = (v >= 0.5f); else if (what == 1) s_ao_w_fwd[w] = v; else if (what == 2) s_ao_w_side[w] = v; else if (what == 3) s_ao_w_range[w] = (v >= 0.5f);
    else if (what == 4 || what == 5) { if (v < 0.1f) v = 0.1f; if (v > 3.f) v = 3.f; if (what == 4) s_ao_w_gain[w] = v; else s_ao_w_gain_dn[w] = v; }
    else if (what == 6) { if (v < 0.f) v = 0.f; if (v > 45.f) v = 45.f; s_ao_w_over_dn[w] = v; }
    else return;
    s_ao_w_menu[w] |= (1 << what);
}
void dc_aimoff_set_bias(float sg, float gl) { if (sg > -900.f) s_ao_w_fwd[1] = sg; if (gl > -900.f) s_ao_w_fwd[2] = gl; }
static void ao_cfg(void) {
    if (s_ao_on >= 0) return;
    const char *e = getenv("DC_AIMOFF"); s_ao_on = (e && *e == '0') ? 0 : 1;
    e = getenv("DC_AIMOFF_MAX"); if (e && *e) s_ao_max = (float)atof(e);
    e = getenv("DC_AIMOFF_SLEW"); if (e && *e) s_ao_slew = (float)atof(e);
    e = getenv("DC_AIMOFF_TORSO"); if (e && *e) s_ao_torso = (float)atof(e);
    e = getenv("DC_AIMOFF_HEAD"); if (e && *e) s_ao_head = (float)atof(e);
    e = getenv("DC_AIMOFF_GAIN"); if (e && *e) s_ao_gain = (float)atof(e);
    e = getenv("DC_AIM_REMAP"); if (e && *e == '0') s_ao_remap = 0;
    e = getenv("DC_AIMOFF_FORCE"); if (e && *e) s_ao_force = (float)atof(e);
    e = getenv("DC_AIMOFF_AXIS"); if (e && (*e == 'h' || *e == 'H')) s_ao_axis_hand = 1;
    e = getenv("DC_AIMOFF_BIAS_SG"); if (e && *e && !(s_ao_w_menu[1] & 2)) s_ao_w_fwd[1] = (float)atof(e);
    e = getenv("DC_AIMOFF_BIAS_GL"); if (e && *e && !(s_ao_w_menu[2] & 2)) s_ao_w_fwd[2] = (float)atof(e);
    s_ao_torso_g = s_ao_torso; s_ao_head_g = s_ao_head;
    fprintf(stderr, "aimoff: %s (max %.0f град, torso %.2f, head %.2f, gain %.2f), перекарта байта высоты %s\n",
            s_ao_on ? "вкл" : "выкл", s_ao_max, s_ao_torso, s_ao_head, s_ao_gain, s_ao_remap ? "вкл" : "выкл");
}
/* (1) байт высоты прицела по высоте перекрестия. pitch_down_rad - наклон камеры (>0 вниз) минус
 * уровень; legacy_gain - старый множитель для отката. Возвращает 0..255. */
float dc_aimoff_blend(float pitch_down_rad, float legacy_gain) {
    ao_cfg();
    uint8_t *ram = memory_get_ram_ptr();
    float bv;
    if (!s_ao_remap || !ram || !ao_extremes(ram)) { bv = 128.f + pitch_down_rad * legacy_gain; }
    else {
        const int wi = ao_widx(ram);
        const float gw = (pitch_down_rad > 0.f) ? s_ao_w_gain_dn[wi] : s_ao_w_gain[wi];   /* 30.09: вниз — своё усиление */
        float target = -pitch_down_rad * 180.f / 3.14159265f * s_ao_gain * gw;   /* высота перекрестия, вверх +, × gain оружия */
        float w;
        if (s_ao_w_range[wi]) {
            /* SCALE: доля хода камеры до её предела = доля хода до крайней стойки */
            float up, dn; ao_cam_limits(&up, &dn);
            float g = gw;
            if (pitch_down_rad <= 0.f) { w = -pitch_down_rad / up * g; if (w > 1.f) w = 1.f; bv = 128.f - 128.f * w; }
            else { w = pitch_down_rad / dn * g; if (w > 1.f) w = 1.f; bv = 128.f + 128.f * w; }
        }
        else if (target >= s_ao_ext.e_level) { w = (target - s_ao_ext.e_level) / (s_ao_ext.e_up - s_ao_ext.e_level); if (w > 1.f) w = 1.f; bv = 128.f - 128.f * w; }
        else { w = (s_ao_ext.e_level - target) / (s_ao_ext.e_level - s_ao_ext.e_down); if (w > 1.f) w = 1.f; bv = 128.f + 128.f * w; }
    }
    if (bv < 0.f) bv = 0.f; if (bv > 255.f) bv = 255.f;
    return bv;
}
/* (2) доворот на сборке скелета */
typedef struct { int active; int saved[5][3]; int bones[5]; int n; float cur, target, elev, theta; uint32_t frame; long applied, seen; int reason; uint32_t last_actor; int two, phase, had_stance; float e_start, prog; int prev_phase, xfade, prev_valid; int prev_out[45]; int saved_all[45]; int saved_all_valid; int morph[45]; int morph_valid; int slew_hold; float side_cur; int last_k; } AoState;
static AoState s_ao, s_ao_snap;                                  /* снимок состояния на первом вызове кадра хоста (см. pt_anim_aimoff_enter) */
static uint32_t s_ao_key_frame = 0xFFFFFFFFu, s_ao_ring_n_snap = 0; static int s_ao_calls_in_frame = 0;
static float s_ao_target_deg = 0.f; static int s_ao_want = 0;
static int s_ao_byte = -1; static uint32_t s_ao_byte_frame = 0;   /* байт высоты прицела от слоя прицела (−1 = не навязывать) */
static long s_ao_byte_calls = 0;
void dc_aimoff_set_byte(int b) { extern uint64_t s_frame_count; s_ao_byte = b; s_ao_byte_frame = (uint32_t)s_frame_count; s_ao_byte_calls++; }
#define AO_RING 512
static struct { uint32_t frame; int phase, two, sub; float elev, theta, eref; int bnow, bours, bram, age; int xf; float mstep; int mbone; int root[6]; int calls; } s_ao_ring[AO_RING]; static uint32_t s_ao_ring_n = 0;
int dc_aimoff_ring(int idx_from_newest, uint32_t *frame, int *phase, int *two, int *sub, float *elev, float *theta) {
    if ((uint32_t)idx_from_newest >= s_ao_ring_n || idx_from_newest >= AO_RING) return 0;
    uint32_t i = (s_ao_ring_n - 1 - (uint32_t)idx_from_newest) % AO_RING;
    *frame = s_ao_ring[i].frame; *phase = s_ao_ring[i].phase; *two = s_ao_ring[i].two; *sub = s_ao_ring[i].sub; *elev = s_ao_ring[i].elev; *theta = s_ao_ring[i].theta; return 1;
}
int dc_aimoff_ring_root(int idx_from_newest, int *root6) {
    if ((uint32_t)idx_from_newest >= s_ao_ring_n || idx_from_newest >= AO_RING) return 0;
    uint32_t i = (s_ao_ring_n - 1 - (uint32_t)idx_from_newest) % AO_RING; memcpy(root6, s_ao_ring[i].root, 6 * sizeof(int)); return 1;
}
int dc_aimoff_ring_bytes(int idx_from_newest, float *eref, int *bnow, int *bours, int *bram, int *age) {
    if ((uint32_t)idx_from_newest >= s_ao_ring_n || idx_from_newest >= AO_RING) return 0;
    uint32_t i = (s_ao_ring_n - 1 - (uint32_t)idx_from_newest) % AO_RING;
    *eref = s_ao_ring[i].eref; *bnow = s_ao_ring[i].bnow; *bours = (int)lroundf(s_ao_ring[i].mstep * 10.f); *bram = s_ao_ring[i].mbone; *age = s_ao_ring[i].xf * 1000 + s_ao_ring[i].age; return s_ao_ring[i].calls > 0 ? s_ao_ring[i].calls : 1;   /* bours = шаг*10, bram = кость шага, age = xf*1000 + возраст */
}
void dc_aimoff_set_target(float pitch_down_rad, int want) {     /* из слоя прицела каждый кадр хоста */
    ao_cfg();
    s_ao_target_deg = -pitch_down_rad * 180.f / 3.14159265f * s_ao_gain; s_ao_want = want;
}
#define AO_XFADE 6
/* 30.09: остаток «цель − ось» (доворот рук вокруг линии плеч): по умолчанию упор ±s_ao_max (0 — выкл, 06.09); у оружия с запасом
 * вниз (s_ao_w_over_dn) перекрестие ниже нижней стойки доводит руки ещё на столько градусов (цель — с усилением вниз) */
static float ao_resid(const uint8_t *ram, float ref) {
    const int w = ao_widx(ram);
    float th = s_ao_target_deg - ref, lo = -s_ao_max, hi = s_ao_max;
    const float od = s_ao_w_over_dn[w];
    if (od > 0.f && s_ao_target_deg < 0.f) {
        const float t = s_ao_target_deg * s_ao_w_gain_dn[w];
        if (t < s_ao_ext.e_down) { th = t - ref; if (-od < lo) lo = -od; }
    }
    if (th > hi) th = hi; if (th < lo) th = lo;
    return th;
}
static float ao_resid_lo(const uint8_t *ram) {            /* нижний упор общего ограничения для фаз 2..4 */
    const float od = s_ao_w_over_dn[ao_widx(ram)];
    return (od > s_ao_max && s_ao_target_deg < 0.f) ? -od : -s_ao_max;
}
/* (07.09, баг игрока: быстрые нажатия прицела сдвигали Регину на ~170 ед. за нажатие — tools/aim_tap_test.py:
 * с доводкой −405/−543 за 5 нажатий, без неё 0) корень (0), таз (8) и ноги (9..14) НЕ ТРОГАЕМ: их углы игра
 * использует для движения; наши записи туда во время вскидывания/опускания давали дрейф. Всё наше — только
 * верх тела: руки 1..6 и голова 7. */
#define AO_UPPER_ONLY 1
static int ao_bone_ours(int b) { return b >= 1 && b <= 7; }
static float s_ao_slew = 0.f;    /* макс. шаг угла за тик на вскидывании/стыке, град (DC_AIMOFF_SLEW; 0 = выкл, по умолчанию выкл) */
/* ОГРАНИЧИТЕЛЬ СКОРОСТИ выхода (игрок 06.09, кадры 52→53: «переключение в один кадр»). У авторских
 * анимаций вскидывания последний кадр делает шаг локтя 38–39° за тик (P20 M01 к.8, M08 к.6 — данные
 * диска), с близкой камерой на большом угле это читается как переключение. Здесь любой угол любой
 * кости не может уйти от прошлого выхода больше чем на s_ao_slew за тик; поза догоняет за 2–3 тика. */
static void ao_slew(uint8_t *ram) {
    if (!s_ao.prev_valid || s_ao_slew <= 0.f) return;
    int lim = (int)lroundf(s_ao_slew * 4096.f / 360.f);
    for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
        if (!ao_bone_ours(b)) continue;
        uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
        int v = *pp & 0xFFF, pv = s_ao.prev_out[b*3+c];
        int d = ((v - pv + 2048) & 0xFFF) - 2048;
        if (d > lim) d = lim; if (d < -lim) d = -lim;
        *pp = (uint16_t)((*pp & ~0xFFF) | ((pv + d) & 0xFFF));
    }
}
static float s_ao_mstep = 0.f;   /* макс. шаг угла (град) между выходами двух тиков */
static void ao_note_out(uint8_t *ram) {          /* запомнить, что увидит сборка скелета в этот тик */
    int mx = 0, mb = -1;
    for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
        int v = *(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c) & 0xFFF;
        if (s_ao.prev_valid) { int d = ((v - s_ao.prev_out[b*3+c] + 2048) & 0xFFF) - 2048; if (d < 0) d = -d; if (d > mx) { mx = d; mb = b; } }
        s_ao.prev_out[b*3+c] = v;
    }
    s_ao_mstep = s_ao.prev_valid ? mx * 360.f / 4096.f : 0.f;
    s_ao.prev_valid = 1;
    { uint32_t i = (s_ao_ring_n + AO_RING - 1) % AO_RING; s_ao_ring[i].mstep = s_ao_mstep; s_ao_ring[i].mbone = mb; }   /* в запись этого тика */
}
static void ao_enter(CPUState *cpu, int gate_actor);
/* ДВА ХУКА (06.09 вечер, тест DC_AIMOFF_FORCE=60: позиции узлов в RAM ушли на 60°, а картинка — нет):
 * рисует игра НЕ по матрицам func_80066260. Порядок тика (wtrace по матрице узла): func_80072C84 —
 * RotMatrix углов ВСЕХ актёров (node+40 -> node+0, локальные) -> func_80073F10 сцепляет -> отрисовка ->
 * и только потом func_80066260 ×4 (логика). Поэтому доворот ставится на вход func_80072C84 (без гейта
 * по a0 — там нет актёра, а узлы Регины по фиксированному адресу) и, для RAM-скелета/логики, на
 * func_80066260. Оба вызова в одном кадре хоста дают одну и ту же позу (снимок состояния). */
/* ОТЛОЖЕННЫЙ ВЗРЫВ ГРАНАТЫ (06.09): игра не считает потолок поверхностью — снаряд летит сквозь него.
 * Когда наш снаряд прошёл точку прицела (pt_renderer.c dc_grenade_tick), здесь, уже ВНУТРИ тика игры
 * (вход func_80072C84 — все актёры, валидный CPUState), вызывается её же func_80046330(obj, 1, 1):
 * запрос эффекта взрыва {11,12,0x8009C6E0} в позиции объекта + звук 23; затем состояние +60 = 3 (мёртв),
 * как делает сам обработчик полёта. Регистры сохраняются/восстанавливаются целиком. */
static struct { int pending; uint32_t obj; int16_t pos[3]; } s_gl_call;
void dc_grenade_defer_explode(uint32_t obj, const int16_t pos[3]) { s_gl_call.obj = obj & 0x1FFFFFu; s_gl_call.pos[0] = pos[0]; s_gl_call.pos[1] = pos[1]; s_gl_call.pos[2] = pos[2]; s_gl_call.pending = 1; }
extern void func_80046330(CPUState *cpu);
static void gl_run_deferred(CPUState *cpu) {
    { extern int g_savestate_loads; static int ld = 0;   /* 18.10: отложенный взрыв прежнего мира после быстрой загрузки - */
      if (ld != g_savestate_loads) { ld = g_savestate_loads; s_gl_call.pending = 0; } }   /* не трогать гранату сейва */
    if (!s_gl_call.pending) return;
    s_gl_call.pending = 0;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    uint32_t o = s_gl_call.obj;
    /* 25.09: и в баллистическом полёте (состояние 2 — без цели игра пускает гранату сразу так); DC_GRENADE_ST2=0 — как было */
    static int st2 = -1;
    if (st2 < 0) { const char *e2 = getenv("DC_GRENADE_ST2"); st2 = (e2 && *e2 == '0') ? 0 : 1; }
    if (ram[o + 39] != 12 || ram[o + 60] > (st2 ? 2 : 1)) { fprintf(stderr, "grenade: deferred explode skipped (cls %d st %d)\n", ram[o + 39], ram[o + 60]); return; }
    for (int i = 0; i < 3; i++) { ram[o + 32 + 2*i] = (uint8_t)s_gl_call.pos[i]; ram[o + 33 + 2*i] = (uint8_t)(s_gl_call.pos[i] >> 8); }
    uint32_t save[32]; memcpy(save, cpu->gpr, sizeof save); uint32_t hi = cpu->hi, lo = cpu->lo;
    cpu->gpr[4] = 0x80000000u | o; cpu->gpr[5] = 1; cpu->gpr[6] = 1; cpu->gpr[31] = 0x80072C84u;
    func_80046330(cpu);
    memcpy(cpu->gpr, save, sizeof save); cpu->hi = hi; cpu->lo = lo;
    ram[o + 60] = 3;
    { static long n = 0; if (++n <= 40) fprintf(stderr, "grenade: deferred explode at %d,%d,%d done\n", s_gl_call.pos[0], s_gl_call.pos[1], s_gl_call.pos[2]); }
}
/* (07.09) последняя распаковка кадра оружейного блока: индекс моушена (по таблице набора) и кадр */
static int ao_last_motion(int *m_out, int *k_out) {
    uint32_t src = 0;
    for (int q = 1; q <= 8; q++) { AnimRingEnt *e = &s_aring[(s_aring_head + ANIM_RING - q) % ANIM_RING];
        uint32_t sp2 = e->src & 0x1FFFFFu; if (sp2 >= AP_BLOCK && sp2 < AP_BLOCK + 0x20000) { src = sp2; break; } }
    if (src < AP_BLOCK + 12) return 0;
    uint32_t off = src - AP_BLOCK - 12;
    for (int m = 0; m < s_ao_nmot; m++) if (off >= s_ao_mots[m].data && off < s_ao_mots[m].data + (uint32_t)s_ao_mots[m].nf * 80u) {
        *m_out = m; *k_out = (int)((off - s_ao_mots[m].data) / 80u); return 1; }
    return 0;
}
/* моушен m — выстрел резидентного набора (kind 1 в assets/aim_patch.bin; без патча выстрел не распознаётся) */
static int ao_motion_is_shot(int m) {
    if (s_ap_cur < 0 || s_ap_n <= 0 || s_ap_cur >= s_ap_n || m < 0 || m >= s_ao_nmot) return 0;
    for (int i = 0; i < s_ap[s_ap_cur].nmot; i++) { ApMotion *M = &s_ap[s_ap_cur].mot[i];
        if (M->kind == 1 && s_ao_mots[m].data >= M->off && s_ao_mots[m].data < M->off + (uint32_t)M->nframes * 80u) return 1; }
    return 0;
}
/* 11.10.2026 ГОЛОВА ПОСЛЕ ВЗАИМОДЕЙСТВИЯ (игрок, слот 0: «взаимодействуешь, жмёшь cancel - и Регина при беге мотает головой;
 * с текстами то же»). Контроллер головы (актёр +356, режимы команды 0x3D: 7 взгляд на точку, 9 на актёра, 8 авто-взгляд) ставит
 * части 7 (голова) байт +67 = 1 - «голову ведёт контроллер», и func_80072C84 строит её матрицу другим порядком поворотов
 * (func_8008F984 вместо func_8008F6F4). Взгляд при взаимодействии ставит 1, авто-взгляд (режим 8) после текста так и
 * оставляет, хотя углы головы пишет уже анимация бега (у контроллера крен +44 всегда 0): наклон и поворот бега в чужом порядке
 * дают лишний крен и поворот в такт шагам (замер, слот 45: крен головы 5.2° против 1.1°, поворот 4.7° против 2.6°). Здесь, на
 * входе func_80072C84: режим 8, +67 = 1, крен не 0 (углы - анимации) -> +67 = 0, голову строит обычный порядок; когда
 * контроллер правда ведёт голову (крен 0) - не трогаем. В «оригинальном» виде игры - как у игры. DC_HEAD_FREE=0 - как было. */
static void head_free_tick(uint8_t *ram) {
    static int en = -1; static long n = 0;
    if (en < 0) { const char *e = getenv("DC_HEAD_FREE"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !ram) return;
    extern int dc_orig_pure(void);
    if (dc_orig_pure()) return;
    const uint32_t P = 0xB928Cu, N7 = AP_NODES + AP_NODE_STRIDE * 7u;
    if (ram[P + 356] != 8 || ram[N7 + 67] != 1) return;
    if ((*(uint16_t *)(ram + N7 + AP_NODE_ANG + 4) & 0xFFF) == 0) return;
    ram[N7 + 67] = 0;
    if (n < 20) { n++; fprintf(stderr, "headfree: Regina's head stayed on the head controller (auto look) while the animation drives it - "
                                       "built in the animation's order again (DC_HEAD_FREE=0 off)%c", 10); }
}
void pt_anim_aimoff_draw_enter(CPUState *cpu) { gl_run_deferred(cpu); head_free_tick(memory_get_ram_ptr()); ao_enter(cpu, 0); }
void pt_anim_aimoff_enter(CPUState *cpu) { ao_enter(cpu, 1); }
/* 12.10.2026 поздно, РЕЖИМ 3 заново (игрок: «то же самое, что в режиме 0; только когда жмёшь направление и она
 * поворачивается, уже в повороте принимает позу приготовления к стрельбе - верх тела как на ходу. Сейчас слайдится назад при
 * смене направления, рывки, позы переключаются в один кадр»). Прежняя вставка выходила из ao_enter до снимка углов: выход
 * возвращал в скелет давний снимок всех 15 костей (ноги и корень) - игра двигала её до 400 ед. за такт (на одном вводе: режим 3
 * - скольжения 4.4 и 12.5 м, режим 0 - ни одного), позы скакали. Теперь отдельный проход ПОСЛЕ доводки прицела:
 *  - цель - руки и голова (кости 1..7; корень, таз и ноги - никогда) ПОХОДКИ С ОРУЖИЕМ самой игры для этого оружия: её моушен
 *    в наборе оружия узнаётся по кольцу распаковок, пока она идёт в прицеле (3 такта подряд один моушен), до того - «на 4
 *    раньше стоек» (у пистолета M11 при стойках M15-17); поза - среднее по циклу (размах рук в цикле до 9 град);
 *  - выход костей 1..7 ведёт пружина с критическим затуханием (разгон и торможение плавные, DC_AIM3_OMEGA 24 - 90 % за 0.16 с)
 *    к цели: направление нажато и она стоит или доворачивается - поза походки; уже идёт и игра в походке - сама походка игры;
 *    отпущено - то, что дали игра и доводка прицела; выстрел - та же пружина, втрое быстрее. Сошлось с игрой - отпускаем. */
static struct { int on; float x[21], v[21]; uint32_t frame; uint32_t key; int walk_m, learned, seen_m, seen_n, pose_m; float pose[21];
                int sh_on, sh_ref_ok; int sh_ref[21]; float sh_d[21]; } s_m3 = { 0 };   /* sh_*: ход выстрела мимо пружины (13.10) */
int dc_aim_walk_mode(void);                              /* main.cpp: режим ходьбы в прицеле (F7) */
int dc_aim_walk_raw(void);                               /* main.cpp: номер режима с F7 (6 - свой, в прицеле как 3) */
float dc_strafe_live_f(const char *key, float def);      /* ниже: живые настройки dc_strafe_live.cfg */
static int m3_wrap(int d) { return ((d + 2048) & 4095) - 2048; }
static int m3_pose(uint8_t *ram) {                       /* поза рук походки текущего набора; 0 - нет */
    if (!ao_extremes(ram)) return 0;
    if (s_m3.key != s_ao_ext.key) { s_m3.key = s_ao_ext.key; s_m3.learned = 0; s_m3.walk_m = -1; s_m3.pose_m = -1; s_m3.seen_n = 0; }
    int m = s_m3.learned ? s_m3.walk_m : s_ao_ext.mot[0] - 4;
    if (m < 0 || m >= s_ao_nmot || s_ao_mots[m].nf < 16 || s_ao_mots[m].nf > 64) return 0;
    if (s_m3.pose_m != m) {
        float cs[21], sn[21]; memset(cs, 0, sizeof cs); memset(sn, 0, sizeof sn);
        for (int k = 0; k < s_ao_mots[m].nf; k++) { int ang[45]; ao_unpack45(ram + AP_BLOCK + s_ao_mots[m].data + (uint32_t)k * 80u, ang);
            for (int i = 0; i < 21; i++) { const float a = (float)ang[3 + i] * AO_ANG; cs[i] += cosf(a); sn[i] += sinf(a); } }
        for (int i = 0; i < 21; i++) { float q = atan2f(sn[i], cs[i]) / AO_ANG; if (q < 0.f) q += 4096.f; s_m3.pose[i] = q; }
        s_m3.pose_m = m;
        fprintf(stderr, "aimwalk: mode 3 - walk stance of the arms = motion M%02d of the weapon set (%s, %d frames)%c", m,
                s_m3.learned ? "seen while she walked" : "4 before the stances", s_ao_mots[m].nf, 10);
    }
    return 1;
}
static void m3_fold(void) {                              /* ход выстрела - в состояние пружины: выход не прыгает */
    if (s_m3.sh_on && s_m3.on) for (int i = 0; i < 21; i++) s_m3.x[i] += s_m3.sh_d[i];
    s_m3.sh_on = 0; memset(s_m3.sh_d, 0, sizeof s_m3.sh_d);
}
static void ao_m3_post(void) {
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    extern int dc_orig_pure(void); extern int dc_aim_walk_pose_now(void); extern uint64_t s_frame_count;
    extern int g_dc_fire_btn;
    if (dc_orig_pure()) { s_m3.on = 0; s_m3.sh_on = 0; s_m3.sh_ref_ok = 0; return; }
    const uint32_t fr = (uint32_t)s_frame_count;
    const int act = ram[0xB92C9], sub = ram[0xB92CA];
    int live[21];
    for (int i = 0; i < 21; i++) live[i] = *(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)(1 + i / 3) + AP_NODE_ANG + 2u * (uint32_t)(i % 3)) & 0xFFF;
    /* 13.10.2026 день, МУСОРНЫЙ КАДР ВЫСТРЕЛА (игрок: «прицеливаешься, жмёшь направление, хоть назад, и стреляешь - Регина в этот
     * момент немного повёрнута вправо, видно по руке и пистолету»). Замер нарисованной позы против позы игры (DC_DRAWPOSE_LOG и
     * node_ring): во 2-м такте выстрела анимация игры разом поворачивает корпус (узел 0, пружина его не ведёт) вправо на 26° и назад
     * на 12° и в том же такте доворачивает обратно руку (Rz +27°) и голову (-27°) - ствол и взгляд стоят на цели; в 3-м сгибает
     * локоть на 57°. Руки и голову после походки ведёт эта пружина, а цель она берёт раз за кадр, в первом вызове - ДО распаковки
     * нового такта: доворот руки и головы опаздывал на кадр-два - рука с пистолетом и голова уходили вправо вместе с корпусом, на
     * следующем такте рука вытянута (локоть ещё не согнут). В режиме 0 позы походки нет - пружина не включается, рука сразу как у
     * игры. Теперь ход анимации выстрела (подсостояние 2, многокадровый моушен набора оружия) от позы её кадра 0 идёт мимо пружины,
     * в каждом вызове: выход = пружина + ход, пружина ведёт к позе без хода (к спокойному прицелу). Кончился выстрел или начался
     * новый - ход переходит в состояние пружины, выход не прыгает. Отдача - как у игры. */
    { int mm = -1, kk = 0;
      const int sh = act == 6 && sub == 2 && ao_last_motion(&mm, &kk) && mm >= 0 && mm < s_ao_nmot && s_ao_mots[mm].nf > 2;
      if (sh && kk == 0) { m3_fold(); memcpy(s_m3.sh_ref, live, sizeof s_m3.sh_ref); s_m3.sh_ref_ok = 1; }   /* кадр 0 - поза отсчёта */
      else if (sh && s_m3.sh_ref_ok) { s_m3.sh_on = 1; for (int i = 0; i < 21; i++) s_m3.sh_d[i] = (float)m3_wrap(live[i] - s_m3.sh_ref[i]); }
      else if (!sh) { m3_fold(); s_m3.sh_ref_ok = 0; } }
    if (fr != s_m3.frame) {                              /* раз за кадр: цель и шаг пружины */
        const uint32_t df = s_m3.frame ? fr - s_m3.frame : 1u; s_m3.frame = fr;
        int mm = -1, kk = 0; const int got = ao_last_motion(&mm, &kk); const int shot = got && ao_motion_is_shot(mm);
        int have = 0;
        if (act == 6) {
            have = m3_pose(ram);
            if (sub == 5 && got && !shot && mm >= 0 && mm < s_ao_nmot && s_ao_mots[mm].nf >= 16 && s_ao_mots[mm].nf <= 64) {
                if (mm == s_m3.seen_m) s_m3.seen_n++; else { s_m3.seen_m = mm; s_m3.seen_n = 1; }
                if (s_m3.seen_n >= 3 && (!s_m3.learned || s_m3.walk_m != mm)) { s_m3.learned = 1; s_m3.walk_m = mm; have = m3_pose(ram); }
            }
        }
        const int want = have && dc_aim_walk_pose_now() && act == 6 && (sub == 1 || sub == 5 || sub == 6 || sub == 7) && !shot;
        { static int lg = -1; if (lg < 0) { const char *e = getenv("DC_AIMWALK_LOG"); lg = (e && *e == '1') ? 1 : 0; }
          if (lg && act == 6) fprintf(stderr, "m3: f=%u sub=%d want=%d have=%d posenow=%d shot=%d last=M%02d k%d on=%d%c", fr, sub, want, have,
                                      dc_aim_walk_pose_now(), shot, mm, kk, s_m3.on, 10); }
        float T[21];
        int gait_in = 0;
        if (want && (sub == 5 || sub == 6)) {            /* походка игры уже целиком в позе - её и держим (свой размах рук) */
            gait_in = 1;
            for (int i = 0; i < 21; i++) { int d = m3_wrap(live[i] - (int)lroundf(s_m3.pose[i])); if (d < 0) d = -d; if (d > 285) { gait_in = 0; break; } }
        }
        for (int i = 0; i < 21; i++) {
            if (want && !gait_in) { float d = s_m3.pose[i] - (float)live[i]; while (d > 2048.f) d -= 4096.f; while (d < -2048.f) d += 4096.f;
                                    T[i] = (float)live[i] + d; }
            else T[i] = (float)live[i];
            if (s_m3.sh_on) T[i] -= s_m3.sh_d[i];            /* пружине - поза без хода выстрела */
        }
        if (!s_m3.on && want && !gait_in) {
            s_m3.on = 1; for (int i = 0; i < 21; i++) { s_m3.x[i] = (float)live[i]; s_m3.v[i] = 0.f; }
            { static long n = 0; if (++n <= 12 || (n % 200) == 0) fprintf(stderr, "aimwalk: mode 3 - arms to the walk stance while she turns (sub %d)%c", sub, 10); }
        }
        if (s_m3.on) {
            static float om = -1.f; if (om < 0.f) { const char *e = getenv("DC_AIM3_OMEGA"); om = e ? (float)atof(e) : 24.f; if (om < 4.f) om = 4.f; }
            /* 13.10 (игрок: «жмёшь идти вбок и стрельбу - на кадр пистолет куда-то в сторону, потом переход»): выстрел (кнопка, подсостояние
             * 2) - к позе игры втрое быстрее: к выстрелу руки уже в прицеле, а не в позе походки стволом вверх */
            const float w = (shot || g_dc_fire_btn || sub == 2) ? om * 3.f : om;
            const float dt = (float)(df > 4u ? 4u : df) / 60.f, ex = expf(-w * dt);
            int settled = !want;
            for (int i = 0; i < 21; i++) {
                float e = s_m3.x[i] - T[i]; while (e > 2048.f) e -= 4096.f; while (e < -2048.f) e += 4096.f;
                const float v = s_m3.v[i], c = v + w * e;
                const float en = (e + c * dt) * ex, vn = (v - w * c * dt) * ex;
                s_m3.x[i] = T[i] + en; s_m3.v[i] = vn;
                if (fabsf(en) > 6.f || fabsf(vn) > 30.f) settled = 0;
            }
            if (settled) s_m3.on = 0;
        }
        { static int lg2 = -1; if (lg2 < 0) { const char *e = getenv("DC_M3ARM_LOG"); lg2 = (e && *e == '1') ? 1 : 0; }   /* 13.10: поза рук по кадрам (разбор выстрела на ходу) */
          if (lg2 && act == 6) { fprintf(stderr, "m3arm: f=%u sub=%d want=%d gait=%d shot=%d on=%d live", fr, sub, want, gait_in, shot, s_m3.on);
                                 for (int i = 0; i < 21; i++) fprintf(stderr, " %d", live[i]);
                                 fprintf(stderr, " T"); for (int i = 0; i < 21; i++) fprintf(stderr, " %.0f", T[i]);
                                 fprintf(stderr, " x"); for (int i = 0; i < 21; i++) fprintf(stderr, " %.0f", s_m3.on ? s_m3.x[i] : (float)live[i]);
                                 fprintf(stderr, "%c", 10); } }
    }
    if (!s_m3.on) return;
    for (int i = 0; i < 21; i++) {
        uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)(1 + i / 3) + AP_NODE_ANG + 2u * (uint32_t)(i % 3));
        const int q = (int)lroundf(s_m3.x[i] + (s_m3.sh_on ? s_m3.sh_d[i] : 0.f)) & 0xFFF;   /* + ход выстрела как есть */
        *pp = (uint16_t)((*pp & ~0xFFF) | q);
    }
    s_ao.active = 1;                                     /* выход вернёт снимок входа (s_ao.saved_all) */
}
static void ao_drawpose_log(uint8_t *ram) {                     /* 13.10: DC_DRAWPOSE_LOG=1 - углы узлов 0-8, по которым рисуется кадр */
    static int lg = -1; if (lg < 0) { const char *e = getenv("DC_DRAWPOSE_LOG"); lg = (e && *e == '1') ? 1 : 0; }
    if (!lg || ram[0xB92C9] != 6) return;
    extern uint64_t s_frame_count; int mm = -1, kk = -1; ao_last_motion(&mm, &kk);
    fprintf(stderr, "drawpose: f=%u sub=%d m=%d k=%d m3=%d sh=%d a", (uint32_t)s_frame_count, ram[0xB92CA], mm, kk, s_m3.on, s_m3.sh_on);
    for (int i = 0; i < 27; i++) fprintf(stderr, " %d", *(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)(i / 3) + AP_NODE_ANG + 2u * (uint32_t)(i % 3)) & 0xFFF);
    fprintf(stderr, "%c", 10);
}
static void ao_enter_main(CPUState *cpu, int gate_actor);
static void ao_enter(CPUState *cpu, int gate_actor) {
    if (gate_actor && (cpu->gpr[4] & 0x1FFFFFu) != 0xB928Cu) return;
    ao_enter_main(cpu, gate_actor);
    ao_m3_post();
    if (!gate_actor) { uint8_t *ram = memory_get_ram_ptr(); if (ram) ao_drawpose_log(ram); }
}
static void ao_enter_main(CPUState *cpu, int gate_actor) {
    /* Хук стоит на входе func_80066260 — построение ЛОКАЛЬНЫХ матриц узлов актёра из углов
     * (a0 = актёр; libgte RotMatrix по узлам, запись node+0 с ra 0x800662DC). Раньше стоял на
     * func_80073F10 — а та лишь сцепляет уже готовые матрицы: правки углов там до рендера не
     * доходили вовсе (06.09, скрины с доворотом 33° и без — попиксельно одинаковы). Только Регина. */
    if (gate_actor && (cpu->gpr[4] & 0x1FFFFFu) != 0xB928Cu) return;
    /* ОДИН ТИК — ОДИН ШАГ (06.09, замер кольца: func_80066260 для Регины зовут 4 раза за тик игры,
     * все в одном кадре хоста; первый — до распаковки кадра моушена, остальные — после). Раньше
     * каждый вызов двигал автомат (кроссфейд 6 тиков схлопывался в полтора — «переключение в один
     * кадр» на стыке вскидывания и стойки), а шаг позы между вызовами был нулевым. Теперь: первый
     * вызов кадра снимает состояние, каждый следующий восстанавливает его и считает заново от живых
     * углов — итог тика = последний вызов, автомат двигается один раз. */
    { extern uint64_t s_frame_count; uint32_t fr = (uint32_t)s_frame_count;
      if (fr != s_ao_key_frame) { s_ao_key_frame = fr; s_ao_calls_in_frame = 1; s_ao_snap = s_ao; s_ao_ring_n_snap = s_ao_ring_n; }
      else { s_ao_calls_in_frame++; s_ao = s_ao_snap; s_ao_ring_n = s_ao_ring_n_snap; } }
    ao_cfg(); s_ao.active = 0;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    /* 12.10.2026 поздно (режим 3: «слайдится назад при смене направления, позы в один кадр»): снимок углов ВСЕХ костей на
     * входе - всегда, на каждом вызове. Выход (pt_anim_aimoff_exit) возвращает его, если мы что-то меняли; раньше пути с
     * ранним выходом (режим 3, стойка DC_AIM_READY_POSE) ставили active, а снимок оставался давний - выход каждый такт
     * возвращал в скелет ноги и корень из старого кадра, и игра двигала её до 400 ед. за такт. */
    for (int b0 = 0; b0 < 15; b0++) for (int c0 = 0; c0 < 3; c0++)
        s_ao.saved_all[b0*3+c0] = *(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b0 + AP_NODE_ANG + 2*c0) & 0xFFF;
    s_ao.saved_all_valid = 1;
    /* (07.09) ходьба с оружием (подсостояния 5 вперёд / 6 назад): миксер стоек игры НЕ работает — анимация
     * походки держит руку на своей высоте (замер: ось −17° при байте 0 «вверх»), и оружие не смотрит на
     * перекрестие. Верх тела на ходу собираем сами (фаза 6 ниже) — и при существе под перекрестием тоже
     * (байт тогда игровой), поэтому гейт «нет цели» здесь не выходит для ходьбы. */
    int walking_now = (ram[0xB92C9] == 6 && (ram[0xB92CA] == 5 || ram[0xB92CA] == 6));
    /* (07.09 ~11:30, игрок: «верни оригинальные анимации при ходьбе в прицеле — Регина не вскидывает пистолет как
     * раньше») подмена верха тела на ходу ВЫКЛЮЧЕНА по умолчанию: на ходу играет походка игры как есть, без наших
     * поправок; DC_AIMOFF_WALK=1 — включить подмену (руки+голова = смесь стоек по перекрестию + fwd/side). */
    static int s_ao_walk = -1;
    if (s_ao_walk < 0) { const char *e = getenv("DC_AIMOFF_WALK"); s_ao_walk = (e && *e == '1') ? 1 : 0; }
    /* 04.10 (6): в оригинальном режиме руки ведёт игра (её автоприцел) — доворота от нашего прицела нет (DC_ORIG_PURE) */
    { extern int dc_orig_pure(void);
      if (dc_orig_pure()) { s_ao_want = 0; s_ao.cur = 0.f; s_ao.side_cur = 0.f; s_ao.reason = 6; s_ao.prev_valid = 0; s_ao.xfade = 0; return; } }
    if (!s_ao_on || (!s_ao_want && !(walking_now && s_ao_walk))) { s_ao.cur *= 0.8f; s_ao.reason = 4; return; }
    /* func_80073F10 обходит ВСЕХ актёров за один вызов (список на стеке от func_80073E34), поэтому
     * отбор по объекту не нужен: узлы Регины лежат по фиксированному 0xB94EC — правим их на входе,
     * возвращаем на выходе (объект в скретчпаде 0x1F800000 = 0xB7850, это не актор). */
    s_ao.last_actor = cpu->read_word(0x1F800000u) & 0x1FFFFFu;
    if (ram[0xB92C9] != 6) {
        /* вышли из прицела: если кроссфейд ещё идёт (или доворот не затух) — дожить его к чистой игровой
         * позе, без доворотов; иначе сброс (07.09, игрок: резкий выход из tuning) */
        if (s_ao.prev_valid && (s_ao.xfade > 0 || fabsf(s_ao.cur) >= 0.5f || fabsf(s_ao.side_cur) >= 0.5f)) {
            if (s_ao.xfade <= 0) s_ao.xfade = AO_XFADE;
            int ang0[45];
            for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) ang0[b*3+c] = *(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c) & 0xFFF;
            memcpy(s_ao.saved_all, ang0, sizeof s_ao.saved_all); s_ao.saved_all_valid = 1;
            float t = 1.f / (float)(s_ao.xfade + 1);
            for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
                if (!ao_bone_ours(b)) continue;
                uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
                int cur = *pp & 0xFFF, prev = s_ao.prev_out[b*3+c];
                int d = ((cur - prev + 2048) & 0xFFF) - 2048;
                *pp = (uint16_t)((*pp & ~0xFFF) | ((prev + (int)lroundf(d * t)) & 0xFFF));
            }
            s_ao.xfade--; s_ao.cur = 0.f; s_ao.side_cur = 0.f; s_ao.phase = 0; s_ao.active = 1; ao_note_out(ram);
            if (s_ao.xfade <= 0) s_ao.prev_valid = 0;
            return;
        }
        s_ao.cur *= 0.8f; s_ao.reason = 2; s_ao.prev_valid = 0; s_ao.prev_phase = 0; s_ao.xfade = 0; s_ao.saved_all_valid = 0; return; }   /* только в прицеле (и выстреле: act остаётся 6) */
    if (!s_ao_w_on[ao_widx(ram)]) { s_ao.cur *= 0.8f; s_ao.side_cur *= 0.8f; s_ao.reason = 5; s_ao.prev_valid = 0; s_ao.prev_phase = 0; s_ao.xfade = 0; s_ao.saved_all_valid = 0; return; }   /* доводка выключена для этого оружия (меню) */
    if (!ao_extremes(ram)) { s_ao.reason = 3; return; }
    /* 12.10.2026 ХОДЬБА В ПРИЦЕЛЕ, режим «СТОЙКА» (main.cpp dc_aim_walk_mode = 2; игрок: «пошёл в стороны - Регина переходит в
     * стойку, где пистолет поднят»): пока флаг стоит, руки (кости 1..6) - стойка «вверх» резидентного набора (байт высоты 0) и
     * ещё DC_AIM_READY_UP градусов вверх вокруг линии плеч; голова, корпус и ноги - походка игры. Вход и выход - смесь за
     * несколько тиков (вес раз за тик). Вскидывание (подсостояние 0) и опускание (4) - как у игры. */
    /* 12.10.2026 вечер (игрок: «в режиме поднятого оружия оставь всё как в оригинальном поведении, только отдаляй камеру и
     * скрывай прицел»): поза рук больше не меняется - походка и руки игры; DC_AIM_READY_POSE=1 - вернуть руки «вверх». */
    { extern int dc_aim_ready_now(void); extern uint64_t s_frame_count;
      static float rw = 0.f, rup = -999.f; static uint32_t rw_frame = 0xFFFFFFFFu; static int rpose = -1;
      if (rup < -900.f) { const char *e = getenv("DC_AIM_READY_UP"); rup = e ? (float)atof(e) : 15.f; }
      if (rpose < 0) { const char *e = getenv("DC_AIM_READY_POSE"); rpose = (e && *e == '1') ? 1 : 0; }
      const int want_ready = rpose && dc_aim_ready_now() && ram[0xB92CA] != 0 && ram[0xB92CA] != 4;
      const uint32_t fr = (uint32_t)s_frame_count;
      if (fr != rw_frame) { rw_frame = fr; rw += ((want_ready ? 1.f : 0.f) - rw) * 0.35f;
                            if (!want_ready && rw < 0.02f) rw = 0.f; if (want_ready && rw > 0.98f) rw = 1.f; }
      if (rw > 0.f) {
          int up[45], fin[45]; ao_blend_stances(0, up);
          float Wu[15][9], Pu[15][3]; ao_fk(up, Wu, Pu);
          const float hx2 = Pu[AO_HAND_R][0]-Pu[AO_HAND_L][0], hy2 = Pu[AO_HAND_R][1]-Pu[AO_HAND_L][1], hz2 = Pu[AO_HAND_R][2]-Pu[AO_HAND_L][2];
          const int two = (sqrtf(hx2*hx2 + hy2*hy2 + hz2*hz2) < 500.f);
          ao_offset_pose(up, rup, 0.f, two, fin);
          for (int b = 1; b <= 6; b++) for (int c = 0; c < 3; c++) {
              uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
              const int cur = *pp & 0xFFF, d = ((fin[b*3+c] - cur + 2048) & 0xFFF) - 2048;
              *pp = (uint16_t)((*pp & ~0xFFF) | ((cur + (int)lroundf(d * rw)) & 0xFFF));
          }
          s_ao.cur = 0.f; s_ao.side_cur = 0.f; s_ao.prev_valid = 0; s_ao.xfade = 0; s_ao.phase = 8; s_ao.reason = 8; s_ao.active = 1;
          { static long n = 0; if (++n <= 6 || (n % 600) == 0) fprintf(stderr, "aimwalk: ready stance, arms up (weight %.2f, extra %.0f deg, %s)%c", rw, rup, two ? "two hands" : "one hand", 10); }
          return;
      } }
    s_ao.reason = 0; s_ao.seen++;
    int ang[45]; float W[15][9], P[15][3];
    for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) ang[b*3+c] = *(uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c) & 0xFFF;
    ao_fk(ang, W, P);
    float elev = ao_elev2(W, P);
    float hx = P[AO_HAND_R][0]-P[AO_HAND_L][0], hy = P[AO_HAND_R][1]-P[AO_HAND_L][1], hz = P[AO_HAND_R][2]-P[AO_HAND_L][2];
    int hands_close = (sqrtf(hx*hx + hy*hy + hz*hz) < 500.f);      /* обе кисти на оружии */
    /* ФАЗЫ (игрок 06.09: «сначала встаёт в дефолтную позу, потом одним кадром в расширенную» и
     * «выстрел начинается выше/ниже»):
     *  вскидывание (подсостояние 0): доворот растёт с прогрессом анимации вскидывания и в её
     *    последнем кадре равен (цель − уровень) — рука приходит СРАЗУ в расширенную позу; на первом
     *    тике смешивания стоек остаток пересчитывается от живой позы, сумма непрерывна;
     *  спокойный прицел (стойки распаковывались только что): остаток = цель − ось живой позы;
     *  всё остальное в act 6 (выстрел, перезарядка, помпа): доворот ЗАМОРОЖЕН на значении прицела,
     *    отдача играет поверх — старт выстрела ровно в позе прицела; хват тоже защёлкнут. */
    extern uint64_t s_frame_count;
    int is_stance = (s_ap_aim.valid && (uint32_t)s_frame_count - s_ap_aim.frame <= 1);   /* стойка распакована в ЭТОМ тике */
    int raising = (ram[0xB92CA] == 0), lowering = (ram[0xB92CA] == 4);
    int byte_now = (s_ao_byte >= 0 && (uint32_t)s_frame_count - s_ao_byte_frame <= 30) ? s_ao_byte : ram[0xB94B2];   /* НАШ байт (единственный честный источник в прицеле: в RAM игра при очереди прибавляет по 16 за выстрел) */
    float e_ref = ao_elev_for_byte(byte_now);          /* ось, которую даёт смесь стоек при этом байте */
    /* (07.09) ходьба с оружием: какой моушен играет — походка или выстрел (у выстрела верх тела не подменяем,
     * отдача остаётся игровой; после выстрела сразу возвращаемся к стойке кроссфейдом) */
    int mcur = -1, kcur = 0; int shot_now = ao_last_motion(&mcur, &kcur) && ao_motion_is_shot(mcur);
    float theta;
    /* (07.09 ~11:45) ФАЗА 7 — вскидывание при удерживаемом движении: у двуручных «иду → прицел» игра сначала играет
     * отдельное вскидывание (sub 0), потом походку с оружием; наш морф заканчивал вскидывание в подогнанной стойке,
     * и кроссфейд в игровую походку давал лишний провал (гранатомёт: ось −10 → −14 → 0). Игрок хочет оригинальную
     * походку — значит и вскидывание перед ней оригинальное: руки прочь, пока держится направление. */
    int handsoff_raise = 0;
    { extern int dc_ots_move_intent(void);
      /* + тик(и) sub 1 сразу после такого вскидывания: игра переходит в походку через одно стояние — иначе один тик
       * фазы 4 с наклоном fwd (пистолет: «7/0 ×12, 4/1, 6/5», доворот 8.9 на тик) */
      handsoff_raise = ((raising || (s_ao.prev_phase == 7 && ram[0xB92CA] == 1 && !is_stance)) && dc_ots_move_intent() && !s_ao_walk); }
    if (handsoff_raise) {
        theta = 0.f; s_ao.morph_valid = 0; s_ao.phase = 7; s_ao.had_stance = 0;
    } else if (raising) {
        /* вскидывание: ЖЕЛАЕМАЯ траектория суммы от позы старта к цели (smoothstep по кадру анимации
         * вскидывания), доворот = желаемое − живая ось. Прежний prog² давал горб: при прицеле вниз рука
         * сначала поднималась к уровню за анимацией и только потом опускалась (кадры игрока 06.09). */
        uint32_t src = 0; float prog = 0.f;
        for (int q = 1; q <= 8; q++) { AnimRingEnt *e = &s_aring[(s_aring_head + ANIM_RING - q) % ANIM_RING];
            uint32_t sp2 = e->src & 0x1FFFFFu; if (sp2 >= AP_BLOCK && sp2 < AP_BLOCK + 0x20000) { src = sp2; break; } }
        if (src >= AP_BLOCK + 12) {
            uint32_t off = src - AP_BLOCK - 12;
            for (int m = 0; m < s_ao_nmot; m++) if (off >= s_ao_mots[m].data && off < s_ao_mots[m].data + (uint32_t)s_ao_mots[m].nf * 80u) {
                int k = (int)((off - s_ao_mots[m].data) / 80u); int nf = s_ao_mots[m].nf;
                prog = nf > 2 ? (float)(k + 1) / (float)(nf - 1) : 1.f; if (prog > 1.f) prog = 1.f;
                /* (07.09, игрок: «при отжатии/нажатии в определённом тайминге анимация перехода сбрасывается
                 * и Регина одним кадром встаёт в позу прицела») кадр вскидывания пошёл с начала = игра
                 * перезапустила вскидывание: морф тоже заново, с текущей позы (если phase 1 не прерывался,
                 * prog «только вперёд» держал бы ~1 и первый же кадр давал бы полную стойку) */
                if (s_ao.phase == 1 && k + 1 < s_ao.last_k) { s_ao.prog = 0.f; s_ao.e_start = elev; s_ao.prev_valid = 0; }
                s_ao.last_k = k + 1; break; }
        }
        if (s_ao.phase != 1) { s_ao.e_start = elev; s_ao.prog = 0.f; s_ao.last_k = 0; }
        if (prog < s_ao.prog) prog = s_ao.prog;                        /* только вперёд */
        s_ao.prog = prog;
        float sm = prog * prog * (3.f - 2.f * prog);
        /* МОРФ ПОЗЫ (игрок 06.09: «переключение видно на максимальных углах»): жёсткий поворот
         * анимации вскидывания давал согнутую руку под большим углом, а первый тик миксера — вытянутую
         * стойку. Теперь целевая поза считается сразу: смесь стоек при нашем байте + остаток доворота,
         * и поза вскидывания по всем костям морфится в неё по smoothstep. В конце вскидывания поза
         * равна тому, что даст миксер, — стык исчезает по построению. */
        int aim_ang[45], fin[45]; ao_blend_stances(byte_now, aim_ang);
        { float Wa[15][9], Pa[15][3]; ao_fk(aim_ang, Wa, Pa);
          float hx2 = Pa[AO_HAND_R][0]-Pa[AO_HAND_L][0], hy2 = Pa[AO_HAND_R][1]-Pa[AO_HAND_L][1], hz2 = Pa[AO_HAND_R][2]-Pa[AO_HAND_L][2];
          s_ao.two = (sqrtf(hx2*hx2 + hy2*hy2 + hz2*hz2) < 500.f); }
        float th_ref = ao_resid(ram, e_ref);                           /* 30.09: + запас вниз оружия (ao_resid) */
        th_ref += ao_bias(ram);                                        /* постоянная поправка двуручных */
        ao_offset_pose(aim_ang, th_ref, ao_side(ram), s_ao.two, fin);
        for (int i2 = 0; i2 < 45; i2++) { int d = ((fin[i2] - ang[i2] + 2048) & 0xFFF) - 2048; s_ao.morph[i2] = (ang[i2] + (int)lroundf(d * sm)) & 0xFFF; }
        { float Wm[15][9], Pm[15][3]; ao_fk(s_ao.morph, Wm, Pm); theta = ao_elev2(Wm, Pm) - elev; }   /* для кольца: сумма = ось выхода */
        s_ao.morph_valid = 1;
        s_ao.two = hands_close; s_ao.phase = 1; s_ao.had_stance = 0;
    } else if (lowering) {
        theta = s_ao.cur * 0.75f; s_ao.phase = 5;                     /* опускание оружия: доворот затухает */
    } else if (walking_now && !shot_now && !s_ao_walk) {   /* и в тик распаковки стойки на входе в походку — иначе один тик наклона fwd */
        /* ФАЗА 6 по умолчанию — ХОДЬБА С ОРУЖИЕМ БЕЗ НАШИХ ПРАВОК (07.09 ~11:30, игрок: «верни оригинальные
         * анимации»). Игра в подсостояниях 5/6 играет походку целиком (байт высоты не читает), стойки не
         * распаковываются; раньше доводка сидела здесь в фазе 3 и добавляла к походке постоянный наклон fwd/side
         * — «оружие задирается на оффсеты». Теперь на ходу поза игры остаётся как есть: ни доворота, ни поправок;
         * на стыках стойка<->ходьба — только кроссфейд AO_XFADE (ниже), чтобы не было щелчка. */
        theta = 0.f; s_ao.morph_valid = 0; s_ao.phase = 6; s_ao.had_stance = 1;
    } else if (walking_now && !shot_now && !is_stance) {
        /* ФАЗА 6 с DC_AIMOFF_WALK=1 — ПОДМЕНА ВЕРХА ТЕЛА (07.09 ~11:00; игрок отверг: хочет оригинальную походку).
         * Верх тела (руки, голова — кости 1..7) на ходу = ТА ЖЕ смесь стоек по байту высоты + те же поправки
         * fwd/side, что стоя; корпус, таз и ноги — походка игры (AO_UPPER_ONLY), покачивание спины несёт руки.
         * Стыки — кроссфейд AO_XFADE. Выстрел на ходу (моушен kind 1) — фаза 3: отдача игровая. При существе под
         * перекрестием байт игровой — руки идут за её прицелом. Оставлено на случай, если игрок захочет попробовать. */
        int aim_ang[45], fin[45]; ao_blend_stances(byte_now, aim_ang);
        { float Wa[15][9], Pa[15][3]; ao_fk(aim_ang, Wa, Pa);
          float hx2 = Pa[AO_HAND_R][0]-Pa[AO_HAND_L][0], hy2 = Pa[AO_HAND_R][1]-Pa[AO_HAND_L][1], hz2 = Pa[AO_HAND_R][2]-Pa[AO_HAND_L][2];
          s_ao.two = (sqrtf(hx2*hx2 + hy2*hy2 + hz2*hz2) < 500.f); }
        float th_ref = s_ao_want ? (s_ao_target_deg - e_ref) : 0.f; if (th_ref > s_ao_max) th_ref = s_ao_max; if (th_ref < -s_ao_max) th_ref = -s_ao_max;
        th_ref += ao_bias(ram);
        ao_offset_pose(aim_ang, th_ref, ao_side(ram), s_ao.two, fin);
        memcpy(s_ao.morph, fin, sizeof s_ao.morph); s_ao.morph_valid = 1;
        { float Wm[15][9], Pm[15][3]; ao_fk(fin, Wm, Pm); theta = ao_elev2(Wm, Pm) - elev; }   /* для кольца: на сколько подняли относительно походки */
        s_ao.phase = 6; s_ao.had_stance = 1;
    } else if (!s_ao.had_stance) {
        theta = s_ao_want ? ao_resid(ram, elev) : s_ao.cur * 0.8f; s_ao.phase = 4;   /* мост: сумма на цели */
        if (is_stance) s_ao.had_stance = 1;
    } else {
        /* прицел и выстрел/перезарядка/помпа: доворот считается от БАЙТА, а не от живой позы —
         * переходные позы (кроссфейд после выстрела, шаг байта игрой при очереди) не замораживаются
         * в доворот (у игрока: −40° вместо −16°, ствол на 30° ниже перекрестия). Отдача и возврат
         * видны как разница живой позы и смеси стоек. */
        theta = s_ao_want ? ao_resid(ram, e_ref) : s_ao.cur * 0.8f;
        if (is_stance) s_ao.two = hands_close;
        s_ao.phase = is_stance ? 2 : 3;
    }
    int handsoff = (s_ao.phase == 6 || s_ao.phase == 7);                                 /* 6 походка (по умолчанию руки прочь / подмена целиком), 7 вскидывание при движении */
    if (s_ao.phase != 1 && !handsoff) { const float lo_c = ao_resid_lo(ram); if (theta > s_ao_max) theta = s_ao_max; if (theta < lo_c) theta = lo_c; }
    if (s_ao.phase == 2 || s_ao.phase == 3 || s_ao.phase == 4) theta += ao_bias(ram);   /* постоянная поправка двуручных */
    s_ao.side_cur = (s_ao.phase == 5) ? s_ao.side_cur * 0.75f : (handsoff ? 0.f : ao_side(ram));   /* поворот вбок (меню), при опускании затухает; 6/7 — нет */
    if (s_ao_force > -900.f) { theta = s_ao_force; s_ao.phase = 3; s_ao.morph_valid = 0; handsoff = 0; }   /* отладка: фиксированный доворот */
    s_ao.cur = handsoff ? 0.f : theta;                                                   /* 6/7: жёсткого доворота сверху нет */
    s_ao.elev = elev; s_ao.target = s_ao_target_deg; s_ao.theta = theta;
    { uint32_t i = s_ao_ring_n % AO_RING; s_ao_ring[i].frame = (uint32_t)s_frame_count; s_ao_ring[i].phase = s_ao.phase;
      s_ao_ring[i].elev = elev; s_ao_ring[i].theta = theta; s_ao_ring[i].two = s_ao.two; s_ao_ring[i].sub = ram[0xB92CA]; s_ao_ring[i].eref = e_ref; s_ao_ring[i].bnow = byte_now; s_ao_ring[i].bours = s_ao_byte; s_ao_ring[i].bram = ram[0xB94B2]; s_ao_ring[i].age = (int)((uint32_t)s_frame_count - s_ao_byte_frame); s_ao_ring[i].xf = s_ao.xfade;
      for (int r6 = 0; r6 < 3; r6++) { s_ao_ring[i].root[r6] = *(uint16_t *)(ram + AP_NODES + 24 + 2*r6) & 0xFFF; s_ao_ring[i].root[3+r6] = (int16_t)*(uint16_t *)(ram + AP_NODES + 68 + 2*r6); }
      s_ao_ring[i].calls = s_ao_calls_in_frame; s_ao_ring[i].mstep = 0.f; s_ao_ring[i].mbone = -1;
      s_ao_ring_n++; }
    /* КРОССФЕЙД НА СТЫКЕ (игрок 06.09: «#50 дефолтное вскидывание, #51 уже правленая смесь стоек» —
     * форма руки/головы меняется за один кадр: конец анимации вскидывания = оригинальная поза,
     * первый тик миксера = смесь с правленными стойками; у игры тут лишь один тик 50 %):
     * после фазы 1 в течение AO_XFADE тиков выход = смесь углов ВСЕХ костей между тем, что мы
     * записали в прошлый тик, и новой позой (с доворотом), кратчайшая дуга. */
    memcpy(s_ao.saved_all, ang, sizeof s_ao.saved_all); s_ao.saved_all_valid = 1;
    if (s_ao.prev_phase == 1 && s_ao.phase != 1 && s_ao.prev_valid) { s_ao.xfade = AO_XFADE; s_ao.slew_hold = 8; }
    /* (07.09, игрок: «при отжатии та же резкость, что была при нажатии») стык стойка -> опускание: тот же
     * кроссфейд всех костей от последнего выхода к позе опускания */
    if (s_ao.phase == 5 && s_ao.prev_phase != 5 && s_ao.prev_phase != 0 && s_ao.prev_valid) { s_ao.xfade = AO_XFADE; }
    /* (07.09) стыки стойка/выстрел <-> ходьба с оружием (фаза 6): тот же кроссфейд всех верхних костей */
    { int ho_now = (s_ao.phase == 6 || s_ao.phase == 7), ho_prev = (s_ao.prev_phase == 6 || s_ao.prev_phase == 7);
      if (ho_now != ho_prev && s_ao.prev_phase >= 2 && s_ao.prev_valid) { s_ao.xfade = AO_XFADE; } }
    s_ao.prev_phase = s_ao.phase;
    if (s_ao.phase == 6 && s_ao.morph_valid) {   /* ходьба с оружием: верх тела = смесь стоек + поправки; дальше — только кроссфейд стыка */
        for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
            if (!ao_bone_ours(b)) continue;
            uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
            *pp = (uint16_t)((*pp & ~0xFFF) | (s_ao.morph[b*3+c] & 0xFFF)); }
        s_ao.morph_valid = 0; s_ao.active = 1; s_ao.applied++;
    }
    if (s_ao.phase == 1 && s_ao.morph_valid) {
        for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
            if (!ao_bone_ours(b)) continue;
            uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
            *pp = (uint16_t)((*pp & ~0xFFF) | (s_ao.morph[b*3+c] & 0xFFF)); }
        s_ao.morph_valid = 0; ao_slew(ram); s_ao.active = 1; s_ao.applied++; ao_note_out(ram); return;
    }
    if (fabsf(s_ao.cur) < 0.05f && fabsf(s_ao.side_cur) < 0.05f && s_ao.xfade <= 0 && s_ao.slew_hold <= 0) { ao_note_out(ram); return; }
    if (fabsf(s_ao.cur) >= 0.05f || fabsf(s_ao.side_cur) >= 0.05f) {
    /* ось = линия плеч (левое плечо -> правое), в системе персонажа */
    float axis[3] = { P[AO_ARM_R][0] - P[AO_ARM_L][0], P[AO_ARM_R][1] - P[AO_ARM_L][1], P[AO_ARM_R][2] - P[AO_ARM_L][2] };
    int two_hands = s_ao.two;                                       /* хват защёлкнут в фазе прицела */
    float th = s_ao.cur, Rs[9], Ra[9], Rh[9];
    ao_rot3(axis, th, s_ao.side_cur, Rs, Ra, Rh);                   /* корпус / руки / голова: наклон + поворот вбок */
    /* желаемые мировые повороты: спина Rs*W; таз без изменений (ноги на месте); руки Ra*Rs*W = полный th;
     * голова Rh*Rs*W */
    float Ws[9], WsT[9], tmp[9], Ln[9]; int q[3];
    ao_mul(Rs, W[AO_SPINE], Ws); ao_tr(Ws, WsT);
    int list[5] = { AO_SPINE, AO_PELVIS, AO_ARM_R, AO_ARM_L, AO_HEAD }; s_ao.n = 0;
    for (int k = 0; k < 5; k++) {
        int b = list[k];
        if (b == AO_ARM_L && !two_hands) continue;
        if ((b == AO_SPINE || b == AO_PELVIS) && !ao_bone_ours(b)) continue;   /* корень/таз — никогда (дрейф) */
        if (b == AO_SPINE) memcpy(Ln, Ws, sizeof Ln);
        else if (b == AO_PELVIS) ao_mul(WsT, W[AO_PELVIS], Ln);
        else { float Wd[9]; ao_mul(Rs, W[b], tmp); ao_mul(b == AO_HEAD ? Rh : Ra, tmp, Wd); ao_mul(WsT, Wd, Ln); }
        ao_decompose(Ln, q);
        s_ao.bones[s_ao.n] = b;
        for (int c = 0; c < 3; c++) { uint16_t *p = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
            s_ao.saved[s_ao.n][c] = *p; *p = (uint16_t)((*p & ~0xFFF) | (q[c] & 0xFFF)); }
        s_ao.n++;
    }
    }
    if (s_ao.xfade > 0) {
        float t = 1.f / (float)(s_ao.xfade + 1);           /* 1/6, 1/5, ... 1/1 -> плавно до новой позы */
        for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
            if (!ao_bone_ours(b)) continue;
            uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
            int cur = *pp & 0xFFF, prev = s_ao.prev_out[b*3+c];
            int d = ((cur - prev + 2048) & 0xFFF) - 2048;
            int out = (prev + (int)lroundf(d * t)) & 0xFFF;
            *pp = (uint16_t)((*pp & ~0xFFF) | out);
        }
        s_ao.xfade--;
    }
    if (s_ao.phase == 4 || s_ao.xfade > 0 || s_ao.slew_hold > 0) { ao_slew(ram); if (s_ao.slew_hold > 0) s_ao.slew_hold--; }
    s_ao.active = 1; s_ao.applied++;
    ao_note_out(ram);
    { extern uint64_t s_frame_count; s_ao.frame = (uint32_t)s_frame_count; }
    { static long n = 0; if ((++n % 120) == 1) fprintf(stderr, "aimoff: ось %+.1f цель %+.1f доворот %+.1f град (%s)\n", elev, s_ao_target_deg, s_ao.cur, s_ao.two ? "обе руки" : "правая"); }
}
void pt_anim_aimoff_exit(CPUState *cpu) {
    (void)cpu;
    if (!s_ao.active) return;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    if (s_ao.saved_all_valid)
        for (int b = 0; b < 15; b++) for (int c = 0; c < 3; c++) {
            uint16_t *pp = (uint16_t *)(ram + AP_NODES + AP_NODE_STRIDE * b + AP_NODE_ANG + 2*c);
            *pp = (uint16_t)((*pp & ~0xFFF) | (s_ao.saved_all[b*3+c] & 0xFFF)); }
    s_ao.active = 0;
}
/* ⭐ 28.09.2026 (3) — РЕГИНА НА ЛЕСТНИЦЕ НЕ В СТУПЕНЯХ (игрок, слот 19, комната 51 = ST408: «жмёшь взаимодействие, Регина
 * поднимается по лестнице, только она очень глубоко смещена в лестницу»). Родная отрисовка консоли даёт то же: так её ставит
 * анимация игры. Лестница — подпрограмма скрипта комнаты: ставит Регину к лестнице (0x36/0x37), помечает движение скриптовым
 * (+338 игрока = 2, команда 2A 00 21), играет движение из блока комнаты (подъём/спуск), затемнение, перенос на другой этаж.
 * Признак лестницы — само движение: корень поднимается над стойкой (y -1179) больше чем на 500 при вертикальном ходе >= 600 и
 * почти без хода по горизонтали (|x|, |z| <= 250) — у обоих подъёмов и спусков ST408 так, у ходьбы, стоек и поворотов нет.
 * На время ОТРИСОВКИ (вызов func_80073F10 в func_80073084, правка в сгенерированном коде) смещение корня скелета (узел 0 =
 * 0xB94EC, +68 — в системе персонажа; лицом к лестнице — +z) сдвигается назад от лестницы на DC_LADDER_SHIFT единиц (200:
 * замер на ST408 — при 160 ступени на подъёме ещё режут бёдра, при 240 на спуске косая камера уводит её от лестницы вбок); спуск,
 * который кончается у пола, последние кадры возвращает к нулю (без скачка к лестнице). На выходе смещение возвращается — логика игры
 * (руки, столкновения, камеры) сдвига не видит. DC_LADDER_SHIFT=0 — как было. */
#define LAD_NODE0 0xB94ECu
static int lad_shift_cfg(void) { static int d = -99999; if (d == -99999) { const char *e = getenv("DC_LADDER_SHIFT"); d = e ? atoi(e) : 200; } return d; }
/* 28.09 (3) (игрок: «спуск хорошо, а подъём ещё отодвигать надо»): подъём — своя величина DC_LADDER_SHIFT_UP (320); 0 — как спуск;
 * DC_LADDER_SHIFT=0 выключает оба */
static int lad_shift_up_cfg(void) { static int d = -99999; if (d == -99999) { const char *e = getenv("DC_LADDER_SHIFT_UP"); d = e ? atoi(e) : 320; } return d; }
static int16_t lad_s16(const uint8_t *ram, uint32_t a) { return (int16_t)(ram[a] | (ram[a + 1] << 8)); }
static uint32_t lad_u32(const uint8_t *ram, uint32_t a) { return (uint32_t)ram[a] | ((uint32_t)ram[a + 1] << 8) | ((uint32_t)ram[a + 2] << 16) | ((uint32_t)ram[a + 3] << 24); }
static uint32_t s_lad_key = 0; static int s_lad_is = 0, s_lad_endfloor = 0, s_lad_up = 0;
static int s_lad_applied = 0; static int16_t s_lad_saved[3], s_lad_wrote[3];
/* ===== ХОДЬБА В ПРИЦЕЛЕ, режим «СТРЕЙФ»: ОТРИСОВКА (12.10.2026, main.cpp dc_aim_walk_mode = 1) ===========================
 * Логика ведёт её по направлению шага (курс = направление, походка с оружием - коллизия и скорость игры), а рисуем лицом к
 * перекрестию: на входе func_80073F10 (она сцепляет узлы от матрицы актёра 0x800B928C) поворот матрицы актёра R' = R*Ry(δ),
 * δ = курс прицела - курс логики; на выходе - вернуть. Ноги - sl_legs. Углы в RAM не трогаем - логика (func_80066260) строит
 * свои матрицы заново. DC_STRAFE_SIGN=-1 - если поворот окажется не в ту сторону. */
static struct { int aim_q, legs_q; uint32_t frame; int applied; int16_t saved[9], wrote[9];
                int papplied; int16_t psaved[9], pwrote[9];
                int gapplied; int16_t gsaved[2][3][16], gwrote[2][3][16];
                int rapplied; int16_t rsaved[3], rwrote[3];
                int t0applied; int16_t t0saved[16], t0wrote[16]; int lf_q; } s_sf;
/* 12.10 (вечер, игрок: «на один-два кадра моделька поворачивается в противоположную сторону»): main.cpp отдаёт АБСОЛЮТНЫЙ курс
 * прицела, а не разницу: матрица актёра в кадре смены курса бывает ещё прошлой (её строит игра по курсу не в каждом кадре до
 * отрисовки), и поворот «на разницу» от неё давал разворот на 90° в другую сторону. Теперь курс матрицы меряется по ней самой.
 * torso_q - куда рисовать корпус (вся модель поворачивается на torso_q - курс логики), legs_q - направление шага для ног
 * (sl_legs: круг клипов набора стрейфов). 20.10: прежние виды ног (DC_STRAFE_LEGS 1 - приставной шаг из размаха бёдер,
 * 2 - поворот таза, 3 - свой шаг вбок) вырезаны. */
void dc_strafe_draw_set2(int torso_q, int legs_q) {
    extern uint64_t s_frame_count;
    s_sf.aim_q = torso_q & 4095; s_sf.legs_q = legs_q & 4095;
    s_sf.frame = (uint32_t)s_frame_count;
}
/* 12.10 ночь: смещение перекрестия от направления камеры (кванты, + вправо; main.cpp, пока идёт стрейф) - оно правее камеры на
 * ~8-12°. Ноги считают направление на круге клипов от «камеры» (корпус минус это смещение; доворот режима 2 - вместе с корпусом). */
void dc_strafe_legs_frame(int q) { s_sf.lf_q = q; }
/* 13.10 поздний вечер (4): режим 3 - куда пойдут ноги от корпуса, когда он довернётся (нажатие минус цель корпуса, кванты, + вправо;
 * main.cpp, каждый кадр стрейфа режима 3). «Влево» за боком (h3, sl_legs) держится, только если и цель там */
static int s_goal_q; static uint32_t s_goal_f;
void dc_strafe_legs_goal(int q) { extern uint64_t s_frame_count; s_goal_q = q; s_goal_f = (uint32_t)s_frame_count; }
static void sf_load(const int16_t *m, float *f) { for (int i = 0; i < 9; i++) f[i] = (float)m[i] / 4096.f; }
static void sf_store(const float *f, int16_t *m) { for (int i = 0; i < 9; i++) { float v = f[i] * 4096.f; if (v > 32767.f) v = 32767.f; if (v < -32768.f) v = -32768.f; m[i] = (int16_t)lroundf(v); } }
static void sf_rmin(const float *a, const float *b, float *m) {   /* наименьший поворот, переводящий единичный a в b */
    float ax[3] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0] };
    float dt = a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; if (dt > 1.f) dt = 1.f; if (dt < -1.f) dt = -1.f;
    float n = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    if (n < 1e-6f) { float I[9] = {1,0,0, 0,1,0, 0,0,1}; memcpy(m, I, sizeof I); return; }
    ao_axis_rot(ax, acosf(dt) * 180.f / 3.14159265f, m);
}
/* смещения суставов ног в системе родителя (0: бедро правое в тазу, 1: колено в бедре, 2: лодыжка в голени, 3-5 - левая):
 * на входе func_80073F10 в узлах свежие ЛОКАЛЬНЫЕ повороты, но смещения - мировые с прошлого кадра (замер: «бедро 9716»),
 * поэтому они считаются после сцепки: off = R_родителя^T (T_ребёнка - T_родителя) - постоянные, длины костей из скелета */
static float s_go[6][3]; static int s_go_ok = 0;
static void gait_offsets_capture(const uint8_t *ram) {
    static const int par[6] = { 8, 9, 10, 8, 12, 13 }, chi[6] = { 9, 10, 11, 12, 13, 14 };
    float o[6][3];
    for (int k = 0; k < 6; k++) {
        const int16_t *Mp = (const int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)par[k]);
        const int32_t *tp = (const int32_t *)((const uint8_t *)Mp + 20);
        const int32_t *tc = (const int32_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)chi[k] + 20);
        float R[9]; sf_load(Mp, R);
        const float d[3] = { (float)(tc[0] - tp[0]), (float)(tc[1] - tp[1]), (float)(tc[2] - tp[2]) };
        for (int r = 0; r < 3; r++) o[k][r] = R[0*3+r] * d[0] + R[1*3+r] * d[1] + R[2*3+r] * d[2];     /* R^T d */
        const float n = sqrtf(o[k][0]*o[k][0] + o[k][1]*o[k][1] + o[k][2]*o[k][2]);
        if (n < 40.f || n > 1500.f) return;
    }
    memcpy(s_go, o, sizeof s_go);
    if (!s_go_ok) { s_go_ok = 1;
        fprintf(stderr, "aimwalk: leg bones from the skeleton - hip R %.0f %.0f %.0f, L %.0f %.0f %.0f, knee %.0f %.0f %.0f, ankle %.0f %.0f %.0f%c",
                o[0][0], o[0][1], o[0][2], o[3][0], o[3][1], o[3][2], o[1][0], o[1][1], o[1][2], o[2][0], o[2][1], o[2][2], 10); }
}
/* ===== 12.10.2026 поздно: НОГИ СТРЕЙФА ИЗ НАБОРА ИГРОКА (вид ног 4, по умолчанию; main.cpp DC_STRAFE_LEGS) ==================
 * Игрок: «возьми из моего набора анимации стрейфов; все оригинальные анимации Регины оставь - перенимаешь только ноги стрейфа
 * влево-вправо, дальше как будет лучше»; выбрал стрейф с ногой сзади (BL/BR), диагонали тоже из набора, вперёд и назад - её
 * родная ходьба. Ноги - круг из 8 направлений движения относительно нарисованного корпуса (лицом на перекрестие): вперёд - её
 * походка с оружием (моушен ходьбы набора оружия, игра его и играет), назад - её походка назад (следующий моушен), ±45/±90/±135
 * - стрейфы набора, перенесённые на её скелет (pt_strafe_legs.h; scratchpad mocap/retarget.py: длины ног, высота стопы на
 * опоре как у неё - 111, шаг подогнан к её 1462 ед. за 34 кадра, колено - IK в плоскости колена записи, стопа - поворот записи
 * от плоской опоры). Между соседними направлениями - плавная смесь; фаза одна - от кадра её походки, которую играет игра
 * (выровнена по постановке правой стопы), поэтому корпус и руки игры идут в такт ногам. Скорость: вперёд и вбок - её шаг (43
 * за такт), назад - 1140 за цикл. На развороте больше 100° main.cpp тормозит по старому курсу и разгоняет по новому
 * (DC_STRAFE_BRAKE 8 / DC_STRAFE_ACCEL 12 развёрток): на малой скорости ноги уходят в стойку прицела, смена направления не видна.
 * Начало и конец - смесь с ногами игры (~0.17 с). Таз и смещение корня - тоже из смеси (покачивание шага), корпус и руки - игры. */
#include "pt_strafe_legs.h"
#include <sys/stat.h>
/* 12.10.2026 поздно (игрок: «анимации определил правильно, но в игре часть подставил неправильно и смешиваются плохо; давай
 * я буду тестировать в реальном времени, а ты править»): ЖИВЫЕ НАСТРОЙКИ - файл dc_strafe_live.cfg в папке игры,
 * перечитывается на ходу (раз в полсекунды по времени изменения): что играет на каждом из 8 направлений движения относительно
 * корпуса (dir.F/FR/R/BR/B/BL/L/FL = имя анимации набора BL BR FL FR L45 R45 L135 R135 F B или own_fwd / own_back - её
 * родная ходьба), ширина смешивания соседних (blend 0.1..1), пружины, скорости, доля ровного корпуса, сдвиг фазы клипа
 * (phase.<имя> в кадрах). Без файла - как ниже в sl_live_defaults. Фаза теперь по ОБЕИМ стопам: у каждого клипа кадры
 * постановки правой и левой (td_r, td_l), между ними - половина цикла, так что при смешивании стопы ставятся одновременно
 * (раньше - только по правой, и у BR она была определена на полцикла не там: ноги в смеси шли в противофазе). */
#define SL_NLV 96                                                   /* 13.10 вечер: 64 -> 96 ключей (было 54 из 64) */
static struct { char k[SL_NLV][32], v[SL_NLV][48]; int n; long long mtime; uint32_t check; int loaded; } s_lv;
static int sl_lv_find(const char *key) { for (int i = 0; i < s_lv.n; i++) if (!strcmp(s_lv.k[i], key)) return i; return -1; }
static const char *sl_lv_str(const char *key) { const int i = sl_lv_find(key); return i >= 0 ? s_lv.v[i] : NULL; }
static float sl_lv_f(const char *key, float def) { const char *v = sl_lv_str(key); return v ? (float)atof(v) : def; }
static void sl_live_poll(void);
float dc_strafe_live_f(const char *key, float def) { sl_live_poll(); return sl_lv_f(key, def); }   /* main.cpp: turn_omega, brake, accel; 13.10 день (3) - сам перечитывает файл (в режиме 4 ног стрейфа нет) */
static struct { float phi, th, thv, won; uint32_t frame, spd_frame; float spdk; int init; int nc; int dq[9], kd[9];
                float ws, wsv; int active, hold, prev_sub, t0m_m; uint32_t t0m_key; float t0m[3]; float k; uint32_t k_frame;
                int v_game, v_have, v_owned; int hold_x, hold_z; float ks;
                int idle_ok; uint32_t idle_key; float idle_pose[24]; float rho;
                float base_yaw; int base_m; uint32_t base_key;
                int kv[2]; float lv[2]; int lx[2]; float te;               /* 13.10: боковой вариант «нога сзади» (справа, слева) и его доля */
                float thp; int thp_lag;                                     /* 13.10: поза на круге (ждёт на боку, пока вариант не тот) */
                int h3, h3s; float t3;                                      /* 13.10 день (3): режим 3, слева за боком - «влево» (h3s - от старта); время от старта шага */
                int h3r; float h3r_t, h3r_from;                             /* 13.10 поздний вечер (4): «влево» отпущено - поза к направлению плавно (время, откуда) */
                int hg;                                                     /* 13.10 поздний вечер (4): цель шага (после поворота корпуса) - в зоне «влево» */
                float pm, pmv, pmc; int pm_on, pm_reset;                    /* 13.10 поздний вечер (5), режим 6: курс таза позы под пружиной, поправка */
                float py_last; uint32_t py_f;                               /* курс таза нарисованной позы ног (прошлый кадр) */
                int vw, vwd, dipk;                                          /* 13.10 день (3): режим 3, поза ждёт вариант бока, а направление ушло (vwd - смена ещё не началась; dipk - шаг игры не ниже m3_wait_speed) */
                float tg, twa, twc, tw0, twt; int tws, twi; uint32_t twf;   /* 13.10 поздний вечер: клэмп таза от корпуса (tw_max), кванты: поворот позы
                                                                             * в этом такте (tg), выдано (twa), цель (twc), бленд от tw0 и его время (twt,
                                                                             * меньше 0 - нет), сторона за 180° (tws), готово (twi), такт (twf) */
                float hdy; int hdy_m, hdy_ok; uint32_t hdy_key; } s_sl;    /* 13.10: рост клипов под ходьбу текущего оружия */
#ifdef DCME_RELEASE   /* ME aim-walk tuning: our data from the program (dc_pak.c) */
#include "dc_pak.h"
#pragma push_macro("GetFileAttributesExA")
#undef GetFileAttributesExA
#pragma push_macro("FILE")
#undef FILE
#pragma push_macro("fopen")
#undef fopen
#pragma push_macro("fgets")
#undef fgets
#pragma push_macro("fread")
#undef fread
#pragma push_macro("fclose")
#undef fclose
#pragma push_macro("_stat64")
#undef _stat64
#pragma push_macro("_findfirst")
#undef _findfirst
#pragma push_macro("_findnext")
#undef _findnext
#pragma push_macro("_findclose")
#undef _findclose
#define GetFileAttributesExA(p, lvl, fa) dcme_file_attr((p), (fa))
#define FILE DcMemFile
#define fopen(p, m) dc_mf_open(p)
#define fgets(b, n, f) dc_mf_gets((b), (n), (f))
#define fread(d, s, c, f) dc_mf_read((d), (s), (c), (f))
#define fclose(f) dc_mf_close(f)
#define _stat64(p, st) dcme_stat64((p), (st))
#define _findfirst(p, fd) dcme_findfirst((p), (fd))
#define _findnext(h, fd) dcme_findnext((h), (fd))
#define _findclose(h) dcme_findclose(h)
#endif
static void sl_live_poll(void) {
    extern uint64_t s_frame_count; const uint32_t fr = (uint32_t)s_frame_count;
    if (s_lv.loaded && fr - s_lv.check < 30u) return;
    s_lv.check = fr;
    struct _stat64 st; long long mt = -1;
    if (_stat64("dc_strafe_live.cfg", &st) == 0) mt = (long long)st.st_mtime * 1000 + (long long)(st.st_size & 0x3FF);
    if (s_lv.loaded && mt == s_lv.mtime) return;
    s_lv.loaded = 1; s_lv.mtime = mt; s_lv.n = 0; s_sl.nc = 0;                 /* круг направлений - заново */
    FILE *f = mt >= 0 ? fopen("dc_strafe_live.cfg", "r") : NULL;
    char line[512], all[2048]; all[0] = 0;
    if (f) {
        while (fgets(line, sizeof line, f) && s_lv.n < SL_NLV) {
            /* 13.10 вечер: строка длиннее буфера (было 160 байт - 80 букв кириллицы) читалась кусками, и хвост комментария
             * «... m3_wait_dip = 0 - выкл» стал настройкой m3_wait_dip = 0 (первая находка выигрывает): нырок режима 3 был
             * выключен с 13.10 днём. Хвост длинной строки теперь пропускается */
            /* 13.10 поздний вечер (7): хвост читается через fgets - релиз берёт файл из программы (release_edits.py, шим загрузчика) */
            { const size_t L = strlen(line);
              if (L > 0 && line[L - 1] != '\n') { char tl[256];
                  while (fgets(tl, sizeof tl, f)) { const size_t T = strlen(tl); if (T > 0 && tl[T - 1] == '\n') break; } } }
            char *h = strchr(line, '#'); if (h) *h = 0;
            char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
            char *k = line, *v = eq + 1;
            while (*k == ' ' || *k == '\t') k++; for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            while (*v == ' ' || *v == '\t') v++; for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
            if (!*k) continue;
            snprintf(s_lv.k[s_lv.n], sizeof s_lv.k[0], "%s", k); snprintf(s_lv.v[s_lv.n], sizeof s_lv.v[0], "%s", v); s_lv.n++;
            if (strlen(all) + strlen(k) + strlen(v) + 3 < sizeof all) { strcat(all, k); strcat(all, "="); strcat(all, v); strcat(all, " "); }
        }
        fclose(f);
    }
    fprintf(stderr, "strafe live: %s %s%c", f ? "dc_strafe_live.cfg read:" : "no dc_strafe_live.cfg - defaults", all, 10);
}
#ifdef DCME_RELEASE   /* ME aim-walk tuning: end */
#undef GetFileAttributesExA
#pragma pop_macro("GetFileAttributesExA")
#undef FILE
#pragma pop_macro("FILE")
#undef fopen
#pragma pop_macro("fopen")
#undef fgets
#pragma pop_macro("fgets")
#undef fread
#pragma pop_macro("fread")
#undef fclose
#pragma pop_macro("fclose")
#undef _stat64
#pragma pop_macro("_stat64")
#undef _findfirst
#pragma pop_macro("_findfirst")
#undef _findnext
#pragma pop_macro("_findnext")
#undef _findclose
#pragma pop_macro("_findclose")
#endif
/* 12.10 поздно (игрок: «вперёд-влево и вправо - ноги скрещиваются и клипаются, подставь нормальные»): ЗЕРКАЛО клипа - имя
 * с приставкой m (mBL, mR45 ...): клип отражён в системе персонажа (x -> -x, ноги местами), то есть «вправо» = отражённое
 * «влево», которое игрока устраивает. Замер по клипам: у BR колени заходят друг за друга на 188 ед., у BL - 108; у L45 колени
 * сходятся до 21 ед., у R45 - 76; таз у BR развёрнут от прицела на 64°, у соседних направлений 13-41° - смесь в секторе
 * ломалась; у отражённого BL таз -21° (как у её ходьбы). */
/* (12.10 поздно, съёмка ног сзади отодвинутой камерой) на «вправо» чище всего FR набора: таз ровно к камере, колени не
 * заходят друг за друга (минимум 26 ед. между ними), голени сходятся на 1-2 кадрах; зеркало «влево» (mBL) - колени
 * перекрещиваются на 2-3 кадрах из 8. По умолчанию: вправо FR, вперёд-влево mR45, остальное - прежнее. */
#define SL_MIRROR 100
static int sl_clip_by_name(const char *v) {
    if (!v) return -9;
    if (!strcmp(v, "own_fwd")) return -1;
    if (!strcmp(v, "own_back")) return -2;
    for (int i = 0; i < SL_NCLIP; i++) if (!strcmp(v, SL_CLIPS[i].name)) return i;
    if (v[0] == 'm') for (int i = 0; i < SL_NCLIP; i++) if (!strcmp(v + 1, SL_CLIPS[i].name)) return SL_MIRROR + i;
    return -9;
}
static void sl_circle(void) {                     /* 8 направлений через 45°: назад (-2048) ... вперёд (0) ... назад (2048) */
    if (s_sl.nc) return;
    static const char *key[9] = { "dir.B", "dir.BL", "dir.L", "dir.FL", "dir.F", "dir.FR", "dir.R", "dir.BR", "dir.B" };
    /* 13.10.2026 (игрок: «в режиме 2 вправо-назад и влево-назад ноги скрещиваются, клипаются; возьми оригинальные анимации набора,
     * подгони только рост и скорость»): все направления - анимации набора как есть, без зеркал (перенос заново, tools/strafe_mocap:
     * стопы как в записи): вперёд-вправо R45, вбок FR / BR, назад-вправо R135; слева L45, FL / BL, L135. Вбок - ДВА варианта:
     * FR/FL (отстающая нога проходит спереди) - в смеси с диагональю вперёд, BR/BL (нога сзади) - с диагональю назад (dir.R_B,
     * dir.L_B). Смесь «нога спереди» с диагональю назад проводила голени друг сквозь друга (замер зазора: FR+R135 - 4..27 ед.,
     * BR+R135 - 119..125; режим 2 с доворотом кладёт «вправо-назад» как раз между ними). */
    static const char *def[9] = { "own_back", "L135", "FL", "L45", "own_fwd", "R45", "FR", "R135", "own_back" };
    for (int i = 0; i < 9; i++) {
        int c = sl_clip_by_name(sl_lv_str(key[i]));
        if (c == -9) c = sl_clip_by_name(def[i]);
        s_sl.dq[i] = -2048 + 512 * i; s_sl.kd[i] = c;
    }
    { int c = sl_clip_by_name(sl_lv_str("dir.R_B")); s_sl.kv[0] = c != -9 ? c : sl_clip_by_name("BR");
      c = sl_clip_by_name(sl_lv_str("dir.L_B"));     s_sl.kv[1] = c != -9 ? c : sl_clip_by_name("BL"); }
    s_sl.nc = 9;
}
static float sl_smooth(float t) { if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f; return t * t * (3.f - 2.f * t); }
static float sl_wq(float d) { while (d > 2048.f) d -= 4096.f; while (d < -2048.f) d += 4096.f; return d; }
/* пружина с критическим затуханием на углах/величинах: x к цели T, скорость v, жёсткость om, шаг dt (точное решение) */
static void sl_spring(float *x, float *v, float T, float om, float dt, int ang) {
    float e = *x - T; if (ang) e = sl_wq(e);
    const float ex = expf(-om * dt), c = *v + om * e;
    const float en = (e + c * dt) * ex; *v = (*v - om * c * dt) * ex; *x = T + en;
}
void dc_strafe_spd(float k) { extern uint64_t s_frame_count; s_sl.spdk = k; s_sl.spd_frame = (uint32_t)s_frame_count; }
static float sl_spdk_raw(void) { extern uint64_t s_frame_count; return ((uint32_t)s_frame_count - s_sl.spd_frame <= 3u) ? s_sl.spdk : 0.f; }
/* 12.10 поздно (игрок: «на коротких нажатиях нет интерполяции, рывки, анимации сбрасываются»): доля скорости для ног - не
 * прямо от main.cpp (там она обрывалась в 0, когда стрейф кончался, - ноги прыгали в стойку за такт), а сглаженная копия,
 * не быстрее 8 в секунду (разгон и тормоз main.cpp проходят как есть) */
static float sl_spdk(void) { return s_sl.ks; }
static int sl_seg(float th, float *t) {           /* отрезок круга с направлением th и доля в нём (ширина смеси - blend) */
    sl_circle();
    int i = 0; while (i + 2 < s_sl.nc && th >= (float)s_sl.dq[i + 1]) i++;
    float r = (th - (float)s_sl.dq[i]) / 512.f;
    float b = sl_lv_f("blend", 1.f); if (b < 0.05f) b = 0.05f; if (b > 1.f) b = 1.f;
    /* 12.10 ночь: при полной смеси (blend 1) доли - по прямой: смесь клипов идёт ровно по направлению между ними, поворот ног
     * под шаг остаётся постоянным (сглаженные доли расходились с направлением до ±4° - таз качался при вращении стика) */
    { float u = (r - 0.5f) / b + 0.5f; if (u < 0.f) u = 0.f; if (u > 1.f) u = 1.f; *t = b >= 0.999f ? u : sl_smooth(u); }
    return i;
}
static float sl_back_w(float th) { float t; const int i = sl_seg(th, &t); return (s_sl.kd[i] == -2 ? 1.f - t : 0.f) + (s_sl.kd[i + 1] == -2 ? t : 0.f); }
/* поза ног: [0..2] положение узла 0 (смещение корня), [3..5] таз в мире персонажа (углы), [6..23] локальные углы узлов 9..14 */
static void sl_lerp(const float *a, const float *b, float t, float *o) {
    for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
    for (int i = 3; i < 24; i++) o[i] = a[i] + sl_wq(b[i] - a[i]) * t;
}
/* 12.10 ночь (игрок: «каждый раз дрыжки - неровная интерполяция в её позы и между каждой анимацией ходьбы; ноги, таз»): смесь
 * РАЗНЫХ поз (клип с клипом, с её ходьбой, со стойкой прицела, с позой игры) - через кватернионы: каждый сустав идёт кратчайшей
 * дугой с ровной скоростью (slerp). Раньше три угла Эйлера смешивались по отдельности - при больших различиях (шаг вбок и стойка
 * с широко расставленными ногами) кость шла кривой дугой, то быстрее, то медленнее. Соседние кадры одного клипа - как было. */
static void sl_eulf(const float *e, float *m) {                 /* углы Эйлера (кванты, дробные) -> матрица, как ao_euler */
    const float cx = cosf(e[0] * AO_ANG), sx = sinf(e[0] * AO_ANG), cy = cosf(e[1] * AO_ANG), sy = sinf(e[1] * AO_ANG);
    const float cz = cosf(e[2] * AO_ANG), sz = sinf(e[2] * AO_ANG);
    const float rx[9] = {1,0,0, 0,cx,-sx, 0,sx,cx}, ry[9] = {cy,0,sy, 0,1,0, -sy,0,cy}, rz[9] = {cz,-sz,0, sz,cz,0, 0,0,1};
    float t[9]; ao_mul(rx, ry, t); ao_mul(t, rz, m);
}
static void sl_decf(const float *m, float *e) {                 /* матрица -> углы Эйлера (кванты, дробные), как ao_decompose */
    float sy = m[2]; if (sy > 1.f) sy = 1.f; if (sy < -1.f) sy = -1.f;
    e[1] = asinf(sy) / AO_ANG;
    if (fabsf(m[2]) < 0.999999f) { e[0] = atan2f(-m[5], m[8]) / AO_ANG; e[2] = atan2f(-m[1], m[0]) / AO_ANG; }
    else { e[0] = atan2f(m[7], m[4]) / AO_ANG; e[2] = 0.f; }
}
static void sl_m2q(const float *m, float *q) {                  /* матрица -> кватернион (w, x, y, z) */
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.f) { const float s = sqrtf(tr + 1.f) * 2.f; q[0] = 0.25f * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { const float s = sqrtf(1.f + m[0] - m[4] - m[8]) * 2.f; q[0] = (m[7] - m[5]) / s; q[1] = 0.25f * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { const float s = sqrtf(1.f + m[4] - m[0] - m[8]) * 2.f; q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25f * s; q[3] = (m[5] + m[7]) / s; }
    else { const float s = sqrtf(1.f + m[8] - m[0] - m[4]) * 2.f; q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25f * s; }
}
static void sl_q2m(const float *q, float *m) {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1.f - 2.f * (y*y + z*z); m[1] = 2.f * (x*y - w*z);       m[2] = 2.f * (x*z + w*y);
    m[3] = 2.f * (x*y + w*z);       m[4] = 1.f - 2.f * (x*x + z*z); m[5] = 2.f * (y*z - w*x);
    m[6] = 2.f * (x*z - w*y);       m[7] = 2.f * (y*z + w*x);       m[8] = 1.f - 2.f * (x*x + y*y);
}
static void sl_blend(const float *a, const float *b, float t, float *o) {   /* поза ног: корень - по прямой, повороты - slerp */
    if (t <= 0.f) { if (o != a) memcpy(o, a, 24 * sizeof(float)); return; }
    if (t >= 1.f) { if (o != b) memcpy(o, b, 24 * sizeof(float)); return; }
    float r[24];
    for (int i = 0; i < 3; i++) r[i] = a[i] + (b[i] - a[i]) * t;
    for (int j = 0; j < 7; j++) {
        const float *ea = a + 3 + 3 * j, *eb = b + 3 + 3 * j;
        float ma[9], mb[9], qa[4], qb[4], q[4], m[9], e[3];
        sl_eulf(ea, ma); sl_eulf(eb, mb); sl_m2q(ma, qa); sl_m2q(mb, qb);
        float d = qa[0]*qb[0] + qa[1]*qb[1] + qa[2]*qb[2] + qa[3]*qb[3];
        if (d < 0.f) { d = -d; for (int k = 0; k < 4; k++) qb[k] = -qb[k]; }
        float wa = 1.f - t, wb = t;
        if (d < 0.9995f) { const float th = acosf(d), s = sinf(th); wa = sinf((1.f - t) * th) / s; wb = sinf(t * th) / s; }
        float n = 0.f; for (int k = 0; k < 4; k++) { q[k] = qa[k] * wa + qb[k] * wb; n += q[k] * q[k]; }
        n = sqrtf(n); if (n < 1e-8f) n = 1.f; for (int k = 0; k < 4; k++) q[k] /= n;
        sl_q2m(q, m); sl_decf(m, e);
        for (int k = 0; k < 3; k++) { const float lin = ea[k] + sl_wq(eb[k] - ea[k]) * t; r[3 + 3 * j + k] = lin + sl_wq(e[k] - lin); }
    }
    memcpy(o, r, sizeof r);
}
/* нормированная фаза (0 - постановка правой, 0.5 - левой) -> кадр клипа с метками tr, tl из nf */
static float sl_ph2frame(float ph, int tr, int tl, int nf) {
    ph -= floorf(ph);
    const float l1 = (float)(((tl - tr) % nf + nf) % nf), l2 = (float)nf - l1;
    float f = ph < 0.5f ? (float)tr + ph * 2.f * l1 : (float)tl + (ph - 0.5f) * 2.f * l2;
    f -= floorf(f / (float)nf) * (float)nf;
    return f;
}
static float sl_frame2ph(float f, int tr, int tl, int nf) {
    const float l1 = (float)(((tl - tr) % nf + nf) % nf), l2 = (float)nf - l1;
    float a = f - (float)tr; a -= floorf(a / (float)nf) * (float)nf;
    return a < l1 ? 0.5f * a / l1 : 0.5f + 0.5f * (a - l1) / l2;
}
/* зеркало позы ног в системе персонажа (x вправо): корень x -> -x, повороты R -> M R M (M = diag(-1,1,1)) = углы (a,-b,-c),
 * правая нога <-> левая (смещения суставов скелета симметричны) */
static void sl_mirror(float *p) {
    p[0] = -p[0]; p[4] = -p[4]; p[5] = -p[5];
    float t[9]; memcpy(t, p + 6, sizeof t); memcpy(p + 6, p + 15, sizeof t); memcpy(p + 15, t, sizeof t);
    for (int k = 0; k < 6; k++) { p[6 + 3 * k + 1] = -p[6 + 3 * k + 1]; p[6 + 3 * k + 2] = -p[6 + 3 * k + 2]; }
}
static void sl_clip_pose(int c, float ph, float *o) {
    const int mir = c >= SL_MIRROR; if (mir) c -= SL_MIRROR;
    const SlClip *C = &SL_CLIPS[c];
    char key[40]; snprintf(key, sizeof key, "phase.%s%s", mir ? "m" : "", C->name);
    /* у зеркала правая стопа - левая исходного: метки постановки меняются местами */
    float f = sl_ph2frame(ph, mir ? C->td_l : C->td_r, mir ? C->td_r : C->td_l, SL_NF) + sl_lv_f(key, 0.f);
    f -= floorf(f / (float)SL_NF) * (float)SL_NF;
    const int i0 = (int)f % SL_NF, i1 = (i0 + 1) % SL_NF; const float t = f - floorf(f);
    float a[24], b[24];
    for (int i = 0; i < 24; i++) { a[i] = (float)C->f[i0][i]; b[i] = (float)C->f[i1][i]; }
    sl_lerp(a, b, t, o);
    if (mir) sl_mirror(o);
}
static void sl_from_frame(const uint8_t *fr, float *o) {       /* кадр моушена (80 байт): корень + 45 углов -> поза ног */
    int ang[45]; ao_unpack45(fr, ang);
    for (int i = 0; i < 3; i++) o[i] = (float)*(const int16_t *)(fr + 2 * i);
    float L0[9], L8[9], W8[9]; int q[3];
    ao_euler(ang[0], ang[1], ang[2], L0); ao_euler(ang[24], ang[25], ang[26], L8); ao_mul(L0, L8, W8); ao_decompose(W8, q);
    for (int i = 0; i < 3; i++) o[3 + i] = (float)q[i];
    for (int i = 0; i < 18; i++) o[6 + i] = (float)ang[27 + i];
}
static int sl_own_pose(uint8_t *ram, int m, int back, float ph, float *o) {
    if (m < 0 || m >= s_ao_nmot || s_ao_mots[m].nf < 16 || s_ao_mots[m].nf > 64) return 0;
    const int nf = s_ao_mots[m].nf;
    const int tr = back ? SL_OWN_TD_BACK : SL_OWN_TD_FWD, tl = back ? SL_OWN_TDL_BACK : SL_OWN_TDL_FWD;
    float f = sl_ph2frame(ph, tr % nf, tl % nf, nf) + sl_lv_f(back ? "phase.own_back" : "phase.own_fwd", 0.f);
    f -= floorf(f / (float)nf) * (float)nf;
    const int i0 = (int)f % nf, i1 = (i0 + 1) % nf; const float t = f - floorf(f);
    float a[24], b[24];
    sl_from_frame(ram + AP_BLOCK + s_ao_mots[m].data + (uint32_t)i0 * 80u, a);
    sl_from_frame(ram + AP_BLOCK + s_ao_mots[m].data + (uint32_t)i1 * 80u, b);
    sl_lerp(a, b, t, o); return 1;
}
/* ноги шире (игрок: «на вперёд-влево и вправо ноги пересекаются, клипаются одна в другую»): бедро поворачивается наружу
 * вокруг оси «вперёд» персонажа на spread.<имя> (или spread) градусов - стопы расходятся, без скольжения (сдвиг постоянный) */
static void sl_spread(float *p, float deg) {
    if (fabsf(deg) < 0.01f) return;
    float W8[9], W8T[9]; ao_euler((int)lroundf(p[3]) & 4095, (int)lroundf(p[4]) & 4095, (int)lroundf(p[5]) & 4095, W8);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) W8T[r*3+c] = W8[c*3+r];
    static const float fwd[3] = { 0.f, 0.f, 1.f };
    for (int i = 0; i < 2; i++) {                                  /* 0 - правая (+x, наружу - минус по Rz), 1 - левая */
        float Rz[9], L9[9], t1[9], t2[9], t3[9]; int q[3];
        ao_axis_rot(fwd, i == 0 ? -deg : deg, Rz);
        float *a = p + 6 + i * 9;
        ao_euler((int)lroundf(a[0]) & 4095, (int)lroundf(a[1]) & 4095, (int)lroundf(a[2]) & 4095, L9);
        ao_mul(W8, L9, t1); ao_mul(Rz, t1, t2); ao_mul(W8T, t2, t3); ao_decompose(t3, q);
        for (int c = 0; c < 3; c++) { float d = sl_wq((float)q[c] - a[c]); a[c] += d; }
    }
}
/* поворот позы ног (корень и таз; ноги - от таза) вокруг вертикали персонажа на q квантов, + вправо */
static void sl_yaw_pose(float *p, float q) {
    if (fabsf(q) < 0.5f) return;
    const float an = q * AO_ANG, c = cosf(an), s = sinf(an);
    const float x = p[0], z = p[2]; p[0] = c * x + s * z; p[2] = -s * x + c * z;
    float W8[9], Ry[9], W[9]; int qq[3];
    ao_euler((int)lroundf(p[3]) & 4095, (int)lroundf(p[4]) & 4095, (int)lroundf(p[5]) & 4095, W8);
    ao_euler(0, (int)lroundf(q) & 4095, 0, Ry); ao_mul(Ry, W8, W); ao_decompose(W, qq);
    for (int i = 0; i < 3; i++) p[3 + i] += sl_wq((float)qq[i] - p[3 + i]);
}
/* курс таза позы (кванты, + вправо): куда смотрит его ось «вперёд» */
static float sl_pose_yaw(const float *p) {
    float W[9]; ao_euler((int)lroundf(p[3]) & 4095, (int)lroundf(p[4]) & 4095, (int)lroundf(p[5]) & 4095, W);
    return atan2f(W[2], W[8]) / AO_ANG;
}
/* средний курс таза клипа (по 34 кадрам; зеркальный - после отражения), раз на клип */
static float sl_clip_yaw(int kd) {
    static float y[2][SL_NCLIP]; static int ok[2][SL_NCLIP];
    const int mir = kd >= SL_MIRROR, c = mir ? kd - SL_MIRROR : kd;
    if (c < 0 || c >= SL_NCLIP) return 0.f;
    if (!ok[mir][c]) {
        float cs = 0.f, sn = 0.f;
        for (int k = 0; k < SL_NF; k++) { float p[24]; for (int i = 0; i < 24; i++) p[i] = (float)SL_CLIPS[c].f[k][i];
            if (mir) sl_mirror(p);
            const float a = sl_pose_yaw(p) * AO_ANG; cs += cosf(a); sn += sinf(a); }
        y[mir][c] = atan2f(sn, cs) / AO_ANG; ok[mir][c] = 1;
    }
    return y[mir][c];
}
/* средний курс таза её ходьбы с оружием (моушен wm) - от него отсчитывается разворот таза клипов */
static float sl_base_yaw(uint8_t *ram, int wm) {
    if (wm < 0 || wm >= s_ao_nmot || s_ao_mots[wm].nf < 16 || s_ao_mots[wm].nf > 64) return -9999.f;
    if (s_sl.base_m != wm || s_sl.base_key != s_ao_ext.key) {
        float cs = 0.f, sn = 0.f;
        for (int k = 0; k < s_ao_mots[wm].nf; k++) { float p[24]; sl_from_frame(ram + AP_BLOCK + s_ao_mots[wm].data + (uint32_t)k * 80u, p);
            const float a = sl_pose_yaw(p) * AO_ANG; cs += cosf(a); sn += sinf(a); }
        s_sl.base_yaw = atan2f(sn, cs) / AO_ANG; s_sl.base_m = wm; s_sl.base_key = s_ao_ext.key;
    }
    return s_sl.base_yaw;
}
/* таз повернуть на dq квантов вокруг вертикали, бёдра - обратно (их поворот в мире прежний: ноги идут туда же, суставы бёдер
 * сдвигаются на постоянную величину - стопы не скользят) */
static void sl_twist(float *p, float dq) {
    if (fabsf(dq) < 0.5f) return;
    float W8[9], Ry[9], W8n[9], W8nT[9], A[9]; int q[3];
    ao_euler((int)lroundf(p[3]) & 4095, (int)lroundf(p[4]) & 4095, (int)lroundf(p[5]) & 4095, W8);
    ao_euler(0, (int)lroundf(dq) & 4095, 0, Ry); ao_mul(Ry, W8, W8n);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) W8nT[r*3+c] = W8n[c*3+r];
    ao_mul(W8nT, W8, A);
    for (int i = 0; i < 2; i++) {
        float *a = p + 6 + i * 9; float L[9], t1[9];
        ao_euler((int)lroundf(a[0]) & 4095, (int)lroundf(a[1]) & 4095, (int)lroundf(a[2]) & 4095, L);
        ao_mul(A, L, t1); ao_decompose(t1, q);
        for (int c = 0; c < 3; c++) a[c] += sl_wq((float)q[c] - a[c]);
    }
    ao_decompose(W8n, q); for (int c = 0; c < 3; c++) p[3 + c] += sl_wq((float)q[c] - p[3 + c]);
}
/* 13.10.2026: НОГИ - К ЗАДАННЫМ ЛОДЫЖКАМ (двухзвенный IK, система персонажа). Корень и таз позы уже на месте; колено гнётся вокруг
 * оси X бедра (как у её ходьбы и клипов), бедро - наименьшим поворотом к цели, стопа сохраняет поворот в мире. Им держатся стопы
 * клипа на месте, когда таз доворачивается (pelvis_twist - раньше стопы уезжали с бёдрами на ±30 ед.) и когда корень опускается
 * под рост её ходьбы с другим оружием. */
static void sl_leg_points(const float *p, float A[2][3]) {      /* лодыжки позы */
    float W8[9]; sl_eulf(p + 3, W8);
    const float P8[3] = { p[0], p[1] - 7.f, p[2] };
    for (int s = 0; s < 2; s++) {
        const float *o = p + 6 + 9 * s;
        float L9[9], L10[9], W9[9], W10[9], v[3], H[3], K[3];
        sl_eulf(o, L9); sl_eulf(o + 3, L10);
        ao_mv(W8, AO_BONES[9 + 3 * s].off, v); for (int i = 0; i < 3; i++) H[i] = P8[i] + v[i];
        ao_mul(W8, L9, W9); ao_mv(W9, AO_BONES[10 + 3 * s].off, v); for (int i = 0; i < 3; i++) K[i] = H[i] + v[i];
        ao_mul(W9, L10, W10); ao_mv(W10, AO_BONES[11 + 3 * s].off, v); for (int i = 0; i < 3; i++) A[s][i] = K[i] + v[i];
    }
}
static void sl_leg_ik(float *p, const float A[2][3]) {
    float W8[9], W8T[9]; sl_eulf(p + 3, W8); ao_tr(W8, W8T);
    const float P8[3] = { p[0], p[1] - 7.f, p[2] };
    for (int s = 0; s < 2; s++) {
        float *o = p + 6 + 9 * s;
        float L9[9], L10[9], L11[9], W9[9], W10[9], W11[9], v[3], H[3];
        sl_eulf(o, L9); sl_eulf(o + 3, L10); sl_eulf(o + 6, L11);
        ao_mv(W8, AO_BONES[9 + 3 * s].off, v); for (int i = 0; i < 3; i++) H[i] = P8[i] + v[i];
        ao_mul(W8, L9, W9); ao_mul(W9, L10, W10); ao_mul(W10, L11, W11);
        const float *k = AO_BONES[10 + 3 * s].off; float u[3]; ao_mv(L10, AO_BONES[11 + 3 * s].off, u);
        const float dv[3] = { A[s][0] - H[0], A[s][1] - H[1], A[s][2] - H[2] };
        const float d = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]); if (d < 1.f) continue;
        /* сгиб колена: |k + Rx(dl) u| = d  ->  a cos(dl) + b sin(dl) = c */
        const float kk = k[0] * k[0] + k[1] * k[1] + k[2] * k[2], uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
        const float c = (d * d - kk - uu) * 0.5f - k[0] * u[0];
        const float a = k[1] * u[1] + k[2] * u[2], b = k[2] * u[1] - k[1] * u[2];
        const float rr = sqrtf(a * a + b * b); if (rr < 1e-3f) continue;
        float cc = c / rr; if (cc > 1.f) cc = 1.f; if (cc < -1.f) cc = -1.f;
        const float ph = atan2f(b, a), al = acosf(cc);
        float d1 = ph + al, d2 = ph - al;
        while (d1 > 3.14159265f) d1 -= 6.2831853f; while (d1 < -3.14159265f) d1 += 6.2831853f;
        while (d2 > 3.14159265f) d2 -= 6.2831853f; while (d2 < -3.14159265f) d2 += 6.2831853f;
        const float dl = fabsf(d1) < fabsf(d2) ? d1 : d2;
        const float cs = cosf(dl), sn = sinf(dl);
        const float Rx[9] = { 1.f, 0.f, 0.f, 0.f, cs, -sn, 0.f, sn, cs };
        float L10n[9], t[3], vw[3]; ao_mul(Rx, L10, L10n);
        ao_mv(L10n, AO_BONES[11 + 3 * s].off, t); for (int i = 0; i < 3; i++) t[i] += k[i];
        ao_mv(W9, t, vw);
        const float nv = sqrtf(vw[0] * vw[0] + vw[1] * vw[1] + vw[2] * vw[2]); if (nv < 1.f) continue;
        const float a1[3] = { vw[0] / nv, vw[1] / nv, vw[2] / nv }, b1[3] = { dv[0] / d, dv[1] / d, dv[2] / d };
        float Rm[9], W9n[9], L9n[9], W10n[9], W10nT[9], L11n[9];
        sf_rmin(a1, b1, Rm); ao_mul(Rm, W9, W9n); ao_mul(W8T, W9n, L9n); ao_mul(W9n, L10n, W10n); ao_tr(W10n, W10nT); ao_mul(W10nT, W11, L11n);
        const float *Ls[3] = { L9n, L10n, L11n };
        for (int j = 0; j < 3; j++) { float e[3]; sl_decf(Ls[j], e); for (int c3 = 0; c3 < 3; c3++) o[3 * j + c3] += sl_wq(e[c3] - o[3 * j + c3]); }
    }
}
/* 13.10.2026 (игрок: «с пистолетом ты подогнал - на чужих анимациях Регина не выше; с дробовиком, гранатомётом, пистолетом в двух
 * руках - привстаёт; выровняй по росту оригинальных, во всех режимах с чужими анимациями»): клипы подогнаны по высоте корня к её
 * ходьбе с пистолетом (SL_REF_Y); с другим оружием её ходьба ниже (дробовик M18: корень -1094 против -1115, стойки -1075) - корень
 * клипа опускается на разницу средних высот корня её ходьбы с этим оружием, ноги - IK к прежним стопам. height_fit 0 - выкл. */
static float sl_height_dy(uint8_t *ram, int wm) {
    if (wm < 0 || wm >= s_ao_nmot || s_ao_mots[wm].nf < 16 || s_ao_mots[wm].nf > 64) return 0.f;
    if (!s_sl.hdy_ok || s_sl.hdy_m != wm || s_sl.hdy_key != s_ao_ext.key) {
        float sy = 0.f;
        for (int k = 0; k < s_ao_mots[wm].nf; k++) sy += (float)*(const int16_t *)(ram + AP_BLOCK + s_ao_mots[wm].data + (uint32_t)k * 80u + 2u);
        sy /= (float)s_ao_mots[wm].nf;
        s_sl.hdy = sy - (float)SL_REF_Y; s_sl.hdy_m = wm; s_sl.hdy_key = s_ao_ext.key; s_sl.hdy_ok = 1;
        fprintf(stderr, "aimwalk: strafe legs height - her walk M%02d root %.0f, the set's clips are fitted to %d: %s by %.0f%c",
                wm, sy, SL_REF_Y, s_sl.hdy >= 0.f ? "lowered" : "raised", fabsf(s_sl.hdy), 10);
    }
    return s_sl.hdy * sl_lv_f("height_fit", 1.f);
}
static int sl_node_pose(uint8_t *ram, int kd, int wm, float ph, float *o) {
    if (kd >= 0) { sl_clip_pose(kd, ph, o);
                   const int mir = kd >= SL_MIRROR;
                   char key[40]; snprintf(key, sizeof key, "spread.%s%s", mir ? "m" : "", SL_CLIPS[mir ? kd - SL_MIRROR : kd].name);
                   sl_spread(o, sl_lv_f(key, sl_lv_f("spread", 0.f)));
                   float A0[2][3]; sl_leg_points(o, A0);                   /* 13.10: стопы клипа держатся на месте (ниже - IK) */
                   /* 12.10 ночь (игрок: «таз слишком сильно закручиваешь»): свой разворот таза клипа относительно её ходьбы
                    * (у шага вбок набора - до 43°) - долей pelvis_twist (0.4; 1 - как в клипе), качание таза в шаге - как есть */
                   /* 13.10: доля - по анимации (pelvis_twist.<имя>, иначе общая); у BL (влево, нога сзади) доворот таза при стопах на
                    * месте сводит колени (зазор бёдер: как в записи 116, при 0.4 - 48) - у неё таз почти как в записи (0.9: 106) */
                   { char tk[40]; snprintf(tk, sizeof tk, "pelvis_twist.%s%s", mir ? "m" : "", SL_CLIPS[mir ? kd - SL_MIRROR : kd].name);
                     const float twd = (!mir && !strcmp(SL_CLIPS[kd].name, "BL")) ? 0.9f : sl_lv_f("pelvis_twist", 0.4f);
                     const float tw = sl_lv_f(tk, twd), base = sl_base_yaw(ram, wm);
                     if (tw < 0.999f && base > -9000.f) sl_twist(o, -(1.f - tw) * sl_wq(sl_clip_yaw(kd) - base)); }
                   o[1] += sl_height_dy(ram, wm);                         /* 13.10: рост - под ходьбу текущего оружия (Y вниз) */
                   if (sl_lv_f("foot_lock", 1.f) != 0.f) sl_leg_ik(o, A0);
                   return 1; }
    return kd == -1 ? sl_own_pose(ram, wm, 0, ph, o) : sl_own_pose(ram, wm + 1, 1, ph, o);
}
/* 13.10 поздний вечер (5): средний курс таза позы узла круга (кванты, система персонажа, + вправо) - как его строит sl_node_pose:
 * у клипа набора - курс её ходьбы плюс доля pelvis_twist его разворота, у её ходьбы вперёд и назад - их собственный средний */
static float sl_own_mean_yaw(uint8_t *ram, int m) {
    static float y = 0.f; static int ym = -1; static uint32_t yk = 0;
    if (m < 0 || m >= s_ao_nmot || s_ao_mots[m].nf < 16 || s_ao_mots[m].nf > 64) return -9999.f;
    if (ym != m || yk != s_ao_ext.key) {
        float cs = 0.f, sn = 0.f;
        for (int k = 0; k < s_ao_mots[m].nf; k++) { float p[24]; sl_from_frame(ram + AP_BLOCK + s_ao_mots[m].data + (uint32_t)k * 80u, p);
            const float a = sl_pose_yaw(p) * AO_ANG; cs += cosf(a); sn += sinf(a); }
        y = atan2f(sn, cs) / AO_ANG; ym = m; yk = s_ao_ext.key;
    }
    return y;
}
static float sl_node_mean_yaw(uint8_t *ram, int kd, int wm) {
    const float base = sl_base_yaw(ram, wm);
    if (base < -9000.f) return 0.f;
    if (kd == -2) { const float yb = sl_own_mean_yaw(ram, wm + 1); return yb < -9000.f ? base : yb; }
    if (kd < 0) return base;
    const int mir = kd >= SL_MIRROR; const char *nm = SL_CLIPS[mir ? kd - SL_MIRROR : kd].name;
    char tk[40]; snprintf(tk, sizeof tk, "pelvis_twist.%s%s", mir ? "m" : "", nm);
    const float tw = sl_lv_f(tk, (!mir && !strcmp(nm, "BL")) ? 0.9f : sl_lv_f("pelvis_twist", 0.4f));
    const float cy = sl_clip_yaw(kd);
    return tw < 0.999f ? base + tw * sl_wq(cy - base) : cy;
}
static void sl_live_pose(const uint8_t *ram, float *o) {       /* как её построила игра (свежие локальные матрицы на входе) */
    const int16_t *rp = (const int16_t *)(ram + LAD_NODE0 + 68u);
    for (int i = 0; i < 3; i++) o[i] = (float)rp[i];
    float L0[9], L8[9], W8[9]; int q[3];
    sf_load((const int16_t *)(ram + AP_NODES), L0); sf_load((const int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * 8u), L8);
    ao_mul(L0, L8, W8); ao_decompose(W8, q); for (int i = 0; i < 3; i++) o[3 + i] = (float)q[i];
    for (int b = 9; b <= 14; b++) { float L[9]; sf_load((const int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)b), L);
        ao_decompose(L, q); for (int i = 0; i < 3; i++) o[6 + (b - 9) * 3 + i] = (float)q[i]; }
}
static void sl_apply(uint8_t *ram, const float *p) {
    int16_t *rp = (int16_t *)(ram + LAD_NODE0 + 68u);
    for (int k = 0; k < 3; k++) s_sf.rsaved[k] = rp[k];
    for (int k = 0; k < 3; k++) rp[k] = (int16_t)lroundf(p[k]);
    for (int k = 0; k < 3; k++) s_sf.rwrote[k] = rp[k];
    s_sf.rapplied = 1;
    float L0[9], L0T[9], W8[9], L8[9];
    sf_load((const int16_t *)(ram + AP_NODES), L0);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) L0T[r*3+c] = L0[c*3+r];
    ao_euler((int)lroundf(p[3]) & 4095, (int)lroundf(p[4]) & 4095, (int)lroundf(p[5]) & 4095, W8); ao_mul(L0T, W8, L8);
    int16_t *P8 = (int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * 8u);
    memcpy(s_sf.psaved, P8, sizeof s_sf.psaved); sf_store(L8, P8); memcpy(s_sf.pwrote, P8, sizeof s_sf.pwrote); s_sf.papplied = 1;
    for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) {
        int16_t *Ln = (int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)(9 + 3 * i + j));
        memcpy(s_sf.gsaved[i][j], Ln, sizeof s_sf.gsaved[i][j]);
        float L[9]; const float *a = p + 6 + (i * 3 + j) * 3;
        ao_euler((int)lroundf(a[0]) & 4095, (int)lroundf(a[1]) & 4095, (int)lroundf(a[2]) & 4095, L); sf_store(L, Ln);
        memcpy(s_sf.gwrote[i][j], Ln, sizeof s_sf.gwrote[i][j]);
    }
    s_sf.gapplied = 1;
}
/* СКОРОСТЬ - ИГРЕ ДО ШАГА (12.10 поздно, рывки в сессии игрока 21:37): поправка шага на отрисовке шла ПОСЛЕ камеры, камера
 * видела шаг игры, а Регина стояла по-нашему - на нажатии/отпускании она прыгала в кадре на 4-10°, на развороте камера уезжала
 * на ~9 см. Теперь шаг игры за такт (0xB92D0 - корень моушена за кадр, его пишет анимация перед шагом) масштабируется в самой
 * записи (memory.c dc_speed_spike): игра делает уже наш шаг - с коллизией, камера и картинка видят одно место. */
int dc_strafe_speed_scale(int v, int *out) {
    extern uint64_t s_frame_count;
    s_sl.v_game = v; s_sl.v_have = 1;                          /* игра пишет скорость не каждый такт - её значение помним */
    if ((uint32_t)s_frame_count - s_sl.k_frame > 4u) return 0;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram || ram[0xB92C8] != 1) return 0;
    *out = (int)lroundf((float)v * s_sl.k); s_sl.v_owned = 1;
    return 1;
}
/* корпус, нарисованный стрейфом (курс на перекрестие), пока стрейф рисуется - для камеры (memory.c): её точка слежения - тело
 * столкновений 0xB93C8, оно на ~36 позади и ~10 левее по курсу ЛОГИКИ; в режиме 1 курс логики - направление шага, и камера
 * уезжала вбок при каждой смене направления (опыт: курс на месте поворачивается - точка обегает Регину) */
/* остановка стрейфа: игра ещё 2-3 такта двигает её шагом остановки (до 90 ед.), уже по курсу на прицел, - место держится
 * (memory.c, до камеры: камера и картинка видят одно место), пока наши ноги стоят в стойке */
int dc_strafe_hold_pos(int *x, int *z) {
    uint8_t *ram = memory_get_ram_ptr();
    if (s_sl.hold <= 0 || !ram || ram[0xB92C8] != 1 || ram[0xB92C9] != 6 || ram[0xB92CA] == 5) return 0;
    *x = s_sl.hold_x; *z = s_sl.hold_z; return 1;
}
int dc_strafe_body_yaw(int *yaw) {
    extern uint64_t s_frame_count;
    if ((uint32_t)s_frame_count - s_sf.frame > 3u) return 0;
    *yaw = s_sf.aim_q & 4095; return 1;
}
static void sl_legs(uint8_t *ram, int fresh) {
    extern uint64_t s_frame_count; const uint32_t fr = (uint32_t)s_frame_count;
    sl_live_poll();
    const int sub = ram[0xB92CA], act = ram[0xB92C9];
    const float thb = sl_wq((float)(((s_sf.legs_q - s_sf.aim_q + 2048) & 4095) - 2048));   /* движение от корпуса, + вправо */
    /* 12.10 ночь (игрок: «перебираешь направления или крутишь стик - таз бесится, рывки и дрожание»): при узкой смеси (0.5) клип
     * на каждых 45° менялся за 2-3 такта, а поворот ног под шаг прыгал от -12° до +12°. Теперь направление на круге клипов - от
     * «камеры» (корпус минус смещение перекрестия, main.cpp dc_strafe_legs_frame): клавиши попадают ровно в узлы круга - на них
     * чистый клип и при широкой смеси (blend 1), стик меняет клип плавно, ноги довёрнуты на постоянные ~10°. legs_cam 0 - как было */
    const float daim = sl_lv_f("legs_cam", 1.f) != 0.f ? (float)s_sf.lf_q : 0.f;
    const float thq = sl_wq(thb + daim);
    /* 1. её походка в наборе оружия (как у режима 3): моушен ходьбы и фаза по кадру, который играет игра */
    int wm = -1;
    if (m3_pose(ram)) wm = s_m3.learned ? s_m3.walk_m : s_ao_ext.mot[0] - 4;
    float dt = 0.f;
    if (fr != s_sl.frame) {
        const uint32_t df = s_sl.frame ? fr - s_sl.frame : 2u; s_sl.frame = fr;
        dt = (float)(df > 8u ? 8u : df) / 60.f;
        /* доля скорости ног: пока шаг идёт (main.cpp присылает долю - разгон, торможение перед разворотом) - не быстрее 8 в
         * секунду, ноги успевают за торможением и разворачиваются уже в стойке; шаг кончился - к стойке не быстрее stance_rate
         * (4 - за ~0.25 с, ровно) */
        /* 12.10 ночь (быстрое нажатие в обратную сторону - ноги ещё наполовину шли прежним шагом и прокручивались на 180° через
         * задние клипы уже на ходу, рывки до 26°): разворот направления ног больше 100° - сначала ноги к стойке (доля скорости
         * к нулю, шаг игры тоже), там направление сменяется сразу, потом разгон по новому. */
        const int walking = fresh && sub == 5;
        /* 13.10 день (3), режим 3: и когда поза ждёт на боку смены варианта, а направление ушло от бока дальше m3_wait_dip (30°) -
         * ноги тормозят, как на развороте (ниже, в конце позы на круге): ожидание кончается фазой шага или стойкой, поворот ног не
         * растёт (было: назад-влево -> назад ждало до 0.8 с, поворот ног до 90°, таз на 112° от корпуса). Смена варианта вне своей
         * фазы шага (пробовал) проводит голени друг сквозь друга (зазор 0). Тормоз - только пока смена не началась (vwd), направление
         * ног стоит, пока она не кончилась (vw) */
        const int flip = walking && s_sl.init && (fabsf(sl_wq(thq - s_sl.th)) > 1138.f || s_sl.vwd);
        if (walking && s_sl.vwd) s_sl.dipk = 1;
        else if (!walking || s_sl.ks >= sl_lv_f("m3_wait_speed", 0.5f) || fabsf(sl_wq(thq - s_sl.th)) > 1138.f) s_sl.dipk = 0;
        /* 13.10 ночь (игрок: «в 1 быстро перебираешь направления - резкие переходы, анимация сбивается»): разворот ног нырял в
         * стойку за 2 такта, не доходил до неё (смена стороны при 10% шага) и за 2 такта выныривал - рывки 15-25°. Теперь вниз -
         * flip_rate (4 в секунду), сторона меняется в стойке (доля шага 3%), разгон - не быстрее rise_rate (5 в секунду). */
        { const int spd_fresh = (uint32_t)s_frame_count - s_sl.spd_frame <= 3u;
          const float sr = sl_lv_f("stance_rate", 4.f), rr = sl_lv_f("rise_rate", 4.f), fr4 = sl_lv_f("flip_rate", 4.f);
          const float tgt = flip ? 0.f : sl_spdk_raw();
          const float rate = flip ? fr4 : (!spd_fresh ? sr : (tgt > s_sl.ks ? rr : (sr > 8.f ? sr : 8.f)));
          const float lim = dt * rate;
          if (s_sl.ks < tgt - lim) s_sl.ks += lim; else if (s_sl.ks > tgt + lim) s_sl.ks -= lim; else s_sl.ks = tgt; }
        /* фаза - СВОЯ и непрерывная: шагает с долей скорости (стопы не скользят на разгоне и торможении); игра на каждом новом
         * нажатии начинает свою ходьбу с одного кадра - раньше фаза прыгала туда же и анимация «сбрасывалась». К фазе походки
         * игры она лишь мягко подтягивается, когда впереди её родная ходьба (корпус игры в такт ногам). */
        s_sl.phi += dt * 30.f / 34.f * s_sl.ks; s_sl.phi -= floorf(s_sl.phi);
        { int mm = -1, kk = 0;
          if (wm >= 0 && sub == 5 && ao_last_motion(&mm, &kk) && mm == wm) {
              float tt; const int si0 = sl_seg(thq, &tt);
              const float wown = (s_sl.kd[si0] == -1 ? 1.f - tt : 0.f) + (s_sl.kd[si0 + 1] == -1 ? tt : 0.f);
              float e = sl_frame2ph((float)kk, SL_OWN_TD_FWD, SL_OWN_TDL_FWD, s_ao_mots[wm].nf) - s_sl.phi; e -= floorf(e + 0.5f);
              s_sl.phi += e * 0.12f * wown; s_sl.phi -= floorf(s_sl.phi);
          } }
        /* остановка (шаг игры кончился, она в прицеле): ~0.4 с держим свою стойку, пока игра доигрывает свою остановку */
        if (sub == 1 && s_sl.prev_sub == 5 && s_sl.won > 0.5f && act == 6) {
            s_sl.hold = 12; s_sl.hold_x = *(const int16_t *)(ram + 0xB92ACu); s_sl.hold_z = *(const int16_t *)(ram + 0xB92B0u); }
        if (sub == 5 || act != 6) s_sl.hold = 0; else if (s_sl.hold > 0) s_sl.hold--;
        s_sl.prev_sub = sub;
        const float wt = ((fresh && sub == 5) || s_sl.hold > 0) ? 1.f : 0.f;
        { float wtm = sl_lv_f("won_time", 0.25f); if (wtm < 0.05f) wtm = 0.05f;   /* переход ног игры <-> стрейф (12.10 ночь: 0.17 -> 0.25 с, ровнее старт) */
          if (s_sl.won < wt) { s_sl.won += dt / wtm; if (s_sl.won > wt) s_sl.won = wt; }
          else { s_sl.won -= dt / wtm; if (s_sl.won < wt) s_sl.won = wt; } }
        const float widle = 1.f - sl_smooth(sl_spdk());
        /* 12.10 ночь: направление ног ведётся только на шаге; на остановке оно прежнее, пока ноги уходят в стойку (стрейф кончился -
         * курс шага сбрасывался на корпус, направление прыгало «вперёд», и ноги за пару тактов прокручивались через круг клипов) */
        int th_snap = 0;
        if (!s_sl.init || s_sl.won < 0.05f || (walking && (flip ? s_sl.ks <= 0.03f : widle > 0.85f))) { s_sl.th = thq; s_sl.thv = 0.f; s_sl.init = 1; th_snap = 1; }
        else if (walking && !flip && !s_sl.vw) sl_spring(&s_sl.th, &s_sl.thv, thq, sl_lv_f("dir_omega", 40.f), dt, 1);   /* направление ног - пружина без перелёта (на развороте - ждёт стойку) */
        else s_sl.thv = 0.f;
        s_sl.th = sl_wq(s_sl.th);
        /* 13.10: ПОЗА НА КРУГЕ (thp) и вариант бока (lv: 0 - FR/FL, отстающая нога спереди - в смеси с диагональю вперёд; 1 - BR/BL,
         * нога сзади - с диагональю назад). Поза идёт за направлением ног th, но в сектор, где нужен другой вариант бока, не заходит:
         * ждёт ровно на боку (чистый боковой клип; поворот ног под шаг - warp - ведёт стопы по настоящему направлению), там вариант
         * сменяется смесью за lat_xfade (0.2 с), начатой в той части шага, где ноги далеко друг от друга при любых долях (замер: справа
         * фаза 0.64..1.18, слева 0.17..0.68 - зазор голеней не меньше 95 ед.), потом поза догоняет направление (lat_catch квантов в
         * секунду). Несовместимые (нога спереди с диагональю назад и наоборот) не смешиваются вовсе. Вне бока (вперёд, назад), в стойке
         * и на старте - сразу. */
        { const float ath = fabsf(s_sl.th);
          /* 13.10.2026 день (3) (игрок: «режим 3, ходьба назад-влево: туловище с тазом сильно перекручивается, подставь туда влево
           * анимацию»): в 3 корпус назад-влево повёрнут от камеры на 35° влево, шаг - на 135°, ноги идут на -100° от корпуса: смесь
           * 78% BL (влево, нога сзади; таз в записи развёрнут на 43° вправо от её ходьбы и оставлен на 0.9 - иначе сходятся колени) и
           * 22% L135 - таз на ~41° правее корпуса (у её ходьбы +10°, назад-вправо -5°). Теперь в режиме 3 слева, пока ноги не дальше
           * m3_left_hold (30°) за боком, поза стоит на боку с «влево» dir.L (FL, таз к шагу, доля 0.4), остаток до направления шага -
           * поворотом ног (warp), как у «влево» в режиме 1. На старте с места (и после торможения до стойки) корпус ещё поворачивается
           * и ноги сначала на -135..-145° - с самого старта, пока ноги не дальше m3_left_start (60°) и не дольше m3_left_time (0.6 с),
           * так же: без BL и без смены варианта посреди разгона. Дальше назад - как было. */
          const int snap = th_snap || s_sl.won < 0.05f || s_sl.ks < 0.03f;
          s_sl.pm_reset = snap;                                   /* 13.10 поздний вечер (5): курс таза режима 6 - без отставания */
          if (snap) s_sl.t3 = 0.f; else s_sl.t3 += dt;
          const int m3l = dc_aim_walk_mode() == 3;
          int h3 = 0;
          { const float qd = 4096.f / 360.f, hz = sl_lv_f("m3_left_hold", 30.f) * qd, hs = sl_lv_f("m3_left_start", 60.f) * qd;
            /* 13.10 поздний вечер (4) (запись B игрока, S, D, W, A по кругу): и цель шага - куда пойдут ноги, когда корпус
             * довернётся, - в этой же зоне (назад-влево). Ноги лишь проходят её («влево -> назад», корпус ещё поворачивается) - не
             * держится: держалось 0.1-0.2 с, таз с поворотом ног уходил влево на 47° и сразу обратно на 96°. m3_left_goal 0 - как было */
            s_sl.hg = 1;
            if (sl_lv_f("m3_left_goal", 1.f) != 0.f && fr - s_goal_f <= 3u) {
                const float g = sl_wq((float)s_goal_q), ag = fabsf(g);
                s_sl.hg = g < 0.f && ag > 1024.5f && ag < 1024.f + hz + (s_sl.h3 ? 2.f : -2.f) * qd;
            }
            const int lb = m3l && s_sl.hg && s_sl.th < 0.f && ath > 1024.5f;          /* слева за боком (и цель там) */
            const int in_s = lb && hs > 0.f && ath < 1024.f + hs;
            if (snap) s_sl.h3s = in_s;                                                 /* зона старта - только со старта, пока ноги в ней */
            else if (s_sl.h3s && (!in_s || s_sl.t3 >= sl_lv_f("m3_left_time", 0.6f))) s_sl.h3s = 0;
            h3 = lb && ((hz > 0.f && ath < 1024.f + hz + (s_sl.h3 ? 2.f : -2.f) * qd) || s_sl.h3s); }
          const int h3_was = s_sl.h3;
          s_sl.h3 = h3;
          const float tht = h3 ? -1024.f : s_sl.th;               /* куда идёт поза на круге */
          if (snap) {
              s_sl.thp = tht; s_sl.thp_lag = 0; s_sl.lx[0] = s_sl.lx[1] = 0; s_sl.vw = 0; s_sl.vwd = 0; s_sl.h3r = 0;
              if (h3) s_sl.lv[1] = 0.f;
              else if (fabsf(ath - 1024.f) > 0.5f) s_sl.lv[s_sl.th >= 0.f ? 0 : 1] = ath > 1024.f ? 1.f : 0.f;
          } else {
              /* 13.10 поздний вечер (2) (игрок: «мудришь с хождением: идёт вперёд или вперёд-влево - таз с ногами повёрнут на все 90°,
               * будто смотрит назад; таз с ногами - своя ориентация, минимум поворота от торса; вправо - анимация вправо, влево -
               * влево, назад - назад»). Его запись 16:52: влево -> вперёд -> вправо - поза ждала на левом боку смены варианта (thp
               * -90), направление ушло на +90, остаток добирал поворот ног под шаг (до 159°), клэмп держал 90. lat_wait 0 (по
               * умолчанию): поза на круге идёт за направлением сразу, вариант бока - по сектору, где поза (смена - в своей фазе шага,
               * как была); поворот ног под шаг тогда - только постоянное смещение перекрестия от камеры. 1 - как было (ждёт на боку) */
              const int lwait = sl_lv_f("lat_wait", 0.f) != 0.f;
              /* 13.10 поздний вечер (4) (игрок, режим 3, запись B: «перебираю S, D, W, A по кругу - дрыжок, смена угла таза с ногами в
               * один кадр»): поза «влево» (h3) держится на боку, пока ноги не дальше m3_left_hold за боком, остаток - поворотом ног (warp,
               * до 23°); отпустила - поза на круге одним тактом прыгала с -90 на -124° (смесь BL/L135), warp - на 0: таз с ногами за такт
               * на 50° (его запись: 8 раз за 2.5 минуты, каждый раз на «влево -> назад»). Теперь отпускание плавное: поза идёт от бока к
               * направлению ног по smoothstep за m3_left_release (0.3 с), остаток - тем же поворотом ног, направление ног не прыгает.
               * Без h3 прыжка позы не бывает: она идёт за пружиной направления (dir_omega). */
              if (h3_was && !h3) { s_sl.h3r = 1; s_sl.h3r_t = 0.f; s_sl.h3r_from = s_sl.thp; }
              float step = sl_wq(tht - s_sl.thp);
              if (s_sl.h3r && !h3) {
                  const float T = sl_lv_f("m3_left_release", 0.3f);
                  s_sl.h3r_t += dt;
                  if (T <= 0.f || s_sl.h3r_t >= T) s_sl.h3r = 0;
                  else step = sl_wq(s_sl.h3r_from + sl_wq(tht - s_sl.h3r_from) * sl_smooth(s_sl.h3r_t / T) - s_sl.thp);
              } else {
                  s_sl.h3r = 0;
                  if (s_sl.thp_lag || h3) { const float r = sl_lv_f("lat_catch", 2500.f) * dt; if (step > r) step = r; if (step < -r) step = -r; }
              }
              float np = sl_wq(s_sl.thp + step);
              const float ap = fabsf(s_sl.thp), an = fabsf(np);
              const int sd = s_sl.thp >= 0.f ? 0 : 1;
              int blocked = 0;
              if (lwait && (np >= 0.f) == (s_sl.thp >= 0.f) && an > 512.f && an < 1536.f && fabsf(an - 1024.f) > 0.5f) {
                  const float need = an > 1024.f ? 1.f : 0.f;
                  const int cross = (ap - 1024.f) * (an - 1024.f) < 0.f || fabsf(ap - 1024.f) <= 0.5f;
                  if (cross && fabsf(s_sl.lv[sd] - need) > 0.001f) { np = s_sl.thp >= 0.f ? 1024.f : -1024.f; blocked = 1; }
              }
              s_sl.thp = np;
              s_sl.thp_lag = blocked || fabsf(sl_wq(tht - s_sl.thp)) > 2.f;
              { const float qd = 4096.f / 360.f, dg = sl_lv_f("m3_wait_dip", 30.f) * qd;   /* 13.10 день (3): ждёт на боку, направление ушло */
                /* (и когда «влево» ждёт на боку с BL смены на FL - назад -> назад-влево) */
                const int hw = h3 && fabsf(fabsf(s_sl.thp) - 1024.f) <= 0.5f && s_sl.thp < 0.f && s_sl.lv[1] > 0.001f;
                s_sl.vw = m3l && dg > 0.f && ((blocked && fabsf(sl_wq(s_sl.th - s_sl.thp)) > (s_sl.vw ? dg - 10.f * qd : dg)) || hw); }
              const float apn = fabsf(s_sl.thp); const int sp = s_sl.thp >= 0.f ? 0 : 1;
              if (apn <= 512.f || apn >= 1536.f) { s_sl.lv[sp] = apn > 1024.f ? 1.f : 0.f; s_sl.lx[sp] = 0; }   /* бок не в позе - сразу */
              else if (!lwait || fabsf(apn - 1024.f) <= 0.5f) {     /* поза на боку (lat_wait 0 - у бока): вариант - к сектору */
                  const float dd = lwait ? sl_wq(s_sl.th - s_sl.thp) * (s_sl.thp >= 0.f ? 1.f : -1.f) : apn - 1024.f;   /* + - к диагонали назад */
                  const float hy = sl_lv_f("lat_hyst", 2.f) * (4096.f / 360.f);
                  float tg = s_sl.lv[sp];
                  if (h3 && sp == 1) tg = 0.f;                           /* 13.10 день (3): режим 3 слева за боком - «влево» (FL) */
                  /* lat_wait 0: ровно на боку - вариант «нога спереди» (FR/FL: таз ближе к корпусу; у BL таз в анимации до 54°) */
                  else if (dd > hy) tg = 1.f; else if (dd < (lwait ? -hy : 0.5f * hy)) tg = 0.f;
                  if (s_sl.lv[sp] != tg) {
                      float xf = sl_lv_f("lat_xfade", 0.2f); if (xf < 0.02f) xf = 0.02f;
                      if (!s_sl.lx[sp]) {
                          const float w0 = sp == 0 ? 0.64f : 0.17f, wl = sp == 0 ? 0.54f : 0.51f;
                          float in = s_sl.phi - w0; in -= floorf(in);
                          const float cyc = (34.f / 30.f) / (s_sl.ks > 0.1f ? s_sl.ks : 0.1f);
                          if (in < wl && (wl - in) * cyc >= xf) s_sl.lx[sp] = 1;
                      }
                      if (s_sl.lx[sp]) {
                          const float st = dt / xf;
                          if (s_sl.lv[sp] < tg) { s_sl.lv[sp] += st; if (s_sl.lv[sp] >= tg) { s_sl.lv[sp] = tg; s_sl.lx[sp] = 0; } }
                          else { s_sl.lv[sp] -= st; if (s_sl.lv[sp] <= tg) { s_sl.lv[sp] = tg; s_sl.lx[sp] = 0; } }
                      }
                  } else s_sl.lx[sp] = 0;
              }
              s_sl.vwd = s_sl.vw && !s_sl.lx[sp];                  /* 13.10 день (3): тормоз - пока смена варианта не началась */
          } }
        /* множитель шага на следующий такт (игре - в запись 0xB92D0): назад - её ход назад, вбок/вперёд - side, разгон/тормоз */
        const float bk = sl_lv_f("speed_back", 1140.f / 34.f / 43.f), sk = sl_lv_f("speed_side", 1.f);
        const float wb = sl_back_w(thq);
        /* 13.10 день (3), режим 3: пока ноги ныряют в стойку ради смены варианта бока (и выныривают), шаг игры - не меньше m3_wait_speed
         * её шага (0.5): полная остановка на «назад-влево -> назад» читалась заминкой; стопы чуть скользят, пока ноги в стойке */
        { float kk = sl_spdk(); if (s_sl.dipk) { const float fl = sl_lv_f("m3_wait_speed", 0.5f); if (kk < fl) kk = fl; }
          s_sl.k = s_sl.hold > 0 ? 0.f : (bk * wb + sk * (1.f - wb)) * sl_smooth(kk); }
        if ((fresh && sub == 5) || s_sl.hold > 0) {
            s_sl.k_frame = fr;
            if (s_sl.v_have) {                                   /* шаг следующего такта = скорость игры * наш множитель */
                const int nv = (int)lroundf((float)s_sl.v_game * s_sl.k);
                ram[0xB92D0] = (uint8_t)(nv & 0xFF); ram[0xB92D1] = (uint8_t)((nv >> 8) & 0xFF); s_sl.v_owned = 1;
            }
        } else if (s_sl.v_owned && s_sl.v_have) {               /* стрейф кончился - вернуть игре её скорость */
            ram[0xB92D0] = (uint8_t)(s_sl.v_game & 0xFF); ram[0xB92D1] = (uint8_t)((s_sl.v_game >> 8) & 0xFF); s_sl.v_owned = 0;
        }
    }
    /* 2. поза: смесь двух соседних направлений -> к стойке прицела при малой скорости -> к ногам игры по весу включения */
    float live[24], mv[24], idle[24], pose[24], out[24];
    sl_live_pose(ram, live);
    /* 13.10: смесь клипов - по позе на круге (thp: ждёт на боку, пока вариант бока не тот), поворот ног под шаг - по направлению th */
    const float thp = s_sl.init ? s_sl.thp : s_sl.th;
    float t; const int si = sl_seg(thp, &t);
    float a[24], b[24], te = t; int okp = 0;
    float pmA = 0.f, pmB = 0.f;                                 /* 13.10 поздний вечер (5): средний курс таза концов отрезка */
    if (wm >= 0) {
        const int lat_a = abs(s_sl.dq[si]) == 1024, lat_b = abs(s_sl.dq[si + 1]) == 1024, sd = thp >= 0.f ? 0 : 1;
        if (lat_a || lat_b) {                                   /* 13.10: отрезок с боком - вариант бока и совместимая диагональ */
            const int back = lat_a ? abs(s_sl.dq[si + 1]) > 1024 : abs(s_sl.dq[si]) > 1024;
            float v = s_sl.lv[sd];
            { const float lf = sl_lv_f("lv_force", -1.f); if (lf >= 0.f) v = lf; }   /* 13.10 день (3): отладка - доля варианта бока вручную */
            (void)back;                                             /* (поза ждёт на боку, пока вариант не тот, - смесь всегда совместима) */
            float *N = lat_a ? a : b, *D = lat_a ? b : a, nf[24], nb[24];
            const int kf = s_sl.kd[lat_a ? si : si + 1], kb2 = s_sl.kv[sd];
            int okn;
            if (v <= 0.001f || kb2 < -2) okn = sl_node_pose(ram, kf, wm, s_sl.phi, N);
            else if (v >= 0.999f) okn = sl_node_pose(ram, kb2, wm, s_sl.phi, N);
            else { okn = sl_node_pose(ram, kf, wm, s_sl.phi, nf) && sl_node_pose(ram, kb2, wm, s_sl.phi, nb); if (okn) sl_blend(nf, nb, v, N); }
            okp = okn && sl_node_pose(ram, s_sl.kd[lat_a ? si + 1 : si], wm, s_sl.phi, D);
            if (okp && dc_aim_walk_raw() == 6) {
                const float mf = sl_node_mean_yaw(ram, kf, wm);
                const float mn = (v <= 0.001f || kb2 < -2) ? mf : sl_wq(mf + sl_wq(sl_node_mean_yaw(ram, kb2, wm) - mf) * (v >= 0.999f ? 1.f : v));
                const float md = sl_node_mean_yaw(ram, s_sl.kd[lat_a ? si + 1 : si], wm);
                pmA = lat_a ? mn : md; pmB = lat_a ? md : mn;
            }
        } else {
            okp = sl_node_pose(ram, s_sl.kd[si], wm, s_sl.phi, a) && sl_node_pose(ram, s_sl.kd[si + 1], wm, s_sl.phi, b);
            if (okp && dc_aim_walk_raw() == 6) { pmA = sl_node_mean_yaw(ram, s_sl.kd[si], wm); pmB = sl_node_mean_yaw(ram, s_sl.kd[si + 1], wm); }
        }
    }
    s_sl.te = te;
    if (okp) {
        sl_blend(a, b, te, mv);
        /* 12.10 поздно: ПОВОРОТ НОГ ПОД ШАГ. Корпус смотрит на перекрестие, а оно правее направления камеры на ~11° (камера за
         * правым плечом) - клавиши давали не 8 направлений круга, а их же минус 11°: «вправо» = 85% вбок + 15% диагонали вперёд
         * (другая походка - ноги в смеси проходили одна сквозь другую), «вперёд-влево» = 85% диагонали + 15% шага вбок, тогда как
         * «влево» и «вперёд-вправо» смешивали похожие. Теперь смесь двух клипов идёт по своему направлению dq + t*512, а таз и
         * ноги поворачиваются на остаток до направления шага (warp, 1 - полностью): стопы идут ровно по шагу, а при узкой смеси
         * (blend 0.5) на клавишах играет чистый клип. Корпус остаётся на перекрестии. */
        const float wk = sl_lv_f("warp", 1.f);
        s_sl.rho = wk != 0.f ? sl_wq(s_sl.th - daim - ((float)s_sl.dq[si] + te * 512.f)) * wk : 0.f;
        sl_yaw_pose(mv, s_sl.rho);
        /* 13.10 поздний вечер (5) (игрок, режим 6, запись B: «перебираю клавиши по кругу - таз дрожит, скачки, рывки; используй
         * наиболее подходящие анимации»): клипы и их смеси - прежние (вбок у «ноги спереди» FL/FR и «ноги сзади» BL/BR свой
         * разворот таза: слева -17 и +38°, справа +11 и -17° от её ходьбы - без смены варианта на боку голени проходят друг
         * сквозь друга, замер blend_gap), но средний курс таза позы (смесь средних клипов + поворот ног под шаг) ведёт пружина
         * m6_pelvis_omega (8; 0 - как было): таз поворачивается к нему, бёдра - обратно, стопы - IK на места клипа. Смена варианта
         * (55° слева за 0.2 с), проход бока и «влево» за боком - таз плавно, качание таза в шаге - как в клипе. Отставание не
         * больше m6_pelvis_max (45°, мягко). Стойка, старт и разворот через стойку - без отставания. Только режим 6. */
        s_sl.pmc = 0.f;
        { const float om = sl_lv_f("m6_pelvis_omega", 8.f);
          if (dc_aim_walk_raw() == 6 && om > 0.f) {
              const float tgt = sl_wq(pmA + sl_wq(pmB - pmA) * te + s_sl.rho);
              /* старт, стойка, разворот через стойку: пружина - от таза, который нарисован сейчас (на старте - её стойка), а не
               * сразу на цели (старт назад-влево: поза «влево» с поворотом ног до 50° включалась за 0.25 с - таз на -69° от корпуса) */
              if (!s_sl.pm_on || s_sl.pm_reset) { s_sl.pm = (fr - s_sl.py_f <= 4u) ? s_sl.py_last : sl_pose_yaw(live); s_sl.pmv = 0.f; s_sl.pm_on = 1; }
              else if (dt > 0.f) sl_spring(&s_sl.pm, &s_sl.pmv, tgt, om, dt, 1);
              float c = sl_wq(s_sl.pm - tgt);
              const float qd = 4096.f / 360.f, cm = sl_lv_f("m6_pelvis_max", 45.f) * qd, kn = (cm > 15.f * qd ? 15.f * qd : cm);
              if (cm > 0.f && fabsf(c) > cm - kn) {
                  float ac = (cm - kn) + kn * tanhf((fabsf(c) - (cm - kn)) / (kn > 1.f ? kn : 1.f));
                  if (ac > cm) ac = cm;
                  c = c < 0.f ? -ac : ac;
              }
              s_sl.pm = sl_wq(tgt + c); s_sl.pmc = c;
              if (fabsf(c) >= 0.5f) {
                  float A0[2][3]; sl_leg_points(mv, A0);
                  sl_twist(mv, c);
                  if (sl_lv_f("foot_lock", 1.f) != 0.f) sl_leg_ik(mv, A0);
              }
          } else s_sl.pm_on = 0; }
    }
    else { memcpy(mv, live, sizeof mv); s_sl.pm_on = 0; }
    /* 12.10 поздно (игрок: «останавливаешься, начинаешь целиться - вся Регина как-то смещается»): стойка, в которую ноги
     * сходятся на остановке, была всегда «прямо» (M16), а игра стоя показывает стойку по высоте прицела (в его сессии - «вниз»,
     * M17: корень на 30 ед. впереди, ноги почти те же). Через ~0.4 с, когда ноги возвращались игре, таз уезжал на 3 см над
     * стоящими стопами. Теперь цель остановки - стойка, которую рисует игра (её живая поза, когда она стоит в прицеле в одной из
     * стоек набора), до остановки - последняя такая (новый набор оружия - заново), без неё - «прямо». */
    { int mm = -1, kk = 0;
      if (s_sl.idle_ok && s_sl.idle_key != s_ao_ext.key) s_sl.idle_ok = 0;
      if (act == 6 && sub == 1 && s_ao_ext.valid && ao_last_motion(&mm, &kk) &&
          (mm == s_ao_ext.mot[0] || mm == s_ao_ext.mot[1] || mm == s_ao_ext.mot[2])) {
          memcpy(idle, live, sizeof idle); memcpy(s_sl.idle_pose, live, sizeof idle); s_sl.idle_ok = 1; s_sl.idle_key = s_ao_ext.key;
      } else if (s_sl.idle_ok) memcpy(idle, s_sl.idle_pose, sizeof idle);
      else if (s_ao_ext.valid) sl_from_frame(ram + AP_BLOCK + s_ao_mots[s_ao_ext.mot[1]].data, idle);   /* стойка прицела «прямо» */
      else memcpy(idle, live, sizeof idle); }
    const float widle = 1.f - sl_smooth(sl_spdk());
    const float won = sl_smooth(s_sl.won);
    sl_blend(mv, idle, widle, pose);
    sl_blend(live, pose, won, out);
    /* 3. КОРПУС: на шаге вбок и по диагонали ровный (среднее по циклу её походки), вперёд и назад - её родной; доля - пружиной */
    float wst = 0.f;
    {
        if (wm >= 0 && (s_sl.t0m_m != wm || s_sl.t0m_key != s_ao_ext.key)) {
            float cs[3] = { 0, 0, 0 }, sn[3] = { 0, 0, 0 };
            for (int k = 0; k < s_ao_mots[wm].nf; k++) { int ang[45]; ao_unpack45(ram + AP_BLOCK + s_ao_mots[wm].data + (uint32_t)k * 80u, ang);
                for (int c = 0; c < 3; c++) { cs[c] += cosf((float)ang[c] * AO_ANG); sn[c] += sinf((float)ang[c] * AO_ANG); } }
            for (int c = 0; c < 3; c++) s_sl.t0m[c] = atan2f(sn[c], cs[c]) / AO_ANG;
            s_sl.t0m_m = wm; s_sl.t0m_key = s_ao_ext.key;
        }
        const float wown = (s_sl.kd[si] < 0 ? 1.f - t : 0.f) + (s_sl.kd[si + 1] < 0 ? t : 0.f);
        wst = (wm >= 0 ? (1.f - wown) : 0.f) * won * (1.f - widle) * sl_lv_f("torso_steady", 1.f);
        if (dt > 0.f) sl_spring(&s_sl.ws, &s_sl.wsv, wst, sl_lv_f("torso_omega", 10.f), dt, 0);
        float wsc = s_sl.ws; if (wsc < 0.f) wsc = 0.f; if (wsc > 1.f) wsc = 1.f;
        if (s_sl.won < 0.005f && wsc < 0.003f && fabsf(s_sl.wsv) < 0.05f) {   /* всё вернулось к игре - отпускаем */
            s_sl.active = 0; s_sl.init = 0; s_sl.ws = 0.f; s_sl.wsv = 0.f; return;
        }
        s_sl.active = 1;
        if (wsc > 0.002f) {
            float L0[9]; int ql[3]; sf_load((const int16_t *)(ram + AP_NODES), L0); ao_decompose(L0, ql);
            int T0[3]; for (int c = 0; c < 3; c++) T0[c] = (int)lroundf((float)ql[c] + sl_wq(s_sl.t0m[c] - (float)ql[c]) * wsc) & 4095;
            float Ln[9]; ao_euler(T0[0], T0[1], T0[2], Ln);
            int16_t *N0 = (int16_t *)(ram + AP_NODES);
            memcpy(s_sf.t0saved, N0, sizeof s_sf.t0saved); sf_store(Ln, N0); memcpy(s_sf.t0wrote, N0, sizeof s_sf.t0wrote); s_sf.t0applied = 1;
        }
    }
    /* 13.10 поздний вечер (игрок: «откати правку по ногам, двигаться как до неё - плавно; но таз с ногами не должен делать полный
     * оборот, максимум 90° от верхнего торса: просто клэмп и бленд по кратчайшему пути, чтобы не было перескакивания»). Нырки в
     * стойку вечера убраны (с ними она останавливалась и дрожала): ноги - как в его сессии 15:10. Здесь, перед выдачей, таз позы
     * от корпуса (кратчайший угол) не дальше tw_max (90°), последние tw_knee (15°) - мягко; за 180° прежняя сторона держится ещё
     * tw_hold (30°), у «прямо назад» таз не мечется; цель прыгнула больше tw_jump (60°) за такт (смена стороны, +90 -> -90) -
     * переход от нарисованного к цели за tw_blend (0.4 с, плавные старт и конец) через «прямо». Поворачивается вся поза ног (в эти
     * мгновения стопы скользят), внутри 75° ноги как были. Его запись 15:10: таз был до 179°, 13 переходов через 180° - стало не
     * дальше 90° и 13 плавных переходов. tw_max 0 - выкл */
    s_sl.tg = 0.f;
    { const float qd = 4096.f / 360.f, tm = sl_lv_f("tw_max", 90.f) * qd;
      if (tm > 0.f) {
          float L0g[9]; sf_load((const int16_t *)(ram + AP_NODES), L0g);
          float d = sl_wq(sl_pose_yaw(out) - atan2f(L0g[2], L0g[8]) / AO_ANG);             /* таз от корпуса, кратчайший угол */
          if (!s_sl.twi || fr - s_sl.twf > 8u) { s_sl.twi = 0; s_sl.twt = -1.f; }             /* ноги стрейфа заново - без истории */
          const float hold = sl_lv_f("tw_hold", 30.f) * qd;
          if (s_sl.twi && s_sl.tws > 0 && d < -2048.f + hold) d += 4096.f;                  /* за 180° - прежняя сторона */
          else if (s_sl.twi && s_sl.tws < 0 && d > 2048.f - hold) d -= 4096.f;
          float kn = sl_lv_f("tw_knee", 15.f) * qd; if (kn > tm) kn = tm;
          const float ad = fabsf(d), k0 = tm - kn;
          float c = (ad <= k0 || kn <= 0.f) ? ad : k0 + kn * tanhf((ad - k0) / kn);         /* мягкий клэмп */
          if (c > tm) c = tm;
          if (d < 0.f) c = -c;
          const float T = sl_lv_f("tw_blend", 0.4f);
          if (!s_sl.twi) { s_sl.twa = s_sl.twc = c; s_sl.twt = -1.f; s_sl.tws = c >= 0.f ? 1 : -1; s_sl.twi = 1; }
          else if (dt > 0.f) {
              if (s_sl.twt >= 0.f) { s_sl.twt += dt; if (T <= 0.f || s_sl.twt >= T) s_sl.twt = -1.f; }
              if (T > 0.f && fabsf(c - s_sl.twc) > sl_lv_f("tw_jump", 60.f) * qd) { s_sl.tw0 = s_sl.twa; s_sl.twt = 0.f; }
              s_sl.tws = c > 0.f ? 1 : (c < 0.f ? -1 : s_sl.tws);
              s_sl.twc = c;
          }
          const float aw = (s_sl.twt >= 0.f && T > 0.f) ? s_sl.tw0 + (c - s_sl.tw0) * sl_smooth(s_sl.twt / T) : c;   /* бленд к цели */
          if (dt > 0.f) s_sl.twa = aw;
          s_sl.twf = fr;
          s_sl.tg = sl_wq(aw - d);
          sl_yaw_pose(out, s_sl.tg);                                                          /* меньше 0.5 кванта - не трогает */
      } else s_sl.twi = 0; }
    s_sl.py_last = sl_pose_yaw(out); s_sl.py_f = fr;                 /* 13.10 поздний вечер (5): курс таза нарисованной позы */
    sl_apply(ram, out);
    { static int dbg4 = -1; if (dbg4 < 0) { const char *e = getenv("DC_STRAFE_DBG"); dbg4 = (e && (*e == '1' || *e == '2')) ? 1 : 0; }
      static float po[24]; static uint32_t pf = 0; float jmp = 0.f, jr = 0.f;
      if (fr - pf <= 4u) { for (int i = 3; i < 24; i++) { const float d = fabsf(sl_wq(out[i] - po[i])) * 360.f / 4096.f; if (d > jmp) jmp = d; }
                           for (int i = 0; i < 3; i++) { const float d = fabsf(out[i] - po[i]); if (d > jr) jr = d; } }
      memcpy(po, out, sizeof po); pf = fr;
      /* 13.10 день (3): tw - таз от корпуса, как нарисованы (градусы, + вправо; у её ходьбы +10) */
      float L0d[9]; sf_load((const int16_t *)(ram + AP_NODES), L0d);
      const float twd = sl_wq(sl_pose_yaw(out) - atan2f(L0d[2], L0d[8]) / AO_ANG) * 360.f / 4096.f;
      if (dbg4) fprintf(stderr, "strafe4: f=%u sub=%d th=%.0f ths=%.0f seg=%d/%d t=%.2f phi=%.3f won=%.2f idle=%.2f spdk=%.2f wm=%d fresh=%d jump=%.1f root=%.0f hold=%d wst=%.2f ws=%.2f k=%.2f rho=%.0f lv=%.2f/%.2f thp=%.0f hdy=%.0f h3=%d hg=%d gq=%d tw=%.1f vw=%d tg=%.1f pc=%.1f%c",
                        fr, sub, thq, s_sl.th, s_sl.kd[si], s_sl.kd[si + 1], t, s_sl.phi, s_sl.won, widle, sl_spdk(), wm, fresh, jmp, jr, s_sl.hold,
                        wst, s_sl.ws, s_sl.k, s_sl.rho, s_sl.lv[0], s_sl.lv[1], s_sl.thp, s_sl.hdy, s_sl.h3, s_sl.hg, fr - s_goal_f <= 3u ? s_goal_q : 9999, twd, s_sl.vw, s_sl.tg * 360.f / 4096.f, s_sl.pmc * 360.f / 4096.f, 10);
      { static int dbgp = -1; if (dbgp < 0) { const char *e = getenv("DC_STRAFE_DBG"); dbgp = (e && *e == '2') ? 1 : 0; }   /* 2: итоговая поза ног */
        if (dbgp) { fprintf(stderr, "strafe4p: f=%u", fr); for (int i = 0; i < 24; i++) fprintf(stderr, " %.1f", out[i]); fprintf(stderr, "%c", 10); } } }
    { static long n = 0; if (++n <= 3 || (n % 1800) == 0)
        fprintf(stderr, "aimwalk: strafe legs from the player's set (legs 4) - direction %.0f deg, walk motion M%02d, phase %.2f%c",
                s_sl.th * 360.f / 4096.f, wm, s_sl.phi, 10); }
}
static void strafe_draw_enter(void) {
    extern uint64_t s_frame_count;
    s_sf.applied = 0; s_sf.papplied = 0; s_sf.gapplied = 0; s_sf.rapplied = 0; s_sf.t0applied = 0;
    { static int dbg = -1;                                                     /* DC_STRAFE_DBG=1: курс каждой отрисовки в прицеле */
      if (dbg < 0) { const char *e = getenv("DC_STRAFE_DBG"); dbg = (e && (*e == '1' || *e == '2')) ? 1 : 0; }
      uint8_t *rd = memory_get_ram_ptr();
      if (dbg && rd && rd[0xB92C9] == 6) {
          const int16_t *Md = (const int16_t *)(rd + 0xB928Cu);
          const int yrd = (int)lroundf(atan2f((float)Md[2], (float)Md[0]) / AO_ANG) & 4095;
          const int ry = *(const int16_t *)(rd + 0xB92B6u) & 4095;
          fprintf(stderr, "strafedbg: f=%u sub=%d ramyaw=%d matyaw=%d R4=%.3f aim=%d fresh=%d%c", (uint32_t)s_frame_count, rd[0xB92CA], ry, yrd,
                  (float)Md[4] / 4096.f, s_sf.aim_q, (uint32_t)s_frame_count - s_sf.frame <= 3u, 10);
      } }
    const int fresh = (uint32_t)s_frame_count - s_sf.frame <= 3u;
    if (!fresh && !s_sl.active) return;                                      /* ноги после стрейфа ещё возвращаются к ногам игры */
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    if (ram[0xB92C8] != 1 || (fresh && ram[0xB92C9] != 6)) return;            /* только мир и прицел */
    if ((lad_u32(ram, LAD_NODE0 + 56u) & 0x1FFFFFu) != 0xB928Cu) return;       /* корень скелета - узел игрока */
    static float sgn = 0.f;
    if (sgn == 0.f) { const char *e = getenv("DC_STRAFE_SIGN"); sgn = (e && atoi(e) < 0) ? -1.f : 1.f; }
    int16_t *M = (int16_t *)(ram + 0xB928Cu);
    float R[9], Ry[9], Rn[9]; sf_load(M, R);
    /* 12.10.2026 вечер (игрок: «стрейф не работает вообще, Регина моментально поворачивается туда, куда жму»): здесь стояла
     * проверка «R - чистый поворот вокруг вертикали» (R[4] = 1). Но 0xB928C - матрица ВИДА: камера зоны игры x поворот
     * Регины (замер на копии слота 0: R4 != 1, курс матрицы - курс RAM + 1576, в ST403 было -164 и R4 ~ 1 - потому тест
     * там проходил). Под любым наклоном камеры зоны проверка молча выключала разворот. Поворот справа R*Ry - вокруг её
     * собственной вертикали, он верен при любой камере зоны; проверка убрана. */
    /* поворот = курс прицела минус ЖИВОЙ курс логики из RAM (замер DC_STRAFE_DBG: матрица строится из него в том же кадре, но
     * со своим постоянным сдвигом ~ -164 кванта - потому поворот «на разницу», а не «на абсолютный курс»: сдвиг сохраняется,
     * и стрейф рисуется ровно как стойка прицела) */
    const int ry = *(const int16_t *)(ram + 0xB92B6u) & 4095;
    const int dqs = fresh ? (int)lroundf(sgn * (float)(((s_sf.aim_q - ry + 2048) & 4095) - 2048)) : 0;
    if (dqs != 0) {
        ao_euler(0, dqs, 0, Ry); ao_mul(R, Ry, Rn);
        memcpy(s_sf.saved, M, sizeof s_sf.saved); sf_store(Rn, M); memcpy(s_sf.wrote, M, sizeof s_sf.wrote);
        s_sf.applied = 1;
    }
    { static int dbg2 = -1; if (dbg2 < 0) { const char *e = getenv("DC_STRAFE_DBG"); dbg2 = (e && (*e == '1' || *e == '2')) ? 1 : 0; }
      if (dbg2) fprintf(stderr, "strafedbg:   drawn matyaw=%d (turned %d) legs %d%c", (int)lroundf(atan2f((float)M[2], (float)M[0]) / AO_ANG) & 4095,
                        dqs, s_sf.legs_q, 10); }
    sl_legs(ram, fresh);                                                    /* ноги - из набора стрейфов игрока */
}
static void strafe_draw_exit(void) {
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) { s_sf.applied = s_sf.papplied = s_sf.gapplied = s_sf.rapplied = 0; return; }
    if (s_sf.rapplied) {                                                       /* качание таза - вернуть смещение корня */
        s_sf.rapplied = 0;
        int16_t *rp = (int16_t *)(ram + LAD_NODE0 + 68u);
        if (rp[0] == s_sf.rwrote[0] && rp[1] == s_sf.rwrote[1] && rp[2] == s_sf.rwrote[2]) for (int k = 0; k < 3; k++) rp[k] = s_sf.rsaved[k];
    }
    if (ram[0xB92C8] == 1 && ram[0xB92C9] == 6 && (lad_u32(ram, LAD_NODE0 + 56u) & 0x1FFFFFu) == 0xB928Cu) gait_offsets_capture(ram);
    if (s_sf.t0applied) {                                                      /* корпус (узел 0) - как его построила игра */
        s_sf.t0applied = 0;
        int16_t *N0 = (int16_t *)(ram + AP_NODES);
        if (!memcmp(N0, s_sf.t0wrote, sizeof s_sf.t0wrote)) memcpy(N0, s_sf.t0saved, sizeof s_sf.t0saved);
    }
    if (s_sf.gapplied) {                                                       /* ноги шага вбок - как их построила игра */
        s_sf.gapplied = 0;
        static const int legs[2] = { 9, 12 };
        for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) {
            int16_t *Ln = (int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)(legs[i] + j));
            if (!memcmp(Ln, s_sf.gwrote[i][j], sizeof s_sf.gwrote[i][j])) memcpy(Ln, s_sf.gsaved[i][j], sizeof s_sf.gsaved[i][j]);
        }
    }
    if (s_sf.papplied) {                                                       /* таз - вернуть локальную матрицу, как её построила игра */
        s_sf.papplied = 0;
        int16_t *P8 = (int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * 8u);
        if (!memcmp(P8, s_sf.pwrote, sizeof s_sf.pwrote)) memcpy(P8, s_sf.psaved, sizeof s_sf.psaved);
    }
    if (!s_sf.applied) return;
    s_sf.applied = 0;
    int16_t *M = (int16_t *)(ram + 0xB928Cu);
    if (!memcmp(M, s_sf.wrote, sizeof s_sf.wrote)) memcpy(M, s_sf.saved, sizeof s_sf.saved);
}
/* 13.10 поздний вечер (3): DC_POSE_LOG=1 - нарисованная поза Регины на входе в её отрисовку, после всех наших правок (руки и
 * голова - ao, ноги и корпус - стрейф): углы локальных матриц узлов 0 (корпус), 7 (голова), 8 (таз), смещение корня и курс
 * нарисованной матрицы самой Регины (0xB928C). Кольцо узлов порта (node_ring) пишет углы игры, без наших правок, - для поиска
 * дрожи нарисованного оно не годится */
static void pose_log(void) {
    static int en = -1; if (en < 0) { const char *e = getenv("DC_POSE_LOG"); en = (e && *e == '1') ? 1 : 0; }
    if (!en) return;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    if ((lad_u32(ram, LAD_NODE0 + 56u) & 0x1FFFFFu) != 0xB928Cu) return;       /* корень скелета - узел игрока */
    extern uint64_t s_frame_count;
    static const int nodes[3] = { 0, 7, 8 };
    int q[4][3]; float L[9];
    for (int i = 0; i < 3; i++) { sf_load((const int16_t *)(ram + AP_NODES + AP_NODE_STRIDE * (uint32_t)nodes[i]), L); ao_decompose(L, q[i]); }
    sf_load((const int16_t *)(ram + 0xB928Cu), L); ao_decompose(L, q[3]);
    /* 21.10: wy - нарисованный курс в мире (матрица её отрисовки без камеры зоны 0xBF9F4: Rc^T * M), его можно сравнивать с yaw */
    float C[9], W0[3]; sf_load((const int16_t *)(ram + 0xBF9F4u), C);
    for (int j = 0; j < 3; j++) W0[j] = C[0] * L[j] + C[3] * L[3 + j] + C[6] * L[6 + j];
    const int wyq = (int)lroundf(atan2f(W0[2], W0[0]) / AO_ANG) & 4095;
    const int16_t *rp = (const int16_t *)(ram + LAD_NODE0 + 68u);
    fprintf(stderr, "poselog: f=%u aw=%d act=%d sub=%d n0=%d,%d,%d n7=%d,%d,%d n8=%d,%d,%d m=%d,%d,%d root=%d,%d,%d yaw=%d wy=%d%c",
            (uint32_t)s_frame_count, dc_aim_walk_mode(), ram[0xB92C9], ram[0xB92CA], q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2],
            q[2][0], q[2][1], q[2][2], q[3][0], q[3][1], q[3][2], rp[0], rp[1], rp[2], (int)*(const int16_t *)(ram + 0xB92B6u), wyq, 10);
}
void dc_ladder_draw_enter(CPUState *cpu) {
    (void)cpu;
    strafe_draw_enter();                       /* 12.10: стрейф в прицеле (выше) */
    pose_log();                                /* 13.10 поздний вечер (3): DC_POSE_LOG - нарисованная поза */
    s_lad_applied = 0;
    const int d = lad_shift_cfg(); if (!d) return;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    if (ram[0xB92C8] != 4 || lad_s16(ram, 0xB93DE) != 2) return;              /* скриптовое движение Регины */
    if ((lad_u32(ram, LAD_NODE0 + 56u) & 0x1FFFFFu) != 0xB928Cu) return;       /* корень скелета — узел игрока */
    const uint32_t frames = lad_u32(ram, 0xB92FCu) & 0x1FFFFFu, hp8 = lad_u32(ram, 0xB9300u) & 0x1FFFFFu;
    if (frames < 0x10000u || hp8 < 0x10008u || hp8 >= 0x200000u) return;
    const uint32_t nf = lad_u32(ram, hp8 - 4u);
    if ((lad_u32(ram, hp8 - 8u) & 0x1FFFFFu) != frames || nf < 8u || nf > 400u || frames + 80u * nf > 0x200000u) return;
    const uint32_t key = frames ^ (nf << 23) ^ lad_u32(ram, frames + 12u) ^ lad_u32(ram, frames + 80u * (nf - 1u) + 2u);
    if (key != s_lad_key) {
        s_lad_key = key;
        int ymin = 32767, ymax = -32768, hmax = 0;
        for (uint32_t k = 0; k < nf; k++) {
            const uint32_t f = frames + 80u * k;
            const int x = lad_s16(ram, f), y = lad_s16(ram, f + 2u), z = lad_s16(ram, f + 4u);
            if (y < ymin) ymin = y; if (y > ymax) ymax = y;
            if (abs(x) > hmax) hmax = abs(x); if (abs(z) > hmax) hmax = abs(z);
        }
        s_lad_is = (ymin <= -1179 - 500 && ymax - ymin >= 600 && hmax <= 250);
        s_lad_endfloor = abs(lad_s16(ram, frames + 80u * (nf - 1u) + 2u) + 1179) < 200;
        s_lad_up = lad_s16(ram, frames + 80u * (nf - 1u) + 2u) < lad_s16(ram, frames + 2u) - 300;   /* 28.09 (3): корень уходит вверх */
        fprintf(stderr, "ladder: Regina's scripted motion %06X (%u frames): root y %d..%d, sideways/forward up to %d - %s%s%c",
                frames, nf, ymin, ymax, hmax, s_lad_is ? (s_lad_up ? "a ladder climb UP, drawn moved back from the ladder by DC_LADDER_SHIFT_UP"
                                                                   : "a ladder climb, drawn moved back from the ladder by DC_LADDER_SHIFT") : "not a ladder",
                s_lad_is ? (s_lad_endfloor ? " (it ends on the floor: the last frames bring it back)" : "") : "", 10);
    }
    if (!s_lad_is) return;
    float t = 1.f;
    if (s_lad_endfloor) {                                                      /* спуск: к последнему кадру сдвиг уходит в ноль */
        const uint32_t cur = lad_u32(ram, 0xB92E0u) & 0x1FFFFFu;
        if (cur >= frames && cur < frames + 80u * nf) { const float left = (float)(nf - 1u - (cur - frames) / 80u);
            t = left / 12.f; if (t > 1.f) t = 1.f; if (t < 0.f) t = 0.f; }
    }
    const int dd = (s_lad_up && lad_shift_up_cfg()) ? lad_shift_up_cfg() : d;   /* 28.09 (3): подъём — своя величина */
    const int dz = (int)lroundf((float)dd * t); if (!dz) return;
    int16_t *p = (int16_t *)(ram + LAD_NODE0 + 68u);
    for (int i = 0; i < 3; i++) s_lad_saved[i] = p[i];
    p[2] = (int16_t)(p[2] - dz);                                               /* лицом к лестнице — +z; назад — минус */
    for (int i = 0; i < 3; i++) s_lad_wrote[i] = p[i];
    s_lad_applied = 1;
}
void dc_ladder_draw_exit(CPUState *cpu) {
    (void)cpu;
    strafe_draw_exit();                        /* 12.10: стрейф в прицеле */
    if (!s_lad_applied) return;
    s_lad_applied = 0;
    uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;
    int16_t *p = (int16_t *)(ram + LAD_NODE0 + 68u);
    if (p[0] == s_lad_wrote[0] && p[1] == s_lad_wrote[1] && p[2] == s_lad_wrote[2]) { for (int i = 0; i < 3; i++) p[i] = s_lad_saved[i]; }
    else { static int n = 0; if (++n <= 5) fprintf(stderr, "ladder: the root offset changed during the draw - left as the game wrote it%c", 10); }
}
int dc_aimoff_status(float *elev, float *target, float *theta, float *e_up, float *e_level, float *e_down, int *pistol, long *applied) {
    ao_cfg();
    *elev = s_ao.elev; *target = s_ao.target; *theta = s_ao.theta; *applied = s_ao.applied;
    { static long n = 0; if (++n <= 50) fprintf(stderr, "aimoff status: reason %d seen %ld applied %ld last_actor %05X want %d target %.1f\n", s_ao.reason, s_ao.seen, s_ao.applied, s_ao.last_actor, s_ao_want, s_ao_target_deg); }
    *e_up = s_ao_ext.e_up; *e_level = s_ao_ext.e_level; *e_down = s_ao_ext.e_down; *pistol = s_ao_ext.pistol;
    { static long n = 0; extern uint64_t s_frame_count; if (++n <= 40) fprintf(stderr, "aimoff byte: want %d last %d writes %ld calls %ld age %u frames\n", s_ao_byte, s_ao_byte_last, s_ao_byte_writes, s_ao_byte_calls, (uint32_t)s_frame_count - s_ao_byte_frame); }
    return s_ao_on && s_ao_ext.valid;
}
