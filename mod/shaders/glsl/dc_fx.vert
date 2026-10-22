#version 460
// 25.09.2026 вечер: СВЕЧЕНИЕ ИСТОЧНИКОВ И ОТРАЖЕНИЯ НА ПОЛУ (gpu_vk_raster.cpp fx_geom_pass) — вершины кадра в цели
// половинного разрешения. Вид прохода — pc.prevMap.w: 1 — свечение (та же проекция, что у основного прохода), 2 — отражение:
// видовая точка (обратная перспектива по PGXP w, как у кадра B) отражается плоскостью пола (блок ShadowUBO, rfp: нормаль к
// камере, n·p = d) и проецируется обратно. Кадр B (pc.wt.w > 0.5) перепроецирует точку, как dc_raster.vert, — отражение
// строится в пространстве кадра A, потом поворот. Без глубины PGXP (интерфейс, текст) — отсекается.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_info;
layout(location = 4) in float in_w;

layout(push_constant) uniform PC {
    vec4 view;
    vec4 params;
    vec4 prevRect;    // здесь — параметры прохода (dc_raster.frag, DC_FX_PASS)
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
    vec4 rfp;         // плоскость пола в видовом пространстве кадра A: xyz нормаль (к камере), w = d
    vec4 rfc;
    vec4 rfm0;        // 25.09 ночь: материалы пола, луна гладкой тенью (dc_raster.frag)
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
layout(location = 5)      out float v_rfd;   // высота над полом (отражение: ниже пола — отбрасывается)
layout(location = 6) flat out int   v_rep;   // 06.10.2026: картинка из mods — у потока нет (−1), у мешей — dc_mesh_fx.vert
layout(location = 7)      out vec2  v_repuv;

void main() {
    v_uv = in_uv;
    v_color = in_color.rgb;
    v_info = ivec4(round(in_info));
    v_uvp = in_uv;
    v_view = vec3(0.0);
    v_rfd = 1e6;
    v_rep = -1;
    v_repuv = vec2(0.0);
    if (in_w < 1.5) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }   // без глубины — не мир
    const int mode = int(pc.prevMap.w + 0.5);
    const bool warp = pc.wt.w > 0.5;
    if (mode == 1 && !warp) {
        // свечение кадра A: ровно та же проекция, что у основного прохода (ореол не съезжает с источника)
        vec2 ndc = ((in_pos - pc.view.xy) / pc.view.zw) * 2.0 - 1.0;
        gl_Position = vec4(ndc * in_w, min(128.0, in_w * 0.999), in_w);
        return;
    }
    vec3 p = vec3((in_pos.x - pc.wp.x) * in_w / pc.wp.w, (in_pos.y - pc.wp.y) * in_w / pc.wp.z, in_w);
    const bool actor = (int(round(in_info.w)) & 16) != 0;
    const bool rigid = actor && pc.wd.w > 0.5;          // кадр B в прицеливании: её вершины не трогаются
    if (warp && actor && !rigid && sh.hc.w > 0.5) p = sh.hc.xyz + qrot(sh.hq, p - sh.hc.xyz);   // 19.10: её доворот (dc_raster.vert)
    if (warp && actor && !rigid) p += pc.wd.xyz;        // Регина едет с пивотом (в пространстве кадра A)
    if (mode == 2) {
        float sd = dot(sh.rfp.xyz, p) - sh.rfp.w;       // высота над полом
        v_rfd = sd;
        p -= 2.0 * sd * sh.rfp.xyz;
    }
    if (warp && !rigid) p = qrot(pc.wq, p) + pc.wt.xyz;
    vec2 cxy = vec2(((pc.wp.x - pc.view.x) / pc.view.z * 2.0 - 1.0) * p.z + 2.0 * pc.wp.w * p.x / pc.view.z,
                    ((pc.wp.y - pc.view.y) / pc.view.w * 2.0 - 1.0) * p.z + 2.0 * pc.wp.z * p.y / pc.view.w);
    gl_Position = vec4(cxy, min(128.0, p.z * 0.999), p.z);
}
