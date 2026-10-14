// strikers-quest: goal celebration pieces as 3D meshes, lit: a key light from above, a fill, a
// highlight and a rim, mirror-like for gold and foil. Detail (spots, eyes, plates, swirls) is
// painted from the piece's own coordinates. Shape ids match xr_fx.cpp's Shape enum; output is
// premultiplied.
#version 450

layout(location = 0) in vec3 vLocal;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vView;
layout(location = 3) in vec4 vColor1;
layout(location = 4) in vec4 vColor2;
layout(location = 5) in vec4 vParams;
layout(location = 6) in vec2 vMatAux;

layout(location = 0) out vec4 outColor;

const vec3 kKey = vec3(0.2756, 0.9187, 0.3675);    // normalize(0.3, 1.0, 0.4)
const vec3 kFill = vec3(-0.7194, 0.2998, -0.5995); // normalize(-0.6, 0.25, -0.5)
const float kPi = 3.14159265;

vec3 lin(vec3 srgb) { return pow(srgb, vec3(2.2)); }

bool inEllipse(vec2 p, vec2 c, vec2 r) {
    vec2 d = (p - c) / r;
    return dot(d, d) < 1.0;
}

bool spotAt(vec3 d, vec3 s, float size) { return dot(d, normalize(s)) > size; }

