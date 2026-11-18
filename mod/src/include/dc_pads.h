/* dc_pads.h - every connected gamepad watched, so the one in use can take the input (dc_pads.c).
 * 18.11.2026 (Steam Deck with an 8BitDo Ultimate): Auto opened only the first gamepad SDL listed - on a Deck its own
 * controls - and a second pad was never read. main.cpp still reads one gamepad; these say which one is being used. */
#ifndef DC_PADS_H_INCLUDED
#define DC_PADS_H_INCLUDED

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DC_PADS_MAX 8
/* the stick or trigger travel that counts as use (a resting stick that drifts does not) */
#define DC_PADS_AXIS_USE 20000

/* opens every gamepad not watched yet (SDL sends events only for open ones); returns how many are watched */
int  dc_pads_watch_all(void);
/* a gamepad was removed: its handle is closed */
void dc_pads_unwatch(SDL_JoystickID id);
void dc_pads_unwatch_all(void);
/* the gamepad an event shows in use - a button pressed, a stick or trigger pushed far; -1 none */
SDL_JoystickID dc_pads_used_by(const SDL_Event *ev);
/* the device index of an instance (for SDL_GameControllerOpen), -1 when it is gone or not a gamepad */
int  dc_pads_index_of(SDL_JoystickID id);
/* 0 when DC_PAD_FOLLOW=0: the first gamepad only, as before */
int  dc_pads_follow_on(void);

#ifdef __cplusplus
}
#endif

#endif
