#version 450
// t_vkn's vertex shader with a uniform block as slice 0 declares one (set 0, binding 1, NVN slot 0: a 64 KiB array of vec4): the colour is its first two vec4 summed
layout(location = 0) in vec4 a_pos;
layout(location = 0) out vec4 v_col;
layout(set = 0, binding = 1, std140) uniform vp_c3 { vec4 c[4096]; } u;
void main() { gl_Position = a_pos; v_col = u.c[0] + u.c[1]; }
