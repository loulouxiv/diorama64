#include "glsl_emitter.h"
namespace ar {
namespace {
// The uniforms of render/metal/msl_emitter.cpp in std140: every member is a vec4, a mat4, a vec4 array or a run of four
// scalars, so the offsets equal the MSL structs' (static_asserts in vk_device.cpp: ViewU 160, DrawU 80, LightU 704).
const char *kCommon = R"(#version 450
layout(set = 0, binding = 0, std140) uniform ViewUB {
    mat4 clip_from_model; vec4 eye_pull; vec4 eye_view; uint layer; uint pad0; uint pad1; uint pad2;
    vec4 wclip_lo; vec4 wclip_hi;     // world clip (level units): lo.w floor on, hi.w box on
    vec4 interp;                      // render interpolation: x alpha, y 1 = on (previous positions, binding 9)
} V;
layout(set = 0, binding = 1, std140) uniform LightUB {
    vec4 dir_on; vec4 tint_amb; vec4 contact; mat4 under_uv_from_model; vec4 under_range;
    vec4 shadow; mat4 shadow_uv_from_model; vec4 cast_v; vec4 relight; vec4 ao; vec4 ao_min; vec4 ao_inv; vec4 toy;
    vec4 env0; vec4 env1; vec4 env2; vec4 env3; vec4 env4; vec4 env5;
    vec4 liquid; vec4 liquid2; vec4 water_box[8]; vec4 water_y[2]; vec4 water_foot;
    vec4 cut; vec4 cut2; vec4 trans; vec4 trans_rgb; vec4 trans_c; vec4 tex;
} L;
layout(set = 1, binding = 0, std140) uniform DrawUB {
    vec4 prim; vec4 env; float lod; float decal; float noise_seed; float fade; vec4 clip; float clip_on; uint lflags; float pull; float talpha;
} d;
)";

const char *kVertex = R"(layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv0;
layout(location = 2) in vec2 a_uv1;
layout(location = 3) in vec4 a_shade;
layout(set = 0, binding = 8, std430) readonly buffer Normals { int n[]; } nrm;
layout(set = 0, binding = 9, std430) readonly buffer PrevPos { float p[]; } prevp;
layout(location = 0) out vec2 v_uv0;
layout(location = 1) out vec2 v_uv1;
layout(location = 2) out vec4 v_shade;
layout(location = 3) out vec3 v_mpos;
layout(location = 4) out vec3 v_nrm;
layout(location = 5) out vec3 v_veye;
layout(location = 6) out float v_lit;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[5]; };
void main() {
    const int vid = gl_VertexIndex;
    // render interpolation: this vertex between the previous tick (alpha 0) and now (1); off: exactly a_pos
    const vec3 pos = V.interp.y > 0.5 ? mix(vec3(prevp.p[3 * vid], prevp.p[3 * vid + 1], prevp.p[3 * vid + 2]), a_pos, V.interp.x) : a_pos;
    // Pull toward the centre eye (level units): an N64 decal's (eye_pull.w) plus a silhouette's clearance (d.pull).
    vec3 p = pos;
    if (d.decal > 0.5 || d.pull > 0.0) {
        const float pull = (d.decal > 0.5 ? V.eye_pull.w : 0.0) + d.pull;
        const vec3 to_eye = V.eye_pull.xyz - p;
        const float len = length(to_eye);
        if (len > 0.0) { p += to_eye * ((d.pull > 0.0 ? min(pull, 0.99 * len) : pull) / len); }
    }
    gl_Position = V.clip_from_model * vec4(d.pull > 0.0 ? pos : p, 1.0);
    if (d.pull > 0.0) {                        // depth only: the pulled point's NDC depth at the unpulled position
        const vec4 cp = V.clip_from_model * vec4(p, 1.0);
        gl_Position.z = cp.z / cp.w * gl_Position.w;
    }
    v_uv0 = a_uv0; v_uv1 = a_uv1; v_shade = a_shade; v_mpos = pos;
    // world clip: hardware clip distances. Undecaled position: the plane is in level units.
    const float off = 1.0;
    gl_ClipDistance[0] = V.wclip_lo.w > 0.5 ? pos.y - V.wclip_lo.y : off;
    const bool box = V.wclip_hi.w > 0.5;
    gl_ClipDistance[1] = box ? pos.x - V.wclip_lo.x : off;
    gl_ClipDistance[2] = box ? V.wclip_hi.x - pos.x : off;
    gl_ClipDistance[3] = box ? pos.z - V.wclip_lo.z : off;
    gl_ClipDistance[4] = box ? V.wclip_hi.z - pos.z : off;
    if ((d.lflags & 256u) != 0u) {
        const int w = nrm.n[vid];
        v_nrm = vec3(bitfieldExtract(w, 0, 8), bitfieldExtract(w, 8, 8), bitfieldExtract(w, 16, 8)) / 127.0;
    } else {
        v_nrm = vec3(0.0);
    }
    v_veye = V.eye_pull.xyz - pos;
    const float nl = length(v_nrm);
    v_lit = L.tint_amb.a + (1.0 - L.tint_amb.a) * (nl > 0.5 ? max(dot(v_nrm / nl, L.dir_on.xyz), 0.0) : 0.0);
}
)";

