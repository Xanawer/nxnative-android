#version 450
// t_vkn's vertex shader: position and colour from two RGBA32F attributes, as slice 0's modules lay a vertex shader out (inputs by location, outputs by location)
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 0) out vec4 v_col;
void main() { gl_Position = a_pos; v_col = a_col; }
