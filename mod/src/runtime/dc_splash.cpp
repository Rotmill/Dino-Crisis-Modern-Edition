/* dc_splash — экраны запуска мода: выбор языка, «о моде» (04.10 вечер 7) и «Раскладка управления».
 *
 * Дизайн — проект Claude Design «Раскладка управления» (10.09.2026). Игрок:
 * «надо именно то, что сделал дизайн, перенести». Поэтому экраны не
 * пересобираются нашим шрифтом, а рендерятся браузером в PNG теми же шрифтами
 * и значками (design/controls_layout/render/render.py, тексты — i18n.js); здесь
 * — показ этих картинок поверх кадра и навигация. Разрешение картинок —
 * 1920x1080 (игрок: «1080p, не больше»). Языков восемь, как у установщика;
 * индекс — ui.lang (dc_settings.h), коды — k_splash_lang_code.
 *
 * Кадр складывается из слоёв, все в системе кадра дизайна 1920x1080, который
 * вписывается в окно с сохранением 16:9:
 *   фон        радиальный градиент дизайна, считается здесь (браузер его дизерит,
 *              и с ним PNG почти не сжимается);
 *   геймпад    ui_gamepad.png — схема из дизайна без логотипа PS, одна на все
 *              языки, кладётся в блок геймпада (dc_splash_layout.h);
 *   подписи    вкладка «Геймпад» собирается ЗДЕСЬ по input.ini (игрок 10.09:
 *              «если переназначить в конфиге кнопки, это отобразится в окне
 *              раскладки»): что делает каждая кнопка, говорит main.cpp
 *              (pad_slot_actions), значки и подписи берутся кусками из атласов
 *              ui_padicons.png и ui_padlbl_<код>.png и раскладываются по
 *              правилам вёрстки дизайна (pad_layout); вкладки — спрайт
 *              ui_ctrl_<код>_tabs.png (отдельный блок «Разворот на 180°» под
 *              геймпадом снят 15.09 ночью: разворот теперь на R3 — подпись у кнопки);
 *   слой       ui_ctrl_<код>_kb.png — вкладка клавиатуры целиком на прозрачном
 *              фоне (клавиатура в input.ini не переназначается); выбор языка:
 *              ui_lang_<код>_<box|go>.png (список закрыт, фокус на поле или на
 *              «Продолжить») + ui_lang_list_off.png, поверх которого строка под
 *              курсором берётся из ui_lang_list_on.png;
 *   подсказки  ui_ft_<boot|menu|lang>_<pad|kb>_<код>.png — полоса у нижнего
 *              края: чем игрок пользуется, те кнопки и показываем.
 *
 * Пока экран открыт, машина стоит — так же, как под нашим меню: обёртка
 * sdl_vblank_present (main.cpp) держит кадр внутри колбэка vblank.
 * Нет картинок — экран закрывается сам: игра не должна зависнуть на чёрном.
 *
 * PNG декодирует компонент Windows (WIC), так в моде не прибавляется чужого кода.
 */
#include "dc_splash.h"
#include "dc_settings.h"          /* коды DC_MENU_* */
#include "dc_splash_layout.h"     /* прямоугольники, места подписей, коды языков — сгенерированы render.py */
#include "dc_menu_atlas.h"        /* атлас строк меню настроек — тоже render.py */
#include "dc_binds.h"             /* 04.10 (6): назначения кнопок — строки подстраниц и своя раскладка на вкладке клавиатуры */

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <objbase.h>
#  include <wincodec.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "windowscodecs.lib")
#    pragma comment(lib, "ole32.lib")
#  endif
#endif

/* В релизе наши данные вшиты в exe (dc_embedded.c, tools/embed_assets.py);
 * в дереве разработки его нет — там картинки читаются из assets/ui. */
#if defined(MCM_NO_IMGUI) || defined(DC_EMBEDDED_ASSETS)
extern "C" const unsigned char *dc_embedded(const char *name, unsigned *size);
#  define DC_SPLASH_EMBEDDED 1
#endif

static_assert(DC_SPLASH_LANGS == DC_LANG_COUNT, "языки картинок (render.py) и меню (dc_settings.h) разошлись");
static_assert(DC_SPLASH_SLOTS == DCS_COUNT, "кнопки схемы (render.py, ctrl.html) и DCS_* (dc_splash.h) разошлись");
static_assert(DC_SPLASH_LBL_NONE_F == DCA_COUNT, "подписи атласа (slots.html) и DCA_* (dc_splash.h) разошлись");

/* ⭐ 04.10.2026 (вечер, 7), игрок: «в начальные экраны после экрана с выбором языка сделай экран — дисклеймер о том, что
 * это мод и он бесплатен и т.д. А так напиши, что опции можно менять в модерн опциях». SPL_DISC — «о моде»
 * (ui_disc_<код>.png из design/.../disc.html): только на первом запуске, между выбором языка и раскладкой; кнопка и
 * подсказка — «Продолжить». DC_BOOT_DISC=0 — без него, как было. */
enum { SPL_NONE = 0, SPL_LANG = 1, SPL_CTRL = 2, SPL_DISC = 3 };
enum { N_LANG = DC_SPLASH_LANGS };

static DcSplashHooks s_h;
static int      s_screen;        /* что на экране сейчас */
static int      s_next_ctrl;     /* после выбора языка — раскладка */
static int      s_next_disc;     /* 04.10 (7): после выбора языка — «о моде», потом раскладка */
static int      s_from_menu;     /* открыто из меню настроек: «назад» — туда */
static int      s_tab;           /* 0 — геймпад, 1 — клавиатура + мышь */
static int      s_dev_pad;       /* последним жали геймпад — подсказки под него */
/* выбор языка: выпадающий список */
static int      s_sel;           /* выбранный язык — он же уже применён */
static int      s_hl;            /* строка под курсором в раскрытом списке */
static int      s_open;          /* список раскрыт */
static int      s_focus;         /* 0 — поле списка, 1 — кнопка «Продолжить» */
static unsigned s_closed_tick;
static float    s_mouse_x = -1.0f, s_mouse_y = -1.0f;

/* ---- картинки -------------------------------------------------------------- */

#ifdef _WIN32
static IWICImagingFactory *s_wic;

