#version 450
// Quasar Viewer: the four-texture terrain drawn into a G-buffer by Vulkan (QuasarVulkanTerrainDraws;
// QLVulkanPresenter::gbufferTerrain), as GL's gDeferredTerrainProgram draws it: one patch a surface patch, its corners
// here, tessellated from the region's height map. Kept in step with newview's deferred/terrainPatchV.glsl,
// terrainTC.glsl, terrainTE.glsl, terrainSurface.glsl and terrainF.glsl, and pbrterrainUtilF.glsl's ramp and
// triplanar weights; and PBR terrain, pbrterrainTE.glsl and pbrterrainF.glsl with pbrterrainUtilF.glsl's sampling, at
// full detail over the alpha ramp. Hex tiling, pinned levels, the parcel overlay, flat normals, a paint map and masked
// materials stay GL's (lldrawpoolterrain.cpp sends no region then).

struct Region
{
    mat4 mvp;               // the region's model-view-projection (its render matrix applied)
    mat4 normal_matrix;
    mat4 tess_clip;         // terrain_tess_clip: a capture's projection and view
    vec4 tess_clip_origin;  // terrain_tess_clip_origin
    vec4 tess;              // terrain_tess_origin (region-local), terrain_tess_density
    vec4 grid;              // terrain_grid_scale, region_scale, terrain_smoothing, terrainTriplanarBlend
    vec4 plane_s;           // object_plane_s
    vec4 plane_t;           // object_plane_t
    vec4 misc;              // terrain_tess_raster, TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT, the ramp is R8
    // PBR terrain (pbrterrainF.glsl's uniforms): x set where the region's terrain is PBR, y the undecoded data maps
    // read through decoding views (normal 1 << m, ORM 16 << m).
    vec4 pbr;
    vec4 uv_transform[4];   // terrain_uv_transform, a mat2 each (column-major)
    vec4 uv_offset[2];      // terrain_uv_offset, two a vec4
    vec4 normal_axes[4];    // terrain_normal_axes
    vec4 base_color[4];     // baseColorFactors
    vec4 metallic;          // metallicFactors
    vec4 roughness;         // roughnessFactors
    vec4 emissive[4];       // emissiveColors
};

layout(std430, set = 0, binding = 0) readonly buffer Regions
{
    Region regions[];
};

layout(push_constant) uniform Push
{
    vec4 flags;  // depth remap
} pc;

layout(set = 1, binding = 0) uniform sampler2D terrain_height_map;
layout(set = 1, binding = 1) uniform sampler2D terrain_composition_map;

const int TERRAIN_APRON = 2;

vec4 terrain_gather(ivec2 i)
{
    vec2 texel = 1.0 / vec2(textureSize(terrain_height_map, 0));
    return textureGather(terrain_height_map, vec2(i + (TERRAIN_APRON + 1)) * texel);
}

float terrain_height_linear(vec2 g)
{
    ivec2 i = ivec2(floor(g));
    vec2  f = g - vec2(i);
    vec4  q = terrain_gather(i);
    float lb = q.w;
    float rb = q.z;
    float lt = q.x;
    float rt = q.y;
    return (f.y > f.x) ? lb + f.y * (lt - lb) + f.x * (rt - lt)
                       : lb + f.x * (rb - lb) + f.y * (rt - rb);
}

vec3 terrain_normal_linear(vec2 g, float grid_scale)
{
    float s = 2.0 * grid_scale;
    vec3 p00 = vec3(-s, -s, terrain_height_linear(g + vec2(-2.0, -2.0)));
    vec3 p01 = vec3(-s,  s, terrain_height_linear(g + vec2(-2.0,  2.0)));
    vec3 p10 = vec3( s, -s, terrain_height_linear(g + vec2( 2.0, -2.0)));
    vec3 p11 = vec3( s,  s, terrain_height_linear(g + vec2( 2.0,  2.0)));
    return normalize(cross(p11 - p00, p01 - p10));
}

float terrain_limited_slope(float left, float mid, float right)
{
    float dl = mid - left;
    float dr = right - mid;
    if (dl * dr <= 0.0)
    {
        return 0.0;
    }
    float m = 0.5 * (dl + dr);
    float bound = 3.0 * min(abs(dl), abs(dr));
    return clamp(m, -bound, bound);
}

vec4 terrain_hermite(float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4( 2.0 * t3 - 3.0 * t2 + 1.0,
                -2.0 * t3 + 3.0 * t2,
                       t3 - 2.0 * t2 + t,
                       t3 -       t2);
}

