#version 450
// Native Metal stage 3c: the stage 3b square (pixels 32..96 x 16..80 of 128x96), drawn with a constant translucent colour, so
// standard alpha blending (src*a + dst*(1-a)) over the clear colour gives one exactly computable value per pixel.
void main() {
    const vec2 p[6] = vec2[](vec2(-0.5, -2.0 / 3.0), vec2(0.5, -2.0 / 3.0), vec2(-0.5, 2.0 / 3.0),
                             vec2(-0.5, 2.0 / 3.0), vec2(0.5, -2.0 / 3.0), vec2(0.5, 2.0 / 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
