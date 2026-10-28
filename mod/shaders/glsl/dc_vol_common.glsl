// DC GPU raster — ОБЪЁМНЫЙ СВЕТ У ЛАМП (27.09.2026): общее для dc_vol.frag, dc_vol_comp.frag, dc_dust.vert/frag.
// Блок ShadowUBO — тот же, что у растра (dc_raster.frag), плюс поля 256..351: лампы игры для дымки (pt_renderer.c
// dc_vol_frame, кадр A) и матрицы «вид -> комната» / «комната -> вид» (пыль и клубы висят в комнате, а не на экране).
layout(binding = 1) uniform sampler2DArray shadowMap;
layout(binding = 2, std140) uniform ShadowUBO {
    mat4 vp[4];       // видовая точка -> отсечённое пространство лампы (карта теней персонажей)
    vec4 par[4];      // y/z — A и B (глубина карты -> расстояние)
    vec4 lpos[4];
    vec4 cfg;         // x — число ламп с картой
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
    vec4 spare;       // x — число источников света от текстур «светится»
    vec4 glp[8];      // их точки в видовом пространстве кадра A, w = радиус
    vec4 glc[8];      // цвет × сила
    vec4 msp;
    vec4 msp2;
    vec4 spp[4];
    vec4 spc[4];
    vec4 senv;
    vec4 msp3;
    vec4 vcfg;        // 27.09: x — ламп игры для дымки, w > 0.5 — матрицы годны
    vec4 v2r[3];      //        вид (кадр A) -> комната: строки xyz + перенос в w
    vec4 r2v[3];      //        комната -> вид (кадр A)
    vec4 vspare;
    vec4 vlp[8];      //        лампы игры: точка в видовом пространстве кадра A, w = радиус из таблицы игры
    vec4 vlc[8];      //        цвет (1 = белый игры), w — слой карты теней (-1 — нет)
    vec4 vpad[22];    // 352..439 — тени от ламп-текстур, тени без зерна, металл (их читает dc_raster.frag)
    vec4 veilV[4];    // 28.09 (6): пелена игры поверх мира (vspare.x — сколько): строки от/до (точки цели), множитель, вычитание
    vec4 vglint;      // 456..459 - блеск предметов (читает dc_raster.frag), здесь только ради раскладки
    vec4 vrsv460;     // 460..463 - запас
    vec4 glp2[8];     // 464..495 - источники «светится» 9..16 (как у растра: точка вида кадра A, w = радиус; сколько - spare.w)
    vec4 glc2[8];     // 496..527 - их цвет × сила
    vec4 vrsv528[803];// 528..3739 - её доворот, скелет, пары кусков (dc_raster.vert) - здесь не читается, только ради раскладки
    vec4 glp3[16];    // 28.10: источники 17..32 (gpu_vk_raster.cpp SH_UBO_GL3)
    vec4 glc3[16];
    vec4 ghz[8];      // 28.10: доля дымки места 0..31 (1 - полная, 0 - нет)
} sh;

// 28.10.2026 (игрок, слот 49: «идешь вперед и прям видно как в глазах включаются эти дымка от света»): дымка - у всех мест
// света «светится» (до 32), каждое по своей доле (gpu_vk_raster.cpp gl_room_out: лампа входит в место и уходит плавно).
// Прежние пути пишут долю 1 местам 0..7 и 0 прочим - дымка у первых 8, как было.
vec4 glP(int i) { return (i < 8) ? sh.glp[min(i, 7)] : (i < 16) ? sh.glp2[clamp(i - 8, 0, 7)] : sh.glp3[clamp(i - 16, 0, 15)]; }
vec4 glC(int i) { return (i < 8) ? sh.glc[min(i, 7)] : (i < 16) ? sh.glc2[clamp(i - 8, 0, 7)] : sh.glc3[clamp(i - 16, 0, 15)]; }
float glHazeW(int i) { return clamp(sh.ghz[clamp(i >> 2, 0, 7)][i & 3], 0.0, 1.0); }

layout(push_constant) uniform PC {
    vec4 view;   // x0, y0, w, h — область дисплея в draw-space
    vec4 wp;     // ox, oy (центр проекции + сдвиг области рисования), H, H·сжатие по горизонтали
    vec4 rt;     // активная область цели полного разрешения: ширина, высота, 1/ширина, 1/высота
    vec4 wq;     // кадр B: кватернион поворота кадра A -> B
    vec4 wt;     //         перенос; w > 0.5 — кадр B
    vec4 par;    // x — сила дымки ламп игры, y — текстур «светится», z — доля радиуса, w — рассеяние вперёд g
    vec4 par2;   // x — клубы (неровность воздуха), y — время, с, z — тени персонажей в дымке, w — шагов на лампу
    vec4 par3;   // x — показать точки ламп, y — размер пылинки (ед.), z — яркость пыли, w — пылинок
} pc;

vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
vec3 roomOf(vec3 p) {
    return vec3(dot(sh.v2r[0].xyz, p) + sh.v2r[0].w, dot(sh.v2r[1].xyz, p) + sh.v2r[1].w, dot(sh.v2r[2].xyz, p) + sh.v2r[2].w);
}
vec3 viewOf(vec3 r) {
    return vec3(dot(sh.r2v[0].xyz, r) + sh.r2v[0].w, dot(sh.r2v[1].xyz, r) + sh.r2v[1].w, dot(sh.r2v[2].xyz, r) + sh.r2v[2].w);
}
// точка S (видовое пространство кадра A) освещена лампой со слоем карты layer? Карта — только персонажи (dc_shadow.vert)
float volShadow(int layer, vec3 S) {
    vec4 lp = sh.vp[layer] * vec4(S, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 nd = lp.xyz / lp.w;
    vec2 uv = nd.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || nd.z <= 0.0 || nd.z >= 1.0) return 1.0;
    float d = textureLod(shadowMap, vec3(uv, float(layer)), 0.0).r;
    if (d >= 1.0) return 1.0;
    float A = sh.par[layer].y, B = sh.par[layer].z;
    float z = B / max(A - d, 1e-6), zr = B / max(A - nd.z, 1e-6);
    return (z < zr - 25.0) ? 0.0 : 1.0;
}
bool volLayerOk(int layer) { return layer >= 0 && layer < int(sh.cfg.x + 0.5) && pc.par2.z > 0.5; }
float hash13(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
vec3 hash31(float n) { vec3 p = fract(vec3(n) * vec3(0.1031, 0.1030, 0.0973)); p += dot(p, p.yzx + 33.33); return fract((p.xxy + p.yzz) * p.zyx); }
