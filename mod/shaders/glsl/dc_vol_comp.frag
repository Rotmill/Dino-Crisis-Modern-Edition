#version 460
#extension GL_GOOGLE_include_directive : require
// DC GPU raster — ОБЪЁМНЫЙ СВЕТ У ЛАМП, проход 2 из 2 (27.09.2026): окно 4x4 половинной сетки с учётом глубины (сводит узор
// шагов прохода 1 и не тянет дымку дальней стены на близкий край) и СЛОЖЕНИЕ с готовым кадром (после SSAO и свечения).
// Альфа = 1 (readback — SDL BLEND). Поверх плоского (интерфейс, текст) не кладётся ничего.
// par3.x > 0.5 — отладка: точки ламп поверх кадра (пурпурные — лампы игры, голубые — текстуры «светится»).
#include "dc_vol_common.glsl"
layout(binding = 0) uniform sampler2D zTex;
layout(binding = 3) uniform sampler2D volTex;
layout(location = 0) out vec4 out_color;

// точка видового пространства кадра A -> пиксель этой цели (кадр B — своим поворотом); z <= 0 — за камерой
vec3 toPix(vec3 pA) {
    vec3 p = (pc.wt.w > 0.5) ? qrot(pc.wq, pA) + pc.wt.xyz : pA;
    if (p.z <= 50.0) return vec3(-1e4, -1e4, 0.0);
    vec2 ds = vec2(pc.wp.x + p.x * pc.wp.w / p.z, pc.wp.y + p.y * pc.wp.z / p.z);
    return vec3((ds - pc.view.xy) / pc.view.zw * pc.rt.xy, p.z);
}

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    float zc = texelFetch(zTex, ip, 0).r;
    vec3 c = vec3(0.0);
    if (zc >= 0.0) {
        ivec2 hs = ivec2((pc.rt.xy + 1.0) * 0.5), hp = ip / 2;     /* активная область половинной сетки этого кадра */
        vec3 sum = vec3(0.0); float ws = 0.0;
        for (int y = -2; y <= 1; y++)
            for (int x = -2; x <= 1; x++) {
                vec4 v = texelFetch(volTex, clamp(hp + ivec2(x, y), ivec2(0), hs - 1), 0);
                if (v.a < 0.0) continue;
                float wgt;
                if (zc > 0.0) wgt = (v.a > 0.0) ? exp(-abs(v.a - zc) / (0.03 * zc + 30.0)) : 0.02;
                else          wgt = (v.a == 0.0) ? 1.0 : 0.02;
                sum += v.rgb * wgt; ws += wgt;
            }
        if (ws > 1e-5) c = sum / ws;
    }
    // 28.09 (6): пелена игры — затемнение перехода и полосы кино (квады «вычитание» / «среднее» поверх мира, gpu_vk_raster.cpp
    // veil_collect) — гасит дымку так же, как весь остальной кадр: дымки не видно там, где не видно и комнаты
    {
        int nv = int(sh.vspare.x + 0.5);
        for (int i = 0; i < 4; i++) {
            if (i >= nv) break;
            vec4 V = sh.veilV[i];
            if (gl_FragCoord.y >= V.x && gl_FragCoord.y < V.y) c = max(c * V.z - vec3(V.w), vec3(0.0));
        }
    }
    if (pc.par3.x > 0.5) {
        vec2 fp = vec2(ip) + 0.5;
        int nl = (pc.par.x > 0.0) ? int(sh.vcfg.x + 0.5) : 0, ng = int(sh.spare.x + 0.5);
        for (int i = 0; i < 16; i++) {
            if (i >= nl + ng) break;
            vec3 L = (i < nl) ? sh.vlp[i].xyz : sh.glp[i - nl].xyz;
            vec3 q = toPix(L);
            float d = length(fp - q.xy);
            if (q.z > 0.0 && d < 7.0 && d > 3.5) c = (i < nl) ? vec3(1.0, 0.1, 1.0) : vec3(0.1, 1.0, 1.0);
        }
    }
    out_color = vec4(c, 1.0);
}
