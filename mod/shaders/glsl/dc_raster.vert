#version 460
// DC GPU raster backend — vertex stage.
// Inputs are native PS1 draw-space coordinates (PGXP sub-pixel floats) plus
// the PGXP per-vertex w. Multiplying the clip position by w makes the GPU
// interpolate uv/color perspective-correct exactly like the CPU fast spans;
// gl_Position.z = DEPTH_K yields depth = DEPTH_K / w — linear in 1/w, the
// same quantity the CPU z-buffer stores (compare GREATER, nearer wins).
// Primitives without PGXP w arrive with w = 1 (affine, depth handled by
// per-draw depth-write state, mirroring zt=0 on the CPU).

layout(location = 0) in vec2 in_pos;      // draw-space x,y (0..1023, 0..511)
layout(location = 1) in vec2 in_uv;       // texel coords (0..255)
layout(location = 2) in vec4 in_color;    // modulation r,g,b in 0..255; a unused
layout(location = 3) in vec4 in_info;     // tpage, clut_x, clut_y, flags
layout(location = 4) in float in_w;       // PGXP w (1.0 = affine)
// 05.10.2026: текстура PNG из mods (второй буфер вершин, gpu_vk_raster.cpp TEXREP): x — номер в массиве repTex (−1 — нет),
// yz — место в картинке замены (0..1)
layout(location = 5) in vec4 in_rep;

layout(push_constant) uniform PC {
    vec4 view;        // x0, y0, w, h of the display area in draw space
    vec4 params;      // pass (0=opaque part,1=blend part), semi_mode, depth_k, unused
    vec4 prevRect;    // fragment stage (previous hi-res frame copy)
    vec4 prevMap;     // fragment stage
    // 17.09.2026: FRAME WARP - the in-between frame (B vblank) re-projects the last frame's triangles for a camera
    // half way to the next one. View space comes back from (x, y, w) exactly as the CPU near clip does it.
    vec4 wq;          // rotation quaternion (x, y, z, w) of the view-space rigid transform
    vec4 wt;          // xyz = translation; w > 0.5 -> warp on
    vec4 wd;          // xyz = offset added to Regina's vertices (F_ACTOR) before the transform
    vec4 wp;          // ox, oy (projection centre + draw offset), H, H * horizontal squeeze
} pc;

// 18.09.2026: ТЕНИ ОТ ЛАМП ИГРЫ. Данные ламп лежат в отдельном блоке (раньше
// были в константах, но 208 байт не даёт добрая половина устройств). Здесь
// нужна только пометка «тени включены»: по ней считается видовая точка.
layout(binding = 4, std140) uniform ShadowUBO {
    mat4 vp[4];
    vec4 par[4];
    vec4 lpos[4];
    vec4 cfg;         // x = число ламп, y = смещение, z = 1/размер карты, w = отладка
    vec4 cfg2;
    vec4 flp;         // 25.09: вспышка выстрела/взрыва — точка в видовом пространстве, w = радиус (0 — нет)
    vec4 flc;         //        цвет × яркость, w = «обтекание»
    vec4 mdir;        // 25.09 вечер: луна (dc_raster.frag)
    vec4 mcol;
    vec4 rfp;         // 25.09 вечер: отражения — плоскость пола в видовом пространстве (xyz нормаль, w = d)
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
    // 20.10.2026: её руки, кисти с оружием и голова между тактами (gpu_vk_raster.cpp arm_warp_make; 25.10: все 15 костей,
    // ячейки 536..663)
    vec4 bq[16];      // кватернион вершин кости в виде кадра A
    vec4 bt[16];      // xyz - перенос, w > 0.5 - есть
} sh;

vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