vec4 terrain_hermite_d(float t)
{
    float t2 = t * t;
    return vec4( 6.0 * t2 - 6.0 * t,
                -6.0 * t2 + 6.0 * t,
                 3.0 * t2 - 4.0 * t + 1.0,
                 3.0 * t2 - 2.0 * t);
}

float terrain_height_smooth(vec2 g, out vec2 gradient)
{
    ivec2 i = ivec2(floor(g));
    vec2  f = g - vec2(i);
    vec4 q00 = terrain_gather(i + ivec2(-1, -1));
    vec4 q10 = terrain_gather(i + ivec2( 1, -1));
    vec4 q01 = terrain_gather(i + ivec2(-1,  1));
    vec4 q11 = terrain_gather(i + ivec2( 1,  1));
    float r0[4] = float[4](q00.x, q00.y, q10.x, q10.y);
    float r1[4] = float[4](q01.w, q01.z, q11.w, q11.z);
    float c0m = q00.z;
    float c0p = q01.y;
    float c1m = q10.w;
    float c1p = q11.x;
    float f00 = r0[1], f10 = r0[2], f01 = r1[1], f11 = r1[2];
    float fx00 = terrain_limited_slope(r0[0], f00, f10);
    float fx10 = terrain_limited_slope(f00, f10, r0[3]);
    float fx01 = terrain_limited_slope(r1[0], f01, f11);
    float fx11 = terrain_limited_slope(f01, f11, r1[3]);
    float fy00 = terrain_limited_slope(c0m, f00, f01);
    float fy01 = terrain_limited_slope(f00, f01, c0p);
    float fy10 = terrain_limited_slope(c1m, f10, f11);
    float fy11 = terrain_limited_slope(f10, f11, c1p);
    vec4 hu = terrain_hermite(f.x), hv = terrain_hermite(f.y);
    vec4 du = terrain_hermite_d(f.x), dv = terrain_hermite_d(f.y);
    float h   = hu.x * (hv.x * f00 + hv.y * f01) + hu.y * (hv.x * f10 + hv.y * f11)
              + hu.z * (hv.x * fx00 + hv.y * fx01) + hu.w * (hv.x * fx10 + hv.y * fx11)
              + hu.x * (hv.z * fy00 + hv.w * fy01) + hu.y * (hv.z * fy10 + hv.w * fy11);
    float dhx = du.x * (hv.x * f00 + hv.y * f01) + du.y * (hv.x * f10 + hv.y * f11)
              + du.z * (hv.x * fx00 + hv.y * fx01) + du.w * (hv.x * fx10 + hv.y * fx11)
              + du.x * (hv.z * fy00 + hv.w * fy01) + du.y * (hv.z * fy10 + hv.w * fy11);
    float dhy = hu.x * (dv.x * f00 + dv.y * f01) + hu.y * (dv.x * f10 + dv.y * f11)
              + hu.z * (dv.x * fx00 + dv.y * fx01) + hu.w * (dv.x * fx10 + dv.y * fx11)
              + hu.x * (dv.z * fy00 + dv.w * fy01) + hu.y * (dv.z * fy10 + dv.w * fy11);
    gradient = vec2(dhx, dhy);
    return clamp(h, min(min(f00, f10), min(f01, f11)), max(max(f00, f10), max(f01, f11)));
}

layout(set = 1, binding = 2) uniform sampler2D alpha_ramp;
layout(set = 1, binding = 3) uniform sampler2D detail_0;
layout(set = 1, binding = 4) uniform sampler2D detail_1;
layout(set = 1, binding = 5) uniform sampler2D detail_2;
layout(set = 1, binding = 6) uniform sampler2D detail_3;
// PBR terrain: detail_0..3 are its base colours; then its normal, ORM and emissive maps.
layout(set = 1, binding = 7) uniform sampler2D detail_0_normal;
layout(set = 1, binding = 8) uniform sampler2D detail_1_normal;
layout(set = 1, binding = 9) uniform sampler2D detail_2_normal;
layout(set = 1, binding = 10) uniform sampler2D detail_3_normal;
layout(set = 1, binding = 11) uniform sampler2D detail_0_orm;
layout(set = 1, binding = 12) uniform sampler2D detail_1_orm;
layout(set = 1, binding = 13) uniform sampler2D detail_2_orm;
layout(set = 1, binding = 14) uniform sampler2D detail_3_orm;
layout(set = 1, binding = 15) uniform sampler2D detail_0_emissive;
layout(set = 1, binding = 16) uniform sampler2D detail_1_emissive;
layout(set = 1, binding = 17) uniform sampler2D detail_2_emissive;
layout(set = 1, binding = 18) uniform sampler2D detail_3_emissive;

