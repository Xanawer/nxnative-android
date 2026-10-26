#version 450
// t_vkn's vertex shader with signed integers: an ivec4 input at location 2 that nothing feeds (the zero buffer, as R32G32B32A32_SINT) and a flat ivec4 output at location 1 beside the colour
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in ivec4 a_i;
layout(location = 0) out vec4 v_col;
layout(location = 1) flat out ivec4 v_i;
void main() { gl_Position = a_pos; v_col = a_col; v_i = a_i + ivec4(1, 0, 0, 0); }
