/**
 * @file slbenchreplay.cpp
 * @brief Render benchmark replay: capture -> clean stores (#1304). See slbenchreplay.h.
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

#include "llviewerprecompiledheaders.h"

#include "slbenchreplay.h"

#include "hbxxh.h"
#include "lldir.h"
#include "llcleanqvertex.h"
#include "llcleanskinstore.h"
#include "llcleaninstancestore.h"
#include "llenvironment.h"
#include "llimagepng.h"
#include "llimagetga.h"
#include "llsdserialize.h"
#include "llsettingsvo.h"
#include "lltimer.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>

using namespace SLBenchScene;

constexpr size_t JOINT_FLOATS = LLCleanSkinStore::JOINT_WORLD_FLOATS;   // pose floats per joint (12)
static_assert(kMaxSkinJoints == LLCleanSkinStore::MAX_JOINTS, "skin joint limit drifted from the skin store");
static_assert(kMaxArmatureJoints == LLCleanSkinStore::MAX_AVATAR_JOINTS, "armature joint limit drifted from the skin store");

namespace
{
    constexpr U32 kSentinel = 0xFFFFFFFFu;
    enum KeyKind : U32 { KEY_ROW = 1, KEY_SLOT = 2, KEY_GROUP = 3, KEY_TERRAIN = 4 };

    bool readFile(const std::string& path, std::vector<U8>& out)
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bool ok = n >= 0;
        if (ok)
        {
            out.resize((size_t)n);
            ok = std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
        }
        std::fclose(f);
        return ok;
    }

    // A capture binary: header checked (magic, version, kind); payload = what follows.
    bool readBin(const std::string& dir, const char* name, U32 kind, U32 version, U64& count, std::vector<U8>& payload,
                 std::string& err)
    {
        std::vector<U8> raw;
        if (!readFile(dir + "/" + name, raw) || raw.size() < sizeof(FileHeader))
        {
            err = std::string("cannot read ") + name;
            return false;
        }
        FileHeader h;
        std::memcpy(&h, raw.data(), sizeof(h));
        if (std::memcmp(h.magic, kMagic, sizeof(h.magic)) != 0 || h.version != version || h.kind != kind)
        {
            err = std::string(name) + ": not a capture file of this format version (" + std::to_string(version) + ")";
            return false;
        }
        count = h.count;
        payload = std::move(raw);   // (no second copy of a big file: the header is cut off in place)
        payload.erase(payload.begin(), payload.begin() + sizeof(FileHeader));
        return true;
    }

    // A skins / poses payload past this is refused before anything is allocated for it.
    constexpr size_t kMaxSkinPayload = (size_t)1 << 30;

    bool allFinite(const F32* v, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            if (!std::isfinite(v[i])) return false;
        return true;
    }

    template <typename T>
    bool podArray(const std::vector<U8>& payload, U64 count, std::vector<T>& out, const char* name, std::string& err)
    {
        if (payload.size() != count * sizeof(T))
        {
            err = std::string(name) + ": size does not match its record count";
            return false;
        }
        out.resize((size_t)count);
        if (count) std::memcpy(out.data(), payload.data(), payload.size());
        return true;
    }

    // Offsets of each SampleHdr record of a track blob (records of elem bytes after each header).
    bool indexTrack(const std::vector<U8>& blob, U64 count, size_t elem, std::vector<size_t>& offsets,
                    std::vector<U32>& samples, const char* name, std::string& err)
    {
        size_t off = 0;
        for (U64 i = 0; i < count; ++i)
        {
            if (off + sizeof(SampleHdr) > blob.size())
            {
                err = std::string(name) + ": truncated";
                return false;
            }
            SampleHdr h;
            std::memcpy(&h, blob.data() + off, sizeof(h));
            const size_t next = off + sizeof(SampleHdr) + (size_t)h.count * elem;
            if (next > blob.size() || (!samples.empty() && h.sample < samples.back()))
            {
                err = std::string(name) + ": malformed";
                return false;
            }
            offsets.push_back(off);
            samples.push_back(h.sample);
            off = next;
        }
        return true;
    }

    // The viewer always writes the atmos post lanes non-zero (buildVulkanWorldFrame: exposure in
    // [0.5, 4]). A capture made outside the viewer (render2's Blender exporter) leaves the whole
    // atmos block zero, and read verbatim that is a black frame: exposure 0 scales everything to
    // 0 (clean_tonemap.frag: c *= p0.x * exp_scale), and zero blue / haze density a black sky
    // (clean_atmos_common.glsl skyHazeCore). No exposure = no environment block.
    bool envHasAtmos(const EnvRec& e)
    {
        const F32 exposure = e.atmos[13 * 4 + 0];
        return std::isfinite(exposure) && exposure > 0.f;
    }
}

SLBenchReplay::SLBenchReplay() = default;
SLBenchReplay::~SLBenchReplay() = default;

LLUUID SLBenchReplay::benchKey(U32 kind, U32 id)
{
    // "5LBE7C4" + kind + id: a fixed pattern no asset / object id uses (those are random v4).
    LLUUID k;
    static const U8 prefix[8] = { 0x5B, 0xE7, 0xC4, 0x00, 0x13, 0x04, 0xBE, 0x7C };
    std::memcpy(k.mData, prefix, 8);
    std::memcpy(k.mData + 8, &kind, 4);
    std::memcpy(k.mData + 12, &id, 4);
    return k;
}

bool SLBenchReplay::load(const std::string& dir, std::string& err)
{
    mDir = dir;
    std::vector<U8> manifest_bytes;
    if (!readFile(dir + "/manifest.json", manifest_bytes))
    {
        err = "no manifest.json in " + dir + " (not a capture, or an incomplete one)";
        return false;
    }
    mCaptureId = hex64(HBXXH64::digest(manifest_bytes.data(), manifest_bytes.size()));
    boost::system::error_code ec;
    const boost::json::value mv = boost::json::parse(
        std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()), ec);
    if (ec || !mv.is_object())
    {
        err = "manifest.json is not JSON";
        return false;
    }
    const boost::json::object& m = mv.as_object();
    const boost::json::value* fmt = m.if_contains("format");
    const boost::json::value* ver = m.if_contains("version");
    if (!fmt || !fmt->is_string() || fmt->as_string() != "slipstream-bench-capture" || !ver || !ver->is_number()
        || ver->to_number<U32>() < kFormatVersion || ver->to_number<U32>() > kFormatVersionMax)
    {
        err = "unsupported capture (format / version; this viewer reads versions " + std::to_string(kFormatVersion) + " to "
              + std::to_string(kFormatVersionMax) + ")";
        return false;
    }
    mVersion = ver->to_number<U32>();
    if (const boost::json::value* hz = m.if_contains("track_hz"); hz && hz->is_number())
        mTrackHz = llclamp((F32)hz->to_number<double>(), 1.f, 1000.f);

    // Every listed file: size and XXH64 first (a capture is either intact or refused).
    const boost::json::value* files = m.if_contains("files");
    if (!files || !files->is_object())
    {
        err = "manifest lists no files";
        return false;
    }
    for (const auto& kv : files->as_object())
    {
        const std::string name(kv.key());
        if (name.find("..") != std::string::npos || (!name.empty() && name[0] == '/'))
        {
            err = "manifest names a file outside the capture: " + name;
            return false;
        }
        const boost::json::object* fo = kv.value().if_object();
        const boost::json::value* want = fo ? fo->if_contains("xxh64") : nullptr;
        if (!want || !want->is_string())
        {
            err = "manifest entry without a checksum: " + name;
            return false;
        }
        const std::string got = fileDigest(dir + "/" + name);
        if (got != std::string(want->as_string()))
        {
            err = got.empty() ? "missing file " + name : "checksum mismatch: " + name;
            return false;
        }
    }

    try
    {
        U64 n = 0;
        std::vector<U8> payload;
        if (!readBin(dir, "instances.bin", KIND_INSTANCES, mVersion, n, payload, err)
            || !podArray(payload, n, mData.instances, "instances.bin", err)) return false;
        if (!readBin(dir, "groups.bin", KIND_GROUPS, mVersion, n, payload, err)
            || !podArray(payload, n, mData.groups, "groups.bin", err)) return false;
        if (!readBin(dir, "slots.bin", KIND_SLOTS, mVersion, n, mData.slots, err)) return false;
        mData.slot_count = (U32)n;
        if (!readBin(dir, "materials.bin", KIND_MATERIALS, mVersion, n, payload, err)
            || !podArray(payload, n, mData.materials, "materials.bin", err)) return false;
        if (!readBin(dir, "terrain.bin", KIND_TERRAIN, mVersion, n, payload, err)
            || !podArray(payload, n, mData.terrain, "terrain.bin", err)) return false;
        if (!readBin(dir, "env.bin", KIND_ENV, mVersion, n, payload, err)) return false;
        if (payload.size() < sizeof(EnvRec))
        {
            err = "env.bin: truncated";
            return false;
        }
        std::memcpy(&mData.env, payload.data(), sizeof(EnvRec));
        mHasEnv = envHasAtmos(mData.env);
        if (!mHasEnv)
            LL_WARNS("Bench") << "replay: env.bin has no atmosphere / post block (exposure 0: a capture made outside the "
                              << "viewer?); the sky, sun and post inputs come from the local environment ("
                              << (files->as_object().contains("sky.xml") ? "the capture's sky.xml" : "default sky")
                              << "), only water, probes and lights from the capture" << LL_ENDL;
        const size_t want = sizeof(EnvRec) + (size_t)mData.env.probe_count * sizeof(LLVKProbe)
                          + (size_t)mData.env.region_count * sizeof(LLVKProbeRegion);
        if (payload.size() != want)
        {
            err = "env.bin: size does not match its probe counts";
            return false;
        }
        mData.probes.resize(mData.env.probe_count);
        if (!mData.probes.empty())
            std::memcpy(mData.probes.data(), payload.data() + sizeof(EnvRec), mData.probes.size() * sizeof(LLVKProbe));
        mData.regions.resize(mData.env.region_count);
        if (!mData.regions.empty())
            std::memcpy(mData.regions.data(), payload.data() + sizeof(EnvRec) + mData.probes.size() * sizeof(LLVKProbe),
                        mData.regions.size() * sizeof(LLVKProbeRegion));
        if (!readBin(dir, "lights.bin", KIND_LIGHTS, mVersion, n, mData.lights, err)
            || !indexTrack(mData.lights, n, sizeof(LLVKLight), mLightOffsets, mLightSamples, "lights.bin", err)) return false;
        mData.light_samples = (U32)n;
        if (!readBin(dir, "movers.bin", KIND_MOVERS, mVersion, n, mData.movers, err)
            || !indexTrack(mData.movers, n, sizeof(MoverRec), mMoverOffsets, mMoverSamples, "movers.bin", err)) return false;
        mData.mover_samples = (U32)n;
        if (!loadSkins(files->as_object().contains("skins.bin"), files->as_object().contains("poses.bin"), err)) return false;
    }
    catch (const std::bad_alloc&)
    {
        err = "out of memory reading the capture";
        return false;
    }

    // Textures (verified above) and the capture camera.
    if (const boost::json::value* tex = m.if_contains("textures"); tex && tex->is_array())
    {
        for (const boost::json::value& t : tex->as_array())
        {
            const boost::json::object* to = t.if_object();
            const boost::json::value* id = to ? to->if_contains("id") : nullptr;
            if (!id || !id->is_string()) continue;
            TexEntry e;
            if (!e.id.set(std::string(id->as_string()), false)) continue;
            e.file = "textures/" + e.id.asString() + ".png";
            if (files->as_object().contains(e.file)) mTexFiles.push_back(e);
        }
    }
    // The environment settings (verified above when listed).
    const std::pair<const char*, LLSD*> envFiles[] = { { "sky.xml", &mSkySettings },
                                                       { "water.xml", &mWaterSettings } };
    for (const auto& f : envFiles)
    {
        if (!files->as_object().contains(f.first)) continue;
        std::ifstream in(dir + "/" + f.first, std::ios::binary);
        if (!in || LLSDSerialize::fromXML(*f.second, in) <= 0 || !f.second->isMap())
        {
            err = std::string(f.first) + ": not LLSD";
            return false;
        }
    }
    std::string cam_err;
    if (!mCamera.load(dir + "/camera.json", cam_err))
        LL_WARNS("Bench") << "replay: capture camera unusable (" << cam_err << "); a --bench-path is required" << LL_ENDL;
    for (size_t i = 0; i < mData.instances.size(); ++i) mRowIndex[mData.instances[i].row] = i;
    LL_INFOS("Bench") << "replay: loaded " << dir << " (capture " << mCaptureId << "): " << mData.instances.size()
                      << " rows, " << mData.slot_count << " slots, " << mData.materials.size() << " materials, "
                      << mTexFiles.size() << " textures, " << mData.light_samples << " light samples" << LL_ENDL;
    if (!mData.skins.empty())
        LL_INFOS("Bench") << "replay: skins: " << mData.skins.size() << " skins, " << mArmatureIds.size() << " armatures, "
                          << mData.pose_samples << " pose samples, " << mRiggedRows << " rigged rows (format version "
                          << mVersion << ")" << LL_ENDL;
    return true;
}

// Version 2: skins.bin + poses.bin, validated completely here (counts, sizes, ranges, finite
// matrices, every rigged row's skin) so injection and the per-frame pose never meet bad data.
bool SLBenchReplay::loadSkins(bool has_skins_file, bool has_poses_file, std::string& err)
{
    U32 rigged_rows = 0;
    for (const InstanceRec& r : mData.instances)
        if ((r.inst.flags & kFlagRigged) && !(r.inst.flags & kFlagTerrain)) ++rigged_rows;
    mRiggedRows = rigged_rows;

    const bool has_skins = mVersion >= 2 && has_skins_file;
    const bool has_poses = mVersion >= 2 && has_poses_file;
    if (!has_skins && !has_poses)
    {
        if (rigged_rows)
        {
            err = "instances.bin: " + std::to_string(rigged_rows) + " rigged rows but the capture has no skins.bin / poses.bin";
            return false;
        }
        return true;
    }
    if (has_skins != has_poses)
    {
        err = "a capture needs both skins.bin and poses.bin (or neither)";
        return false;
    }

    U64 n = 0;
    std::vector<U8> payload;
    if (!readBin(mDir, "skins.bin", KIND_SKINS, mVersion, n, payload, err)) return false;
    if (n > kMaxSkins || payload.size() > kMaxSkinPayload)
    {
        err = "skins.bin: too many skins (limit " + std::to_string(kMaxSkins) + ")";
        return false;
    }
    if (!podArray(payload, n, mData.skins, "skins.bin", err)) return false;
    std::unordered_map<U32, U32> max_joint;   // armature -> highest joint index its skins name
    for (const SkinRec& k : mData.skins)
    {
        const std::string who = "skins.bin: skin " + std::to_string(k.skin) + ": ";
        if (!mSkinIndex.emplace(k.skin, (U32)(&k - mData.skins.data())).second)
        {
            err = who + "duplicate skin id";
            return false;
        }
        if (k.armature >= kMaxArmatures || k.joint_count == 0 || k.joint_count > kMaxSkinJoints)
        {
            err = who + "armature or joint count out of range";
            return false;
        }
        if (!allFinite(k.bind_shape, 16) || !allFinite(k.inv_bind, (size_t)k.joint_count * 16) || k.bind_shape[15] != 1.f)
        {
            err = who + "bind shape / inverse bind matrices not finite (or bind shape not affine)";
            return false;
        }
        U32& mj = max_joint.emplace(k.armature, 0u).first->second;
        for (U32 j = 0; j < k.joint_count; ++j)
        {
            if (k.joint_nums[j] < 0 || k.joint_nums[j] >= (S32)kMaxArmatureJoints)
            {
                err = who + "joint number out of range";
                return false;
            }
            mj = std::max(mj, (U32)k.joint_nums[j]);
        }
    }
    for (const auto& kv : max_joint) mArmatureIds.push_back(kv.first);
    std::sort(mArmatureIds.begin(), mArmatureIds.end());

    // Rigged rows: each names a skin.
    for (const InstanceRec& r : mData.instances)
        if ((r.inst.flags & kFlagRigged) && !(r.inst.flags & kFlagTerrain) && !mSkinIndex.count(r.inst.skin_id))
        {
            err = "instances.bin: rigged row " + std::to_string(r.row) + " names skin " + std::to_string(r.inst.skin_id)
                  + " which skins.bin does not hold";
            return false;
        }

    // Pose track.
    if (!readBin(mDir, "poses.bin", KIND_POSES, mVersion, n, mData.poses, err)) return false;
    if (mData.poses.size() > kMaxSkinPayload)
    {
        err = "poses.bin: too large";
        return false;
    }
    mData.pose_samples = (U32)std::min<U64>(n, 0xFFFFFFFFull);
    size_t off = 0;
    std::unordered_map<U32, U32> first_joints;   // armature -> joint_count in the first record
    for (U64 i = 0; i < n; ++i)
    {
        if (off + sizeof(SampleHdr) > mData.poses.size())
        {
            err = "poses.bin: truncated";
            return false;
        }
        SampleHdr h;
        std::memcpy(&h, mData.poses.data() + off, sizeof(h));
        if (!mPoseSamples.empty() && h.sample < mPoseSamples.back())
        {
            err = "poses.bin: sample numbers go backwards";
            return false;
        }
        mPoseOffsets.push_back(off);
        mPoseSamples.push_back(h.sample);
        size_t p = off + sizeof(SampleHdr);
        for (U32 a = 0; a < h.count; ++a)
        {
            PoseHdr ph;
            if (p + sizeof(ph) > mData.poses.size())
            {
                err = "poses.bin: truncated";
                return false;
            }
            std::memcpy(&ph, mData.poses.data() + p, sizeof(ph));
            if (ph.armature >= kMaxArmatures || ph.joint_count == 0 || ph.joint_count > kMaxArmatureJoints)
            {
                err = "poses.bin: armature or joint count out of range";
                return false;
            }
            const size_t bytes = (size_t)ph.joint_count * JOINT_FLOATS * sizeof(F32);
            if (p + sizeof(ph) + bytes > mData.poses.size())
            {
                err = "poses.bin: truncated";
                return false;
            }
            const U8* m = mData.poses.data() + p + sizeof(ph);
            for (size_t f = 0; f < (size_t)ph.joint_count * JOINT_FLOATS; ++f)
            {
                F32 v;
                std::memcpy(&v, m + f * sizeof(F32), sizeof(v));
                if (!std::isfinite(v))
                {
                    err = "poses.bin: non-finite matrix";
                    return false;
                }
            }
            if (i == 0) first_joints[ph.armature] = ph.joint_count;
            p += sizeof(ph) + bytes;
        }
        off = p;
    }
    if (off != mData.poses.size())
    {
        err = "poses.bin: size does not match its records";
        return false;
    }
    if (n == 0 && !mArmatureIds.empty())
    {
        err = "poses.bin: no samples";
        return false;
    }
    for (const auto& kv : max_joint)
    {
        auto f = first_joints.find(kv.first);
        if (f == first_joints.end() || f->second <= kv.second)
        {
            err = "poses.bin: the first sample does not pose armature " + std::to_string(kv.first) + " up to joint "
                  + std::to_string(kv.second);
            return false;
        }
    }
    return true;
}

void SLBenchReplay::applyPoseRecord(size_t rec)
{
    if (rec >= mPoseOffsets.size()) return;
    LLCleanSkinStore& skins = LLCleanSkinStore::instance();
    SampleHdr h;
    std::memcpy(&h, mData.poses.data() + mPoseOffsets[rec], sizeof(h));
    const U8* p = mData.poses.data() + mPoseOffsets[rec] + sizeof(SampleHdr);
    for (U32 a = 0; a < h.count; ++a)
    {
        PoseHdr ph;
        std::memcpy(&ph, p, sizeof(ph));   // (validated at load)
        const U8* m = p + sizeof(ph);
        p = m + (size_t)ph.joint_count * JOINT_FLOATS * sizeof(F32);
        auto it = mArmatureSlot.find(ph.armature);
        if (it == mArmatureSlot.end()) continue;   // an armature no skin uses
        std::vector<F32>& tmp = mPoseTmp;
        tmp.resize((size_t)ph.joint_count * JOINT_FLOATS);
        std::memcpy(tmp.data(), m, tmp.size() * sizeof(F32));
        skins.replaySetPose(it->second, 0, ph.joint_count, tmp.data());
    }
}

bool SLBenchReplay::injectStep(std::string& err)
{
    if (mInjected) return true;
    LLCleanMeshAsset& mesh = LLCleanMeshAsset::instance();
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    LLCleanMaterialStore& mats = LLCleanMaterialStore::instance();
    const F64 deadline = LLTimer::getTotalSeconds() + kInjectSliceSec;
    auto out_of_time = [&]() { return LLTimer::getTotalSeconds() >= deadline; };

    if (mStage == STAGE_INIT)
    {
        mesh.init();
        store.init();
        mats.init();   // material 0 = default white
        // The captured sky and water as a fixed local environment, at once: the frame's
        // environment inputs come from env.bin, but some renderer modules read LLEnvironment.
        // The replay never logs in, so the login step that initialises LLEnvironment never runs:
        // initialise it here (with no region its settings request is skipped). A capture without
        // sky/water settings (one exported from Blender) gets the defaults.
        LLSettingsSky::ptr_t sky = mSkySettings.isMap() ? LLSettingsVOSky::buildSky(mSkySettings) : LLSettingsSky::ptr_t();
        LLSettingsWater::ptr_t water = mWaterSettings.isMap() ? LLSettingsVOWater::buildWater(mWaterSettings) : LLSettingsWater::ptr_t();
        if (!sky) sky = LLSettingsVOSky::buildDefaultSky();
        if (!water) water = LLSettingsVOWater::buildDefaultWater();
        if (!mHasEnv) applyCaptureSun(*sky);
        LLEnvironment& env = LLEnvironment::instance();
        env.initSingleton();
        env.setEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::fixedEnvironment_t(sky, water));
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        if (!mHasEnv) registerBundledCloudNoise(sky->getCloudNoiseTextureId());
        mStage = STAGE_TEXTURES;
        mCursor = 0;
    }

    // Textures first: materials name them by UUID and the renderer resolves those through
    // the image list. Registered as local images (not re-sourceable: their CPU copy stays).
    while (mStage == STAGE_TEXTURES)
    {
        if (mCursor >= mTexFiles.size())
        {
            mStage = STAGE_MATERIALS;
            break;
        }
        if (out_of_time()) return true;
        const TexEntry& e = mTexFiles[mCursor++];
        if (LLViewerTextureManager::findFetchedTexture(e.id, TEX_LIST_STANDARD))
        {
            ++mTexPreexisting;   // already known (a built-in image): left as it is
            continue;
        }
        const std::string path = mDir + "/" + e.file;
        LLPointer<LLImagePNG> png = new LLImagePNG();
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        if (!png->load(path) || !png->decode(raw, 0.f) || !raw->getData())
        {
            ++mTexFailed;   // never fatal: the material draws without it
            continue;
        }
        LLPointer<LLViewerFetchedTexture> tex = new LLViewerFetchedTexture("file://" + path, FTT_LOCAL_FILE, e.id, true);
        tex->setFromRaw(0, raw);
        gTextureList.addImage(tex, TEX_LIST_STANDARD);
        mTextures.push_back(tex);
        ++mTexLoaded;
    }

    // Materials, terrain records (cheap: one pass).
    if (mStage == STAGE_MATERIALS)
    {
        mMatMap[0] = 0;
        for (const MaterialRec& r : mData.materials)
            mMatMap[r.id] = r.id == 0 ? 0u : mats.getOrCreate(fromRec(r));
        for (const TerrainRec& t : mData.terrain)
        {
            LLCleanMaterialStore::TerrainRecord rec = t.rec;
            for (U32& m : rec.mat) m = mapMat(m);
            const U64 key = ((U64)0x5BE7C400u << 32) | t.idx;
            mTrecMap[t.idx] = mats.setTerrainRecord(key, rec);
        }
        mStage = STAGE_SLOTS;
        mCursor = 0;
        mSlotOff = 0;
    }

    // Geometry: the captured mirror decoded and re-encoded in its own quantization domain
    // (bit-identical lattice), one raw slot per capture slot.
    while (mStage == STAGE_SLOTS)
    {
        if (mCursor >= mData.slot_count)
        {
            mStage = STAGE_GROUPS;
            break;
        }
        if (out_of_time()) return true;
        const std::vector<U8>& b = mData.slots;
        if (mSlotOff + sizeof(SlotRec) > b.size())
        {
            err = "slots.bin: truncated";
            return false;
        }
        SlotRec r;
        std::memcpy(&r, b.data() + mSlotOff, sizeof(r));
        const size_t off = mSlotOff + sizeof(r);
        const size_t vbytes = (size_t)(r.nv + LLCleanMeshAsset::QV_HEADER) * sizeof(LLCleanMeshAsset::QVertex);
        const size_t ibytes = (size_t)r.ni * sizeof(U16);
        const size_t wbytes = r.has_weights ? (size_t)r.nv * sizeof(LLCleanMeshAsset::MirrorWeight) : 0;
        if (off + vbytes + ibytes + wbytes > b.size())
        {
            err = "slots.bin: truncated";
            return false;
        }
        mSlotOff = off + vbytes + ibytes + wbytes;
        ++mCursor;

        std::vector<LLCleanMeshAsset::QVertex> range((size_t)r.nv + LLCleanMeshAsset::QV_HEADER);
        std::memcpy(range.data(), b.data() + off, vbytes);
        std::vector<U16> idx16(r.ni);
        if (ibytes) std::memcpy(idx16.data(), b.data() + off + vbytes, ibytes);
        std::vector<LLCleanMeshAsset::MirrorWeight> wts(r.has_weights ? r.nv : 0);
        if (wbytes) std::memcpy(wts.data(), b.data() + off + vbytes + ibytes, wbytes);

        LLCleanMeshAsset::QHeader hdr;
        LLCleanQV::readHeader(range.data(), hdr);
        std::vector<LLCleanMeshAsset::MirrorVertex> verts;
        LLCleanMeshAsset::decodeVerts(range.data(), r.nv, verts);
        std::vector<U32> idx32(idx16.begin(), idx16.end());
        const U32 ns = mesh.captureRawSlot(benchKey(KEY_SLOT, r.slot), 0, 0, verts.data(),
                                           wts.empty() ? nullptr : wts.data(), idx32.data(), r.nv, r.ni,
                                           /*reclaimable*/ false, &hdr);
        if (ns == kSentinel) { ++mSlotsFailed; continue; }
        mSlotMap[r.slot] = ns;
        if (r.has_weights) mSlotWeighted.insert(r.slot);
        ++mSlotsLoaded;
    }

    // LOD groups: each capture group becomes one group keyed by its capture id.
    if (mStage == STAGE_GROUPS)
    {
        for (const GroupRec& g : mData.groups)
        {
            U32 ng = kSentinel;
            bool weighted = true;
            for (S32 l = 0; l < LLCleanMeshAsset::NUM_LOD; ++l)
            {
                auto it = mSlotMap.find(g.slot[l]);
                if (g.slot[l] == LLCleanMeshAsset::LOD_SENTINEL || it == mSlotMap.end()) continue;
                ng = mesh.registerLodSlot(benchKey(KEY_GROUP, g.gid), 0, l, it->second);
                if (!mSlotWeighted.count(g.slot[l])) weighted = false;
            }
            if (ng != kSentinel)
            {
                mGroupMap[g.gid] = ng;
                if (weighted) mGroupWeighted.insert(ng);
            }
        }
        mStage = STAGE_SKINS;
        mCursor = 0;
    }

    // Skins (version 2): one joint-table slot per armature, one skin entry per skin, then the
    // first pose so frame 0 is already posed.
    if (mStage == STAGE_SKINS)
    {
        LLCleanSkinStore& skins = LLCleanSkinStore::instance();
        for (U32 a : mArmatureIds)
        {
            const U32 slot = skins.replayArmature();
            if (slot == kSentinel)
            {
                err = "skin store: no joint-table slot left for armature " + std::to_string(a);
                return false;
            }
            mArmatureSlot[a] = slot;
        }
        for (const SkinRec& k : mData.skins)
        {
            const U32 id = skins.replaySkin(mArmatureSlot[k.armature], k.joint_count, k.joint_nums, k.inv_bind, k.bind_shape);
            if (id == kSentinel)
            {
                err = "skin store: cannot register skin " + std::to_string(k.skin);
                return false;
            }
            mSkinMap[k.skin] = id;
            ++mSkinsLoaded;
        }
        if (!mPoseOffsets.empty())
        {
            applyPoseRecord(0);
            mPoseRec = 0;
        }
        mStage = STAGE_ROWS;
        mCursor = 0;
    }

    // Rows (remapped in place: the tracks re-upsert these).
    while (mStage == STAGE_ROWS)
    {
        if (mCursor >= mData.instances.size())
        {
            mStage = STAGE_DONE;
            break;
        }
        if ((mCursor & 1023) == 0 && out_of_time()) return true;
        InstanceRec& r = mData.instances[mCursor++];
        LLCleanInstanceStore::Instance& in = r.inst;
        auto g = mGroupMap.find(in.lod_group_id);
        if (g == mGroupMap.end())
        {
            mRowIndex.erase(r.row);   // no geometry survived: not drawn, and the tracks leave it alone
            continue;
        }
        in.lod_group_id = g->second;
        in.material_id = mapMat(in.material_id);
        if (in.flags & kFlagTerrain)
        {
            auto t = mTrecMap.find(in.skin_id);
            in.skin_id = t == mTrecMap.end() ? kSentinel : t->second;
        }
        else if (in.flags & kFlagRigged)
        {
            // The renderer only skins a slot that carries weights (anything else draws as a static
            // bind-pose mesh): refuse the capture rather than replay a silently wrong avatar.
            if (!mGroupWeighted.count(in.lod_group_id))
            {
                err = "rigged row " + std::to_string(r.row) + ": its geometry has no skin weights";
                return false;
            }
            auto k = mSkinMap.find(in.skin_id);
            if (k == mSkinMap.end())
            {
                err = "rigged row " + std::to_string(r.row) + ": skin " + std::to_string(in.skin_id) + " was not registered";
                return false;
            }
            in.skin_id = k->second;
            ++mRiggedInjected;
        }
        store.upsert(benchKey(KEY_ROW, r.row), 0, in, r.active != 0);
        ++mRowsLoaded;
    }

    mInjected = true;
    LL_INFOS("Bench") << "replay: injected " << mRowsLoaded << " rows, " << mSlotsLoaded << " slots (" << mSlotsFailed
                      << " failed), " << mMatMap.size() << " materials, " << mTrecMap.size() << " terrain records, "
                      << mTexLoaded << " textures (" << mTexFailed << " failed, " << mTexPreexisting << " built-in)"
                      << LL_ENDL;
    if (!mData.skins.empty())
        LL_INFOS("Bench") << "replay: injected skins " << mSkinsLoaded << " (0 failed), armatures " << mArmatureSlot.size()
                          << ", rigged rows " << mRiggedInjected << ", pose samples " << mData.pose_samples << LL_ENDL;
    // The remap tables are only needed while injecting.
    mMatMap.clear(); mTrecMap.clear(); mSlotMap.clear(); mGroupMap.clear();
    mSlotWeighted.clear(); mGroupWeighted.clear(); mSkinMap.clear();
    return true;
}

