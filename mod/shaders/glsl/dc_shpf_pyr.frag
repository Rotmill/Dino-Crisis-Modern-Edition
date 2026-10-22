#version 460
// 27.09.2026: ТЕНИ БЕЗ ЗЕРНА — шаг 2 (gpu_vk_raster.cpp spf_run): следующий уровень пирамиды из предыдущего (вдвое крупнее).
// Пять билинейных выборок: центр (ровно 2x2 под текселем, вес 4) и четыре угла на тексель источника наружу (вес 1) — фильтр
// 4x4 с мягким краем, уровни выходят гладкими, без квадратов. Каналы: доля заслонённого и доля × расстояние.
layout(binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;   // b.xy — размер источника
layout(location = 0) out vec4 o;
void main() {
    vec2 ts = 1.0 / pc.b.xy;
    vec2 c = gl_FragCoord.xy * 2.0;                  // центр текселя цели в текселях источника (угол между четырьмя)
    vec4 s = texture(src, c * ts) * 4.0;
    s += texture(src, (c + vec2(-1.0, -1.0)) * ts);
    s += texture(src, (c + vec2( 1.0, -1.0)) * ts);
    s += texture(src, (c + vec2(-1.0,  1.0)) * ts);
    s += texture(src, (c + vec2( 1.0,  1.0)) * ts);
    o = s * 0.125;
}
