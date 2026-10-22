#version 460
#extension GL_EXT_nonuniform_qualifier : require
// DC GPU raster backend — fragment stage.
// PS1 texel pipeline transplanted from the CPU fast spans (gpu_sw_renderer.c)
// and dc.rchit: CLUT decode from the VRAM snapshot, texture window, color
// modulation ((t8 * mod8) >> 7 with saturation), 4x4 ordered dither, and the
// per-texel semi-transparency split: the SAME primitive carries opaque
// (bit15=0) and blended (bit15=1) texels, so semi prims render in two passes
// selected by pc.params.x — pass 0 keeps opaque texels (blending off, depth
// write on), pass 1 keeps bit15 texels (pipeline blend state on, no depth
// write). Blend math itself lives in the pipeline blend state.

layout(binding = 0) uniform usampler2D vramTex;
// Hi-res font atlas (DC_FONT_HIRES, default x4): the game's 4bpp font page
// (768,0) rebuilt from the TTF at S times the texel density. Per texel:
//   a = coverage (body + outline), r = body share of that coverage,
//   g = grey index / 15 for the body (vertical gradient), b = 1 -> cell is
//   NOT ours: fall through to the VRAM texel path.
// Colours still come from the primitive's CLUT (body index by g, outline
// index 1), so palette-driven text colours and flashes keep working.
layout(binding = 1) uniform sampler2D fontHi;
// Previous hi-res frame (15.09.2026, gpu_vk_raster.cpp s_prevImg): the briefing terminal lays a
// half-transparent copy of the PREVIOUS frame (15 bpp quads over the display buffer) on every
// frame. From the 1x VRAM that copy carried the software raster's picture - the doctor's bust
// squeezed by the widescreen GTE - over our hi-res bust: two busts. With prevMap.z = 1 a 15 bpp
// texel inside prevRect takes its COLOUR from our previous hi-res frame; transparency and bit 15
// still come from VRAM.
layout(binding = 2) uniform sampler2D prevTex;

// 25.09.2026: MSAA «по выборкам» (dc_raster_ss.frag.spv, -DDC_SAMPLE_SHADING): гладкие входы берутся в точке каждой
// выборки, шейдер работает на выборку — сглаживаются и края прозрачных текселей (решётки, сетки).
#ifdef DC_SAMPLE_SHADING
layout(location = 0) sample  in vec2 v_uv;
layout(location = 1) sample  in vec3 v_color;
#else
layout(location = 0)      in vec2 v_uv;
layout(location = 1)      in vec3 v_color;
#endif
layout(location = 2) flat in ivec4 v_info;   // tpage, clut_x, clut_y, flags
layout(location = 3) flat in vec2  v_uvp;    // stable prim identity (UV of provoking vertex)
#ifdef DC_SAMPLE_SHADING
layout(location = 4) sample  in vec3 v_view;
#else
layout(location = 4)      in vec3  v_view;   // положение в видовом пространстве (тени)
#endif
// 25.09.2026 вечер: СВЕЧЕНИЕ ИСТОЧНИКОВ И ОТРАЖЕНИЯ (gpu_vk_raster.cpp fx_geom_pass). Этот же текст, собранный с
// DC_FX_PASS (dc_fx.frag.spv, вершины — dc_fx.vert), рисует проходы свечения (1) и отражения (2) — цвет текселя как обычно,
// а вывод — свой (см. конец разбора текселя). Основной проход берёт готовое отражение с биндинга 5 на полу «блестит».
#ifdef DC_FX_PASS
layout(location = 5) in float v_rfd;             // высота над полом (проход отражения)
// 06.10.2026: меш из mods отражается и заслоняет свечение своей картинкой (dc_mesh_fx.vert; у потока dc_fx.vert даёт −1)
layout(location = 6) flat in int v_rep;
layout(location = 7) in vec2 v_repuv;
layout(binding = 8) uniform sampler2D repTex[512];
#else
// 05.10.2026: ТЕКСТУРЫ PNG ИЗ MODS (gpu_vk_raster.cpp TEXREP): номер картинки в массиве и место в ней; цвет — из картинки,
// прозрачность — её альфа, полупрозрачность текселя (бит 15) — как у текселя игры под ней
layout(location = 5) flat in int v_rep;
#ifdef DC_SAMPLE_SHADING
layout(location = 6) sample in vec2 v_repuv;
#else
layout(location = 6) in vec2 v_repuv;
#endif
layout(location = 7) in float v_glint;           // 12.10.2026: блеск предметов — место вдоль диагонали предмета 1..2, 0 — не предмет
layout(binding = 8) uniform sampler2D repTex[512];
layout(binding = 5) uniform sampler2D reflTex;   // отражение пола — цель прохода отражения (половинное разрешение)
layout(binding = 6) uniform sampler2D moonPF;    // 25.09 ночь: лунная тень, сглаженная заранее: rg — размытая (доля, доля·глубина), ba — резкая
layout(binding = 7) uniform sampler2DArray shadowPF;   // 27.09 (2): пирамида карт теней ламп — r доля заслонённого, g доля × расстояние / 4096
#endif

// 18.09.2026: карта теней — глубина сцены, снятая из точки лампы игры.
// Слой на лампу: ламп в кадре может быть несколько, их тени умножаются.
layout(binding = 3) uniform sampler2DArray shadowMap;
layout(binding = 4, std140) uniform ShadowUBO {
    mat4 vp[4];       // видовая точка нашей камеры -> отсечённое пространство лампы
    vec4 par[4];      // x — сила, y/z — A и B (глубина карты -> расстояние), w — размер источника в UV·расстояние
    vec4 lpos[4];     // xyz — лампа в видовом пространстве, w — её радиус
    vec4 cfg;         // x — число ламп, y — смещение (мировых единиц), z — 1/размер карты, w — отладка
    vec4 cfg2;        // x — тень на самом персонаже (0 — выкл), y — её множитель смещения
    vec4 flp;         // 25.09: вспышка выстрела/взрыва — точка в видовом пространстве, w = радиус (0 — нет)
    vec4 flc;         //        цвет × яркость, w = «обтекание» (сколько света заходит за край грани)
    vec4 mdir;        // 25.09 вечер: ЛУНА — направление на неё в видовом пространстве, w = её слой карты (-1 — луны нет)
    vec4 mcol;        //        цвет × сила света, w = «обтекание» (27.09 вечер: w < 0 — персонажи по граням, обтекание −1 − w)
    vec4 rfp;         // 25.09 вечер: ОТРАЖЕНИЯ — плоскость пола в видовом пространстве: xyz нормаль (к камере), w = d (n·p = d)
    vec4 rfc;         //        x (не занято с 25.09 ночи), y (не занято), z допуск по высоте над плоскостью, w > 0.5 — включено
    vec4 rfm0;        // 25.09 ночь: МАТЕРИАЛЫ пола — x/y кафель: сила, размытие; z/w металл: сила, размытие (пиксели цели)
    vec4 rfm1;        //        x/y глянец (мокрый пол): сила, размытие; z — металл окрашивает отражение своим цветом (0..1);
                      //        w — 27.09: металл светлеет только от отражённого (без постоянной доли отблеска)
    vec4 mpf;         // 25.09 ночь: ЛУНА ГЛАДКОЙ ТЕНЬЮ — x > 0.5 вкл (биндинг 6), y зазор до полного размытия, z смещение, w ширина перехода (ед.)
    vec4 spare;       // x — число источников света от светящихся текстур (25.09 поздно); y > 0.5 — блики материалов, z — ламп для них
    vec4 glp[8];      //        их точки в видовом пространстве кадра A, w = радиус
    vec4 glc[8];      //        цвет × сила, w = «обтекание»
    vec4 msp;         // 25.09 ночь: БЛИКИ МАТЕРИАЛОВ — x/y кафель: степень, сила; z/w металл: степень, сила
    vec4 msp2;        //        x/y глянец: степень, сила; z — металл в свой цвет (0..1); w — доля бликов от светящихся текстур
    vec4 spp[4];      //        лампы игры для бликов: точка в видовом пространстве кадра A, w = радиус
    vec4 spc[4];      //        их цвет × сила
    vec4 senv;        //        xyz — «вверх» комнаты (к потолку) в видовом пространстве, w > 0.5 — есть
    vec4 msp3;        //        отблеск окружения: x кафель, y металл, z глянец (доля от 120 уровней)
    vec4 vcfg;        // 27.09: объёмный свет (dc_vol_common.glsl) — здесь не читается, только ради раскладки блока
    vec4 v2r[3];
    vec4 r2v[3];
    vec4 vspare;
    vec4 vlp[8];
    vec4 vlc[8];
    vec4 lsh[4];      // 27.09: ТЕНИ ОТ ЛАМП-ТЕКСТУР — эллипс мягкости слоя карты: xy — первый столбец, zw — второй (1 0 0 1 — круг)
    // 27.09 (2): ТЕНИ БЕЗ ЗЕРНА И ДОЛЯ СВЕТА (игрок: «тени шумные и не совсем точные, от нескольких источников сложение не
    // реалистично») — пирамида карты (биндинг 7) вместо выборок с поворотом; тень лампы отнимает только её долю света
    vec4 spf;         //        x > 0.5 — новый путь, y — доля общего (запечённого) света комнаты, z — смещение (ед.), w — переход (ед.)
    vec4 spw;         //        сила света лампы каждого слоя (для доли)
    vec4 sps;         //        x — ламп «только свет» (без своей тени), y — обтекание граней, z/w — запас
    vec4 skA[4];      //        полуоси полутени слоя в плоскости карты (UV · расстояние): xy — длинная, zw — короткая
    vec4 slp[8];      //        лампы без своей тени: точка в виде кадра A + радиус
    vec4 slw[2];      //        их сила
    // 27.09 вечер: ШЕРОХОВАТЫЙ МЕТАЛЛ (см. ниже) — x шероховатость (0 — гладкий, как было), y сила рельефа из текстуры,
    // z матовость ржавчины и грязи, w зерно текселя; все 0 — как было
    vec4 mrg;
    vec4 veilV[4];    // 28.09 (6): пелена игры (dc_vol_common.glsl) — здесь не читается, только ради раскладки блока
    // 12.10.2026: БЛЕСК ПРЕДМЕТОВ — x сила (0 — выкл), y где полоса (0..1 вдоль диагонали предмета), z общий отсвет, w полуширина
    vec4 glint;
    vec4 rsv460;      // запас (460..463)
    // 19.10.2026: СВЕТ СВЕТЯЩИХСЯ ТЕКСТУР, источники 9..16 (первые 8 — glp/glc; сколько здесь — spare.w): точка вида кадра A +
    // радиус и цвет × сила + обтекание, как у glp/glc. Дымка и пыль берут только первые 8.
    vec4 glp2[8];
    vec4 glc2[8];
    vec4 hq;          // 19.10.2026: её доворот в кадрах между тактами (dc_raster.vert); здесь нужен только hc.w
    vec4 hc;
} sh;

layout(push_constant) uniform PC {
    vec4 view;
    vec4 params;      // pass, body fade (1 = solid), fade dither cell px, flags
    vec4 prevRect;    // previous frame's display rect in draw space: x, y, w, h
    vec4 prevMap;     // active share of the image x, y; z = 1 -> use prevTex; w unused
    vec4 wq;          // (поля вершинной ступени: перепроецирование кадра B)
    vec4 wt;
    vec4 wd;
    vec4 wp;
} pc;

layout(location = 0) out vec4 out_color;

// 09.10.2026: MSAA «ПО ВЫБОРКАМ» СО СВЕТОМ НА ПИКСЕЛЬ (dc_raster_sl.frag.spv, -DDC_SAMPLE_LOOP). Прежний вариант (dc_raster_ss,
// шейдер на каждую выборку) считал всё четыре раза на пиксель — и тени ламп (до десятков выборок пирамиды на лампу), и луну,
// и свет, хотя по выборкам меняется только тексель. Здесь шейдер идёт один раз на пиксель: тексель (цвет, прозрачность,
// полупрозрачность) — в точке каждой покрытой выборки (interpolateAtSample), прозрачные снимаются маской gl_SampleMask — края
// решёток, сеток и букв сглажены, как прежде; свет, тени, луна, вспышки, отражения и блики — один раз, по среднему цвету.
// Входы текселя — через переменные: в прочих вариантах это просто входы.
vec2 g_uv;              // UV текселя
vec3 g_col;             // цвет вершины
vec2 g_ruv;             // место в картинке PNG из mods
vec2 g_dUx, g_dUy;      // производные UV по пикселю
vec2 g_dRx, g_dRy;      // производные места в картинке PNG (цикл выборок: выборка с явными производными)
#ifdef DC_SAMPLE_LOOP
#define TEX_D(smp, uv, dx, dy) textureGrad(smp, uv, dx, dy)
#define TEX_0(smp, uv) textureLod(smp, uv, 0.0)
#else
#define TEX_D(smp, uv, dx, dy) texture(smp, uv)
#define TEX_0(smp, uv) texture(smp, uv)
#endif

