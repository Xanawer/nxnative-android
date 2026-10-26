#version 450
// t_vkn's vertex shader writing two vec4s, at locations 0 (the colour) and 1 (zero): the locations of a vec4[2], not its variable
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 0) out vec4 v0;
layout(location = 1) out vec4 v1;
void main() { gl_Position = a_pos; v0 = a_col; v1 = vec4(0); }
