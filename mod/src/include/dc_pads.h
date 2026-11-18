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

/* ---- the original vibration (18.11.2026) ----
 * Dino Crisis drives the DualShock motors: with 0x4D it says which byte of the 0x42 poll goes to which motor, then sends
 * the motor bytes with every poll. sio.c hands each poll here; main.cpp passes the levels to the active gamepad. */
/* the motors after one poll: the byte mapped 0x00 - the small motor, on when bit 0 is set; the byte mapped 0x01 - the
 * large one, 0..255. An unmapped pad (0xFF everywhere) does not vibrate. */
void dc_pads_motors(const uint8_t map[6], const uint8_t tx[6], uint8_t *small_on, uint8_t *large);
/* the host rumble for them: lo - the large motor (low frequency), hi - the small one (high); pct - strength 0..100;
 * since_poll_ms - time since the game last polled: past DC_PADS_RUMBLE_STALE_MS it stopped (pause, loading) - still */
#define DC_PADS_RUMBLE_STALE_MS 150u
void dc_pads_rumble_levels(int small_on, int large, int pct, Uint32 since_poll_ms, Uint16 *lo, Uint16 *hi);

#ifdef __cplusplus
}
#endif

#endif
