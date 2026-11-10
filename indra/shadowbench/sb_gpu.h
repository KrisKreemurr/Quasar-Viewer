// shadowbench -- GPU side: the synthetic scene, the shadow passes (a replica of
// LLCleanRendererVK::recordShadows' structure running the viewer's caster shaders), the
// receiver G-buffer and the viewer's receiver-side shadow evaluation.
#pragma once

#include "llvkshadowcascades.h"
#include "sb_scenario.h"
#include "vk_device.h"

#include <string>
#include <vector>

// A policy = one way of running the sun cascades (the A or the B of an experiment).
struct SbPolicy
{
    std::string name;
    uint32_t mode = 0;           // 0 realtime (every cascade every frame), 1 cached (event cache + overlay)
    float    scale0 = 1.f;       // fit scale, cascade 0 (cached only)
    float    scale_n = 1.15f;    // fit scale, cascades 1-3 (cached only)
    bool     roll = false;       // drift test counts the basis roll (the old rule)
    int      lever = 0;          // 0 the map's extent, 1 2R, 2 2R + 64 m, 3 measured (GPU reduction)
    float    light_tx = 0.5f;
    uint32_t far_n = 2;          // staggered far overlay period
    uint32_t defer = 2;          // far deferral frames
    bool     trans_ovl = false;  // translucency layer overlay (dynamic translucent casters per frame)
    bool     trans_skip = false; // skip the translucency pass when no translucent caster exists
    bool     swept = false;      // swept-volume caster planes (F3)
    bool     force_all = false;  // cached structure, but every cascade re-renders every frame
    float    border_frac = 0.f;  // cached: map border per side, as a fraction of the half map (texels)
    bool     static_slice_planes = false;   // cached static culls use the slice's caster planes (the old cull)
};

struct SbPassTimes
{
    // GPU ms of this frame, by pass
    double ref = 0, cull = 0, draw = 0, trans = 0, copy = 0, dyncull = 0, ovl = 0, shadow_total = 0, receiver = 0;
};

struct SbRunResult
{
    std::vector<SbPassTimes> frames;
    uint32_t renders[VKSC_CASCADES] = {};    // cascade re-renders over the run
    uint32_t overlays[VKSC_CASCADES] = {};
    uint64_t draws[VKSC_CASCADES] = {};      // caster draws (GPU counts, sampled frames)
    uint32_t draw_samples[VKSC_CASCADES] = {};
    // receiver images at the sample frames (vis in .a, tint x vis in .rgb), unorm16
    std::vector<std::vector<uint16_t>> samples;
};

struct SbOptions
{
    uint32_t res = 2048;         // shadow map resolution
    uint32_t recv_w = 960, recv_h = 540;
    int      filter = 1;         // SlipstreamRenderShadowFilterQuality
    float    fps = 60.f;
    int      sample_every = 30;  // receiver image every N frames
    float    seconds = 0.f;      // 0 = the scenario's
    bool     validate = false;
    bool     keep_samples = true;
};

class SbBench
{
public:
    bool init(VkDeviceContext& ctx, const std::string& shader_dir, const SbOptions& opt);
    SbRunResult run(const SbScenario& sc, const SbPolicy& pol);
    void shutdown();
    const char* deviceName() const { return mDeviceName.c_str(); }

private:
    struct Impl;
    Impl* m = nullptr;
    std::string mDeviceName;
};
