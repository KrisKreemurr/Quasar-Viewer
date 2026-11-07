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
//     data samplers read them: where their view decodes, each texel is encoded back before it is filtered (trilinear,
//     by hand: sampleRaw), since the view's own filtering would blend decoded values.
//   - Cut-outs, in the pipeline built with MASKED (the draws of alpha mode MASK alone, so the opaque pipeline has no
//     discard and keeps its early depth test): simple (deferred/diffuseAlphaMaskIndexedF.glsl: the texel times the tint,
//     alpha included, dropped below the cutoff; no specular, no environment), PBR (pbropaqueF.glsl's HAS_ALPHA_MASK:
//     the base colour's alpha against the material's cutoff), and the legacy materials' mask.

// Set where the pipeline's draws are cut-outs; constant, so the opaque pipeline's stage holds no discard.
layout(constant_id = 0) const bool MASKED = false;
const float MASK_BIAS = 0.001953125;  // 1/512, half an 8-bit quantization, as GL's mask programs bias the cutoff

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
    vec4 specular;  // a legacy material's specular colour (sRGB) and glossiness
    vec4 material;  // environment intensity, alpha cutoff, fullbright, alpha mode | normal map 4 | specular map 8
    vec4 rig;
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

vec4 rawTexel(sampler2D s, ivec2 p, ivec2 size, int level)
{
    p = ((p % size) + size) % size;  // repeat, as GL's AnisoWrap
    vec4 c = texelFetch(s, p, level);
    return vec4(linear_to_srgb(c.rgb), c.a);
}

vec4 rawBilinear(sampler2D s, vec2 uv, int level)
{
    ivec2 size = textureSize(s, level);
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - floor(p);
    vec4 a = rawTexel(s, i0, size, level);
    vec4 b = rawTexel(s, i0 + ivec2(1, 0), size, level);
    vec4 c = rawTexel(s, i0 + ivec2(0, 1), size, level);
    vec4 e = rawTexel(s, i0 + ivec2(1, 1), size, level);
    return mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
}

// A texel of a data map through a view that decodes sRGB: as GL's undecoded sampler filters it, near enough (trilinear
// over the raw values, at the level the hardware would pick; GL's anisotropy is not followed).
vec4 sampleRaw(sampler2D s, vec2 uv)
{
    float lod = textureQueryLod(s, uv).x;
    int top = textureQueryLevels(s) - 1;
    int l0 = clamp(int(floor(lod)), 0, top);
    int l1 = min(l0 + 1, top);
    vec4 near = rawBilinear(s, uv, l0);
    if (l1 == l0)
    {
        return near;
    }
    return mix(near, rawBilinear(s, uv, l1), clamp(lod - float(l0), 0.0, 1.0));
}

vec3 srgb_to_linear(vec3 cs)
{
    vec3 low_range = cs / vec3(12.92);
    vec3 high_range = pow((cs + vec3(0.055)) / vec3(1.055), vec3(2.4));
    return mix(high_range, low_range, lessThanEqual(cs, vec3(0.04045)));
}

// A legacy material, class3/deferred/materialF.glsl's G-buffer path: the diffuse (decoded) times the tint, the mask's
// discard, the specular map (decoded, as AnisoWrapSRGB) times the linear specular colour, glossiness times the normal
// map's alpha and widened by filterSpecularRoughness, the normal map (read undecoded, as AnisoWrap) in the tangent
// frame, fullbright or the emissive mask's alpha as the albedo's alpha.
void writeMaterial(Draw d, uint srgb_bits)
{
    uint flags = uint(d.material.w + 0.5);
    uint alpha_mode = flags & 3u;
    vec4 diffcol = texture(diffuseMap, base_color_texcoord);
    diffcol.rgb *= vertex_color.rgb;
    if (MASKED && alpha_mode == 2u && diffcol.a < d.material.y - MASK_BIAS)
    {
        discard;
    }
    vec4 spec;
    if ((flags & 8u) != 0u)
    {
        spec = texture(specularMap, metallic_roughness_texcoord);
        spec.rgb *= srgb_to_linear(d.specular.rgb);
    }
    else
    {
        spec = vec4(srgb_to_linear(d.specular.rgb), 1.0);
    }
    float env = d.material.x * spec.a;
    float glossiness = d.specular.a;
    vec3 norm;
    if ((flags & 4u) != 0u)
    {
        vec4 vNt = (srgb_bits & 2u) != 0u ? sampleRaw(bumpMap, normal_texcoord) : texture(bumpMap, normal_texcoord);
        glossiness *= vNt.a;
        vNt.xyz = vNt.xyz * 2.0 - 1.0;
        vec3 vN = vary_normal;
        vec3 vT = vary_tangent;
        vec3 vB = vary_sign * cross(vN, vT);
        norm = normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);
    }
    else
    {
        norm = normalize(vary_normal);
    }
    glossiness = 1.0 - filterSpecularRoughness(1.0 - glossiness, norm);
    float emissive = alpha_mode == 3u ? max(diffcol.a, d.material.z) : d.material.z;
    frag_data0 = max(vec4(diffcol.rgb, emissive), vec4(0));
    frag_data1 = max(vec4(linear_to_srgb(spec.rgb), glossiness), vec4(0));
    frag_data2 = vec4(octEncode(norm), env, GBUFFER_FLAG_HAS_ATMOS);
    frag_data3 = vec4(0.0);
}

void main()
{
    Draw d = draws[draw_index];
    if (d.pbr.z > 1.5)
    {
        writeMaterial(d, uint(d.pbr.w + 0.5));
        return;
    }
    if (d.pbr.z < 0.5 && MASKED)
    {
        vec4 col = texture(diffuseMap, base_color_texcoord) * vertex_color;
        if (col.a < d.material.y - MASK_BIAS)
        {
            discard;
        }
        frag_data0 = vec4(col.rgb, 0.0);
        frag_data1 = vec4(0.0);
        frag_data2 = vec4(octEncode(normalize(vary_normal)), 0.0, GBUFFER_FLAG_HAS_ATMOS);
        frag_data3 = vec4(0.0);
        return;
    }
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
    if (MASKED && basecolor.a < d.material.y - MASK_BIAS)
    {
        discard;
    }
    vec3 col = basecolor.rgb;

    vec3 normal_texel = (srgb_bits & 2u) != 0u ? sampleRaw(bumpMap, normal_texcoord).xyz
                                                 : texture(bumpMap, normal_texcoord).xyz;
    vec3 vNt = normal_texel * 2.0 - 1.0;
    vec3 vN = vary_normal;
    vec3 vT = vary_tangent;
    vec3 vB = vary_sign * cross(vN, vT);
    vec3 tnorm = normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);

    vec3 spec = (srgb_bits & 4u) != 0u ? sampleRaw(specularMap, metallic_roughness_texcoord).rgb
                                         : texture(specularMap, metallic_roughness_texcoord).rgb;
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
