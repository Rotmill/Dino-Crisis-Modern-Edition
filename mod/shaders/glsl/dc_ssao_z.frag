#version 460
// DC GPU raster — ЗАТЕНЕНИЕ УГЛОВ (SSAO), проход 1 из 3 (25.09.2026): линейная глубина кадра + маска, полное разрешение.
// Глубина растра = 128 / w (dc_raster.vert) -> w, видовая глубина. Знак — маска из альфы цветной цели (dc_raster.frag
// aoMaskOut): +w — точку последней закрасила непрозрачная 3D-геометрия (затенять можно), -w — поверх неё плоское
// (интерфейс, текст), 0 — геометрии нет. Проходы 2 и 3 читают только эту текстуру: с MSAA глубина N выборок читается
// здесь один раз (выборка 0), а не на каждой выборке затенения.
#ifdef DC_MS
layout(binding = 0) uniform sampler2DMS depthTex;
#else
layout(binding = 0) uniform sampler2D depthTex;
#endif
layout(binding = 1) uniform sampler2D colorTex;
layout(location = 0) out vec4 out_z;

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, ip, 0).r;
    float m = texelFetch(colorTex, ip, 0).a;
    float w = (d > 0.0) ? 128.0 / d : 0.0;
    // 01.10.2026: g — доля затенения (цель RG32F): 1 у обычной 3D-точки (альфа 1), 0.5..1 альфы — ячейка «третьей энергии»,
    // которая гасит затенение там, где картинка сдвинута (dc_raster.frag g_aoCell); проход 3 умножает на неё силу
    out_z = vec4((m >= 0.5) ? w : -w, clamp(m * 2.0 - 1.0, 0.0, 1.0), 0.0, 1.0);
}
