// strikers-quest: depth-only stand-ins (the Tabletop field box, the screen) drawn before the effects
// so confetti behind them is hidden. Same push constants as xr_fx.vert.
#version 450

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 camRight;
    vec4 camUp;
} pc;

layout(location = 0) in vec3 inPos;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
}
