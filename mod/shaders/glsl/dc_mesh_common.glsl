// 06.10.2026: МЕШИ ИЗ MODS НА ВИДЕОКАРТЕ (dc_meshrep.cpp → gpu_vk_raster.cpp dcvk_mesh_*). Игрок: «мод не должен тормозить даже
// на сильно устаревших ПК — 60 кадров, как игра PS1». Модель лежит в видеопамяти один раз (вершины позы покоя + индексы); за
// кадр процессор отдаёт только экземпляр: кости (поворот, перенос, сустав покоя модели), свет каждой кости (LLM, LCM — как GTE
// игры), фон и цвет персонажа, перенос нашей камеры. Здесь — то же, что делал процессор (dc_meshrep.cpp transform_inst):
// вершина к костям по весам, свет NCCT по кости с наибольшим весом, итог — видовая точка (x, y, глубина w) той же камеры,
// что у потока примитивов игры; дальше каждый шейдер проецирует её ровно как свой собрат для потока.

layout(location = 0) in vec3  in_p;      // поза покоя модели, координаты игры (мм, Y вниз)
layout(location = 1) in vec3  in_n;
layout(location = 2) in vec2  in_uv;     // место в картинке замены (0..1)
layout(location = 3) in uvec4 in_j;      // кости игры (255 — корень)
layout(location = 4) in vec4  in_wt;     // веса (0..1)
layout(location = 5) in vec4  in_col;    // цвет вершины × цвет материала (0..1)
layout(location = 6) in float in_slot;   // картинка в массиве repTex

// экземпляр: 9 vec4 — [0] ox, oy, H, H·сжатие; [1] множитель глубины, порядок (painter), вид (0 персонаж, 1 группа), свет у
// группы; [2] tpage, clut_x, clut_y, флаги; [3] фон xyz, число костей; [4] цвет персонажа xyz, корень; [5..7] строки D + T
// (перенос нашей камеры); [8] x — первая кость в буфере костей
layout(std430, set = 1, binding = 0) readonly buffer MeshInst { vec4 mi[]; };
// кость: 10 vec4 — [0..2] строки поворота, w — перенос; [3] сустав покоя модели; [4..6] строки LLM; [7..9] строки LCM
layout(std430, set = 1, binding = 1) readonly buffer MeshBone { vec4 mb[]; };

vec3 mb_rot(int o, vec3 v) { return vec3(dot(mb[o].xyz, v), dot(mb[o + 1].xyz, v), dot(mb[o + 2].xyz, v)); }

// видовая точка вершины (до множителя глубины) и её цвет (0..255, как in_color потока); light = false — без света (тени)
void mesh_vertex(bool light, out vec3 p, out vec3 col) {
    const int I = gl_InstanceIndex * 9;
    const vec4 misc = mi[I + 1], bk = mi[I + 3], rg = mi[I + 4];
    const int base = int(mi[I + 8].x + 0.5), nb = int(bk.w + 0.5), root = int(rg.w + 0.5);
    const bool actor = misc.z < 0.5;
    vec3 v = vec3(0.0), n = vec3(0.0);
    int dom = 0;
    if (actor) {
        float best = -1.0, ws = 0.0;
        for (int k = 0; k < 4; k++) {
            const float w = in_wt[k];
            if (w <= 0.0) continue;
            int b = int(in_j[k]);
            if (b >= nb) b = root;
            const int o = (base + b) * 10;
            v += w * (mb_rot(o, in_p - mb[o + 3].xyz) + vec3(mb[o].w, mb[o + 1].w, mb[o + 2].w));
            n += w * mb_rot(o, in_n);
            ws += w;
            if (w > best) { best = w; dom = b; }
        }
        if (ws > 0.0) { v /= ws; n /= ws; }
    } else {
        const int o = base * 10;
        v = mb_rot(o, in_p) + vec3(mb[o].w, mb[o + 1].w, mb[o + 2].w);
        n = mb_rot(o, in_n);
    }
    col = vec3(128.0);
    if (light) {
        const float ln = length(n);
        if (ln > 1e-8) n /= ln;
        if (actor || misc.w > 0.5) {                     // NCCT: IR = LLM·n (≥0), L = фон + LCM·IR, цвет = RGBC·L
            const int o = (base + dom) * 10;
            const vec3 ir = clamp(vec3(dot(mb[o + 4].xyz, n), dot(mb[o + 5].xyz, n), dot(mb[o + 6].xyz, n)), 0.0, 7.999);
            const vec3 L = clamp(bk.xyz + vec3(dot(mb[o + 7].xyz, ir), dot(mb[o + 8].xyz, ir), dot(mb[o + 9].xyz, ir)), 0.0, 7.999);
            col = clamp(rg.xyz * L * in_col.rgb, 0.0, 255.0);
        } else {
            col = 128.0 * in_col.rgb;
        }
    }
    const vec4 d0 = mi[I + 5], d1 = mi[I + 6], d2 = mi[I + 7];   // перенос нашей камеры (без него — единичный)
    p = vec3(dot(d0.xyz, v) + d0.w, dot(d1.xyz, v) + d1.w, dot(d2.xyz, v) + d2.w);
}

// отсечённые координаты x, y без деления на глубину (видеокарта сама режет то, что ушло за камеру): экран = o + H·x/w
vec2 mesh_clip(vec4 pr, vec4 view, vec3 p) {
    return vec2(((pr.x - view.x) / view.z * 2.0 - 1.0) * p.z + 2.0 * pr.w * p.x / view.z,
                ((pr.y - view.y) / view.w * 2.0 - 1.0) * p.z + 2.0 * pr.z * p.y / view.w);
}