layout(location = 0) in vec3 vary_normal;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec2 vary_composition;
layout(location = 3) in vec2 vary_region_uv;
layout(location = 4) in vec4 vary_texcoord_side;
layout(location = 5) flat in uint fregion;
layout(location = 6) in vec3 vary_region_position;

layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
const float GBUFFER_FLAG_HAS_PBR   = 0.67;
const float TERRAIN_RAMP_MIX_THRESHOLD = 0.1;
const float TERRAIN_TRIPLANAR_MIX_THRESHOLD = 0.01;
const int SAMPLE_X = 1 << 0;
const int SAMPLE_Y = 1 << 1;
const int SAMPLE_Z = 1 << 2;
const int MIX_X = 1 << 3;
const int MIX_Y = 1 << 4;
const int MIX_Z = 1 << 5;
const int MIX_W = 1 << 6;

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

struct TerrainMix
{
    vec4 weight;
    int type;
};

TerrainMix get_terrain_mix_weights(float alpha1, float alpha2, float alphaFinal)
{
    TerrainMix tm;
    vec4 sample_x = vec4(1,0,0,0);
    vec4 sample_y = vec4(0,1,0,0);
    vec4 sample_z = vec4(0,0,1,0);
    vec4 sample_w = vec4(0,0,0,1);
    tm.weight = mix( mix(sample_w, sample_z, alpha2), mix(sample_y, sample_x, alpha1), alphaFinal );
    tm.weight -= TERRAIN_RAMP_MIX_THRESHOLD;
    ivec4 usage = max(ivec4(0), ivec4(ceil(tm.weight)));
    tm.weight = tm.weight*vec4(usage);
    tm.weight /= (tm.weight.x + tm.weight.y + tm.weight.z + tm.weight.w);
    tm.type = (usage.x * MIX_X) | (usage.y * MIX_Y) | (usage.z * MIX_Z) | (usage.w * MIX_W);
    return tm;
}

// The ramp's alpha: its red where GL keeps it as R8 (misc.w), as GL's swizzle reads it.
float ramp_alpha(vec2 uv, bool red)
{
    vec4 t = texture(alpha_ramp, uv);
    return red ? t.r : t.a;
}

TerrainMix terrain_ramp_mix(vec2 composition, bool red)
{
    float alpha1 = ramp_alpha(composition, red);
    float alpha2 = ramp_alpha(composition - vec2(2.0, 0.0), red);
    float alphaFinal = ramp_alpha(composition - vec2(1.0, 0.0), red);
    return get_terrain_mix_weights(alpha1, alpha2, alphaFinal);
}

struct TerrainTriplanar
{
    vec3 weight;
    int type;
    float sx;
    float sy;
};

TerrainTriplanar terrain_triplanar_weights(vec3 facet_region, float sharpness)
{
    float threshold = TERRAIN_TRIPLANAR_MIX_THRESHOLD;
    vec3 weight_signed = pow(abs(facet_region), vec3(sharpness));
    weight_signed /= (weight_signed.x + weight_signed.y + weight_signed.z);
    weight_signed -= vec3(threshold);
    TerrainTriplanar tw;
    tw.weight = max(vec3(0), weight_signed);
    tw.weight /= (tw.weight.x + tw.weight.y + tw.weight.z);
    ivec3 usage = ivec3(round(max(vec3(0), sign(weight_signed))));
    tw.type = ((usage.x) * SAMPLE_X) | ((usage.y) * SAMPLE_Y) | ((usage.z) * SAMPLE_Z);
    tw.sx = facet_region.x > 0.0 ? 1.0 : -1.0;
    tw.sy = facet_region.y > 0.0 ? 1.0 : -1.0;
    return tw;
}