void main() {
    int shape = int(vParams.x + 0.5);
    int mat = int(vMatAux.x + 0.5);
    float aux = vMatAux.y;
    vec3 c1 = lin(vColor1.rgb);
    vec3 c2 = lin(vColor2.rgb);
    vec3 N = normalize(vNormal);
    vec3 V = normalize(vView);
    vec3 p = vLocal;

    vec3 base = mat == 0 ? c1 : (mat == 1 ? c2 : (mat == 2 ? vec3(0.86, 0.76, 0.58) : vec3(0.05, 0.035, 0.025)));
    float spec = 0.4;    // highlight strength
    float shine = 40.0;  // highlight tightness
    bool metal = false;
    float glow = 0.0;

    if (shape == 0 || shape == 15) {
        // Paper and foil: a different colour on each side, lit on the side you see.
        bool front = dot(N, V) >= 0.0;
        base = front ? c1 : c2;
        if (!front) N = -N;
        spec = 0.12;
        shine = 16.0;
        metal = shape == 15;
    } else if (shape == 1) {         // coin: all gold
        metal = true;
    } else if (shape == 2) {         // heart: glossy
        spec = 0.75;
        shine = 60.0;
    } else if (shape == 3) {         // Super Star: glossy, a little glow, two eyes on the front
        spec = 0.8;
        shine = 70.0;
        glow = 0.18;
        if (p.z > 0.05 && (inEllipse(p.xy, vec2(-0.07, 0.04), vec2(0.028, 0.07)) ||
                           inEllipse(p.xy, vec2(0.07, 0.04), vec2(0.028, 0.07)))) {
            base = vec3(0.015);
            glow = 0.0;
        }
    } else if (shape == 4) {         // petal: yellow toward the base
        base = mix(c2, c1, smoothstep(-0.44, -0.22, p.y));
        spec = 0.2;
        shine = 20.0;
    } else if (shape == 5) {         // mushroom
        if (mat == 0) {
            vec3 d = normalize((p - vec3(0.0, -0.06, 0.0)) / vec3(0.5, 0.48, 0.5));
            bool spot = spotAt(d, vec3(0.0, 1.0, 0.0), 0.93);
            for (int i = 0; i < 5; ++i) {
                float a = 0.31 + float(i) * 1.2566;
                spot = spot || spotAt(d, vec3(0.866 * cos(a), 0.45, 0.866 * sin(a)), 0.95);
            }
            if (spot) base = c2;
            spec = 0.65;
            shine = 50.0;
        } else {
            if (aux > 0.5) base *= 0.78;   // gills
            else if (p.z > 0.08 && (inEllipse(p.xy, vec2(-0.07, -0.24), vec2(0.028, 0.065)) ||
                                    inEllipse(p.xy, vec2(0.07, -0.24), vec2(0.028, 0.065)))) {
                base = vec3(0.015);        // the eyes
            }
            spec = 0.15;
            shine = 20.0;
        }
    } else if (shape == 6) {         // egg: spots
        vec3 d = normalize(p / vec3(0.36, 0.5, 0.36));
        if (spotAt(d, vec3(0.0, 0.85, 0.52), 0.94) || spotAt(d, vec3(0.8, 0.3, 0.52), 0.93) ||
            spotAt(d, vec3(-0.7, 0.2, -0.68), 0.93) || spotAt(d, vec3(0.3, -0.5, -0.8), 0.94) ||
            spotAt(d, vec3(-0.5, -0.6, 0.62), 0.93) || spotAt(d, vec3(0.9, -0.35, -0.25), 0.95) ||
            spotAt(d, vec3(-0.95, 0.25, 0.2), 0.95) || spotAt(d, vec3(0.1, -0.95, 0.1), 0.95)) {
            base = c2;
        }
        spec = 0.5;
        shine = 45.0;
    } else if (shape == 7) {         // banana: brown tips
        if (aux < 0.06 || aux > 0.94) base = c2;
        spec = 0.35;
        shine = 30.0;
    } else if (shape == 8) {         // shell: plates outlined in the rim colour
        if (mat == 0) {
            vec3 d = normalize(vec3(p.x / 0.44, p.y / 0.34, p.z / 0.44));
            float pol = acos(clamp(d.y, -1.0, 1.0));
            float seg = atan(d.z, d.x) / (kPi / 3.0);
            float edge = abs(seg - floor(seg + 0.5)) * (kPi / 3.0) * sin(pol);
            if (abs(pol - 0.62) < 0.05 || (pol > 0.62 && edge < 0.04)) base = c2;
        }
        spec = 0.7;
        shine = 60.0;
    } else if (shape == 9) {         // hammer: steel head, wooden handle
        if (mat == 1) metal = true;
        else { spec = 0.2; shine = 18.0; }
    } else if (shape == 10) {        // rose: the bloom swirls toward its heart
        if (mat == 0) {
            float r = length(p.xz);
            float swirl = sin(atan(p.z, p.x) + r * 48.0);
            float top = smoothstep(0.7, 0.85, aux);
            base *= mix(1.0, 0.62 + 0.38 * smoothstep(-0.3, 0.6, swirl), top);
            spec = 0.3;
            shine = 25.0;
        } else {
            spec = 0.25;
            shine = 25.0;
        }
    } else if (shape == 11) {        // garlic: papery creases between the cloves
        base = mix(c2 * 0.85, c1, smoothstep(0.05, 0.35, aux));
        if (p.y < -0.42) base *= 0.75;
        spec = 0.25;
        shine = 22.0;
    } else if (shape == 12) {        // bow: satin
        spec = 0.45;
        shine = 25.0;
    }

    float key = max(dot(N, kKey), 0.0);
    float fill = max(dot(N, kFill), 0.0);
    float nv = max(dot(N, V), 0.0);
    float rim = pow(1.0 - nv, 3.0);
    vec3 col;
    if (metal) {
        // A bright sky above, a dark floor below, the key light as a hot spot.
        vec3 R = reflect(-V, N);
        float sky = smoothstep(-0.35, 0.6, R.y);
        vec3 env = mix(vec3(0.08), vec3(1.3), sky) + vec3(3.5) * pow(max(dot(R, kKey), 0.0), 90.0);
        col = base * (0.15 + 0.85 * env * (0.45 + 0.55 * (0.5 + 0.5 * dot(N, kKey)))) + base * 0.3 * rim;
    } else {
        vec3 H = normalize(kKey + V);
        float hl = pow(max(dot(N, H), 0.0), shine) * spec;
        col = base * (0.3 + 0.85 * key + 0.25 * fill) + vec3(hl) + rim * 0.22 * mix(base, vec3(1.0), 0.5);
    }
    col += base * glow;
    float a = vParams.z;
    outColor = vec4(col * a, a);
}