/* PNG -> BGRA 8888 без премножения (как SDL_PIXELFORMAT_ARGB8888 в памяти). */
static bool png_decode(const unsigned char *data, size_t n, std::vector<unsigned char> &px, int *w, int *h)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   /* как у SDL */
    const bool uninit = SUCCEEDED(hr);   /* S_OK/S_FALSE — парный CoUninitialize; иначе COM уже поднят в другом режиме */
    if (!s_wic)
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&s_wic));
    IWICStream *st = nullptr; IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *fr = nullptr; IWICFormatConverter *cv = nullptr;
    bool ok = false;
    if (s_wic &&
        SUCCEEDED(s_wic->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromMemory((BYTE *)data, (DWORD)n)) &&
        SUCCEEDED(s_wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(s_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT W = 0, H = 0;
        if (SUCCEEDED(cv->GetSize(&W, &H)) && W && H) {
            px.resize((size_t)W * H * 4);
            if (SUCCEEDED(cv->CopyPixels(nullptr, W * 4, (UINT)px.size(), px.data()))) {
                *w = (int)W; *h = (int)H; ok = true;
            }
        }
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (uninit) CoUninitialize();
    return ok;
}
#else
static bool png_decode(const unsigned char *, size_t, std::vector<unsigned char> &, int *, int *)
{
    return false;   /* декодер — компонент Windows; на других системах экраны просто не показываются */
}
#endif

/* Байты файла: сначала вшитые в exe (релиз), потом assets/ui рядом с exe или
 * выше по дереву сборки (DC_UI_DIR — своя папка). */
static bool asset_bytes(const char *name, std::vector<unsigned char> &buf,
                        const unsigned char **p, size_t *n)
{
#ifdef DC_SPLASH_EMBEDDED
    unsigned sz = 0;
    const unsigned char *e = dc_embedded(name, &sz);
    if (e && sz) { *p = e; *n = sz; return true; }
#endif
    const char *env = SDL_getenv("DC_UI_DIR");
    const std::string dirs[] = { env ? std::string(env) + "/" : std::string(),
                                 "assets/ui/", "../../assets/ui/", "../../../assets/ui/" };
    for (const std::string &d : dirs) {
        if (d.empty()) continue;
        const std::string path = d + name;
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bool ok = false;
        if (len > 0) {
            buf.resize((size_t)len);
            ok = std::fread(buf.data(), 1, (size_t)len, f) == (size_t)len;
        }
        std::fclose(f);
        if (ok) { *p = buf.data(); *n = buf.size(); return true; }
    }
    return false;
}

/* Маленький кэш текстур: вкладки и строки списка переключаются без повторного
 * декодирования. Вкладка «Геймпад» держит шесть картинок сразу. Всё
 * освобождается при закрытии экрана. */
struct Tex { std::string key; SDL_Texture *t = nullptr; unsigned used = 0; };
static Tex          s_cache[12];
static unsigned     s_use_tick;
static SDL_Texture *s_bg;
static std::vector<std::string> s_missing;   /* не грузить заново каждый кадр */

static void tex_free_all(void)
{
    for (Tex &c : s_cache) { if (c.t) SDL_DestroyTexture(c.t); c = Tex(); }
    if (s_bg) { SDL_DestroyTexture(s_bg); s_bg = nullptr; }
}

/* 18.10.2026: устройство вывода сброшено (main.cpp, g_dc_ren_gen) - прежние текстуры пусты, кэш - заново */
extern int g_dc_ren_gen;
static void tex_gen_check(void)
{
    static int gen = 0;
    if (gen != g_dc_ren_gen) { tex_free_all(); gen = g_dc_ren_gen; }
}

static SDL_Texture *tex_get(SDL_Renderer *r, const char *name)
{
    tex_gen_check();
    for (Tex &c : s_cache)
        if (c.t && c.key == name) { c.used = ++s_use_tick; return c.t; }
    for (const std::string &m : s_missing) if (m == name) return nullptr;

    const Uint64 t0 = SDL_GetPerformanceCounter();
    std::vector<unsigned char> file, px;
    const unsigned char *p = nullptr; size_t n = 0; int w = 0, h = 0;
    if (!asset_bytes(name, file, &p, &n) || !png_decode(p, n, px, &w, &h)) {
        std::fprintf(stderr, "splash: нет картинки %s (assets/ui или вшитые данные)\n", name);
        s_missing.push_back(name);
        return nullptr;
    }
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) {
        std::fprintf(stderr, "splash: текстура %s %dx%d: %s\n", name, w, h, SDL_GetError());
        s_missing.push_back(name);
        return nullptr;
    }
    SDL_UpdateTexture(t, nullptr, px.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);

    Tex *slot = &s_cache[0];
    for (Tex &c : s_cache) {
        if (!c.t) { slot = &c; break; }
        if (c.used < slot->used) slot = &c;
    }
    if (slot->t) SDL_DestroyTexture(slot->t);
    slot->key = name; slot->t = t; slot->used = ++s_use_tick;
    std::fprintf(stderr, "splash: %s %dx%d за %.0f мс\n", name, w, h,
                 (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    return t;
}

/* Фон дизайна: radial-gradient(120% 90% at 50% 45%, #0d1114 0%, #000 75%) на
 * блоке 1920x1080. Перепад всего 20 уровней, поэтому с упорядоченным
 * дизерингом 4x4 — без него на большом экране видны ступени. */
static SDL_Texture *bg_get(SDL_Renderer *r)
{
    tex_gen_check();
    if (s_bg) return s_bg;
    const int W = 1920, H = 1080;
    static const unsigned char bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
                                               { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    std::vector<Uint32> px((size_t)W * H);
    const float cx = 0.5f * W, cy = 0.45f * H, rx = 1.2f * W, ry = 0.9f * H;
    for (int y = 0; y < H; y++) {
        const float dy = (y + 0.5f - cy) / ry;
        for (int x = 0; x < W; x++) {
            const float dx = (x + 0.5f - cx) / rx;
            float k = 1.0f - std::sqrt(dx * dx + dy * dy) / 0.75f;
            if (k < 0.0f) k = 0.0f;
            const float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f;
            const Uint32 R = (Uint32)(13.0f * k + d), G = (Uint32)(17.0f * k + d), B = (Uint32)(20.0f * k + d);
            px[(size_t)y * W + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
        }
    }
    s_bg = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (s_bg) {
        SDL_UpdateTexture(s_bg, nullptr, px.data(), W * 4);
        SDL_SetTextureBlendMode(s_bg, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(s_bg, SDL_ScaleModeLinear);
    }
    return s_bg;
}

/* ---- схема геймпада по input.ini ------------------------------------------- */

/* Что делает кнопка схемы: маска 1<<DCA_* от main.cpp. Без ручки — раскладка
 * дизайна (она же input.ini по умолчанию). */
static unsigned slot_mask(int slot)
{
    if (s_h.pad_slot_actions) return s_h.pad_slot_actions(slot);
    static const unsigned def[DCS_COUNT] = {
        1u << DCA_AIM, 1u << DCA_AIM, 1u << DCA_MOVEMENT, 1u << DCA_MOVE,               /* 15.09 ночь: L1 = L2, R1 = R2 */
        1u << DCA_FIRE, 1u << DCA_FIRE, 1u << DCA_INVENTORY, 1u << DCA_RUN_HOLD,
        (1u << DCA_INVENTORY) | (1u << DCA_BACK), 1u << DCA_ACTION, 1u << DCA_CAMERA,   /* 12.09: B = предметы + назад */
        0u, 1u << DCA_PAUSE, 1u << DCA_TOGGLE_RUN, 1u << DCA_QUICK_TURN,                /* 15.09 ночь: R3 — разворот */
    };
    return def[slot];
}

/* 04.10 (6), игрок: «геймпады у людей разные». Значок кнопки — по типу пада: у Nintendo на местах △ □ ○ ✕ свои буквы
 * (X Y A B), значки — из того же атласа (k_splash_icon_n); остальные кнопки и все прочие пады — как в дизайне. */
static int pad_style_now(void) { return s_h.pad_style ? s_h.pad_style() : 0; }
static const float *pad_icon(int slot, int dim)
{
    if (pad_style_now() == 1) {
        switch (slot) {
        case DCS_TRI: return k_splash_icon_n[0][dim];
        case DCS_SQR: return k_splash_icon_n[1][dim];
        case DCS_CIR: return k_splash_icon_n[2][dim];
        case DCS_CRS: return k_splash_icon_n[3][dim];
        default: break;
        }
    }
    return k_splash_icon[slot][dim];
}
static const char *pad_dev_code(void) { return pad_style_now() == 1 ? "padn" : "pad"; }
static void kb_custom_present(SDL_Renderer *r, int L, float sc, float ox, float oy);   /* своя раскладка — ниже, у меню */

/* Строка схемы: значок кнопки и подпись — одно действие или несколько через
 * « / » («Бег, пока держишь / Назад»). Координаты — левый верх рамки элемента
 * в кадре 1920x1080 при сдвиге 0. */
struct PadRow {
    unsigned mask;
    int   dim;                        /* не назначена: значок и подпись приглушены */
    int   n;                          /* кусков подписи */
    int   lbl[2 * DCA_COUNT];         /* номера в атласе подписей, с разделителями */
    float lx[2 * DCA_COUNT], ly[2 * DCA_COUNT];
    float ix, iy;                     /* значок */
};

/* Вёрстка строк — правила дизайна (ctrl.html): строка — значок и подпись через
 * зазор, выровненные по центру по вертикали; слева от блока геймпада строка
 * прижата к нему правым краем, справа — левым, по вертикали стоит на отметке
 * доли высоты блока; сверху и снизу — по центру на отметке доли ширины. Места,
 * зазоры и отступы сняты со стилей строк дизайна (k_splash_place). Возвращает
 * сдвиг влево: как в ctrl.html fit(), если строка вылезает за поле страницы,
 * блок, подписи и «Разворот» уезжают на вылет. Проверка против снимка дизайна —
 * render/check_pad.py (та же вёрстка на Python). */
static float pad_layout(int L, PadRow *rows)
{
    const float *box = k_splash_box[L];
    float right = box[0] + box[2];
    for (int s = 0; s < DCS_COUNT; s++) {
        PadRow &r = rows[s];
        const DcSplashPlace &p = k_splash_place[s];
        r.mask = slot_mask(s);
        r.n = 0;
        for (int a = 0; a < DCA_COUNT; a++) {
            if (!(r.mask & (1u << a))) continue;
            if (r.n) r.lbl[r.n++] = DC_SPLASH_LBL_SEP;
            r.lbl[r.n++] = a;
        }
        r.dim = (r.n == 0);
        if (r.dim) r.lbl[r.n++] = (s == DCS_TOUCH) ? DC_SPLASH_LBL_NONE_M : DC_SPLASH_LBL_NONE_F;
        float lw = 0.0f, lh = 0.0f;
        for (int i = 0; i < r.n; i++) {
            const float *q = k_splash_lbl[L][r.lbl[i]];
            lw += q[2];
            lh = std::fmax(lh, q[3]);
        }
        const float *ic = pad_icon(s, r.dim);
        const float rw = ic[2] + p.gap + lw, rh = std::fmax(ic[3], lh);
        float x, cy;
        switch (p.side) {
        case 'L': x = box[0] - p.margin - rw;              cy = box[1] + p.pct * box[3]; break;
        case 'R': x = box[0] + box[2] + p.margin;          cy = box[1] + p.pct * box[3]; break;
        case 'T': x = box[0] + p.pct * box[2] - rw * 0.5f; cy = box[1] - p.margin - rh * 0.5f; break;
        default:  x = box[0] + p.pct * box[2] - rw * 0.5f; cy = box[1] + box[3] + p.margin + rh * 0.5f; break;
        }
        right = std::fmax(right, x + rw);
        /* По вертикали строка сперва встаёт на целый пиксель, дети — от неё: так
         * рисует Chrome (строка сдвинута transform: translateY(-50%)), и у
         * подписей рядом с плашками L2/R2 (35 px против 30) полпикселя уходят
         * вниз, как в дизайне. По горизонтали каждый кусок округляется сам —
         * значок строки слева встаёт ровно к её правому краю. Сверка с
         * эталоном дизайна — render/check_pad.py. */
        const float ry = std::floor(cy - rh * 0.5f + 0.5f);
        float lx = p.label_first ? x : x + ic[2] + p.gap;
        r.ix = p.label_first ? x + lw + p.gap : x;
        r.iy = ry + (rh - ic[3]) * 0.5f;
        for (int i = 0; i < r.n; i++) {
            const float *q = k_splash_lbl[L][r.lbl[i]];
            r.lx[i] = lx;
            r.ly[i] = ry + (rh - q[3]) * 0.5f;
            lx += q[2];
        }
    }
    const float over = right - DC_SPLASH_PAGE_RIGHT;
    return over > 0.0f ? std::ceil(over) : 0.0f;
}

/* Кусок атласа q (рамка элемента) — в точку (x, y) кадра дизайна. С полями в
 * 2 пикселя: чернила букв и сглаживание значков выходят за рамку элемента, а
 * соседи в атласе стоят далеко. Место — к целому пикселю кадра, как у PNG-слоёв. */
static void blit_piece(SDL_Renderer *r, SDL_Texture *t, const float *q, float x, float y,
                       float sc, float ox, float oy)
{
    const int P = 2;
    const int x0 = (int)std::floor(q[0]) - P, y0 = (int)std::floor(q[1]) - P;
    const int x1 = (int)std::ceil(q[0] + q[2]) + P, y1 = (int)std::ceil(q[1] + q[3]) + P;
    const SDL_Rect src = { x0, y0, x1 - x0, y1 - y0 };
    const float dx = std::floor(x + ((float)x0 - q[0]) + 0.5f);
    const float dy = std::floor(y + ((float)y0 - q[1]) + 0.5f);
    const SDL_FRect d = { ox + dx * sc, oy + dy * sc, (float)src.w * sc, (float)src.h * sc };
    SDL_RenderCopyF(r, t, &src, &d);
}

/* ---- состояние ------------------------------------------------------------- */

static int lang_now(void)
{
    int l = s_h.get_lang ? s_h.get_lang() : 0;
    return (l >= 0 && l < N_LANG) ? l : 0;
}
static int pad_now(void) { return s_h.pad_connected ? s_h.pad_connected() : 0; }

static void open_ctrl(void)
{
    s_screen = SPL_CTRL;
    s_tab = s_dev_pad ? 0 : 1;          /* вкладка того, чем игрок пользуется */
    char m[DCS_COUNT * 9 + 1];
    int k = 0;
    m[0] = 0;
    for (int s = 0; s < DCS_COUNT; s++)
        k += std::snprintf(m + k, sizeof m - (size_t)k, "%s%x", s ? "," : "", slot_mask(s));
    std::fprintf(stderr, "splash: раскладка управления (%s, вкладка %s, язык %s; схема из input.ini, маски DCA по DCS: %s)\n",
                 s_from_menu ? "из меню" : "запуск", s_tab ? "клавиатура" : "геймпад",
                 k_splash_lang_code[lang_now()], m);
}

static void open_lang(void)
{
    s_screen = SPL_LANG;
    s_sel = s_hl = lang_now();          /* по умолчанию английский — ui.lang=0 */
    s_open = 0;
    s_focus = 0;
}

/* Выбор строки списка: язык применяется сразу — меню, подсказки и следующий
 * экран уже на нём. */
static void lang_pick(int i)
{
    s_sel = s_hl = i;
    if (s_h.set_lang) s_h.set_lang(i);
    std::fprintf(stderr, "splash: язык %s\n", k_splash_lang_code[i]);
}

/* 04.10 (7): «о моде» — бесплатный фанатский мод, не Capcom, настройки — в MODERN OPTIONS */
static int disc_on(void)
{
    const char *e = SDL_getenv("DC_BOOT_DISC");
    return !(e && *e == '0');
}
/* 18.10.2026 (6), игрок: «modern options - когда выставляешь русский язык, разве они не по-русски называются? подставляй
 * правильный вариант». Пункт родного меню OPTION называется по версии игры, а не по ui.lang: MODERN OPTIONS, а в русской
 * (VIT / Акелла) - «НАСТРОЙКИ МОДА» (шрифт игры). Экран «о моде» называет его так же: ui_disc_<код>_rumenu.png и своя
 * кнопка k_splash_disc_go_rumenu. Установщик русской версии ставит здесь 1 (Installer\_source\versions\ru_vit\port_spec.py);
 * DC_DISC_RUMENU=1 / 0 - показать другой вид для проверки. */
static const int k_disc_rumenu_ver = 0;
static int disc_rumenu(void)
{
    const char *e = SDL_getenv("DC_DISC_RUMENU");
    return (e && (*e == '0' || *e == '1')) ? (*e == '1') : k_disc_rumenu_ver;
}
static void open_disc(void)
{
    s_screen = SPL_DISC;
    std::fprintf(stderr, "splash: о моде (язык %s)\n", k_splash_lang_code[lang_now()]);
}
static void disc_done(void)
{
    if (s_next_ctrl) { s_next_ctrl = 0; open_ctrl(); }
    else dc_splash_close("о моде прочитано");
}

/* «Продолжить» (или «назад» = пропустить с тем, что выбрано). */
static void lang_done(void)
{
    if (s_h.lang_chosen) s_h.lang_chosen();
    if (s_next_disc) { s_next_disc = 0; open_disc(); }
    else if (s_next_ctrl) { s_next_ctrl = 0; open_ctrl(); }
    else dc_splash_close("язык выбран");
}

extern "C" void dc_splash_init(const DcSplashHooks *h)
{
    if (h) s_h = *h;
}

extern "C" void dc_splash_boot(int show_lang)
{
    s_from_menu = 0;
    s_dev_pad = pad_now();
    if (show_lang) {
        s_next_ctrl = 1;
        s_next_disc = disc_on();
        open_lang();
        std::fprintf(stderr, "splash: выбор языка\n");
    } else if (disc_on()) {
        s_next_ctrl = 1;
        s_next_disc = 0;
        open_disc();
    } else {
        open_ctrl();
    }
}

extern "C" void dc_splash_open_controls(int from_menu)
{
    s_from_menu = from_menu ? 1 : 0;
    s_next_ctrl = 0;
    s_next_disc = 0;
    s_dev_pad = pad_now();
    open_ctrl();
}

extern "C" void dc_splash_open_lang(void)
{
    s_from_menu = 0;
    s_next_ctrl = 0;
    s_next_disc = 0;
    s_dev_pad = pad_now();
    open_lang();
}

extern "C" void dc_splash_open_disc(void)              /* 04.10 (7): отдельно — для отладочного порта */
{
    s_from_menu = 0;
    s_next_ctrl = 0;
    s_next_disc = 0;
    s_dev_pad = pad_now();
    open_disc();
}

extern "C" int dc_splash_active(void) { return s_screen != SPL_NONE; }
extern "C" unsigned dc_splash_closed_tick(void) { return s_closed_tick; }

extern "C" void dc_splash_close(const char *why)
{
    if (!s_screen) return;
    s_screen = SPL_NONE;
    s_next_ctrl = 0;
    s_next_disc = 0;
    s_open = 0;
    s_closed_tick = SDL_GetTicks();
    tex_free_all();
    s_missing.clear();
    std::fprintf(stderr, "splash: закрыт (%s)\n", why ? why : "");
}

extern "C" int dc_splash_action(int act, int from_pad)
{
    if (!s_screen) return 0;
    s_dev_pad = from_pad ? 1 : 0;
    if (s_screen == SPL_LANG) {
        if (s_open) {                                   /* раскрытый список */
            switch (act) {
            case DC_MENU_UP: case DC_MENU_LEFT: case DC_MENU_PAGE_PREV:    s_hl = (s_hl + N_LANG - 1) % N_LANG; break;
            case DC_MENU_DOWN: case DC_MENU_RIGHT: case DC_MENU_PAGE_NEXT: s_hl = (s_hl + 1) % N_LANG; break;
            case DC_MENU_OK:   lang_pick(s_hl); s_open = 0; s_focus = 1; break;   /* дальше — «Продолжить» */
            case DC_MENU_BACK: s_open = 0; s_hl = s_sel; break;
            default: return 0;
            }
            return 1;
        }
        switch (act) {                                  /* список закрыт */
        case DC_MENU_UP:   s_focus = 0; break;
        case DC_MENU_DOWN: s_focus = 1; break;
        case DC_MENU_LEFT: case DC_MENU_PAGE_PREV:      /* на поле — листать языки, не раскрывая */
            if (s_focus == 0) lang_pick((s_sel + N_LANG - 1) % N_LANG);
            break;
        case DC_MENU_RIGHT: case DC_MENU_PAGE_NEXT:
            if (s_focus == 0) lang_pick((s_sel + 1) % N_LANG);
            break;
        case DC_MENU_OK:
            if (s_focus == 0) { s_open = 1; s_hl = s_sel; }
            else lang_done();
            break;
        case DC_MENU_BACK: lang_done(); break;          /* пропустить — с тем языком, что выбран */
        default: return 0;
        }
        return 1;
    }
    if (s_screen == SPL_DISC) {                         /* 04.10 (7): «о моде» — только «Продолжить» */
        switch (act) {
        case DC_MENU_OK:
        case DC_MENU_BACK: disc_done(); break;
        case DC_MENU_UP: case DC_MENU_DOWN: case DC_MENU_LEFT: case DC_MENU_RIGHT:
        case DC_MENU_PAGE_PREV: case DC_MENU_PAGE_NEXT: break;
        default: return 0;
        }
        return 1;
    }
    switch (act) {
    case DC_MENU_LEFT:  s_tab = 0; break;
    case DC_MENU_RIGHT: s_tab = 1; break;
    case DC_MENU_PAGE_PREV:
    case DC_MENU_PAGE_NEXT: s_tab ^= 1; break;
    case DC_MENU_OK:
    case DC_MENU_BACK:  dc_splash_close(s_from_menu ? "назад в меню" : "дальше в игру"); break;
    case DC_MENU_UP:
    case DC_MENU_DOWN:  break;
    default: return 0;
    }
    return 1;
}

/* Вписать кадр дизайна 1920x1080 в логическую область: масштаб и сдвиг. */
static void fit(int lw, int lh, float *sc, float *ox, float *oy)
{
    *sc = std::fmin((float)lw / 1920.0f, (float)lh / 1080.0f);
    *ox = ((float)lw - 1920.0f * *sc) * 0.5f;
    *oy = ((float)lh - 1080.0f * *sc) * 0.5f;
}

/* Кадр дизайна 1920x1080 — прямо в пиксели окна, мимо логического размера
 * рендерера (854x480). Через него кадр ложился в 1918.5x1079.2 со сдвигом на
 * 0.75 px: вся картинка перемасштабировалась и мылилась (сверка меню с
 * макетом 10.09 — буквы расходились каймой, сильнее к низу кадра). Теперь на
 * 1080p — пиксель в пиксель. Мышь по-прежнему считает в логических
 * координатах (fit): разница меньше пикселя кадра. */
struct OutFrame { int lw, lh; float sx, sy; SDL_Rect vp; };
static void frame_begin(SDL_Renderer *r, OutFrame *f, int lw, int lh, float *sc, float *ox, float *oy)
{
    SDL_RenderGetLogicalSize(r, &f->lw, &f->lh);
    SDL_RenderGetScale(r, &f->sx, &f->sy);
    SDL_RenderGetViewport(r, &f->vp);
    int ow = 0, oh = 0;
    if (SDL_GetRendererOutputSize(r, &ow, &oh) != 0 || ow <= 0 || oh <= 0) {
        f->lw = -1;                              /* не вышло — по-старому, в логических координатах */
        fit(lw, lh, sc, ox, oy);
        return;
    }
    SDL_RenderSetLogicalSize(r, 0, 0);          /* сбрасывает и масштаб, и область вывода */
    *sc = std::fmin((float)ow / 1920.0f, (float)oh / 1080.0f);
    *ox = std::floor(((float)ow - 1920.0f * *sc) * 0.5f);
    *oy = std::floor(((float)oh - 1080.0f * *sc) * 0.5f);
}
static void frame_end(SDL_Renderer *r, const OutFrame *f)
{
    if (f->lw < 0) return;
    if (f->lw > 0 && f->lh > 0) {
        SDL_RenderSetLogicalSize(r, f->lw, f->lh);   /* пересчитает масштаб и область сам */
    } else {
        SDL_RenderSetScale(r, f->sx, f->sy);
        SDL_RenderSetViewport(r, &f->vp);
    }
}

static int inside(const float *rc, float x, float y)
{
    return x >= rc[0] && x < rc[0] + rc[2] && y >= rc[1] && y < rc[1] + rc[3];
}

extern "C" void dc_splash_mouse(float lx, float ly, int lclick, int rclick, int lw, int lh)
{
    if (!s_screen) return;
    float sc, ox, oy;
    fit(lw, lh, &sc, &ox, &oy);
    if (sc <= 0.0f) return;
    const float x = (lx - ox) / sc, y = (ly - oy) / sc;      /* пиксели кадра дизайна */
    const int moved = (x != s_mouse_x || y != s_mouse_y);
    s_mouse_x = x; s_mouse_y = y;
    if (rclick) { dc_splash_action(DC_MENU_BACK, 0); return; }

    if (s_screen == SPL_LANG) {
        if (s_open) {
            for (int i = 0; i < N_LANG; i++) {
                if (!inside(k_splash_lang_row[i], x, y)) continue;
                if (moved || lclick) { s_hl = i; s_dev_pad = 0; }   /* наведение ведёт курсор, как в меню */
                if (lclick) dc_splash_action(DC_MENU_OK, 0);
                return;
            }
            if (lclick && !inside(k_splash_lang_list, x, y) && !inside(k_splash_lang_box, x, y))
                dc_splash_action(DC_MENU_BACK, 0);             /* щелчок мимо — закрыть список */
            else if (lclick && inside(k_splash_lang_box, x, y))
                dc_splash_action(DC_MENU_BACK, 0);             /* по полю ещё раз — свернуть */
            return;
        }
        if (inside(k_splash_lang_box, x, y)) {
            if (moved || lclick) { s_focus = 0; s_dev_pad = 0; }
            if (lclick) dc_splash_action(DC_MENU_OK, 0);        /* раскрыть */
        } else if (inside(k_splash_lang_go, x, y)) {
            if (moved || lclick) { s_focus = 1; s_dev_pad = 0; }
            if (lclick) dc_splash_action(DC_MENU_OK, 0);        /* продолжить */
        }
        return;
    }
    const int L = lang_now();
    if (s_screen == SPL_DISC) {                         /* 04.10 (7): щелчок по «Продолжить» или по строке подсказок */
        const float *go = disc_rumenu() ? k_splash_disc_go_rumenu[L] : k_splash_disc_go[L];   /* 18.10 (6) */
        if (lclick && (inside(go, x, y) || (y >= DC_SPLASH_FOOTER_Y && x >= 1200.0f))) {
            s_dev_pad = 0;
            dc_splash_action(DC_MENU_OK, 0);
        }
        return;
    }
    for (int i = 0; i < 2; i++)
        if (lclick && inside(k_splash_tab_rect[L][i], x, y)) { s_tab = i; s_dev_pad = 0; return; }
    /* щелчок по строке подсказок справа внизу — «дальше» */
    if (lclick && y >= DC_SPLASH_FOOTER_Y && x >= 1200.0f) dc_splash_action(DC_MENU_OK, 0);
}

extern "C" void dc_splash_present(SDL_Renderer *r, int lw, int lh)
{
    if (!s_screen || !r) return;
    const int L = lang_now();
    const char *code = k_splash_lang_code[L];
    const char *dev = s_dev_pad ? pad_dev_code() : "kb";      /* 04.10 (6): у пада Nintendo — свои буквы в подсказках */
    const int pad_tab = (s_screen == SPL_CTRL && s_tab == 0);
    /* 04.10 (6): вкладка клавиатуры — по шаблону раскладки и типу игры; своя раскладка — списком из атласа меню */
    const int kb_kind = (s_screen == SPL_CTRL && !pad_tab && s_h.kb_layout_kind) ? s_h.kb_layout_kind() : 0;

    char name[64], foot[64];
    SDL_Texture *tl = nullptr;                               /* слой целиком: выбор языка, вкладка клавиатуры */
    SDL_Texture *tp = nullptr, *ttabs = nullptr, *tico = nullptr, *tlbl = nullptr;
    if (s_screen == SPL_LANG) {
        /* раскрыт список — кадр того языка, что под курсором: заголовок и поле
         * заранее показывают, на что переключится интерфейс */
        const int shown = s_open ? s_hl : s_sel;
        std::snprintf(name, sizeof name, "ui_lang_%s_%s.png", k_splash_lang_code[shown],
                      (s_open || s_focus == 0) ? "box" : "go");
        std::snprintf(foot, sizeof foot, "ui_ft_lang_%s_%s.png", dev, k_splash_lang_code[shown]);
        tl = tex_get(r, name);
    } else if (s_screen == SPL_DISC) {                       /* 04.10 (7): «о моде» — слой целиком */
        std::snprintf(name, sizeof name, disc_rumenu() ? "ui_disc_%s_rumenu.png" : "ui_disc_%s.png", code);   /* 18.10 (6) */
        std::snprintf(foot, sizeof foot, "ui_ft_disc_%s_%s.png", dev, code);
        tl = tex_get(r, name);
    } else {
        std::snprintf(foot, sizeof foot, "ui_ft_%s_%s_%s.png", s_from_menu ? "menu" : "boot", dev, code);
        if (pad_tab) {
            std::snprintf(name, sizeof name, "ui_ctrl_%s_tabs.png", code); ttabs = tex_get(r, name);
            std::snprintf(name, sizeof name, "ui_padlbl_%s.png", code);    tlbl  = tex_get(r, name);
            tico = tex_get(r, "ui_padicons.png");
            tp   = tex_get(r, "ui_gamepad.png");
        } else {
            static const char *const kbs[5] = { "", "_mo", "_om", "_oo", "" };
            std::snprintf(name, sizeof name, "ui_ctrl_%s_kb%s.png", code, kbs[kb_kind >= 0 && kb_kind < 5 ? kb_kind : 0]);
            tl = tex_get(r, name);
            if (!tl && kb_kind) {                                /* картинки шаблона нет (render.py не прогнан) — как было */
                std::snprintf(name, sizeof name, "ui_ctrl_%s_kb.png", code);
                tl = tex_get(r, name);
            }
        }
    }
    if (pad_tab ? !(ttabs && tlbl && tico && tp) : !tl) {
        dc_splash_close("нет картинок экрана");             /* не держать игру на чёрном экране */
        return;
    }
    SDL_Texture *tb = bg_get(r);
    SDL_Texture *tf = tex_get(r, foot);
    SDL_Texture *t_off = nullptr, *t_on = nullptr;
    if (s_screen == SPL_LANG && s_open) {
        t_off = tex_get(r, "ui_lang_list_off.png");
        t_on  = tex_get(r, "ui_lang_list_on.png");
    }

    float sc, ox, oy;
    OutFrame of;
    frame_begin(r, &of, lw, lh, &sc, &ox, &oy);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRect(r, nullptr);                          /* поля вокруг 16:9 и кадр игры под нами */
    const SDL_FRect full = { ox, oy, 1920.0f * sc, 1080.0f * sc };
    if (tb) SDL_RenderCopyF(r, tb, nullptr, &full);
    if (pad_tab) {
        PadRow rows[DCS_COUNT];
        const float sh = pad_layout(L, rows);
        const float *bx = k_splash_box[L], *tr = k_splash_tabs[L];
        const SDL_FRect dp = { ox + (bx[0] - sh) * sc, oy + bx[1] * sc, bx[2] * sc, bx[3] * sc };
        const SDL_FRect dt = { ox + tr[0] * sc, oy + tr[1] * sc, tr[2] * sc, tr[3] * sc };
        SDL_RenderCopyF(r, tp, nullptr, &dp);
        SDL_RenderCopyF(r, ttabs, nullptr, &dt);
        for (int s = 0; s < DCS_COUNT; s++) {
            const PadRow &w = rows[s];
            blit_piece(r, tico, pad_icon(s, w.dim), w.ix - sh, w.iy, sc, ox, oy);
            for (int i = 0; i < w.n; i++)
                blit_piece(r, tlbl, k_splash_lbl[L][w.lbl[i]], w.lx[i] - sh, w.ly[i], sc, ox, oy);
        }
    } else if (s_screen == SPL_CTRL && kb_kind == 4) {
        /* 04.10 (6): своя раскладка — из картинки клавиатуры только лента вкладок, ниже список действий и клавиш */
        const float *tr = k_splash_tabs[L];
        const SDL_Rect src = { (int)tr[0], (int)tr[1], (int)(tr[2] + 0.5f), (int)(tr[3] + 0.5f) };
        const SDL_FRect dt = { ox + tr[0] * sc, oy + tr[1] * sc, tr[2] * sc, tr[3] * sc };
        SDL_RenderCopyF(r, tl, &src, &dt);
        kb_custom_present(r, L, sc, ox, oy);
    } else {
        SDL_RenderCopyF(r, tl, nullptr, &full);
    }
    if (t_off) SDL_RenderCopyF(r, t_off, nullptr, &full);
    if (t_on) {                                              /* строка под курсором — из подсвеченного списка */
        const float *rr = k_splash_lang_row[s_hl];
        const SDL_Rect src = { (int)rr[0], (int)rr[1], (int)(rr[2] + 0.5f), (int)(rr[3] + 0.5f) };
        const SDL_FRect d = { ox + (float)src.x * sc, oy + (float)src.y * sc, (float)src.w * sc, (float)src.h * sc };
        SDL_RenderCopyF(r, t_on, &src, &d);
    }
    if (tf) {
        const SDL_FRect d = { ox, oy + DC_SPLASH_FOOTER_Y * sc, 1920.0f * sc, 120.0f * sc };
        SDL_RenderCopyF(r, tf, nullptr, &d);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    frame_end(r, &of);
}

/* ⭐ 18.10.2026 - ЭКРАН «ПОДГОТОВКА ШЕЙДЕРОВ» (main.cpp dc_shader_prep: растр собирает конвейеры на своём потоке, игрок
 * просил «как в современных AAA - шейдеры в самом начале, отдельным экраном, полоской прогресса»). Фон экранов запуска,
 * слой ui_shader_<код>.png (design/.../shader.html: заголовок, пустая рамка полосы, пояснение), заполнение полосы и
 * проценты из ui_shader_digits.png (клетки 14x40: 0..9 и %) - здесь. Места полосы и чисел - те же, что в shader.html
 * (менять вместе). progress < 0 - только фон: первый кадр окна, пока не ясно, нужен ли экран. Нет картинок (render.py не
 * прогнан) - фон и полоса без текста. */
static const float SH_BAR_X = 660.0f, SH_BAR_Y = 526.0f, SH_BAR_W = 600.0f, SH_BAR_H = 8.0f, SH_PCT_Y = 546.0f;
static const int   SH_DIG_W = 14, SH_DIG_H = 40;
extern "C" void dc_splash_shader_present(SDL_Renderer *r, int lw, int lh, int lang, float progress)
{
    if (!r) return;
    SDL_Texture *tb = bg_get(r);
    SDL_Texture *tl = nullptr, *td = nullptr;
    if (progress >= 0.0f) {
        char name[64];
        std::snprintf(name, sizeof name, "ui_shader_%s.png", k_splash_lang_code[(lang >= 0 && lang < N_LANG) ? lang : 0]);
        tl = tex_get(r, name);
        td = tex_get(r, "ui_shader_digits.png");
        if (progress > 1.0f) progress = 1.0f;
    }
    float sc, ox, oy;
    OutFrame of;
    frame_begin(r, &of, lw, lh, &sc, &ox, &oy);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRect(r, nullptr);                          /* поля вокруг 16:9 */
    const SDL_FRect full = { ox, oy, 1920.0f * sc, 1080.0f * sc };
    if (tb) SDL_RenderCopyF(r, tb, nullptr, &full);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (progress >= 0.0f) {
        if (tl) SDL_RenderCopyF(r, tl, nullptr, &full);
        else {                                               /* без картинки - рамка полосы здесь */
            SDL_SetRenderDrawColor(r, 242, 245, 246, 41);
            const SDL_FRect fr = { ox + SH_BAR_X * sc, oy + SH_BAR_Y * sc, SH_BAR_W * sc, SH_BAR_H * sc };
            SDL_RenderDrawRectF(r, &fr);
        }
        const float iw = (SH_BAR_W - 2.0f) * progress;       /* заполнение - внутри рамки */
        if (iw > 0.25f) {
            SDL_SetRenderDrawColor(r, 127, 227, 214, 255);   /* #7fe3d6 - бирюза дизайна */
            const SDL_FRect fill = { ox + (SH_BAR_X + 1.0f) * sc, oy + (SH_BAR_Y + 1.0f) * sc, iw * sc, (SH_BAR_H - 2.0f) * sc };
            SDL_RenderFillRectF(r, &fill);
        }
        if (td) {
            char num[8];
            std::snprintf(num, sizeof num, "%d%%", (int)(progress * 100.0f));   /* 100 - только когда всё готово */
            const int n = (int)std::strlen(num);
            float x = 960.0f - (float)(n * SH_DIG_W) * 0.5f;
            SDL_SetTextureAlphaMod(td, 204);                 /* rgba(242,245,246,0.8) - как значения меню */
            for (int i = 0; i < n; i++, x += (float)SH_DIG_W) {
                const int cell = (num[i] == '%') ? 10 : (num[i] - '0');
                const SDL_Rect src = { cell * SH_DIG_W, 0, SH_DIG_W, SH_DIG_H };
                const SDL_FRect d = { ox + x * sc, oy + SH_PCT_Y * sc, (float)SH_DIG_W * sc, (float)SH_DIG_H * sc };
                SDL_RenderCopyF(r, td, &src, &d);
            }
        }
    }
    frame_end(r, &of);
}
/* экран подготовки закрыт - его картинки больше не нужны (экраны запуска грузят свои) */
extern "C" void dc_splash_shader_done(void)
{
    tex_free_all();
    s_missing.clear();
}

/* ⭐ 20.10.2026 - ГЛАВНОЕ МЕНЮ ТИТУЛА: ОТЗЫВ И DISCORD (игрок: «сделай то же самое в главном меню мода: справа снизу надпись
 * Please leave feedback, версия мода, Made by ROTMILL и ссылку только на Discord, со значком»; образец - его King's Field
 * Remade). Картинки ui_feedback_off/on.png (render.py, feedback.html): блок 360x90 пикселей кадра 1920x1080, прижат вправо,
 * строка со значком (x FB_HIT_X..360, y 0..52) - ссылка, _on - мышь над ней. Блок - в правый нижний угол ОКНА (не кадра
 * дизайна: на 21:9 и шире тоже в угол), масштаб по высоте, в пиксели окна (на 1080p - пиксель в пиксель). Кадр игры доходит
 * до угла (4:3 во всё окно) - там строка копирайта Capcom (kx, ky - её правый верх): блок поднимается над ней. Появление
 * 0.25 с, уход 0.15 с. Числа FB_* - те же, что в render.py (FB_SIZE, FB_HIT_X). Логика (когда, наведение, щелчок) - main.cpp. */
static const float FB_W = 360.0f, FB_H = 90.0f, FB_HIT_X = 46.0f, FB_HIT_H = 52.0f, FB_MR = 40.0f, FB_MB = 36.0f;
static float  s_fb_a;                  /* видимость 0..1 */
static Uint32 s_fb_t;                  /* SDL_GetTicks прошлого показа */
static float  s_fb_hot[4];             /* строка со ссылкой - в логических координатах рендерера (прошлый показ) */
static int    s_fb_hot_ok;

extern "C" int dc_splash_feedback_hit(float lx, float ly)
{
    return s_fb_hot_ok && s_fb_a > 0.5f && lx >= s_fb_hot[0] && lx < s_fb_hot[0] + s_fb_hot[2] &&
           ly >= s_fb_hot[1] && ly < s_fb_hot[1] + s_fb_hot[3];
}

extern "C" void dc_splash_feedback_present(SDL_Renderer *r, int lw, int lh, int show, int hover, float kx, float ky)
{
    const Uint32 now = SDL_GetTicks();
    const float dt = s_fb_t ? (float)(now - s_fb_t) * 0.001f : 0.0f;
    s_fb_t = now;
    s_fb_a += show ? dt / 0.25f : -dt / 0.15f;
    if (s_fb_a < 0.0f) s_fb_a = 0.0f;
    if (s_fb_a > 1.0f) s_fb_a = 1.0f;
    s_fb_hot_ok = 0;
    if (s_fb_a <= 0.0f || lw <= 0 || lh <= 0) return;
    SDL_Texture *t = tex_get(r, hover ? "ui_feedback_on.png" : "ui_feedback_off.png");
    if (!t) return;
    OutFrame f; float sc, ox, oy;
    frame_begin(r, &f, lw, lh, &sc, &ox, &oy);
    /* логические -> пиксели окна: (вьюпорт + x) * масштаб; без вывода в пиксели (f.lw < 0) - в логических */
    const bool px = f.lw >= 0;
    const float msx = px ? f.sx : 1.0f, msy = px ? f.sy : 1.0f;
    const float vx = px ? (float)f.vp.x : 0.0f, vy = px ? (float)f.vp.y : 0.0f;
    float ow = (float)lw, oh = (float)lh;
    if (px) { int iw = 0, ih = 0; SDL_GetRendererOutputSize(r, &iw, &ih); ow = (float)iw; oh = (float)ih; }
    const float k = oh / 1080.0f;
    float x = std::floor(ow - (FB_MR + FB_W) * k + 0.5f), y = std::floor(oh - (FB_MB + FB_H) * k + 0.5f);
    if (kx > 0.0f && ky > 0.0f) {                      /* строка копирайта под блоком - поднять над ней */
        const float kpx = (vx + kx) * msx, kpy = (vy + ky) * msy;
        if (x + FB_HIT_X * k < kpx + 16.0f * k && y + FB_H * k > kpy - 8.0f * k) y = std::floor(kpy - 8.0f * k - FB_H * k);
    }
    const SDL_FRect d = { x, y, std::floor(FB_W * k + 0.5f), std::floor(FB_H * k + 0.5f) };
    SDL_SetTextureAlphaMod(t, (Uint8)(255.0f * s_fb_a + 0.5f));
    SDL_RenderCopyF(r, t, nullptr, &d);
    SDL_SetTextureAlphaMod(t, 255);
    frame_end(r, &f);
    s_fb_hot[0] = (x + FB_HIT_X * k) / msx - vx;  s_fb_hot[1] = y / msy - vy;
    s_fb_hot[2] = (FB_W - FB_HIT_X) * k / msx;     s_fb_hot[3] = FB_HIT_H * k / msy;
    s_fb_hot_ok = show;
}

/* ==== МЕНЮ НАСТРОЕК В СТИЛЕ ЭТИХ ЭКРАНОВ (10.09.2026) ========================
 * Игрок: «сделай стиль меню опций наших полностью таким же, как окна раскладки
 * и выбора языка». Макет — design/controls_layout/render/menu.html; здесь он
 * собирается по тем же размерам (пиксели кадра 1920x1080): строки меню —
 * кусками атласа ui_menu_<код>.png (браузер набрал их теми же шрифтами и
 * кеглями, что макет, белым — цвет задаём здесь), рамки и подсветка —
 * прямоугольниками цветов дизайна, числа — из цифр атласа, подсказки кнопок —
 * полоса ui_ft_opt_<pad|kb>_<код>.png, фон — градиент экранов запуска. Логика
 * меню, таблица и переводы — dc_settings.c. Строки нет в атласе (пункт
 * добавили, а render.py не прогнали) — остаётся прежнее меню (dc_menu_draw). */

static_assert(DC_MT_LANGS == DC_SPLASH_LANGS, "языки атласа меню и экранов запуска разошлись");

enum {
    M_TABS_Y = 40,                        /* лента вкладок: верх рамки (как в ctrl.html) */
    M_LIST_X = 410, M_LIST_W = 1100,      /* список и плашка пояснения */
    M_LIST_Y = 172, M_ROW_H = 56,
    M_PANEL_Y = 780, M_PANEL_H = 150,
};
struct Rgba { Uint8 r, g, b, a; };
static const Rgba C_TEXT = { 242, 245, 246, 255 };   /* #f2f5f6 */
static const Rgba C_ON   = { 185, 242, 234, 255 };   /* #b9f2ea — выбранное */
static const Rgba C_TEAL = { 127, 227, 214, 255 };   /* #7fe3d6 — заголовки, рамка фокуса */

static int         s_menu_style = -1;                /* DC_MENU_STYLE=0 — прежнее меню */
static signed char s_mt_ok[DC_SPLASH_LANGS];          /* атлас языка: 0 — не смотрели, 1 — есть, -1 — нет */
static int         s_mt_warned;
static int         s_mscroll, s_mpage = -1, s_mwas_open, s_mprimed;
static float       s_mtab[12][4];
static int         s_mtabs;
struct MRowHit { int opt, type; float y, lx0, lx1, rx0;   /* зоны мыши: строка, левая стрелка, правая */
                 int slider; float tx0, tx1, lo, hi; };    /* 11.10: ползунок числовой строки - полоса и пределы */
static MRowHit     s_mrow[32];
static int         s_mrows;
static float       s_mmx, s_mmy;
static int         s_mdrag = -1;                     /* 11.10: строка, чей ползунок тянут мышью */
/* 20.10.2026 (игрок: «скролл-бар толще визуально, и чтобы можно было его мышкой перетаскивать... везде, в настройках, во всех
 * разделах»): полоса прокрутки 12 px вместо 3, бегунок тянется мышью, щелчок по полосе мимо бегунка ставит бегунок под мышь
 * и тоже тянет. Список догоняет выбранную строку только когда выбор сменился (клавиши, пад, наведение) - иначе прокрутку
 * мышью сразу возвращало бы к ней. DC_MENU_SB_OLD=1 - как было (3 px, мышью не тянется). */
static int         s_msb_on, s_msb_drag, s_msb_n, s_msb_fit, s_msel_seen = -1;
static float       s_msb_x0, s_msb_x1, s_msb_y0, s_msb_h, s_msb_ty, s_msb_th, s_msb_grab;
static int sb_old(void) { static int v = -1; if (v < 0) { const char *e = SDL_getenv("DC_MENU_SB_OLD"); v = (e && *e == '1') ? 1 : 0; } return v; }

static int mt_find(int cat, const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < DC_MT_N; i++)
        if (k_mt_key[i].cat == cat && std::strcmp(k_mt_key[i].key, key) == 0) return i;
    return -1;
}

/* Ширина числа из цифр атласа; -1 — какого-то знака в атласе нет. */
static float mt_num_width(int L, const char *s)
{
    float w = 0.0f;
    for (const char *p = s; *p; p++) {
        const char k[2] = { *p, 0 };
        const int i = mt_find(DC_MT_DIG, k);
        if (i < 0) return -1.0f;
        w += k_mt_rect[L][i][2];
    }
    return w;
}

/* Все строки вкладки есть в атласе? Нет — прежнее меню, в лог — один раз. */
static int mt_page_ok(int L, const DcMenuRow *rows, int n)
{
    auto miss = [](const char *what) -> int {
        if (!s_mt_warned) {
            std::fprintf(stderr, "menu: в атласе нет строки «%s» — прежнее меню (прогони render.py)\n", what);
            s_mt_warned = 1;
        }
        return 0;
    };
    for (int p = 0; p < dc_menu_page_count(); p++)
        if (mt_find(DC_MT_TAB, dc_menu_page_key(p)) < 0) return miss(dc_menu_page_key(p));
    if (const char *st = dc_menu_sub_title())                      /* 04.10 (6): подстраница назначений */
        if (mt_find(DC_MT_TAB, st) < 0) return miss(st);
    for (int i = 0; i < n; i++) {
        const DcMenuRow &r = rows[i];
        if (mt_find(DC_MT_HDR, r.title) < 0) return miss(r.title);
        if (r.type != DC_OPT_HEADER && mt_find(DC_MT_LBL, r.title) < 0) return miss(r.title);
        if (r.hint && mt_find(DC_MT_HINT, r.hint) < 0) return miss(r.hint);
        if (r.value_key && mt_find(DC_MT_VAL, r.value_key) < 0) return miss(r.value_key);
        if (r.value_num[0] && mt_num_width(L, r.value_num) < 0.0f) return miss(r.value_num);
        if (r.choices)
            for (const char *const *c = r.choices; *c; c++)
                if (mt_find(DC_MT_VAL, *c) < 0) return miss(*c);
        if (r.type == DC_OPT_BIND) {                               /* 04.10 (6): подписи входов и «нажмите…» */
            for (int k = 0; k < r.bind_n; k++)
                if (!r.bind_lbl[k] || mt_find(DC_MT_VAL, r.bind_lbl[k]) < 0) return miss(r.bind_lbl[k] ? r.bind_lbl[k] : "(bind)");
            static const char *const need[] = { "Not assigned", "Press a key", "Press a button", "/" };
            for (const char *s : need) if (mt_find(DC_MT_VAL, s) < 0) return miss(s);
        }
    }
    return 1;
}

/* ⭐ 04.10 (вечер, 6) — значение строки назначения: входы через « / » по правому краю xr. Клавиши и кнопки мыши —
 * подписями атласа меню, кнопки геймпада — значками схемы (ui_padicons.png, по типу пада; у крестовины — значок и
 * стрелка); нет назначений — приглушённое «Не назначено»; строка ждёт вход — мигающее «Нажмите клавишу / кнопку». */
static void menu_bind_value(SDL_Renderer *r, SDL_Texture *ta, int L, const DcMenuRow &rw, float xr, float y, int on,
                            float sc, float ox, float oy)
{
    const Rgba vc = on ? C_ON : Rgba{ 242, 245, 246, 204 };
    auto put = [&](int idx, float x, Rgba c) {
        SDL_SetTextureColorMod(ta, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(ta, c.a);
        blit_piece(r, ta, k_mt_rect[L][idx], x, y + 1.0f + (54.0f - k_mt_rect[L][idx][3]) * 0.5f, sc, ox, oy);
    };
    if (rw.capturing) {
        const int ip = mt_find(DC_MT_VAL, rw.bind_dev == DCB_PAD ? "Press a button" : "Press a key");
        if (ip < 0) return;
        const float t = (float)(SDL_GetTicks() % 1200u) / 1200.0f;
        const Uint8 a = (Uint8)(140.0f + 115.0f * std::fabs(std::sin(t * 3.14159265f)));
        put(ip, xr - k_mt_rect[L][ip][2], Rgba{ C_TEAL.r, C_TEAL.g, C_TEAL.b, a });
        return;
    }
    if (rw.bind_n <= 0) {
        const int in = mt_find(DC_MT_VAL, "Not assigned");
        if (in >= 0) put(in, xr - k_mt_rect[L][in][2], Rgba{ 242, 245, 246, 110 });
        return;
    }
    SDL_Texture *ti = (rw.bind_dev == DCB_PAD) ? tex_get(r, "ui_padicons.png") : nullptr;
    const int isl = mt_find(DC_MT_VAL, "/");
    float x = xr;
    for (int k = rw.bind_n - 1; k >= 0; k--) {               /* справа налево */
        const int code = rw.bind_code[k];
        const int icon = ti ? dcb_code_pad_icon(code) : -1;
        const int il = rw.bind_lbl[k] ? mt_find(DC_MT_VAL, rw.bind_lbl[k]) : -1;
        if (icon >= 0) {
            if (icon == DCS_DPAD && il >= 0) { x -= k_mt_rect[L][il][2]; put(il, x, vc); x -= 6.0f; }   /* стрелка крестовины */
            const float *q = pad_icon(icon, 0);
            x -= q[2];
            SDL_SetTextureColorMod(ti, 255, 255, 255);
            SDL_SetTextureAlphaMod(ti, on ? 255 : 215);
            blit_piece(r, ti, q, x, y + (56.0f - q[3]) * 0.5f, sc, ox, oy);
        } else if (il >= 0) {
            x -= k_mt_rect[L][il][2];
            put(il, x, vc);
        }
        if (k > 0 && isl >= 0) {
            x -= 12.0f + k_mt_rect[L][isl][2];
            put(isl, x, Rgba{ 242, 245, 246, 110 });
            x -= 12.0f;
        }
    }
}

/* ⭐ 04.10 (6) — вкладка клавиатуры экрана «Раскладка управления» для СВОЕЙ раскладки: список действий и их клавиш
 * (у шаблонов — картинки дизайна ui_ctrl_<код>_kb*.png). Те же группы, что на подстранице назначений; шрифты и цвета —
 * из атласа меню (подписи, заголовки групп, подписи клавиш). */
static void kb_custom_present(SDL_Renderer *r, int L, float sc, float ox, float oy)
{
    char name[48];
    std::snprintf(name, sizeof name, "ui_menu_%s.png", k_splash_lang_code[L]);
    SDL_Texture *ta = tex_get(r, name);
    if (!ta) return;
    struct Grp { const char *title; int col; int n; int acts[8]; };
    static const Grp grp[] = {
        { "Movement", 0, 7, { DCB_FWD, DCB_BACK, DCB_LEFT, DCB_RIGHT, DCB_RUN, DCB_RUN_TOGGLE, DCB_QUICKTURN } },
        { "Combat", 0, 3, { DCB_AIM, DCB_FIRE, DCB_TARGET } },
        { "Actions & Menus", 1, 5, { DCB_ACTION, DCB_INVENTORY, DCB_CANCEL, DCB_PAUSE, DCB_OPTIONS } },
    };
    const float colw = 720.0f, gap = 96.0f, x0 = (1920.0f - 2.0f * colw - gap) * 0.5f;
    float ycol[2] = { 196.0f, 196.0f };
    auto put = [&](int idx, float x, float yy, Rgba c) {
        SDL_SetTextureColorMod(ta, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(ta, c.a);
        blit_piece(r, ta, k_mt_rect[L][idx], x, yy, sc, ox, oy);
    };
    auto line = [&](float x, float yy, float w, Rgba c) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        const SDL_FRect d = { ox + std::floor(x) * sc, oy + std::floor(yy) * sc, std::floor(w) * sc, 1.0f * sc };
        SDL_RenderFillRectF(r, &d);
    };
    const int mode = dcb_mode(), isl = mt_find(DC_MT_VAL, "/"), ina = mt_find(DC_MT_VAL, "Not assigned");
    for (const Grp &g : grp) {
        const float x = x0 + (float)g.col * (colw + gap);
        float &yy = ycol[g.col];
        const int ih = mt_find(DC_MT_HDR, g.title);
        if (ih >= 0) { put(ih, x, yy, C_TEAL); yy += k_mt_rect[L][ih][3] + 12.0f; }
        line(x, yy, colw, Rgba{ 127, 227, 214, 89 });
        for (int j = 0; j < g.n; j++) {
            const int a = g.acts[j];
            const float rh = 46.0f;
            const int il = mt_find(DC_MT_LBL, dcb_act_title(a));
            if (il >= 0) put(il, x + 4.0f, yy + (rh - k_mt_rect[L][il][3]) * 0.5f, C_TEXT);
            int c[DCB_MAX];
            const int nb = dcb_get(mode, DCB_KB, a, c);
            float xr = x + colw - 4.0f;
            if (nb <= 0 && ina >= 0) {
                xr -= k_mt_rect[L][ina][2];
                put(ina, xr, yy + (rh - k_mt_rect[L][ina][3]) * 0.5f, Rgba{ 242, 245, 246, 110 });
            }
            for (int k = nb - 1; k >= 0; k--) {
                const char *lb = dcb_code_label(c[k]);
                const int iv = lb ? mt_find(DC_MT_VAL, lb) : -1;
                if (iv < 0) continue;
                xr -= k_mt_rect[L][iv][2];
                put(iv, xr, yy + (rh - k_mt_rect[L][iv][3]) * 0.5f, C_ON);
                if (k > 0 && isl >= 0) {
                    xr -= 10.0f + k_mt_rect[L][isl][2];
                    put(isl, xr, yy + (rh - k_mt_rect[L][isl][3]) * 0.5f, Rgba{ 242, 245, 246, 110 });
                    xr -= 10.0f;
                }
            }
            yy += rh;
            line(x, yy - 1.0f, colw, Rgba{ 242, 245, 246, 23 });
        }
        yy += 34.0f;
    }
    SDL_SetTextureColorMod(ta, 255, 255, 255);
    SDL_SetTextureAlphaMod(ta, 255);
}

extern "C" int dc_menu_ui_ready(void)
{
    if (s_menu_style < 0) {
        const char *e = SDL_getenv("DC_MENU_STYLE");
        s_menu_style = (e && *e == '0') ? 0 : 1;
    }
    if (!s_menu_style || !dc_menu_is_open()) return 0;
    const int L = lang_now();
    if (!s_mt_ok[L]) {
        char name[48];
        std::snprintf(name, sizeof name, "ui_menu_%s.png", k_splash_lang_code[L]);
        std::vector<unsigned char> buf;
        const unsigned char *p = nullptr; size_t n = 0;
        s_mt_ok[L] = asset_bytes(name, buf, &p, &n) ? 1 : -1;
        if (s_mt_ok[L] < 0) std::fprintf(stderr, "menu: нет %s — прежнее меню\n", name);
    }
    if (s_mt_ok[L] < 0) return 0;
    DcMenuRow rows[64];
    const int n = dc_menu_rows(rows, 64);
    return mt_page_ok(L, rows, n);
}

extern "C" void dc_menu_ui_present(SDL_Renderer *r, int lw, int lh)
{
    const int open = dc_menu_is_open();
    if (open && !s_mwas_open) {                    /* меню только что открыли */
        dc_menu_set_dev(pad_now());
        s_mscroll = 0; s_mpage = -1; s_mprimed = 0; s_msb_drag = 0; s_msel_seen = -1;
    }
    s_mwas_open = open;
    if (!open || !r || !dc_menu_ui_ready()) return;
    const int L = lang_now();
    const char *code = k_splash_lang_code[L];
    char name[48], foot[48];
    std::snprintf(name, sizeof name, "ui_menu_%s.png", code);
    const int sub = dc_menu_sub_title() != nullptr;               /* 04.10 (6): подстраница назначений кнопок */
    std::snprintf(foot, sizeof foot, "ui_ft_%s_%s_%s.png", sub ? "bind" : "opt", dc_menu_dev() ? pad_dev_code() : "kb", code);
    SDL_Texture *ta = tex_get(r, name);
    if (!ta) { s_mt_ok[L] = -1; return; }            /* со следующего кадра — прежнее меню */
    SDL_Texture *tb = bg_get(r), *tf = tex_get(r, foot);

    float sc, ox, oy;
    OutFrame of;
    frame_begin(r, &of, lw, lh, &sc, &ox, &oy);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRect(r, nullptr);                 /* как экраны запуска — меню закрывает кадр целиком */
    const SDL_FRect full = { ox, oy, 1920.0f * sc, 1080.0f * sc };
    if (tb) SDL_RenderCopyF(r, tb, nullptr, &full);

    /* прямоугольник: края — к целому пикселю кадра, как у блоков CSS */
    auto fill = [&](float x, float y, float w, float h, Rgba c) {
        const float x0 = std::floor(x + 0.5f), y0 = std::floor(y + 0.5f);
        const float x1 = std::floor(x + w + 0.5f), y1 = std::floor(y + h + 0.5f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        const SDL_FRect d = { ox + x0 * sc, oy + y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc };
        SDL_RenderFillRectF(r, &d);
    };
    /* блок с фоном и рамкой 1 px: фон и под рамкой тоже, рамка сверху — как в CSS */
    auto box = [&](float x, float y, float w, float h, Rgba bg, Rgba bd) {
        const float x0 = std::floor(x + 0.5f), y0 = std::floor(y + 0.5f);
        const float x1 = std::floor(x + w + 0.5f), y1 = std::floor(y + h + 0.5f);
        if (bg.a) fill(x0, y0, x1 - x0, y1 - y0, bg);
        fill(x0, y0, x1 - x0, 1.0f, bd);
        fill(x0, y1 - 1.0f, x1 - x0, 1.0f, bd);
        fill(x0, y0 + 1.0f, 1.0f, y1 - y0 - 2.0f, bd);
        fill(x1 - 1.0f, y0 + 1.0f, 1.0f, y1 - y0 - 2.0f, bd);
    };
    auto text = [&](int idx, float x, float y, Rgba c) {
        SDL_SetTextureColorMod(ta, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(ta, c.a);
        blit_piece(r, ta, k_mt_rect[L][idx], x, y, sc, ox, oy);
    };

    /* ---- лента вкладок: дословно ctrl.html — поля ленты 6, зазор 8, поля вкладки 10/26 */
    /* 04.10 (6): на подстранице назначений подсвечена её вкладка (CONTROLS) */
    const int np = std::min(dc_menu_page_count(), 12), cur = dc_menu_page_parent(), curp = dc_menu_page();
    int ti[12];
    float tw = 0.0f, th = 0.0f;
    for (int p = 0; p < np; p++) {
        ti[p] = mt_find(DC_MT_TAB, dc_menu_page_key(p));
        tw += k_mt_rect[L][ti[p]][2] + 54.0f;        /* текст + поля 26+26 + рамка 1+1 */
        th = std::fmax(th, k_mt_rect[L][ti[p]][3]);
    }
    const float bw = tw + 8.0f * (float)(np - 1) + 14.0f, bh = th + 22.0f + 14.0f;
    const float bx = 960.0f - bw * 0.5f;
    box(bx, (float)M_TABS_Y, bw, bh, Rgba{ 242, 245, 246, 8 }, Rgba{ 242, 245, 246, 41 });
    float x = bx + 7.0f;
    s_mtabs = np;
    for (int p = 0; p < np; p++) {
        const float w = k_mt_rect[L][ti[p]][2] + 54.0f, y = (float)M_TABS_Y + 7.0f, h = th + 22.0f;
        if (p == cur) box(x, y, w, h, Rgba{ 127, 227, 214, 36 }, C_TEAL);
        text(ti[p], x + 27.0f, y + 11.0f, p == cur ? C_ON : Rgba{ 242, 245, 246, 140 });
        s_mtab[p][0] = x; s_mtab[p][1] = y; s_mtab[p][2] = w; s_mtab[p][3] = h;
        x += w + 8.0f;
    }

    /* ---- строки: подпись слева, справа [‹] значение [›]; колонка значений —
     * под самое широкое значение вкладки, чтобы стрелки не прыгали */
    DcMenuRow rows[64];
    const int n = dc_menu_rows(rows, 64), sel = dc_menu_sel_opt();
    if (curp != s_mpage) { s_mpage = curp; s_mscroll = 0; s_msel_seen = -1; s_msb_drag = 0; }
    /* 04.10 (6): заголовок подстраницы назначений над списком; справа — для какого типа игры и какой шаблон */
    if (sub) {
        const int it = mt_find(DC_MT_TAB, dc_menu_sub_title());
        if (it >= 0) text(it, (float)M_LIST_X + 4.0f, (float)M_LIST_Y - 14.0f - k_mt_rect[L][it][3], C_TEAL);
        /* «Тип игры  СОВРЕМЕННЫЙ    Шаблон раскладки  СВОЙ» — подписи строк вкладок GAME и CONTROLS из атласа */
        /* 10.10: тип игры - тот, чью раскладку правят (строка «For Game Type»), шаблон - этого устройства */
        const int sdev = (std::strcmp(dc_menu_sub_title(), "GAMEPAD") == 0) ? DCB_PAD : DCB_KB;
        const int gm = dcb_edit_mode(), tp = dcb_layout_dev(gm, sdev);
        const int ig = mt_find(DC_MT_VAL, gm == DCB_ORIG ? "Original" : "Modern");
        const int il = mt_find(DC_MT_VAL, tp == DCB_TPL_ORIG ? "Original" : tp == DCB_TPL_MODERN ? "Modern" : "Custom");
        const int lg = mt_find(DC_MT_LBL, "Game Type"), ll = mt_find(DC_MT_LBL, "Button Layout");
        if (ig >= 0 && il >= 0 && lg >= 0 && ll >= 0) {
            float x1 = (float)(M_LIST_X + M_LIST_W) - 21.0f;
            const float yb = (float)M_LIST_Y - 14.0f;
            const Rgba dim = Rgba{ 242, 245, 246, 140 };
            x1 -= k_mt_rect[L][il][2]; text(il, x1, yb - k_mt_rect[L][il][3], C_ON);
            x1 -= 10.0f + k_mt_rect[L][ll][2]; text(ll, x1, yb - k_mt_rect[L][ll][3] + 2.0f, dim);
            x1 -= 34.0f + k_mt_rect[L][ig][2]; text(ig, x1, yb - k_mt_rect[L][ig][3], C_ON);
            x1 -= 10.0f + k_mt_rect[L][lg][2]; text(lg, x1, yb - k_mt_rect[L][lg][3] + 2.0f, dim);
        }
    }
    float colw = 0.0f;
    for (int i = 0; i < n; i++) {
        const DcMenuRow &rw = rows[i];
        if (rw.type == DC_OPT_DEVICE && rw.choices) {             /* 04.10 (6): «4.» + самая длинная подпись */
            const float pw = mt_num_width(L, "4.") + 10.0f;
            for (const char *const *c = rw.choices; *c; c++) colw = std::fmax(colw, pw + k_mt_rect[L][mt_find(DC_MT_VAL, *c)][2]);
        } else if (rw.type == DC_OPT_ENUM && rw.choices) {
            for (const char *const *c = rw.choices; *c; c++) colw = std::fmax(colw, k_mt_rect[L][mt_find(DC_MT_VAL, *c)][2]);
        } else if (rw.type == DC_OPT_BIND) {
            /* назначения — каждая строка своей ширины, по правому краю; колонку стрелок не трогают */
        } else if (rw.type == DC_OPT_BOOL) {
            colw = std::fmax(colw, k_mt_rect[L][mt_find(DC_MT_VAL, "On")][2]);
            colw = std::fmax(colw, k_mt_rect[L][mt_find(DC_MT_VAL, "Off")][2]);
        } else if (rw.value_num[0]) {
            colw = std::fmax(colw, mt_num_width(L, rw.value_num));
        }
    }
    colw = std::ceil(colw);
    /* 11.10.2026 (игрок: «настройки камеры все ползунками»): числовые строки - полоса с бегунком слева от значения;
     * полоса начинается за самой длинной подписью числовых строк вкладки (у всех строк одна и та же). DC_MENU_SLIDERS=0 -
     * как было, только стрелки. */
    static int sliders = -1;
    if (sliders < 0) { const char *e = SDL_getenv("DC_MENU_SLIDERS"); sliders = (e && *e == '0') ? 0 : 1; }
    float lab_max = 0.0f;
    for (int i = 0; i < n; i++)
        if ((rows[i].type == DC_OPT_INT || rows[i].type == DC_OPT_FLOAT) && rows[i].value_num[0]) {
            const int il = mt_find(DC_MT_LBL, rows[i].title);
            if (il >= 0) lab_max = std::fmax(lab_max, k_mt_rect[L][il][2]);
        }
    const int fit_rows = (M_PANEL_Y - 20 - M_LIST_Y) / M_ROW_H;
    int sel_k = 0;
    for (int i = 0; i < n; i++) if (rows[i].opt == sel) sel_k = i;
    if (sb_old() || sel != s_msel_seen) {                         /* 20.10: догонять выбор - только когда он сменился */
        s_msel_seen = sel;
        if (sel_k < s_mscroll) s_mscroll = sel_k;
        if (sel_k >= s_mscroll + fit_rows) s_mscroll = sel_k - fit_rows + 1;
    }
    if (s_mscroll > n - fit_rows) s_mscroll = n - fit_rows;
    if (s_mscroll < 0) s_mscroll = 0;
    const int ichl = mt_find(DC_MT_CHEV_L, ""), ichr = mt_find(DC_MT_CHEV_R, ""), i0 = mt_find(DC_MT_DIG, "0");
    s_mrows = 0;
    for (int k = s_mscroll; k < n && k < s_mscroll + fit_rows; k++) {
        const DcMenuRow &rw = rows[k];
        /* 11.10: последняя строка «Restore Defaults» (сброс вкладки или раскладки) - чуть ниже прочих, отдельно от
         * группы над ней (на вкладке GRAPHICS иначе читалась бы частью Experimental) */
        const float gap = (k == n - 1 && rw.type == DC_OPT_ACTION && std::strcmp(rw.title, "Restore Defaults") == 0) ? 16.0f : 0.0f;
        const float y = (float)M_LIST_Y + (float)(k - s_mscroll) * (float)M_ROW_H + gap;
        const int on = (rw.opt == sel);
        if (rw.type == DC_OPT_HEADER) {             /* заголовок группы — как разделы вкладки клавиатуры */
            const int ih = mt_find(DC_MT_HDR, rw.title);
            text(ih, (float)M_LIST_X + 4.0f, y + (float)M_ROW_H - 13.0f - k_mt_rect[L][ih][3], C_TEAL);
            fill((float)M_LIST_X, y + (float)M_ROW_H - 1.0f, (float)M_LIST_W, 1.0f, Rgba{ 127, 227, 214, 89 });
            continue;
        }
        if (on) box((float)M_LIST_X, y, (float)M_LIST_W, (float)M_ROW_H, Rgba{ 127, 227, 214, 36 }, C_TEAL);
        else    fill((float)M_LIST_X, y + (float)M_ROW_H - 1.0f, (float)M_LIST_W, 1.0f, Rgba{ 242, 245, 246, 23 });
        const int il = mt_find(DC_MT_LBL, rw.title);
        text(il, (float)M_LIST_X + 23.0f, y + 1.0f + (54.0f - k_mt_rect[L][il][3]) * 0.5f, on ? C_ON : C_TEXT);

        const float xr = (float)(M_LIST_X + M_LIST_W) - 21.0f;   /* правый край группы значения */
        const float cy = y + 28.0f;                               /* середина строки */
        const float chr_x = xr - 11.0f;                           /* рамка правой стрелки 11x11 */
        MRowHit &hh = s_mrow[s_mrows < 32 ? s_mrows++ : 31];
        hh.opt = rw.opt; hh.type = rw.type; hh.y = y; hh.rx0 = chr_x - 9.0f;
        hh.lx0 = hh.lx1 = chr_x;
        hh.slider = 0;
        if (rw.type == DC_OPT_ACTION) {                           /* строка-кнопка: одна стрелка «открыть» */
            text(ichr, chr_x + 5.5f - 12.5f, cy - 12.5f, on ? C_ON : Rgba{ 242, 245, 246, 112 });
            continue;
        }
        if (rw.type == DC_OPT_BIND) {                             /* 04.10 (6): назначения — по правому краю */
            menu_bind_value(r, ta, L, rw, xr, y, on, sc, ox, oy);
            continue;
        }
        int iv = -1;
        float vw;
        if (rw.value_key) { iv = mt_find(DC_MT_VAL, rw.value_key); vw = k_mt_rect[L][iv][2]; }
        else vw = mt_num_width(L, rw.value_num);
        const float pre = (rw.type == DC_OPT_DEVICE && rw.value_num[0]) ? mt_num_width(L, rw.value_num) + 10.0f : 0.0f;
        vw += pre;                                                /* 04.10 (6): «2.» перед подписью геймпада */
        const float cw = std::fmax(colw, vw);
        const float colx = chr_x - 18.0f - cw;                    /* колонка, текст по центру */
        const float tx = colx + (cw - vw) * 0.5f;
        const float vh = (iv >= 0) ? k_mt_rect[L][iv][3] : k_mt_rect[L][i0][3];
        const float ty = y + 1.0f + (54.0f - vh) * 0.5f;
        const Rgba vc = on ? C_ON : Rgba{ 242, 245, 246, 204 };
        if (iv >= 0 && pre > 0.0f) {
            float px = tx;
            const float dy = y + 1.0f + (54.0f - k_mt_rect[L][i0][3]) * 0.5f;
            for (const char *p = rw.value_num; *p; p++) {
                const char kk[2] = { *p, 0 };
                const int id = mt_find(DC_MT_DIG, kk);
                text(id, px, dy, vc);
                px += k_mt_rect[L][id][2];
            }
            text(iv, tx + pre, ty, vc);
        } else if (iv >= 0) {
            text(iv, tx, ty, vc);
        } else {
            float px = tx;
            for (const char *p = rw.value_num; *p; p++) {
                const char kk[2] = { *p, 0 };
                const int id = mt_find(DC_MT_DIG, kk);
                text(id, px, ty, vc);
                px += k_mt_rect[L][id][2];
            }
        }
        const float chl_x = colx - 18.0f - 11.0f;                 /* рамка левой стрелки */
        if (on) {
            text(ichl, chl_x + 5.5f - 12.5f, cy - 12.5f, C_ON);
            text(ichr, chr_x + 5.5f - 12.5f, cy - 12.5f, C_ON);
        }
        hh.lx0 = chl_x - 9.0f; hh.lx1 = chl_x + 20.0f;
        /* 11.10: ползунок числовой строки - полоса от «нуля» (у пределов со знаком) или от начала до значения, бегунок */
        if (sliders && (rw.type == DC_OPT_INT || rw.type == DC_OPT_FLOAT) && rw.num_hi > rw.num_lo) {
            const float x1 = chl_x - 24.0f;
            const float x0 = std::fmax((float)M_LIST_X + 23.0f + lab_max + 40.0f, x1 - 420.0f);
            if (x1 - x0 >= 120.0f) {
                const float lo = rw.num_lo, hi = rw.num_hi;
                auto at = [&](float v) { float f = (v - lo) / (hi - lo); f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); return x0 + f * (x1 - x0); };
                const float kx = at(rw.num_v), zx = (lo < 0.0f && hi > 0.0f) ? at(0.0f) : x0;
                fill(x0, cy - 2.0f, x1 - x0, 4.0f, Rgba{ 242, 245, 246, 46 });
                if (zx != x0) fill(zx - 1.0f, cy - 9.0f, 2.0f, 18.0f, Rgba{ 242, 245, 246, 90 });
                fill(std::fmin(zx, kx), cy - 2.0f, std::fabs(kx - zx), 4.0f, on ? C_TEAL : Rgba{ 127, 227, 214, 150 });
                fill(kx - 6.0f, cy - 13.0f, 12.0f, 26.0f, on ? C_ON : Rgba{ 242, 245, 246, 210 });
                hh.slider = 1; hh.tx0 = x0; hh.tx1 = x1; hh.lo = lo; hh.hi = hi;
            }
        }
    }
    s_msb_on = 0;
    if (n > fit_rows && sb_old()) {                               /* полоса прокрутки (как было до 20.10) */
        const float trk = (float)(fit_rows * M_ROW_H);
        fill((float)(M_LIST_X + M_LIST_W) + 14.0f, (float)M_LIST_Y, 3.0f, trk, Rgba{ 242, 245, 246, 31 });
        fill((float)(M_LIST_X + M_LIST_W) + 14.0f, (float)M_LIST_Y + trk * (float)s_mscroll / (float)n,
             3.0f, trk * (float)fit_rows / (float)n, C_TEAL);
    } else if (n > fit_rows) {                                    /* 20.10: толстая, бегунок тянется мышью */
        const float trk = (float)(fit_rows * M_ROW_H), bw = 12.0f, bx = (float)(M_LIST_X + M_LIST_W) + 12.0f;
        const float th = std::fmax(36.0f, trk * (float)fit_rows / (float)n);
        const float ty = (float)M_LIST_Y + (trk - th) * (float)s_mscroll / (float)(n - fit_rows);
        const int hot = s_msb_drag || (s_mmx >= bx - 8.0f && s_mmx < bx + bw + 8.0f && s_mmy >= ty && s_mmy < ty + th);
        fill(bx, (float)M_LIST_Y, bw, trk, Rgba{ 242, 245, 246, 26 });
        fill(bx, ty, bw, th, hot ? C_ON : C_TEAL);
        s_msb_on = 1; s_msb_n = n; s_msb_fit = fit_rows;
        s_msb_x0 = bx - 8.0f; s_msb_x1 = bx + bw + 8.0f;          /* попасть мышью - с запасом по 8 px с боков */
        s_msb_y0 = (float)M_LIST_Y; s_msb_h = trk; s_msb_ty = ty; s_msb_th = th;
    }

    /* ---- плашка пояснения: как «Прочие функции управления» в ctrl.html */
    box((float)M_LIST_X, (float)M_PANEL_Y, (float)M_LIST_W, (float)M_PANEL_H,
        Rgba{ 242, 245, 246, 5 }, Rgba{ 242, 245, 246, 31 });
    const DcMenuRow *sr = nullptr;
    for (int i = 0; i < n; i++) if (rows[i].opt == sel) sr = &rows[i];
    float py = (float)M_PANEL_Y + 21.0f;                          /* рамка 1 + поле 20 */
    if (sr) {
        const int ih = mt_find(DC_MT_HDR, sr->title);
        text(ih, (float)M_LIST_X + 25.0f, py, C_TEAL);
        py += k_mt_rect[L][ih][3] + 12.0f;
        if (sr->hint) {
            const int ip = mt_find(DC_MT_HINT, sr->hint);
            text(ip, (float)M_LIST_X + 25.0f, py, C_TEXT);
            py += k_mt_rect[L][ip][3];
        }
    }
    const int in = mt_find(DC_MT_NOTE, dc_menu_note_key());
    if (in >= 0) text(in, (float)M_LIST_X + 25.0f, py + 6.0f, C_TEAL);

    if (tf) {
        const SDL_FRect d = { ox, oy + DC_SPLASH_FOOTER_Y * sc, 1920.0f * sc, 120.0f * sc };
        SDL_RenderCopyF(r, tf, nullptr, &d);
    }
    SDL_SetTextureColorMod(ta, 255, 255, 255);
    SDL_SetTextureAlphaMod(ta, 255);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    frame_end(r, &of);
}

extern "C" void dc_menu_ui_mouse(float lx, float ly, int lclick, int lw, int lh)
{
    if (!dc_menu_ui_ready()) return;
    float sc, ox, oy;
    fit(lw, lh, &sc, &ox, &oy);
    if (sc <= 0.0f) return;
    const float x = (lx - ox) / sc, y = (ly - oy) / sc;          /* пиксели кадра дизайна */
    if (!s_mprimed) { s_mmx = x; s_mmy = y; s_mprimed = 1; }     /* где мышь стояла при открытии — не «движение» */
    const int moved = (x != s_mmx || y != s_mmy);
    s_mmx = x; s_mmy = y;
    /* 20.10: полоса прокрутки - взялись за бегунок (или щёлкнули по полосе: бегунок встаёт под мышь) и тянем, пока кнопка зажата */
    auto sb_to = [&](float my) {
        const float span = s_msb_h - s_msb_th;
        float f = span > 0.0f ? (my - s_msb_grab - s_msb_y0) / span : 0.0f;
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        s_mscroll = (int)std::lround(f * (float)(s_msb_n - s_msb_fit));
    };
    if (lclick != 2) s_msb_drag = 0;
    if (s_msb_drag) { if (moved && s_msb_on) sb_to(y); return; }
    if (lclick == 1 && s_msb_on && x >= s_msb_x0 && x < s_msb_x1 && y >= s_msb_y0 && y < s_msb_y0 + s_msb_h) {
        s_msb_grab = (y >= s_msb_ty && y < s_msb_ty + s_msb_th) ? y - s_msb_ty : s_msb_th * 0.5f;
        s_msb_drag = 1;
        sb_to(y);
        return;
    }
    /* 11.10: lclick 1 - щелчок, 2 - левая зажата: тянем ползунок, за который взялись щелчком */
    auto slide = [&](const MRowHit &h) {
        float f = (x - h.tx0) / (h.tx1 - h.tx0);
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        dc_menu_set_value(h.opt, h.lo + f * (h.hi - h.lo));
    };
    if (lclick != 2) s_mdrag = -1;
    if (lclick == 2) {
        if (s_mdrag >= 0) {
            if (moved)
                for (int i = 0; i < s_mrows; i++) if (s_mrow[i].opt == s_mdrag && s_mrow[i].slider) slide(s_mrow[i]);
            return;
        }
        lclick = 0;                                               /* зажата не на ползунке - как простое движение */
    }
    if (!moved && !lclick) return;
    dc_menu_set_dev(0);                                           /* мышь — подсказки клавиатуры */
    for (int p = 0; p < s_mtabs; p++)
        if (lclick && inside(s_mtab[p], x, y)) { dc_menu_set_page(p); return; }
    for (int i = 0; i < s_mrows; i++) {
        const MRowHit &h = s_mrow[i];
        if (x < (float)M_LIST_X || x >= (float)(M_LIST_X + M_LIST_W) || y < h.y || y >= h.y + (float)M_ROW_H) continue;
        if (!lclick) { dc_menu_hover(h.opt); return; }             /* наведение ведёт курсор, как в меню */
        if (h.slider && x >= h.tx0 - 16.0f && x <= h.tx1 + 16.0f) { /* 11.10: щелчок по полосе - значение здесь, дальше тянем */
            s_mdrag = h.opt;
            slide(h);
            return;
        }
        int dir = 0;
        if (h.type == DC_OPT_ACTION || x >= h.rx0) dir = 1;       /* правая стрелка */
        else if (x >= h.lx0 && x < h.lx1) dir = -1;               /* левая стрелка */
        else if (x >= h.lx1) dir = 1;                             /* по самому значению — вперёд */
        dc_menu_click(h.opt, dir);
        return;
    }
}

extern "C" int dc_splash_state(char *buf, int n)
{
    const char *scr = s_screen == SPL_LANG ? "lang" : s_screen == SPL_CTRL ? "ctrl" : s_screen == SPL_DISC ? "disc" : "none";
    PadRow rows[DCS_COUNT];
    const float sh = pad_layout(lang_now(), rows);          /* схема геймпада — для проверки input.ini с порта */
    char m[DCS_COUNT * 11 + 1];
    int k = 0;
    m[0] = 0;
    for (int s = 0; s < DCS_COUNT; s++)
        k += std::snprintf(m + k, sizeof m - (size_t)k, "%s%u", s ? "," : "", rows[s].mask);
    return std::snprintf(buf, (size_t)n,
        "\"active\":%d,\"screen\":\"%s\",\"tab\":\"%s\",\"ctx\":\"%s\",\"dev\":\"%s\",\"lang\":\"%s\","
        "\"sel\":\"%s\",\"hl\":\"%s\",\"open\":%d,\"focus\":\"%s\",\"next_ctrl\":%d,\"next_disc\":%d,\"pad\":[%s],\"shift\":%d",
        s_screen != SPL_NONE, scr, s_tab ? "kb" : "pad", s_from_menu ? "menu" : "boot", s_dev_pad ? "pad" : "kb",
        k_splash_lang_code[lang_now()], k_splash_lang_code[s_sel], k_splash_lang_code[s_hl], s_open,
        s_focus ? "go" : "box", s_next_ctrl, s_next_disc, m, (int)sh);
}
