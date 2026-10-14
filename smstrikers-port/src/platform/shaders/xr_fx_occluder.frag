// strikers-quest: depth only; the pipeline's colour write mask is 0.
#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0);
}
