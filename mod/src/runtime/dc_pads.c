/* dc_pads.c - every connected gamepad watched (dc_pads.h). SDL sends button and axis events only for an opened
 * gamepad, so each one is opened here; main.cpp keeps its own handle to the active one (SDL counts the opens). */
#include "dc_pads.h"

#include <stdlib.h>

static SDL_GameController *s_watch[DC_PADS_MAX];

int dc_pads_follow_on(void)
{
    static int en = -1;
    if (en < 0) { const char *e = SDL_getenv("DC_PAD_FOLLOW"); en = (e && *e == '0') ? 0 : 1; }
    return en;
}

static SDL_JoystickID watch_id(int k)
{
    SDL_Joystick *j = s_watch[k] ? SDL_GameControllerGetJoystick(s_watch[k]) : NULL;
    return j ? SDL_JoystickInstanceID(j) : -1;
}

int dc_pads_watch_all(void)
{
    const int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj; i++) {
        if (!SDL_IsGameController(i)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int have = 0, free_k = -1;
        for (int k = 0; k < DC_PADS_MAX; k++) {
            if (!s_watch[k]) { if (free_k < 0) free_k = k; }
            else if (watch_id(k) == id) have = 1;
        }
        if (!have && free_k >= 0) s_watch[free_k] = SDL_GameControllerOpen(i);
    }
    int n = 0;
    for (int k = 0; k < DC_PADS_MAX; k++) n += s_watch[k] != NULL;
    return n;
}

void dc_pads_unwatch(SDL_JoystickID id)
{
    for (int k = 0; k < DC_PADS_MAX; k++)
        if (s_watch[k] && (watch_id(k) == id || watch_id(k) < 0)) {
            SDL_GameControllerClose(s_watch[k]);
            s_watch[k] = NULL;
        }
}

void dc_pads_unwatch_all(void)
{
    for (int k = 0; k < DC_PADS_MAX; k++)
        if (s_watch[k]) { SDL_GameControllerClose(s_watch[k]); s_watch[k] = NULL; }
}

SDL_JoystickID dc_pads_used_by(const SDL_Event *ev)
{
    if (!ev) return -1;
    if (ev->type == SDL_CONTROLLERBUTTONDOWN) return ev->cbutton.which;
    if (ev->type == SDL_CONTROLLERAXISMOTION && abs((int)ev->caxis.value) >= DC_PADS_AXIS_USE) return ev->caxis.which;
    return -1;
}

void dc_pads_motors(const uint8_t map[6], const uint8_t tx[6], uint8_t *small_on, uint8_t *large)
{
    *small_on = 0;
    *large = 0;
    for (int i = 0; i < 6; i++) {
        if (map[i] == 0x00) *small_on = (tx[i] & 0x01) ? 1 : 0;
        else if (map[i] == 0x01) *large = tx[i];
    }
}

void dc_pads_rumble_levels(int small_on, int large, int pct, Uint32 since_poll_ms, Uint16 *lo, Uint16 *hi)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (large < 0) large = 0;
    if (large > 255) large = 255;
    if (since_poll_ms > DC_PADS_RUMBLE_STALE_MS) small_on = 0, large = 0;
    /* the small motor of a DualShock is a light buzz: 5/8 of the full high-frequency rumble */
    *lo = (Uint16)((unsigned)large * 257u * (unsigned)pct / 100u);
    *hi = small_on ? (Uint16)(0xA000u * (unsigned)pct / 100u) : 0;
}

int dc_pads_index_of(SDL_JoystickID id)
{
    const int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj; i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) return SDL_IsGameController(i) ? i : -1;
    return -1;
}