// The flat projection alone (TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT 1), or the three (3), as terrainF's TERRAIN_DETAIL.
vec4 detail_sample(sampler2D tex, bool triplanar, TerrainTriplanar tw, vec2 uv, vec2 uv_ddx, vec2 uv_ddy,
                   vec4 side, vec4 side_ddx, vec4 side_ddy)
{
    if (!triplanar)
    {
        return textureGrad(tex, uv, uv_ddx, uv_ddy);
    }
    vec4 c = vec4(0.0);
    if ((tw.type & SAMPLE_X) != 0)
    {
        vec2 flip = vec2(tw.sx, 1.0);
        c += textureGrad(tex, flip * side.xy, flip * side_ddx.xy, flip * side_ddy.xy) * tw.weight.x;
    }
    if ((tw.type & SAMPLE_Y) != 0)
    {
        vec2 flip = vec2(-tw.sy, 1.0);
        c += textureGrad(tex, flip * side.zw, flip * side_ddx.zw, flip * side_ddy.zw) * tw.weight.y;
    }
    if ((tw.type & SAMPLE_Z) != 0)
    {
        c += textureGrad(tex, uv, uv_ddx, uv_ddy) * tw.weight.z;
    }
    return c;
}

// ---- PBR terrain ----

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

// qlgbuffer.frag's raw read of a data map through a decoding view, at the level explicit gradients pick.
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
    p = ((p % size) + size) % size;
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

vec4 dataGrad(sampler2D s, bool raw, vec2 uv, vec2 ddx, vec2 ddy)
{
    if (!raw)
    {
        return textureGrad(s, uv, ddx, ddy);
    }
    vec2 size = vec2(textureSize(s, 0));
    float rho = max(length(ddx * size), length(ddy * size));
    float lod = log2(max(rho, 1e-6));
    int top = textureQueryLevels(s) - 1;
    int l0 = clamp(int(floor(lod)), 0, top);
    int l1 = min(l0 + 1, top);
    vec4 near = rawBilinear(s, uv, l0);
    return l1 == l0 ? near : mix(near, rawBilinear(s, uv, l1), clamp(lod - float(l0), 0.0, 1.0));
}

struct PBRMix
{
    vec4 col;
    vec3 orm;
    vec3 vNt;
    vec3 emissive;
};

PBRMix init_pbr_mix()
{
    PBRMix m;
    m.col = vec4(0);
    m.orm = vec3(0);
    m.vNt = vec3(0);
    m.emissive = vec3(0);
    return m;
}

PBRMix mix_pbr(PBRMix a, PBRMix b, float w)
{
    PBRMix m;
    m.col = a.col + b.col * w;
    m.orm = a.orm + b.orm * w;
    m.vNt = a.vNt + b.vNt * w;
    m.emissive = a.emissive + b.emissive * w;
    return m;
}

PBRMix fetch_pbr(vec2 uv, vec2 ddx, vec2 ddy, sampler2D tex_col, sampler2D tex_orm, bool orm_raw, sampler2D tex_vNt,
                 bool normal_raw, sampler2D tex_emissive)
{
    PBRMix m;
    m.col = textureGrad(tex_col, uv, ddx, ddy);
    m.orm = dataGrad(tex_orm, orm_raw, uv, ddx, ddy).xyz;
    m.vNt = dataGrad(tex_vNt, normal_raw, uv, ddx, ddy).xyz * 2.0 - 1.0;
    m.emissive = textureGrad(tex_emissive, uv, ddx, ddy).xyz;
    return m;
}

vec3 _t_normal_compose(vec3 n, mat2 uv_axes, vec3 g)
{
    n.xy = uv_axes * n.xy;
    return normalize(vec3(n.xy * g.z + g.xy * n.z, n.z * g.z));
}

