#version 460
// DC GPU raster - ЗАТЕНЕНИЕ УГЛОВ (SSAO) в четверть разрешения: размытие на самой сетке (30.10.2026, игрок: «сделай SSAO
// в низком пресете дешевле»). Вход - сырое затенение dc_ssao_q.frag (aoTex, RG32F: r - незатенённость, g - w точки, 0 -
// нет). Окно 5x5 клеток, у крайних рядов вес 1/2: сумма весов по оси 4, все 16 поворотов узора 4x4 с равным весом, окно
// по центру клетки. Вес глубины - та же поверхность (w ближе допуска). Выход - та же сетка: r - размытое, g - w самой
// клетки. Размытие выключено (par2.y, отладка) - сырое как есть. Кадр берёт отсюда 2x2 клетки (dc_ssao_comp2.frag,
// DC_AO_STEP=4): размытие 16 выборок на пиксель кадра стало 25 на клетку сетки.
layout(binding = 2) uniform sampler2D aoTex;

layout(push_constant) uniform PC {
    vec4 view;
    vec4 wp;
    vec4 rt;     // активная область, полное разрешение
    vec4 par;    // x - радиус, y - сила, z - смещение, w - выборок
    vec4 par2;   // x - предел радиуса на экране, y - размытие (0/1), z - отладка (0/1)
} pc;

layout(location = 0) out vec4 out_ao;

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    ivec2 qsz = (ivec2(pc.rt.xy) + 3) / 4;
    vec2 a0 = texelFetch(aoTex, c, 0).rg;
    if (a0.g <= 0.0 || pc.par2.y <= 0.5) { out_ao = vec4(a0, 0.0, 1.0); return; }
    float tol = a0.g * 0.04 + 16.0;
    float s = 0.0, ws = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            vec2 a = texelFetch(aoTex, clamp(c + ivec2(x, y), ivec2(0), qsz - 1), 0).rg;
            if (a.g <= 0.0) continue;
            float k = max(1.0 - abs(a.g - a0.g) / tol, 0.0) * (abs(x) == 2 ? 0.5 : 1.0) * (abs(y) == 2 ? 0.5 : 1.0);
            s += a.r * k; ws += k;
        }
    out_ao = vec4(ws > 1e-4 ? s / ws : a0.r, a0.g, 0.0, 1.0);
}