// flags bits (must match gpu_sw_renderer.c dcvk_mirror_textri)
const int F_TEXTURED = 1;
const int F_SEMI     = 2;
const int F_RAW      = 4;
const int F_TWIN     = 8;    // F_BLIT: VRAM refresh quad (upload/copy mirror)
const int F_ACTOR    = 16;   // primitive from the character assembler (Path B)
const int F_HIFONT   = 64;   // textured with the game's font page: sample fontHi first
const int F_HIFONT2  = 128;  // ...from the title-strip page (704,0): lower half of fontHi
const int F_DITHER   = 256;  // room poly with a dither mask over a drawn mesh: fine checker only (pt_renderer.c rl_emit_poly)
const int F_DLATT    = 512;  // ...checker-only texture: procedural chain-link (period MESH_P); bits 10..21 = cell colour 4:4:4
const int F_DPH      = 4194304;  // bit 22: checker phase (which parity of u+v is punched)
const int F_SUBST    = 1024;     // red grille -> sample the page's chain-link window inside the checker zone
const int F_CHAR     = 8388608;  // bit 23, 18.09: полигон персонажа (Регина, NPC, звери) — окклюдер тени
const int F_GLASS    = 2097152;  // bit 21, 01.10 вечер: стекло (gpu_sw_renderer.c dc_glass_prim; проход params.x = 2, конвейер 11)

// 29.09 поздно (игрок: «назначь сам материалы текстурам: металл, бумага, бетон (матовая), кафель, жидкость (кровь),
// экран»): материал текстуры — биты 21..22 и 20 страницы (gpu.c dc_tex_class): 0 нет, 1 кафель, 2 металл, 3 глянец,
// 4 бумага, 5 бетон (матовая), 6 жидкость (кровь), 7 экран. У рабицы (F_SUBST) биты 16..23 — окно рабицы, у нетекстурных
// материала нет. DC_MAT_EXT=0 (gpu.c) — номера 4..7 сюда не доходят, всё как было.
int texMat(int flags, int info) {
    if ((flags & F_TEXTURED) == 0 || (flags & F_SUBST) != 0) return 0;
    return ((info >> 21) & 3) | (((info >> 20) & 1) << 2);
}
// блик и отблеск: бумага и бетон — матовые (0), жидкость — как глянец (3), экран — стекло, слабый узкий блик как кафель (1)
int texMatShade(int m) { return (m == 6) ? 3 : (m == 7) ? 1 : (m >= 4) ? 0 : m; }

// 4x4 Bayer threshold matrix, 0..15
const int bayer4[16] = int[16]( 0,  8,  2, 10,
                               12,  4, 14,  6,
                                3, 11,  1,  9,
                               15,  7, 13,  5);

uint vramFetch(int x, int y) {
    return texelFetch(vramTex, ivec2(x & 1023, y & 511), 0).r;
}

uint sampleTexel(int u, int v, int tpage, int cx, int cy) {
    int tpx = (tpage & 15) * 64;
    int tpy = ((tpage >> 4) & 1) * 256;
    int mode = (tpage >> 7) & 3;
    if (mode == 0) {
        uint w = vramFetch(tpx + (u >> 2), tpy + v);
        uint nib = (w >> ((u & 3) * 4)) & 0xFu;
        return vramFetch(cx + int(nib), cy);
    } else if (mode == 1) {
        uint w = vramFetch(tpx + (u >> 1), tpy + v);
        uint b = (w >> ((u & 1) * 8)) & 0xFFu;
        return vramFetch(cx + int(b), cy);
    }
    return vramFetch(tpx + u, tpy + v);
}

uint sampleWin(int u, int v, int iy, int iz, int tpage) {
    u &= 255; v &= 255;
    int twmx = (iy >> 10) & 31, twox = (iy >> 15) & 31;
    int twmy = (iz >> 10) & 31, twoy = (iz >> 15) & 31;
    if ((twmx | twmy) != 0) {
        u = (u & ~(twmx * 8)) | ((twox & twmx) * 8);
        v = (v & ~(twmy * 8)) | ((twoy & twmy) * 8);
    }
    return sampleTexel(u, v, tpage, iy & 0x3FF, iz & 0x3FF);
}

vec3 t5to8(uint t) {
    return vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u)) * 8.0;
}

// FINE MESH ONLY (14.09.2026 evening): is texel (u,v) inside the dither
// checker zone? An opaque texel with >= 2 transparent 4-neighbours (the
// checker's own texels and the drawn bars sitting in it), or a transparent
// texel with >= 2 opaque texels within radius 2 (checker holes and the small
// drawn holes of the mesh). A solid frame / panel and a big real hole are not.
bool dzone(int u, int v, uint texel, int iy, int iz, int tp) {
    if (texel != 0u) {
        int tr = 0;
        if (sampleWin(u - 1, v, iy, iz, tp) == 0u) tr++;
        if (sampleWin(u + 1, v, iy, iz, tp) == 0u) tr++;
        if (sampleWin(u, v - 1, iy, iz, tp) == 0u) tr++;
        if (sampleWin(u, v + 1, iy, iz, tp) == 0u) tr++;
        return tr >= 2;
    }
    int op = 0;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if ((dx != 0 || dy != 0) && sampleWin(u + dx, v + dy, iy, iz, tp) != 0u) op++;
    return op >= 2;
}

const int dither4[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1,
                                -3, 1, -4, 0, 3, -1, 2, -2);

// 25.09.2026 — МАСКА ЗАТЕНЕНИЯ УГЛОВ (SSAO) В АЛЬФЕ. params.w бит 128: в этом кадре работает SSAO; бит 256: draw
// непрозрачный с записью глубины (конвейер 0). Такой пишет альфу 1 — «эту точку закрасила 3D-геометрия», плоские
// непрозрачные (интерфейс, текст, экраны) — 0; полупрозрачные альфу не меняют (смешение альфы ZERO/ONE). Без бита 128
// альфа 1, как было. Проход умножения (dc_ssao_comp.frag) возвращает альфу в 1.
// 01.10.2026: доля затенения у ячейки «третьей энергии» (1 — обычная точка). Альфа 0.5..1 = «затенять с долей»:
// dc_ssao_z.frag кладёт долю в g, dc_ssao_comp.frag гасит затенение этой долей.
float g_aoCell = 1.0;
float aoMaskOut() {
    int pw = int(pc.params.w + 0.5);
    if ((pw & 128) == 0) return 1.0;
    if ((pw & 256) == 0) return 0.0;
    return (g_aoCell >= 0.999) ? 1.0 : 0.5 + 0.5 * g_aoCell;
}

// TEXEL COORDINATE SNAP (15.09.2026 late night, player, slot 0: "the lamp - double polygons z-fighting"). Not
// twins: the lamp's bottom is mapped onto ONE texture row (v = 222 at every vertex), exactly on a
// texel boundary. Perspective-correct interpolation lands on 221.99998 in some pixels and 222.00001
// in the next, int() picks row 221 (dark) or 222 (light) - grey-white noise. The console
// interpolates in fixed point: exactly 222. Round to 1/256 texel before truncating - far below a
// texel, far above the float error. pc.params.w bit 64, DC_UV_SNAP=0 - as before.
const int P_UVSNAP = 64;
vec2 texelUV() {
    if ((int(pc.params.w + 0.5) & P_UVSNAP) == 0) return g_uv;
    return floor(g_uv * 256.0 + 0.5) / 256.0;
}

// ===== МЯГКАЯ ТЕНЬ ОТ ЛАМП ИГРЫ (18.09.2026, вечер) ====================
// Карта глубины снята ИЗ ТОЧКИ ЛАМПЫ и держит только подвижное. Фрагмент
// переводится в пространство лампы и сравнивается с картой. Край не просто
// сглажен: сначала ищется, НАСКОЛЬКО ДАЛЕКО от приёмника стоит заслонивший,
// и ширина полутени берётся от этого расстояния — у ног тень резкая, к
// своему концу расходится и тает. Так ведёт себя тень от лампы, а не от
// математической точки.
const vec2 SH_DISK[12] = vec2[12](
    vec2( 0.000,  1.000), vec2( 0.866,  0.500), vec2( 0.866, -0.500),
    vec2( 0.000, -1.000), vec2(-0.866, -0.500), vec2(-0.866,  0.500),
    vec2( 0.000,  0.520), vec2( 0.450,  0.260), vec2( 0.450, -0.260),
    vec2( 0.000, -0.520), vec2(-0.450, -0.260), vec2(-0.450,  0.260));

vec2 shRot(vec2 v, vec2 r) { return vec2(v.x * r.x - v.y * r.y, v.x * r.y + v.y * r.x); }
// 27.09: форма мягкости слоя (лампа-текстура — вытянутая полоса, панель): выборки диска растянуты эллипсом её осей в карте
vec2 shShape(int idx, vec2 v) {
    vec4 e = sh.lsh[idx];
    if (abs(e.x) + abs(e.y) + abs(e.z) + abs(e.w) < 1e-4) return v;     // старый блок без формы — круг
    return vec2(e.x * v.x + e.z * v.y, e.y * v.x + e.w * v.y);
}

// поворот вектора кватернионом и обратный к нему (кадр B, см. dc_raster.vert)
vec3 qrotf(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
vec3 shadowViewPoint() {
    if (pc.wt.w <= 0.5) return v_view;                       // обычный кадр
    if ((v_info.w & F_ACTOR) != 0 && pc.wd.w > 0.5) return v_view;   // 20.09: в прицеливании её вершины кадр B не трогает
    if ((v_info.w & F_ACTOR) != 0 && sh.hc.w > 0.5) return v_view;   // 19.10: при её довороте вершинная ступень отдала точку кадра A
    vec3 p = qrotf(vec4(-pc.wq.xyz, pc.wq.w), v_view - pc.wt.xyz);
    if ((v_info.w & F_ACTOR) != 0) p -= pc.wd.xyz;           // её вершины ехали с пивотом
    return p;
}

// 0 — фрагмент вне поля зрения лампы, 1 — в нём, 2 — грань от лампы отвёрнута
int shadowProbe(int idx, vec3 P, vec3 N) {
    vec4 lp = sh.vp[idx] * vec4(P, 1.0);
    if (lp.w <= 0.0) return 0;
    vec3 nd = lp.xyz / lp.w;
    vec2 uv = nd.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || nd.z <= 0.0 || nd.z >= 1.0) return 0;
    vec3 toL = sh.lpos[idx].xyz - P;
    float Ld = length(toL);
    return (Ld > 1e-3 && dot(N, toL / Ld) > 0.03) ? 1 : 2;
}

float shadowLit(int idx, vec3 P, vec3 N, vec2 rot, float biasK) {
    vec4 lp = sh.vp[idx] * vec4(P, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 nd = lp.xyz / lp.w;
    vec2 uv = nd.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || nd.z <= 0.0 || nd.z >= 1.0)
        return 1.0;
    vec3 toL = sh.lpos[idx].xyz - P;
    float Ld = length(toL);
    float ndl = (Ld > 1e-3) ? dot(N, toL / Ld) : 0.0;
    if (ndl <= 0.03) return 1.0;            // грань отвёрнута от лампы — её света там и так нет
    float A = sh.par[idx].y, B = sh.par[idx].z;
    float zr = B / max(A - nd.z, 1e-6);     // расстояние лампа -> фрагмент, в единицах комнаты
    float bias = sh.cfg.y * biasK * (1.0 + 3.0 * (1.0 - ndl));   // смещение тоже в единицах комнаты
    float kp = sh.par[idx].w;               // размер источника, пересчитанный в UV·расстояние
    float texel = sh.cfg.z;
    float srad = clamp(kp / zr, texel, 0.06);
    // 1) кто заслонил и насколько он дальше от лампы, чем приёмник
    float zb = 0.0, nb = 0.0;
    for (int i = 0; i < 12; i += 2) {
        float d = texture(shadowMap, vec3(uv + shShape(idx, shRot(SH_DISK[i], rot)) * srad, float(idx))).r;
        if (d >= 1.0) continue;
        float z = B / max(A - d, 1e-6);
        if (z < zr - bias) { zb += z; nb += 1.0; }
    }
    if (nb < 0.5) return 1.0;               // никто не заслоняет
    zb /= nb;
    // 2) ширина полутени растёт с зазором «заслонивший -> приёмник»
    float pen = clamp(kp * (zr - zb) / (zb * zr), texel, srad);
    float lit = 0.0;
    for (int i = 0; i < 12; i++) {
        float d = texture(shadowMap, vec3(uv + shShape(idx, shRot(SH_DISK[i], rot)) * pen, float(idx))).r;
        float z = B / max(A - d, 1e-6);
        lit += (d < 1.0 && z < zr - bias) ? 0.0 : 1.0;
    }
    lit /= 12.0;
    // 3) тень тает с удалением от того, кто её отбросил, и по краю поля лампы.
    //    rr — расстояние, на котором свет лампы сходит на нет (радиус из
    //    таблицы игры, умноженный на DC_SHADOW_FADE): там же гаснет и тень.
    float rr = max(sh.lpos[idx].w, 1.0);                  /* докуда тень вообще видна */
    float rgap = max(rr * sh.cfg2.w, 1.0);                /* на чём она распыляется к концу */
    lit = mix(1.0, lit, clamp(1.0 - (zr - zb) / rgap, 0.0, 1.0));
    lit = mix(1.0, lit, clamp((rr - Ld) / (rr * 0.45), 0.0, 1.0));
    vec2 e = min(uv, 1.0 - uv);
    lit = mix(1.0, lit, smoothstep(0.0, 0.03, min(e.x, e.y)));
    return lit;
}

