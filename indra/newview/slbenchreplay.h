/**
 * @file slbenchreplay.h
 * @brief Render benchmark: feed the renderer's stores from a scene capture (#1304).
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

#ifndef SL_BENCHREPLAY_H
#define SL_BENCHREPLAY_H

#include "slbenchpath.h"
#include "slbenchscene.h"

#include "llpointer.h"
#include "llsd.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLViewerFetchedTexture;
class LLSettingsSky;

// REPLAY. load() reads and verifies a capture (format version, every file's size and XXH64)
// without touching the renderer; inject() puts it into the clean stores the way the live
// ingest would (textures under their own UUIDs as local images, materials, terrain records,
// geometry through captureRawSlot with the captured quantization domain, LOD groups, rows),
// remapping every capture id to the id the store hands out. Per frame, applyTracks() moves
// the rows to the mover track's pose for the (fixed-step) world time and sceneInputs() gives
// the frame the captured environment, probes and that sample's lights; both tracks loop.
class SLBenchReplay
{
public:
    SLBenchReplay();
    ~SLBenchReplay();   // (out of line: the held textures' type is incomplete here)

    bool load(const std::string& dir, std::string& err);
    // One time-budgeted slice of the injection (call once per frame until injected()); false +
    // err on a malformed capture. Slicing keeps the main loop (and its watchdog) running.
    bool injectStep(std::string& err);
    bool injected() const { return mInjected; }

    void applyTracks(F64 world_time);
    void sceneInputs(LLVKWorldFrame& wf) const;
    // False for a capture whose env.bin holds no atmosphere / post block (exposure lane 0: one
    // made outside the viewer, e.g. by render2's Blender exporter). The frame then derives its
    // sky, sun, clouds and post inputs from LLEnvironment -- the capture's sky.xml / water.xml,
    // else the defaults (injectStep) -- as a live frame does; the capture keeps its water lanes
    // (no region: the live water height would be the 20 m default), probes and lights.
    bool hasEnvironment() const { return mHasEnv; }

    const std::string& dir() const { return mDir; }
    const std::string& captureId() const { return mCaptureId; }   // XXH64 of the manifest
    const SLBenchPath& camera() const { return mCamera; }
    F64 trackSeconds() const;
    LLSD info() const;   // counts for the report

private:
    // Index of the track record in effect at world_time (looped over the track's length).
    static size_t recordAt(const std::vector<U32>& sample_nums, F64 world_time, F32 hz);
    void resetMovers();
    void applyMoverRecord(size_t rec);
    static LLUUID benchKey(U32 kind, U32 id);   // synthetic store keys (never a real asset id)

    U32 mapMat(U32 capture_id) const;
    // Environment-less capture: the default sky's cloud noise from the viewer's own files.
    void registerBundledCloudNoise(const LLUUID& noise_id);
    // Environment-less capture: the local sky's sun turned to the capture's env.bin sun.
    void applyCaptureSun(LLSettingsSky& sky);
    bool loadSkins(bool has_skins_file, bool has_poses_file, std::string& err);   // version 2: skins.bin + poses.bin
    void applyPoseRecord(size_t rec);

    static constexpr F64 kInjectSliceSec = 0.02;   // main-thread time per injectStep
    enum Stage { STAGE_INIT, STAGE_TEXTURES, STAGE_MATERIALS, STAGE_SLOTS, STAGE_GROUPS, STAGE_SKINS, STAGE_ROWS, STAGE_DONE };
    Stage mStage = STAGE_INIT;
    size_t mCursor = 0;    // item within the stage
    size_t mSlotOff = 0;   // byte offset of the next slot record
    // capture id -> store id, while injecting
    std::unordered_map<U32, U32> mMatMap, mTrecMap, mSlotMap, mGroupMap;

    U32 mVersion = 1;   // the capture's format version
    std::string mDir, mCaptureId;
    bool mInjected = false;
    bool mHasEnv = true;
    bool mCaptureSun = false;      // applyCaptureSun: env.bin's sun direction + colour light the frame
    F32 mTrackHz = 30.f;
    SLBenchScene::Data mData;
    std::vector<size_t> mLightOffsets;   // per record: offset of its SampleHdr in mData.lights
    std::vector<U32>    mLightSamples;   // per record: its sample number
    std::vector<size_t> mMoverOffsets;   // per record: offset in mData.movers
    std::vector<U32>    mMoverSamples;
    std::vector<size_t> mPoseOffsets;    // per record: offset in mData.poses
    std::vector<U32>    mPoseSamples;
    std::vector<F32>    mPoseTmp;
    std::unordered_map<U32, U32> mSkinIndex;      // capture skin id -> index in mData.skins
    std::vector<U32> mArmatureIds;                // capture armature ids the skins use (sorted)
    std::unordered_map<U32, U32> mArmatureSlot;   // capture armature -> joint-table slot
    std::unordered_map<U32, U32> mSkinMap;        // capture skin -> store skin id (while injecting)
    std::unordered_set<U32> mSlotWeighted, mGroupWeighted;   // while injecting
    S64 mPoseRec = -1;
    U32 mSkinsLoaded = 0, mRiggedRows = 0, mRiggedInjected = 0;
    size_t mLightRec = 0;                // the light record sceneInputs() hands out
    struct TexEntry { LLUUID id; std::string file; };
    std::vector<TexEntry> mTexFiles;
    std::vector<LLPointer<LLViewerFetchedTexture>> mTextures;   // held for the session
    SLBenchPath mCamera;
    LLSD mSkySettings, mWaterSettings;   // sky.xml / water.xml (undefined when absent)

    std::unordered_map<U32, size_t> mRowIndex;   // capture row -> index in mData.instances
    std::vector<U32> mTouched;                   // rows the tracks changed since the loop start
    S64 mMoverRec = -1;                          // last applied mover record
    U32 mTexLoaded = 0, mTexFailed = 0, mTexPreexisting = 0, mSlotsLoaded = 0, mSlotsFailed = 0, mRowsLoaded = 0;
};

#endif // SL_BENCHREPLAY_H
