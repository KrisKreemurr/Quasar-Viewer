/**
 * @file slbench.cpp
 * @brief Deterministic render benchmark (#1304): run controller, path recording, report.
 *
 * See slbench.h for the overview, doc/bench.md for use.
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

#include "slbench.h"

#include "slbenchcapture.h"
#include "slbenchpath.h"
#include "slbenchreplay.h"
#include "slbenchscene.h"

#include "llappviewer.h"
#include "llfocusmgr.h"
#include "llimagepng.h"
#include "llrand.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "lltexturedata.h"
#include "lltimer.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llworldclock.h"
#include "vkgfx/llvulkancontext.h"
#include "vkgfx/vk_pipeline.h"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace
{
    constexpr F64 kFixedDt = 1.0 / 60.0;     // replay world step (s)
    constexpr U32 kSeed = 1304u;             // replay RNG seed (main thread + std::rand)
    constexpr U32 kWarmupReplay = 300;       // frames at the first pose before timing (streaming settles)
    constexpr U32 kWarmupLive = 60;
    constexpr U32 kWarmupRestart = 60;
    constexpr F64 kPipelineWaitMax = 900.0;  // s: the replay's longest wait for its pipelines
    constexpr U32 kStillSettle = 4;          // frames at a still key before its noise samples
    constexpr size_t kMaxBatches = 1024;     // pipeline batch summaries kept (then counted only)
    constexpr size_t kMaxSamplesPerSeries = 1u << 20;   // per segment / per pass (a 4.6 h run at 60 fps)

    // ---- pipeline batches (any thread) ----
    struct Batch
    {
        std::string what;
        U64 total = 0, compiled = 0, cached = 0;
        double wall_ms = 0.0, sum_ms = 0.0;
        unsigned failed = 0, threads = 0;
        double at_s = 0.0;   // seconds since launch
    };
    std::mutex sBatchMutex;
    std::vector<Batch> sBatches;
    U64 sBatchesDropped = 0;

    // ---- run ----
    enum class RunState { Off, WaitReplay, Warmup, Running, Still, Done };

    struct Series
    {
        std::vector<float> v;
        void add(double x) { if (v.size() < kMaxSamplesPerSeries) v.push_back((float)x); }
    };

    struct SegStats
    {
        Series frame_ms, gpu_ms;
        std::map<std::string, Series> passes;
        U32 unfocused = 0;
    };

    struct StillAcc
    {
        size_t key = 0;
        F64 t = 0.0;
        std::string segment;
        U32 want = 0, settle = 0, got = 0;
        U32 w = 0, h = 0;
        std::vector<float> sum, sumsq;
        bool failed = false;
        std::string why;
    };

    struct Run
    {
        RunState state = RunState::Off;
        bool replay = false;
        SLBenchPath path;
        std::string path_file, report_file, label;
        U32 loops = 1, loop = 0;
        F64 t = 0.0;
        U32 warm_left = 0;
        size_t last_key = 0;
        std::map<std::string, SegStats> segs;
        std::vector<std::string> seg_order;
        SegStats overall;
        std::map<std::string, double> pending;   // this GPU frame's passes
        std::unique_ptr<StillAcc> still;
        boost::json::array stills;
        U32 restarts = 0;
        U64 vram_peak = 0;
        F64 launch_wait_s = 0.0, compile_wait_s = 0.0;
        std::string pipelines_unsettled;   // the pipelines still pending at the start (empty: all landed)
        F64 start_wall = 0.0;
        std::vector<std::pair<std::string, LLSD>> saved;   // settings to restore
        std::chrono::steady_clock::time_point last_end{};
        bool have_last = false;
        std::string error;
    };

    struct Recording
    {
        bool on = false;
        F64 start = 0.0, last = -1.0;
        F32 hz = 10.f;
        std::string segment = "default";
        U32 pending_still = 0;
        SLBenchPath path;
    };

    struct State
    {
        bool launched = false;
        Run run;
        Recording rec;
        SLBenchCapture capture;
        std::unique_ptr<SLBenchReplay> replay;
        F64 replay_clock = 0.0;
        std::string last_path_file, last_report;
    };
    State& st()
    {
        static State s;
        return s;
    }

    double pct(std::vector<float> v, double p)
    {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        const size_t i = (size_t)std::min<double>((double)v.size() - 1.0, std::ceil(p * (double)v.size()) - 1.0);
        return v[std::max<size_t>(0, i)];
    }

    boost::json::object stats(const Series& s)
    {
        boost::json::object o;
        o["n"] = s.v.size();
        if (s.v.empty()) return o;
        double sum = 0.0, mx = 0.0;
        for (float f : s.v) { sum += f; mx = std::max(mx, (double)f); }
        o["avg"] = sum / (double)s.v.size();
        o["p50"] = pct(s.v, 0.50);
        o["p95"] = pct(s.v, 0.95);
        o["p99"] = pct(s.v, 0.99);
        o["max"] = mx;
        return o;
    }

    LLSD err(const std::string& msg) { return llsd::map("ok", false, "error", msg); }

    void saveAndSet(Run& r, const char* name, const LLSD& value)
    {
        LLControlVariable* c = gSavedSettings.getControl(name);
        if (!c) return;
        r.saved.emplace_back(name, c->getValue());
        c->setValue(value, false);   // (not persisted: restored before the run ends)
    }

    void restoreSettings(Run& r)
    {
        for (auto it = r.saved.rbegin(); it != r.saved.rend(); ++it)
            if (LLControlVariable* c = gSavedSettings.getControl(it->first)) c->setValue(it->second, false);
        r.saved.clear();
    }

    void applyCamera(const SLBenchPath::Pose& p)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        cam->setOrigin(p.pos);
        cam->setAxes(p.rot);
        cam->setSpeedFovBoost(0.f);
        cam->setViewNoBroadcast(p.fov);
    }

    // frozen: a still hold with BenchFreezeStills -- the world clock (avatar poses, light / mover
    // tracks, animated textures) stands still, the frame counter and the renderer's own clocks
    // (history time, sample seeds) run on: the still's std image is then pure rendering noise.
    void advanceReplayClock(bool frozen)
    {
        State& s = st();
        if (!frozen) s.replay_clock += kFixedDt;
        gFrameTimeSeconds = (F32)s.replay_clock;
        gFrameIntervalSeconds = (F32)kFixedDt;   // (never 0: interval-divided rates stay finite)
        if (!frozen) LLWorldClock::tick();
        // The in-world frame clocks (displayVulkanUI advances them once logged in).
        gFrameCount++;
        LLTextureData::updateStats(gFrameTimeSeconds);
    }

    std::string defaultReportFile(const std::string& label)
    {
        return SLBenchScene::benchRoot() + "/reports/" + SLBenchScene::stamp() + "-" + label + ".json";
    }

    void seedDeterminism()
    {
        ll_seed_rand(kSeed);
        std::srand(kSeed);
        LLWorldClock::setFixedStep(kFixedDt);   // world time restarts at 0
    }

    void beginTiming(Run& r)
    {
        r.state = RunState::Running;
        r.t = 0.0;
        r.loop = 0;
        r.last_key = 0;
        r.pending.clear();
        r.start_wall = LLTimer::getTotalSeconds();
        if (!r.path.empty() && r.path.keys().front().still) r.state = RunState::Still;   // (set up in frameBegin)
    }

    void enterStill(Run& r, size_t key)
    {
        const SLBenchPath::Key& k = r.path.keys()[key];
        r.still = std::make_unique<StillAcc>();
        r.still->key = key;
        r.still->t = k.t;
        r.still->segment = k.segment;
        r.still->want = k.still;
        r.still->settle = kStillSettle;
        r.t = k.t;
        r.state = RunState::Still;
    }

    void writeStillImage(const StillAcc& a, const std::string& file)
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)a.w, (U16)a.h, 3);
        if (!raw->getData()) return;
        U8* px = raw->getData();
        const size_t n = (size_t)a.w * a.h * 3;
        for (size_t i = 0; i < n; ++i)
            px[i] = (U8)llclamp((S32)std::lround(a.sum[i] / (double)a.got * 255.0), 0, 255);
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (png->encode(raw, 0.f)) png->save(file);
    }

    void finishStill(Run& r)
    {
        StillAcc& a = *r.still;
        boost::json::object o;
        o["key"] = a.key;
        o["t"] = a.t;
        o["segment"] = a.segment;
        o["frames"] = a.got;
        if (a.failed || a.got < 2)
        {
            o["error"] = a.why.empty() ? "too few frames captured" : a.why;
        }
        else
        {
            // Temporal noise at a fixed camera: per-pixel variance across the held frames
            // (0..1 units), its mean and max, and the PSNR of a frame against the accumulated
            // mean (10 log10(1 / mean variance)).
            const size_t n = (size_t)a.w * a.h * 3;
            const double inv = 1.0 / (double)a.got;
            double mean_var = 0.0, max_var = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                const double m = a.sum[i] * inv;
                const double v = std::max(0.0, a.sumsq[i] * inv - m * m);
                mean_var += v;
                max_var = std::max(max_var, v);
            }
            mean_var /= (double)n;
            o["width"] = a.w;
            o["height"] = a.h;
            o["temporal_variance"] = mean_var;
            o["max_pixel_variance"] = max_var;
            o["psnr_db"] = mean_var > 0.0 ? 10.0 * std::log10(1.0 / mean_var) : 99.0;
            if (!r.report_file.empty())
            {
                const std::string img = r.report_file.substr(0, r.report_file.size() - 5) + ".still" + std::to_string(a.key) + ".png";
                writeStillImage(a, img);
                o["mean_image"] = img;
            }
        }
        r.stills.push_back(std::move(o));
        r.still.reset();
        r.state = RunState::Running;
    }

    void writeReport(Run& r)
    {
        State& s = st();
        boost::json::object rep;
        rep["format"] = "slipstream-bench-report";
        rep["version"] = 1;
        rep["ok"] = r.error.empty();
        if (!r.error.empty()) rep["error"] = r.error;
        rep["created"] = SLBenchScene::stamp();
        rep["viewer"] = LLVersionInfo::instance().getChannelAndVersion();
        rep["gpu"] = LLVulkanContext::driverIdentityLabel();
        rep["driver"] = LLVulkanContext::driverIdentityToken();
        rep["mode"] = r.replay ? "replay" : "live";
        if (r.replay && s.replay)
        {
            rep["capture"] = s.replay->dir();
            rep["capture_id"] = s.replay->captureId();
            boost::json::value info = LlsdToJson(s.replay->info());
            rep["capture_info"] = std::move(info);
            rep["fixed_dt"] = kFixedDt;
            rep["seed"] = kSeed;
        }
        rep["path"] = r.path_file.empty() ? std::string("(capture camera)") : r.path_file;
        rep["path_seconds"] = r.path.duration();
        rep["loops"] = r.loops;
        rep["restarts"] = r.restarts;
        if (gViewerWindow) rep["window"] = { gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw() };
        boost::json::object settings;
        for (const char* n : { "RenderFarClip", "RenderVKFramePacing", "RenderVolumeLODFactor", "RenderShadowDetail",
                               "RenderReflectionProbeDetail", "RenderVKRayTracing", "BenchFreezeStills" })
            if (LLControlVariable* c = gSavedSettings.getControl(n)) settings[n] = LlsdToJson(c->getValue());
        settings["vsync"] = false;
        settings["gpu_timers"] = true;
        rep["settings"] = std::move(settings);

        boost::json::array segs;
        for (const std::string& name : r.seg_order)
        {
            const SegStats& g = r.segs[name];
            boost::json::object o;
            o["name"] = name;
            o["frame_ms"] = stats(g.frame_ms);
            o["gpu_ms"] = stats(g.gpu_ms);
            boost::json::object passes;
            for (const auto& kv : g.passes) passes[kv.first] = stats(kv.second);
            o["passes"] = std::move(passes);
            o["unfocused_frames"] = g.unfocused;
            segs.push_back(std::move(o));
        }
        rep["segments"] = std::move(segs);
        {
            boost::json::object o;
            o["frame_ms"] = stats(r.overall.frame_ms);
            o["gpu_ms"] = stats(r.overall.gpu_ms);
            boost::json::object passes;
            for (const auto& kv : r.overall.passes) passes[kv.first] = stats(kv.second);
            o["passes"] = std::move(passes);
            o["unfocused_frames"] = r.overall.unfocused;
            rep["overall"] = std::move(o);
        }
        {
            const LLVKVramSummary v = LLVulkanContext::instance().vramSummary();
            boost::json::object o;
            o["valid"] = v.valid;
            o["usage_mb_end"] = v.usage >> 20;
            o["usage_mb_peak"] = r.vram_peak >> 20;
            o["budget_mb"] = v.budget >> 20;
            o["capacity_mb"] = v.capacity >> 20;
            o["ladder_rung"] = v.rung;
            rep["vram"] = std::move(o);
        }
        {
            boost::json::object o;
            o["cold"] = SLBench::coldPipelines();
            o["wait_s"] = r.compile_wait_s;
            o["settled"] = r.pipelines_unsettled.empty();
            if (!r.pipelines_unsettled.empty()) o["unsettled"] = r.pipelines_unsettled;
            boost::json::array arr;
            U64 total = 0, compiled = 0, cached = 0;
            double wall = 0.0, sum = 0.0;
            {
                std::lock_guard<std::mutex> lk(sBatchMutex);
                for (const Batch& b : sBatches)
                {
                    boost::json::object bo;
                    bo["what"] = b.what;
                    bo["total"] = b.total;
                    bo["compiled"] = b.compiled;
                    bo["dedup"] = b.cached;
                    bo["wall_ms"] = b.wall_ms;
                    bo["sum_ms"] = b.sum_ms;
                    bo["failed"] = b.failed;
                    bo["threads"] = b.threads;
                    bo["at_s"] = b.at_s;
                    arr.push_back(std::move(bo));
                    total += b.total; compiled += b.compiled; cached += b.cached;
                    wall += b.wall_ms; sum += b.sum_ms;
                }
                o["batches_dropped"] = sBatchesDropped;
            }
            o["batches"] = std::move(arr);
            o["total_pipelines"] = total;
            o["compiled"] = compiled;
            o["dedup"] = cached;
            o["wall_ms"] = wall;
            o["sum_ms"] = sum;
            rep["pipelines"] = std::move(o);
        }
        rep["noise"] = std::move(r.stills);
        r.stills = boost::json::array();

        std::string file = r.report_file;
        std::string e;
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos && SLBenchScene::makeOutputDir(file.substr(0, slash), e))
        {
            const std::string tmp = file + ".tmp";
            FILE* f = std::fopen(tmp.c_str(), "wb");
            bool ok = f != nullptr;
            if (f)
            {
                const std::string text = boost::json::serialize(rep);
                ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
                ok = (std::fclose(f) == 0) && ok;
            }
            if (ok && std::rename(tmp.c_str(), file.c_str()) == 0)
            {
                s.last_report = file;
                LL_INFOS("Bench") << "report: " << file << LL_ENDL;
                return;
            }
            e = "cannot write " + file;
        }
        LL_WARNS("Bench") << "report not written: " << (e.empty() ? "no report path" : e) << LL_ENDL;
    }

    // End of a run (finished, failed or aborted): report, settings back, replay quits.
    void endRun(Run& r, const std::string& error = std::string())
    {
        if (r.state == RunState::Off) return;
        if (!error.empty()) r.error = error;
        if (!r.error.empty()) LL_WARNS("Bench") << "run: " << r.error << LL_ENDL;
        if (r.still) finishStill(r);
        writeReport(r);
        restoreSettings(r);
        const bool replay = r.replay;
        r = Run();
        r.state = RunState::Done;
        if (replay)
        {
            LL_INFOS("Bench") << "replay finished; quitting" << LL_ENDL;
            LLAppViewer::instance()->forceQuit();
        }
    }

    bool startRun(Run& r, bool replay, const SLBenchPath& path, const std::string& path_file, U32 loops,
                  const std::string& report_file, const std::string& label, std::string& e)
    {
        if (path.empty())
        {
            e = "no camera path";
            return false;
        }
        r = Run();
        r.replay = replay;
        r.path = path;
        r.path_file = path_file;
        r.loops = llclamp(loops, 1u, 1000u);
        r.label = label;
        r.report_file = report_file.empty() ? defaultReportFile(label) : report_file;
        // The report's directory now: still-key images are written next to it during the run.
        const size_t slash = r.report_file.find_last_of("/\\");
        if (slash == std::string::npos || !SLBenchScene::makeOutputDir(r.report_file.substr(0, slash), e))
        {
            if (e.empty()) e = "the report path needs a directory: " + r.report_file;
            return false;
        }
        saveAndSet(r, "RenderVKGpuTimers", LLSD(true));
        saveAndSet(r, "RenderVSyncEnable", LLSD(false));
        saveAndSet(r, "RenderFPSLimit", LLSD(0));
        r.warm_left = replay ? kWarmupReplay : kWarmupLive;
        r.state = RunState::Warmup;
        if (replay) seedDeterminism();
        LL_INFOS("Bench") << "run: " << (replay ? "replay" : "live") << ", path " << (path_file.empty() ? "(capture camera)" : path_file)
                          << " (" << path.duration() << " s, " << path.size() << " keys), " << r.loops << " loop(s), report "
                          << r.report_file << LL_ENDL;
        return true;
    }

    void launch()
    {
        State& s = st();
        s.launched = true;
        const std::string dir = gSavedSettings.getString("BenchCaptureDir");
        if (dir.empty()) return;
        s.run = Run();
        s.run.replay = true;
        s.run.state = RunState::WaitReplay;
        s.run.launch_wait_s = LLTimer::getTotalSeconds();
        s.replay = std::make_unique<SLBenchReplay>();
        std::string e;
        if (!s.replay->load(dir, e))
        {
            s.run.report_file = defaultReportFile("replay-error");
            endRun(s.run, "replay: " + e);
        }
    }

    // Replay: inject once Vulkan is up, then start the run when the world pipelines are ready.
    void replayPrepare()
    {
        State& s = st();
        Run& r = s.run;
        LLVulkanContext& vk = LLVulkanContext::instance();
        if (!vk.isInitialized() || !gViewerWindow) return;
        if (!s.replay->injected())
        {
            // Sliced over frames (a time budget per frame): the main loop keeps running and
            // pinging its watchdog while a large capture goes into the stores.
            std::string e;
            if (!s.replay->injectStep(e))
            {
                r.report_file = defaultReportFile("replay-error");
                endRun(r, "replay: " + e);
                return;
            }
            if (!s.replay->injected()) return;
        }
        std::string unsettled;
        {
            // Wait for the lit variant the run will actually draw: the world pipelines, the
            // deferred ray-query group (until it lands every pass draws the plain, shadow-mapped
            // variant) and the settings' specialized set with its ray-query phase. A run that
            // started early timed the wrong variant (a 135 s ray-query compile on a read-only
            // pipeline cache). Bounded: past kPipelineWaitMax the run starts with a warning.
            const char* what = nullptr;
            if (vk.benchPipelinesPending(&what))
            {
                const F64 waited = LLTimer::getTotalSeconds() - r.launch_wait_s;
                if (waited < kPipelineWaitMax) return;
                LL_WARNS("Bench") << "replay: " << (what ? what : "?") << " pipelines still pending after " << waited
                                  << " s; starting anyway (this run may time a fallback variant)" << LL_ENDL;
                unsettled = what ? what : "?";
            }
        }
        const F64 wait = LLTimer::getTotalSeconds() - r.launch_wait_s;
        SLBenchPath path;
        std::string path_file = gSavedSettings.getString("BenchPathFile");
        std::string e;
        if (!path_file.empty())
        {
            if (!path.load(path_file, e))
            {
                r.report_file = defaultReportFile("replay-error");
                endRun(r, "replay: " + e);
                return;
            }
        }
        else
        {
            path = s.replay->camera();
        }
        const std::string capture_dir = s.replay->dir();
        const size_t slash = capture_dir.find_last_of("/\\", capture_dir.size() > 1 ? capture_dir.size() - 2 : 0);
        std::string label = slash == std::string::npos ? capture_dir : capture_dir.substr(slash + 1);
        while (!label.empty() && (label.back() == '/' || label.back() == '\\')) label.pop_back();
        if (!startRun(r, true, path, path_file, gSavedSettings.getU32("BenchLoops"),
                      gSavedSettings.getString("BenchReportFile"), label.empty() ? "replay" : label, e))
        {
            r.report_file = defaultReportFile("replay-error");
            endRun(r, "replay: " + e);
            return;
        }
        r.compile_wait_s = wait;
        r.pipelines_unsettled = unsettled;
        LL_INFOS("Bench") << "replay: pipelines ready (world, ray-query, spec) after " << wait << " s" << (SLBench::coldPipelines() ? " (cold)" : "")
                          << LL_ENDL;
    }
}

namespace SLBench
{

bool replayRequested()
{
    static const bool s_req = !gSavedSettings.getString("BenchCaptureDir").empty();
    return s_req;
}

bool coldPipelines()
{
    return gSavedSettings.getBOOL("BenchColdPipelines");
}

bool replayDrivesWorld()
{
    const State& s = st();
    return s.replay && s.replay->injected() && LLStartUp::getStartupState() < STATE_STARTED;
}

bool replayHasEnvironment()
{
    const State& s = st();
    return replayDrivesWorld() && s.replay->hasEnvironment();
}

void frameBegin()
{
    State& s = st();
    if (!s.launched) launch();
    s.capture.tick();
    Run& r = s.run;
    if (r.state == RunState::WaitReplay) replayPrepare();
    if (replayDrivesWorld())
    {
        static LLCachedControl<bool> s_freeze(gSavedSettings, "BenchFreezeStills", false);
        advanceReplayClock(s_freeze && r.state == RunState::Still);
    }
    if (r.state == RunState::Warmup || r.state == RunState::Running || r.state == RunState::Still)
    {
        applyCamera(r.path.sample(r.t));
        if (r.state == RunState::Still && r.still && r.still->settle == 0)
            LLVulkanContext::instance().requestCapture(1);
    }
    if (s.replay && s.replay->injected() && replayDrivesWorld())
        s.replay->applyTracks(LLWorldClock::getTime());
}

void sceneInputs(LLVKWorldFrame& wf)
{
    State& s = st();
    if (s.replay && s.replay->injected()) s.replay->sceneInputs(wf);
}

void worldFrameBuilt(LLVKWorldFrame& wf)
{
    State& s = st();
    s.capture.worldFrame(wf);
    Run& r = s.run;
    // The first timed frame and every path restart is a camera cut (no temporal history).
    if (r.state == RunState::Running && r.t == 0.0 && r.loop == 0) wf.temporal_reset = true;
    // Path recording samples the final camera of the frame (flycam included).
    Recording& rec = s.rec;
    if (rec.on && rec.hz > 0.f)
    {
        const F64 now = LLTimer::getTotalSeconds() - rec.start;
        if (rec.last < 0.0 || now - rec.last >= 1.0 / rec.hz)
        {
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            SLBenchPath::Key k;
            k.t = now;
            k.pos = cam->getOrigin();
            k.rot = cam->getQuaternion();
            k.fov = cam->getView();
            k.segment = rec.segment;
            rec.path.append(k);
            rec.last = now;
        }
    }
}

void frameEnd()
{
    State& s = st();
    Run& r = s.run;
    const auto now = std::chrono::steady_clock::now();
    const double frame_ms = r.have_last ? std::chrono::duration<double, std::milli>(now - r.last_end).count() : 0.0;
    r.last_end = now;
    r.have_last = true;

    switch (r.state)
    {
        case RunState::Warmup:
            if (r.warm_left > 0) --r.warm_left;
            if (r.warm_left == 0)
            {
                beginTiming(r);
                if (r.state == RunState::Still) enterStill(r, 0);
            }
            return;

        case RunState::Still:
        {
            StillAcc& a = *r.still;
            if (a.settle > 0)
            {
                --a.settle;
                return;
            }
            std::vector<U8> rgb;
            U32 w = 0, h = 0;
            if (!LLVulkanContext::instance().readCapture(rgb, w, h) || rgb.size() < (size_t)w * h * 3 || !w || !h)
            {
                a.failed = true;
                a.why = "frame capture unavailable";
            }
            else if (!a.failed)
            {
                try
                {
                    if (a.sum.empty())
                    {
                        a.w = w;
                        a.h = h;
                        a.sum.assign((size_t)w * h * 3, 0.f);
                        a.sumsq.assign((size_t)w * h * 3, 0.f);
                    }
                    if (w != a.w || h != a.h)
                    {
                        a.failed = true;
                        a.why = "the frame size changed during the hold";
                    }
                    else
                    {
                        const size_t n = (size_t)w * h * 3;
                        for (size_t i = 0; i < n; ++i)
                        {
                            const float x = rgb[i] * (1.f / 255.f);
                            a.sum[i] += x;
                            a.sumsq[i] += x * x;
                        }
                        ++a.got;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    a.failed = true;   // allocation failure: this still is dropped, the run goes on
                    a.why = "out of memory for the accumulation";
                    a.sum.clear(); a.sum.shrink_to_fit();
                    a.sumsq.clear(); a.sumsq.shrink_to_fit();
                }
            }
            if (a.failed || a.got >= a.want) finishStill(r);
            return;
        }

        case RunState::Running:
        {
            const std::string& seg = r.path.segmentAt(r.t);
            if (r.segs.find(seg) == r.segs.end()) r.seg_order.push_back(seg);
            SegStats& g = r.segs[seg];
            g.frame_ms.add(frame_ms);
            r.overall.frame_ms.add(frame_ms);
            if (!gFocusMgr.getAppHasFocus()) { ++g.unfocused; ++r.overall.unfocused; }
            if ((gFrameCount % 30) == 0)
                r.vram_peak = std::max<U64>(r.vram_peak, LLVulkanContext::instance().vramSummary().usage);

            // Advance the path: fixed step in a replay, the frame's wall time live.
            const F64 step = r.replay ? kFixedDt : frame_ms * 0.001;
            const F64 next_t = r.t + step;
            const size_t k_next = r.path.keyAt(next_t);
            for (size_t k = r.last_key + 1; k <= k_next && k < r.path.size(); ++k)
            {
                if (r.path.keys()[k].still)
                {
                    r.last_key = k;
                    enterStill(r, k);
                    return;
                }
            }
            r.last_key = std::max(r.last_key, k_next);
            r.t = next_t;
            if (r.t >= r.path.duration())
            {
                if (++r.loop >= r.loops)
                {
                    endRun(r);
                    return;
                }
                r.t = 0.0;
                r.last_key = 0;
                if (r.path.keys().front().still) enterStill(r, 0);   // every loop holds at a still first key
            }
            return;
        }

        default:
            return;
    }
}

void restartPath()
{
    Run& r = st().run;
    if (r.state != RunState::Warmup && r.state != RunState::Running && r.state != RunState::Still) return;
    r.segs.clear();
    r.seg_order.clear();
    r.overall = SegStats();
    r.pending.clear();
    r.still.reset();
    r.stills = boost::json::array();
    r.t = 0.0;
    r.loop = 0;
    r.last_key = 0;
    r.vram_peak = 0;
    ++r.restarts;
    r.warm_left = kWarmupRestart;
    r.state = RunState::Warmup;
    if (r.replay) seedDeterminism();
    LL_INFOS("Bench") << "run: path restarted (" << r.restarts << ")" << LL_ENDL;
}

void gpuSubSample(const char* name, double ms, bool async)
{
    Run& r = st().run;
    if (r.state != RunState::Running || !name) return;
    std::string key(name);
    if (async) key += "@async";
    r.pending[key] += ms;
}

void gpuFrameDone(double total_ms)
{
    Run& r = st().run;
    if (r.state != RunState::Running)
    {
        r.pending.clear();
        return;
    }
    const std::string& seg = r.path.segmentAt(r.t);
    if (r.segs.find(seg) == r.segs.end()) r.seg_order.push_back(seg);
    SegStats& g = r.segs[seg];
    g.gpu_ms.add(total_ms);
    r.overall.gpu_ms.add(total_ms);
    for (const auto& kv : r.pending)
    {
        g.passes[kv.first].add(kv.second);
        r.overall.passes[kv.first].add(kv.second);
    }
    r.pending.clear();
}

void notePipelineBatch(const VkPipelineBatchReport& rep)
{
    Batch b;
    b.what = rep.what ? rep.what : "";
    b.total = rep.total;
    b.compiled = rep.compiled;
    b.cached = rep.fromCache;
    b.wall_ms = rep.wallMs;
    b.sum_ms = rep.sumMs;
    b.failed = rep.failed;
    b.threads = rep.threads;
    b.at_s = LLTimer::getTotalSeconds();
    std::lock_guard<std::mutex> lk(sBatchMutex);
    if (sBatches.size() < kMaxBatches) sBatches.push_back(std::move(b));
    else ++sBatchesDropped;
}

LLSD control(const LLSD& req)
{
    State& s = st();
    const std::string op = req["op"].asString();
    const bool in_world = LLStartUp::getStartupState() >= STATE_STARTED;

    if (op == "status")
    {
        LLSD r = llsd::map("ok", true);
        r["capture"] = s.capture.status();
        static const char* names[] = { "off", "wait_replay", "warmup", "running", "still", "done" };
        r["run"] = llsd::map("state", names[(int)s.run.state], "t", s.run.t, "loop", (S32)s.run.loop,
                             "segment", s.run.path.segmentAt(s.run.t));
        r["recording"] = llsd::map("on", s.rec.on, "keys", (S32)s.rec.path.size(), "segment", s.rec.segment);
        if (!s.last_report.empty()) r["last_report"] = s.last_report;
        if (!s.last_path_file.empty()) r["last_path"] = s.last_path_file;
        return r;
    }
    if (op == "capture")
    {
        if (!in_world) return err("capture needs a live scene (log in first)");
        std::string dir = req["dir"].asString();
        if (dir.empty())
        {
            const std::string name = req.has("name") ? req["name"].asString() : SLBenchScene::stamp();
            if (name.empty() || name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos)
                return err("bad capture name");
            dir = SLBenchScene::benchRoot() + "/captures/" + name;
        }
        const F32 secs = req.has("seconds") ? (F32)req["seconds"].asReal() : gSavedSettings.getF32("BenchCaptureSeconds");
        const U64 budget = (U64)llmax(64u, gSavedSettings.getU32("BenchCaptureBudgetMB")) << 20;
        std::string e;
        if (!s.capture.start(dir, secs, budget, e)) return err(e);
        return llsd::map("ok", true, "dir", dir);
    }
    if (op == "path_start")
    {
        s.rec = Recording();
        s.rec.on = true;
        s.rec.start = LLTimer::getTotalSeconds();
        s.rec.hz = req.has("hz") ? llclamp((F32)req["hz"].asReal(), 0.f, 120.f) : 10.f;
        if (req.has("segment") && !req["segment"].asString().empty()) s.rec.segment = req["segment"].asString();
        LL_INFOS("Bench") << "path: recording at " << s.rec.hz << " Hz" << LL_ENDL;
        return llsd::map("ok", true);
    }
    if (op == "path_segment")
    {
        if (!s.rec.on) return err("not recording");
        const std::string name = req["name"].asString();
        if (name.empty()) return err("need name");
        s.rec.segment = name;
        return llsd::map("ok", true);
    }
    if (op == "path_key")
    {
        if (!s.rec.on) return err("not recording");
        if (req.has("segment") && !req["segment"].asString().empty()) s.rec.segment = req["segment"].asString();
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        SLBenchPath::Key k;
        k.t = LLTimer::getTotalSeconds() - s.rec.start;
        k.pos = cam->getOrigin();
        k.rot = cam->getQuaternion();
        k.fov = cam->getView();
        k.segment = s.rec.segment;
        k.still = req.has("still") ? (U32)llclamp(req["still"].asInteger(), 0, 4096) : 0u;
        if (!s.rec.path.append(k)) return err("key out of time order");
        s.rec.last = k.t;
        return llsd::map("ok", true, "keys", (S32)s.rec.path.size());
    }
    if (op == "path_stop")
    {
        if (!s.rec.on) return err("not recording");
        s.rec.on = false;
        if (s.rec.path.empty()) return err("no keys recorded");
        std::string file = req["file"].asString();
        if (file.empty()) file = SLBenchScene::benchRoot() + "/paths/" + SLBenchScene::stamp() + ".json";
        const size_t slash = file.find_last_of("/\\");
        std::string e;
        if (slash == std::string::npos || !SLBenchScene::makeOutputDir(file.substr(0, slash), e)) return err(e.empty() ? "bad file" : e);
        if (!s.rec.path.save(file, e)) return err(e);
        s.last_path_file = file;
        LL_INFOS("Bench") << "path: " << s.rec.path.size() << " keys, " << s.rec.path.duration() << " s -> " << file << LL_ENDL;
        return llsd::map("ok", true, "file", file, "keys", (S32)s.rec.path.size(), "seconds", s.rec.path.duration());
    }
    if (op == "run")
    {
        if (s.run.state != RunState::Off && s.run.state != RunState::Done) return err("a run is active");
        if (!in_world) return err("a live run needs a live scene (use --bench <capture> for a replay)");
        std::string file = req["path"].asString();
        if (file.empty()) file = s.last_path_file;
        if (file.empty()) return err("need path (no path recorded this session)");
        SLBenchPath path;
        std::string e;
        if (!path.load(file, e)) return err(e);
        const U32 loops = req.has("loops") ? (U32)llmax(1, req["loops"].asInteger()) : gSavedSettings.getU32("BenchLoops");
        if (!startRun(s.run, false, path, file, loops, req["report"].asString(), "live", e)) return err(e);
        return llsd::map("ok", true, "report", s.run.report_file);
    }
    if (op == "restart")
    {
        restartPath();
        return llsd::map("ok", true);
    }
    if (op == "abort")
    {
        s.capture.abort("requested");
        if (s.run.state != RunState::Off && s.run.state != RunState::Done) endRun(s.run, "aborted");
        s.rec.on = false;
        return llsd::map("ok", true);
    }
    return err("unknown bench op '" + op + "'");
}

void shutdown()
{
    State& s = st();
    restoreSettings(s.run);
    s.capture.shutdown();
    s.replay.reset();   // its textures go while the texture list still exists
}

} // namespace SLBench