void SLBenchReplay::applyCaptureSun(LLSettingsSky& sky)
{
    // An environment-less capture (no atmosphere block: one exported from Blender) still names
    // its sun in env.bin. The local sky's sun is turned to that direction, so the frame (sky disc,
    // atmosphere, sunlight colour, shadows) is lit from where the scene was authored to be, for
    // both pipelines: the current one reads LLEnvironment, render2 the frame built from it. The
    // sunlight colour stays the sky's (the atmosphere at that elevation, viewer units). Without a
    // usable sun (zero, non-finite, at or below the horizon) the sky keeps its own.
    const EnvRec& e = mData.env;
    LLVector3 d(e.sun_dir[0], e.sun_dir[1], e.sun_dir[2]);
    if (!std::isfinite(d.mV[0]) || !std::isfinite(d.mV[1]) || !std::isfinite(d.mV[2]) || d.lengthSquared() < 1e-6f)
    {
        LL_INFOS("Bench") << "replay: the capture names no sun; the sky's own sun lights the frame" << LL_ENDL;
        return;
    }
    d.normalize();
    if (d.mV[2] < 0.02f)
    {
        LL_WARNS("Bench") << "replay: the capture's sun " << d << " is at or below the horizon; the sky's own sun is kept" << LL_ENDL;
        return;
    }
    LLQuaternion q;
    q.shortestArc(LLVector3::x_axis, d);   // (a sky's sun direction is +X turned by its sun rotation)
    sky.setSunRotation(q);
    // Its colour too, when it has one (sceneInputs): an exported capture's sunlight, in frame units.
    mCaptureSun = (e.sun_color[0] > 0.f || e.sun_color[1] > 0.f || e.sun_color[2] > 0.f)
               && std::isfinite(e.sun_color[0] + e.sun_color[1] + e.sun_color[2]);
    LL_INFOS("Bench") << "replay: the sky's sun turned to the capture's sun " << d << " (sky now reports "
                      << sky.getSunDirection() << ")" << LL_ENDL;
}

