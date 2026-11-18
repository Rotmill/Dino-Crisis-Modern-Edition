/* Every gamepad watched, the one in use named (mod/src/runtime/dc_pads.c) - on SDL virtual gamepads, no hardware.
 * 18.11.2026 regression (Steam Deck with an 8BitDo Ultimate): a second gamepad was never opened, so its buttons
 * never reached the game. */
#include "dc_pads.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #c, SDL_GetError()); return 1; } } while (0)

static int plug(const char *name, Uint16 pid)
{
    SDL_VirtualJoystickDesc d;
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.vendor_id = 0x2DC8;                 /* 8BitDo */
    d.product_id = pid;
    d.name = name;
    return SDL_JoystickAttachVirtualEx(&d);
}

/* what the event queue says about the pads: the pad shown in use by the last event that shows one, -1 none */
static SDL_JoystickID drain(void)
{
    SDL_Event ev;
    SDL_JoystickID used = -1;
    SDL_PumpEvents();
    while (SDL_PollEvent(&ev)) {
        const SDL_JoystickID id = dc_pads_used_by(&ev);
        if (id >= 0) used = id;
    }
    return used;
}

int main(void)
{
    CHECK(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    const int i1 = plug("built-in controls", 0x0001), i2 = plug("8BitDo Ultimate", 0x3106);
    CHECK(i1 >= 0 && i2 >= 0 && SDL_IsGameController(i1) && SDL_IsGameController(i2));
    const SDL_JoystickID id1 = SDL_JoystickGetDeviceInstanceID(i1), id2 = SDL_JoystickGetDeviceInstanceID(i2);
    /* the test's own handles drive the virtual pads */
    SDL_Joystick *j1 = SDL_JoystickOpen(i1), *j2 = SDL_JoystickOpen(i2);
    CHECK(j1 && j2);

    CHECK(dc_pads_watch_all() == 2);
    CHECK(dc_pads_watch_all() == 2);                      /* no second handle for a watched pad */
    drain();

    /* the second pad's button: it is the one in use */
    SDL_JoystickSetVirtualButton(j2, SDL_CONTROLLER_BUTTON_A, 1);
    CHECK(drain() == id2);
    SDL_JoystickSetVirtualButton(j2, SDL_CONTROLLER_BUTTON_A, 0);
    CHECK(drain() == -1);                                 /* a release is not use */
    /* a stick that drifts is not use, a stick pushed is */
    SDL_JoystickSetVirtualAxis(j1, SDL_CONTROLLER_AXIS_LEFTX, 6000);
    CHECK(drain() == -1);
    SDL_JoystickSetVirtualAxis(j1, SDL_CONTROLLER_AXIS_LEFTX, -30000);
    CHECK(drain() == id1);
    CHECK(dc_pads_index_of(id2) == i2 && dc_pads_index_of(id1) == i1 && dc_pads_index_of(12345) == -1);

    /* the second pad leaves: one watched; it comes back (a new instance): two again */
    SDL_JoystickClose(j2);
    dc_pads_unwatch(id2);
    CHECK(SDL_JoystickDetachVirtual(i2) == 0);
    drain();
    CHECK(dc_pads_watch_all() == 1 && dc_pads_index_of(id2) == -1);
    const int i3 = plug("8BitDo Ultimate", 0x3106);
    CHECK(i3 >= 0 && dc_pads_watch_all() == 2);

    dc_pads_unwatch_all();
    SDL_JoystickClose(j1);

    /* ---- the original vibration: the DualShock motor bytes of a 0x42 poll, mapped by 0x4D ---- */
    {
        uint8_t sm, lg;
        const uint8_t unmapped[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        const uint8_t libpad[6] = { 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF };   /* the usual libpad map: small, large */
        const uint8_t swapped[6] = { 0x01, 0xFF, 0x00, 0xFF, 0xFF, 0xFF };
        const uint8_t both[6] = { 0x01, 0xC0, 0, 0, 0, 0 }, odd[6] = { 0x02, 0x40, 0x01, 0, 0, 0 };
        dc_pads_motors(unmapped, both, &sm, &lg);
        CHECK(sm == 0 && lg == 0);                             /* not set up by the game: still */
        dc_pads_motors(libpad, both, &sm, &lg);
        CHECK(sm == 1 && lg == 0xC0);
        dc_pads_motors(libpad, odd, &sm, &lg);
        CHECK(sm == 0 && lg == 0x40);                          /* the small motor reads bit 0 only */
        dc_pads_motors(swapped, odd, &sm, &lg);
        CHECK(sm == 1 && lg == 0x02);

        Uint16 lo, hi;
        dc_pads_rumble_levels(1, 255, 100, 16, &lo, &hi);
        CHECK(lo == 0xFFFF && hi == 0xA000);
        dc_pads_rumble_levels(0, 128, 50, 16, &lo, &hi);
        CHECK(lo == 128u * 257u / 2u && hi == 0);
        dc_pads_rumble_levels(1, 255, 0, 16, &lo, &hi);
        CHECK(lo == 0 && hi == 0);                             /* strength 0: off */
        dc_pads_rumble_levels(1, 255, 100, DC_PADS_RUMBLE_STALE_MS + 1, &lo, &hi);
        CHECK(lo == 0 && hi == 0);                             /* the game stopped polling (pause): the motors stop */
        dc_pads_rumble_levels(1, 999, 250, 0, &lo, &hi);
        CHECK(lo == 0xFFFF && hi == 0xA000);                   /* out of range: clamped */
    }
    SDL_Quit();
    puts("gamepad watch and vibration test: ok");
    return 0;
}
