#version 460
// DC GPU raster — ОБЪЁМНЫЙ СВЕТ У ЛАМП: ПЫЛЬ В ЛУЧАХ (27.09.2026), пиксель пылинки: круглая, мягкий край; за стеной или
// предметом (глубина из прохода 1 SSAO) и поверх интерфейса — не видна. Сложение с кадром, альфа = 1.
layout(binding = 0) uniform sampler2D zTex;
layout(location = 0) in vec3 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_w;
layout(location = 0) out vec4 out_color;

void main() {
    const float zc = texelFetch(zTex, ivec2(gl_FragCoord.xy), 0).r;
    if (zc < 0.0) discard;                              // интерфейс поверх
    if (zc > 0.0 && v_w > zc + 15.0) discard;           // заслонена
    const float r = length(v_uv);
    if (r >= 1.0) discard;
    float a = 1.0 - r;
    out_color = vec4(v_col * (a * a), 1.0);
}