// ===== ЛУНА ГЛАДКОЙ ТЕНЬЮ (25.09 ночь, игрок: «тень от этого источника света очень шумная») =====
// Тень луны широкая и мягкая, и 12 выборок с поворотом на пиксель давали в её полутени зерно, а поиск заслонившего
// (6 выборок по большому кругу) — дыры прямо в тени. Здесь слой луны заранее ужат и размыт (gpu_vk_raster.cpp, проходы
// после карты теней): в каждой точке — доля заслонённого и средняя глубина заслонивших, резко (ba) и размыто (rg).
// Выборка — две билинейные, шума нет вовсе. Поверхность дальше заслонивших (глубже по лучу луны) в тени; у ног — резкая
// доля, к концу тени (зазор растёт) — размытая. mpf.x = 0 (DC_MOON_PF=0) — прежний путь shadowLit.
#ifdef DC_FX_PASS
float moonLitPF(int idx, vec3 P, vec3 N) { return 1.0; }
#else
float moonLitPF(int idx, vec3 P, vec3 N) {
    vec4 lp = sh.vp[idx] * vec4(P, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 nd = lp.xyz / lp.w;
    vec2 uv = nd.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || nd.z <= 0.0 || nd.z >= 1.0)
        return 1.0;
    float ndl = dot(N, sh.mdir.xyz);
    if (ndl <= 0.03) return 1.0;            // грань отвёрнута от луны — её света там и так нет
    vec4 m = texture(moonPF, uv);
    if (m.r < 0.002 && m.b < 0.002) return 1.0;
    float A = sh.par[idx].y, B = sh.par[idx].z;
    float zr = B / max(A - nd.z, 1e-6);
    float z1 = B / max(A - m.g / max(m.r, 1e-4), 1e-6);    // средняя глубина заслонивших: размыто
    float z0 = B / max(A - m.a / max(m.b, 1e-4), 1e-6);    //                                 резко
    float bias = sh.mpf.z * (1.0 + 3.0 * (1.0 - ndl));
    float in1 = smoothstep(bias, bias + sh.mpf.w, zr - z1);
    float in0 = smoothstep(bias, bias + sh.mpf.w, zr - z0);
    float t = smoothstep(0.0, sh.mpf.y, zr - z1);           // у ног резко, к концу тени — размыто
    float occ = mix(m.b * in0, m.r * in1, t);
    vec2 e = min(uv, 1.0 - uv);
    occ *= smoothstep(0.0, 0.03, min(e.x, e.y));
    return 1.0 - clamp(occ, 0.0, 1.0);
}
#endif

// ===== ТЕНИ БЕЗ ЗЕРНА (27.09 (2), игрок: «тени выглядят очень шумными и не совсем точными. От нескольких источников сложение
// не реалистично работает») =====
// Шум давали 12 выборок с поворотом на пиксель по кругу, растянутому до размера лампы-текстуры (у полосы 1006 ед. — полосы и
// зерно), и поиск заслонившего по 6 точкам. Теперь слой лампы заранее ужат в пирамиду (gpu_vk_raster.cpp spf_run: доля
// заслонённого и доля × расстояние до заслонившего, уровни 1024 -> 4), и это тот же PCSS, только каждая «выборка» — гладкое
// среднее по окну нужного размера: (1) заслонившие вокруг — окно размером с лампу, видимую из точки; (2) полутень — от зазора
// «заслонивший -> точка», зазор уточняется по окну самой полутени (у ног резко); (3) доля заслонённого — кубическим B-сплайном
// по двум соседним уровням. Вытянутая лампа (полоса) — до 5 таких выборок вдоль её длинной оси. Ни зерна, ни полос.
#ifdef DC_FX_PASS
float shadowOccPF(int idx, vec3 P, float biasK) { return 0.0; }
#else
vec2 pfCubic(vec2 uv, float layer, int lv) {
    vec2 ts = vec2(textureSize(shadowPF, lv).xy);
    vec2 p = uv * ts - 0.5;
    vec2 i = floor(p), f = p - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) * (1.0 / 6.0);
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) * (1.0 / 6.0);
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) * (1.0 / 6.0);
    vec2 w3 = f3 * (1.0 / 6.0);
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 h0 = (i - 0.5 + w1 / g0) / ts;       // пара текселей i-1, i одной билинейной выборкой
    vec2 h1 = (i + 1.5 + w3 / g1) / ts;       // пара i+1, i+2
    float l = float(lv);
    vec2 a = textureLod(shadowPF, vec3(h0.x, h0.y, layer), l).rg;
    vec2 b = textureLod(shadowPF, vec3(h1.x, h0.y, layer), l).rg;
    vec2 c = textureLod(shadowPF, vec3(h0.x, h1.y, layer), l).rg;
    vec2 d = textureLod(shadowPF, vec3(h1.x, h1.y, layer), l).rg;
    return g0.y * (g0.x * a + g1.x * b) + g1.y * (g0.x * c + g1.x * d);
}
// гладкое окно уровня lod (дробный — смесь двух соседних уровней)
vec2 pfSmooth(vec2 uv, float layer, float lod, int nlv) {
    lod = clamp(lod, 0.0, float(nlv - 1));
    int l0 = int(lod);
    float t = lod - float(l0);
    vec2 v = pfCubic(uv, layer, l0);
    if (t > 0.01 && l0 + 1 < nlv) v = mix(v, pfCubic(uv, layer, l0 + 1), t);
    return v;
}
// окно радиуса r (тексели уровня 0) -> уровень пирамиды: уровень L размывает примерно на σ = 0.85·2^L текселей
float pfLod(float r) { return log2(max(r, 1e-4) * (1.0 / 1.7)); }

// доля заслонённого света лампы слоя idx в точке P: 0 — ничто не заслоняет, 1 — лампу не видно вовсе
float shadowOccPF(int idx, vec3 P, float biasK) {
    vec4 lp = sh.vp[idx] * vec4(P, 1.0);
    if (lp.w <= 0.0) return 0.0;
    vec3 nd = lp.xyz / lp.w;
    vec2 uv = nd.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || nd.z <= 0.0 || nd.z >= 1.0) return 0.0;
    const float A = sh.par[idx].y, B = sh.par[idx].z;
    const float zr = B / max(A - nd.z, 1e-6);                  // лампа -> точка, единицы комнаты
    const int nlv = textureQueryLevels(shadowPF);
    const float R0 = float(textureSize(shadowPF, 0).x);
    const float lay = float(idx);
    const vec4 k = sh.skA[idx];                                // полуоси лампы в плоскости карты, UV · расстояние
    const float la = length(k.xy), lb = length(k.zw);
    const float lmax = float(nlv - 1);
    // 1) заслонившие вокруг: окно — лампа, видимая из точки (как поиск PCSS), одна выборка пирамиды
    vec2 m = textureLod(shadowPF, vec3(uv, lay), clamp(pfLod(max(la, lb) / zr * R0), 0.0, lmax)).rg;
    if (m.r < 0.002) return 0.0;
    float zb = m.g / m.r * 4096.0;
    // 2) полутень от зазора «заслонивший -> точка»; зазор уточняется по окну самой полутени — у ног заслоняют ступни, а не
    //    всё тело, и тень там резкая
    float g = max(zr - zb, 0.0) / max(zb * zr, 1.0);
    for (int it = 0; it < 2; it++) {
        vec2 m2 = textureLod(shadowPF, vec3(uv, lay), clamp(pfLod(lb * g * R0), 0.0, lmax)).rg;
        if (m2.r < 0.002) break;
        zb = m2.g / m2.r * 4096.0;
        g = max(zr - zb, 0.0) / max(zb * zr, 1.0);
    }
    const float bias = sh.spf.z * biasK, tw = max(sh.spf.w, 1.0);
    // 3) доля заслонённого по окну полутени; у вытянутой лампы — выборки вдоль её длинной оси
    const float rMaj = la * g * R0, rMin = lb * g * R0;       // полуоси полутени, тексели уровня 0
    int n = 1; float span = 0.0;
    if (rMaj > rMin * 1.5) { n = (rMaj > rMin * 3.5) ? 5 : 3; span = rMaj - rMin; }
    const float stp = (n > 1) ? 2.0 * span / float(n - 1) : 0.0;
    const float lod = pfLod(max(rMin, stp * 0.6));
    const vec2 dirA = (la > 1e-9) ? k.xy / la : vec2(1.0, 0.0);
    float occ = 0.0;
    for (int t = 0; t < 5; t++) {
        if (t >= n) break;
        const vec2 q = uv + dirA * ((-span + stp * float(t)) / R0);
        const vec2 v = pfSmooth(q, lay, lod, nlv);
        if (v.r < 0.002) continue;
        occ += v.r * smoothstep(bias, bias + tw, zr - v.g / v.r * 4096.0);   // заслонившие перед точкой, а не за ней
    }
    occ /= float(n);
    // 4) тень распыляется к концу (зазор до заслонившего, «spread») и гаснет у края карты
    const float rgap = max(sh.lpos[idx].w * sh.cfg2.w, 1.0);
    occ *= clamp(1.0 - (zr - zb) / rgap, 0.0, 1.0);
    const vec2 e = min(uv, 1.0 - uv);
    occ *= smoothstep(0.0, 0.03, min(e.x, e.y));
    return clamp(occ, 0.0, 1.0);
}
#endif
// сила света лампы в точке (для доли света): спад (1 - (d/R)^2)^2 и косинус с обтеканием wr
float lampE(vec3 L, float R, float pw, vec3 P, vec3 N, float wr) {
    vec3 toL = L - P;
    float d = length(toL);
    if (pw <= 0.0 || d >= R) return 0.0;
    float x = d / R, a = 1.0 - x * x;
    float ndl = (d > 1e-3) ? dot(N, toL / d) : 1.0;
    return pw * a * a * clamp((ndl + wr) / (1.0 + wr), 0.0, 1.0);
}

