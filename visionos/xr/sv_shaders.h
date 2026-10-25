// StrikersVision headset layer: every Metal shader of the composite (sv_xr.mm) and the goal
// celebrations (sv_fx.mm), as one MSL source compiled at run time with newLibraryWithSource. At run
// time on purpose: a compile error then lands in the game log with its line number, where it can be
// read without a Mac; a build-time .metal would only fail inside Xcode.
//
// The celebration shaders are src/platform/shaders/xr_fx*.vert|frag (Quest, GLSL for Vulkan) ported
// to MSL with the same maths. Differences: matrices come from the drawable (Metal clip space,
// reverse-Z), instances are read from a buffer by [[instance_id]] (the draw's buffer offset selects
// the first instance), and push constants are setVertexBytes.
#pragma once

static const char kSvShadersMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------------------------
// Composite: the 3D eye image (Tabletop / Stadium) into the drawable, with an estimated depth
// ---------------------------------------------------------------------------------------------

struct EyeUniforms {
    float4x4 viewProj;     // LOCAL -> clip of this drawable view (the pose the image was drawn for)
    float4x4 invViewProj;  // clip -> LOCAL
    float4 eyePos;         // xyz LOCAL
    float4 tableA;         // table centre xyz (LOCAL, field top), yaw
    float4 tableB;         // table scale (m per game unit), floor y, max distance (m), table plane on
    float4 mode;           // x: over passthrough (alpha = coverage), y: depth mode for the system (0 off, 1 near, 2 full), z: eye depth bound, w: near cutoff (m)
    float4 proj;           // x, y: this view's projection P22, P23; z, w: the eye pass's near, far (m)
};

struct EyeVOut {
    float4 pos [[position]];
    float2 uv;
};

vertex EyeVOut eye_vertex(uint vid [[vertex_id]]) {
    // One triangle over the whole viewport.
    float2 p = float2(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0);
    EyeVOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return o;
}

struct EyeFOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

// The drawable depth (reverse-Z) of the eye pixel at uv, and its distance from the eye in metres
// (1e9 when nothing is there). The eye pass's own exported depth when bound (mode.z), else an
// estimate (the Tabletop field's top, or the floor, or far away).
float eye_pixel_depth(float2 uv, texture2d<float> eyeDepth, constant EyeUniforms& u, thread float& dist) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 a = u.invViewProj * float4(ndc, 1.0, 1.0);
    float4 b = u.invViewProj * float4(ndc, 0.5, 1.0);
    float3 pa = a.xyz / a.w;
    float3 pb = b.xyz / b.w;
    float3 dir = normalize(pb - pa);
    float3 org = u.eyePos.xyz;
    dist = 1e9;
    if (u.mode.z > 0.5) {
        // The eye pass's own depth (Aurora's reverse-Z snapshot: 1 near, 0 far / nothing drawn), its
        // float's four bytes in an RGBA8 texel, read at the nearest texel: filtering depth across an
        // edge would invent a point in between.
        uint2 px = uint2(clamp(uv, 0.0, 0.9999) * float2(eyeDepth.get_width(), eyeDepth.get_height()));
        uint4 bb = uint4(round(eyeDepth.read(px) * 255.0));
        float d = as_type<float>(bb.x | (bb.y << 8) | (bb.z << 16) | (bb.w << 24));
        if (!(d > 0.0) || !(d <= 1.0)) d = 0.0;  // bytes that are no depth (never written): far
        if (d > 0.0) {
            float n = u.proj.z, f = u.proj.w;
            float zv = -n * f / (d * (f - n) + n);  // view-space z of the eye pass (metres, negative)
            float od = clamp((u.proj.x * zv + u.proj.y) / (-zv), 0.0, 1.0);
            // Distance from the eye: unproject the point this depth stands for.
            float4 w = u.invViewProj * float4(ndc, od, 1.0);
            if (od > 0.0 && abs(w.w) > 1e-9) dist = length(w.xyz / w.w - org);
            return od;
        }
        // Nothing drawn here: the estimate below (the stadium modes' sky, the room's floor).
    }
    // The eye passes' own depth stays inside Dawn when not exported; this estimate is what the
    // system's positional reprojection needs (rotation does not depend on it).
    float maxT = u.tableB.z;
    float t = maxT;
    if (abs(dir.y) > 1e-4) {
        if (u.tableB.w > 0.5) {
            float tt = (u.tableA.y - org.y) / dir.y;
            if (tt > 0.0) {
                float3 p = org + dir * tt;
                float dx = p.x - u.tableA.x, dz = p.z - u.tableA.z;
                float cs = cos(u.tableA.w), sn = sin(u.tableA.w), s = max(u.tableB.x, 1e-4);
                float gx = (dx * cs - dz * sn) / s, gy = -(dx * sn + dz * cs) / s;
                if (abs(gx) < 26.0 && abs(gy) < 17.0) t = min(t, tt);
            }
        }
        float tf = (u.tableB.y - org.y) / dir.y;
        if (tf > 0.0) t = min(t, tf);
    }
    dist = t;
    float4 clip = u.viewProj * float4(org + dir * t, 1.0);
    return clamp(clip.z / clip.w, 0.0, 1.0);
}