void SLBenchReplay::registerBundledCloudNoise(const LLUUID& noise_id)
{
    // An environment-less replay draws the sky from LLEnvironment (hasEnvironment), and the
    // default sky's cloud noise is an asset: with no login there is no texture capability, so it
    // came from whatever the local j2c cache held (often a partial entry decoded past its data:
    // streaked clouds) or not at all -- different per machine. The viewer ships the WindLight
    // cloud noise (app_settings/windlight/clouds2.tga): it stands in under the sky's noise id, as
    // a local image (the same route as the capture's own textures), before anything fetches it.
    // (Only the default sky's noise: a capture's sky.xml naming its own is left to the fetch.)
    const LLUUID& id = noise_id;
    if (id.isNull() || id != LLSettingsSky::GetDefaultCloudNoiseTextureId()) return;
    if (LLViewerTextureManager::findFetchedTexture(id, TEX_LIST_STANDARD))
    {
        LL_WARNS("Bench") << "replay: the cloud noise " << id << " is already loaded; the bundled one is not used" << LL_ENDL;
        return;
    }
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "windlight", "clouds2.tga");
    LLPointer<LLImageTGA> tga = new LLImageTGA(path);
    LLPointer<LLImageRaw> raw = new LLImageRaw();
    if (!tga->getData() || !tga->decode(raw, 0.f) || !raw->getData())
    {
        LL_WARNS("Bench") << "replay: cannot load the bundled cloud noise " << path << LL_ENDL;
        return;
    }
    LLPointer<LLViewerFetchedTexture> tex = new LLViewerFetchedTexture("file://" + path, FTT_LOCAL_FILE, id, true);
    tex->setFromRaw(0, raw);
    gTextureList.addImage(tex, TEX_LIST_STANDARD);
    mTextures.push_back(tex);
    LL_INFOS("Bench") << "replay: cloud noise " << id << " from the bundled " << path << " (" << raw->getWidth() << "x"
                      << raw->getHeight() << ")" << LL_ENDL;
}