// The fragment prelude: the helpers of msl_emitter.cpp, reading the interpolants from globals (GLSL has no stage_in
// struct) and the uniforms from the blocks.
const char *kFragment = R"(layout(set = 0, binding = 2) uniform sampler2DShadow sm;
layout(set = 0, binding = 3) uniform sampler3D ao;
layout(set = 0, binding = 4) uniform samplerCube env;
layout(set = 0, binding = 5) uniform sampler2D under;
layout(set = 0, binding = 6) uniform samplerCube env_prev;
layout(set = 0, binding = 7) uniform sampler2D foot;
layout(set = 1, binding = 1) uniform sampler2D t0;
layout(set = 1, binding = 2) uniform sampler2D t1;
layout(location = 0) in vec2 v_uv0;
layout(location = 1) in vec2 v_uv1;
layout(location = 2) in vec4 v_shade;
layout(location = 3) in vec3 v_mpos;
layout(location = 4) in vec3 v_nrm;
layout(location = 5) in vec3 v_veye;
layout(location = 6) in float v_lit;
layout(location = 0) out vec4 o_color;
// The draw's normal at this fragment (interpolated, unit), or up without normals.
vec3 surface_normal() {
    const float l = length(v_nrm);
    return ((d.lflags & 256u) != 0u && l > 0.5) ? v_nrm / l : vec3(0.0, 1.0, 0.0);
}
// The key light's visibility (1 lit, 0 shadowed): 3x3 hardware-PCF taps, the receiver pushed along n against acne.
float shadow_vis(vec3 n) {
    if (L.shadow.x < 0.5) { return 1.0; }
    const vec4 q = L.shadow_uv_from_model * vec4(v_mpos + n * L.shadow.w, 1.0);
    const vec2 uv = q.xy / q.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { return 1.0; }
    const float r = L.shadow.y * L.shadow.z;
    const float ref = min(q.z / q.w, 1.0);
    float s = 0.0;
    for (int j = -1; j <= 1; j++) { for (int i = -1; i <= 1; i++) { s += texture(sm, vec3(uv + vec2(i, j) * r, ref)); } }
    return s / 9.0;
}
// The room-lit look's factor on a FC_LF_TINT draw's colour (msl_emitter.cpp room_light).
vec3 room_light() {
    const vec3 l = L.dir_on.xyz;
    const float A = L.tint_amb.a, K = 1.0 - A;
    const bool has_n = (d.lflags & 256u) != 0u && length(v_nrm) > 0.5;
    const vec3 n = surface_normal();
    const bool receiver = (d.lflags & 32u) != 0u;
    const bool self_lit = (d.lflags & 512u) != 0u;
    vec3 face = vec3(0.0, 1.0, 0.0);
    vec3 g = vec3(0.0, 1.0, 0.0);
    if (receiver && (L.shadow.x > 0.5 || L.ao.x > 0.5)) {
        g = cross(dFdx(v_mpos), dFdy(v_mpos));
        g = length(g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
        face = dot(g, v_veye) < 0.0 ? -g : g;
    }
    float S = 1.0;
    if (receiver && L.shadow.x > 0.5 && !self_lit) {
        S = shadow_vis(dot(g, l) < 0.0 ? -g : g);
    }
    float O = 1.0;
    if (receiver && L.ao.x > 0.5) {
        const vec3 an = has_n ? n : face;
        const vec3 t = (v_mpos + an * 1.5 * L.ao_min.w - L.ao_min.xyz) / L.ao_min.w + 0.5;
        const vec3 beyond = max(-t, t - 1.0 / L.ao_inv.xyz);
        const float fade = clamp(max(beyond.x, max(beyond.y, beyond.z)), 0.0, 1.0);
        const float free_sky = clamp(0.5 + 0.5 * an.y, 0.2, 1.0);
        const float vis = min(texture(ao, t * L.ao_inv.xyz).r / free_sky, 1.0);
        O = mix(1.0, mix(vis, 1.0, fade), L.ao.y);
    }
    const bool relight = L.relight.x > 0.5 && has_n;
    float m;
    if ((d.lflags & 4u) != 0u) {
        const float c = max(dot(has_n ? n : face, l), 0.0);
        const float flat_m = 1.0 - (A * (1.0 - O) + K * c * (1.0 - S)) / (A + K * c);
        const float full = clamp((A * O + K * c * S) / max(v_lit, 1e-3), 0.0, 1.6);
        m = (relight && (d.lflags & 8u) != 0u) ? mix(flat_m, full, L.relight.y) : flat_m;
    } else {
        const float u = max(l.y, 0.2);
        const float flat_m = 1.0 - (A * (1.0 - O) + K * u * (1.0 - S)) / (A + K * u);
        const float full = clamp((A * O + K * max(dot(n, l), 0.0) * S) / (A + K * u), 0.0, 1.6);
        m = ((d.lflags & 2u) != 0u && relight) ? mix(flat_m, full, L.relight.y) : flat_m;
    }
    return L.tint_amb.rgb * m;
}
vec4 room_shade() {
    vec4 s = v_shade;
    if (L.dir_on.w < 0.5 || (d.lflags & 1u) == 0u) { return s; }
    s.rgb = clamp(s.rgb * room_light(), 0.0, 1.0);
    return s;
}
// The toy material (msl_emitter.cpp toy_surface).
vec3 toy_surface(vec3 c, vec3 ref) {
    if (L.dir_on.w < 0.5 || L.toy.x < 0.5 || (d.lflags & 12u) != 4u) { return c; }
    const vec3 n = surface_normal();
    const vec3 v = normalize(V.eye_view.xyz - v_mpos);
    const vec3 l = L.dir_on.xyz;
    const float K = 1.0 - L.tint_amb.a;
    const float S = ((d.lflags & 32u) != 0u && (d.lflags & 512u) == 0u) ? shadow_vis(n) : 1.0;
    const float p = mix(4.0, 32.0, L.toy.y);
    const float spec = L.toy.y * K * S * pow(max(dot(n, normalize(l + v + vec3(0.0, 1e-4, 0.0))), 0.0), p) * (p + 8.0) / 64.0;
    const float rim = L.toy.z * pow(1.0 - max(dot(n, v), 0.0), 3.0) * (0.6 + 0.4 * L.tint_amb.a);
    vec3 o = c + (spec + rim) * L.tint_amb.rgb;
    if (L.toy.w > 0.0 && (d.lflags & 1152u) == 1152u) {
        const vec3 r = reflect(-v, n);
        vec3 e = max(texture(env, vec3(dot(L.env0.xyz, r), dot(L.env1.xyz, r), dot(L.env2.xyz, r))).rgb, vec3(0.0)) * L.env1.w;
        e = pow(e / (1.0 + e), vec3(1.0 / 2.2));
        if (L.env5.w < 1.0) {
            const vec3 q = max(texture(env_prev, vec3(dot(L.env3.xyz, r), dot(L.env4.xyz, r), dot(L.env5.xyz, r))).rgb, vec3(0.0)) * L.env4.w;
            e = mix(pow(q / (1.0 + q), vec3(1.0 / 2.2)), e, L.env5.w);
        }
        const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
        const float cap = 1.5 * dot(max(ref, vec3(0.0)), lw), le = dot(e, lw);
        if (le > cap) { e *= cap / le; }
        o = mix(o, e, 0.6 * L.toy.w);
    }
    return min(o, vec3(1.0));
}
// The contact shadow on the table plus the key light's cast shadow (msl_emitter.cpp table_shadow).
vec4 table_shadow() {
    const vec2 disc[12] = vec2[12](vec2(1.0, 0.0), vec2(0.5, 0.866), vec2(-0.5, 0.866), vec2(-1.0, 0.0), vec2(-0.5, -0.866),
                                   vec2(0.5, -0.866), vec2(0.35, 0.2), vec2(0.0, 0.4), vec2(-0.35, 0.2), vec2(-0.35, -0.2),
                                   vec2(0.0, -0.4), vec2(0.35, -0.2));
    float occ = 0.0;
    if (L.contact.x > 0.0) {
        for (int i = 0; i < 12; i++) {
            const vec3 p = v_mpos + vec3(disc[i].x, 0.0, disc[i].y) * L.contact.z;
            const vec4 q = L.under_uv_from_model * vec4(p, 1.0);
            const float dd = texture(under, q.xy / q.w).r;
            const float h = max(L.under_range.x + dd * L.under_range.y - L.contact.w, 0.0);
            occ += dd < 1.0 ? clamp(1.0 - h / L.contact.y, 0.0, 1.0) : 0.0;
        }
    }
    const float cast_s = L.cast_v.x * (1.0 - L.tint_amb.a) * (1.0 - shadow_vis(vec3(0.0, 1.0, 0.0)));
    const float a = 1.0 - (1.0 - L.contact.x * occ / 12.0) * (1.0 - cast_s);
    if (a <= 1.0 / 255.0) { discard; }
    return vec4(0.0, 0.0, 0.0, a);
}
vec2 lava_shimmer(vec3 p) {
    const float t = L.liquid.y, k = L.liquid2.x * 0.5;
    return 0.004 * vec2(sin(p.x * k + t * 3.1), cos(p.z * k + t * 2.7));
}
vec4 liquid_surface(vec4 c) {
    if (L.liquid.x < 0.5 || (d.lflags & 10240u) == 0u) { return c; }
    const float t = L.liquid.y;
    if ((d.lflags & 8192u) != 0u) {
        const float pulse = 0.85 + 0.15 * sin(t * 2.0 + dot(v_mpos.xz, vec2(0.004, 0.003)));
        const float dim = max(dot(L.tint_amb.rgb, vec3(0.299, 0.587, 0.114)), 0.35);
        c.rgb = min(c.rgb * pulse / dim, vec3(1.0));
        return c;
    }
    vec3 g = cross(dFdx(v_mpos), dFdy(v_mpos));
    g = length(g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
    const vec3 v = normalize(V.eye_view.xyz - v_mpos);
    if (dot(g, v) < 0.0) { g = -g; }
    const float k = L.liquid2.x;
    const vec2 w = vec2(cos(dot(v_mpos.xz, vec2(k, 0.31 * k)) + t * 1.7), cos(dot(v_mpos.xz, vec2(-0.43 * k, 0.9 * k)) + t * 1.3));
    const vec3 n = normalize(g + L.liquid2.y * vec3(w.x, 0.0, w.y));
    const float F = 0.02 + 0.98 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
    vec3 e = L.tint_amb.rgb * vec3(0.80, 0.88, 1.0);
    if (L.toy.w > 0.0) {
        const vec3 r = reflect(-v, n);
        const vec3 q = max(texture(env, vec3(dot(L.env0.xyz, r), dot(L.env1.xyz, r), dot(L.env2.xyz, r))).rgb, vec3(0.0)) * L.env1.w;
        e = pow(q / (1.0 + q), vec3(1.0 / 2.2));
    }
    c.rgb = mix(c.rgb, min(e, vec3(1.0)), F);
    c.a = mix(c.a, 1.0, F);
    return c;
}
vec3 under_water(vec3 c) {
    if (L.liquid.x < 0.5 || L.liquid.z < 0.5 || (d.lflags & 32u) == 0u || (d.lflags & 10240u) != 0u) { return c; }
    const int n = min(int(L.liquid.z), 8);
    for (int i = 0; i < n; i++) {
        const vec4 b = L.water_box[i];
        const float y = L.water_y[i / 4][i % 4];
        if (v_mpos.x > b.x && v_mpos.x < b.z && v_mpos.z > b.y && v_mpos.z < b.w && v_mpos.y < y) {
            const float f = 0.6 * clamp((y - v_mpos.y) / max(L.liquid.w, 1e-3), 0.0, 1.0);
            return mix(c, vec3(0.10, 0.35, 0.45) * L.tint_amb.rgb, f);
        }
    }
    return c;
}
float water_trim() {
    if (L.liquid.x < 0.5 || L.liquid2.z < 0.5 || (d.lflags & 2048u) == 0u) { return 1.0; }
    const int n = min(int(L.liquid.z), 8);
    bool sheet = false;
    for (int i = 0; i < n; i++) { sheet = sheet || abs(v_mpos.y - L.water_y[i / 4][i % 4]) <= L.liquid2.w; }
    if (!sheet) { return 1.0; }
    const vec2 uv = (v_mpos.xz - L.water_foot.xy) * L.water_foot.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { return 0.0; }
    const vec2 size = vec2(textureSize(foot, 0));
    const vec2 t = uv * size - 0.5;
    const vec2 f = fract(t);
    const ivec2 i0 = ivec2(floor(t)), top = ivec2(size) - 1;
    float c[4];
    for (int k = 0; k < 4; k++) {
        const ivec2 q = clamp(i0 + ivec2(k & 1, k >> 1), ivec2(0), top);
        c[k] = texelFetch(foot, q, 0).r < 1.0 ? 1.0 : 0.0;
    }
    return mix(mix(c[0], c[1], f.x), mix(c[2], c[3], f.x), f.y);
}
uint cut_hash(ivec3 c) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ uint(c.z) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
bool cut_away(float fade) {
    vec3 g = cross(dFdx(v_mpos), dFdy(v_mpos));
    const vec3 e = V.eye_pull.xyz, a = L.cut.xyz - e;
    const float len2 = dot(a, a);
    if (len2 < 1e-6) { return false; }
    const vec3 p = v_mpos - e;
    const float s = dot(p, a) / len2;
    if (s <= 0.0 || s >= 1.0) { return false; }
    const float r = L.cut.w * s;
    const float dist = length(p - a * s);
    if (dist >= r) { return false; }
    g = length(g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
    if (dot(g, -p) < 0.0) { g = -g; }
    if (g.y > 0.01 && (1.0 - s) * sqrt(len2) < L.cut2.y) { return false; }
    const vec3 q = floor(v_mpos / L.cut2.z), n = abs(g);
    ivec3 cell;
    if (n.y >= max(n.x, n.z) - 0.01) { cell = ivec3(int(q.x), 0, int(q.z)); }
    else if (n.x >= n.z - 0.01) { cell = ivec3(0, int(q.y), int(q.z)); }
    else { cell = ivec3(int(q.x), int(q.y), 0); }
    const float h = float(cut_hash(cell) >> 8) * (1.0 / 16777216.0);
    const float keep = mix(L.cut2.x, 1.0, smoothstep(0.7 * r, r, dist));
    return h >= (fade < 1.0 ? mix(1.0, keep, fade) : keep);
}
vec3 transition_cover(vec3 c) {
    if (L.trans.x < 0.5 || L.trans.y <= 0.0) { return c; }
    float f = L.trans.y;
    if (L.trans.x > 1.5) {
        const float r = L.trans.z * (1.0 - L.trans.y);
        f = smoothstep(r - L.trans.w, r, length(v_mpos.xz - L.trans_c.xz));
    }
    return mix(c, L.trans_rgb.rgb, clamp(f, 0.0, 1.0));
}
vec3 ink_flip(vec3 c) { return clamp(c + (1.0 - (max(c.r, max(c.g, c.b)) + min(c.r, min(c.g, c.b)))), 0.0, 1.0); }
void main() {
)";
const char *kClip = R"(    if (d.clip_on > 0.5 && (v_mpos.x < d.clip.x || v_mpos.y < d.clip.y || v_mpos.x > d.clip.z || v_mpos.y > d.clip.w)) { discard; }
)";
const char *kBodyStart = R"(    vec2 uv0 = v_uv0;
    if ((d.lflags & 8192u) != 0u && L.liquid.x > 0.5) { uv0 += lava_shimmer(v_mpos); }
    vec4 tex0 = texture(t0, uv0);
    vec4 tex1 = texture(t1, v_uv1);
    vec3 cell = floor(v_mpos) + d.noise_seed;
    float noise = floor(fract(sin(dot(sin(cell), vec3(12.9898, 78.233, 37.719))) * 143758.5453) + 0.5);
    vec4 c = vec4(0.0);
)";

std::string rgb_op(uint8_t s) {
    switch (s) {
        case FC_SRC_1: return "vec3(1.0)";
        case FC_SRC_TEXEL0: return "tex0.rgb";
        case FC_SRC_TEXEL0A: return "vec3(tex0.a)";
        case FC_SRC_TEXEL1: return "tex1.rgb";
        case FC_SRC_TEXEL1A: return "vec3(tex1.a)";
        case FC_SRC_PRIM: return "d.prim.rgb";
        case FC_SRC_PRIMA: return "vec3(d.prim.a)";
        case FC_SRC_SHADE: return "shade.rgb";
        case FC_SRC_SHADEA: return "vec3(shade.a)";
        case FC_SRC_ENV: return "d.env.rgb";
        case FC_SRC_ENVA: return "vec3(d.env.a)";
        case FC_SRC_LOD: return "vec3(d.lod)";
        case FC_SRC_COMBINED: return "c.rgb";
        case FC_SRC_COMBINEDA: return "vec3(c.a)";
        case FC_SRC_NOISE: return "vec3(noise)";
        default: return "vec3(0.0)";
    }
}
std::string a_op(uint8_t s) {
    switch (s) {
        case FC_SRC_1: return "1.0";
        case FC_SRC_TEXEL0: case FC_SRC_TEXEL0A: return "tex0.a";
        case FC_SRC_TEXEL1: case FC_SRC_TEXEL1A: return "tex1.a";
        case FC_SRC_PRIM: case FC_SRC_PRIMA: return "d.prim.a";
        case FC_SRC_SHADE: case FC_SRC_SHADEA: return "shade.a";
        case FC_SRC_ENV: case FC_SRC_ENVA: return "d.env.a";
        case FC_SRC_LOD: return "d.lod";
        case FC_SRC_COMBINED: case FC_SRC_COMBINEDA: return "c.a";
        case FC_SRC_NOISE: return "noise";
        default: return "0.0";
    }
}
bool reads_shade_rgb(const CombinerIR &ir) {
    for (int cyc = 0; cyc < (ir.two_cycle ? 2 : 1); cyc++)
        for (int k = 0; k < 4; k++) if (ir.rgb[cyc][k] == FC_SRC_SHADE) return true;
    return false;
}
bool reads_texel0_rgb(const CombinerIR &ir) {
    for (int cyc = 0; cyc < (ir.two_cycle ? 2 : 1); cyc++)
        for (int k = 0; k < 4; k++) if (ir.rgb[cyc][k] == FC_SRC_TEXEL0) return true;
    return false;
}
}

