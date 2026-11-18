/* The PS1-style HUD (mod/src/runtime/dc_hud.c): when it shows, what it draws. No SDL, no game: the logic and the raster.
 * test-hud [out.raw]  - with a path, also writes the four states side by side (ARGB, 4*DC_HUD_W x DC_HUD_H) */
#include "dc_hud.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint32_t px[DC_HUD_W * DC_HUD_H];
static uint32_t at(int x, int y) { return px[y * DC_HUD_W + x]; }
static int lit(int x0, int y0, int x1, int y1, uint32_t c)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += at(x, y) == c;
    return n;
}

static DcHudIn world(int hp, int weapon, int mag)
{
    DcHudIn in;
    memset(&in, 0, sizeof in);
    in.visible = 1; in.hp = hp; in.hpmax = 1200; in.weapon = weapon; in.mag = mag; in.reserve = -1;
    return in;
}

static void run(DcHudState *s, DcHudIn in, float seconds, int always)
{
    for (float t = 0.f; t < seconds; t += 1.f / 60.f) dc_hud_tick(s, &in, 1.f / 60.f, always);
}

int main(int argc, char **argv)
{
    const uint32_t GREEN = 0xFF7FC24Au, AMBER = 0xFFE8A92Cu, RED = 0xFFD84038u, WHITE = 0xFFEEF1F3u;

    /* states */
    CHECK(dc_hud_state(1200, 1200) == 0 && dc_hud_state(801, 1200) == 0);
    CHECK(dc_hud_state(800, 1200) == 1 && dc_hud_state(401, 1200) == 1);
    CHECK(dc_hud_state(400, 1200) == 2 && dc_hud_state(0, 1200) == 2);

    /* quiet world: nothing changes, not aiming - nothing on screen */
    DcHudState s;
    memset(&s, 0, sizeof s);
    s.bleed_t = -1.f;
    run(&s, world(1200, 0x20, 12), 1.f, 0);
    CHECK(s.vis == 0.f && !dc_hud_raster(&s, px));

    /* a shot: shown at once, held 3 s, gone 0.5 s later */
    run(&s, world(1200, 0x20, 11), 0.3f, 0);
    CHECK(s.vis == 1.f);
    run(&s, world(1200, 0x20, 11), 2.5f, 0);
    CHECK(s.vis == 1.f);
    run(&s, world(1200, 0x20, 11), 1.0f, 0);
    CHECK(s.vis == 0.f);

    /* aiming keeps it */
    DcHudIn aim = world(1200, 0x20, 11);
    aim.aiming = 1;
    run(&s, aim, 6.f, 0);
    CHECK(s.vis == 1.f);
    run(&s, world(1200, 0x20, 11), 4.f, 0);
    CHECK(s.vis == 0.f);

    /* a bite (-100) shows it, no bleeding; then -1 steps: bleeding */
    run(&s, world(1100, 0x20, 11), 0.2f, 0);
    CHECK(s.vis == 1.f && s.bleed_t < 0.f);
    run(&s, world(1099, 0x20, 11), 0.1f, 0);
    CHECK(s.bleed_t >= 0.f);
    run(&s, world(1099, 0x20, 11), 2.f, 0);
    CHECK(s.bleed_t < 0.f);                                  /* stopped bleeding */

    /* a menu / a scene on top: hidden at once; a reload under it does not pop the panel afterwards */
    run(&s, world(1099, 0x20, 2), 0.2f, 0);
    CHECK(s.vis > 0.f);
    DcHudIn menu = world(1099, 0x20, 12);
    menu.visible = 0;
    run(&s, menu, 0.05f, 0);
    CHECK(s.vis == 0.f && !dc_hud_raster(&s, px));
    run(&s, menu, 4.f, 0);
    run(&s, world(1099, 0x20, 12), 0.5f, 0);
    CHECK(s.vis == 0.f);

    /* always shown */
    run(&s, world(1099, 0x20, 12), 0.5f, 1);
    CHECK(s.vis == 1.f);

    /* raster: Fine is green, the weapon label and the rounds are drawn */
    CHECK(dc_hud_raster(&s, px));
    CHECK(lit(4, 4, 60, 19, GREEN) > 40);                    /* heartbeat */
    CHECK(lit(4, 22, 30, 27, GREEN) > 10);                   /* FINE */
    CHECK(lit(68, 12, 92, 28, WHITE) > 30);                  /* "12", 3x scale */
    CHECK(at(0, 5) == 0xFF8A929Au && at(DC_HUD_W - 1, 5) == 0xFF3A3F45u);   /* bevel */
    /* Caution amber, Danger red; health blocks follow */
    run(&s, world(600, 0x20, 12), 0.2f, 1);
    dc_hud_raster(&s, px);
    CHECK(lit(4, 4, 60, 19, AMBER) > 40 && lit(4, 29, 64, 31, AMBER) == 5 * 2 * 5);
    run(&s, world(240, 0x20, 0), 0.2f, 1);
    dc_hud_raster(&s, px);
    CHECK(lit(4, 4, 60, 19, RED) > 40 && lit(4, 29, 64, 31, RED) == 5 * 2 * 2);
    CHECK(lit(68, 12, 92, 28, RED) > 30 && lit(68, 12, 92, 28, WHITE) == 0);   /* empty gun: red */
    /* no gun: health only */
    run(&s, world(1200, 0, -1), 0.2f, 1);
    dc_hud_raster(&s, px);
    CHECK(lit(66, 2, DC_HUD_W - 2, DC_HUD_H - 2, WHITE) == 0 && lit(66, 2, DC_HUD_W - 2, 10, 0xFFAAB2BAu) == 0);
    /* dead: flat line */
    run(&s, world(0, 0x20, 3), 0.2f, 1);
    dc_hud_raster(&s, px);
    CHECK(lit(4, 4, 60, 12, RED) == 0 && lit(4, 13, 60, 19, RED) == 0);

    /* ---- the rounds carried, learned at reloads ---- */
    {
        enum { BASE = 0x080000, SIZE = 0x1000, STACK = 0x0800, TIMER = 0x0900, OTHER = 0x0A00 };
        static uint8_t ram[0x200000], prev[SIZE];
        DcHudLearn l;
        memset(&l, 0, sizeof l);
        uint8_t *cur = ram + BASE;
        cur[STACK - 1] = 0x31;                                 /* item kind next to the count */
        cur[STACK] = 45;
        cur[TIMER] = 250;
        cur[OTHER] = 40;
#define FRAME(dm) do { memcpy(prev, cur, SIZE); \
                       cur[TIMER]--; if (dm > 0) cur[STACK] -= (uint8_t)(dm); \
                       dc_hud_learn_step(&l, prev, cur, BASE, SIZE, dm, BASE + 0x0C00, BASE + 0x0C10); } while (0)
        for (int i = 0; i < 30; i++) FRAME(0);
        /* a reload one round per frame: the timer falls by 1 on the same frames */
        for (int i = 0; i < 9; i++) FRAME(1);
        CHECK(l.addr == 0);                                    /* stack and timer both still in */
        for (int i = 0; i < 10; i++) FRAME(0);
        CHECK(l.addr == BASE + STACK && l.tag_off == -1 && l.tag == 0x31);
        CHECK(dc_hud_reserve(&l, ram, sizeof ram) == 45 - 9);
        /* the stack runs out and another item takes its slot: hidden, then learned again */
        cur[STACK - 1] = 0x07; cur[STACK] = 1;
        for (int i = 0; i < 119; i++) CHECK(dc_hud_reserve(&l, ram, sizeof ram) == -1 && l.addr);
        CHECK(dc_hud_reserve(&l, ram, sizeof ram) == -1 && l.addr);
        CHECK(dc_hud_reserve(&l, ram, sizeof ram) == -1 && !l.addr);
        /* one big reload (+6) where two cells drop by 6: needs the second reload to tell them apart */
        memset(&l, 0, sizeof l);
        cur[STACK - 1] = 0x31; cur[STACK] = 30; cur[OTHER] = 40;
        memcpy(prev, cur, SIZE); cur[STACK] -= 6; cur[OTHER] -= 6;
        dc_hud_learn_step(&l, prev, cur, BASE, SIZE, 6, 0, 0);
        CHECK(l.addr == 0 && l.ncand == 2);
        memcpy(prev, cur, SIZE); cur[STACK] -= 4;
        dc_hud_learn_step(&l, prev, cur, BASE, SIZE, 4, 0, 0);
        CHECK(l.addr == BASE + STACK);
        /* a cell in the skipped range (the gun's own array) is never taken */
        memset(&l, 0, sizeof l);
        cur[0x0C05] = 50;
        for (int k = 0; k < 2; k++) { memcpy(prev, cur, SIZE); cur[0x0C05] -= 2; cur[STACK] -= 2;
                                      dc_hud_learn_step(&l, prev, cur, BASE, SIZE, 2, BASE + 0x0C00, BASE + 0x0C10); }
        CHECK(l.addr == BASE + STACK);
#undef FRAME
    }

    if (argc > 1) {                                          /* preview: Fine, Caution (bleeding), Danger, empty gun */
        static uint32_t sheet[4 * DC_HUD_W * DC_HUD_H];
        const DcHudIn ins[4] = { world(1150, 0x20, 12), world(620, 0x10, 5), world(300, 0x30, 1), world(900, 0x20, 0) };
        for (int k = 0; k < 4; k++) {
            DcHudState p;
            memset(&p, 0, sizeof p);
            p.bleed_t = k == 1 ? 0.f : -1.f;
            run(&p, ins[k], 0.4f + 0.1f * k, 1);
            if (k == 1) p.bleed_t = 0.1f, p.t = 0.f;
            dc_hud_raster(&p, px);
            for (int y = 0; y < DC_HUD_H; y++)
                memcpy(&sheet[y * 4 * DC_HUD_W + k * DC_HUD_W], &px[y * DC_HUD_W], DC_HUD_W * 4);
        }
        FILE *f = fopen(argv[1], "wb");
        CHECK(f && fwrite(sheet, 4, 4 * DC_HUD_W * DC_HUD_H, f) == (size_t)(4 * DC_HUD_W * DC_HUD_H));
        fclose(f);
    }
    puts("hud test: ok");
    return 0;
}