U32 SLBenchReplay::mapMat(U32 id) const
{
    auto it = mMatMap.find(id);
    return it == mMatMap.end() ? 0u : it->second;
}


size_t SLBenchReplay::recordAt(const std::vector<U32>& samples, F64 world_time, F32 hz)
{
    if (samples.empty()) return 0;
    const U64 span = (U64)samples.back() + 1;
    const U64 s = (U64)std::max<F64>(0.0, std::floor(world_time * hz)) % span;
    auto it = std::upper_bound(samples.begin(), samples.end(), (U32)s);
    return it == samples.begin() ? 0 : (size_t)(it - samples.begin()) - 1;
}

void SLBenchReplay::resetMovers()
{
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    for (U32 row : mTouched)
    {
        auto it = mRowIndex.find(row);
        if (it == mRowIndex.end()) continue;
        const InstanceRec& r = mData.instances[it->second];
        store.upsert(benchKey(KEY_ROW, row), 0, r.inst, r.active != 0);
    }
    mTouched.clear();
}

void SLBenchReplay::applyMoverRecord(size_t rec)
{
    if (rec >= mMoverOffsets.size()) return;
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    SampleHdr h;
    std::memcpy(&h, mData.movers.data() + mMoverOffsets[rec], sizeof(h));
    const U8* p = mData.movers.data() + mMoverOffsets[rec] + sizeof(SampleHdr);
    for (U32 i = 0; i < h.count; ++i, p += sizeof(MoverRec))
    {
        MoverRec m;
        std::memcpy(&m, p, sizeof(m));
        auto it = mRowIndex.find(m.row);
        if (it == mRowIndex.end()) continue;
        const LLUUID key = benchKey(KEY_ROW, m.row);
        if (m.kind == MOVER_GONE)
        {
            store.release(key, 0);
        }
        else
        {
            LLCleanInstanceStore::Instance in = mData.instances[it->second].inst;
            std::memcpy(in.model_matrix, m.model, sizeof(in.model_matrix));
            std::memcpy(in.spin_axis, m.spin_axis, sizeof(in.spin_axis));
            std::memcpy(in.spin_pivot, m.spin_pivot, 3 * sizeof(F32));   // (.w: the slot's UV density lane)
            store.upsert(key, 0, in, true);
        }
        mTouched.push_back(m.row);
    }
}

