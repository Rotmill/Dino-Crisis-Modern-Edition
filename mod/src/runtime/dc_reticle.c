/* dc_reticle.c — перекрестие средствами SDL, без Dear ImGui.
 *
 * Раньше прицел рисовался в foreground-список ImGui. Это тянуло чужую
 * библиотеку в релизную сборку ради единственной картинки, при том что
 * отладочные окна (F2/F8/F9) в релизе не нужны вовсе. Здесь та же геометрия
 * выложена на SDL_RenderGeometry — тем же способом, каким её рисовал сам
 * ImGui, так что вид не меняется.
 *
 * Логика не тронута: форма выбирается по оружию, прицел расходится и сходится
 * по кривой сведения, на цели краснеет и утолщается. Числа и коэффициенты
 * перенесены как есть — это подгонка игрока, а не свободные параметры.
 *
 * Координаты — в логическом пространстве рендерера (854x480 в широком кадре).
 * ImGui-версии приходилось знать про несовпадение своего 640x480 с этим
 * пространством; здесь такой поправки не нужно, центр берётся из самого
 * рендерера.
 */

#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dc_reticle.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── примитивы ────────────────────────────────────────────────────────────
 * SDL не умеет ни толстых линий, ни заливок по дуге, поэтому и то и другое
 * собирается из треугольников — ровно как это делает ImGui внутри себя. */

typedef struct { SDL_Color c; } RetCol;

static void tri_push(SDL_Vertex *v, int *n, float x, float y, SDL_Color c)
{
    v[*n].position.x = x; v[*n].position.y = y;
    v[*n].color = c;
    v[*n].tex_coord.x = 0.f; v[*n].tex_coord.y = 0.f;
    (*n)++;
}

/* Отрезок толщиной th: четырёхугольник из двух треугольников. */
static void ret_line(SDL_Renderer *r, float x0, float y0, float x1, float y1,
                     float th, SDL_Color c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    SDL_Vertex v[6]; int n = 0;
    tri_push(v, &n, x0 + nx, y0 + ny, c);
    tri_push(v, &n, x1 + nx, y1 + ny, c);
    tri_push(v, &n, x1 - nx, y1 - ny, c);
    tri_push(v, &n, x0 + nx, y0 + ny, c);
    tri_push(v, &n, x1 - nx, y1 - ny, c);
    tri_push(v, &n, x0 - nx, y0 - ny, c);
    SDL_RenderGeometry(r, NULL, v, n, NULL, 0);
}

/* Залитый круг: веер треугольников. */
static void ret_disc(SDL_Renderer *r, float cx, float cy, float rad, int seg, SDL_Color c)
{
    if (seg < 6) seg = 6;
    if (seg > 32) seg = 32;
    SDL_Vertex v[32 * 3]; int n = 0;
    for (int i = 0; i < seg; i++) {
        float a0 = (float)(2.0 * M_PI) * (float)i / (float)seg;
        float a1 = (float)(2.0 * M_PI) * (float)(i + 1) / (float)seg;
        tri_push(v, &n, cx, cy, c);
        tri_push(v, &n, cx + cosf(a0) * rad, cy + sinf(a0) * rad, c);
        tri_push(v, &n, cx + cosf(a1) * rad, cy + sinf(a1) * rad, c);
    }
    SDL_RenderGeometry(r, NULL, v, n, NULL, 0);
}

/* Дуга толщиной th от a0 до a1: ломаная из отрезков, как PathArcTo+PathStroke. */
static void ret_arc(SDL_Renderer *r, float cx, float cy, float rad,
                    float a0, float a1, int seg, float th, SDL_Color c)
{
    if (seg < 2) seg = 2;
    float px = cx + cosf(a0) * rad, py = cy + sinf(a0) * rad;
    for (int i = 1; i <= seg; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)seg;
        float x = cx + cosf(a) * rad, y = cy + sinf(a) * rad;
        ret_line(r, px, py, x, y, th, c);
        px = x; py = y;
    }
}

/* ── сама отрисовка ─────────────────────────────────────────────────────── */