// ===== РАЗБОР ТЕКСЕЛЯ (09.10.2026: вынесен из main в texelPath) ===================================================
// Цвет текселя × цвет вершины (rgb8), «альбедо» для света (alb8) и «смешивается ли точка» (blend_this). false — точка
// прозрачна или не этого прохода (прежде здесь был discard). Входы — через g_uv / g_col / g_ruv: в варианте DC_SAMPLE_LOOP
// они взяты в точке выборки, в прочих — как пришли. Текст разбора — прежний.
bool texelPath(const int flags, const int pass_blend, out vec3 rgb8, out vec3 alb8, out bool blend_this) {
    rgb8 = vec3(0.0);
    alb8 = vec3(0.0);          // 25.09: «альбедо» для вспышки — цвет текселя (или вершины у нетекстурированных)
    blend_this = false;
    bool texel_blend = false;

    if ((flags & F_DITHER) != 0) {
        // CHAIN-LINK, NO CHECKER (14.09.2026 evening, player's two frames: "replace
        // mesh 1 (checker) with mesh 2 (the yard fence's diamond wire); where 2
        // already exists, remove 1"). A drawn lattice punched with a checker: keep
        // the wire texels (colour differs from the cell colour), the cells and the
        // punched texels are see-through. A checker-only texture: a procedural
        // diamond wire of period MESH_P in the texture's own colour, see-through
        // between the wires. Outside the checker zone (frame, panels): as drawn.
        int iy = v_info.y, iz = v_info.z;
        vec2 tuv = texelUV();
        int u = int(tuv.x), v = int(tuv.y);
        int zc = (flags >> 10) & 4095;
        vec3 cell8 = vec3(float(zc & 15), float((zc >> 4) & 15), float((zc >> 8) & 15)) * 17.0;
        uint texel = sampleWin(u, v, iy, iz, v_info.x);
        vec3 t8;
        if ((flags & F_DLATT) != 0) {
            // PURE CHECKER (14.09.2026 evening, final): a fine dither grille whose
            // "low-res underlay" is drawn both as colour and as extra holes. Inside
            // the checker zone draw ONE native-phase checker in the cell colour
            // (punched parity see-through, the other parity filled whatever the
            // texel), outside it (frame) the texture as drawn.
            int ph = ((flags & F_DPH) != 0) ? 1 : 0;
            if (dzone(u, v, texel, iy, iz, v_info.x)) {
                if (((u + v) & 1) == ph) return false;
                t8 = cell8;
            } else {
                if (texel == 0u) return false;
                t8 = t5to8(texel);
            }
            texel_blend = false;
        } else if ((flags & F_SUBST) != 0) {
            // CHAIN-LINK INSTEAD OF THE FINE DITHER GRILLE (14.09.2026 evening, player's
            // markup: green = chain-link is right, red = the fine grille is wrong): inside
            // the checker zone sample a clean 64x64 window of the page's chain-link with
            // its own CLUT; outside the zone (frame) the texture as drawn.
            // 15.09: packing (pt_renderer.c rl_emit_poly): window wu/16, wv/16 in tpage bits
            // 16..19 / 20..23, chain-link CLUT (x/16-48, y-496) in bits 20..23 of the CLUT
            // fields, scale x4 in flags bits 11..15 - the chain-link runs at S x the
            // grille's UV rate so its cells match the real chain-link next to it.
            if (dzone(u, v, texel, iy, iz, v_info.x)) {
                int scx = (((iy >> 20) & 15) + 48) * 16, scy = ((iz >> 20) & 15) + 496;
                int wu = ((v_info.x >> 16) & 15) * 16, wv = ((v_info.x >> 20) & 15) * 16;
                float sc = float((flags >> 11) & 31) * 0.25;
                if (sc <= 0.0) sc = 1.0;
                int su = int(floor(g_uv.x * sc)) & 63, sv = int(floor(g_uv.y * sc)) & 63;
                uint t2 = sampleTexel(wu + su, wv + sv, v_info.x, scx, scy);
                if (t2 == 0u) return false;
                t8 = t5to8(t2);
            } else {
                if (texel == 0u) return false;
                t8 = t5to8(texel);
            }
            texel_blend = false;
        } else {
            // WIRE ON REAL HOLES + CHECKER (val 2): every opaque texel is wire (or
            // frame) and stays, unless it is an isolated speck; a punched texel ON
            // a wire line (two opaque neighbours on opposite sides) is filled from
            // them, every other transparent texel is a real hole.
            uint n[8];
            n[0] = sampleWin(u - 1, v, iy, iz, v_info.x); n[1] = sampleWin(u + 1, v, iy, iz, v_info.x);
            n[2] = sampleWin(u, v - 1, iy, iz, v_info.x); n[3] = sampleWin(u, v + 1, iy, iz, v_info.x);
            n[4] = sampleWin(u - 1, v - 1, iy, iz, v_info.x); n[5] = sampleWin(u + 1, v + 1, iy, iz, v_info.x);
            n[6] = sampleWin(u - 1, v + 1, iy, iz, v_info.x); n[7] = sampleWin(u + 1, v - 1, iy, iz, v_info.x);
            if (texel != 0u) {
                int cnt = 0;
                for (int i = 0; i < 8; i++) if (n[i] != 0u) cnt++;
                if (cnt == 0) return false;
                t8 = t5to8(texel);
                texel_blend = (texel & 0x8000u) != 0u;
            } else {
                vec3 acc = vec3(0.0); float cnt = 0.0;
                for (int i = 0; i < 8; i += 2)
                    if (n[i] != 0u && n[i + 1] != 0u) { acc += t5to8(n[i]) + t5to8(n[i + 1]); cnt += 2.0; }
                if (cnt == 0.0) return false;
                t8 = acc / cnt;
                texel_blend = false;
            }
        }
        rgb8 = min(t8 * g_col / 128.0, vec3(255.0));
        alb8 = t8;
    } else if ((flags & F_TWIN) != 0) {
        // F_BLIT — VRAM refresh quad (upload/copy mirrored in stream order):
        // raw 15bpp fetch at absolute VRAM coords, no CLUT, no transparency.
#ifndef DC_FX_PASS
        if (v_rep >= 0) {
            // 05.10: залит экран из mods — его картинка PNG (любого размера) вместо залитых пикселей
            rgb8 = TEX_D(repTex[nonuniformEXT(v_rep)], g_ruv, g_dRx, g_dRy).rgb * 255.0;
        } else
#endif
        {
        uint texel = vramFetch(int(g_uv.x), int(g_uv.y));
        rgb8 = vec3(float(texel & 31u), float((texel >> 5) & 31u),
                    float((texel >> 10) & 31u)) * 8.0;
        }
    } else if ((flags & F_TEXTURED) != 0) {
        int iy = v_info.y, iz = v_info.z;
        int pw = int(pc.params.w + 0.5);
        bool true_color = (pw & 1) != 0;
        bool tex_filter = (pw & 2) != 0;
        vec3 t8;
        bool hifont_done = false;
        if (v_rep >= 0) {                     // 06.10.2026: и в проходах свечения/отражения (меш из mods)
            vec4 rc = TEX_D(repTex[nonuniformEXT(v_rep)], g_ruv, g_dRx, g_dRy);
            if (rc.a < 0.5) return false;
            vec2 ruv = texelUV();
            uint rtx = sampleWin(int(ruv.x), int(ruv.y), iy, iz, v_info.x);
            texel_blend = (rtx & 0x8000u) != 0u;
            t8 = rc.rgb * 255.0;
            hifont_done = true;   // дальше — как у готового текселя
        } else
        if ((flags & F_HIFONT) != 0) {
            // page-relative texel -> atlas coordinate (the atlas covers the
            // whole 256x256 page, bilinear across the S x S sub-texels)
            // the image holds two pages stacked: font page on top, title strips below
            float pageOff = ((flags & F_HIFONT2) != 0) ? 256.0 : 0.0;
            vec2 fuv = vec2(mod(g_uv.x, 256.0) / 256.0, (mod(g_uv.y, 256.0) + pageOff) / 512.0);
            vec4 hf = TEX_D(fontHi, fuv, g_dUx * vec2(1.0 / 256.0, 1.0 / 512.0), g_dUy * vec2(1.0 / 256.0, 1.0 / 512.0));
            if (hf.b < 0.5) {
                if (hf.a < 0.5) return false;                 // crisp edge at S x density
                int gi = clamp(int(hf.g * 15.0 + 0.5), 1, 15);
                vec3 body = t5to8(vramFetch((iy & 0x3FF) + gi, iz & 0x3FF));
                vec3 outl = t5to8(vramFetch((iy & 0x3FF) + 1,  iz & 0x3FF));
                t8 = mix(outl, body, clamp(hf.r, 0.0, 1.0));
                texel_blend = false;
                hifont_done = true;
            }
        }
        bool prev_done = false;
        if (!hifont_done && pc.prevMap.z > 0.5 && ((v_info.x >> 7) & 3) == 2) {
            // 15 bpp texel = a copy of the display buffer? colour from the previous hi-res frame
            vec2 cuv = g_uv;
            float dome = 1.0;
            if (pc.prevMap.z > 2.5) {
                // 21.09.2026: SMOOTH SCENE-CAPTURE CELL. The game warps the captured piece with four quads sharing a displaced
                // centre vertex - a piecewise-linear pyramid whose creases (and the flat x1.078 tint of the whole cell) read as
                // rectangles at 1080p. Here the cell is ONE undisplaced quad (g_uv = position inside the cell, texels) and the
                // displacement is a smooth dome: D * smoothstep(1 - r), zero with zero slope at the rim. Inverse by fixed point:
                // find p with p + D f(p) = this fragment. Cell size rides in the tpage high bits, D in the CLUT fields.
                vec2 hs = vec2(float(((v_info.x >> 9) & 63) * 4), float(((v_info.x >> 15) & 63) * 4)) * 0.5;
                vec2 dn = (vec2(float(v_info.y), float(v_info.z)) - 32768.0) / 16.0 / hs;
                float dl = length(dn);
                if (dl > 0.62) dn *= 0.62 / dl;                 // keep the map one-to-one (max slope of the dome is 1.5)
                vec2 sp = g_uv / hs - 1.0;
                vec2 p = sp;
                for (int it = 0; it < 20; it++) {
                    float u1 = clamp(1.0 - length(p), 0.0, 1.0);
                    dome = u1 * u1 * (3.0 - 2.0 * u1);
                    p = sp - dn * dome;
                }
                cuv = (p + 1.0) * hs;
                // 01.10.2026 (player, slot 6: "visual jitter"): SSAO is computed from the UNdisplaced depth of the world, so
                // under a dome its dark creases and halos (Regina's outline!) lie beside the displaced picture and swim at
                // 30 Hz. Fade the occlusion out where the picture is shifted by more than ~2 output pixels (prevMap.w bit 2).
                if ((int(pc.prevMap.w + 0.5) & 2) != 0) {
                    float tpo = max(length(g_dUy), 1e-4);          // PS1 pixels per output pixel
                    float sh = length(dn * hs) * dome / tpo;      // this fragment's shift, output pixels
                    g_aoCell = clamp(1.0 - sh * 0.5, 0.0, 1.0);
                }
            }
            vec2 P = vec2(float((v_info.x & 15) * 64), float(((v_info.x >> 4) & 1) * 256)) + cuv;
            vec2 q = (P - pc.prevRect.xy) / pc.prevRect.zw;
            if (q.x >= 0.0 && q.y >= 0.0 && q.x < 1.0 && q.y < 1.0) {
                if (pc.prevMap.z > 1.5) {
                    // 21.09.2026: SCENE CAPTURE (gpu_vk_raster.cpp Scap) - the game copied a piece of THIS frame into a
                    // scratch texture and draws it back warped (the "Third Energy" space distortion). prevTex holds our own
                    // frame as drawn so far; the VRAM copy is the software raster's picture (no room in it - black), so
                    // its zero texels must not punch holes. Bilinear by hand: the sampler is NEAREST.
                    vec2 ts = vec2(textureSize(prevTex, 0));
                    vec2 st = q * pc.prevMap.xy * ts - 0.5;
                    vec2 fr = fract(st);
                    vec2 b0 = (floor(st) + 0.5) / ts, b1 = b0 + 1.0 / ts;
                    vec3 c00 = TEX_0(prevTex, b0).rgb, c10 = TEX_0(prevTex, vec2(b1.x, b0.y)).rgb;
                    vec3 c01 = TEX_0(prevTex, vec2(b0.x, b1.y)).rgb, c11 = TEX_0(prevTex, b1).rgb;
                    t8 = mix(mix(c00, c10, fr.x), mix(c01, c11, fr.x), fr.y) * 255.0;
                    texel_blend = true;               // the frame is drawn with the mask bit set: a semi prim blends whole
                    if ((int(pc.prevMap.w + 0.5) & 1) != 0) {   // 01.10: no tint at all - only the transparent displacement
                        t8 = t8 / max(g_col / 128.0, vec3(0.01));
                    } else if (pc.prevMap.z > 2.5) {  // smooth cell: the cell's tint fades out with the dome (no bright box)
                        vec3 m = max(g_col / 128.0, vec3(0.01));
                        t8 = t8 * mix(vec3(1.0), m, dome) / m;
                    }
                } else {
                    uint texel = sampleWin(int(g_uv.x), int(g_uv.y), iy, iz, v_info.x);
                    if (texel == 0u) return false;            // transparent in VRAM = transparent here
                    texel_blend = (texel & 0x8000u) != 0u;
                    t8 = TEX_0(prevTex, q * pc.prevMap.xy).rgb * 255.0;
                }
                prev_done = true;
            }
        }
        if (hifont_done || prev_done) {
            // done above
        } else if (tex_filter) {
            // DuckStation "Bilinear": filter AFTER palette decode, masking
            // fully-transparent texels out of the weight sum.
            float fu = g_uv.x - 0.5, fv = g_uv.y - 0.5;
            int u0 = int(floor(fu)), v0 = int(floor(fv));
            float du = fract(fu), dv = fract(fv);
            uint t00 = sampleWin(u0,     v0,     iy, iz, v_info.x);
            uint t10 = sampleWin(u0 + 1, v0,     iy, iz, v_info.x);
            uint t01 = sampleWin(u0,     v0 + 1, iy, iz, v_info.x);
            uint t11 = sampleWin(u0 + 1, v0 + 1, iy, iz, v_info.x);
            float w00 = (1.0 - du) * (1.0 - dv) * (t00 != 0u ? 1.0 : 0.0);
            float w10 = du * (1.0 - dv)         * (t10 != 0u ? 1.0 : 0.0);
            float w01 = (1.0 - du) * dv         * (t01 != 0u ? 1.0 : 0.0);
            float w11 = du * dv                 * (t11 != 0u ? 1.0 : 0.0);
            float ws = w00 + w10 + w01 + w11;
            if (ws < 0.25) return false;              // mostly transparent
            t8 = (t5to8(t00) * w00 + t5to8(t10) * w10 +
                  t5to8(t01) * w01 + t5to8(t11) * w11) / ws;
            // semi bit from the dominant contributor
            uint dom = t00; float wd = w00;
            if (w10 > wd) { dom = t10; wd = w10; }
            if (w01 > wd) { dom = t01; wd = w01; }
            if (w11 > wd) { dom = t11; wd = w11; }
            texel_blend = (dom & 0x8000u) != 0u;
        } else {
            vec2 tuv = texelUV();
            uint texel = sampleWin(int(tuv.x), int(tuv.y), iy, iz, v_info.x);
            if (texel == 0u) return false;            // fully transparent texel
            texel_blend = (texel & 0x8000u) != 0u;
            t8 = t5to8(texel);
        }
        alb8 = t8;
        if ((flags & F_RAW) != 0) {
            rgb8 = t8;
        } else {
            rgb8 = min(t8 * g_col / 128.0, vec3(255.0));
            // DuckStation "true colour": keep the full 8-bit result, no
            // ordered dither — at high internal resolution the dither grid
            // reads as noise instead of smoothing 5-bit banding.
            if (!true_color) {
                int d = dither4[(int(gl_FragCoord.y) & 3) * 4 + (int(gl_FragCoord.x) & 3)];
                rgb8 = clamp(rgb8 + vec3(float(d)), vec3(0.0), vec3(255.0));
            }
        }
    } else {
        // untextured: Gouraud color; whole prim blends when semi
        texel_blend = true;
        rgb8 = g_col;
        alb8 = g_col;
        if ((int(pc.params.w + 0.5) & 1) == 0) {
            int d = dither4[(int(gl_FragCoord.y) & 3) * 4 + (int(gl_FragCoord.x) & 3)];
            rgb8 = clamp(rgb8 + vec3(float(d)), vec3(0.0), vec3(255.0));
        }
    }

    bool semi = (flags & F_SEMI) != 0;
    blend_this = semi && texel_blend;
    // two-pass split: pass 0 draws non-blend texels, pass 1 draws blend texels (01.10 вечер: pass 2 — стекло, все тексели)
    if (pass_blend != 2 && (pass_blend == 0 ? blend_this : !blend_this)) return false;
    return true;
}

