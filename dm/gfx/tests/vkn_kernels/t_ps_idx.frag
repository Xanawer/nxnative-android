#version 450
// t_vkn's pixel shader whose one output is at location 0, Index 1 (dual-source blending's second colour): no Index 0 output for alpha to coverage to take
layout(location = 0) in vec4 v_col;
layout(location = 0, index = 1) out vec4 o;
void main() { o = v_col; }
