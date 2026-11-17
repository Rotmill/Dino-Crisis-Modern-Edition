/* dc_hud.h - the PS1-style HUD: health, bleeding and the rounds in the gun (dc_hud.c). */
#ifndef DC_HUD_H_INCLUDED
#define DC_HUD_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the panel in PS1 pixels (the game image is 240 lines tall: one HUD pixel = one PS1 pixel) */
#define DC_HUD_W 108
#define DC_HUD_H 34

typedef struct {
    int visible;      /* the world, nothing on top of it (no menu, scene, movie, inventory, game over) */
    int hp, hpmax;    /* Regina's health and its maximum (0xB93A4 / 0xB93A6) */
    int weapon;       /* equipped weapon id without the modification bits: 0x10 shotgun, 0x20 handgun, 0x30 grenade gun; 0 none */
    int mag;          /* rounds loaded in it, -1 unknown */
    int reserve;      /* rounds carried for it, -1 unknown (not mapped yet) */
    int aiming;       /* the weapon is raised */
} DcHudIn;

typedef struct {
    DcHudIn in;       /* what is drawn */
    float vis;        /* 0..1: fade */
    float hold;       /* seconds left fully shown */
    float phase;      /* heartbeat scroll */
    float t;          /* time, for the blink */
    float bleed_t;    /* seconds since the last bleeding tick (-1 none) */
    int have;         /* a previous input exists */
    int last_hp, last_mag, last_weapon;
} DcHudState;

/* options (dc_user.cfg, hidden page: hud.on, hud.always) */
extern int g_opt_hud_on;
extern int g_opt_hud_always;

/* one frame of logic: what changed shows the panel for DC_HUD_HOLD seconds, then it fades */
void dc_hud_tick(DcHudState *s, const DcHudIn *in, float dt, int always);
/* the panel into a DC_HUD_W x DC_HUD_H ARGB8888 buffer (0 = transparent); 0 when there is nothing to show */
int  dc_hud_raster(const DcHudState *s, uint32_t *px);
/* health state: 0 fine, 1 caution, 2 danger */
int  dc_hud_state(int hp, int hpmax);
/* the panel at the bottom right of an out_w x out_h logical frame (SDL; one HUD pixel = out_h / 240) */
struct SDL_Renderer;
void dc_hud_draw(struct SDL_Renderer *r, const DcHudState *s, float out_w, float out_h);

#ifdef __cplusplus
}
#endif

#endif
