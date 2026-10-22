#version 460
// DC GPU raster — background fragment: sample the VRAM display region
// (dispX,dispY,dispW,dispH in pc.view) as the base image, 15bpp direct.
layout(binding = 0) uniform usampler2D vramTex;
layout(location = 0) in vec2 v_uv;
layout(push_constant) uniform PC { vec4 view; vec4 params; } pc;
layout(location = 0) out vec4 out_color;
void main() {
    // outside the display region (only reachable when the vertex stage
    // squeezes the triangle): leave the cleared black — the side bars
    if (v_uv.x > 1.0 || v_uv.y > 1.0) discard;
    int x = int(pc.view.x + v_uv.x * pc.view.z);
    int y = int(pc.view.y + v_uv.y * pc.view.w);
    uint t = texelFetch(vramTex, ivec2(x & 1023, y & 511), 0).r;
    vec3 c = vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u)) / 31.0;
    out_color = vec4(c, 1.0);
}
