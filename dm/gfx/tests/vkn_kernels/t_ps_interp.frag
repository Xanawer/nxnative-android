#version 450
// t_vkn's pixel shader with interpolateAtCentroid (capability InterpolationFunction: Vulkan's sampleRateShading feature)
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o;
void main() { o = interpolateAtCentroid(v_col); }
