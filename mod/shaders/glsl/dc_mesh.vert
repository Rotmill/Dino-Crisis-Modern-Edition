#version 460
#extension GL_GOOGLE_include_directive : require
// 06.10.2026: меш из mods — основной проход и кадр B (то же, что dc_raster.vert для потока примитивов, но вершина считается
// здесь: dc_mesh_common.glsl). Выход — те же переменные, фрагментная ступень — тот же dc_raster.frag.
#include "dc_mesh_common.glsl"

layout(push_constant) uniform PC {
    vec4 view;
    vec4 params;
    vec4 prevRect;
    vec4 prevMap;
    vec4 wq;          // кадр B: поворот видового пространства
    vec4 wt;          // xyz перенос; w > 0.5 — кадр B
    vec4 wd;          // xyz сдвиг Регины (F_ACTOR); w > 0.5 — прицеливание (её не трогать)
    vec4 wp;
} pc;

layout(binding = 4, std140) uniform ShadowUBO {
    mat4 vp[4];
    vec4 par[4];
    vec4 lpos[4];
    vec4 cfg;
    vec4 cfg2;
    vec4 flp;
    vec4 flc;
    vec4 mdir;
    vec4 mcol;
    vec4 rfp;
    vec4 rfc;
    vec4 rfm0;
    vec4 rfm1;
    vec4 mpf;
    vec4 spare;
    // 19.10.2026: её собственный доворот в кадрах между тактами (gpu_vk_raster.cpp warp_pc_make, ячейки 528..535; поля
    // dc_raster.frag до них - здесь только ради раскладки блока)
    vec4 pad144_[96];
    vec4 hq;          // кватернион её поворота в видовом пространстве кадра A (доля пути)
    vec4 hc;          // xyz - точка на её вертикальной оси (вид кадра A), w > 0.5 - доворот есть
} sh;

vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

layout(location = 0)      out vec2 v_uv;
layout(location = 1)      out vec3 v_color;
layout(location = 2) flat out ivec4 v_info;
layout(location = 3) flat out vec2  v_uvp;
layout(location = 4)      out vec3  v_view;
layout(location = 5) flat out int   v_rep;
layout(location = 6)      out vec2  v_repuv;
layout(location = 7)      out float v_glint;   // 12.10: блеск предметов (dc_raster.vert) - у мешей из mods нет

void main() {
    vec3 p, col;
    mesh_vertex(true, p, col);
    const int I = gl_InstanceIndex * 9;
    const vec4 pr = mi[I], misc = mi[I + 1], info = mi[I + 2];
    const float zmul = misc.x, seq = misc.y;
    v_uv = vec2(0.0);
    v_uvp = vec2(0.0);
    v_color = col;
    v_info = ivec4(round(info));
    v_rep = int(round(in_slot));
    v_repuv = in_uv;
    v_glint = 0.0;
    const vec3 q = p * zmul;                               // видовая точка с множителем глубины (как поток: экран — от p)
    const bool fx = sh.cfg.x > 0.5 || sh.flp.w > 0.0 || sh.rfc.w > 0.5 || sh.spare.x > 0.5 || sh.spare.y > 0.5;   /* 19.10: и свет текстур, блики */
    if (pc.wt.w > 0.5) {                                   // кадр B — как ветка перепроецирования dc_raster.vert
        vec3 b = q;
        const bool actor = (v_info.w & 16) != 0;
        const bool hturn = actor && sh.hc.w > 0.5 && !(pc.wd.w > 0.5);   // 19.10: её доворот (dc_raster.vert)
        if (!(actor && pc.wd.w > 0.5)) {
            if (hturn) b = sh.hc.xyz + qrot(sh.hq, b - sh.hc.xyz);
            if (actor) b += pc.wd.xyz;
            b = qrot(pc.wq, b) + pc.wt.xyz;
        }
        float zw = min(128.0, b.z * 0.999);
        zw *= 1.0 + seq * 2e-7;
        gl_Position = vec4(mesh_clip(pr, pc.view, b), zw, b.z);
        v_view = fx ? (hturn ? q : b) : vec3(0.0);   // 19.10: при довороте - точка кадра A (dc_raster.frag)
        return;
    }
    v_view = (fx && q.z >= 1.5) ? q : vec3(0.0);
    float zc = min(128.0, q.z * 0.999);
    zc *= 1.0 + seq * 2e-7;
    gl_Position = vec4(mesh_clip(pr, pc.view, p) * zmul, zc, q.z);
}