void dc_reticle_draw(SDL_Renderer *r, const DcReticle *o)
{
    if (!r || !o) return;

    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    if (lw <= 0 || lh <= 0) SDL_GetRendererOutputSize(r, &lw, &lh);
    if (lw <= 0 || lh <= 0) return;
    const float cx = (float)lw * 0.5f, cy = (float)lh * 0.5f;

    SDL_BlendMode old_blend;
    SDL_GetRenderDrawBlendMode(r, &old_blend);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    SDL_Color col;
    if (o->red) { col.r = 255; col.g = 70;  col.b = 60;  col.a = o->locked ? 245 : 200; }
    else        { col.r = 235; col.g = 235; col.b = 235; col.a = o->locked ? 210 : 120; }

    const float rs   = o->size;
    const float open = o->open;
    const float tb   = o->on_target ? 0.5f : 0.0f;

    switch (o->shape) {
    case DC_RET_DOT:
        ret_disc(r, cx, cy, 1.6f, 20, col);
        break;

    case DC_RET_ARCS: {
        /* ДРОБОВИК: две встречные дуги «( )». Не сходится — урон дробовика
         * зависит от дистанции, а не от сведения, и анимация обещала бы
         * механику, которой в игре нет. Припаркована на половине хода, размах
         * колебания — пятая часть радиуса. */
        float R  = (rs * 1.55f + o->sg_park * rs * 2.60f) * (1.0f + o->sg_anim * (1.0f - o->focus_raw));
        float ha = 0.45f, th = 1.4f + tb;
        ret_arc(r, cx, cy, R, (float)M_PI - ha, (float)M_PI + ha, 12, th, col);
        ret_arc(r, cx, cy, R, -ha, ha, 12, th, col);
        break;
    }

    case DC_RET_RING: {
        /* ГРАНАТОМЁТ: четыре диагональных штриха и ничего больше — читается
         * как площадь, а не точка, и не закрывает цель. */
        float R = rs * 1.15f + open * rs * 1.60f;
        float ti = R, lt = rs * 0.70f, tht = 1.2f + tb;
        for (int k = 0; k < 4; k++) {
            float a = 0.78539816f + (float)k * 1.57079633f;
            float ax = cosf(a), ay = sinf(a);
            ret_line(r, cx + ax * ti, cy + ay * ti,
                        cx + ax * (ti + lt), cy + ay * (ti + lt), tht, col);
        }
        break;
    }

    case DC_RET_DART: {
        /* ТРАНКВИЛИЗАТОР: три штриха, нижнего нет — намеренно несимметричная
         * метка, чтобы не спутать с картечью. */
        float g = rs * 0.50f + open * rs * 1.40f;
        float lt = rs * 0.90f, ls = rs * 0.60f, th = 1.0f + tb;
        ret_line(r, cx, cy - g - lt, cx, cy - g, th, col);
        ret_line(r, cx - g - ls, cy, cx - g, cy, th, col);
        ret_line(r, cx + g, cy, cx + g + ls, cy, th, col);
        break;
    }

    default: {
        /* ПИСТОЛЕТ и всё неопознанное: крест из четырёх штрихов. */
        float g = rs * 0.55f + open * rs * 1.9f;
        float l = rs * 0.75f, th = 1.0f + tb;
        ret_line(r, cx - g - l, cy, cx - g, cy, th, col);
        ret_line(r, cx + g, cy, cx + g + l, cy, th, col);
        ret_line(r, cx, cy - g - l, cx, cy - g, th, col);
        ret_line(r, cx, cy + g, cx, cy + g + l, th, col);
        break;
    }
    }

    SDL_SetRenderDrawBlendMode(r, old_blend);
}

/* ── сообщения (квиксейвы): прежний шрифт, размер и место как в деве ───────
 * 13.10.2026 вечер, игрок: «верни старый шрифт, только размеры и положение
 * сделай такие же, как в деве». Шрифт прежний - системный Bahnschrift (нет его,
 * тогда Segoe UI, затем Arial: тот же поиск, что у слоя интерфейса в dc_font.c).
 * Он читается из папки шрифтов Windows и с модом не раздаётся, поэтому в
 * THIRD-PARTY.txt его нет. Размер и место - как в деве: там сообщения (dc_osd)
 * рисует Dear ImGui шрифтом по умолчанию 13 px в логическом пространстве
 * рендерера, последними в кадре, от x = 10 с шагом строк 17; заглавные у него
 * высотой 8 px, базовая линия на 11 ниже верха строки. Здесь заглавная H того
 * же роста на той же линии, в том же пространстве и в тот же момент кадра;
 * тень, цвет и угасание те же (main.cpp, правка release_edits.py). Знаки
 * растеризуются в пикселях экрана (на 1080p заглавная 18 px) и встают на целые
 * пиксели, поэтому растяжка холста их не мылит. Прежний вывод релиза - наш слой
 * 4:3 крупным кеглем с обводкой: крупнее, а в 16:9 сдвинут к центру кадра. */

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../deps/stb/stb_truetype.h"   /* stb_truetype v1.26, public domain: своя статическая копия, как в dc_font.c */