std::string glsl_vertex() { return std::string(kCommon) + kVertex; }

std::string glsl_fragment(const CombinerIR &ir) {
    std::string s = std::string(kCommon) + kFragment;
    if (ir.clip) s += kClip;
    if (ir.cutaway) s += "    if (L.cut.w > 0.0 && cut_away(d.fade)) { discard; }\n";
    s += kBodyStart;
    if (ir.flat_color) {
        if (ir.flat_cutout) s += "    if (tex0.a <= 0.3) { discard; }\n";
        s += "    o_color = vec4(transition_cover(d.prim.rgb), d.prim.a * d.talpha);\n}\n";
        return s;
    }
    if (ir.table_shadow) { s += "    o_color = table_shadow();\n}\n"; return s; }
    const bool shade_rgb = reads_shade_rgb(ir);
    s += shade_rgb ? "    vec4 shade = room_shade();\n" : "    vec4 shade = v_shade;\n";
    for (int cyc = 0; cyc < (ir.two_cycle ? 2 : 1); cyc++) {
        const uint8_t *r = ir.rgb[cyc], *a = ir.alpha[cyc];
        s += "    {\n";
        s += "        vec3 rgb = (" + rgb_op(r[0]) + " - " + rgb_op(r[1]) + ") * " + rgb_op(r[2]) + " + " + rgb_op(r[3]) + ";\n";
        s += "        float a = (" + a_op(a[0]) + " - " + a_op(a[1]) + ") * " + a_op(a[2]) + " + " + a_op(a[3]) + ";\n";
        s += "        c = vec4(rgb, a);\n";
        s += "    }\n";
    }
    if (!shade_rgb)
        s += "    if (L.dir_on.w > 0.5 && (d.lflags & 1u) != 0u) { c.rgb = clamp(c.rgb * room_light(), 0.0, 1.0); }\n";
    s += "    vec4 texel = c;\n";
    s += std::string("    texel.rgb = toy_surface(texel.rgb, ") + (reads_texel0_rgb(ir) ? "tex0.rgb" : "c.rgb") + ");\n";
    s += "    texel = liquid_surface(texel);\n";
    s += "    texel.rgb = under_water(texel.rgb);\n";
    s += "    texel.rgb = transition_cover(texel.rgb);\n";
    s += "    if ((d.lflags & 4096u) != 0u) { texel.rgb = ink_flip(texel.rgb); }\n";
    s += "    texel.a *= water_trim();\n";
    if (!ir.alpha_enabled) {
        s += "    texel.a = 1.0;\n";
    } else {
        if (ir.texture_edge)
            s += "    if (L.tex.x > 0.5) { texel.a = min(texel.a * (1.0 + max(textureQueryLod(t0, uv0).y, 0.0) * L.tex.y), 1.0); }\n";
        if (ir.texture_edge && ir.alpha_coverage)
            s += "    texel.a = clamp((texel.a - 0.3) / max(fwidth(texel.a), 1.0e-4) + 0.5, 0.0, 1.0);\n"
                 "    if (texel.a <= 0.0) { discard; }\n";
        else if (ir.texture_edge) s += "    if (texel.a > 0.3) { texel.a = 1.0; } else { discard; }\n";
        if (ir.dither) s += "    texel.a *= noise;\n";
    }
    if (ir.depth_fill == kDepthOnlyPrepass) s += "    if (texel.a < 0.5) { discard; }\n";
    s += "    texel.a *= d.fade * d.talpha;\n";
    if (ir.depth_fill == kDepthOnlyHalf) s += "    if (texel.a < 0.5) { discard; }\n";
    if (ir.depth_fill == kDepthOnlyFaint) s += "    if (texel.a <= 1.0 / 255.0) { discard; }\n";
    s += "    o_color = texel;\n}\n";
    return s;
}

