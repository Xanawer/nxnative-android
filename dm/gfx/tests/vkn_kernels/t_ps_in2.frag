#version 450
// t_vkn's pixel shader reading location 2 beside 0 (the test vertex shaders write location 0 only: the pair does not match)
layout(location = 0) in vec4 v_col;
layout(location = 2) in vec4 v_extra;
layout(location = 0) out vec4 o;
void main() { o = v_col + v_extra; }
