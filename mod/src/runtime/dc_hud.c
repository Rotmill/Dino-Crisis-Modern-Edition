/* dc_hud.c - a HUD in the look of the PS1 game (18.11.2026).
 *
 * Player: "a proper HUD with the bullets and the health"; "PS1-themed, in the same graphics, so it does not look
 * strange". So the panel is drawn in PS1 pixels - a 3x5 bitmap font, a checker dither under the transparency, a 1-pixel
 * bevel like the game's own windows, a short palette - into a small buffer, and that buffer is scaled to the screen
 * with nearest filtering by the same factor as the game image (one HUD pixel = one PS1 pixel, whatever the window or
 * render size). Nothing is smoothed.
 *
 *   left:  a heartbeat trace (faster in Caution, faster still in Danger), FINE / CAUTION / DANGER, ten health blocks,
 *          a blinking drop while she bleeds (health falling 1 point at a time, as bleeding does: 1 every 0.6 s)
 *   right: the weapon (HG / SG / GG) and the rounds loaded in it; "/n" carried once that address is known
 *
 * It shows for 3 s when something changes (damage, a shot, a reload, another weapon) and while the weapon is raised,
 * then fades; hud.always keeps it. Off by default (hud.on): the original hides Regina's health on purpose.
 * The logic and the raster here know nothing of SDL or the game's memory (main.cpp feeds DcHudIn), so they are tested
 * on their own (linux/tests/test_hud.c). */
#include "dc_hud.h"

#include <string.h>

int g_opt_hud_on = 0;
int g_opt_hud_always = 0;

#define DC_HUD_HOLD   3.0f      /* seconds fully shown after a change */
#define DC_HUD_IN     8.0f      /* fade in per second (0.125 s) */
#define DC_HUD_OUT    2.0f      /* fade out per second (0.5 s) */
#define DC_HUD_BLEED  1.5f      /* the drop stays this long after a bleeding tick (ticks come every 0.6 s) */

int dc_hud_state(int hp, int hpmax)
{
    if (hpmax <= 0) return 0;
    if (hp * 3 > hpmax * 2) return 0;     /* above 2/3 */
    if (hp * 3 > hpmax) return 1;         /* above 1/3 */
    return 2;
}

void dc_hud_tick(DcHudState *s, const DcHudIn *in, float dt, int always)
{
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.25f) dt = 0.25f;
    s->t += dt;
    const int st = dc_hud_state(in->hp, in->hpmax);
    s->phase += dt * (st == 0 ? 1.0f : st == 1 ? 1.6f : 2.4f);
    if (s->phase > 1000.f) s->phase -= 1000.f;
    if (s->bleed_t >= 0.f) {
        s->bleed_t += dt;
        if (s->bleed_t > DC_HUD_BLEED) s->bleed_t = -1.f;
    }
    if (!in->visible || in->hpmax <= 0) {
        /* under a menu, a scene or the inventory: hidden at once; values follow quietly (a reload in the inventory
         * does not pop the panel up afterwards) */
        s->vis = 0.f;
        s->hold = 0.f;
        s->last_hp = in->hp; s->last_mag = in->mag; s->last_weapon = in->weapon;
        s->have = in->hpmax > 0;
        s->in = *in;
        return;
    }
    int poke = 0;
    if (s->have) {
        const int dhp = s->last_hp - in->hp;
        if (dhp == 1 || dhp == 2) s->bleed_t = 0.f;            /* bleeding: a point at a time */
        if (dhp != 0) poke = 1;
        if (in->mag != s->last_mag || in->weapon != s->last_weapon) poke = 1;
    }
    if (in->aiming) poke = 1;
    if (poke) s->hold = DC_HUD_HOLD;
    else if (s->hold > 0.f) s->hold -= dt;
    const float want = (always || s->hold > 0.f) ? 1.f : 0.f;
    if (s->vis < want) { s->vis += dt * DC_HUD_IN; if (s->vis > want) s->vis = want; }
    else if (s->vis > want) { s->vis -= dt * DC_HUD_OUT; if (s->vis < want) s->vis = want; }
    s->last_hp = in->hp; s->last_mag = in->mag; s->last_weapon = in->weapon;
    s->have = 1;
    s->in = *in;
}

/* ---------------------------------------------------------------- rounds carried (learned) */
static int in_skip(uint32_t a, uint32_t lo, uint32_t hi) { return a >= lo && a <= hi; }

static void learn_take(DcHudLearn *l, const uint8_t *prev, const uint8_t *cur, uint32_t base)
{
    const uint32_t a = l->cand[0], o = a - base;
    /* the item's kind: the neighbour that held still and is not zero */
    if (cur[o - 1] == prev[o - 1] && cur[o - 1] != 0) { l->tag_off = -1; l->tag = cur[o - 1]; }
    else if (cur[o + 1] == prev[o + 1] && cur[o + 1] != 0) { l->tag_off = 1; l->tag = cur[o + 1]; }
    else l->tag_off = 0;
    l->addr = a;
    l->bad = 0;
}

