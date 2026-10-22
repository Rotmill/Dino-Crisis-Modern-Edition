#version 460
// DC GPU raster — пост-проходы (25.09.2026, SSAO): один треугольник на всю цель, без вершинного буфера.
// Вьюпорт = активная область кадра, gl_FragCoord.xy во фрагментной ступени — пиксели этой области.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
