// vqbench (#1039): the REPRODUCIBLE SYNTHETIC SCENE -- a seeded world of opaque, alpha-tested and translucent
// geometry in the viewer's RT data layout (clean_rt_common.glsl: RtInstance 224 B, RtMaterial 96 B, rt_geom rows,
// the QUANTIZED static VBO of clean_qvertex.glsl with its slot headers, 16-bit indices, rt_geom_vc), its BLASes and
// TLAS (custom index = instance id, mask = the instance's class -- clean_rt_instances.comp maskClass --, FORCE_OPAQUE /
// FORCE_NO_OPAQUE as geomFlags), the bindless textures (an alpha-masked leaf atlas, a fence lattice, a tinted glass
// sheet, white), the light records (clean_lights_common.glsl Light: N point lights + M projectors) and the camera.
#pragma once

#include "vqb_vk.h"

#include <array>
#include <cstdint>
#include <vector>

struct VqbVec3 { float x = 0, y = 0, z = 0; };

struct VqbSceneCfg
{
    uint32_t seed = 1039;
    uint32_t width = 2560, height = 1440;   // render extent (1440p-class, the owner's)
    uint32_t pointLights = 48, projectors = 16;
    uint32_t trees = 40, buildings = 28, glass = 14, fences = 10;
    float sunElevDeg = 38.f, sunAzimDeg = 215.f;
};

struct VqbLightRec   // = clean_lights_common.glsl Light (160 B)
{
    float pos_radius[4];
    float color_falloff[4];
    float proj_params[4];
    float proj_origin[4];
    float proj_axis[4];
    float proj_mat[16];
    uint32_t tex[4];
};
static_assert(sizeof(VqbLightRec) == 160, "Light");

struct VqbInstanceRec   // = RtInstance (224 B)
{
    float model_matrix[16];
    float aabb_min[4];
    float aabb_max[4];
    float tint[4];
    float uv_matrix[16];
    uint32_t lod_group_id, material_id, flags, skin_id;
    float spin_axis[4];
    float spin_pivot[4];
};
static_assert(sizeof(VqbInstanceRec) == 224, "RtInstance");

struct VqbMaterialRec   // = RtMaterial (96 B)
{
    uint32_t base_color_handle[2];
    uint32_t normal_handle[2];
    uint32_t mr_handle[2];
    uint32_t emissive_handle[2];
    float base_color_factor[4];
    float emissive_factor[4];
    float spec_color_gloss[4];
    float metallic_factor, roughness_factor, alpha_cutoff;
    uint32_t flags;
};
static_assert(sizeof(VqbMaterialRec) == 96, "RtMaterial");

struct VqbScene
{
    VqbSceneCfg cfg;
    // camera (absolute region space, z up)
    VqbVec3 eye, sunDir;
    float fovY = 1.0472f, zNear = 0.1f, zFar = 512.f;
    float view[16], proj[16], vpRel[16], invVpRel[16], vpAbs[16], invVpAbs[16];
    // the RT tables (CPU copies; uploaded by build())
    std::vector<VqbInstanceRec> instances;
    std::vector<VqbMaterialRec> materials;
    std::vector<uint32_t> geom;      // uvec4 per instance
    std::vector<uint32_t> geomVc;    // RT_GEOM_VC_WORDS (8) per instance
    std::vector<uint32_t> vbo;       // QVertex words (+ slot headers)
    std::vector<uint32_t> ibo;       // 16-bit indices, 2 per word
    std::vector<VqbLightRec> lights;
    // the bench's own G-buffer generator's per-vertex floats (pos.xyz, n.xyz, uv) and per-instance info
    std::vector<float> gbVerts;      // 8 floats per vertex, in the VBO's vertex order (header slots = zeros)
    std::vector<uint32_t> gbIdx;     // absolute vertex index per corner
    std::vector<uint32_t> gbInst;    // 8 words per instance: first corner, base vertex, material, flags, tex, 0, 0, 0
    // GPU objects
    VqbBuffer bInstances, bMaterials, bGeom, bGeomVc, bVbo, bIbo, bLights, bGbVerts, bGbIdx, bGbInst;
    std::vector<VqbAccel> blas;
    VqbAccel tlas;
    std::vector<VqbImage> textures;   // bindless table (uTextures)
    // stats
    uint32_t triCount = 0, opaqueInst = 0, maskInst = 0, blendInst = 0;

    void generate(const VqbSceneCfg& c);
    void build(VqbCtx& ctx);

    struct SlotBuild { std::vector<float> pos; std::vector<uint16_t> idx; };
    struct Mat4 { float m[16]; };
    std::vector<SlotBuild> mSlotsForBuild;
    std::vector<uint32_t> mInstSlot;
    std::vector<Mat4> mInstModel;
};
