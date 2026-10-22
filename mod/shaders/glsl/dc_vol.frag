#version 460
#extension GL_GOOGLE_include_directive : require
// DC GPU raster — ОБЪЁМНЫЙ СВЕТ У ЛАМП, проход 1 из 2 (27.09.2026, игрок: «Попробуй объемный свет у ламп сделать (дымка и
// пыль в лучах)» — пункт №4 списка улучшений от 24.09). Половинное разрешение. Луч из камеры до поверхности — глубина из
// прохода 1 SSAO (zTex: +w — 3D, −w — поверх плоское, 0 — пусто); по лучу набирается свет, рассеянный в воздухе:
//  * у ламп игры (таблица ctx+348: точка, радиус, цвет; pt_renderer.c dc_vol_frame — vlp/vlc): шар радиус × доля (par.z),
//    спад (1 − d/R)², рассеяние вперёд по Хеньи — Гринстейну (ярче, когда смотришь в сторону лампы); у ламп, у которых
//    есть карта теней персонажей, — их тень прямо в дымке;
//  * у текстур «светится» (glp/glc — те же источники, что светят на стены): своя сила, без теней.
// Воздух неровный: медленно плывущие клубы — шум в координатах КОМНАТЫ (v2r), дымка не «прилипает» к экрану.
// Шагов на лампу — par2.w, начало шага сдвинуто по узору 4x4 (проход 2 усредняет окно 4x4 — полос нет).
// Кадр B: точки луча возвращаются в пространство кадра A тем же кватернионом, что у растра (dc_raster.frag shadowViewPoint).
// Выход RGBA16F: rgb — свет дымки, a — w точки из zTex (проход 2 сводит по глубине).
#include "dc_vol_common.glsl"
layout(binding = 0) uniform sampler2D zTex;
layout(location = 0) out vec4 out_v;

const int bayer4[16] = int[16]( 0,  8,  2, 10,
                               12,  4, 14,  6,
                                3, 11,  1,  9,
                               15,  7, 13,  5);

float vnoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(i), b = hash13(i + vec3(1, 0, 0)), c = hash13(i + vec3(0, 1, 0)), d = hash13(i + vec3(1, 1, 0));
    float e = hash13(i + vec3(0, 0, 1)), g = hash13(i + vec3(1, 0, 1)), h = hash13(i + vec3(0, 1, 1)), k = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}
// плотность воздуха в точке S: 1 ± клубы (шум в комнате, плывёт медленно)
float density(vec3 S) {
    if (pc.par2.x <= 0.0 || sh.vcfg.w < 0.5) return 1.0;
    vec3 r = roomOf(S) * (1.0 / 900.0) + vec3(pc.par2.y * 0.021, pc.par2.y * 0.013, -pc.par2.y * 0.017);
    float n = vnoise(r) * 0.65 + vnoise(r * 2.3 + vec3(7.1, 3.7, 1.9)) * 0.35;
    return max(0.0, 1.0 + pc.par2.x * (n * 2.0 - 1.0));
}
// рассеяние по Хеньи — Гринстейну, 1 — равномерное
float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}
// свет одной лампы, рассеянный на отрезке луча oA + dA·t, t < tEnd (t — глубина w в кадре, где строился луч)
vec3 lampHaze(vec3 L, float Rh, vec3 col, int layer, vec3 oA, vec3 dA, float tEnd, float jit, int ns) {
    if (Rh < 1.0 || max(col.r, max(col.g, col.b)) <= 0.0) return vec3(0.0);
    vec3 oc = oA - L;
    float a = dot(dA, dA), b = dot(oc, dA), c = dot(oc, oc) - Rh * Rh;
    float disc = b * b - a * c;
    if (disc <= 0.0) return vec3(0.0);
    float sq = sqrt(disc);
    float t0 = max((-b - sq) / a, 60.0), t1 = min((-b + sq) / a, tEnd);
    if (t1 <= t0) return vec3(0.0);
    float dt = (t1 - t0) / float(ns);
    bool shd = volLayerOk(layer);
    float sum = 0.0;
    const float la = sqrt(a), r0 = 0.2 * Rh;
    for (int k = 0; k < 32; k++) {
        if (k >= ns) break;
        float t = t0 + (float(k) + jit) * dt;
        vec3 S = oA + dA * t;
        vec3 toS = S - L;
        float d = length(toS);
        float at = 1.0 - d / Rh;
        if (at <= 0.0) continue;
        // 27.09 (игрок: дымка — «на местах светящихся текстур»): плотное ядро у самого источника (r0) и слабый хвост —
        // дымка держится у лампы, а не заволакивает комнату; у объектива (150..900 ед.) её нет — не пелена на весь кадр
        float q = d / r0;
        at *= at * (0.12 + 0.88 / ((1.0 + q * q) * (1.0 + q * q)));
        at *= smoothstep(150.0, 900.0, t * la);
        vec3 toEye = oA - S;
        float cth = dot(toS, toEye) / max(d * length(toEye), 1e-3);
        float v = shd ? volShadow(layer, S) : 1.0;
        sum += at * v * phaseHG(cth, pc.par.w) * density(S);
    }
    return col * (sum * dt * la / Rh);            // шаг — в единицах комнаты, доля радиуса
}

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 ip = hp * 2;
    if (ip.x >= int(pc.rt.x) || ip.y >= int(pc.rt.y)) { out_v = vec4(0.0); return; }
    float zs = texelFetch(zTex, ip, 0).r;
    if (zs < 0.0) { out_v = vec4(0.0, 0.0, 0.0, zs); return; }     // поверх плоское (интерфейс, текст) — без дымки
    vec2 fc = vec2(ip) + 1.0;                                      // центр своего квадрата 2x2
    vec2 ds = pc.view.xy + fc * pc.rt.zw * pc.view.zw;
    vec3 dB = vec3((ds.x - pc.wp.x) / pc.wp.w, (ds.y - pc.wp.y) / pc.wp.z, 1.0);   // точка луча при w = 1
    vec3 oA = vec3(0.0), dA = dB;
    if (pc.wt.w > 0.5) { vec4 qi = vec4(-pc.wq.xyz, pc.wq.w); oA = qrot(qi, -pc.wt.xyz); dA = qrot(qi, dB); }
    float tEnd = (zs > 0.0) ? zs : 60000.0;
    float jit = (float(bayer4[(hp.y & 3) * 4 + (hp.x & 3)]) + 0.5) / 16.0;
    int ns = clamp(int(pc.par2.w + 0.5), 2, 32);
    vec3 acc = vec3(0.0);
    if (pc.par.x > 0.0) {
        int nl = int(sh.vcfg.x + 0.5);
        for (int i = 0; i < 8; i++) {
            if (i >= nl) break;
            acc += lampHaze(sh.vlp[i].xyz, sh.vlp[i].w * pc.par.z, sh.vlc[i].rgb * pc.par.x, int(floor(sh.vlc[i].w + 0.5)),
                            oA, dA, tEnd, jit, ns);
        }
    }
    if (pc.par.y > 0.0) {
        int ng = int(sh.spare.x + 0.5);
        for (int i = 0; i < 8; i++) {
            if (i >= ng) break;
            acc += lampHaze(sh.glp[i].xyz, sh.glp[i].w * pc.par.z, sh.glc[i].rgb * pc.par.y, -1, oA, dA, tEnd, jit, max(ns / 2, 3));
        }
    }
    out_v = vec4(acc, zs);
}
