#version 450
// t_vkn's pixel shader with signed integers: a flat ivec4 input at location 1 and an isampler2D (set 1, binding 1, combined slot 0). Red is the colour's; green 1 where the input is
// (1, 0, 0, 0), as t_vs_int writes it from a zero input, and the texel (the signed dummy) is zero
layout(location = 0) in vec4 v_col;
layout(location = 1) flat in ivec4 v_i;
layout(location = 0) out vec4 o;
layout(set = 1, binding = 1) uniform isampler2D t0;
void main() { ivec4 t = texture(t0, v_col.xy); o = vec4(v_col.r, v_i == ivec4(1, 0, 0, 0) && t == ivec4(0) ? 1.0 : 0.0, 0, 1); }
