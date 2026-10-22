#version 460
// 25.09.2026 ночь: ЛУНА ГЛАДКОЙ ТЕНЬЮ — шаг 3 (gpu_vk_raster.cpp mpf_run). Размытие по вертикали уже размытого по
// горизонтали (тот же Гаусс, что у dc_glow_blur.frag: 13 выборок, σ = 3 шага, шаг pc.a.y текселей) — в rg, а резкая,
// только ужатая версия (шаг 1) — в ba. Растр (moonLitPF) берёт у ног резкую, к концу тени — размытую.
layout(binding = 0) uniform sampler2D src;           // размытое по горизонтали: r доля, g доля·глубина
layout(binding = 1) uniform sampler2D sharp;         // ужатое без размытия
layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;   // a.y — шаг; b.xy — размер цели
layout(location = 0) out vec4 o;
void main() {
    vec2 ts = vec2(textureSize(src, 0));
    vec2 c = gl_FragCoord.xy;
    vec2 s = vec2(0.0);
    float ws = 0.0;
    for (int i = -6; i <= 6; i++) {
        float wgt = exp(-float(i * i) / 18.0);
        vec2 q = clamp(c + vec2(0.0, pc.a.y * float(i)), vec2(0.5), pc.b.xy - 0.5);
        s += texture(src, q / ts).rg * wgt;
        ws += wgt;
    }
    o = vec4(s / ws, texelFetch(sharp, ivec2(c), 0).rg);
}
