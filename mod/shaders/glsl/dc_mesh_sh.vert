#version 460
#extension GL_GOOGLE_include_directive : require
// 06.10.2026: меш из mods в карте теней лампы — как dc_shadow.vert: окклюдер — персонаж (F_ACTOR 16, F_CHAR 8388608), точка
// вида → пространство лампы. Свет вершины не нужен.
#include "dc_mesh_common.glsl"

layout(push_constant) uniform PC {
    vec4 view;
    vec4 params;
    vec4 prevRect;
    vec4 prevMap;
    vec4 wq;
    vec4 wt;
    vec4 wd;          // w = номер лампы на этом проходе
    vec4 wp;
} pc;

layout(binding = 4, std140) uniform ShadowUBO {
    mat4 vp[4];
    vec4 par[4];
    vec4 lpos[4];
    vec4 cfg;
    vec4 cfg2;
} sh;

void main() {
    const int I = gl_InstanceIndex * 9;
    const int fl = int(round(mi[I + 2].w));
    if ((fl & (16 | 8388608)) == 0 || (fl & 2) != 0) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    vec3 p, col;
    mesh_vertex(false, p, col);
    const vec3 q = p * mi[I + 1].x;
    if (q.z < 1.5) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    const int idx = clamp(int(pc.wd.w + 0.5), 0, 3);
    gl_Position = sh.vp[idx] * vec4(q, 1.0);
}