// pbrterrainUtilF's terrain_sample_and_multiply_pbr, without hex tiling.
PBRMix terrain_sample_and_multiply_pbr(vec3 p, vec3 pddx, vec3 pddy, mat2 uv_transform, vec2 uv_offset, bool triplanar,
                                       TerrainTriplanar tw, sampler2D tex_col, sampler2D tex_orm, bool orm_raw,
                                       sampler2D tex_vNt, bool normal_raw, mat2 uv_axes, vec3 geom_normal_region,
                                       sampler2D tex_emissive, vec4 factor_col, vec3 factor_orm, vec3 factor_emissive)
{
    PBRMix mix = init_pbr_mix();
    if (!triplanar)
    {
        mix = fetch_pbr(uv_transform * p.xy + uv_offset, uv_transform * pddx.xy, uv_transform * pddy.xy, tex_col,
                        tex_orm, orm_raw, tex_vNt, normal_raw, tex_emissive);
        mix.vNt = _t_normal_compose(mix.vNt, uv_axes, geom_normal_region);
    }
    else
    {
        float sx = tw.sx;
        float sy = tw.sy;
        if ((tw.type & SAMPLE_X) != 0)
        {
            PBRMix mix_x = fetch_pbr(uv_transform * vec2(sx * p.y, p.z) + uv_offset,
                                     uv_transform * vec2(sx * pddx.y, pddx.z), uv_transform * vec2(sx * pddy.y, pddy.z),
                                     tex_col, tex_orm, orm_raw, tex_vNt, normal_raw, tex_emissive);
            vec3 g = vec3(sx * geom_normal_region.y, geom_normal_region.z, sx * geom_normal_region.x);
            vec3 rr = _t_normal_compose(mix_x.vNt, uv_axes, g);
            mix_x.vNt = vec3(sx * rr.z, sx * rr.x, rr.y);
            mix = mix_pbr(mix, mix_x, tw.weight.x);
        }
        if ((tw.type & SAMPLE_Y) != 0)
        {
            PBRMix mix_y = fetch_pbr(uv_transform * vec2(-sy * p.x, p.z) + uv_offset,
                                     uv_transform * vec2(-sy * pddx.x, pddx.z), uv_transform * vec2(-sy * pddy.x, pddy.z),
                                     tex_col, tex_orm, orm_raw, tex_vNt, normal_raw, tex_emissive);
            vec3 g = vec3(-sy * geom_normal_region.x, geom_normal_region.z, sy * geom_normal_region.y);
            vec3 rr = _t_normal_compose(mix_y.vNt, uv_axes, g);
            mix_y.vNt = vec3(-sy * rr.x, sy * rr.z, rr.y);
            mix = mix_pbr(mix, mix_y, tw.weight.y);
        }
        if ((tw.type & SAMPLE_Z) != 0)
        {
            PBRMix mix_z = fetch_pbr(uv_transform * p.xy + uv_offset, uv_transform * pddx.xy, uv_transform * pddy.xy,
                                     tex_col, tex_orm, orm_raw, tex_vNt, normal_raw, tex_emissive);
            mix_z.vNt = _t_normal_compose(mix_z.vNt, uv_axes, geom_normal_region);
            mix = mix_pbr(mix, mix_z, tw.weight.z);
        }
        mix.vNt = normalize(mix.vNt);
    }
    mix.col *= factor_col;
    mix.orm *= factor_orm;
    mix.emissive *= factor_emissive;
    return mix;
}

mat2 m2(vec4 v)
{
    return mat2(v.xy, v.zw);
}

void writePBR(Region r, TerrainMix tm, bool triplanar, TerrainTriplanar tw)
{
    vec3 geom_normal = vary_normal;
    vec3 p = vary_region_position;
    vec3 pddx = dFdx(p);
    vec3 pddy = dFdy(p);
    vec3 geom_normal_region = transpose(mat3(r.normal_matrix)) * geom_normal;
    uint raw = uint(r.pbr.y + 0.5);
    vec2 offsets[4] = vec2[4](r.uv_offset[0].xy, r.uv_offset[0].zw, r.uv_offset[1].xy, r.uv_offset[1].zw);
    PBRMix pbr_mix = init_pbr_mix();
    if ((tm.type & MIX_X) != 0)
    {
        PBRMix m = terrain_sample_and_multiply_pbr(p, pddx, pddy, m2(r.uv_transform[0]), offsets[0], triplanar, tw,
            detail_0, detail_0_orm, (raw & 16u) != 0u, detail_0_normal, (raw & 1u) != 0u, m2(r.normal_axes[0]),
            geom_normal_region, detail_0_emissive, r.base_color[0], vec3(1.0, r.roughness.x, r.metallic.x),
            r.emissive[0].rgb);
        pbr_mix = mix_pbr(pbr_mix, m, tm.weight.x);
    }
    if ((tm.type & MIX_Y) != 0)
    {
        PBRMix m = terrain_sample_and_multiply_pbr(p, pddx, pddy, m2(r.uv_transform[1]), offsets[1], triplanar, tw,
            detail_1, detail_1_orm, (raw & 32u) != 0u, detail_1_normal, (raw & 2u) != 0u, m2(r.normal_axes[1]),
            geom_normal_region, detail_1_emissive, r.base_color[1], vec3(1.0, r.roughness.y, r.metallic.y),
            r.emissive[1].rgb);
        pbr_mix = mix_pbr(pbr_mix, m, tm.weight.y);
    }
    if ((tm.type & MIX_Z) != 0)
    {
        PBRMix m = terrain_sample_and_multiply_pbr(p, pddx, pddy, m2(r.uv_transform[2]), offsets[2], triplanar, tw,
            detail_2, detail_2_orm, (raw & 64u) != 0u, detail_2_normal, (raw & 4u) != 0u, m2(r.normal_axes[2]),
            geom_normal_region, detail_2_emissive, r.base_color[2], vec3(1.0, r.roughness.z, r.metallic.z),
            r.emissive[2].rgb);
        pbr_mix = mix_pbr(pbr_mix, m, tm.weight.z);
    }
    if ((tm.type & MIX_W) != 0)
    {
        PBRMix m = terrain_sample_and_multiply_pbr(p, pddx, pddy, m2(r.uv_transform[3]), offsets[3], triplanar, tw,
            detail_3, detail_3_orm, (raw & 128u) != 0u, detail_3_normal, (raw & 8u) != 0u, m2(r.normal_axes[3]),
            geom_normal_region, detail_3_emissive, r.base_color[3], vec3(1.0, r.roughness.w, r.metallic.w),
            r.emissive[3].rgb);
        pbr_mix = mix_pbr(pbr_mix, m, tm.weight.w);
    }
    float face = gl_FrontFacing ? 1.0 : -1.0;
    vec3 tnorm = normalize(mat3(r.normal_matrix) * pbr_mix.vNt) * face;
    vec3 orm_out = pbr_mix.orm;
    orm_out.g = filterSpecularRoughness(orm_out.g, tnorm);
    frag_data0 = max(vec4(pbr_mix.col.xyz, 0.0), vec4(0));
    frag_data1 = packORM(max(orm_out, vec3(0)));
    frag_data2 = vec4(octEncode(tnorm), packGeoNormal(geom_normal * face, pc.flags.y > 0.5), GBUFFER_FLAG_HAS_PBR);
    frag_data3 = max(vec4(pbr_mix.emissive, 0.0), vec4(0));
}

