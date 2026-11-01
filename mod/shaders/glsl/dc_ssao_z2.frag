#version 460
// DC GPU raster - ЗАТЕНЕНИЕ УГЛОВ (SSAO), проход 1 из 3 с раздельными целями (29.10.2026, пункт 10 плана скорости).
// То же, что dc_ssao_z.frag, но вместо одной цели RG32F (r = ±w, g = доля затенения) - две: R32F (±w) и R8 (сырая альфа m
// цветной цели). Проходы 2 и 3 и дымка читают глубину на каждой выборке - теперь 4 байта вместо 8. Долю затенения
// g = clamp(m * 2 - 1, 0, 1) считает проход 3 (dc_ssao_comp2.frag) из той же m: R8 хранит альфу RGBA8 без потерь,
// m * 2 - 1 точно (Штербенц), поэтому картинка та же бит в бит. DC_SSAO_ZSPLIT=0 - прежняя цель RG32F.
#ifdef DC_MS
layout(binding = 0) uniform sampler2DMS depthTex;
#else
layout(binding = 0) uniform sampler2D depthTex;
#endif
layout(binding = 1) uniform sampler2D colorTex;
layout(location = 0) out float out_z;
layout(location = 1) out float out_m;

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, ip, 0).r;
    float m = texelFetch(colorTex, ip, 0).a;
    float w = (d > 0.0) ? 128.0 / d : 0.0;
    out_z = (m >= 0.5) ? w : -w;
    out_m = m;
}
