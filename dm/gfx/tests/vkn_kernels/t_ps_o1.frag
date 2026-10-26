#version 450
// t_vkn's pixel shader writing colour 1 only (no location 0: alpha to coverage has no alpha to take)
layout(location = 0) in vec4 v_col;
layout(location = 1) out vec4 o1;
void main() { o1 = v_col; }
