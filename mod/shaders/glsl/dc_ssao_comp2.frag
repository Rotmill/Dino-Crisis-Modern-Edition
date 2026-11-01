#version 460
// DC GPU raster - ЗАТЕНЕНИЕ УГЛОВ (SSAO), проход 3 из 3 при раздельных целях прохода 1 (29.10.2026, dc_ssao_z2.frag).
// То же, что dc_ssao_comp.frag: zTex теперь R32F (только ±w), доля затенения - из сырой альфы m (maskTex, R8), той же
// формулой, что прежде считал проход 1: g = clamp(m * 2 - 1, 0, 1). Свой набор дескрипторов (s_pdset2): binding 1 - маска
// вместо цветной цели (её этот проход не читает), 2 - aoTex, 3 - zTex.
// 30.10.2026 (игрок: «сделай SSAO в низком пресете дешевле»): DC_AO_STEP=4 - dc_ssao_comp2_q.frag.spv, свой набор
// (s_pdset3): aoTex - сетка в четверть разрешения, уже размытая (dc_ssao_qblur.frag), точка клетки g - пиксель 4g + 2.
// Пиксель берёт 2x2 ближайших клетки с весами по расстоянию (без ступенек по 4 пикселя) и весом глубины; ни одна не с
// его поверхности - подходящие из 4x4 вокруг с весом глубины (тонкое, пологое), иначе без затенения.
#ifndef DC_AO_STEP
#define DC_AO_STEP 2
#endif
layout(binding = 1) uniform sampler2D maskTex;   // полное разрешение: r - альфа цветной цели после прохода кадра
layout(binding = 2) uniform sampler2D aoTex;     // половинная сетка: r - незатенённость, g - w точки (0 - нет)
layout(binding = 3) uniform sampler2D zTex;      // полное разрешение: +w / -w / 0 (см. dc_ssao_z2.frag)

layout(push_constant) uniform PC {
    vec4 view;
    vec4 wp;
    vec4 rt;     // активная область, полное разрешение
    vec4 par;    // x - радиус, y - сила, z - смещение, w - выборок
    vec4 par2;   // x - предел радиуса на экране, y - размытие (0/1), z - отладка (0/1)
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    bool dbg = pc.par2.z > 0.5;
    float w0 = texelFetch(zTex, ip, 0).r;
    if (w0 <= 0.0) { out_color = dbg ? vec4(0.10, 0.12, 0.35, 1.0) : vec4(1.0); return; }
#if DC_AO_STEP == 4
    ivec2 qsz = (ivec2(pc.rt.xy) + 3) / 4;
    float tol = w0 * 0.04 + 16.0;                        // точки вдвое дальше, чем у половинной сетки - допуск вдвое
    float ao = 1.0;
    vec2 u = (vec2(ip) - 2.0) * 0.25;
    vec2 g0f = floor(u), f = u - g0f;
    ivec2 g0 = ivec2(g0f);
    // 30.10.2026 (разбор): клетка своей поверхности не получает вес ровно 0 (в точке клетки 4g + 2 f = 0, соседняя клетка
    // выпадала - у тонкого шов в 1 пиксель через каждые 4)
    f = clamp(f, 1.0 / 64.0, 63.0 / 64.0);
    float s = 0.0, ws = 0.0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            vec2 a = texelFetch(aoTex, clamp(g0 + ivec2(x, y), ivec2(0), qsz - 1), 0).rg;
            if (a.g <= 0.0) continue;
            float k = max(1.0 - abs(a.g - w0) / tol, 0.0) * (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            s += a.r * k; ws += k;
        }
    if (ws > 0.0) ao = s / ws;
    else {
        // ни одна из 2x2 не с его поверхности (тонкое, пологое): 4x4 вокруг с весом глубины, как у половинной сетки - без
        // скачков, когда окно сдвигается на клетку (прежде - одна ближайшая клетка)
        float s2 = 0.0, w2 = 0.0;
        for (int y = -1; y < 3; y++)
            for (int x = -1; x < 3; x++) {
                vec2 a = texelFetch(aoTex, clamp(g0 + ivec2(x, y), ivec2(0), qsz - 1), 0).rg;
                if (a.g <= 0.0) continue;
                float k = max(1.0 - abs(a.g - w0) / tol, 0.0);
                s2 += a.r * k; w2 += k;
            }
        if (w2 > 0.0) ao = s2 / w2;
    }
    ao = max(ao, 1e-6);                                  // сила 0 ниже: pow(0, 0) - не число
#else
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
#endif
    ao = pow(clamp(ao, 0.0, 1.0), max(pc.par.y, 0.0));
    float m = texelFetch(maskTex, ip, 0).r;
    ao = mix(1.0, ao, clamp(m * 2.0 - 1.0, 0.0, 1.0));  // 01.10: доля затенения точки (ячейка «третьей энергии» - меньше 1)
    out_color = vec4(vec3(ao), 1.0);
}
