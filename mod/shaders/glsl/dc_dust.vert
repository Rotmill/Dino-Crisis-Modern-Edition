#version 460
#extension GL_GOOGLE_include_directive : require
// DC GPU raster — ОБЪЁМНЫЙ СВЕТ У ЛАМП: ПЫЛЬ В ЛУЧАХ (27.09.2026). Пылинки — точки в координатах КОМНАТЫ вокруг ламп (у
// каждой лампы свои): медленно плывут, чуть оседают и покачиваются, по кругу внутри куба лампы (на краю шара свет уже нулевой —
// перескок не виден). Светятся светом лампы в своей точке: цвет × (1 − d/R)², тень персонажа (карта лампы), мерцание.
// Без вершинного буфера: 6 вершин на пылинку, номер — gl_VertexIndex. Кадр B — тем же поворотом, что у растра (wq/wt).
#include "dc_vol_common.glsl"
layout(location = 0) out vec3 v_col;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_w;

void main() {
    const int id = gl_VertexIndex / 6, corner = gl_VertexIndex % 6;
    const vec2 CQ[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 cq = CQ[corner];
    gl_Position = vec4(2.0, 2.0, 0.5, 1.0);        // по умолчанию — за краем цели (отброшено)
    v_col = vec3(0.0); v_uv = cq; v_w = 0.0;
    if (sh.vcfg.w < 0.5) return;                     // нет матриц «вид <-> комната»
    const int nl = (pc.par.x > 0.0) ? int(sh.vcfg.x + 0.5) : 0, ng = (pc.par.y > 0.0) ? int(sh.spare.x + 0.5) : 0;
    const int nt = nl + ng;
    if (nt <= 0) return;
    const int li = id % nt;
    vec3 L; float Rh; vec3 col; int layer = -1;
    if (li < nl) { L = sh.vlp[li].xyz; Rh = sh.vlp[li].w * pc.par.z; col = sh.vlc[li].rgb; layer = int(floor(sh.vlc[li].w + 0.5)); }
    else { L = sh.glp[li - nl].xyz; Rh = sh.glp[li - nl].w * pc.par.z; col = sh.glc[li - nl].rgb; }
    if (Rh < 10.0) return;
    const float t = pc.par2.y;
    const vec3 h = hash31(float(id) * 1.618 + 3.1), h2 = hash31(float(id) * 2.39 + 11.7);
    const float Rd = Rh * 0.5;                        // куб пыли — вокруг ядра света (дальше свет слабый, пылинок не видно)
    // скорость, ед./с: плывут во все стороны и чуть оседают (ось Y комнаты — вниз)
    const vec3 vel = (h2 - 0.5) * vec3(36.0, 18.0, 36.0) + vec3(0.0, 9.0, 0.0);
    const vec3 sway = vec3(sin(t * 0.37 + h.x * 6.283), sin(t * 0.29 + h.y * 6.283), sin(t * 0.31 + h.z * 6.283)) * 30.0;
    const vec3 w3 = fract(h + vel * t / (2.0 * Rd));
    const vec3 off = w3 * 2.0 - 1.0;
    const vec3 ef = smoothstep(0.0, 0.08, w3) * (1.0 - smoothstep(0.92, 1.0, w3));   // у грани куба гаснет — перескок не виден
    const float edge = ef.x * ef.y * ef.z;
    const vec3 pA = viewOf(roomOf(L) + off * Rd + sway);
    const float d = length(pA - L);
    float at = 1.0 - d / Rh;
    if (at <= 0.0) return;
    const float q = d / (0.3 * Rh);
    at *= at * (0.3 + 0.7 / (1.0 + q * q));          // ярче у самого источника, как дымка (чуть шире её ядра)
    const float vis = volLayerOk(layer) ? volShadow(layer, pA) : 1.0;
    if (vis <= 0.0) return;
    const float tw = 0.55 + 0.45 * sin(t * (1.3 + 2.2 * h2.y) + h.z * 40.0);   // мерцание: пылинка поворачивается к свету
    const vec3 pB = (pc.wt.w > 0.5) ? qrot(pc.wq, pA) + pc.wt.xyz : pA;
    if (pB.z < 80.0) return;
    const vec2 dsp = vec2(pc.wp.x + pB.x * pc.wp.w / pB.z, pc.wp.y + pB.y * pc.wp.z / pB.z);
    const vec2 pix = (dsp - pc.view.xy) / pc.view.zw * pc.rt.xy;
    const float sz = clamp(pc.par3.y * pc.wp.z / pB.z * (pc.rt.y / pc.view.w), 0.75, 2.2);   // полуразмер, пиксели цели
    gl_Position = vec4((pix + cq * sz) * pc.rt.zw * 2.0 - 1.0, 0.5, 1.0);
    v_col = col * (at * vis * tw * edge * pc.par3.z);
    v_w = pB.z;
}
