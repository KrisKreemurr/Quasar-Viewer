#version 450
// Quasar Viewer: the G-buffer texels of a prim drawn by Vulkan, as GL's fragment stages write them.
//   - Simple (kind 0), deferred/diffuseIndexedF.glsl: the diffuse (decoded by its sRGB view, as GL's LINEAR_DIFFUSE
//     sampler decodes it) times the linear tint into the sRGB albedo attachment (encoded on store, as
//     GL_FRAMEBUFFER_SRGB does), the tint's alpha as the legacy specular and glossiness, the normal octahedron-encoded
//     with the tint's alpha as environment intensity and the atmospherics flag (globalF.glsl's encodeNormal,
//     GBUFFER_FLAG_HAS_ATMOS), emissive zero.
//   - PBR (kind 1), deferred/pbropaqueF.glsl (opaque, no mask): base colour times the tint; the normal map in the
//     tangent frame (mikktspace); occlusion, roughness and metal from the ORM map times the factors, roughness widened
//     by filterSpecularRoughness and packed by packORM; the normal with the geometric normal (encodeNormalGeo) and
//     GBUFFER_FLAG_HAS_PBR; emissive colour times the emissive map. Base colour and emissive are decoded by their views
//     where the image is sRGB, as GL's colour samplers decode them; the normal and ORM maps are read undecoded, as GL's
//     data samplers read them, so where their view decodes, the value is encoded back.

layout(set = 1, binding = 0) uniform sampler2D diffuseMap;
layout(set = 1, binding = 1) uniform sampler2D bumpMap;
layout(set = 1, binding = 2) uniform sampler2D specularMap;
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;

struct Draw
{
    mat4 mvp;
    mat4 normal_matrix;
    mat4 texture_matrix;
    vec4 transforms[8];
    vec4 pbr;  // metallic, roughness, kind, sRGB view bits
    vec4 emissive;
    vec4 spare[2];
};

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    Draw draws[];
};

layout(push_constant) uniform Push
{
    vec4 flags;  // depth remap, GBUFFER_NORM_HDR, emissive attachment
} pc;

layout(location = 0) in vec3 vary_normal;
layout(location = 1) in vec2 base_color_texcoord;
layout(location = 2) in vec4 vertex_color;
layout(location = 3) in vec2 normal_texcoord;
layout(location = 4) in vec2 metallic_roughness_texcoord;
layout(location = 5) in vec2 emissive_texcoord;
layout(location = 6) in vec3 vary_tangent;
layout(location = 7) flat in float vary_sign;
layout(location = 8) flat in uint draw_index;

layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
const float GBUFFER_FLAG_HAS_PBR   = 0.67;

vec2 OctWrap(vec2 v)
{
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

float packGeoNormal(vec3 g, bool hdr)
{
    float levels = hdr ? 255.0 : 31.0;
    float range  = hdr ? 65535.0 : 1023.0;
    vec2 o = clamp(octEncode(g), 0.0, 1.0);
    vec2 q = floor(o * levels + 0.5);
    return ((q.x * (levels + 1.0)) + q.y) / range;
}

vec4 packORM(vec3 orm)
{
    float r  = clamp(orm.g, 0.0, 1.0);
    float hi = floor(r * 255.0) / 255.0;
    return vec4(orm.r, hi, orm.b, (r - hi) * 255.0);
}

float filterSpecularRoughness(float perceptualRoughness, vec3 n)
{
    const float SPEC_AA_VARIANCE  = 0.15;
    const float SPEC_AA_THRESHOLD = 0.25;
    vec3 du = dFdx(n);
    vec3 dv = dFdy(n);
    float variance = SPEC_AA_VARIANCE * (dot(du, du) + dot(dv, dv));
    float kernel   = min(2.0 * variance, SPEC_AA_THRESHOLD);
    float alpha = perceptualRoughness * perceptualRoughness;
    return sqrt(sqrt(clamp(alpha * alpha + kernel, 0.0, 1.0)));
}

// The sRGB encode, exact: undoes a decoding view's read of an undecoded value.
vec3 linear_to_srgb(vec3 cl)
{
    cl = clamp(cl, vec3(0), vec3(1));
    vec3 low_range  = cl * 12.92;
    vec3 high_range = 1.055 * pow(cl, vec3(0.41666)) - 0.055;
    bvec3 lt = lessThan(cl, vec3(0.0031308));
    return mix(high_range, low_range, lt);
}

void main()
{
    Draw d = draws[draw_index];
    if (d.pbr.z < 0.5)
    {
        vec3 col = vertex_color.rgb * texture(diffuseMap, base_color_texcoord).rgb;
        frag_data0 = vec4(col, 0.0);
        frag_data1 = vec4(vec3(vertex_color.a), vertex_color.a);
        vec3 nvn = normalize(vary_normal);
        frag_data2 = vec4(octEncode(nvn), vertex_color.a, GBUFFER_FLAG_HAS_ATMOS);
        frag_data3 = vec4(0.0);
        return;
    }
    uint srgb_bits = uint(d.pbr.w + 0.5);
    vec4 basecolor = texture(diffuseMap, base_color_texcoord) * vertex_color;
    vec3 col = basecolor.rgb;

    vec3 normal_texel = texture(bumpMap, normal_texcoord).xyz;
    if ((srgb_bits & 2u) != 0u)
    {
        normal_texel = linear_to_srgb(normal_texel);
    }
    vec3 vNt = normal_texel * 2.0 - 1.0;
    vec3 vN = vary_normal;
    vec3 vT = vary_tangent;
    vec3 vB = vary_sign * cross(vN, vT);
    vec3 tnorm = normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);

    vec3 spec = texture(specularMap, metallic_roughness_texcoord).rgb;
    if ((srgb_bits & 4u) != 0u)
    {
        spec = linear_to_srgb(spec);
    }
    spec.g *= d.pbr.y;  // roughness
    spec.b *= d.pbr.x;  // metallic
    spec.g = filterSpecularRoughness(spec.g, tnorm);

    vec3 emissive = d.emissive.rgb * texture(emissiveMap, emissive_texcoord).rgb;

    float face = gl_FrontFacing ? 1.0 : -1.0;
    tnorm *= face;
    frag_data0 = max(vec4(col, 0.0), vec4(0));
    frag_data1 = packORM(max(spec, vec3(0)));
    vec3 geo = normalize(vary_normal) * face;
    frag_data2 = vec4(octEncode(tnorm), packGeoNormal(geo, pc.flags.y > 0.5), GBUFFER_FLAG_HAS_PBR);
    frag_data3 = max(vec4(emissive, 0.0), vec4(0));
}
