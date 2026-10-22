#version 460
// 25.09.2026 ночь: ЛУНА ГЛАДКОЙ ТЕНЬЮ — шаг 1 (gpu_vk_raster.cpp mpf_run). Слой луны карты теней (D32, в нём только
// персонажи) ужимается в pc.a.x раз по каждой оси: в точке цели — доля текселей, где кто-то заслонил луну, и эта доля ×
// средняя глубина заслонивших (после размытия глубина = g / r). Дальше — размытие по горизонтали (dc_glow_blur.frag) и по
// вертикали с упаковкой резкой версии (dc_moon_pack.frag); растр читает результат с биндинга 6 (moonLitPF).
layout(binding = 0) uniform sampler2D dep;          // один слой карты теней (вид на слой)
layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;   // a.x — во сколько раз ужать
layout(location = 0) out vec4 o;
void main() {
    int k = max(int(pc.a.x + 0.5), 1);
    ivec2 base = ivec2(gl_FragCoord.xy) * k;
    float c = 0.0, s = 0.0;
    for (int y = 0; y < k; y++)
        for (int x = 0; x < k; x++) {
            float d = texelFetch(dep, base + ivec2(x, y), 0).r;
            if (d < 1.0) { c += 1.0; s += d; }
        }
    float n = float(k * k);
    o = vec4(c / n, s / n, 0.0, 1.0);
}
