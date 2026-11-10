/**
 * @file slbenchcapture.h
 * @brief Render benchmark: record the live scene to a capture directory (#1304).
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

#ifndef SL_BENCHCAPTURE_H
#define SL_BENCHCAPTURE_H

#include "slbenchpath.h"
#include "slbenchscene.h"

#include "llsd.h"
#include "llviewertexturelist.h"   // LLLoadedCallbackEntry::source_callback_list_t (needs LLTextureKey)

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

class LLViewerFetchedTexture;
class LLVKCpuLease;

// SCENE CAPTURE. Reads the renderer's own inputs -- the clean stores (instance rows, LOD
// groups, the packed geometry mirror, materials, terrain records) and the built world frame
// (environment, lights, probes, camera) -- never LL's object layer, so a replay feeds the
// renderer exactly what it drew. Phases, all on the main thread except the last:
//  SNAPSHOT  the first frame after start(): rows, groups, geometry, materials, environment
//  RECORD    `seconds` of tracks at kTrackHz: lights, mover poses, the camera
//  TEXTURES  every referenced texture's decoded pixels (CPU copies re-sourced under a lease)
//  WRITING   a worker thread writes the files, checksums them, then the manifest
// Memory is bounded by the budget: geometry over it fails the capture, tracks over it are
// truncated (manifest "truncated"), textures over it are left out (listed as missing).
class SLBenchCapture
{
public:
    static constexpr F32 kTrackHz = 30.f;
    static constexpr F64 kTextureWaitSec = 30.0;

    SLBenchCapture();
    ~SLBenchCapture();

    bool start(const std::string& dir, F32 seconds, U64 budget_bytes, std::string& err);
    // Main thread, every frame, after the world frame is built (wf.enabled: in world).
    void worldFrame(const LLVKWorldFrame& wf);
    // Main thread, every frame: texture gathering and the writer's completion.
    void tick();
    bool busy() const { return mPhase != IDLE && mPhase != DONE && mPhase != FAILED; }
    void abort(const std::string& why);
    // Viewer exit (while the texture list still exists): an unfinished capture is dropped, a
    // capture being written is waited for.
    void shutdown();
    LLSD status() const;

private:
    enum Phase { IDLE, SNAPSHOT, RECORD, TEXTURES, WRITING, DONE, FAILED };
    static const char* phaseName(Phase p);

    bool snapshot(const LLVKWorldFrame& wf);
    void record(const LLVKWorldFrame& wf);
    void beginTextures();
    void finishTextures();
    void fail(const std::string& why);
    U64  heldBytes() const;
    bool captureSlot(U32 slot);            // false = mirror not resident (requested)
    void noteTexture(const LLUUID& id);
    void noteTexture(const U8 bytes[16]);
    static void writeAll(SLBenchScene::Data* data, std::string dir, std::string manifest_head,
                         std::atomic<int>* result, std::string* message);
    static void onTexLoaded(bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw* aux,
                            S32 discard, bool final, void* userdata);
    void dropTextureWaits();   // callbacks cancelled, then the waits (and their leases) released

    Phase mPhase = IDLE;
    std::string mDir, mError, mMessage;
    F32  mSeconds = 10.f;
    U64  mBudget = 0;
    F64  mStartTime = 0.0;   // world time at the snapshot
    F64  mTexStart = 0.0;    // wall time the texture phase began
    S64  mLastSample = -1;
    bool mTruncated = false;

    std::unique_ptr<SLBenchScene::Data> mData;
    SLBenchPath mCamera;
    std::map<U32, U32> mRowGen;                         // captured row -> its generation at snapshot
    std::unordered_map<U32, SLBenchScene::MoverRec> mLast;   // last recorded pose per row
    std::set<U32> mGone;                                // rows released during the capture
    std::set<U32> mMoved;                               // rows whose pose changed during the capture
    std::set<U32> mPendingSlots;                        // mirrors asked back (cluster-released)
    std::set<U32> mSlotsDone;
    std::set<LLUUID> mTexIds;
    struct TexWait;
    std::vector<std::unique_ptr<TexWait>> mTexWaits;
    LLLoadedCallbackEntry::source_callback_list_t mCallbackList;   // our loaded callbacks (cleanup)
    // Counts for the manifest.
    U32 mSkipRigged = 0, mSkipHud = 0, mSkipWorn = 0, mSlotsMissing = 0, mTexMissing = 0, mLightsMax = 0;

    std::thread mWriter;
    std::atomic<int> mWriteResult{ 0 };   // 0 running, 1 ok, -1 failed
    std::string mWriteMessage;
};

#endif // SL_BENCHCAPTURE_H
