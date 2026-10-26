#version 450
// t_vkn's pixel shader reading combined slot 0 (set 1, binding 1) as unsigned integers (a usampler2D: a stencil view, or the unsigned dummy), sampled with texture() at the colour's xy (a
// sampled access, so the sampler's filters apply): the red component over 255 is the output's red
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o;
layout(set = 1, binding = 1) uniform usampler2D t0;
void main() { o = vec4(float(texture(t0, v_col.xy).r) / 255.0, 0, 0, 1); }
