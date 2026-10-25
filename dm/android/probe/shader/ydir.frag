#version 450
// P0.8 T3: red = covered; green = the framebuffer row from gl_FragCoord (Vulkan's origin is always the upper left: row r has y = r + 0.5)
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0, (gl_FragCoord.y - 0.5) / 255.0, 0.0, 1.0); }