void dc_hud_learn_step(DcHudLearn *l, const uint8_t *prev, const uint8_t *cur, uint32_t base, uint32_t size,
                       int mag_delta, uint32_t skip_lo, uint32_t skip_hi)
{
    if (l->addr) return;
    if (mag_delta <= 0) {
        /* no reload this frame: whatever moved is not the ammo item (a timer, a counter that fell with the reload) */
        int k = 0;
        for (int i = 0; i < l->ncand; i++) {
            const uint32_t o = l->cand[i] - base;
            if (cur[o] == prev[o]) l->cand[k++] = l->cand[i];
        }
        if (l->ncand && !k) l->events = 0;                     /* every candidate moved: start again at the next reload */
        l->ncand = k;
        if (l->ncand == 1 && l->events >= 2) learn_take(l, prev, cur, base);
        return;
    }
    uint32_t found[DC_HUD_LEARN_MAX];
    int nf = 0, over = 0;
    for (uint32_t o = 1; o + 1 < size; o++) {
        if (prev[o] < (uint8_t)mag_delta || prev[o] - cur[o] != mag_delta) continue;
        const uint32_t a = base + o;
        if (in_skip(a, skip_lo, skip_hi)) continue;
        if (l->events) {                                        /* already learning: only the candidates still standing */
            int keep = 0;
            for (int i = 0; i < l->ncand && !keep; i++) keep = l->cand[i] == a;
            if (!keep) continue;
        }
        if (nf == DC_HUD_LEARN_MAX) { over = 1; break; }
        found[nf++] = a;
    }
    if (over) { l->ncand = 0; l->events = 0; return; }      /* too many: this reload says nothing */
    if (!nf) { l->ncand = 0; l->events = 0; return; }
    memcpy(l->cand, found, sizeof(uint32_t) * (size_t)nf);
    l->ncand = nf;
    l->events++;
    if (l->ncand == 1 && l->events >= 2) learn_take(l, prev, cur, base);
}

int dc_hud_reserve(DcHudLearn *l, const uint8_t *ram, uint32_t ram_size)
{
    if (!l->addr || l->addr + 1 >= ram_size || l->addr < 1) return -1;
    if (l->tag_off && ram[l->addr + l->tag_off] != l->tag) {
        if (++l->bad > 120) {                                   /* 2 s: the stack is gone or moved - learn it again */
            l->addr = 0; l->ncand = 0; l->events = 0; l->tag_off = 0; l->bad = 0;
        }
        return -1;
    }
    l->bad = 0;
    return ram[l->addr];
}

/* ---------------------------------------------------------------- raster */
static const char *const k_glyph[] = {
    /* 3x5, row by row */
    "0111101101101111", "1010110010010111", "2111001111100111", "3111001111001111", "4101101111001001",
    "5111100111001111", "6111100111101111", "7111001010010010", "8111101111101111", "9111101111001111",
    "/001001010100100", "A010101111101101", "C111100100100111", "D110101101101110", "E111100110100111",
    "F111100110100100", "G111100101101111", "H101101111101101", "I111010010010111", "N110101101101101",
    "O111101101101111", "R110101110101101", "S111100111001111", "T111010010010010", "U101101101101111",
    "-000000111000000", NULL
};

static const char *glyph(char c)
{
    for (int i = 0; k_glyph[i]; i++)
        if (k_glyph[i][0] == c) return k_glyph[i] + 1;
    return NULL;
}

static void put(uint32_t *px, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < DC_HUD_W && y < DC_HUD_H) px[y * DC_HUD_W + x] = c;
}

static void rect(uint32_t *px, int x, int y, int w, int h, uint32_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(px, x + i, y + j, c);
}

static int text(uint32_t *px, const char *s, int x, int y, uint32_t c, int sc)
{
    for (; *s; s++) {
        const char *g = glyph(*s);
        if (g)
            for (int i = 0; i < 15; i++)
                if (g[i] == '1') rect(px, x + (i % 3) * sc, y + (i / 3) * sc, sc, sc, c);
        x += 4 * sc;
    }
    return x;
}

#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static const uint32_t k_state_col[3] = { ARGB(255, 0x7f, 0xc2, 0x4a), ARGB(255, 0xe8, 0xa9, 0x2c), ARGB(255, 0xd8, 0x40, 0x38) };
static const char *const k_state_name[3] = { "FINE", "CAUTION", "DANGER" };

