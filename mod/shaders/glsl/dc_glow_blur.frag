#version 460
// 25.09.2026 вечер: СВЕЧЕНИЕ — размытие по одной оси (gpu_vk_raster.cpp fx_glow_post). 13 выборок по Гауссу (σ = 3 шага),
// шаг — pc.a.xy пикселей источника; pc.a.z = 2 — источник вдвое крупнее цели (заодно уменьшение 2x2 билинейной выборкой).
// Выборки не выходят за активную область источника (pc.b.xy): дальше в образе — чужие кадры другого размера.
layout(binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;
layout(location = 0) out vec4 o;
void main() {
    vec2 ts = vec2(textureSize(src, 0));
    vec2 c = gl_FragCoord.xy * pc.a.z;
    vec3 s = vec3(0.0);
    float ws = 0.0;
    for (int i = -6; i <= 6; i++) {
        float wgt = exp(-float(i * i) / 18.0);
        vec2 q = clamp(c + pc.a.xy * float(i), vec2(0.5), pc.b.xy - 0.5);
        s += texture(src, q / ts).rgb * wgt;
        ws += wgt;
    }
    // 25.09 ночь (3): альфа — метка «сама светящаяся текстура» из прохода свечения, без размытия (тексель под центром):
    // E -> T -> G1 одного размера, в G1 она та же, что в E (dc_glow_comp.frag)
    o = vec4(s / ws, texture(src, clamp(c, vec2(0.5), pc.b.xy - 0.5) / ts).a);
}
