#version 450
// t_vkn's vertex shader writing an array, vec4 v[2] at location 0 (locations 0 and 1): the colour and zero
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 0) out vec4 v[2];
void main() { gl_Position = a_pos; v[0] = a_col; v[1] = vec4(0); }
