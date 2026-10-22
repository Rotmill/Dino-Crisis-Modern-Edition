#version 460
#extension GL_GOOGLE_include_directive : require
// 06.10.2026: меш из mods в проходах свечения (1) и отражения (2) — как dc_fx.vert; картинка замены — v_rep/v_repuv (6, 7).
#include "dc_mesh_common.glsl"

layout(push_constant) uniform PC {
    vec4 view;
    vec4 params;
    vec4 prevRect;
    vec4 prevMap;     // w = вид прохода
    vec4 wq;
    vec4 wt;
    vec4 wd;
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
layout(location = 5)      out float v_rfd;
layout(location = 6) flat out int   v_rep;
layout(location = 7)      out vec2  v_repuv;

void main() {
    vec3 p, col;
    mesh_vertex(true, p, col);
    const int I = gl_InstanceIndex * 9;
    const vec4 pr = mi[I], misc = mi[I + 1], info = mi[I + 2];
    v_uv = vec2(0.0);
    v_uvp = vec2(0.0);
    v_color = col;
    v_info = ivec4(round(info));
    v_view = vec3(0.0);
    v_rfd = 1e6;
    v_rep = int(round(in_slot));
    v_repuv = in_uv;
    vec3 q = p * misc.x;
    if (q.z < 1.5) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    const int mode = int(pc.prevMap.w + 0.5);
    const bool warp = pc.wt.w > 0.5;
    if (mode == 1 && !warp) {
        gl_Position = vec4(mesh_clip(pr, pc.view, p) * misc.x, min(128.0, q.z * 0.999), q.z);
        return;
    }
    const bool actor = (v_info.w & 16) != 0;
    const bool rigid = actor && pc.wd.w > 0.5;
    if (warp && actor && !rigid && sh.hc.w > 0.5) q = sh.hc.xyz + qrot(sh.hq, q - sh.hc.xyz);   // 19.10: её доворот (dc_raster.vert)
    if (warp && actor && !rigid) q += pc.wd.xyz;
    if (mode == 2) {
        const float sd = dot(sh.rfp.xyz, q) - sh.rfp.w;
        v_rfd = sd;
        q -= 2.0 * sd * sh.rfp.xyz;
    }
    if (warp && !rigid) q = qrot(pc.wq, q) + pc.wt.xyz;
    gl_Position = vec4(mesh_clip(pr, pc.view, q), min(128.0, q.z * 0.999), q.z);
}
