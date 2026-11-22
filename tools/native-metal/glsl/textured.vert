#version 450
// Native Metal stage 3b: a 64x64 square at pixels (32..96, 16..80) of a 128x96 target, uv mapping it 1:1 onto a 64x64 texture.
layout(location = 0) out vec2 vUv;
void main() {
    const vec2 p[6] = vec2[](vec2(-0.5, -2.0 / 3.0), vec2(0.5, -2.0 / 3.0), vec2(-0.5, 2.0 / 3.0),
                             vec2(-0.5, 2.0 / 3.0), vec2(0.5, -2.0 / 3.0), vec2(0.5, 2.0 / 3.0));
    const vec2 t[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
    vUv = t[gl_VertexIndex];
}