void main()
{
    Region r = regions[fregion];
    vec2 uv_ddx = dFdx(vary_texcoord0);
    vec2 uv_ddy = dFdy(vary_texcoord0);
    vec4 side_ddx = dFdx(vary_texcoord_side);
    vec4 side_ddy = dFdy(vary_texcoord_side);

    TerrainMix tm = terrain_ramp_mix(vary_composition, r.misc.w > 0.5);
    bool triplanar = r.misc.z > 2.0;
    TerrainTriplanar tw;
    tw.weight = vec3(0.0, 0.0, 1.0);
    tw.type = SAMPLE_Z;
    tw.sx = 1.0;
    tw.sy = 1.0;
    if (triplanar)
    {
        // terrain_facet: the smooth surface's normal under the fragment, whichever surface is drawn.
        vec2 gradient;
        terrain_height_smooth(vary_region_uv * r.grid.y / r.grid.x, gradient);
        tw = terrain_triplanar_weights(normalize(vec3(-gradient / r.grid.x, 1.0)), r.grid.w);
    }

    if (r.pbr.x > 0.5)
    {
        writePBR(r, tm, triplanar, tw);
        return;
    }

    vec4 outColor = vec4(0.0);
    if ((tm.type & MIX_X) != 0)
    {
        outColor += detail_sample(detail_0, triplanar, tw, vary_texcoord0, uv_ddx, uv_ddy, vary_texcoord_side,
                                  side_ddx, side_ddy) * tm.weight.x;
    }
    if ((tm.type & MIX_Y) != 0)
    {
        outColor += detail_sample(detail_1, triplanar, tw, vary_texcoord0, uv_ddx, uv_ddy, vary_texcoord_side,
                                  side_ddx, side_ddy) * tm.weight.y;
    }
    if ((tm.type & MIX_Z) != 0)
    {
        outColor += detail_sample(detail_2, triplanar, tw, vary_texcoord0, uv_ddx, uv_ddy, vary_texcoord_side,
                                  side_ddx, side_ddy) * tm.weight.z;
    }
    if ((tm.type & MIX_W) != 0)
    {
        outColor += detail_sample(detail_3, triplanar, tw, vary_texcoord0, uv_ddx, uv_ddy, vary_texcoord_side,
                                  side_ddx, side_ddy) * tm.weight.w;
    }
    outColor.a = 0.0;  // downstream atmospherics
    frag_data0 = max(outColor, vec4(0));
    frag_data1 = vec4(0.0);  // GL writes (0, 0, 0, -1), which the unsigned attachment holds as 0
    frag_data2 = vec4(octEncode(normalize(vary_normal)), 0.0, GBUFFER_FLAG_HAS_ATMOS);
    frag_data3 = vec4(0.0);
}
