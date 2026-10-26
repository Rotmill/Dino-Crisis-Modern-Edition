#version 460
// 25.10.2026: СГЛАЖИВАНИЕ SMAA и CMAA2, проход 3 из 3 - смешение (gpu_vk_raster.cpp aa_run). Пиксель смешивается с соседом
// через ребро по весам прохода 2, одна билинейная выборка на сторону (как у SMAA). Где весов нет, у CMAA2 (-DDC_CMAA=1) -
// простые формы: пиксель с двумя и больше рёбрами (угол, выступ в пиксель) слегка берёт у соседей через эти рёбра, тем меньше,
// чем больше рёбер вокруг (текст, мелкий узор). Выход - в цель кадра, до SSAO, свечения и дымки.
// Интерфейс и текст (маска в альфе < 0.25) остаются как есть.
// 25.10.2026 вечер: проход загружает цель кадра (там тот же кадр, что в копии), пиксель без смешения не пишется (discard) - запись
// только у рёбер; без маски дальше (альфа = 1) пишется и пиксель, у которого альфа не 1. Сначала читаются веса, цвет - если нужен.
layout(binding = 0) uniform sampler2D src;
layout(binding = 2) uniform sampler2D wgt;
#ifdef DC_CMAA
layout(binding = 1) uniform sampler2D edg;
#endif
layout(push_constant) uniform PC {
    vec4 rt;     // активная область w, h; 1 / ширина и 1 / высота текстуры
    vec4 par;    // x - 1: альфа центра остаётся (маска нужна SSAO и дымке), 0: альфа = 1
} pc;
layout(location = 0) out vec4 o;

ivec2 g_lim;
bool inside(ivec2 q) { return all(greaterThanEqual(q, ivec2(0))) && all(lessThanEqual(q, g_lim)); }
vec4 W(ivec2 q) { return inside(q) ? texelFetch(wgt, q, 0) : vec4(0.0); }
#ifdef DC_CMAA
vec2 E(ivec2 q) { return inside(q) ? texelFetch(edg, q, 0).rg : vec2(0.0); }
vec4 E4(ivec2 q) { return vec4(E(q + ivec2(1, 0)).r, E(q + ivec2(0, 1)).g, E(q).r, E(q).g); }   // справа, снизу, слева, сверху
vec3 px(ivec2 q) { return texelFetch(src, clamp(q, ivec2(0), g_lim), 0).rgb; }
#endif

// пиксель как есть: в цели кадра он уже такой - не пишется; без маски дальше пишется альфа 1, если она у пикселя не 1
void unchanged(vec4 c) {
    if (pc.par.x > 0.5 || c.a > 0.999) discard;
    o = vec4(c.rgb, 1.0);
}

void main() {
    g_lim = ivec2(pc.rt.xy) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    bool keepA = pc.par.x > 0.5;
    vec4 a = vec4(W(p + ivec2(1, 0)).a, W(p + ivec2(0, 1)).g, W(p).b, W(p).r);   // к правому, нижнему, левому, верхнему
    if (dot(a, vec4(1.0)) < 1e-5) {
#ifdef DC_CMAA
        vec4 e = E4(p);
        float n = dot(e, vec4(1.0));
        if (n > 1.5) {                                          // прямое ребро не размываем
            float k = 0.10;
            if (n < 2.5) k *= (1.0 - e.y * e.w) * (1.0 - e.x * e.z) * 0.75;   // два ребра: только угол, не две параллельные
            float around = dot(E4(p + ivec2(-1, 0)).yzw + E4(p + ivec2(1, 0)).xyw + E4(p + ivec2(0, -1)).xzw
                             + E4(p + ivec2(0, 1)).xyz, vec3(1.0));  // рёбра соседей, кроме общих с пикселем
            k *= clamp(1.30 - around / 10.0, 0.0, 1.0);
            if (k > 0.0) {
                vec4 c = texelFetch(src, p, 0);
                vec4 w = e * k;
                vec3 col = c.rgb * (1.0 - dot(w, vec4(1.0))) + w.x * px(p + ivec2(1, 0)) + w.y * px(p + ivec2(0, 1))
                         + w.z * px(p + ivec2(-1, 0)) + w.w * px(p + ivec2(0, -1));
                o = vec4(col, keepA ? c.a : 1.0);
                return;
            }
        }
#endif
        if (keepA) discard;                                     // цвет не нужен
        unchanged(texelFetch(src, p, 0));
        return;
    }
    vec4 c = texelFetch(src, p, 0);
    float aOut = keepA ? c.a : 1.0;
    if (c.a < 0.25) { unchanged(c); return; }
    bool h = max(a.x, a.z) > max(a.y, a.w);                     // смешение вдоль строки (вертикальное ребро) сильнее
    vec2 s1 = h ? vec2(a.x, 0.0) : vec2(0.0, a.y);
    vec2 s2 = h ? vec2(-a.z, 0.0) : vec2(0.0, -a.w);
    vec2 wt = h ? a.xz : a.yw;
    wt /= wt.x + wt.y;
    vec2 uv = (vec2(p) + 0.5) * pc.rt.zw;
    vec3 col = wt.x * textureLod(src, uv + s1 * pc.rt.zw, 0.0).rgb + wt.y * textureLod(src, uv + s2 * pc.rt.zw, 0.0).rgb;
    o = vec4(col, aOut);
}
