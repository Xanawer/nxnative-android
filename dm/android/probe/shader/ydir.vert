#version 450
// P0.8 T3: one triangle covering clip-space y < 0 only (x in [-1, 1], y in [-1, 0]); wound counter-clockwise with NDC y pointing up:
// (-1, 0) -> (-1, -4) -> (3, 0). No vertex buffer, no push constants.
void main() {
    const vec2 p[3] = vec2[](vec2(-1.0, 0.0), vec2(-1.0, -4.0), vec2(3.0, 0.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
