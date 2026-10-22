/*
 * pgxp.h — sub-pixel / perspective vertex cache (implemented in gte.cpp).
 *
 * The GTE projects vertices to a precise 16.16 screen position and a depth,
 * then truncates the position to the integer SXY the game packs into GPU
 * primitives. PGXP caches the precise position + depth keyed by that integer
 * SXY so the rasterizer can recover sub-pixel coordinates (hi-res, jitter-free)
 * and depth (perspective-correct texturing).
 */
#ifndef PSXRECOMP_PGXP_H
#define PSXRECOMP_PGXP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Clear the cache. (The cache normally persists across frames.) */
void pgxp_clear(void);

/* Store the precise projection for a vertex (called from the GTE). */
void pgxp_write(int sx, int sy, float fx, float fy, float w);

/* Recover the precise projection for an integer screen vertex.
 * Returns 1 and fills fx/fy (sub-pixel screen) and w (depth) on hit, else 0. */
int pgxp_lookup(int sx, int sy, float* fx, float* fy, float* w);

/* Projection constants of the game's GTE (H, OFX, OFY), kept by the RTPS hook. */
extern float g_pt_proj_h, g_pt_proj_ofx, g_pt_proj_ofy;

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PGXP_H */
