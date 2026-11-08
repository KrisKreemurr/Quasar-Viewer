#version 450
// Quasar Viewer: the four-texture terrain drawn into a G-buffer by Vulkan (QuasarVulkanTerrainDraws;
// QLVulkanPresenter::gbufferTerrain), as GL's gDeferredTerrainProgram draws it: one patch a surface patch, its corners
// here, tessellated from the region's height map. Kept in step with newview's deferred/terrainPatchV.glsl,
// terrainTC.glsl, terrainTE.glsl, terrainSurface.glsl and terrainF.glsl, and pbrterrainUtilF.glsl's ramp and
// triplanar weights. Hex tiling, pinned levels and the parcel overlay stay GL's (pipeline.cpp sends no region then).

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

layout(location = 0) in vec3 vary_normal;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec2 vary_composition;
layout(location = 3) in vec2 vary_region_uv;
layout(location = 4) in vec4 vary_texcoord_side;
layout(location = 5) flat in uint fregion;

layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
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