// The picture and its real depth. The depth is always written here (the effects and the screen test
// against it inside the pass); what the system finally gets is decided by eye_depth_fixup after the
// pass (STRIKERS_XR_DEPTH).
fragment EyeFOut eye_fragment(EyeVOut in [[stage_in]], texture2d<float> eye [[texture(0)]],
                              texture2d<float> eyeDepth [[texture(1)]], sampler smp [[sampler(0)]],
                              constant EyeUniforms& u [[buffer(0)]]) {
    // The eye image holds display-encoded values (the game's picture), read through an sRGB view so
    // they arrive linear; the sRGB drawable encodes them back unchanged.
    float4 c = eye.sample(smp, in.uv);
    EyeFOut o;
    if (u.mode.x > 0.5) {
        if (c.a <= 0.002) discard_fragment();
        o.color = float4(c.rgb * c.a, c.a);  // premultiplied over the room
    } else {
        o.color = float4(c.rgb, 1.0);
    }
    float dist;
    o.depth = eye_pixel_depth(in.uv, eyeDepth, u, dist);
    return o;
}

struct DepthOnlyOut {
    float depth [[depth(any)]];
};

// Near-only depth for the system (STRIKERS_XR_DEPTH=1): after the pass, over a depth cleared to 0
// (far), writes the eye pixel's real depth only where it is nearer than mode.w metres from the eye.
fragment DepthOnlyOut eye_depth_fixup(EyeVOut in [[stage_in]], texture2d<float> eye [[texture(0)]],
                                      texture2d<float> eyeDepth [[texture(1)]], sampler smp [[sampler(0)]],
                                      constant EyeUniforms& u [[buffer(0)]]) {
    if (u.mode.x > 0.5 && eye.sample(smp, in.uv).a <= 0.002) discard_fragment();
    float dist;
    float d = eye_pixel_depth(in.uv, eyeDepth, u, dist);
    if (!(d > 0.0) || !(dist < u.mode.w)) discard_fragment();
    DepthOnlyOut o;
    o.depth = d;
    return o;
}

// ---------------------------------------------------------------------------------------------
// Composite: the flat game picture on a screen (flat or curved), the Tabletop scoreboard, the Super
// Strike meter panel, the replay jumbotron
// ---------------------------------------------------------------------------------------------

struct ScreenUniforms {
    float4x4 viewProj;
    float4x4 model;   // LOCAL from screen (screen +Z faces the viewer, centre at the origin)
    float4 size;      // width, height (m), curve radius (0 = flat), segments
    float4 uvRect;    // u0, v0, u1, v1 of the picture to show
};

struct ScreenVOut {
    float4 pos [[position]];
    float2 uv;
};

