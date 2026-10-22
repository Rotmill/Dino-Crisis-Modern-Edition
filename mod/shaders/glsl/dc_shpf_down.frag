#version 460
// 27.09.2026: ТЕНИ БЕЗ ЗЕРНА — шаг 1 (gpu_vk_raster.cpp spf_run). Игрок: «тени выглядят очень шумными и не совсем точными».
// Слой карты теней лампы (D32, в нём только персонажи) ужимается в pc.a.x раз по каждой оси: r — доля текселей, где кто-то
// заслонил лампу, g — эта доля × расстояние «лампа -> заслонивший» (единицы комнаты / 4096; после размытия расстояние = g / r).
// Дальше — пирамида уровней (dc_shpf_pyr.frag), растр берёт её гладкими выборками (dc_raster.frag shadowOccPF).
layout(binding = 0) uniform sampler2D dep;          // один слой карты теней (вид на слой)
layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;   // a.x — во сколько раз ужать, a.y/a.z — A и B (глубина -> расстояние)
layout(location = 0) out vec4 o;
void main() {
    int k = max(int(pc.a.x + 0.5), 1);
    ivec2 base = ivec2(gl_FragCoord.xy) * k;
    float c = 0.0, s = 0.0;
    for (int y = 0; y < k; y++)
        for (int x = 0; x < k; x++) {
            float d = texelFetch(dep, base + ivec2(x, y), 0).r;
            if (d < 1.0) { c += 1.0; s += pc.a.z / max(pc.a.y - d, 1e-6); }
        }
    float n = float(k * k);
    o = vec4(c / n, s / n * (1.0 / 4096.0), 0.0, 1.0);
}
