/* Direct body draw (DC_BODY_DIRECT=1).
 *
 * Regina's polygons normally travel the console's own route: the assembler
 * builds a packet from ROUNDED screen coordinates, the game decides from those
 * integers whether the polygon exists and where it lands in the sorting table,
 * and only what survives that reaches the GPU. Every step of that route can
 * lose a small polygon — which is why a face polygon disappears and the head
 * behind it shows through, more often the smaller the polygon is on screen.
 *
 * The character assembler hook (pt_dc_poolprim) sees every body primitive
 * BEFORE any of those decisions, together with the model vertex, the bone
 * matrix, the UVs, the palette and the texture page. This channel collects
 * them there and draws them straight into the rasterizer, with float positions
 * and true w. The game's own body primitives are then suppressed. Nothing
 * between the model data and the GPU can drop a polygon any more.
 */
#ifndef PT_BODY_H
#define PT_BODY_H

#include <stdint.h>

typedef struct {
    int      nv;              /* 3 or 4 */
    float    x[4], y[4], w[4];/* draw-space float position + view w (w < NEAR: x/y unusable) */
    float    vx[4], vy[4], vz[4]; /* VIEW-space position (re-aimed), for the near clip (2026-08-23) */
    int      player;          /* 1 = the player's body (the only one this channel draws) */
    int      u[4], v[4];      /* texel coords */
    uint32_t col[4];          /* gouraud colours, 0x00BBGGRR as the GP0 packet */
    uint16_t clut_x, clut_y;
    uint16_t tpage;
    int      semi;            /* semi-transparent flag from the packet command */
    int      semi_mode;       /* blend mode from the texture page */
    int      raw;             /* raw-texture flag */
    unsigned stamp;           /* assembly frame this prim belongs to (pgeo stamp):
                               * the flush can land MID-assembly of the next frame,
                               * so the steady shift must be matched by stamp */
    int      pass;            /* game draw pass (orchestrator camera load) it was built in */
    signed char bone[4];      /* 20.10.2026: her bone of each vertex (-1 unknown) - the in-between frames move arms/hands/head */
} PtBodyPrim;

#ifdef __cplusplus
extern "C" {
#endif

int  pt_body_direct(void);                 /* channel enabled? */
void pt_body_reset(void);                  /* start of frame */
void pt_body_new_frame(unsigned stamp);    /* capture of a new frame begins: drop an unflushed list */
int  pt_body_count(void);
const PtBodyPrim *pt_body_at(int i);
void pt_body_flush(void);                  /* implemented in gpu.c */

#ifdef __cplusplus
}
#endif

#endif /* PT_BODY_H */