int dc_hud_raster(const DcHudState *s, uint32_t *px)
{
    memset(px, 0, sizeof(uint32_t) * DC_HUD_W * DC_HUD_H);
    if (s->vis <= 0.f || s->in.hpmax <= 0) return 0;
    const int W = DC_HUD_W, H = DC_HUD_H;
    /* panel: a translucent dark fill with a checker dither on it, the bevel of the game's windows */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = ((x + y) & 1) ? ARGB(150, 8, 10, 14) : ARGB(200, 0, 0, 0);
    rect(px, 0, 0, W, 1, ARGB(255, 0x8a, 0x92, 0x9a));
    rect(px, 0, 0, 1, H, ARGB(255, 0x8a, 0x92, 0x9a));
    rect(px, 0, H - 1, W, 1, ARGB(255, 0x3a, 0x3f, 0x45));
    rect(px, W - 1, 0, 1, H, ARGB(255, 0x3a, 0x3f, 0x45));

    const int st = dc_hud_state(s->in.hp, s->in.hpmax);
    const uint32_t c = k_state_col[st];
    /* heartbeat monitor */
    rect(px, 4, 4, 56, 15, ARGB(255, 0x0c, 0x1a, 0x0c));
    for (int x = 4; x < 60; x += 7) rect(px, x, 4, 1, 15, ARGB(255, 0x17, 0x30, 0x17));
    const int base = 12, off = (int)(s->phase * 30.f) % 28;
    for (int i = 0; i < 56; i++) {
        const int p = (i + off) % 28;
        const int dy = p == 9 ? -1 : p == 10 ? -6 : p == 11 ? 5 : p == 12 ? -2 : 0;
        if (s->in.hp <= 0) { put(px, 4 + i, base, c); continue; }   /* flat line */
        const int a = dy < 0 ? dy : 0, b = dy > 0 ? dy : 0;
        rect(px, 4 + i, base + a, 1, b - a + 1, c);
    }
    text(px, k_state_name[st], 4, 22, c, 1);
    /* health blocks */
    int blocks = s->in.hp <= 0 ? 0 : (s->in.hp * 10 + s->in.hpmax - 1) / s->in.hpmax;
    if (blocks > 10) blocks = 10;
    for (int i = 0; i < 10; i++) rect(px, 4 + i * 6, 29, 5, 2, i < blocks ? c : ARGB(255, 0x2a, 0x2f, 0x34));
    /* bleeding drop, blinking */
    if (s->bleed_t >= 0.f && ((int)(s->t * 3.f) & 1) == 0) {
        const uint32_t red = k_state_col[2];
        put(px, 54, 22, red); rect(px, 53, 23, 3, 1, red); rect(px, 52, 24, 5, 2, red); rect(px, 53, 26, 3, 1, red);
    }
    /* weapon */
    rect(px, 64, 4, 1, 26, ARGB(255, 0x3a, 0x3f, 0x45));
    const char *wl = s->in.weapon == 0x10 ? "SG" : s->in.weapon == 0x20 ? "HG" : s->in.weapon == 0x30 ? "GG" : NULL;
    if (wl) {
        text(px, wl, 68, 4, ARGB(255, 0xaa, 0xb2, 0xba), 1);
        char n[8];
        const int m = s->in.mag < 0 ? -1 : s->in.mag > 99 ? 99 : s->in.mag;
        if (m < 0) { n[0] = '-'; n[1] = '-'; n[2] = 0; }
        else { n[0] = (char)('0' + m / 10); n[1] = (char)('0' + m % 10); n[2] = 0; }
        text(px, n, 68, 12, m == 0 ? k_state_col[2] : ARGB(255, 0xee, 0xf1, 0xf3), 3);
        if (s->in.reserve >= 0) {
            char r[8];
            int v = s->in.reserve > 999 ? 999 : s->in.reserve, k = 0;
            r[k++] = '/';
            if (v >= 100) r[k++] = (char)('0' + v / 100);
            if (v >= 10) r[k++] = (char)('0' + (v / 10) % 10);
            r[k++] = (char)('0' + v % 10);
            r[k] = 0;
            text(px, r, 92, 23, ARGB(255, 0x8a, 0x92, 0x9a), 1);
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- on screen (SDL) */
#ifndef DC_HUD_NO_SDL
#include <SDL.h>

static SDL_Renderer *s_tex_r;
static SDL_Texture  *s_tex;

/* the panel at the bottom right of the out_w x out_h logical frame, one HUD pixel = out_h / 240 */
void dc_hud_draw(SDL_Renderer *r, const DcHudState *s, float out_w, float out_h)
{
    static uint32_t px[DC_HUD_W * DC_HUD_H];
    if (!r || !dc_hud_raster(s, px)) return;
    if (r != s_tex_r) { s_tex = NULL; s_tex_r = r; }     /* a new renderer freed the old texture itself */
    if (!s_tex) {
        s_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DC_HUD_W, DC_HUD_H);
        if (!s_tex) return;
        SDL_SetTextureBlendMode(s_tex, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(s_tex, SDL_ScaleModeNearest);
#endif
    }
    SDL_UpdateTexture(s_tex, NULL, px, DC_HUD_W * 4);
    SDL_SetTextureAlphaMod(s_tex, (Uint8)(s->vis * 255.f + 0.5f));
    float u = out_h / 240.f;
    if (u >= 1.f) u = (float)(int)(u + 0.5f);           /* whole logical units per pixel: no uneven pixels */
    const SDL_FRect d = { out_w - (DC_HUD_W + 6) * u, out_h - (DC_HUD_H + 6) * u, DC_HUD_W * u, DC_HUD_H * u };
    SDL_RenderCopyF(r, s_tex, NULL, &d);
}
#endif
