#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 color;
void main() { color = texture(tex, vUv); }
