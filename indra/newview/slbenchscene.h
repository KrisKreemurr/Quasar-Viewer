/**
 * @file slbenchscene.h
 * @brief Render benchmark scene capture: the on-disk format shared by capture and replay (#1304).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SL_BENCHSCENE_H
#define SL_BENCHSCENE_H

#include "llcleaninstancestore.h"
#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llimage.h"
#include "llpointer.h"
#include "lluuid.h"
#include "stdtypes.h"
#include "vkgfx/llvulkancontext.h"   // LLVKLight, LLVKProbe, LLVKProbeRegion, LLVKWorldFrame

#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// CAPTURE FORMAT (version kFormatVersion). A capture is a directory:
//
//   manifest.json      format + version, viewer, counts, what is NOT in it, textures, and
//                      every other file with its size and XXH64 (checked before a replay)
//   instances.bin      static scene rows (agent frame, render offset removed)
//   groups.bin         the rows' LOD groups: per level the slot (capture slot id) or none
//   slots.bin          geometry per slot: the renderer's own packed mirror (header-prefixed
//                      quantized vertices, face-local U16 indices, skin weights if any)
//   materials.bin      material descriptors (texture UUIDs + factors + flags)
//   terrain.bin        terrain records (their materials are capture material ids)
//   env.bin            the resolved sky / water / cloud / atmosphere frame inputs, manual
//                      reflection probes and the regions' probe data
//   lights.bin         light track: every sample's local lights (LLVKLight, verbatim)
//   movers.bin         mover track: per sample the rows whose pose changed / that went away
//   camera.json        the camera during the capture, as a bench camera path (slbenchpath.h)
//   sky.xml water.xml  the resolved sky and water settings (LLSD): a replay makes them the
//                      local environment, for the renderer modules that read LLEnvironment
//   textures/<id>.png  decoded texture pixels (the CPU copy the renderer uploads from)
//   skins.bin          (version 2, optional) skin records of the rigged rows: joint list, inverse
//                      binds, bind shape (SkinRec)
//   poses.bin          (version 2, optional) pose track: per sample the armatures' joint world
//                      matrices (PoseHdr + joint matrices), deltas from the previous sample
//
// Binary files: a 24 B FileHeader, then records. Little-endian, native struct layout of the
// renderer's own GPU / frame structs (they are already byte-specified: static_asserts below);
// a layout change bumps kFormatVersion and old captures are refused, never misread.
//
// VERSION 2 (additive): a capture made outside the viewer (render2's Blender exporter) may hold
// rigged rows, skins and pose tracks. A version 2 manifest has "version": 2 and every binary
// file's header carries version 2; skins.bin and poses.bin are optional (absent = no skins).
// The viewer's own capture writer still writes version 1; this reader accepts 1 and 2. See
// doc/bench.md ("Skins and pose tracks") for the contract; the layout below is the authority.
//
//   A RIGGED ROW is an InstanceRec whose inst.flags has bit 0 (kFlagRigged) and whose
//   inst.skin_id is a capture SKIN id (SkinRec::skin). Its LOD-group slots must carry weights
//   (SlotRec::has_weights, MirrorWeight = LLCleanQV::QWeight, 10 B; joint index j[k] is an index
//   into the SKIN's joint list, 0 .. joint_count - 1). Its model_matrix is only the static
//   fallback (identity is fine) and its aabb is the slot's bind-space bounds, as the live ingest
//   writes them. Vertices are in the mesh's own space; bind_shape takes them into the skeleton's
//   bind space (exactly LLMeshSkinInfo::mBindShapeMatrix).
//
//   Skinning (what the pre-skin does, per skin joint j of a vertex's four influences):
//       palette[j] = jointWorld[armature][joint_nums[j]] * inv_bind[j]          (mat4, column-major)
//       skinned    = sum_k w_k * palette[j_k] * bind_shape * v                   (agent space)
//   An ARMATURE is one joint table (216 joints, LLCleanSkinStore::MAX_AVATAR_JOINTS), the thing
//   the live path calls an avatar slot. Several skins (every rigged mesh of one character)
//   share an armature. Joint 0 is the armature's root (the reduced-tier skin cache anchors on it).
//
// NOT CAPTURED by the viewer's writer (manifest "missing"): avatars (rigged rows, skins, joint /
// pose tracks), HUD and worn rigid attachments, particles, animated textures' state, objects
// that appear after the capture starts, environment changes during the capture (the first
// frame's sky / water is kept), light-takeover face links.
namespace SLBenchScene
{
    constexpr U32 kFormatVersion = 1;        // what the viewer's capture writer produces
    constexpr U32 kFormatVersionMax = 2;     // the newest this reader accepts (2: + skins, poses)
    constexpr char kMagic[8] = { 'S', 'L', 'B', 'E', 'N', 'C', 'H', '1' };

    enum Kind : U32
    {
        KIND_INSTANCES = 1, KIND_GROUPS, KIND_SLOTS, KIND_MATERIALS, KIND_TERRAIN, KIND_ENV,
        KIND_LIGHTS, KIND_MOVERS,
        KIND_SKINS,   // 9  (version 2)
        KIND_POSES    // 10 (version 2)
    };

    struct FileHeader
    {
        char magic[8];
        U32  version;
        U32  kind;
        U64  count;    // records (samples for the track files, 1 for env)
    };
    static_assert(sizeof(FileHeader) == 24, "FileHeader is 24 B");

    struct InstanceRec
    {
        U32 row;    // the capture session's instance id (track key)
        U32 active; // 1 = the row moved during the capture (registered as a mover on replay)
        LLCleanInstanceStore::Instance inst;
    };
    static_assert(sizeof(LLCleanInstanceStore::Instance) == 224, "Instance layout changed: bump kFormatVersion");

    struct GroupRec
    {
        U32 gid;
        U32 slot[LLCleanMeshAsset::NUM_LOD];   // capture slot ids, LOD_SENTINEL = none
    };

    // Followed by (nv + QV_HEADER) QVertex, ni U16, and nv MirrorWeight when has_weights.
    struct SlotRec
    {
        U32 slot;
        U32 nv;
        U32 ni;
        U32 has_weights;
    };
    static_assert(sizeof(LLCleanMeshAsset::QVertex) == 20, "QVertex layout changed: bump kFormatVersion");

    // MaterialDesc with fixed-size fields (the desc itself carries bools / padding).
    struct MaterialRec
    {
        U32 id;
        U8  tex[4][16];          // base, normal, mr, emissive
        F32 base_color_factor[4];
        F32 emissive_factor[4];
        F32 spec_color_gloss[4];
        F32 metallic, roughness, alpha_cutoff, env_intensity, glow;
        U8  legacy, color_all_slots, gltf, emissive_mask, hud, alpha_blend, bump, pad0;
        U8  uv_own[3], pad1;
        F32 uv_xform[3][6];
    };
    MaterialRec toRec(U32 id, const LLCleanMaterialStore::MaterialDesc& d);
    LLCleanMaterialStore::MaterialDesc fromRec(const MaterialRec& r);

    struct TerrainRec
    {
        U32 idx;
        U32 pad;
        LLCleanMaterialStore::TerrainRecord rec;   // mat[] = capture material ids
    };

    // The environment half of LLVKWorldFrame (what buildVulkanWorldFrame derives from
    // LLEnvironment), then probe_count LLVKProbe and region_count LLVKProbeRegion.
    struct EnvRec
    {
        F32 sun_dir[4], sun_color[4], ambient_color[4], sky_horizon[4], sky_zenith[4];
        F32 water_fog[4];
        F32 atmos[64];
        F32 water[24];
        U8  water_normal[2][16];
        F32 clouds[16];
        U8  cloud_noise[2][16];
        F32 bodies[40];
        U8  sky_tex[6][16];
        F32 probe_sky_ambiance;
        S32 probe_agent_cell[2];
        U32 probe_count;
        U32 region_count;
        U32 pad;
    };
    void envFromFrame(const LLVKWorldFrame& wf, EnvRec& out);
    void envToFrame(const EnvRec& e, LLVKWorldFrame& wf);

    struct SampleHdr
    {
        U32 sample;   // track sample index (time = sample / track_hz)
        U32 count;    // records that follow
    };

    enum MoverKind : U32 { MOVER_POSE = 0, MOVER_GONE = 1 };
    struct MoverRec
    {
        U32 row;
        U32 kind;
        F32 model[16];
        F32 spin_axis[4];
        F32 spin_pivot[4];
    };

    // ---- version 2: skins and pose tracks ------------------------------------------------------
    // Limits (the store's own: LLCleanSkinStore::MAX_JOINTS / MAX_AVATAR_JOINTS). The reader
    // refuses a file past any of them, before allocating.
    constexpr U32 kMaxSkinJoints = 110;       // joints one skin may name
    constexpr U32 kMaxArmatureJoints = 216;   // joints one armature (joint table) holds
    constexpr U32 kMaxSkins = 4096;
    constexpr U32 kMaxArmatures = 256;

    // skins.bin: FileHeader (KIND_SKINS, count = skins), then SkinRec each. Fixed size so a file
    // is checked by size alone. Lanes >= joint_count are zero and ignored.
    struct SkinRec
    {
        U32 skin;          // capture skin id (the rigged rows' inst.skin_id); unique
        U32 armature;      // which armature's joint table it reads, < kMaxArmatures
        U32 joint_count;   // 1 .. kMaxSkinJoints
        U32 pad;           // 0
        F32 bind_shape[16];                       // mat4, column-major; m33 must be 1
        S32 joint_nums[kMaxSkinJoints];           // armature joint index of skin joint j, 0 .. 215
        F32 inv_bind[kMaxSkinJoints * 16];        // mat4 per skin joint, column-major
    };
    static_assert(sizeof(SkinRec) == 16 + 64 + 440 + 7040, "SkinRec layout changed: bump the version");

    // poses.bin: FileHeader (KIND_POSES, count = samples), then per sample a SampleHdr (sample
    // number, number of PoseHdr that follow) and that many { PoseHdr, joint_count x 16 F32 }:
    // the joint WORLD matrices of one armature in AGENT space, 12 floats per joint: the affine
    // matrix's upper 3x4 with row r at [r * 4 .. r * 4 + 3] (rotation / scale in columns 0..2,
    // translation in column 3; the last row 0 0 0 1 is implied) -- LLCleanSkinStore::packJointWorld's
    // layout, what the joint table holds; the pose the palette multiplies the inverse binds into. Sample numbers rise; time = sample / track_hz (the manifest's, shared with lights /
    // movers), held until the next record and looped over the track's length. A record lists
    // only the armatures whose pose CHANGED since the previous record; the FIRST record (sample 0)
    // must list every armature a skin uses, with joint_count covering every joint that skin names.
    // An armature's joint_count may differ between records (<= kMaxArmatureJoints): joints a
    // record does not carry keep their last value (identity before any).
    struct PoseHdr
    {
        U32 armature;      // < kMaxArmatures
        U32 joint_count;   // 1 .. kMaxArmatureJoints
        U32 pad0, pad1;    // 0
    };
    static_assert(sizeof(PoseHdr) == 16, "PoseHdr is 16 B");

    static_assert(std::is_trivially_copyable<LLVKLight>::value, "LLVKLight must stay POD");
    static_assert(std::is_trivially_copyable<LLVKProbe>::value, "LLVKProbe must stay POD");
    static_assert(std::is_trivially_copyable<LLVKProbeRegion>::value, "LLVKProbeRegion must stay POD");
    static_assert(std::is_trivially_copyable<LLCleanMaterialStore::TerrainRecord>::value, "TerrainRecord must stay POD");

    // Instance flag bits the capture keeps out (see "NOT CAPTURED").
    constexpr U32 kFlagRigged = 1u, kFlagHud = 8u, kFlagTerrain = 32u, kFlagWorn = 512u;

    // One decoded texture held for writing.
    struct Texture
    {
        LLUUID id;
        S32 discard = 0;
        LLPointer<LLImageRaw> raw;
    };

    // Everything a capture holds in memory before the writer thread puts it on disk.
    struct Data
    {
        std::vector<InstanceRec> instances;
        std::vector<GroupRec> groups;
        std::vector<U8> slots;               // SlotRec + arrays, back to back
        U32 slot_count = 0;
        std::vector<MaterialRec> materials;
        std::vector<TerrainRec> terrain;
        EnvRec env{};
        std::vector<LLVKProbe> probes;
        std::vector<LLVKProbeRegion> regions;
        std::vector<U8> lights;              // SampleHdr + LLVKLight[], back to back
        U32 light_samples = 0;
        std::vector<U8> movers;              // SampleHdr + MoverRec[], back to back
        U32 mover_samples = 0;
        std::vector<SkinRec> skins;          // version 2 (reader only)
        std::vector<U8> poses;               // SampleHdr + { PoseHdr + matrices }[], back to back
        U32 pose_samples = 0;
        std::vector<Texture> textures;
        std::string camera_json;
        std::string sky_xml, water_xml;      // the resolved LLSettingsSky / LLSettingsWater (LLSD XML)
        std::string manifest_extra;          // JSON object text merged into the manifest ("info")
    };

    // The bench directory root: BenchRoot, default <user app dir>/bench (~/.slipstreamviewer/bench).
    std::string benchRoot();
    // True when `dir` (or any directory above it) holds a .git entry: captures and reports must
    // never land inside a source checkout (they hold other people's content).
    bool insideSourceTree(const std::string& dir);
    // Create `dir` (and parents). False + err on failure or when insideSourceTree.
    bool makeOutputDir(const std::string& dir, std::string& err);
    // XXH64 of a whole file, hex; "" if unreadable.
    std::string fileDigest(const std::string& path);
    std::string hex64(U64 v);
    // Local time stamp "YYYYmmdd-HHMMSS".
    std::string stamp();
}

#endif // SL_BENCHSCENE_H
