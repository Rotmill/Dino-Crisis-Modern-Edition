#version 460
// DC GPU raster — ЗАТЕНЕНИЕ УГЛОВ (SSAO), проход 2 из 3 (25.09.2026, игрок: «сделай 2 (ssao)»).
// Сырое затенение в ПОЛОВИННОМ разрешении (точка половинной сетки = левый верхний пиксель своего квадрата 2x2):
// мягкая темнота в стыках стен и пола, под мебелью, у ног.
//  - Видовая точка: w из прохода 1 (zTex, полное разрешение), координаты — по параметрам проекции wp (те же, что у
//    теней: центр + сдвиг области рисования, H, H·сжатие 16:9).
//  - Нормаль — из соседних пикселей полного разрешения: на каждой оси берётся сосед с меньшим скачком глубины (на ребре
//    второй сосед — чужая поверхность). Полигоны PS1 плоские, поэтому нормаль точная.
//  - Выборки — полусфера над нормалью радиусом R единиц комнаты (косинусное распределение, ближе к точке гуще),
//    повёрнутая по узору 4x4 половинной сетки. Проход 3 размывает 4x4 и усредняет все 16 поворотов.
// Выход (RG32F): r — незатенённость 0..1, g — w точки (0 — затенять нечего: пустота или поверх плоское).
layout(binding = 3) uniform sampler2D zTex;

layout(push_constant) uniform PC {
    vec4 view;   // x0, y0, w, h — область дисплея в draw-space
    vec4 wp;     // ox, oy (центр проекции + сдвиг области рисования), H, H * сжатие по горизонтали
    vec4 rt;     // ширина и высота активной области цели (полное разрешение, px), 1/ширина, 1/высота
    vec4 par;    // x — радиус (ед. комнаты), y — сила, z — смещение (ед.), w — выборок
    vec4 par2;   // x — предел радиуса на экране (доля высоты), y — размытие, z — отладка
} pc;

layout(location = 0) out vec4 out_ao;

float wAt(ivec2 p) {                             // |w| пикселя полного разрешения (0 — геометрии нет)
    p = clamp(p, ivec2(0), ivec2(pc.rt.xy) - 1);
    return abs(texelFetch(zTex, p, 0).r);
}
vec3 viewPos(vec2 pix, float w) {
    vec2 ds = pc.view.xy + pix * pc.rt.zw * pc.view.zw;
    return vec3((ds.x - pc.wp.x) * w / pc.wp.w, (ds.y - pc.wp.y) * w / pc.wp.z, w);
}
vec2 projPix(vec3 P) {
    vec2 ds = vec2(pc.wp.x + P.x * pc.wp.w / P.z, pc.wp.y + P.y * pc.wp.z / P.z);
    return (ds - pc.view.xy) / pc.view.zw * pc.rt.xy;
}

const int bayer4[16] = int[16]( 0,  8,  2, 10,
                               12,  4, 14,  6,
                                3, 11,  1,  9,
                               15,  7, 13,  5);

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 ip = hp * 2;
    if (ip.x >= int(pc.rt.x) || ip.y >= int(pc.rt.y)) { out_ao = vec4(1.0, 0.0, 0.0, 1.0); return; }
    float zs = texelFetch(zTex, ip, 0).r;
    if (zs <= 0.0) { out_ao = vec4(1.0, 0.0, 0.0, 1.0); return; }   // пустота или поверх плоское
    vec2 fc = vec2(ip) + 0.5;
    vec3 P = viewPos(fc, zs);

    // нормаль по соседям полного разрешения
    float wr = wAt(ip + ivec2(1, 0)), wl = wAt(ip - ivec2(1, 0));
    float wd = wAt(ip + ivec2(0, 1)), wu = wAt(ip - ivec2(0, 1));
    vec3 N = -normalize(P);
    if ((wr > 0.0 || wl > 0.0) && (wd > 0.0 || wu > 0.0)) {
        vec3 Pr = viewPos(fc + vec2(1.0, 0.0), wr), Pl = viewPos(fc - vec2(1.0, 0.0), wl);
        vec3 Pd = viewPos(fc + vec2(0.0, 1.0), wd), Pu = viewPos(fc - vec2(0.0, 1.0), wu);
        vec3 ex = (wr > 0.0 && (wl <= 0.0 || abs(Pr.z - P.z) < abs(P.z - Pl.z))) ? Pr - P : P - Pl;
        vec3 ey = (wd > 0.0 && (wu <= 0.0 || abs(Pd.z - P.z) < abs(P.z - Pu.z))) ? Pd - P : P - Pu;
        vec3 c = cross(ex, ey);
        float cl = length(c);
        if (cl > 1e-12) N = c / cl;
    }
    if (dot(N, P) > 0.0) N = -N;                         // к камере (она в начале координат)

    // радиус: в единицах комнаты, но вблизи камеры не больше доли кадра
    float R = pc.par.x;
    float pxPerUnit = pc.wp.z / P.z * pc.rt.y / pc.view.w;
    float rpx = R * pxPerUnit;
    float maxPx = pc.par2.x * pc.rt.y;
    if (rpx > maxPx) { R *= maxPx / rpx; rpx = maxPx; }
    if (rpx < 2.0) { out_ao = vec4(1.0, zs, 0.0, 1.0); return; }   // так далеко, что затенению не на чем лечь

    // базис полусферы, повёрнутый по узору 4x4 половинной сетки
    float rot = (float(bayer4[(hp.y & 3) * 4 + (hp.x & 3)]) + 0.5) / 16.0 * 6.2831853;
    vec3 T = normalize(cross(N, (abs(N.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);
    int n = clamp(int(pc.par.w + 0.5), 1, 64);
    float bias = pc.par.z + P.z * 0.0015;
    float occ = 0.0;
    for (int i = 0; i < n; i++) {
        float u = (float(i) + 0.5) / float(n);
        float cosT = sqrt(1.0 - u), sinT = sqrt(u);
        float phi = float(i) * 2.3999632 + rot;
        float t = fract(float(i) * 0.7548776662 + 0.5698402910);
        float sc = 0.12 + 0.88 * t * t;
        vec3 h = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        vec3 S = P + (T * h.x + B * h.y + N * h.z) * (R * sc);
        if (S.z <= 1.0) continue;
        vec2 sp = projPix(S);
        if (sp.x < 0.0 || sp.y < 0.0 || sp.x >= pc.rt.x || sp.y >= pc.rt.y) continue;   // за кадром — сведений нет
        float sw = abs(texelFetch(zTex, ivec2(sp), 0).r);
        if (sw <= 0.0) continue;
        if (sw < S.z - bias) {
            // заслонивший далеко впереди (Регина в метрах от стены) — не угол, ореола нет
            occ += smoothstep(0.0, 1.0, R / max(abs(P.z - sw), 1e-3));
        }
    }
    out_ao = vec4(clamp(1.0 - occ / float(n), 0.0, 1.0), zs, 0.0, 1.0);
}