void SLBenchReplay::applyTracks(F64 world_time)
{
    if (!mInjected) return;
    mLightRec = recordAt(mLightSamples, world_time, mTrackHz);
    if (!mPoseOffsets.empty())
    {
        // The pose track: records are deltas (only the armatures that changed), so a loop replays
        // from record 0 (which lists every armature), exactly like the movers below.
        const S64 prec = (S64)recordAt(mPoseSamples, world_time, mTrackHz);
        if (prec < mPoseRec) mPoseRec = -1;
        for (S64 r = mPoseRec + 1; r <= prec; ++r) applyPoseRecord((size_t)r);
        mPoseRec = prec;
    }
    if (mMoverOffsets.empty()) return;
    const S64 rec = (S64)recordAt(mMoverSamples, world_time, mTrackHz);
    if (rec < mMoverRec)
    {
        resetMovers();   // the track looped: back to the snapshot poses, then forward
        mMoverRec = -1;
    }
    for (S64 r = mMoverRec + 1; r <= rec; ++r) applyMoverRecord((size_t)r);
    mMoverRec = rec;
}

void SLBenchReplay::sceneInputs(LLVKWorldFrame& wf) const
{
    if (mHasEnv)
    {
        envToFrame(mData.env, wf);
    }
    else
    {
        // (the rest came from LLEnvironment in the frame builder; see hasEnvironment)
        const EnvRec& e = mData.env;
        if (mCaptureSun)
        {
            // The capture's own sun, exactly (applyCaptureSun turned the sky to it): both
            // pipelines light with env.bin's direction and colour, as render2's lab does.
            std::memcpy(wf.sun_dir, e.sun_dir, sizeof(e.sun_dir));
            std::memcpy(wf.sun_color, e.sun_color, sizeof(e.sun_color));
        }
        std::memcpy(wf.water_fog, e.water_fog, sizeof(e.water_fog));
        std::memcpy(wf.water, e.water, sizeof(e.water));
        std::memcpy(wf.water_normal, e.water_normal, sizeof(e.water_normal));
        wf.probe_agent_cell[0] = e.probe_agent_cell[0];
        wf.probe_agent_cell[1] = e.probe_agent_cell[1];
    }
    wf.probes = mData.probes;
    wf.probe_regions = mData.regions;
    wf.lights.clear();
    wf.light_takeover.clear();   // (face links are not captured)
    if (mLightRec < mLightOffsets.size())
    {
        SampleHdr h;
        std::memcpy(&h, mData.lights.data() + mLightOffsets[mLightRec], sizeof(h));
        wf.lights.resize(h.count);
        if (h.count)
            std::memcpy(wf.lights.data(), mData.lights.data() + mLightOffsets[mLightRec] + sizeof(SampleHdr),
                        (size_t)h.count * sizeof(LLVKLight));
    }
}

F64 SLBenchReplay::trackSeconds() const
{
    const U32 last = std::max(mLightSamples.empty() ? 0u : mLightSamples.back(),
                              std::max(mMoverSamples.empty() ? 0u : mMoverSamples.back(),
                                       mPoseSamples.empty() ? 0u : mPoseSamples.back()));
    return (F64)(last + 1) / mTrackHz;
}

LLSD SLBenchReplay::info() const
{
    LLSD r;
    r["dir"] = mDir;
    r["capture_id"] = mCaptureId;
    r["rows"] = (S32)mRowsLoaded;
    r["slots"] = (S32)mSlotsLoaded;
    r["slots_failed"] = (S32)mSlotsFailed;
    r["materials"] = (S32)mData.materials.size();
    r["textures"] = (S32)mTexLoaded;
    r["textures_failed"] = (S32)mTexFailed;
    r["textures_builtin"] = (S32)mTexPreexisting;
    r["skins"] = (S32)mSkinsLoaded;
    r["armatures"] = (S32)mArmatureSlot.size();
    r["rigged_rows"] = (S32)mRiggedInjected;
    r["pose_samples"] = (S32)mData.pose_samples;
    r["track_seconds"] = trackSeconds();
    return r;
}
