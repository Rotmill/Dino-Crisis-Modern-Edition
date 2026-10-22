#version 460
// 25.09.2026 вечер: СВЕЧЕНИЕ — сложение с готовым кадром (gpu_vk_raster.cpp fx_glow_post): ближний ореол (половинное
// разрешение) полной силой + дальний (четвертное) долей pc.a.y. Смешение конвейера — сложение, альфа цели не трогается.
// Отладка (pc.a.z, конвейер без смешения): 1 — вместо кадра буфер свечения, 2 — буфер отражения (синее — там ничего не
// отражено: пол там покажет себя как есть).
layout(binding = 0) uniform sampler2D g1;
layout(binding = 1) uniform sampler2D g2;
layout(binding = 2) uniform sampler2D rf;
layout(push_constant) uniform PC { vec4 a; vec4 b; vec4 c; vec4 vv[4]; } pc;   // a: сила, доля дальнего, отладка, доля ореола на самой светящейся текстуре; b: активные области G1 (xy), G2 (zw)
// 28.09 (6): c.x — число полос пелены игры (затемнение перехода, полосы кино); vv[i] — строки от/до (точки цели), множитель, вычитание
layout(location = 0) out vec4 o;
void main() {
    vec2 p = gl_FragCoord.xy;
    vec2 u1 = clamp(p * 0.5, vec2(0.5), pc.b.xy - 0.5) / vec2(textureSize(g1, 0));
    vec2 u2 = clamp(p * 0.25, vec2(0.5), pc.b.zw - 0.5) / vec2(textureSize(g2, 0));
    int dbg = int(pc.a.z + 0.5);
    if (dbg == 2) {
        vec4 r = texture(rf, u1);
        o = vec4(r.rgb + vec3(0.0, 0.0, 0.25) * (1.0 - clamp(r.a, 0.0, 1.0)), 1.0);
        return;
    }
    // 25.09 ночь (3), игрок: «эмиссив уменьши в 3 раза, а свет от светящихся текстур верни как было»: на самой светящейся
    // текстуре (альфа G1 — метка из прохода свечения) — только доля ореола pc.a.w (glow_self), вокруг — полной силой
    vec4 t1 = texture(g1, u1);
    float selfk = mix(1.0, clamp(pc.a.w, 0.0, 1.0), clamp(t1.a, 0.0, 1.0));
    vec3 g = (t1.rgb * pc.a.x + texture(g2, u2).rgb * (pc.a.x * pc.a.y)) * selfk;
    if (dbg == 1) { o = vec4(g, 1.0); return; }
    // 28.09 (6): свечение — под пеленой игры, как весь остальной кадр (не светит сквозь затемнение и полосы кино)
    int nv = int(pc.c.x + 0.5);
    for (int i = 0; i < 4; i++) {
        if (i >= nv) break;
        vec4 V = pc.vv[i];
        if (p.y >= V.x && p.y < V.y) g = max(g * V.z - vec3(V.w), vec3(0.0));
    }
    o = vec4(g, 0.0);
}
