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
    vec4 misc;              // terrain_tess_raster, TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT, 0
};

layout(std430, set = 0, binding = 0) readonly buffer Regions
{
    Region regions[];
};

// Each instance a patch: its region-local corner (x, y), its edge and its region's index.
layout(std430, set = 0, binding = 1) readonly buffer Patches
{
    vec4 patches[];
};

layout(location = 0) out uint vregion;

void main()
{
    // getTerrainGeometry's order: (x0, y0), (x0 + size, y0), (x0 + size, y0 + size), (x0, y0 + size).
    vec4 p = patches[gl_InstanceIndex];
    uint c = uint(gl_VertexIndex);
    vec2 corner = vec2((c == 1u || c == 2u) ? 1.0 : 0.0, c >= 2u ? 1.0 : 0.0);
    gl_Position = vec4(p.xy + corner * p.z, 0.0, 1.0);
    vregion = uint(p.w + 0.5);
}
