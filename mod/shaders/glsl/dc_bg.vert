#version 460
// DC GPU raster — fullscreen background pass. Emits a screen-covering
// triangle (no vertex buffer) so the fragment stage can blit the current
// VRAM display region as the base image. This carries uploaded content the
// primitive stream never draws: pre-rendered menu backgrounds, HUD image
// panels, anything the game puts into VRAM by CPU->VRAM transfer. The prim
// stream then draws on top in painter order.
//
// pc.params.x > 0 = horizontal squeeze about the centre (flat 4:3 screens on
// the wide canvas: the 2D primitives are squeezed to the middle 3/4, so the
// background must land there too — otherwise an MDEC movie or a menu that
// arrives as VRAM uploads shows twice, once squeezed and once not).
layout(location = 0) out vec2 v_uv;
layout(push_constant) uniform PC { vec4 view; vec4 params; } pc;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;                                  // 0..2; visible region 0..1
    float sq = pc.params.x > 0.0 ? pc.params.x : 1.0;
    gl_Position = vec4((p.x * 2.0 - 1.0) * sq, p.y * 2.0 - 1.0, 0.0, 1.0);
}
