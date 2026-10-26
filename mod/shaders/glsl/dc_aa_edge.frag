#version 460
// 25.10.2026: СГЛАЖИВАНИЕ SMAA и CMAA2, проход 1 из 3 - рёбра (меню GRAPHICS -> Anti-Aliasing; gpu_vk_raster.cpp aa_run).
// Своя запись опубликованных алгоритмов: SMAA 1x (Хименес и др., 2012) - перепад яркости, ребро гасится, если рядом перепад
// вдвое больше; CMAA2 (Стругар, Intel, 2018) - взвешенная разница цвета, из неё вычитается доля самого большого перепада рядом.
// Сборка: без -D - SMAA (dc_aa_smaa_edge.frag.spv), -DDC_CMAA=1 - CMAA2 (dc_aa_cmaa_edge.frag.spv).
// Выход R8G8: r - ребро с левым соседом, g - с верхним. Вход - копия кадра после основного прохода (цвет в гамме, как в цели).
// Интерфейс, текст и плоские экраны (маска в альфе < 0.25, dc_raster.frag aoMaskOut) рёбер не дают: такой пиксель - без рёбер,
// такой сосед считается равным центру.
// 25.10.2026 вечер: цель залита нулём в начале прохода (aa_run), пиксель без рёбер не пишется (discard) - запись только у рёбер, а
// проходы 2 и 3 читают в остальном кадре залитое (дешевле на слабых видеокартах).
layout(binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec4 rt;     // активная область w, h; 1 / ширина и 1 / высота текстуры
    vec4 par;    // не используется
} pc;
layout(location = 0) out vec2 o;

#ifdef DC_CMAA
const float THR = 0.07;             // порог ребра (CMAA2, высокое качество)
const float LCA = 0.10;             // доля самого большого перепада рядом, что вычитается из ребра (CMAA2)
float dif(vec4 a, vec4 b) { return dot(abs(a.rgb - b.rgb), vec3(0.299, 0.587, 0.114)); }
#else
const float THR = 0.1;              // порог ребра (SMAA, высокое качество)
const float LCA = 2.0;              // ребро гасится, если рядом перепад больше в LCA раз (SMAA)
float dif(vec4 a, vec4 b) { return abs(dot(a.rgb - b.rgb, vec3(0.2126, 0.7152, 0.0722))); }
#endif

ivec2 g_lim;
vec4 px(ivec2 q) { return texelFetch(src, clamp(q, ivec2(0), g_lim), 0); }   // за краем - крайний пиксель (перепад 0)

void main() {
    g_lim = ivec2(pc.rt.xy) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = px(p);
    if (c.a < 0.25) discard;
    vec4 cL = px(p + ivec2(-1, 0)), cT = px(p + ivec2(0, -1));
    bool wL = p.x > 0 && cL.a >= 0.25, wT = p.y > 0 && cT.a >= 0.25;
    if (!wL && !wT) discard;
    if (!wL) cL = c;
    if (!wT) cT = c;
    vec2 d = vec2(dif(c, cL), dif(c, cT));
    if (max(d.x, d.y) < THR) discard;
    // самый большой перепад рядом: центр со всеми четырьмя соседями, левый и верхний - со своими следующими
    vec4 cR = px(p + ivec2(1, 0)), cB = px(p + ivec2(0, 1)), cLL = px(p + ivec2(-2, 0)), cTT = px(p + ivec2(0, -2));
    if (cR.a < 0.25) cR = c;
    if (cB.a < 0.25) cB = c;
    if (!wL || cLL.a < 0.25) cLL = cL;
    if (!wT || cTT.a < 0.25) cTT = cT;
    float m = max(max(max(d.x, d.y), max(dif(c, cR), dif(c, cB))), max(dif(cL, cLL), dif(cT, cTT)));
#ifdef DC_CMAA
    vec2 e = step(vec2(THR), d - LCA * m);
#else
    vec2 e = step(vec2(THR), d) * step(vec2(m), LCA * d);
#endif
    if (e.x + e.y < 0.5) discard;
    o = e;
}
