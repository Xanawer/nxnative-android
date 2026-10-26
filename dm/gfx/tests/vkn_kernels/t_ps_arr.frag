#version 450
// t_vkn's pixel shader with a fixed-size sampler array (set 1, binding 1), indexed by a constant (no dynamic-indexing capability): one binding of 4 descriptors
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o;
layout(set = 1, binding = 1) uniform sampler2D t[4];
void main() { o = texture(t[1], v_col.xy); }