void main() {
    int flags = v_info.w;
    int pass_blend = int(pc.params.x + 0.5);
    g_uv = v_uv; g_col = v_color; g_ruv = v_repuv;
#ifdef DC_SAMPLE_LOOP
    g_dRx = dFdx(v_repuv); g_dRy = dFdy(v_repuv);   // производные — здесь, в однородном потоке (в цикле выборок их нет)
    gl_SampleMask[0] = gl_SampleMaskIn[0];          // пути с ранним выходом пишут кадр всем покрытым выборкам
#endif

#ifdef DC_FX_PASS
    // 25.09 вечер: проход отражения — только то, что выше пола (пол и всё под ним не отражается и не заслоняет)
    const int fxMode = int(pc.prevMap.w + 0.5);
    if (fxMode == 2 && v_rfd < 3.0) discard;
    vec3 v_vpt = vec3(0.0);              /* ниже в этом варианте не читаются: выход — сразу после разбора текселя */
    vec3 v_nrm = vec3(0.0, 0.0, -1.0);
    vec3 dVx = vec3(0.0), dVy = vec3(0.0);
    vec2 dUx = vec2(0.0), dUy = vec2(0.0);
#else
    // нормаль поверхности из производных видовой точки (18.09: по ней тень
    // не ложится на грани, отвёрнутые от лампы, и берётся смещение по наклону)
    vec3 v_vpt = shadowViewPoint();      /* кадр B возвращает её в своё пространство */
    vec3 v_nrm = vec3(0.0, 0.0, -1.0);
    // 27.09 вечер: производные точки и UV — здесь, в однородном потоке (рельеф металла ниже; после discard и в ветвлениях
    // по текселю производные не определены)
    const vec3 dVx = dFdx(v_vpt), dVy = dFdy(v_vpt);
    const vec2 dUx = dFdx(v_uv), dUy = dFdy(v_uv);
    {
        vec3 nrm = cross(dVx, dVy);
        float nl = length(nrm);
        if (nl > 1e-9) { nrm /= nl; v_nrm = (dot(nrm, v_vpt) > 0.0) ? -nrm : nrm; }
    }

    // Wireframe overlay (DC_WIREFRAME): flat green edges over the finished
    // image. Everything below is skipped - no texturing, no discards - so an
    // edge always shows, including the polygons that texture out to nothing.
    if ((int(pc.params.w + 0.5) & 32) != 0) {
        // vertex colour, not hardcoded green (2026-08-30): the overlay now
        // carries meaning - red enemy boxes, a green targeted one, a yellow
        // aim ray - and painting them all green erased it.
        out_color = vec4(clamp(v_color.rgb / 255.0, 0.0, 1.0), aoMaskOut());
        return;
    }

    // SHORT-BOOM BODY FADE (params.y, 1 = solid; params.z = cell size). When
    // the camera is pushed in close, the character blocks the view - the RE
    // remakes dissolve her there, this is that dissolve. There is no alpha to
    // dissolve WITH (PS1 semi-transparency is fixed halves/quarters and only
    // acts on bit-15 texels, which none of hers are), so it is a per-fragment
    // discard on an ordered 4x4 grid in SCREEN space: identical holes in her
    // front and back faces, so the room shows through them rather than her
    // own insides. 16 levels + the continuous ramp from the solver = smooth.
    if (pc.params.y < 0.999 && (flags & F_ACTOR) != 0) {
        int sc = int(max(pc.params.z, 1.0));
        ivec2 g = ivec2(gl_FragCoord.xy) / sc;
        float thr = (float(bayer4[(g.y & 3) * 4 + (g.x & 3)]) + 0.5) / 16.0;
        if (pc.params.y <= thr) discard;
    }

    // DC_DBG_PRIM: 1 = mosaic per prim identity; 2 = characters green / world
    // black; 3 = per-primitive colour, actor only; 4 = like 2 but the REAL
    // texture path runs first, so every discard the normal renderer performs
    // (transparent texel, bilinear coverage) shows up as a hole in the green
    // silhouette. Mode 2 answers "did the polygon rasterize", mode 4 answers
    // "did the polygon SURVIVE texturing" — the difference is the texture-side
    // dropout that grows as polygons minify with distance.
    int dbg = (int(pc.params.w + 0.5) >> 2) & 7;
    {
        if (dbg != 0 && dbg != 4 && (flags & F_TWIN) == 0) {     /* 28.09: режим 4 — после выборки текстуры (ниже) */
            if (pass_blend == 1) discard;        // single opaque pass
            if (dbg == 2) {
                // Actor green, other GEOMETRY dark blue, background stays
                // black — a black pixel INSIDE the body is a real hole
                // (nothing rasterized there), blue is just equipment/world.
                // Backface actor prims render DARK green: culling them
                // (tried) turned every silhouette-edge prim whose winding
                // breathes through zero into a fake blinking hole, while
                // bright green would hide real holes behind the backfaces.
                // Dark green = "inner surface here", still clearly not a
                // hole and clearly not the front mesh.
                if ((flags & 32) != 0) {
                    out_color = ((flags & 16) != 0) ? vec4(0.0, 0.35, 0.0, aoMaskOut())
                                                    : vec4(0.0, 0.0, 0.45, aoMaskOut());
                    return;
                }
                out_color = ((flags & 16) != 0) ? vec4(0.0, 1.0, 0.0, aoMaskOut())
                                                : vec4(0.0, 0.0, 0.45, aoMaskOut());
                return;
            }
            // dbg 3 = per-PRIMITIVE identity colour, ACTOR only, world black:
            // every body polygon keeps one stable colour frame to frame, so a
            // polygon that blinks, swaps with its neighbour, or loses the
            // depth test shows up as a colour change — invisible in the flat
            // green view where the whole body is one colour.
            uint h = (uint(v_uvp.x) * 73856093u) ^ (uint(v_uvp.y) * 19349663u) ^
                     (uint(v_info.x) * 83492791u) ^ (uint(v_info.z) * 2654435761u);
            vec3 idc = vec3(float((h >> 16) & 255u),
                            float((h >> 8) & 255u),
                            float(h & 255u)) / 255.0;
            out_color = vec4(0.25 + 0.75 * idc, aoMaskOut());
            return;
        }
    }
#endif

    vec3 rgb8;
    vec3 alb8 = vec3(0.0);       // 25.09: «альбедо» для вспышки — цвет текселя (или вершины у нетекстурированных)
    bool blend_this = false;
    g_dUx = dUx; g_dUy = dUy;
#ifdef DC_SAMPLE_LOOP
    {   // 09.10: тексель в точке каждой покрытой выборки; прозрачные и чужого прохода снимаются маской, свет ниже — один раз
        // по среднему цвету оставшихся (свет линеен по цвету текселя: тень — множитель, луна, вспышки, свет — прибавки)
        vec3 rAcc = vec3(0.0), aAcc = vec3(0.0);
        int nKeep = 0, keep = 0;
        const int cov = gl_SampleMaskIn[0];
        for (int s = 0; s < 8; s++) {
            if ((cov & (1 << s)) == 0) continue;
            g_uv = interpolateAtSample(v_uv, s);
            g_col = interpolateAtSample(v_color, s);
            g_ruv = interpolateAtSample(v_repuv, s);
            vec3 r, a; bool bt;
            if (!texelPath(flags, pass_blend, r, a, bt)) continue;
            rAcc += r; aAcc += a; nKeep++; keep |= 1 << s;
            blend_this = bt;
        }
        if (nKeep == 0) discard;
        const float inv = 1.0 / float(nKeep);
        rgb8 = rAcc * inv; alb8 = aAcc * inv;
        gl_SampleMask[0] = keep;
        g_uv = v_uv; g_col = v_color; g_ruv = v_repuv;   // дальше (рельеф металла) — точка пикселя
    }
#else
    if (!texelPath(flags, pass_blend, rgb8, alb8, blend_this)) discard;
#endif
#ifndef DC_FX_PASS
    // 28.09.2026: DC_DBG_PRIM=4 — как 2 (Регина зелёная, прочая геометрия синяя, фон чёрный), но ПОСЛЕ настоящей выборки
    // текстуры: пиксель, который обычная отрисовка выбрасывает (прозрачный тексель), остаётся дырой в силуэте.
    if (((int(pc.params.w + 0.5) >> 2) & 7) == 4 && (flags & F_TWIN) == 0) {
        if (pass_blend == 1) discard;
        out_color = ((flags & 16) != 0) ? vec4(0.0, 1.0, 0.0, aoMaskOut()) : vec4(0.0, 0.0, 0.45, aoMaskOut());
        return;
    }
#endif

#ifdef DC_FX_PASS
    // 25.09 вечер: вывод проходов свечения и отражения. Сложение (pass 1 — полупрозрачные «сложение»: лазеры, ореолы,
    // вспышки) — свой цвет × вклад (prevRect.x), альфа не трогается. Непрозрачное: свечение — текстура «светится» (бит 23
    // страницы с 25.09 ночи; биты 21..22 — материал пола) своим цветом ярче порога (prevRect.z) × вклад (prevRect.y), прочее —
    // чёрный заслон; отражение — персонажи и «светится» своим цветом (альфа 1), прочее — заслон «отражать нечего» (0).
    {
        const bool texOK = (flags & F_TEXTURED) != 0 && (flags & F_SUBST) == 0;
        const bool glowTex = texOK && ((v_info.x >> 23) & 1) != 0;
        const bool charP = (flags & (F_ACTOR | F_CHAR)) != 0;
        if (pass_blend == 1) {
            // свечение: только ядро ярче порога (prevRect.w) — лазеры и ореолы; тусклый большой конус света не светится.
            // 25.09 ночь (игрок, слот 13: «на текстуру лазеров свечение почему-то не действует»): полупрозрачное из списка
            // «светится» — без порога яркости (слой лазера — белый тексель × цвет вершины 20,0,0, ядро 0.15 < порога 0.55)
            // и со своим вкладом prevMap.x (glow_mark).
            float k = pc.prevRect.x;
            if (fxMode == 1) {
                if (glowTex) k = pc.prevMap.x;
                else k *= smoothstep(pc.prevRect.w, pc.prevRect.w + 0.3, max(max(rgb8.r, rgb8.g), rgb8.b) / 255.0);
                // 25.09 ночь: нетекстурное «сложение» (жёлтое стекло смотрового окна ST303 — теперь квадом в 3D, заливки)
                // не источник света — не светится; лазеры, огоньки и вспышки игры текстурные
                if ((flags & F_TEXTURED) == 0) k = 0.0;
            }
            out_color = vec4(rgb8 / 255.0 * k, 0.0);
            return;
        }
        if (fxMode == 1) {
            vec3 e = vec3(0.0);
            if (glowTex) {
                float lum = max(max(rgb8.r, rgb8.g), rgb8.b) / 255.0;
                e = rgb8 / 255.0 * smoothstep(pc.prevRect.z, pc.prevRect.z + 0.25, lum) * pc.prevRect.y;
            }
            // 25.09 ночь (3), игрок: «эмиссив уменьши в 3 раза, а свет от светящихся текстур верни как было»: альфа — метка
            // «здесь сама светящаяся текстура»; размытие несёт её до G1, сложение с кадром (dc_glow_comp.frag) кладёт на неё
            // только долю ореола glow_self — лампа не выбеливается, вокруг ореол полной силой
            out_color = vec4(e, (glowTex && pc.prevRect.y > 0.0) ? 1.0 : 0.0);
        } else {
            out_color = (charP || glowTex) ? vec4(rgb8 / 255.0, 1.0) : vec4(0.0);
        }
        return;
    }
#endif

    // ===== ШЕРОХОВАТЫЙ МЕТАЛЛ (27.09 вечер, игрок, слот 3: «материал металла как будто пластик а не металл. нет
    // шероховатости, нет матовости») ===== Блик, отблеск окружения и отражение пола считались по гладкой нормали грани —
    // весь металл блестел ровно, как пластмасса. Теперь у металла (материал 2, выбор в приложении):
    //  * РЕЛЬЕФ ИЗ САМОЙ ТЕКСТУРЫ: яркость текселя — высота, нормаль наклоняется по разности соседей (u±1, v±1) в базисе
    //    текстуры (T, B из производных видовой точки и UV) — блик и отблеск ломаются по рисунку рифлёнки, царапин, заклёпок;
    //  * МАТОВОСТЬ ПО ТЕКСЕЛЮ: ржавчина (рыжий оттенок) и грязь (тёмное) почти не блестят и не отражают, чистый металл — как
    //    был; зерно — разброс блеска по текселям (привязан к текселю, не плывёт);
    //  * ШЕРОХОВАТОСТЬ: блик шире и мягче, отблеск и отражение на полу слабее и мутнее, отражение дрожит по рельефу.
    // mrg (UBO 436..439): x шероховатость, y сила рельефа, z матовость ржавчины/грязи, w зерно; все 0 — как было
    // (DC_METAL_ROUGH=0 / галка F8 «металл шероховатый»).
    vec3 mN = v_nrm;          // нормаль с рельефом (у металла; прочим — нормаль грани)
    float mShine = 1.0;       // блеск текселя: 1 — чистый металл, меньше — ржавчина, грязь
    float mRough = 0.0;       // шероховатость (0 — как было)
#ifndef DC_FX_PASS
    if (!blend_this && v_vpt.z > 1.0 && (flags & (F_ACTOR | F_CHAR)) == 0 && (flags & F_TEXTURED) != 0 &&
        (flags & (F_SUBST | F_DITHER | F_TWIN)) == 0 && texMat(flags, v_info.x) == 2 &&
        (sh.mrg.x > 0.0 || sh.mrg.y > 0.0 || sh.mrg.z > 0.0 || sh.mrg.w > 0.0)) {
        const vec2 tuv = texelUV();
        const int tu = int(floor(tuv.x)), tv = int(floor(tuv.y));
        const int iy = v_info.y, iz = v_info.z;
        const vec3 LW = vec3(0.299, 0.587, 0.114) / 255.0;
        if (sh.mrg.y > 0.0) {
            const float hC = dot(alb8, LW);
            const uint n0 = sampleWin(tu - 1, tv, iy, iz, v_info.x), n1 = sampleWin(tu + 1, tv, iy, iz, v_info.x);
            const uint n2 = sampleWin(tu, tv - 1, iy, iz, v_info.x), n3 = sampleWin(tu, tv + 1, iy, iz, v_info.x);
            const float hL = (n0 != 0u) ? dot(t5to8(n0), LW) : hC, hR = (n1 != 0u) ? dot(t5to8(n1), LW) : hC;
            const float hU = (n2 != 0u) ? dot(t5to8(n2), LW) : hC, hD = (n3 != 0u) ? dot(t5to8(n3), LW) : hC;
            // базис текстуры без касательных вершин (производные точки и UV): T — куда растёт u, B — куда растёт v
            const vec3 c2 = cross(dVy, v_nrm), c1 = cross(v_nrm, dVx);
            vec3 T = c2 * dUx.x + c1 * dUy.x, B = c2 * dUx.y + c1 * dUy.y;
            const float tb = max(dot(T, T), dot(B, B));
            if (tb > 1e-24) {
                const float im = inversesqrt(tb);
                const float s = 1.5 * sh.mrg.y;                    // перепад яркости 0.2 между соседями — наклон ~17°
                vec3 n = normalize(v_nrm - (T * ((hR - hL) * s) + B * ((hD - hU) * s)) * im);
                if (dot(n, normalize(-v_vpt)) > 0.05) mN = n;       // рельеф не разворачивает грань от камеры
            }
        }
        const vec3 a = alb8 / 255.0;
        const float amx = max(max(a.r, a.g), a.b), amn = min(min(a.r, a.g), a.b);
        const float sat = (amx - amn) / max(amx, 1e-3);
        const float rust = clamp((a.r - max(a.g, a.b)) * 5.0, 0.0, 1.0) * smoothstep(0.12, 0.35, sat);   // рыжее — ржавчина
        const float grime = 1.0 - smoothstep(0.05, 0.28, dot(a, vec3(0.299, 0.587, 0.114)));             // тёмное — грязь
        const uint hh = (uint(tu & 255) * 73856093u) ^ (uint(tv & 255) * 19349663u) ^ (uint(v_info.x & 0x1FF) * 83492791u);
        const float grain = float((hh >> 7) & 1023u) / 1023.0;
        mShine = max((1.0 - clamp(sh.mrg.z, 0.0, 1.0) * max(rust, 0.7 * grime)) * (1.0 + sh.mrg.w * (grain - 0.5)), 0.0);
        mRough = clamp(sh.mrg.x, 0.0, 1.0);
    }
#endif

    // ===== ТЕНЬ ОТ ЛАМПЫ ИГРЫ (18.09.2026) =============================
    // Свет комнаты запечён в вершинах, поэтому лампа ничего не добавляет: она
    // только ОТНИМАЕТ там, где подвижное перекрыло путь до неё. Карта глубины
    // снята из точки лампы по треугольникам персонажа.
    // Ламп может быть несколько: их тени УМНОЖАЮТСЯ, поэтому там, где тени
    // пересекаются, темнее, чем под одной. 27.09 (2): новый путь (spf.x) —
    // тени без зерна, лампы складываются по доле света (см. «ДОЛЯ СВЕТА»).
    float litMoon = 1.0;                  // 25.09 вечер: видна ли отсюда луна (её слой карты) — для лунного света ниже
    if (sh.cfg.x > 0.5) {
        int nL = min(int(sh.cfg.x + 0.5), 4);
        int dbg = int(sh.cfg.w + 0.5);
        vec3 vpt = v_vpt;
        if (vpt.z > 1.0) {
            // угол поворота выборки — от координат пикселя: узор не крутится
            // между кадрами (иначе полутень бы кипела), но кольца не видно
            float ang = fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
            vec2 rot = vec2(cos(ang), sin(ang));
            if (dbg == 2) {
                int cov = 0;                       // 0 вне поля ламп, 1 лицом, 2 отвёрнут
                for (int i = 0; i < nL; i++) { int p = shadowProbe(i, vpt, v_nrm); if (p == 1) { cov = 1; break; } if (p == 2) cov = 2; }
                rgb8 = (cov == 1) ? vec3(40.0, 200.0, 40.0)
                     : (cov == 2) ? vec3(200.0, 40.0, 40.0)
                                  : vec3(200.0, 200.0, 40.0);
            } else {
                // ТЕНЬ НА САМОМ ПЕРСОНАЖЕ (18.09 вечер). Модель низкополигональная,
                // нормали здесь берутся из производных, и её собственные грани
                // ловили тень пятнами — игрок: «рандомно полигоны резко затенены».
                // По умолчанию персонаж свою тень не получает (cfg2.x = 0);
                // DC_SHADOW_SELF=1 возвращает её с увеличенным смещением.
                bool actor = (flags & (F_ACTOR | F_CHAR)) != 0;
                float shade = 1.0;
                if (!actor || sh.cfg2.x > 0.5) {
                    float bk = actor ? sh.cfg2.y : 1.0;
                    int mL = (sh.mdir.w > -0.5) ? int(sh.mdir.w + 0.5) : -1;
                    if (sh.spf.x > 0.5) {
                        // 27.09 (2): ДОЛЯ СВЕТА. Тень лампы отнимает только то, что эта лампа сюда приносит. Свет в точке —
                        // общий свет комнаты (spf.y) + лампы рядом (со своей тенью и без неё): тень одной лампы из нескольких
                        // светлее, где заслонены все — темнее всего. Прежде тени умножались, и каждая была тёмной, как одна.
                        const float wr = actor ? 0.5 : sh.sps.y;
                        float Et = sh.spf.y;
                        float Ei[4] = float[4](0.0, 0.0, 0.0, 0.0);
                        for (int i = 0; i < nL; i++) {
                            if (i == mL || sh.par[i].x <= 0.0) continue;
                            Ei[i] = lampE(sh.lpos[i].xyz, sh.lpos[i].w, sh.spw[i], vpt, v_nrm, wr);
                            Et += Ei[i];
                        }
                        const int nlo = int(sh.sps.x + 0.5);
                        for (int j = 0; j < 8; j++) {
                            if (j >= nlo) break;
                            Et += lampE(sh.slp[j].xyz, sh.slp[j].w, sh.slw[j >> 2][j & 3], vpt, v_nrm, wr);
                        }
                        float lost = 0.0;
                        for (int i = 0; i < nL; i++) {
                            if (i == mL) continue;
                            const float share = sh.par[i].x * Ei[i] / max(Et, 1e-4);
                            if (share < 0.003) continue;
                            lost += share * shadowOccPF(i, vpt, bk);
                        }
                        shade = clamp(1.0 - lost, 0.0, 1.0);
                        if (mL >= 0 && mL < nL) {          // луна — как была: свой слабый свет и своя гладкая тень
                            const float li = (sh.mpf.x > 0.5) ? moonLitPF(mL, vpt, v_nrm) : shadowLit(mL, vpt, v_nrm, rot, bk);
                            litMoon = li;
                            shade *= 1.0 - sh.par[mL].x * (1.0 - li);
                        }
                    } else
                    for (int i = 0; i < nL; i++) {
                        float li = (i == mL && sh.mpf.x > 0.5) ? moonLitPF(i, vpt, v_nrm)   // 25.09 ночь: луна без шума
                                                               : shadowLit(i, vpt, v_nrm, rot, bk);
                        if (i == mL) litMoon = li;
                        shade *= 1.0 - sh.par[i].x * (1.0 - li);
                    }
                }
                if (dbg == 7) {
                    // кто помечен как персонаж: зелёный — Регина, красный — прочие
                    if ((flags & F_ACTOR) != 0)      rgb8 = vec3(40.0, 220.0, 40.0);
                    else if ((flags & F_CHAR) != 0)  rgb8 = vec3(230.0, 40.0, 40.0);
                } else if (dbg == 6) {
                    // восстановленная видовая точка: канал = 128 + координата/40
                    rgb8 = clamp(vec3(128.0) + vpt / 40.0, vec3(0.0), vec3(255.0));
                } else if (dbg == 5) {
                    // куда фрагмент попадает в карте (R,G) и стоит ли там кто-то (синий)
                    vec4 lp = sh.vp[0] * vec4(vpt, 1.0);
                    vec3 nd = lp.xyz / max(lp.w, 1e-6);
                    vec2 suv = nd.xy * 0.5 + 0.5;
                    bool inside = suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0;
                    float d = inside ? texture(shadowMap, vec3(suv, 0.0)).r : 1.0;
                    rgb8 = vec3(255.0 * clamp(suv.x, 0.0, 1.0),
                                255.0 * clamp(suv.y, 0.0, 1.0),
                                (d < 0.9999) ? 255.0 : 0.0);
                } else if (dbg == 4) {
                    rgb8 = vec3(255.0 * shade);     // чистый множитель тени
                } else if (dbg == 3) {
                    // сама карта слоя 0 во весь экран: белое — что в неё попало
                    vec2 m = gl_FragCoord.xy / 1080.0;
                    rgb8 = (m.x < 1.0)
                         ? vec3(255.0 * (1.0 - texture(shadowMap, vec3(m, 0.0)).r))
                         : rgb8;
                } else if (dbg == 1) {
                    vec4 lp = sh.vp[0] * vec4(vpt, 1.0);
                    vec3 nd = (lp.w > 0.0) ? lp.xyz / lp.w : vec3(-1.0);
                    vec2 suv = nd.xy * 0.5 + 0.5;
                    float sd0 = (suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0)
                              ? texture(shadowMap, vec3(suv, 0.0)).r : 1.0;
                    rgb8 = vec3(255.0 * (1.0 - sd0), 255.0 * clamp(nd.z, 0.0, 1.0), 0.0);
                } else if (texMat(flags, v_info.x) != 7) {
                    rgb8 *= shade;          // 29.09 поздно: экран светится сам — тень лампы его не гасит
                }
            }
        } else if (dbg == 2) {
            rgb8 = vec3(40.0, 40.0, 220.0);        // глубины нет — тень сюда лечь не может
        }
    }

    // ===== ЛУННЫЙ СВЕТ (25.09 вечер, игрок: «сделай во всех 13 комнатах слабый директион свет») =====
    // Направленный источник на бесконечности (pt_renderer.c, луна в комнатах с небом): поверхность, обращённая к луне,
    // получает свой цвет × слабый голубоватый свет по нормали из производных; где луну заслонил персонаж (её слой карты
    // теней), света нет — отсюда и лёгкая тень. Только то, у чего есть глубина, и не смешиваемые тексели.
    if (sh.mdir.w > -0.5 && sh.cfg.x > 0.5 && v_vpt.z > 1.0 && !blend_this && int(sh.cfg.w + 0.5) == 0) {
        // 27.09 вечер (игрок: «грани четкие видны вблизи - из-за включенных теней наших»): нормаль из производных — плоская
        // нормаль полигона, и на персонаже луна ложилась на каждый полигон своей ровной долей — лицо и руки гранились. Теперь
        // (mcol.w ≥ 0 — DC_MOON_CHR_SMOOTH=1) персонаж берёт среднюю нормаль видимой стороны (половина луча к камере): свет луны
        // на нём ровный и плавно зависит только от того, с какой стороны мы смотрим. mcol.w < 0 — по граням, как было
        // (обтекание = −1 − mcol.w).
        const bool chrL = (flags & (F_ACTOR | F_CHAR)) != 0;
        const bool mSm = sh.mcol.w >= 0.0;
        float mndl = (chrL && mSm) ? 0.5 * dot(-normalize(v_vpt), sh.mdir.xyz) : dot(v_nrm, sh.mdir.xyz);
        float mwr = mSm ? sh.mcol.w : (-1.0 - sh.mcol.w);
        if (chrL) mwr = max(mwr, 0.5);   // персонажи: грани грубые, свет их обтекает
        float mlam = clamp((mndl + mwr) / (1.0 + mwr), 0.0, 1.0);
        rgb8 = min(rgb8 + alb8 * sh.mcol.rgb * (mlam * litMoon), vec3(255.0));
    }

    // ===== ВСПЫШКА ВЫСТРЕЛА / ВЗРЫВА КАК СВЕТ (25.09.2026) =====
    // Точечный источник на кадр-два (pt_renderer.c dc_flash_frame): поверхность получает свой цвет × свет со спадом
    // (1 - d/R)^2 и по нормали из производных — грань, отвёрнутая от вспышки, почти не светится. Складывается ПОСЛЕ тени
    // лампы (это другой источник). Только то, у чего есть глубина (не интерфейс и не текст), и не смешиваемые тексели.
    if (sh.flp.w > 0.0 && v_vpt.z > 1.0 && !blend_this) {
        vec3 toF = sh.flp.xyz - v_vpt;
        float fd = length(toF);
        if (fd < sh.flp.w) {
            float fa = 1.0 - fd / sh.flp.w;
            fa *= fa;
            // 28.09: персонаж (flc.w ≥ 0) — средняя нормаль видимой стороны, как у луны: без граней полигонов
            const bool fChr = (flags & (F_ACTOR | F_CHAR)) != 0, fSm = sh.flc.w >= 0.0;
            float ndl = (fd <= 1e-3) ? 1.0 : (fChr && fSm) ? 0.5 * dot(-normalize(v_vpt), toF / fd)
                                                          : dot(mN, toF / fd);   /* 27.09 вечер: у металла — с рельефом */
            float wr = fSm ? sh.flc.w : (-1.0 - sh.flc.w);
            if (fChr) wr = max(wr, 0.5);   // персонажи: грани грубые, свет их обтекает
            float lam = clamp((ndl + wr) / (1.0 + wr), 0.0, 1.0);
            rgb8 = min(rgb8 + alb8 * sh.flc.rgb * (fa * lam), vec3(255.0));
        }
    }

    // ===== СВЕТ ОТ СВЕТЯЩИХСЯ ТЕКСТУР (25.09 поздно, игрок: «от светящихся текстур тоже должен быть эффект — реальный свет,
    // настраиваемый, того цвета, какого текстура») ===== До 8 точечных источников (gpu_vk_raster.cpp gl_lights_build: группы
    // треугольников «светится», цвет самой текстуры): альбедо × цвет × (1 − d/R)² × косинус, как у вспышки. Сама
    // светящаяся текстура своим светом не подсвечивается. Только то, у чего есть глубина, и не смешиваемые тексели.
    {
        const int ngl = int(sh.spare.x + 0.5) + int(sh.spare.w + 0.5);   /* 19.10: и источники 9..16 (glp2/glc2) */
        const bool glowSelf = (flags & F_TEXTURED) != 0 && (flags & F_SUBST) == 0 && ((v_info.x >> 23) & 1) != 0;
        if (ngl > 0 && v_vpt.z > 1.0 && !blend_this && !glowSelf) {
            const bool chr = (flags & (F_ACTOR | F_CHAR)) != 0;
            for (int i = 0; i < 16; i++) {
                if (i >= ngl) break;
                const vec4 gP = (i < 8) ? sh.glp[min(i, 7)] : sh.glp2[max(i - 8, 0)];
                const vec4 gC = (i < 8) ? sh.glc[min(i, 7)] : sh.glc2[max(i - 8, 0)];
                vec3 toL = gP.xyz - v_vpt;
                float d = length(toL);
                if (d >= gP.w) continue;
                float fa = 1.0 - d / gP.w;
                fa *= fa;
                // 28.09 (игрок, слот 12: «от дымки света такое же затенение граней, как было от теней»): у персонажа
                // (glc.w ≥ 0) — средняя нормаль видимой стороны (половина луча к камере), как у луны: плоская нормаль
                // полигона гранила руки, спину, волосы. glc.w < 0 — по граням, как было (обтекание −1 − w)
                const bool gSm = gC.w >= 0.0;
                float ndl = (d <= 1e-3) ? 1.0 : (chr && gSm) ? 0.5 * dot(-normalize(v_vpt), toL / d)
                                                              : dot(mN, toL / d);   /* 27.09 вечер: у металла — с рельефом */
                float gw = gSm ? gC.w : (-1.0 - gC.w);
                float wr = chr ? max(gw, 0.5) : gw;
                float lam = clamp((ndl + wr) / (1.0 + wr), 0.0, 1.0);
                rgb8 = min(rgb8 + alb8 * gC.rgb * (fa * lam), vec3(255.0));
            }
        }
    }

#ifndef DC_FX_PASS
    // ===== ОТРАЖЕНИЕ НА ПОЛУ (25.09 вечер, игрок: «на кафеле и металле отражались бы лазеры, лампы и Регина») =====
    // Пол из списка «блестит» (бит 21 страницы, выбор в приложении; rfc.w = 2 — любой пол, проверка) берёт картинку
    // прохода отражения в своей точке экрана: персонажи там со своим цветом и альфой 1 (подмешиваются), светящееся —
    // сложением (альфа 0). Только грань, смотрящая вверх, на уровне плоскости пола (± допуск rfc.z) — ступени, столы и стены
    // не зеркалят; к краю допуска и к прямому взгляду сверху отражение слабее.
    // 25.09 ночь (игрок: «сделай настройку понятнее и чтобы указывать мог что есть что») — МАТЕРИАЛ пола, биты 21..22
    // страницы (выбор в приложении): 1 кафель — неярко и чётко, 2 металл — мутно и в цвет самого металла, 3 глянец (мокрый
    // пол) — сильно и почти зеркально. Сила и размытие у каждого свои (rfm0/rfm1). rfc.w = 2 — любой пол как кафель.
    if (sh.rfc.w > 0.5 && v_vpt.z > 1.0 && !blend_this && (flags & (F_ACTOR | F_CHAR)) == 0) {
        // 29.09 поздно: жидкость (кровь) отражает как глянец; бумага, бетон и экран пол не зеркалят
        int mat = texMat(flags, v_info.x);
        mat = (mat == 7) ? 0 : texMatShade(mat);
        if (mat == 0 && sh.rfc.w > 1.5) mat = 1;
        float hgt = abs(dot(v_vpt, sh.rfp.xyz) - sh.rfp.w);
        if (mat != 0 && dot(v_nrm, sh.rfp.xyz) > 0.85 && hgt < sh.rfc.z) {
            float mstr  = (mat == 1) ? sh.rfm0.x : (mat == 2) ? sh.rfm0.z : sh.rfm1.x;
            float mblur = (mat == 1) ? sh.rfm0.y : (mat == 2) ? sh.rfm0.w : sh.rfm1.y;
            vec2 rs = vec2(textureSize(reflTex, 0));
            vec2 ruv = gl_FragCoord.xy * 0.5 / rs;
            if (mat == 2) {                   // 27.09 вечер: шероховатый металл — мутнее, слабее, дрожит по рельефу
                mblur *= 1.0 + 2.5 * mRough;
                mstr *= mShine * (1.0 - 0.5 * mRough);
                ruv += (mN.xy - v_nrm.xy) * (16.0 * sh.mrg.y) / rs;
            }
            vec4 r = texture(reflTex, ruv);
            if (mblur > 0.05) {
                // два шестиугольника (половина и полный радиус, второй повёрнут на 30°) — без «двойников» креста
                vec2 o1 = vec2(mblur * 0.5) / rs, o2 = vec2(mblur) / rs;
                r *= 0.2;
                for (int j = 0; j < 6; j++) {
                    float a = float(j) * 1.0471976;
                    r += texture(reflTex, ruv + vec2(cos(a), sin(a)) * o1) * (0.8 / 12.0)
                       + texture(reflTex, ruv + vec2(cos(a + 0.5235988), sin(a + 0.5235988)) * o2) * (0.8 / 12.0);
                }
            }
            if (mat == 2) {                   // металл: отражение окрашено цветом самого металла
                float mx = max(max(alb8.r, alb8.g), max(alb8.b, 1.0));
                r.rgb *= mix(vec3(1.0), alb8 / mx, clamp(sh.rfm1.z, 0.0, 1.0));
            }
            float cv = clamp(dot(normalize(-v_vpt), sh.rfp.xyz), 0.0, 1.0);
            float k = clamp(mstr * mix(1.0, 0.55, cv) * (1.0 - hgt / sh.rfc.z), 0.0, 1.0);
            rgb8 = min(rgb8 * (1.0 - k * clamp(r.a, 0.0, 1.0)) + r.rgb * 255.0 * k, vec3(255.0));
        }
    }

    // ===== БЛИКИ МАТЕРИАЛОВ (25.09 ночь, игрок, слот 13: «материал металла кажется не работает» — металлом отмечена кодовая
    // панель на стене, а материал давал только отражение на полу) ===== Поверхность с материалом (биты 21..22 страницы) под
    // ЛЮБЫМ углом — стена, предмет, пол — получает блик Блинна-Фонга от ламп игры (spp/spc: таблица ламп комнаты, pt_renderer.c
    // dc_spec_frame) и от света светящихся текстур (glp/glc): металл — широкий блик в свой цвет, кафель — узкий и слабый,
    // глянец — узкий и сильный. Блик идёт вместе с камерой — по нему металл и читается металлом. Только то, у чего есть
    // глубина, не смешиваемые тексели, не персонажи.
    if (sh.spare.y > 0.5 && v_vpt.z > 1.0 && !blend_this && (flags & (F_ACTOR | F_CHAR)) == 0) {
        const int smat = texMatShade(texMat(flags, v_info.x));   // 29.09 поздно: жидкость — глянец, экран — стекло (кафель)
        if (smat != 0) {
            float shin = (smat == 1) ? sh.msp.x : (smat == 2) ? sh.msp.z : sh.msp2.x;
            float kstr = (smat == 1) ? sh.msp.y : (smat == 2) ? sh.msp.w : sh.msp2.y;
            // 27.09 вечер: шероховатый металл — блик шире и мягче (степень ниже), слабее; ржавчина и грязь не блестят;
            // нормаль с рельефом текстуры (у прочих материалов mN = нормаль грани, mShine = 1, mRough = 0)
            const vec3 N = mN;
            if (smat == 2) {
                shin = max(shin * (1.0 - 0.65 * mRough), 2.0);
                kstr *= mShine * (1.0 - 0.3 * mRough);
            }
            vec3 tint = vec3(1.0);
            if (smat == 2) {
                float mx = max(max(alb8.r, alb8.g), max(alb8.b, 1.0));
                tint = mix(vec3(1.0), alb8 / mx, clamp(sh.msp2.z, 0.0, 1.0));
            }
            const vec3 V = normalize(-v_vpt);
            vec3 acc = vec3(0.0);
            const int nsp = int(sh.spare.z + 0.5);
            for (int i = 0; i < 4; i++) {
                if (i >= nsp) break;
                vec3 toL = sh.spp[i].xyz - v_vpt;
                float d = length(toL);
                if (d >= sh.spp[i].w || d < 1e-3) continue;
                vec3 L = toL / d;
                float ndl = dot(N, L);
                if (ndl <= 0.0) continue;
                float s = pow(max(dot(N, normalize(L + V)), 0.0), shin);
                float fa = 1.0 - d / sh.spp[i].w;
                acc += sh.spc[i].rgb * (s * fa * min(1.0, ndl * 4.0));
            }
            const int ngs = int(sh.spare.x + 0.5) + int(sh.spare.w + 0.5);   /* 19.10: и источники 9..16 */
            if (sh.msp2.w > 0.0) {
                for (int i = 0; i < 16; i++) {
                    if (i >= ngs) break;
                    const vec4 gP = (i < 8) ? sh.glp[min(i, 7)] : sh.glp2[max(i - 8, 0)];
                    const vec3 gCol = (i < 8) ? sh.glc[min(i, 7)].rgb : sh.glc2[max(i - 8, 0)].rgb;
                    vec3 toL = gP.xyz - v_vpt;
                    float d = length(toL);
                    if (d >= gP.w || d < 1e-3) continue;
                    vec3 L = toL / d;
                    float ndl = dot(N, L);
                    if (ndl <= 0.0) continue;
                    float s = pow(max(dot(N, normalize(L + V)), 0.0), shin);
                    float fa = 1.0 - d / gP.w;
                    acc += gCol * (s * fa * fa * min(1.0, ndl * 4.0) * sh.msp2.w);
                }
            }
            // отблеск окружения: отражённый луч смотрит к потолку (светлая комната, лампы) — светлее, к полу — нет; под
            // скользящим углом сильнее (Френель). Лампа видна в блике редко (честный блик — только при удачном угле), а по
            // этому отсвету, гуляющему при движении камеры, металл и читается металлом.
            // 25.09 ночь (3), игрок (слот 1, ST10C, стеллажи «металл»): «металл пересвечивает текстуры — сделай его более
            // реалистичным и чтобы не пересвечивал». Было: прибавка до 96 уровней под скользящим углом — поднимала чёрное
            // (22 -> 50) и выбеливала светлое, узор пропадал. Теперь МЕТАЛЛ отражает окружение СВОИМ цветом — усилением
            // собственного цвета (у металла отражение и есть его цвет): немного всегда, сильнее, когда отражённый луч смотрит к
            // потолку, и под скользящим углом; усиление «экраном» — чёрное остаётся чёрным, светлое почти не трогается. Прибавки
            // (блики ламп, отблеск кафеля и глянца) тоже «экраном»: чем светлее тексель, тем меньше прибавка — до белого не
            // доходит. msp3.w > 0.5 — прежняя формула (DC_SPEC_OLD=1).
            const bool oldSpec = sh.msp3.w > 0.5;
            float env = 0.0, envMul = 1.0;
            if (sh.senv.w > 0.5) {
                const vec3 Rr = reflect(-V, N);
                const float h = clamp(dot(Rr, sh.senv.xyz), -1.0, 1.0);          // +1 — отражается потолок, -1 — пол
                float kenv = (smat == 1) ? sh.msp3.x : (smat == 2) ? sh.msp3.y : sh.msp3.z;
                if (smat == 2) kenv *= mShine * (1.0 - 0.3 * mRough);          // 27.09 вечер: ржавчина/грязь, шероховатость
                if (smat == 2 && !oldSpec) {
                    // металл светлеет своим цветом: немного всегда (шероховатый металл ловит свет комнаты под любым углом),
                    // сильнее, когда отражает потолок, и под скользящим углом; «экран» ниже — светлое почти не трогается
                    const float fres = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 3.0);
                    if (sh.rfm1.w > 0.5) {
                        // 27.09 (игрок: «металл допустим слишком светлым становится. Исправь материал»): постоянная доля
                        // (+24% всегда) поднимала весь металл — стеллажи ST10C светлели целиком, тёмное на 45%. Теперь металл
                        // светлеет только там, где отражённый луч смотрит к потолку (верх полок, пол под углом), и немного
                        // под скользящим углом; вертикальный металл при камере сверху отражает пол — остаётся своим цветом
                        envMul = 1.0 + kenv * (0.35 * smoothstep(0.0, 0.9, h) + 0.15 * fres);
                    } else {
                        envMul = 1.0 + kenv * (0.3 + 0.5 * smoothstep(-0.4, 0.9, h) + 0.2 * fres);
                    }
                } else {
                    const float upk = h * 0.5 + 0.5;
                    const float fres = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 3.0);
                    env = kenv * (0.25 + 0.75 * fres) * (0.6 + 0.4 * upk) * 120.0;
                }
            }
            const vec3 add = tint * (acc * (kstr * 255.0) + vec3(env));
            if (oldSpec) {
                rgb8 = min(rgb8 + add, vec3(255.0));
            } else {
                vec3 c = min(rgb8, vec3(255.0));
                c = (envMul >= 1.0) ? c + c * (envMul - 1.0) * (1.0 - c / 255.0) : c * envMul;
                rgb8 = clamp(c + min(add, vec3(255.0)) * (1.0 - c / 255.0), vec3(0.0), vec3(255.0));
            }
        }
    }

    // ===== БЛЕСК ПРЕДМЕТОВ (12.10.2026, игрок: «оверлей поверх предметов, который раз в 5 секунд волной их выделяет, добавляет
    // немножко свечения, поблескивание, как в Resident Evil, чтобы предмет было легче найти»; GAME -> Item Highlight) =====
    // Предметы (секции таблицы типа 2), которые рисует наш обход комнаты, несут в v_glint место точки вдоль диагонали предмета
    // в виде камеры (1 — левый верхний край, 2 — правый нижний; pt_renderer.c rl_glint_*); раз в период по ним проходит полоса
    // света (glint.y — где она, glint.w — полуширина) и общий лёгкий отсвет (glint.z). Сложение «экраном»: светлое не выбеливается.
    if (sh.glint.x > 0.0 && v_glint > 0.5) {
        const float gd = (v_glint - 1.0 - sh.glint.y) / max(sh.glint.w, 1e-3);
        const float gk = sh.glint.x * (exp(-gd * gd) + 0.22 * sh.glint.z);
        const vec3 gc = min(rgb8, vec3(255.0));
        rgb8 = clamp(gc + vec3(255.0, 244.0, 214.0) * gk * (1.0 - 0.6 * gc / 255.0), vec3(0.0), vec3(255.0));
    }

    // ===== ПОДСВЕТКА ТЕКСТУРЫ ИЗ ПРИЛОЖЕНИЯ (29.09 поздно, игрок: «если выделить текстуру в редакторе — отдельным
    // инструментом, именно для пометки, — она будет дебагом мигать. Не ярко, просто чтобы видно было») ===== Бит 19 страницы
    // (gpu.c dc_tex_class, список dc_highlight_user.cfg — его пишет приложение, инструмент «Найти в игре»): мягкое мигание
    // тёплым цветом 1.2 Гц. rfc.x — время (с), rfc.y — сила (DC_HL_TEX, 0 — выкл). У 15-битных страниц биты 9..20 — глиф
    // шрифта, у рабицы (F_SUBST) — окно: им нет.
    if (sh.rfc.y > 0.0 && (flags & F_TEXTURED) != 0 && (flags & F_SUBST) == 0 && ((v_info.x >> 19) & 1) != 0 &&
        ((v_info.x >> 7) & 3) != 2) {
        const float pulse = 0.5 + 0.5 * sin(sh.rfc.x * 7.5398);
        rgb8 = mix(rgb8, vec3(255.0, 222.0, 110.0), clamp(sh.rfc.y, 0.0, 1.0) * pulse);
    }

    // ===== СТЕКЛО (01.10 вечер, игрок, слот 26, ST507: «сделай нормальное стекло. Сейчас эти два элемента будто просто поверх
    // рисуются») ===== Капсула и пробирка в отсеках у игры — непрозрачный синий цилиндр и плоская «средняя» полупрозрачность
    // PS1, которая не зависит ни от угла, ни от света. Свой проход (конвейер 11, params.x = 2, флаг F_GLASS, после всего
    // непрозрачного, с проверкой глубины): прозрачность по Френелю — по центру видно то, что за стеклом, к краям плотнее;
    // цвет — тон текстуры с освещением сцены, блик ключевого света сверху-слева, отсвет окружения на скользящих углах; красные
    // метки капсулы почти непрозрачны. Выход — предумноженная альфа (смешивание S + D·(1 − α)).
    if (pass_blend == 2 && (flags & F_GLASS) != 0) {
        const vec3 tint = clamp(rgb8 / 255.0, 0.0, 1.0);
        const vec3 gN = v_nrm;
        const vec3 gV = (length(v_vpt) > 1e-3) ? normalize(-v_vpt) : vec3(0.0, 0.0, -1.0);
        const float ndv = clamp(abs(dot(gN, gV)), 0.0, 1.0);
        const float fres = pow(1.0 - ndv, 3.0);
        const vec3 al = clamp(alb8 / 255.0, 0.0, 1.0);           // метка — по цвету самой текстуры (материал листа его не трогает)
        const float mx = max(al.r, max(al.g, al.b)), mn = min(al.r, min(al.g, al.b));
        const float label = smoothstep(0.16, 0.34, mx - mn) * step(max(al.g, al.b) * 1.4, al.r);   // красные метки
        float ga = clamp(0.22 + 0.62 * fres, 0.0, 0.88);
        ga = max(ga, 0.92 * label);
        const vec3 gL = normalize(vec3(-0.45, -0.75, -0.48));   // вид: x вправо, y вниз, z вперёд — свет слева сверху, от камеры
        const float spec = pow(max(dot(gN, normalize(gL + gV)), 0.0), 32.0) * 0.55;   // грани плоские: блик мягкий, без белых квадратов
        const vec3 gR = reflect(-gV, gN);
        const float sky = clamp(-gR.y * 0.5 + 0.5, 0.0, 1.0);   // отражённый луч к потолку — светлее
        const vec3 env = mix(vec3(0.05, 0.06, 0.08), vec3(0.62, 0.68, 0.74), sky) * (0.25 + 0.75 * fres);
        const vec3 body = tint * (0.70 + 0.55 * fres);
        const vec3 gc = body * ga + env * 0.45 * (1.0 - label) + vec3(spec) * (1.0 - 0.6 * label);
        out_color = vec4(clamp(gc, 0.0, 1.0), clamp(ga, 0.0, 1.0));
        return;
    }
#endif

    if ((int(pc.params.w + 0.5) & 1) != 0) {
        // true colour: full 8-bit output, no 5-bit quantization
        out_color = vec4(rgb8 / 255.0, aoMaskOut());
    } else {
        // 5-bit truncation like the CPU path, back to normalized
        vec3 c5 = floor(rgb8 / 8.0);
        out_color = vec4(c5 / 31.0, aoMaskOut());
    }
}