#define OSD_CAP   8.0f     /* рост заглавных в деве: H с 3 по 11 px от верха строки */
#define OSD_BASE 11.0f     /* базовая линия ниже верха строки */
#define OSD_N    95        /* знаки 32..126 */
#define OSD_ROW  2048      /* ширина атласа, дальше - следующий ряд */

static stbtt_fontinfo  s_osd_fi;            /* файл шрифта в памяти живёт до конца игры: s_osd_fi указывает в него */
static int             s_osd_font;          /* 0 - не искали, 1 - есть, -1 - нет ни одного */
static float           s_osd_capu;          /* рост заглавной H в единицах шрифта */

static int osd_font(void)
{
    static const char *const names[] = { "bahnschrift.ttf", "segoeui.ttf", "arial.ttf" };
    char win[260] = "C:/Windows";
    if (s_osd_font) return s_osd_font > 0;
    s_osd_font = -1;
    { const char *e = SDL_getenv("WINDIR"); if (e && *e) SDL_strlcpy(win, e, sizeof win); }   /* сразу к себе: буфер SDL общий */
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s/Fonts/%s", win, names[i]);
        SDL_RWops *rw = SDL_RWFromFile(path, "rb");
        if (!rw) continue;
        const Sint64 n = SDL_RWsize(rw);
        unsigned char *buf = (n > 0 && n < ((Sint64)64 << 20)) ? (unsigned char *)malloc((size_t)n) : NULL;
        const size_t got = buf ? SDL_RWread(rw, buf, 1, (size_t)n) : 0;
        SDL_RWclose(rw);
        const int off = (buf && got == (size_t)n) ? stbtt_GetFontOffsetForIndex(buf, 0) : -1;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (off >= 0 && stbtt_InitFont(&s_osd_fi, buf, off) &&
            stbtt_GetCodepointBox(&s_osd_fi, 'H', &x0, &y0, &x1, &y1) && y1 > 0) {
            s_osd_capu = (float)y1; s_osd_font = 1;
            return 1;
        }
        free(buf);
    }
    return 0;
}

typedef struct { int ax, ay, x0, y0, w, h; float adv; } OsdGlyph;   /* x0, y0, w, h - пиксели экрана от пера и базовой линии */
static OsdGlyph      s_og[OSD_N];
static SDL_Texture  *s_osd_tex;
static SDL_Renderer *s_osd_texr;
static float         s_osd_k;               /* под какой масштаб собран атлас */
static int           s_osd_aw, s_osd_ah;

/* Пикселей экрана в логическом пикселе - как SDL раскладывает логический холст по окну. */
static float osd_scale(SDL_Renderer *r)
{
    int ow = 0, oh = 0, lw = 0, lh = 0;
    float k = 1.0f;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    if (lw > 0 && lh > 0 && SDL_GetRendererOutputSize(r, &ow, &oh) == 0 && ow > 0 && oh > 0) {
        const float kx = (float)ow / (float)lw, ky = (float)oh / (float)lh;
        k = kx < ky ? kx : ky;
    } else {
        float sx = 1.0f, sy = 1.0f;
        SDL_RenderGetScale(r, &sx, &sy);
        if (sy > 0.0f) k = sy;
    }
    return k < 0.5f ? 0.5f : (k > 12.0f ? 12.0f : k);
}

