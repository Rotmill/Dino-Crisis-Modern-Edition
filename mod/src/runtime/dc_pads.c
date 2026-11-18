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

int dc_pads_index_of(SDL_JoystickID id)
{
    const int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj; i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) return SDL_IsGameController(i) ? i : -1;
    return -1;
}
