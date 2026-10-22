#version 460
// DC GPU raster — ЗАТЕНЕНИЕ УГЛОВ (SSAO), проход 3 из 3 (25.09.2026): размытие + умножение кадра, полное разрешение.
// Окно 4x4 половинной сетки (в нём все 16 поворотов выборок прохода 2 — узор уходит) с учётом глубины: берутся только
// точки той же поверхности (w в пределах 2 %), края не смазываются. Где маска прохода 1 не пускает — множитель 1.
// Смешение конвейера: цвет = цвет кадра * выход (ZERO, SRC_COLOR); альфа = 1 — маска своё отработала, вывод SDL видит
// непрозрачный кадр, как раньше. Сила — степень: незатенённость ^ сила.
// Отладка (par2.z = 1, конвейер без смешения): белое — не затенено, чёрное — затенено, синее — вне маски (пустота,
// интерфейс, текст, плоские экраны): туда затенение не кладётся.
layout(binding = 2) uniform sampler2D aoTex;     // половинная сетка: r — незатенённость, g — w точки (0 — нет)
layout(binding = 3) uniform sampler2D zTex;      // полное разрешение: +w / -w / 0 (см. dc_ssao_z.frag)

layout(push_constant) uniform PC {
    vec4 view;
    vec4 wp;
    vec4 rt;     // активная область, полное разрешение
    vec4 par;    // x — радиус, y — сила, z — смещение, w — выборок
    vec4 par2;   // x — предел радиуса на экране, y — размытие (0/1), z — отладка (0/1)
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    bool dbg = pc.par2.z > 0.5;
    float w0 = texelFetch(zTex, ip, 0).r;
    if (w0 <= 0.0) { out_color = dbg ? vec4(0.10, 0.12, 0.35, 1.0) : vec4(1.0); return; }
    ivec2 hsz = (ivec2(pc.rt.xy) + 1) / 2;
    ivec2 hp = ip / 2;
    float tol = w0 * 0.02 + 8.0;                         // 2 % глубины: та же поверхность
    float ao = 1.0;
    if (pc.par2.y > 0.5) {
        float s = 0.0, ws = 0.0;
        for (int y = -2; y < 2; y++)
            for (int x = -2; x < 2; x++) {
                vec2 a = texelFetch(aoTex, clamp(hp + ivec2(x, y), ivec2(0), hsz - 1), 0).rg;
                if (a.g <= 0.0) continue;
                float k = max(1.0 - abs(a.g - w0) / tol, 0.0);
                s += a.r * k; ws += k;
            }
        if (ws > 1e-4) ao = s / ws;
    } else {
        vec2 a = texelFetch(aoTex, clamp(hp, ivec2(0), hsz - 1), 0).rg;
        if (a.g > 0.0 && abs(a.g - w0) < tol) ao = a.r;
    }
    ao = pow(clamp(ao, 0.0, 1.0), max(pc.par.y, 0.0));
    ao = mix(1.0, ao, texelFetch(zTex, ip, 0).g);        // 01.10: доля затенения точки (ячейка «третьей энергии» — меньше 1)
    out_color = vec4(vec3(ao), 1.0);
}
