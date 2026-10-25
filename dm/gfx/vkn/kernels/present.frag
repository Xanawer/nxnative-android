// Present (vk_present.c): the window texture, opaque (a game's alpha would let the compositor blend the window with what is behind it). The coordinate is clamped to the texel centres of the
// crop rectangle (X0 Y0 - X1 Y1, specialization constants as the vertex stage's), as nvn_mtl.m's present does: bilinear filtering never reads the texels outside a crop
#version 450
layout(constant_id = 5) const float X0 = 0.0;
layout(constant_id = 6) const float Y0 = 0.0;
layout(constant_id = 7) const float X1 = 1.0;
layout(constant_id = 8) const float Y1 = 1.0;
layout(set = 0, binding = 0) uniform sampler2D img;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(img, clamp(uv, vec2(X0, Y0), vec2(X1, Y1))).rgb, 1.0); }
