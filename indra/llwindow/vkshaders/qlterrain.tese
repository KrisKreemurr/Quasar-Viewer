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

// The pipeline sets GL's lower-left domain origin, so ccw winds as GL's does.
layout(quads, fractional_odd_spacing, ccw) in;

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

layout(location = 0) patch in uint tregion;

layout(location = 0) out vec3 vary_normal;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) out vec2 vary_composition;
layout(location = 3) out vec2 vary_region_uv;
layout(location = 4) out vec4 vary_texcoord_side;
layout(location = 5) flat out uint fregion;

void main()
{
    Region r = regions[tregion];
    fregion = tregion;
    vec2 bottom = mix(gl_in[0].gl_Position.xy, gl_in[1].gl_Position.xy, gl_TessCoord.x);
    vec2 top    = mix(gl_in[3].gl_Position.xy, gl_in[2].gl_Position.xy, gl_TessCoord.x);
    vec2 xy     = mix(bottom, top, gl_TessCoord.y);

    float grid_scale = r.grid.x;
    vec2 g = xy / grid_scale;
    vec3 normal;
    float h;
    if (r.grid.z > 0.5)
    {
        vec2 gradient;
        h = terrain_height_smooth(g, gradient);
        normal = normalize(vec3(-gradient / grid_scale, 1.0));
    }
    else
    {
        normal = terrain_normal_linear(g, grid_scale);
        h = terrain_height_linear(g);
    }
    vec3 position = vec3(xy, h);
    vec4 pre_pos = vec4(position, 1.0);
    vec4 p = r.mvp * pre_pos;
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
    vary_normal = normalize(mat3(r.normal_matrix) * normal);
    vary_texcoord0 = vec2(dot(pre_pos, r.plane_s), dot(pre_pos, r.plane_t));  // texgen_object, texture matrix 0 identity
    float scale = r.plane_s.x;
    vec2 phase = vec2(r.plane_s.w, r.plane_t.w);
    vary_texcoord_side.xy = vec2(position.y * scale + phase.y, position.z * scale);
    vary_texcoord_side.zw = vec2(position.x * scale + phase.x, position.z * scale);
    vec2 cg = g + float(TERRAIN_APRON) + 0.5;
    vary_composition = texture(terrain_composition_map, cg / vec2(textureSize(terrain_composition_map, 0))).rg;
    vary_region_uv = xy / r.grid.y;
}
