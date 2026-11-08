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

layout(vertices = 4) out;

layout(set = 1, binding = 0) uniform sampler2D terrain_height_map;

layout(location = 0) in uint vregion[];
layout(location = 0) patch out uint tregion;

const int   TERRAIN_APRON      = 2;
const float TERRAIN_MAX_LEVEL  = 64.0;
const float TERRAIN_GRID_LEVEL = 16.0;

vec3 corner(int i, float grid_scale)
{
    vec2 p = gl_in[i].gl_Position.xy;
    ivec2 g = ivec2(round(p / grid_scale)) + TERRAIN_APRON;
    return vec3(p, texelFetch(terrain_height_map, g, 0).r);
}

float edgeLevel(Region r, vec3 a, vec3 b)
{
    float density = r.tess.w;
    if (density <= 0.0)
    {
        return TERRAIN_GRID_LEVEL;
    }
    precise float len   = distance(a, b);
    precise float d     = distance(0.5 * (a + b), r.tess.xyz);
    precise float level = len * density / max(d, 1.0);
    if (r.misc.x > 0.0)
    {
        precise vec4 ca = r.tess_clip * vec4(a - r.tess.xyz, 0.0) + r.tess_clip_origin;
        precise vec4 cb = r.tess_clip * vec4(b - r.tess.xyz, 0.0) + r.tess_clip_origin;
        if (min(ca.w, cb.w) > 0.0)
        {
            precise vec2 span = (cb.xy / cb.w - ca.xy / ca.w) * r.misc.xy;
            level = min(level, length(span));
        }
    }
    return clamp(level, 1.0, TERRAIN_MAX_LEVEL);
}

void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    if (gl_InvocationID == 0)
    {
        tregion = vregion[0];
        Region r = regions[vregion[0]];
        vec3 c0 = corner(0, r.grid.x);
        vec3 c1 = corner(1, r.grid.x);
        vec3 c2 = corner(2, r.grid.x);
        vec3 c3 = corner(3, r.grid.x);
        gl_TessLevelOuter[0] = edgeLevel(r, c0, c3);   // u = 0
        gl_TessLevelOuter[1] = edgeLevel(r, c0, c1);   // v = 0
        gl_TessLevelOuter[2] = edgeLevel(r, c1, c2);   // u = 1
        gl_TessLevelOuter[3] = edgeLevel(r, c3, c2);   // v = 1
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
        gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
    }
}
