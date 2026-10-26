#version 450
// t_vkn's pixel shader: the interpolated colour
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o;
void main() { o = v_col; }
