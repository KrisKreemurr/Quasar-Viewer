/**
 * @file slbench.h
 * @brief Deterministic render benchmark (#1304): scene capture, replay, camera paths, reports.
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

#ifndef SL_BENCH_H
#define SL_BENCH_H

#include "stdtypes.h"

class LLSD;
struct LLVKWorldFrame;
struct VkPipelineBatchReport;

// RENDER BENCHMARK (doc/bench.md).
//
//  CAPTURE  a live scene -> a capture directory (slbenchcapture.h; format: slbenchscene.h)
//  REPLAY   --bench <capture> [--bench-path <path.json>]: no login, no network; the stores are
//           fed from the capture (slbenchreplay.h), world time steps by a fixed 1/60 s, the
//           main-thread RNG is seeded, lights and movers follow their recorded tracks (looped)
//  PATH     camera keyframes recorded from the live camera / flycam, played back smoothly with
//           named segments and "still" holds (slbenchpath.h); also runs in a live session
//  REPORT   per segment frame-time p50 / p95 / p99, every named GPU sub-timer, VRAM, every
//           pipeline batch, noise (variance / PSNR at the still keys) -> JSON;
//           scripts/bench/compare.py diffs two reports
//
// Output never lands inside a source checkout (slbenchscene.h insideSourceTree); the default
// root is ~/.slipstreamviewer/bench. Settings: Bench* (Persist 0).
namespace SLBench
{
    // ---- launch (llappviewer.cpp, after the settings and the command line) ----
    void initFromLaunch();
    bool replayRequested();     // --bench given: startup parks before login (llstartup.cpp)
    bool coldPipelines();       // --bench-cold: this session keeps no disk pipeline cache

    // ---- per frame (displayVulkanUI, llappviewer.cpp) ----
    void frameBegin();                         // before the world frame is built
    bool replayDrivesWorld();                  // the world frame's scene comes from a capture
    bool replayHasEnvironment();               // ... and so does its environment (SLBenchReplay::hasEnvironment)
    void sceneInputs(LLVKWorldFrame& wf);      // replay: environment, lights, probes (buildVulkanWorldFrame)
    void worldFrameBuilt(LLVKWorldFrame& wf);  // after buildVulkanWorldFrame: capture sampling, path cuts
    void frameEnd();                           // after renderFrame: timing, noise readback, report

    // ---- control: the test-control "bench" command and Develop > Render Benchmark ----
    // {"op": "capture" | "status" | "path_start" | "path_key" | "path_segment" | "path_stop" |
    //  "run" | "restart" | "abort", ...}; see doc/bench.md.
    LLSD control(const LLSD& req);

    // HOOK (hot shader reload, #1304 part 5): restart the camera path from t = 0 and drop every
    // sample taken so far, so a report never mixes two shader versions. Warm-up runs again.
    // No-op when no run is active. Main thread.
    void restartPath();

    // ---- measurement sinks ----
    void gpuSubSample(const char* name, double ms, bool async);   // llvkbench.h harvest fan-out
    void gpuFrameDone(double total_ms);
    void notePipelineBatch(const VkPipelineBatchReport& r);       // any thread

    void shutdown();   // LLAppViewer::cleanup: joins the capture writer
}

#endif // SL_BENCH_H
