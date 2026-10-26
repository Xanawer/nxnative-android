#version 450
// t_vkn's pixel shader reading an array, vec4 v[2] at location 0 (locations 0 and 1): their sum
layout(location = 0) in vec4 v[2];
layout(location = 0) out vec4 o;
void main() { o = v[0] + v[1]; }
