// strikers-quest: goal celebration pieces as small 3D meshes (xr_fx_mesh.h), one instance per piece
// with the same per-instance data as the quads (xr_fx.vert). Compile with tools/compile-xr-shaders.ps1.
#version 450

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 camRight;
    vec4 camUp;
    vec4 camPos;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inWid;      // added times the aspect (paper width)
layout(location = 2) in vec3 inNrm;
layout(location = 3) in vec2 inMatAux;   // base colour slot, detail parameter
layout(location = 4) in vec4 inPosSize;  // xyz LOCAL-space meters, w size in meters
layout(location = 5) in vec4 inQuat;
layout(location = 6) in vec4 inColor1;
layout(location = 7) in vec4 inColor2;
layout(location = 8) in vec4 inParams;   // shape id, aspect, alpha, glow

layout(location = 0) out vec3 vLocal;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vView;
layout(location = 3) out vec4 vColor1;
layout(location = 4) out vec4 vColor2;
layout(location = 5) out vec4 vParams;
layout(location = 6) out vec2 vMatAux;

vec3 qrot(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    vec3 local = inPos + inWid * inParams.y;
    vec4 q = normalize(inQuat);
    vec3 world = inPosSize.xyz + qrot(q, local * inPosSize.w);
    gl_Position = pc.viewProj * vec4(world, 1.0);
    vLocal = local;
    vNormal = qrot(q, inNrm);
    vView = pc.camPos.xyz - world;
    vColor1 = inColor1;
    vColor2 = inColor2;
    vParams = inParams;
    vMatAux = inMatAux;
}