layout(location = 0)      out vec2 v_uv;
layout(location = 1)      out vec3 v_color;
layout(location = 2) flat out ivec4 v_info;
layout(location = 3) flat out vec2  v_uvp;   // provoking-vertex UV: stable prim identity
layout(location = 4)      out vec3  v_view;  // положение в видовом пространстве (для теней)
layout(location = 5) flat out int   v_rep;   // 05.10: текстура из mods (−1 — нет)
layout(location = 6)      out vec2  v_repuv;
// 12.10.2026: блеск предметов — место точки вдоль диагонали предмета (1..2), 0 — не предмет (pt_renderer.c rl_glint_*)
layout(location = 7)      out float v_glint;

void main() {
    v_rep = int(round(in_rep.x));
    v_repuv = in_rep.yz;
    v_glint = in_rep.w;
    vec2 ndc = ((in_pos - pc.view.xy) / pc.view.zw) * 2.0 - 1.0;
    float w = max(in_w, 1e-6);
    // положение в видовом пространстве: обратная перспектива по PGXP w — та же
    // выкладка, что у перепроецирования кадра B (18.09: нужна теням)
    // 19.10.2026: и свету светящихся текстур (spare.x — их число), и бликам материалов (spare.y): прежде точка была только
    // в кадре с тенью лампы, вспышкой или отражением, и без них свет и блики гасли целиком
    const bool needView = sh.cfg.x > 0.5 || sh.flp.w > 0.0 || sh.rfc.w > 0.5 || sh.spare.x > 0.5 || sh.spare.y > 0.5;
    v_view = (needView && in_w >= 1.5)     // 25.09: и для вспышки; вечер — и отражению
           ? vec3((in_pos.x - pc.wp.x) * w / pc.wp.w, (in_pos.y - pc.wp.y) * w / pc.wp.z, w)
           : vec3(0.0);
    if (pc.wt.w > 0.5 && in_w >= 1.5) {
        // 17.09.2026: frame warp (depth-carrying vertices only; 2D/HUD arrive with w = 1 and stay put)
        vec3 p = vec3((in_pos.x - pc.wp.x) * w / pc.wp.w, (in_pos.y - pc.wp.y) * w / pc.wp.z, w);
        bool actor = (int(round(in_info.w)) & 16) != 0;
        const vec3 pa = p;                                          // her point of frame A
        const bool hturn = actor && sh.hc.w > 0.5 && !(pc.wd.w > 0.5);
        if (actor && pc.wd.w > 0.5) {
            // 20.09.2026: AIMING - she turns with the camera, so her picture of frame A stays where it is
            // (rotating her with the world made the arm and the gun zigzag every other presented frame)
            // 25.10.2026: her bones still go their share of the way to the next pose, relative to her body
            if (in_rep.w < -0.5) {
                const int bi = int(-in_rep.w + 0.5) - 1;
                if (bi >= 0 && bi < 16 && sh.bt[bi].w > 0.5) p = qrot(sh.bq[bi], p) + sh.bt[bi].xyz;
            }
        } else {
            // 19.10.2026: HER OWN TURN between the game's ticks - about her vertical axis by her share of the turn from
            // this tick to the next (walking after the camera she turned back with the world here, then jumped ahead
            // on the tick: the shake while turning the camera; standing she turned in steps of the tick)
            // 20.10.2026: HER ARMS, HANDS (WITH THE GUN) AND HEAD between the game's ticks - the share of the way from this
            // tick's pose to the next one, relative to her body (in_rep.w = -(bone + 1); the arm with the gun strobed while
            // aiming up and down: the camera moved every frame, the pose only every tick)
            if (actor && in_rep.w < -0.5) {
                const int bi = int(-in_rep.w + 0.5) - 1;
                if (bi >= 0 && bi < 16 && sh.bt[bi].w > 0.5) p = qrot(sh.bq[bi], p) + sh.bt[bi].xyz;   // 25.10: all 15 bones
            }
            if (hturn) p = sh.hc.xyz + qrot(sh.hq, p - sh.hc.xyz);
            if (actor) p += pc.wd.xyz;                              // Regina rides with the camera pivot
            p = qrot(pc.wq, p) + pc.wt.xyz;
        }
        // clip space without dividing by the new depth: the GPU clips what went behind the camera
        vec2 cxy = vec2(((pc.wp.x - pc.view.x) / pc.view.z * 2.0 - 1.0) * p.z + 2.0 * pc.wp.w * p.x / pc.view.z,
                        ((pc.wp.y - pc.view.y) / pc.view.w * 2.0 - 1.0) * p.z + 2.0 * pc.wp.z * p.y / pc.view.w);
        float zw = min(128.0, p.z * 0.999);
        zw *= 1.0 + in_color.a * 2e-7;
        if ((int(round(in_info.w)) & 2) != 0) zw *= 1.0002;
        gl_Position = vec4(cxy, zw, p.z);
        v_uv = in_uv;
        v_color = in_color.rgb;
        v_info = ivec4(round(in_info));
        v_uvp = in_uv;
        // 18.09 вечер: видовая точка кадра B отдаётся в ЕГО пространстве (уже
        // повёрнутая). Отдавать её в старом нельзя: интерполируется она по
        // НОВОЙ проекции, и внутри крупных полигонов пола значения разъезжались
        // — по кадру шли чёрные квадраты. Фрагментная ступень вернёт её назад
        // тем же кватернионом: карта теней снята в старом пространстве.
        if (needView) v_view = hturn ? pa : p;   // 19.10: her point of frame A as is (dc_raster.frag shadowViewPoint)
        // 18.09.2026 (вечер): v_view здесь НЕ обнуляется. Кадр B рисует вершины
        // прошлого кадра, и v_view посчитан выше в ЕГО видовом пространстве —
        // ровно там, где снята карта теней. Обнуление гасило тень на каждом
        // втором кадре, и тень мигала ровно при движении камеры.
        return;
    }
    // Depth = K/w (2026-08-16). The previous depth (linear in view-w) was
    // NOT screen-linearly interpolatable: hardware interpolates z/w, and a
    // quantity linear in w interpolated across a polygon with a large depth
    // span lands off its true per-pixel value — a wall quad's interpolated
    // depth could beat a pipe standing in front of it at some camera angles
    // ("труба уходит в стену"), and at distance the error exceeded the
    // separation between surfaces entirely (per-pixel flicker growing with
    // range). K/w IS affine in 1/w, so z_clip = K (constant) divided by
    // w_clip = w interpolates to the EXACT per-pixel K/w everywhere: near
    // (small w) -> depth ~1, far -> ~0, GREATER_OR_EQUAL keeps the nearer
    // surface; truly coplanar decals still share w -> equal depth -> draw
    // order resolves, no z-fight. D32_SFLOAT keeps 1/w precision at range.
    // Geometry nearer than w=K clamps to depth ~1 and resolves by order.
    // PAINTER TIEBREAKER (in_color.a = per-triangle submit order): later
    // primitives get ~2e-7 more depth per step, so coplanar decals drawn
    // after their base surface (opaque wall signs, floor hatches, blood)
    // win the tie exactly like PS1 painter order, while real surface
    // separations stay orders of magnitude larger than the accumulated
    // bias. Semi prims additionally get +0.02% (decals over geometry).
    float zc = min(128.0, w * 0.999);
    // (2026-08-30: tried step 6e-7 w/cap for wall attachments — made the
    // room-name sign sink DEEPER: its wall is drawn later, so a bigger step
    // favors the wall. Reverted; the attachment fix needs prim-dump data.)
    zc *= 1.0 + in_color.a * 2e-7;
    if ((int(round(in_info.w)) & 2) != 0) zc *= 1.0002;
    gl_Position = vec4(ndc * w, zc, w);
    v_uv = in_uv;
    v_color = in_color.rgb;
    v_info = ivec4(round(in_info));
    v_uvp = in_uv;
}
