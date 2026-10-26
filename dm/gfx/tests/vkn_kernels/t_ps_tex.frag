#version 450
// t_vkn's pixel shader with two combined texture slots (set 1, bindings 1 and 2: NVN slots 0 and 1), sampled at the colour's xy and summed: slot 1 left unbound reads the dummy's zeros
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o;
layout(set = 1, binding = 1) uniform sampler2D t0;
layout(set = 1, binding = 2) uniform sampler2D t1;
void main() { o = texture(t0, v_col.xy) + texture(t1, v_col.xy); }