/* Атлас знаков 32..126 в пикселях экрана: белый, альфа - покрытие; пересобирается при смене масштаба окна. */
static int osd_atlas(SDL_Renderer *r, float k)
{
    if (s_osd_tex && s_osd_texr == r && fabsf(s_osd_k - k) < 0.001f) return 1;
    if (s_osd_tex) { SDL_DestroyTexture(s_osd_tex); s_osd_tex = NULL; }
    if (!osd_font()) return 0;
    const float sl = OSD_CAP / s_osd_capu, sp = sl * k;     /* единицы шрифта -> логические пиксели, -> пиксели экрана */
    int W = 1, cx = 1, cy = 1, rowh = 0;                      /* знаки через пиксель, с пикселем по краям */
    for (int i = 0; i < OSD_N; i++) {
        OsdGlyph *g = &s_og[i];
        int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointHMetrics(&s_osd_fi, 32 + i, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&s_osd_fi, 32 + i, sp, sp, &x0, &y0, &x1, &y1);
        g->adv = (float)adv * sl; g->x0 = x0; g->y0 = y0; g->w = x1 - x0; g->h = y1 - y0; g->ax = g->ay = 0;
        if (g->w <= 0 || g->h <= 0) { g->w = g->h = 0; continue; }
        if (cx + g->w + 1 > OSD_ROW) { cx = 1; cy += rowh + 1; rowh = 0; }
        g->ax = cx; g->ay = cy;
        cx += g->w + 1;
        if (cx > W) W = cx;
        if (g->h > rowh) rowh = g->h;
    }
    const int H = cy + rowh + 1;
    unsigned char *cov = (unsigned char *)calloc((size_t)W * (size_t)H, 1);
    Uint32 *px = (Uint32 *)malloc((size_t)W * (size_t)H * sizeof(Uint32));
    if (!cov || !px) { free(cov); free(px); return 0; }
    for (int i = 0; i < OSD_N; i++)
        if (s_og[i].w > 0)
            stbtt_MakeCodepointBitmap(&s_osd_fi, cov + (size_t)s_og[i].ay * (size_t)W + (size_t)s_og[i].ax,
                                      s_og[i].w, s_og[i].h, W, sp, sp, 32 + i);
    for (size_t j = 0; j < (size_t)W * (size_t)H; j++)
        px[j] = ((Uint32)cov[j] << 24) | 0x00FFFFFFu;          /* ABGR8888: белый, альфа - покрытие знака */
    s_osd_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (s_osd_tex) {
        SDL_UpdateTexture(s_osd_tex, NULL, px, W * (int)sizeof(Uint32));
        SDL_SetTextureBlendMode(s_osd_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(s_osd_tex, SDL_ScaleModeLinear);
        s_osd_texr = r; s_osd_k = k; s_osd_aw = W; s_osd_ah = H;
    }
    free(cov); free(px);
    return s_osd_tex != NULL;
}

void dc_osd_text(SDL_Renderer *r, float x, float y, const char *s, int cr, int cg, int cb, int ca)
{
    if (!r || !s || !*s || ca <= 0) return;
    const float k = osd_scale(r);
    if (!osd_atlas(r, k)) return;
    if (ca > 255) ca = 255;
    SDL_Color c; c.r = (Uint8)cr; c.g = (Uint8)cg; c.b = (Uint8)cb; c.a = (Uint8)ca;
    const float iw = 1.0f / (float)s_osd_aw, ih = 1.0f / (float)s_osd_ah, ik = 1.0f / k;
    const float base = floorf((floorf(y) + OSD_BASE) * k + 0.5f);   /* базовая линия - на целом пикселе экрана */
    float pen = floorf(x);                                           /* перо от целого x, как у ImGui */
    SDL_Vertex v[6 * 64]; int n = 0;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch == '\n' || ch == '\r') continue;
        if (ch < 32 || ch > 126) ch = '?';
        const OsdGlyph *g = &s_og[ch - 32];
        if (g->w > 0) {
            const float px0 = floorf(pen * k + 0.5f) + (float)g->x0, py0 = base + (float)g->y0;   /* знак - на целых пикселях */
            const float x1 = px0 * ik, y1 = py0 * ik, x2 = (px0 + (float)g->w) * ik, y2 = (py0 + (float)g->h) * ik;
            const float u1 = (float)g->ax * iw, v1 = (float)g->ay * ih;
            const float u2 = (float)(g->ax + g->w) * iw, v2 = (float)(g->ay + g->h) * ih;
            const float qx[6] = { x1, x2, x2, x1, x2, x1 }, qy[6] = { y1, y1, y2, y1, y2, y2 };
            const float qu[6] = { u1, u2, u2, u1, u2, u1 }, qv[6] = { v1, v1, v2, v1, v2, v2 };
            for (int q = 0; q < 6; q++) {
                v[n].position.x = qx[q]; v[n].position.y = qy[q];
                v[n].color = c;
                v[n].tex_coord.x = qu[q]; v[n].tex_coord.y = qv[q];
                n++;
            }
            if (n >= 6 * 64) { SDL_RenderGeometry(r, s_osd_tex, v, n, NULL, 0); n = 0; }
        }
        pen += g->adv;
    }
    if (n) SDL_RenderGeometry(r, s_osd_tex, v, n, NULL, 0);
}