vertex ScreenVOut screen_vertex(uint vid [[vertex_id]], constant ScreenUniforms& u [[buffer(0)]]) {
    float segs = max(u.size.w, 1.0);
    float col = float(vid / 2u);
    float row = float(vid & 1u);
    float s = min(col / segs, 1.0);
    float x, z;
    if (u.size.z > 0.0) {
        float phi = (s - 0.5) * u.size.x / u.size.z;
        x = u.size.z * sin(phi);
        z = u.size.z * (1.0 - cos(phi));
    } else {
        x = (s - 0.5) * u.size.x;
        z = 0.0;
    }
    float y = (0.5 - row) * u.size.y;
    ScreenVOut o;
    o.pos = u.viewProj * (u.model * float4(x, y, z, 1.0));
    o.uv = mix(u.uvRect.xy, u.uvRect.zw, float2(s, row));
    return o;
}

fragment float4 screen_fragment(ScreenVOut in [[stage_in]], texture2d<float> pic [[texture(0)]],
                                sampler smp [[sampler(0)]]) {
    // Opaque: the GameCube framebuffer's alpha is not coverage.
    return float4(pic.sample(smp, in.uv).rgb, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Goal celebrations (xr_fx.vert / xr_fx.frag)
// ---------------------------------------------------------------------------------------------

struct FxInstance {
    packed_float4 posSize;  // xyz LOCAL metres, w size (height) in metres
    packed_float4 quat;     // orientation; all zero = face the eye
    uint color1;            // sRGB unorm8x4 (R in the low byte): front / main colour
    uint color2;            // back / detail colour
    packed_float4 params;   // shape id, aspect (width / height), alpha, glow (0..1)
};

struct FxPC {
    float4x4 viewProj;
    float4 camRight;
    float4 camUp;
    float4 camPos;
};

struct FxOut {
    float4 pos [[position]];
    float2 uv;
    float4 c1;
    float4 c2;
    float4 params;
    float shade;
};

static float3 qrot(float4 q, float3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vertex FxOut fx_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                       const device FxInstance* inst [[buffer(0)]], constant FxPC& pc [[buffer(1)]]) {
    FxInstance I = inst[iid];
    float4 posSize = float4(I.posSize);
    float4 q = float4(I.quat);
    float4 params = float4(I.params);
    float2 corner = float2((vid & 1u) != 0u ? 1.0 : -1.0, (vid & 2u) != 0u ? 1.0 : -1.0);
    FxOut o;
    o.uv = corner * 0.5 + 0.5;
    o.c1 = unpack_unorm4x8_to_float(I.color1);
    o.c2 = unpack_unorm4x8_to_float(I.color2);
    o.params = params;
    o.shade = 1.0;
    if (int(params.x + 0.5) == 19) {
        // The game's screen flash across the whole view: the quad is the eye's clip square, far
        // away in reverse-Z (no depth test draws it).
        o.pos = float4(corner, 0.0, 1.0);
        return o;
    }
    float2 local = corner * float2(params.y, 1.0) * 0.5 * posSize.w;
    float3 world;
    if (dot(q, q) < 0.5) {
        world = posSize.xyz + pc.camRight.xyz * local.x + pc.camUp.xyz * local.y;
    } else {
        world = posSize.xyz + qrot(q, float3(local, 0.0));
        float3 n = qrot(q, float3(0.0, 0.0, 1.0));
        o.shade = abs(dot(n, normalize(float3(0.3, 1.0, 0.4))));
    }
    o.pos = pc.viewProj * float4(world, 1.0);
    // NaN / Inf (a bad instance): outside the clip volume (z < 0, reverse-Z), so it is discarded.
    if (!all(isfinite(o.pos))) o.pos = float4(0.0, 0.0, -2.0, 1.0);
    return o;
}

static float dot2(float2 v) { return dot(v, v); }
static float sdCircle(float2 p, float r) { return length(p) - r; }
static float sdBox(float2 p, float2 b) {
    float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
static float sdEllipse(float2 p, float2 r) {
    float k = length(p / r);
    return (k - 1.0) * min(r.x, r.y);
}
static float sdHeart(float2 p) {
    p.x = abs(p.x);
    if (p.y + p.x > 1.0) return sqrt(dot2(p - float2(0.25, 0.75))) - sqrt(2.0) / 4.0;
    return sqrt(min(dot2(p - float2(0.0, 1.0)), dot2(p - 0.5 * max(p.x + p.y, 0.0)))) * sign(p.x - p.y);
}
static float sdStar5(float2 p, float r, float rf) {
    const float2 k1 = float2(0.809016994375, -0.587785252292);
    const float2 k2 = float2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    float2 ba = rf * float2(-k1.y, k1.x) - float2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}
static float3 lin(float3 srgb) { return pow(max(srgb, float3(0.0)), float3(2.2)); }

fragment float4 fx_fragment(FxOut in [[stage_in]], bool front [[front_facing]]) {
    float2 p = in.uv * 2.0 - 1.0;
    int shape = int(in.params.x + 0.5);
    float3 c1 = lin(in.c1.rgb);
    float3 c2 = lin(in.c2.rgb);
    float3 col = c1;
    float d = 1.0;
    float glow = in.params.w;
    float shade = 0.55 + 0.45 * in.shade;
    if (shape == 0) {
        d = sdBox(p, float2(0.92, 0.92)) - 0.06;
        col = front ? c1 : c2;
    } else if (shape == 15) {
        d = sdBox(p, float2(0.92, 0.92)) - 0.06;
        col = front ? c1 : c2;
        shade = 0.45 + 0.35 * in.shade + 1.6 * pow(in.shade, 14.0);
        glow = 0.25 * pow(in.shade, 14.0);
    } else if (shape == 17) {
        float r = length(p);
        d = abs(r - 0.78) - 0.16;
        col = abs(r - 0.78) > 0.1 ? float3(1.0) : c1;
        shade = 1.0;
    } else if (shape == 18) {
        float2 q = float2(abs(p.x), -p.y);
        d = max(q.x * 0.866 + q.y * 0.5 - 0.45, -q.y - 0.75);
        float inner = max(q.x * 0.866 + q.y * 0.5 - 0.27, -q.y - 0.55);
        col = inner < 0.0 ? c1 : float3(1.0);
        shade = 1.0;
    } else if (shape == 19) {
        float a = in.params.z;
        return float4(c1 * a, a);
    } else if (shape == 16) {
        float a = in.params.z * (1.0 - smoothstep(0.6, 1.0, length(p)) * 0.3);
        return float4(c1 * a, a * (1.0 - glow));
    } else if (shape == 1) {
        float r = length(p);
        d = r - 0.95;
        col = r > 0.72 ? c2 : c1;
        if (abs(p.x) < 0.09 && abs(p.y) < 0.42) col = c2;
    } else if (shape == 2) {
        float2 q = float2(p.x, -p.y) * 1.05 + float2(0.0, 0.55);
        d = sdHeart(q) * 0.9;
        col = mix(c1, c2, smoothstep(0.2, -0.6, p.x + p.y) * 0.35);
    } else if (shape == 3) {
        d = sdStar5(p, 0.95, 0.45);
        col = mix(c1, c2, smoothstep(0.0, 0.8, length(p)) * 0.3);
    } else if (shape == 4) {
        d = sdEllipse(p, float2(0.42, 0.95));
        col = mix(c2, c1, smoothstep(-0.9, -0.3, p.y));
    } else if (shape == 5) {
        float cap = max(sdEllipse(p - float2(0.0, 0.05), float2(0.95, 0.8)), -(p.y + 0.05));
        float stem = sdBox(p - float2(0.0, -0.45), float2(0.38, 0.42)) - 0.08;
        d = min(cap, stem);
        if (cap < stem) {
            col = c1;
            if (sdCircle(p - float2(0.0, 0.5), 0.22) < 0.0 || sdCircle(p - float2(-0.55, 0.22), 0.16) < 0.0 ||
                sdCircle(p - float2(0.55, 0.22), 0.16) < 0.0) col = c2;
        } else {
            col = float3(0.93, 0.86, 0.72);
        }
    } else if (shape == 6) {
        float2 q = p;
        q.x *= 1.0 + 0.18 * q.y;
        d = sdEllipse(q, float2(0.72, 0.95));
        col = c1;
        if (sdCircle(p - float2(-0.25, 0.25), 0.2) < 0.0 || sdCircle(p - float2(0.3, -0.2), 0.24) < 0.0 ||
            sdCircle(p - float2(-0.15, -0.55), 0.14) < 0.0) col = c2;
    } else if (shape == 7) {
        float outer = sdCircle(p - float2(0.0, 0.45), 1.05);
        float inner = sdCircle(p - float2(0.0, 0.85), 1.05);
        d = max(max(outer, -inner), -p.y - 0.75);
        col = abs(p.x) > 0.72 ? c2 : c1;
    } else if (shape == 8) {
        d = sdEllipse(p, float2(0.95, 0.75));
        float inner = sdEllipse(p, float2(0.72, 0.52));
        col = inner < 0.0 ? c1 : c2;
        if (inner < 0.0 && (abs(p.x) < 0.05 || abs(p.y) < 0.05)) col = c1 * 0.6;
    } else if (shape == 9) {
        float head = sdBox(p - float2(0.0, 0.55), float2(0.82, 0.3)) - 0.05;
        float handle = sdBox(p - float2(0.0, -0.3), float2(0.13, 0.62));
        d = min(head, handle);
        col = head < handle ? c2 : c1;
    } else if (shape == 10) {
        float bloom = sdCircle(p - float2(0.0, 0.32), 0.6);
        float stem = sdBox(p - float2(0.0, -0.5), float2(0.07, 0.45));
        float leaf = sdEllipse(p - float2(0.22, -0.45), float2(0.22, 0.1));
        d = min(bloom, min(stem, leaf));
        if (bloom <= min(stem, leaf)) {
            float a = atan2(p.y - 0.32, p.x);
            float r = length(p - float2(0.0, 0.32));
            col = c1 * (0.75 + 0.25 * sin(a * 2.0 + r * 22.0));
        } else {
            col = c2;
        }
    } else if (shape == 11) {
        float bulb = sdEllipse(p - float2(0.0, -0.15), float2(0.85, 0.75));
        float tip = sdBox(p - float2(0.0, 0.6), float2(0.1, 0.3));
        d = min(bulb, tip);
        col = c1;
        if (bulb < 0.0 && abs(fract((p.x + 1.0) * 2.5) - 0.5) < 0.06) col = c2;
    } else if (shape == 12) {
        float2 q = float2(abs(p.x), p.y);
        float loopL = sdEllipse(q - float2(0.5, 0.0), float2(0.45, 0.6));
        float knot = sdCircle(p, 0.22);
        d = min(loopL, knot);
        col = knot < 0.0 ? c2 : c1;
    } else {
        // spark: a soft glowing dot
        float r2 = dot(p, p);
        float a = exp(-r2 * 5.0) * (1.0 - smoothstep(0.85, 1.0, r2));
        float3 sc = mix(c2, c1, exp(-r2 * 14.0));
        float alpha = a * in.params.z;
        return float4(sc * alpha * 1.6, alpha * (1.0 - glow));
    }
    float aa = max(fwidth(d), 1e-4);
    float a = clamp(0.5 - d / aa, 0.0, 1.0) * in.params.z;
    float3 rgb = col * shade;
    return float4(rgb * a, a * (1.0 - glow));
}

// ---------------------------------------------------------------------------------------------
// Goal celebrations: the 3D pieces (xr_fx_mesh.vert / xr_fx_mesh.frag)
// ---------------------------------------------------------------------------------------------

struct MeshVertex {
    packed_float3 pos;
    packed_float3 wid;   // added times the aspect (paper width)
    packed_float3 nrm;
    float mat;           // base colour slot
    float aux;           // detail parameter
};

struct MeshOut {
    float4 pos [[position]];
    float3 local;
    float3 normal;
    float3 view;
    float2 matAux;
    uint inst [[flat]];  // the instance (relative to the draw's buffer offset): the fragment reads FxInstance
};

vertex MeshOut mesh_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                           const device MeshVertex* verts [[buffer(0)]], const device FxInstance* inst [[buffer(1)]],
                           constant FxPC& pc [[buffer(2)]]) {
    MeshVertex v = verts[vid];
    FxInstance I = inst[iid];
    float4 posSize = float4(I.posSize);
    float4 params = float4(I.params);
    float3 local = float3(v.pos) + float3(v.wid) * params.y;
    float4 q = normalize(float4(I.quat));
    float3 world = posSize.xyz + qrot(q, local * posSize.w);
    MeshOut o;
    o.pos = pc.viewProj * float4(world, 1.0);
    o.local = local;
    o.normal = qrot(q, float3(v.nrm));
    o.view = pc.camPos.xyz - world;
    o.matAux = float2(v.mat, v.aux);
    o.inst = iid;
    // NaN / Inf (a bad instance): outside the clip volume (z < 0, reverse-Z), so it is discarded.
    if (!all(isfinite(o.pos))) o.pos = float4(0.0, 0.0, -2.0, 1.0);
    return o;
}

static bool inEllipse(float2 p, float2 c, float2 r) {
    float2 d = (p - c) / r;
    return dot(d, d) < 1.0;
}
static bool spotAt(float3 d, float3 s, float size) { return dot(d, normalize(s)) > size; }

fragment float4 mesh_fragment(MeshOut in [[stage_in]], const device FxInstance* insts [[buffer(1)]]) {
    const FxInstance I = insts[in.inst];
    const float4 iparams = float4(I.params);
    const float4 ic1 = unpack_unorm4x8_to_float(I.color1);
    const float4 ic2 = unpack_unorm4x8_to_float(I.color2);
    const float3 kKey = float3(0.2756, 0.9187, 0.3675);
    const float3 kFill = float3(-0.7194, 0.2998, -0.5995);
    const float kPi = 3.14159265;
    int shape = int(iparams.x + 0.5);
    int mat = int(in.matAux.x + 0.5);
    float aux = in.matAux.y;
    float3 c1 = lin(ic1.rgb);
    float3 c2 = lin(ic2.rgb);
    float3 N = normalize(in.normal);
    float3 V = normalize(in.view);
    float3 p = in.local;
    float3 base = mat == 0 ? c1 : (mat == 1 ? c2 : (mat == 2 ? float3(0.86, 0.76, 0.58) : float3(0.05, 0.035, 0.025)));
    float spec = 0.4;
    float shine = 40.0;
    bool metal = false;
    float glow = 0.0;
    if (shape == 0 || shape == 15) {
        bool frontSide = dot(N, V) >= 0.0;
        base = frontSide ? c1 : c2;
        if (!frontSide) N = -N;
        spec = 0.12;
        shine = 16.0;
        metal = shape == 15;
    } else if (shape == 1) {
        metal = true;
    } else if (shape == 2) {
        spec = 0.75;
        shine = 60.0;
    } else if (shape == 3) {
        spec = 0.8;
        shine = 70.0;
        glow = 0.18;
        if (p.z > 0.05 && (inEllipse(p.xy, float2(-0.07, 0.04), float2(0.028, 0.07)) ||
                           inEllipse(p.xy, float2(0.07, 0.04), float2(0.028, 0.07)))) {
            base = float3(0.015);
            glow = 0.0;
        }
    } else if (shape == 4) {
        base = mix(c2, c1, smoothstep(-0.44, -0.22, p.y));
        spec = 0.2;
        shine = 20.0;
    } else if (shape == 5) {
        if (mat == 0) {
            float3 d = normalize((p - float3(0.0, -0.06, 0.0)) / float3(0.5, 0.48, 0.5));
            bool spot = spotAt(d, float3(0.0, 1.0, 0.0), 0.93);
            for (int i = 0; i < 5; ++i) {
                float a = 0.31 + float(i) * 1.2566;
                spot = spot || spotAt(d, float3(0.866 * cos(a), 0.45, 0.866 * sin(a)), 0.95);
            }
            if (spot) base = c2;
            spec = 0.65;
            shine = 50.0;
        } else {
            if (aux > 0.5) base *= 0.78;
            else if (p.z > 0.08 && (inEllipse(p.xy, float2(-0.07, -0.24), float2(0.028, 0.065)) ||
                                    inEllipse(p.xy, float2(0.07, -0.24), float2(0.028, 0.065)))) {
                base = float3(0.015);
            }
            spec = 0.15;
            shine = 20.0;
        }
    } else if (shape == 6) {
        float3 d = normalize(p / float3(0.36, 0.5, 0.36));
        if (spotAt(d, float3(0.0, 0.85, 0.52), 0.94) || spotAt(d, float3(0.8, 0.3, 0.52), 0.93) ||
            spotAt(d, float3(-0.7, 0.2, -0.68), 0.93) || spotAt(d, float3(0.3, -0.5, -0.8), 0.94) ||
            spotAt(d, float3(-0.5, -0.6, 0.62), 0.93) || spotAt(d, float3(0.9, -0.35, -0.25), 0.95) ||
            spotAt(d, float3(-0.95, 0.25, 0.2), 0.95) || spotAt(d, float3(0.1, -0.95, 0.1), 0.95)) {
            base = c2;
        }
        spec = 0.5;
        shine = 45.0;
    } else if (shape == 7) {
        if (aux < 0.06 || aux > 0.94) base = c2;
        spec = 0.35;
        shine = 30.0;
    } else if (shape == 8) {
        if (mat == 0) {
            float3 d = normalize(float3(p.x / 0.44, p.y / 0.34, p.z / 0.44));
            float pol = acos(clamp(d.y, -1.0, 1.0));
            float seg = atan2(d.z, d.x) / (kPi / 3.0);
            float edge = abs(seg - floor(seg + 0.5)) * (kPi / 3.0) * sin(pol);
            if (abs(pol - 0.62) < 0.05 || (pol > 0.62 && edge < 0.04)) base = c2;
        }
        spec = 0.7;
        shine = 60.0;
    } else if (shape == 9) {
        if (mat == 1) metal = true;
        else { spec = 0.2; shine = 18.0; }
    } else if (shape == 10) {
        if (mat == 0) {
            float r = length(p.xz);
            float swirl = sin(atan2(p.z, p.x) + r * 48.0);
            float top = smoothstep(0.7, 0.85, aux);
            base *= mix(1.0, 0.62 + 0.38 * smoothstep(-0.3, 0.6, swirl), top);
            spec = 0.3;
            shine = 25.0;
        } else {
            spec = 0.25;
            shine = 25.0;
        }
    } else if (shape == 11) {
        base = mix(c2 * 0.85, c1, smoothstep(0.05, 0.35, aux));
        if (p.y < -0.42) base *= 0.75;
        spec = 0.25;
        shine = 22.0;
    } else if (shape == 12) {
        spec = 0.45;
        shine = 25.0;
    }
    float key = max(dot(N, kKey), 0.0);
    float fill = max(dot(N, kFill), 0.0);
    float nv = max(dot(N, V), 0.0);
    float rim = pow(1.0 - nv, 3.0);
    float3 col;
    if (metal) {
        float3 R = reflect(-V, N);
        float sky = smoothstep(-0.35, 0.6, R.y);
        float3 env = mix(float3(0.08), float3(1.3), sky) + float3(3.5) * pow(max(dot(R, kKey), 0.0), 90.0);
        col = base * (0.15 + 0.85 * env * (0.45 + 0.55 * (0.5 + 0.5 * dot(N, kKey)))) + base * 0.3 * rim;
    } else {
        float3 H = normalize(kKey + V);
        float hl = pow(max(dot(N, H), 0.0), shine) * spec;
        col = base * (0.3 + 0.85 * key + 0.25 * fill) + float3(hl) + rim * 0.22 * mix(base, float3(1.0), 0.5);
    }
    col += base * glow;
    float a = iparams.z;
    return float4(col * a, a);
}

// ---------------------------------------------------------------------------------------------
// Goal celebrations: depth-only stand-ins (the Tabletop field box) so confetti behind it is hidden
// ---------------------------------------------------------------------------------------------

vertex float4 occluder_vertex(uint vid [[vertex_id]], const device packed_float3* pos [[buffer(0)]],
                              constant FxPC& pc [[buffer(1)]]) {
    return pc.viewProj * float4(float3(pos[vid]), 1.0);
}

fragment float4 occluder_fragment() { return float4(0.0); }
)MSL";
