// Present (vk_present.c): one triangle over the viewport, which is the picture's rectangle on the swapchain image. Parameters are specialization constants, not push constants
// (P0.7 condition 6: Turnip reads garbage from them), so a change needs another pipeline (vk_present.c keeps a few); it changes with the crop, the origin or the surface's rotation.
// s: the fragment's place in the viewport (0..1, x right, y down in the swapchain image's own frame); t: the same place in the picture as the player sees it, the image rotated back
// by ROT quarter turns clockwise (the surface's preTransform); uv: t in the window texture, U0 / V0 + t * US / VS (crop and, for NVN's lower-left origin, the vertical flip, as nvn_mtl.m's present)
#version 450
layout(constant_id = 0) const float U0 = 0.0;
layout(constant_id = 1) const float V0 = 0.0;
layout(constant_id = 2) const float US = 1.0;
layout(constant_id = 3) const float VS = 1.0;
layout(constant_id = 4) const int ROT = 0;
layout(location = 0) out vec2 uv;
void main() {
    vec2 s = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));   // (0,0) (2,0) (0,2): covers the viewport
    gl_Position = vec4(s * 2.0 - 1.0, 0.0, 1.0);
    vec2 t = ROT == 1 ? vec2(s.y, 1.0 - s.x) : ROT == 2 ? vec2(1.0 - s.x, 1.0 - s.y) : ROT == 3 ? vec2(1.0 - s.y, s.x) : s;
    uv = vec2(U0, V0) + t * vec2(US, VS);
}