std::string glsl_depth_vertex() {
    return R"(#version 450
layout(push_constant, std430) uniform DepthPC { mat4 m; vec4 interp; vec4 wclip_lo; vec4 wclip_hi; } u;
layout(set = 0, binding = 0, std430) readonly buffer PrevPos { float p[]; } prevp;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv0;
layout(location = 0) out vec2 v_uv0;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[5]; };
void main() {
    const int vid = gl_VertexIndex;
    const vec3 pos = u.interp.y > 0.5 ? mix(vec3(prevp.p[3 * vid], prevp.p[3 * vid + 1], prevp.p[3 * vid + 2]), a_pos, u.interp.x) : a_pos;
    gl_Position = u.m * vec4(pos, 1.0);
    v_uv0 = a_uv0;
    const float off = 1.0;
    gl_ClipDistance[0] = u.wclip_lo.w > 0.5 ? pos.y - u.wclip_lo.y : off;
    const bool box = u.wclip_hi.w > 0.5;
    gl_ClipDistance[1] = box ? pos.x - u.wclip_lo.x : off;
    gl_ClipDistance[2] = box ? u.wclip_hi.x - pos.x : off;
    gl_ClipDistance[3] = box ? pos.z - u.wclip_lo.z : off;
    gl_ClipDistance[4] = box ? u.wclip_hi.z - pos.z : off;
}
)";
}
std::string glsl_depth_cut_fragment() {
    return R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D t0;
layout(location = 0) in vec2 v_uv0;
void main() { if (texture(t0, v_uv0).a <= 0.3) { discard; } }
)";
}
std::string glsl_fullscreen_vertex() {
    return R"(#version 450
void main() {
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
}
std::string glsl_composite_fragment() {
    return R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D under_t;
layout(set = 0, binding = 1) uniform sampler2D over_t;
layout(location = 0) out vec4 o_color;
void main() {
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const vec4 o = texelFetch(over_t, p, 0);
    const vec4 u = texelFetch(under_t, p, 0);
    o_color = o + u * (1.0 - o.a);
}
)";
}
}
