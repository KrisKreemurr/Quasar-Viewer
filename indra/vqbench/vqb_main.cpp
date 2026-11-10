// vqbench (#1039) -- THE VISIBILITY-QUEUE BENCH: the viewer's lighting kernels (the GI split's TRACE .. REUSE through
// the visibility queue and pass 15's split shadow core), compiled from the viewer's sources with the viewer's defines
// (vqb_spv.py), run on a reproducible synthetic scene (vqb_scene.cpp) in the viewer's default chain and its A/B
// configurations (RenderVKVisQueue 64 and the GI queue policies), timed with GPU timestamps after every dispatch (the viewer's gpuSubMark: each mark = the time since the one
// before it) over N frames after a warm-up, medians reported per kernel and per consumer. See README.md.
#include "vqb_reflect.h"
#include "vqb_scene.h"
#include "vqb_vk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef VQB_SPV_DIR
#define VQB_SPV_DIR "shaders"
#endif

namespace
{
// ---- the push (llcleanrenderervk.h VKDeferPush + VKRtReflPush; 128 B) ----
struct DeferPush { uint32_t view, flags, width, height; float inv_vp_rel[16]; };
struct RtReflPush { DeferPush d; float rtr[4]; float rtr2[4]; uint32_t rtr3[4]; };
static_assert(sizeof(RtReflPush) == 128, "VKRtReflPush");

constexpr uint32_t DEFER_FLAG_PSTOCH = 1u << 18, DEFER_FLAG_PSTOCH_HALF = 1u << 19;
// GiHeader cfg.x (clean_gi_common.glsl GI_F_*; llcleangivk.cpp giWriteHeader's defaults)
constexpr uint32_t GI_F_CACHE = 16u, GI_F_RES_PREV = 64u, GI_F_CACHE_FILL = 128u, GI_F_STATS = 2097152u,
                   GI_F_VIS_TSKIP = 268435456u;   // (#985, RenderVKGIReuseVisSkip: bench mask bit 1024)
constexpr uint32_t kGiFlagsDefault = 0x085FF1FFu;   // the steady state without motion vectors
// clean_lights_common.glsl
constexpr uint32_t GX = 16, GY = 9, GZ = 24, CLUSTER_COUNT = GX * GY * GZ, CLUSTER_CAP = 64;
// llcleanrenderervk.h (the queue)
constexpr uint32_t kVqHdrBytes = 256, kVqReqBytes = 48, kVqSlotBytes = 16;

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::vector<uint32_t> readWords(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "vqbench: cannot read %s (build the bench: its shaders are generated)\n", path.c_str()); std::exit(1); }
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> w(b.size() / 4);
    std::memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}

struct Kernel
{
    std::string sym;
    std::vector<uint32_t> code;
    VqbModule refl;   // the names twin
    VkPipeline pipe = VK_NULL_HANDLE;
    uint64_t regs = 0, lmem = 0, binSize = 0;
};

// The kernels by role (the regen_world_spv.py symbols: the viewer's own pipelines)
enum K
{
    K_GI_TRACE, K_GI_MISS, K_GI_HITMAT, K_GI_HITLIGHT, K_GI_REUSE, K_GI_RESHADE,
    K_GI_HITMAT_VQ, K_GI_HITLIGHT_VQ, K_GI_HITREQ_VQ2, K_GI_HITLIGHT_VQ2,
    K_VIS_WALK,
    K_SH_SELECT, K_SH_SUNVIS, K_SH_SUNVIS_ISO, K_SH_VIS, K_SH_VIS_ISO, K_SH_SHADE,
    K_REFL_HALF, K_REFL_SHARP, K_REFL_VOTE, K_RT_SEL, K_RT_LIST, K_COUNT
};
const char* kSym[K_COUNT] = {
    "kWorldDeferGiSplitTraceSpv", "kWorldDeferGiSplitMissSpv", "kWorldDeferGiSplitHitMatSpv", "kWorldDeferGiSplitHitLightSpv",
    "kWorldDeferGiSplitReuseSpv", "kWorldDeferGiSplitReshadeSpv",
    "kWorldDeferGiSplitHitMatVqSpv", "kWorldDeferGiSplitHitLightVqSpv",
    "kWorldDeferGiSplitHitReqVq2Spv", "kWorldDeferGiSplitHitLightVq2Spv",
    "kWorldDeferVisWalkSpv",
    "kWorldDeferShadowMergeSelectSpv", "kWorldDeferShadowMergeSunVisSpv", "kWorldDeferShadowMergeSunVisIsoSpv",
    "kWorldDeferShadowSplitVisSpv", "kWorldDeferShadowSplitVisIsoSpv", "kWorldDeferShadowSplitShadeSpv",
    "kWorldDeferReflectHalfSpv", "kWorldDeferReflectSharpSpv",   // (perf-4k60: the split lighting's REFLECT pair)
    "kWorldDeferRtVoteSpv",   // (perf-4k60, optional: the split RT tile vote -- loaded only if its .spv exists)
    "kWorldDeferRtSelectSpv", "kWorldDeferRtTraceListSpv",   // (perf-4k60, --refl: the RT reflection ray list + its fused trace)
};

struct Opts
{
    std::vector<uint32_t> configs = {0u, 64u};
    uint32_t frames = 200, warmup = 60, rounds = 4;
    bool dry = false, verify = true, gi = true, shadow = true, isoLod = false, stats = true, refl = false;
    int device = -1;
    uint32_t sunRays = 1, giK = 1;
    float giMinShare = 0.02f;   // (bench bit 512: RenderVKVisQueueGIMinShare's value)
    bool serialize = false;
    std::string dumpDir;
    std::string spvDir = VQB_SPV_DIR, csv;
    VqbSceneCfg scene;
};

const char* kUsage =
    "usage: vqbench [options]\n"
    "  --configs a,b,..   configs to compare (default 0,64): 0 = the viewer's default chain (the GI hits through the\n"
    "                     queue, M2, in the frame walk; the shadow core fused), + the bits below\n"
    "  --frames N         timed frames per config and round (default 200)\n"
    "  --warmup N         untimed frames before each config's block (default 60)\n"
    "  --rounds N         round-robin rounds over the configs (default 4; cancels clock / thermal drift)\n"
    "  --no-gi | --no-shadow   leave a consumer out\n"
    "  --iso-lod          the shadow walks' iso-LOD variants (RenderVKShadowIsoLod 1)\n"
    "  config bits: 64 the GI hits on M1 (RenderVKVisQueue 64), 256 RenderVKVisQueueGISunOpaque,\n"
    "                     512 RenderVKVisQueueGIMinShare = --gi-min-share F (default 0.02),\n"
    "                     1024 RenderVKGIReuseVisSkip (#985)\n"
    "  --sun-rays N       sun disc rays (ssr.sun_rt.y; the viewer's vkRTShadowRays() is 1)   --gi-k N   GI hit shadow-ray budget (default 1)\n"
    "  --serialize        a barrier after every dispatch: per-kernel times attributable (totals slightly inflated)\n"
    "  --size WxH         render extent (default 2560x1440; GI and the shadow denoiser at half extent)\n"
    "  --lights P,J       point lights, projectors (default 48,16; at most 64 in all)\n"
    "  --seed N           scene seed (default 1039)\n"
    "  --no-verify        skip the output comparison against the first config\n"
    "  --csv FILE         per-frame samples\n"
    "  --dry              reflect + check the layout only (no GPU)\n"
    "  --device N         Vulkan device index\n";

Opts parseArgs(int argc, char** argv)
{
    Opts o;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "%s", kUsage); std::exit(2); } return argv[++i]; };
        if (a == "--configs")
        {
            o.configs.clear();
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) o.configs.push_back((uint32_t)std::strtoul(t.c_str(), nullptr, 0));
        }
        else if (a == "--frames") o.frames = (uint32_t)std::atoi(next().c_str());
        else if (a == "--warmup") o.warmup = (uint32_t)std::atoi(next().c_str());
        else if (a == "--rounds") o.rounds = (uint32_t)std::max(1, std::atoi(next().c_str()));
        else if (a == "--no-gi") o.gi = false;
        else if (a == "--no-shadow") o.shadow = false;
        else if (a == "--refl") o.refl = true;   // (perf-4k60) + the REFLECT pair (half + sharp) after the chain
        else if (a == "--iso-lod") o.isoLod = true;
        else if (a == "--sun-rays") o.sunRays = (uint32_t)std::atoi(next().c_str());
        else if (a == "--gi-min-share") o.giMinShare = (float)std::atof(next().c_str());
        else if (a == "--gi-k") o.giK = std::min<uint32_t>((uint32_t)std::atoi(next().c_str()), 4u);
        else if (a == "--size") { const std::string s = next(); std::sscanf(s.c_str(), "%ux%u", &o.scene.width, &o.scene.height); }
        else if (a == "--lights") { const std::string s = next(); std::sscanf(s.c_str(), "%u,%u", &o.scene.pointLights, &o.scene.projectors); }
        else if (a == "--seed") o.scene.seed = (uint32_t)std::strtoul(next().c_str(), nullptr, 0);
        else if (a == "--no-verify") o.verify = false;
        else if (a == "--csv") o.csv = next();
        else if (a == "--dry") o.dry = true;
        else if (a == "--device") o.device = std::atoi(next().c_str());
        else if (a == "--spv") o.spvDir = next();
        else if (a == "--no-stats") o.stats = false;
        else if (a == "--serialize") o.serialize = true;
        else if (a == "--dump-dir") o.dumpDir = next();
        else { std::fprintf(stderr, "unknown option '%s'\n%s", a.c_str(), kUsage); std::exit(2); }
    }
    if (o.scene.pointLights + o.scene.projectors > CLUSTER_CAP)
    {
        std::fprintf(stderr, "vqbench: at most %u lights (every light is listed in every cluster: CLUSTER_CAP)\n", CLUSTER_CAP);
        std::exit(2);
    }
    return o;
}

// ======================= named block writes (through the reflected layout) =======================
struct Block
{
    uint32_t set = 0, binding = 0;
    std::vector<uint8_t> bytes;
    const std::vector<Kernel>* ks = nullptr;
    VqbField find(const std::string& path) const
    {
        for (const Kernel& k : *ks)
        {
            const VqbField f = k.refl.field(set, binding, path);
            if (f.ok) return f;
        }
        return VqbField{};
    }
    void put(const std::string& path, const void* data, size_t n)
    {
        const VqbField f = find(path);
        if (!f.ok) { std::fprintf(stderr, "vqbench: no field '%s' in set %u binding %u (shader layout changed?)\n", path.c_str(), set, binding); std::exit(1); }
        if (n > f.size) { std::fprintf(stderr, "vqbench: field '%s' is %u B, %zu given\n", path.c_str(), f.size, n); std::exit(1); }
        if (bytes.size() < f.offset + n) bytes.resize(f.offset + n);
        std::memcpy(bytes.data() + f.offset, data, n);
    }
    void v4(const std::string& path, float x, float y, float z, float w) { const float v[4] = {x, y, z, w}; put(path, v, 16); }
    void u4(const std::string& path, uint32_t x, uint32_t y, uint32_t z, uint32_t w) { const uint32_t v[4] = {x, y, z, w}; put(path, v, 16); }
    void m4(const std::string& path, const float* m) { put(path, m, 64); }
};

uint32_t median(std::vector<double> v, double& out)
{
    if (v.empty()) { out = 0; return 0; }
    std::sort(v.begin(), v.end());
    out = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    return (uint32_t)v.size();
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
} // namespace

int main(int argc, char** argv)
{
    Opts opt = parseArgs(argc, argv);

    // ---- the kernels + their reflection; ONE layout over all of them (the viewer's mRtReflLayout) ----
    std::vector<Kernel> ks(K_COUNT);
    for (uint32_t k = 0; k < K_COUNT; ++k)
    {
        ks[k].sym = kSym[k];
        if ((k == K_REFL_VOTE || k == K_RT_SEL || k == K_RT_LIST) && !std::ifstream(opt.spvDir + "/" + kSym[k] + ".spv")) continue;   // (optional)
        ks[k].code = readWords(opt.spvDir + "/" + kSym[k] + ".spv");
        std::string err;
        if (!ks[k].refl.parse(readWords(opt.spvDir + "/" + kSym[k] + ".names.spv"), err))
        { std::fprintf(stderr, "vqbench: %s: %s\n", kSym[k], err.c_str()); return 1; }
        if (ks[k].refl.pushSize() > sizeof(RtReflPush))
        { std::fprintf(stderr, "vqbench: %s push is %u B (VKRtReflPush 128)\n", kSym[k], ks[k].refl.pushSize()); return 1; }
    }
    std::map<uint32_t, std::map<uint32_t, VqbBindingInfo>> layout;
    {
        std::vector<const VqbModule*> mods;
        for (const Kernel& k : ks) if (!k.code.empty()) mods.push_back(&k.refl);
        std::string err;
        if (!vqbUnionBindings(mods, layout, err)) { std::fprintf(stderr, "vqbench: layout: %s\n", err.c_str()); return 1; }
    }
    uint32_t nSets = 0;
    for (auto& s : layout) nSets = std::max(nSets, s.first + 1);
    // ---- the scene (CPU) and the uniform blocks it fills, by name (the viewer's declarations) ----
    VqbScene scene;
    scene.generate(opt.scene);
    const uint32_t RW = opt.scene.width, RH = opt.scene.height;
    const uint32_t DW = (RW + 1) / 2, DH = (RH + 1) / 2;   // GI (RenderVKGIScale 0.5) = the shadow denoiser (RenderVKDenoiseScale 0.5)
    const uint32_t N = DW * DH;
    std::function<void(uint32_t, uint32_t, const std::function<void(Block&)>&)> fillUbo;
    const VqbVec3 E = scene.eye, L = scene.sunDir;
    const float tanY = std::tan(scene.fovY * .5f), tanX = tanY * (float)RW / (float)RH;
    auto fillAll = [&]()
    {
        // set 0 binding 9: THE FRAME INFO (clean_frame_info.glsl; LLCleanRendererVK::VKSSRInfo)
        fillUbo(0, 9, [&](Block& b)
        {
            b.v4("atmos[6]", 0.f, 1.f, 1.f, 0.f);           // atmosphere on, HDR scale 1
            b.v4("atmos[7]", L.x, L.y, L.z, 0.f);           // the light direction (sun)
            b.v4("atmos_k[0]", 2.6f, 2.45f, 2.2f, 0.f);     // sunlit
            b.v4("atmos_k[1]", 2e-4f, 2.5e-4f, 3e-4f, 0.f); // haze extinction
            b.v4("atmos_k[4]", 0.30f, 0.34f, 0.42f, 0.f);   // amblit
            b.v4("atmos_k[5]", 2.6f, 2.45f, 2.2f, 0.f);     // sunlit (x sky_sunlight_scale)
            b.v4("shadow_clip", 32.f, 64.f, 128.f, 512.f);
            b.v4("shadow_params", 0.f, 0.f, 0.f, 2.f);      // ray-traced sun
            // (llcleanrenderervk.cpp: the disc's angular radius = RenderVKRTShadowSoftness 0.01 x 5 degrees, the rays
        //  vkRTShadowRays() = 1, RenderVKRTShadowBias 0)
        b.v4("sun_rt", 0.05f * 0.01745329252f, (float)opt.sunRays, 0.f, 1000.f);
            b.v4("render_opts", 0.f, 0.f, 1.f, 0.f);        // translucent (coloured) shadows
            // rtBlendOpts at the viewer's defaults (llcleanrenderervk.cpp lit_dbg[3]): blend parity 1 | the translucent
            // split 2 | probe blend coarse 4 | terminator lift 8 | terminator soften 16 | the adaptive tolerance
            // 0.0625 x 256 << 8 | the transparent-receiver cut-out distance 16 << 16. (Left 0, the bench ran the mixed
            // disc walk: RenderVKRTTranslucentSplit 0.)
            b.v4("lit_dbg", 0.f, 0.f, 0.f, (float)(31u | (16u << 8) | (16u << 16)));
            b.v4("rt_mask", -1.f, 0.f, 0.f, 0.f);           // merged RT shadows: the mask off for every reader
            // foliage transmission mode 2 (tinted) | 32 shadowed projector ambiance | 128 GI ran this frame
            b.v4("foliage_rt", (float)(2u | 32u | 128u), 1.f, 0.5f, 4.f);
            b.v4("cam_origin", E.x, E.y, E.z, 0.f);
            // (perf-4k60, --refl; VQB_SKY_LUT=1) the sky-view LUT ready (ao_up.z): skyColor reads the table, as live
            if (opt.refl && std::getenv("VQB_SKY_LUT")) b.v4("ao_up", 0.f, 0.f, 1.f, 0.f);
            // (perf-4k60, --refl; VQB_SKY=1) a WindLight-like haze sky (midday), so the reflection fallback's sky has content
            if (opt.refl && std::getenv("VQB_SKY"))
            {
                b.v4("atmos[0]", 0.24f, 0.45f, 0.80f, 0.19f);   // blue horizon, haze horizon
                b.v4("atmos[1]", 0.40f, 0.60f, 0.90f, 0.70f);   // blue density, haze density
                b.v4("atmos[2]", 0.f, 0.f, 0.f, 0.30f);         // cloud shadow
                b.v4("atmos[3]", 0.35f, 0.35f, 0.35f, 1605.f);  // ambient, max y
                b.v4("atmos[4]", L.x, L.y, L.z, 0.0003f);       // light norm (z up), density multiplier
                b.v4("atmos[5]", 5.f, 0.001f, -0.48f, 0.f);     // glow
                b.v4("atmos[6]", 1.f, 1.f, 1.f, 0.f);           // sun / moon glow factor, HDR scale 1
                b.v4("atmos[8]", 1.6f, 1.5f, 1.3f, 0.f);        // sunlight
                b.v4("atmos[9]", 0.3f, 0.3f, 0.4f, 1.f);        // moonlight, sun up
            }
            b.m4("views[0].cur_view", scene.view);
            b.m4("views[0].prev_view", scene.view);
            b.m4("views[0].prev_proj", scene.proj);
            b.m4("views[0].taa_cur_vp", scene.vpRel);
            b.m4("views[0].taa_prev_vp", scene.vpRel);   // a static camera: last frame's = this frame's
        });
        // set 0 binding 6: THE LIGHT INFO
        fillUbo(0, 6, [&](Block& b)
        {
            b.v4("linfo.cam_pos", E.x, E.y, E.z, (float)scene.lights.size());
            b.v4("linfo.cam_fwd", -scene.view[2], -scene.view[6], -scene.view[10], 0.f);
            b.v4("linfo.zinfo", scene.zNear, scene.zFar, (float)GZ / std::log(scene.zFar / scene.zNear), scene.zFar);
            b.v4("linfo.grid", (float)RW / GX, (float)RH / GY, tanX, tanY);
            float Vabs[16];
            std::memcpy(Vabs, scene.view, 64);
            Vabs[12] = -(Vabs[0] * E.x + Vabs[4] * E.y + Vabs[8] * E.z);
            Vabs[13] = -(Vabs[1] * E.x + Vabs[5] * E.y + Vabs[9] * E.z);
            Vabs[14] = -(Vabs[2] * E.x + Vabs[6] * E.y + Vabs[10] * E.z);
            b.m4("linfo.view", Vabs);
            b.m4("linfo.fx_vp", scene.vpRel);
            b.v4("linfo.fx_eye", E.x, E.y, E.z, 0.f);
            b.v4("linfo.fx_extent", (float)RW, (float)RH, 1.f / RW, 1.f / RH);
        });
        // set 2 binding 10: the main view's DRAW VIEW (clean_draw_view.glsl)
        fillUbo(2, 10, [&](Block& b)
        {
            b.m4("draw_view_main[0].u_view_proj", scene.vpRel);
            b.v4("draw_view_main[0].u_sun_dir", L.x, L.y, L.z, 0.f);
            b.v4("draw_view_main[0].u_sun_color", 1.f, 0.95f, 0.85f, 0.f);
            b.v4("draw_view_main[0].u_ambient_color", 0.25f, 0.3f, 0.4f, 0.f);
            b.v4("draw_view_main[0].u_cam_time", E.x, E.y, E.z, 10.f);
            b.m4("draw_view_main[0].u_inv_view_proj", scene.invVpAbs);
            b.v4("draw_view_main[0].u_origin", E.x, E.y, E.z, 0.f);
        });
    };
    // THE GI HEADER (llcleangivk.cpp giWriteHeader at the defaults; the cache off -- see README)
    auto giHeader = [&](uint32_t frame, uint32_t parity, bool resPrev, bool visSkip)
    {
        Block b;
        b.ks = &ks; b.set = 3; b.binding = 23;
        b.bytes.assign(5344, 0);
        uint32_t f = (kGiFlagsDefault & ~(GI_F_CACHE | GI_F_CACHE_FILL)) | GI_F_STATS;
        if (!resPrev) f &= ~GI_F_RES_PREV;
        if (visSkip) f |= GI_F_VIS_TSKIP;
        b.u4("gi.cfg", f, frame, 0u, 0u);
        b.v4("gi.prm", 1.f, 0.f, 64.f, 2.f);
        b.v4("gi.prm2", 16.f, 3.f, 16.f, 4.f);
        b.v4("gi.prm3", 0.25f, 8.f, 32.f, 600.f);
        b.v4("gi.prm4", 0.05f, 2.f, 4.f, (float)parity);
        b.v4("gi.prm5", 0.1f, 0.f, 1.5f, 4.f);
        return b.bytes;
    };
    if (opt.dry)
    {
        for (auto& s : layout)
            for (auto& b : s.second)
                std::printf("set %u binding %2u type %2d count %u%s dim %u%s fmt %u block %u B  %s\n", b.second.set, b.second.binding,
                            (int)b.second.type, b.second.count, b.second.runtimeArray ? " (runtime)" : "", b.second.dim,
                            b.second.arrayed ? " array" : "", b.second.spvFormat, b.second.blockSize, b.second.name.c_str());
        for (const Kernel& k : ks) std::printf("%-40s %zu words, push %u B\n", k.sym.c_str(), k.code.size(), k.refl.pushSize());
        // every named field the bench writes must resolve (a missing one exits: the shader layout changed)
        fillUbo = [&](uint32_t s, uint32_t binding, const std::function<void(Block&)>& f)
        {
            Block bl;
            bl.ks = &ks; bl.set = s; bl.binding = binding;
            f(bl);
            std::printf("block set %u binding %u: %zu B written by name\n", s, binding, bl.bytes.size());
        };
        fillAll();
        std::printf("GI header: %zu B written by name\n", giHeader(0, 0, true, false).size());
        Block t;
        t.ks = &ks;
        const char* probe[][3] = {{"0", "9", "sun_rt"}, {"0", "9", "views[0].taa_prev_vp"}, {"0", "6", "linfo.cam_pos"},
                                  {"2", "10", "draw_view_main[0].u_cam_time"}, {"3", "23", "gi.prm5"}};
        for (auto& p : probe)
        {
            t.set = (uint32_t)std::atoi(p[0]); t.binding = (uint32_t)std::atoi(p[1]);
            const VqbField f = t.find(p[2]);
            std::printf("field set %s b %s %-28s %s offset %u size %u\n", p[0], p[1], p[2], f.ok ? "ok" : "MISSING", f.offset, f.size);
        }
        return 0;
    }

    // ======================= device, scene =======================
    VqbCtx ctx;
    ctx.init(opt.device);
    std::printf("vqbench (#1039) on %s (subgroup %u)\n", ctx.deviceName.c_str(), ctx.subgroupSize);
    {
        std::ifstream kf(opt.spvDir + "/kernels.txt");
        std::string line;
        if (std::getline(kf, line)) std::printf("kernels: %s (vqb_spv.py, the viewer's regen_world_spv.py flags)\n", line.c_str());
    }
    scene.build(ctx);
    std::printf("scene: seed %u, %zu instances (%u opaque, %u alpha-mask, %u blend), %u triangles, %u point lights + %u projectors; "
                "render %ux%u, GI / denoiser %ux%u\n", opt.scene.seed, scene.instances.size(), scene.opaqueInst, scene.maskInst,
                scene.blendInst, scene.triCount, opt.scene.pointLights, opt.scene.projectors, RW, RH, DW, DH);

    const VkBufferUsageFlags SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    const VkImageUsageFlags IMG = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // ---- the G-buffer (render extent, 1 layer: texture2DArray), generated from the TLAS ----
    VqbImage gb[5];
    for (int i = 0; i < 4; ++i) gb[i] = ctx.image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R16G16B16A16_SFLOAT, RW, RH, 1, 1, IMG);
    gb[4] = ctx.image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R32_SFLOAT, RW, RH, 1, 1, IMG);

    // ---- the consumers' buffers (sizes: llcleangivk.cpp / llcleanprojstochvk.cpp / llcleanvisqueuevk.cpp) ----
    const uint32_t S = 8;   // pdnVisSlotsWanted at the defaults
    const uint64_t giHitU4 = 3ull + 2ull * ((N + 3) / 4) + 6ull * N;
    VqbBuffer bGiHit = ctx.buffer(giHitU4 * 16, SSBO, false);
    VqbBuffer bGiRes = ctx.buffer(4ull * N * 16, SSBO, false);
    VqbBuffer bGiState = ctx.buffer(5344 + 64, SSBO, false);
    VqbBuffer bPdnSel = ctx.buffer(64ull * N, SSBO, false);
    VqbBuffer bPdnVis = ctx.buffer(16ull + 8ull * N * S, SSBO, false);
    // the queue: the GI hits' region WHOLE (no fused tail in the A/B); an M2 entry = (1 + K) requests + (2 + K) slots
    const uint32_t K2 = opt.giK;
    const uint64_t vqBytes = (kVqHdrBytes + (uint64_t)N * ((1ull + K2) * kVqReqBytes + (2ull + K2) * kVqSlotBytes) + 255) & ~255ull;
    VqbBuffer bVq = ctx.buffer(vqBytes, SSBO, false);
    std::printf("buffers: GI records %.1f MB, reservoirs %.1f MB, pdn_sel %.1f MB, pdn_vis %.1f MB, the queue %.1f MB (whole region)\n",
                bGiHit.size / 1048576.0, bGiRes.size / 1048576.0, bPdnSel.size / 1048576.0, bPdnVis.size / 1048576.0, vqBytes / 1048576.0);

    // ---- lights: pshadow records for the projectors (ray traced: params.z 2), the cluster lists ----
    struct ProjShadow { float vp[16]; float rect[4]; float params[4]; float texel[4]; };
    std::vector<ProjShadow> pshadow;
    std::vector<uint32_t> order;   // projectors first (clusterShadowedLen leads)
    for (uint32_t i = 0; i < scene.lights.size(); ++i)
        if (scene.lights[i].proj_origin[3] > 0.5f)
        {
            ProjShadow p{};
            p.params[1] = 0.1f; p.params[2] = 2.f; p.params[3] = 1.f;
            p.texel[0] = 0.01f; p.texel[1] = 0.f; p.texel[2] = 1.f; p.texel[3] = 0.f;   // (softness 0.01 m, 1 ray: the viewer's)
            pshadow.push_back(p);
            scene.lights[i].tex[2] = (uint32_t)pshadow.size();   // record + 1
            order.push_back(i);
        }
    const uint32_t nProj = (uint32_t)order.size();
    for (uint32_t i = 0; i < scene.lights.size(); ++i)
        if (scene.lights[i].proj_origin[3] <= 0.5f) order.push_back(i);
    ctx.upload(scene.bLights, 0, scene.lights.data(), scene.lights.size() * sizeof(VqbLightRec));
    VqbBuffer bPShadow = ctx.bufferWith(std::max<size_t>(pshadow.size(), 1) * sizeof(ProjShadow), SSBO, pshadow.data(), pshadow.size() * sizeof(ProjShadow));
    // EVERY light in EVERY cluster (each light's own range test decides; the viewer's lists are culled per cell)
    const uint32_t nl = (uint32_t)order.size();
    std::vector<uint32_t> ccount(CLUSTER_COUNT + 16, nl | (nProj << 8) | (nl << 16)), cindex((CLUSTER_COUNT + 16) * CLUSTER_CAP, 0u);
    for (uint32_t c = 0; c < CLUSTER_COUNT + 16; ++c)
        for (uint32_t i = 0; i < nl; ++i) cindex[c * CLUSTER_CAP + i] = order[i];
    VqbBuffer bCCount = ctx.bufferWith(ccount.size() * 4, SSBO, ccount.data(), ccount.size() * 4);
    VqbBuffer bCIndex = ctx.bufferWith(cindex.size() * 4, SSBO, cindex.data(), cindex.size() * 4);

    // ======================= descriptor layout + resources for every reflected binding =======================
    VkSampler sampLin, sampCmp;
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod = 16.f;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(16.f, ctx.props.limits.maxSamplerAnisotropy);
        VQB_CK(vkCreateSampler(ctx.dev, &si, nullptr, &sampLin));
        si.anisotropyEnable = VK_FALSE;
        si.compareEnable = VK_TRUE;
        si.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VQB_CK(vkCreateSampler(ctx.dev, &si, nullptr, &sampCmp));
    }
    const uint32_t kTexTable = 64;   // the bindless table's written size (the scene's 4 textures, cycled)
    std::vector<VkDescriptorSetLayout> dsl(nSets);
    std::map<VkDescriptorType, uint32_t> poolCount;
    for (uint32_t s = 0; s < nSets; ++s)
    {
        std::vector<VkDescriptorSetLayoutBinding> bs;
        std::vector<VkDescriptorBindingFlags> fl;
        for (auto& kv : layout[s])
        {
            const VqbBindingInfo& b = kv.second;
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = b.binding;
            lb.descriptorType = b.type;
            lb.descriptorCount = b.runtimeArray ? kTexTable : std::max(b.count, 1u);
            lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bs.push_back(lb);
            fl.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
            poolCount[b.type] += lb.descriptorCount;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = (uint32_t)fl.size();
        bf.pBindingFlags = fl.data();
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
        lci.bindingCount = (uint32_t)bs.size();
        lci.pBindings = bs.data();
        VQB_CK(vkCreateDescriptorSetLayout(ctx.dev, &lci, nullptr, &dsl[s]));
    }
    VkPipelineLayout pl;
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtReflPush)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = nSets;
        plci.pSetLayouts = dsl.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VQB_CK(vkCreatePipelineLayout(ctx.dev, &plci, nullptr, &pl));
    }
    VkDescriptorPool dpool;
    {
        std::vector<VkDescriptorPoolSize> ps;
        for (auto& kv : poolCount) ps.push_back({kv.first, kv.second + 8});
        ps.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16});
        ps.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16});
        ps.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8});
        ps.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2});
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = nSets + 2;
        pci.poolSizeCount = (uint32_t)ps.size();
        pci.pPoolSizes = ps.data();
        VQB_CK(vkCreateDescriptorPool(ctx.dev, &pci, nullptr, &dpool));
    }
    std::vector<VkDescriptorSet> sets(nSets);
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool;
        ai.descriptorSetCount = nSets;
        ai.pSetLayouts = dsl.data();
        VQB_CK(vkAllocateDescriptorSets(ctx.dev, &ai, sets.data()));
    }

    // the named resources (the viewer's bindings by block / variable name); everything else a zeroed placeholder
    const uint32_t nRtTiles = ((RW + 7) / 8) * ((RH + 7) / 8);   // (perf-4k60) the RT reflection tile list (recordRtMarkInit)
    VqbBuffer bRtTiles = ctx.buffer(68u * 4u + (2u * nRtTiles + 16u) * 4u, SSBO | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    // (perf-4k60, --refl) THE DEFAULT SKY PROBE (probe 0, as the viewer always has it): the probe walk runs as in a
    // live frame -- header (count 1, max lod 6, sky ambiance 1, enabled), probe 0 a huge sphere around the eye,
    // cube slot 0, complete, the default class; every cluster lists no other probe
    VqbBuffer bProbes;
    {
        std::vector<float> pb(4 + 28, 0.f);
        pb[0] = 1.f; pb[1] = 6.f; pb[2] = 1.f; pb[3] = (opt.refl && !std::getenv("VQB_NO_PROBES")) ? 1.f : 0.f;   // (VQB_NO_PROBES: the sky fallback path)
        pb[4] = scene.eye.x; pb[5] = scene.eye.y; pb[6] = scene.eye.z; pb[7] = 1.0e6f;
        for (int i = 0; i < 4; ++i) pb[8 + i * 5] = 1.f;   // box: identity
        pb[24] = 1.f; pb[25] = 1.f; pb[26] = 0.f; pb[27] = 0.f;
        const int32_t idx[4] = {0, 1, 0, 0};
        std::memcpy(&pb[28], idx, 16);
        bProbes = ctx.bufferWith(1u << 16, SSBO, pb.data(), pb.size() * 4);
    }
    std::map<std::string, VqbBuffer> namedBuf = {
        {"RtReflTiles", bRtTiles}, {"Probes", bProbes},
        {"RtInstances", scene.bInstances}, {"PrevInstances", scene.bInstances}, {"RtMaterials", scene.bMaterials}, {"Materials", scene.bMaterials},
        {"RtGeom", scene.bGeom}, {"RtVbo", scene.bVbo}, {"RtIbo", scene.bIbo}, {"RtGeomVc", scene.bGeomVc},
        {"Lights", scene.bLights}, {"CCount", bCCount}, {"CIndex", bCIndex}, {"ProjShadows", bPShadow},
        {"GiState", bGiState}, {"GiReservoirs", bGiRes}, {"GiRecords", bGiHit}, {"PdnSelect", bPdnSel}, {"PdnVis", bPdnVis}};
    struct ImgSpec { uint32_t w, h, layers; };
    std::map<std::string, ImgSpec> namedImg = {
        {"uGiIn", {DW, DH, 2}}, {"uGiGeom", {DW, DH, 2}}, {"uGiMot", {DW, DH, 1}}, {"uGiOut", {DW, DH, 2}},
        {"uPdnIn", {DW, DH, 4}}, {"uPdnGeom", {DW, DH, 5}}, {"uPdnMot", {DW, DH, 1}}, {"uPdnOut", {DW, DH, 4}},
        // (perf-4k60) the REFLECT pair's outputs: the render extent, the half-resolution blocks
        {"uReflIrr", {RW, RH, 1}}, {"uReflSpec", {RW, RH, 1}}, {"uReflEnv", {RW, RH, 1}}, {"uReflRtMark", {RW, RH, 1}},
        {"uReflHalf", {DW, DH, 1}}, {"uVisSun", {RW, RH, 1}}, {"uVisProj", {RW, RH, 4}}, {"uRtTrace", {RW, RH, 1}}};
    std::map<std::string, VqbImage> madeImg;
    std::vector<VqbBuffer> stateBufs = {bGiHit, bGiRes, bPdnSel, bPdnVis, bVq};
    std::map<std::pair<uint32_t, uint32_t>, VqbBuffer> uboOf;   // the uniform blocks the bench fills
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<std::unique_ptr<VkDescriptorBufferInfo>> bInfos;
    std::vector<std::unique_ptr<std::vector<VkDescriptorImageInfo>>> iInfos;
    std::vector<std::unique_ptr<VkWriteDescriptorSetAccelerationStructureKHR>> asInfos;
    auto fmtOf = [](const VqbBindingInfo& b) -> VkFormat
    {
        switch (b.spvFormat)
        {
        case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 3: return VK_FORMAT_R32_SFLOAT;
        case 4: return VK_FORMAT_R8G8B8A8_UNORM;
        case 6: return VK_FORMAT_R32G32_SFLOAT;
        case 7: return VK_FORMAT_R16G16_SFLOAT;
        case 8: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 9: return VK_FORMAT_R16_SFLOAT;
        case 13: return VK_FORMAT_R8G8_UNORM;
        case 15: return VK_FORMAT_R8_UNORM;
        case 24: return VK_FORMAT_R32_SINT;
        case 30: return VK_FORMAT_R32G32B32A32_UINT;
        case 31: return VK_FORMAT_R16G16B16A16_UINT;
        case 32: return VK_FORMAT_R8G8B8A8_UINT;
        case 33: return VK_FORMAT_R32_UINT;
        case 35: return VK_FORMAT_R32G32_UINT;
        case 38: return VK_FORMAT_R16_UINT;
        default: break;
        }
        if (b.depth) return VK_FORMAT_D32_SFLOAT;
        return b.sampledKind == 0 ? VK_FORMAT_R16G16B16A16_SFLOAT : (b.sampledKind == 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT);
    };
    auto viewTypeOf = [](const VqbBindingInfo& b) -> VkImageViewType
    {
        if (b.dim == 2) return VK_IMAGE_VIEW_TYPE_3D;
        if (b.dim == 3) return b.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        return b.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    };
    for (uint32_t s = 0; s < nSets; ++s)
        for (auto& kv : layout[s])
        {
            const VqbBindingInfo& b = kv.second;
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[s];
            w.dstBinding = b.binding;
            w.descriptorType = b.type;
            if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            {
                VqbBuffer buf;
                auto it = namedBuf.find(b.name);
                if (it != namedBuf.end()) buf = it->second;
                else
                {
                    const uint64_t sz = b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? std::max<uint32_t>(b.blockSize, 16u)
                                                                                    : std::max<uint64_t>(b.blockSize + 4096ull * std::max(b.runtimeStride, 16u), 1u << 16);
                    buf = ctx.buffer(sz, SSBO | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false);
                    ctx.oneShot([&](VkCommandBuffer c) { vkCmdFillBuffer(c, buf.buf, 0, VK_WHOLE_SIZE, 0u); });
                    if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) uboOf[{s, b.binding}] = buf;
                }
                auto bi = std::make_unique<VkDescriptorBufferInfo>();
                bi->buffer = buf.buf; bi->offset = 0; bi->range = VK_WHOLE_SIZE;
                if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) bi->range = std::min<VkDeviceSize>(buf.size, ctx.props.limits.maxUniformBufferRange);
                w.descriptorCount = 1;
                w.pBufferInfo = bi.get();
                bInfos.push_back(std::move(bi));
            }
            else if (b.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
            {
                auto as = std::make_unique<VkWriteDescriptorSetAccelerationStructureKHR>();
                as->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                as->accelerationStructureCount = 1;
                as->pAccelerationStructures = &scene.tlas.as;
                w.pNext = as.get();
                w.descriptorCount = 1;
                asInfos.push_back(std::move(as));
            }
            else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER)
            {
                auto ii = std::make_unique<std::vector<VkDescriptorImageInfo>>(1);
                (*ii)[0].sampler = sampLin;
                w.descriptorCount = 1;
                w.pImageInfo = ii->data();
                iInfos.push_back(std::move(ii));
            }
            else   // images
            {
                const uint32_t n = b.runtimeArray ? kTexTable : std::max(b.count, 1u);
                auto ii = std::make_unique<std::vector<VkDescriptorImageInfo>>(n);
                for (uint32_t e = 0; e < n; ++e)
                {
                    VkImageView v;
                    if (b.name == "uTextures") v = scene.textures[e % scene.textures.size()].view;
                    else if (b.name == "uG0") v = gb[0].view;
                    else if (b.name == "uG1") v = gb[1].view;
                    else if (b.name == "uG2") v = gb[2].view;
                    else if (b.name == "uG3") v = gb[3].view;
                    else if (b.name == "uDepthG") v = gb[4].view;
                    else
                    {
                        const std::string key = b.name + "#" + std::to_string(s) + "." + std::to_string(b.binding);
                        auto mi = madeImg.find(key);
                        if (mi == madeImg.end())
                        {
                            auto ns = namedImg.find(b.name);
                            ImgSpec sp = ns != namedImg.end() ? ns->second
                                       : ImgSpec{b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? DW : RW, b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? DH : RH,
                                                 b.arrayed ? 4u : 1u};   // (render-extent placeholders: no texel fetch lands outside one)
                            VkImageViewType vt = viewTypeOf(b);
                            if (vt == VK_IMAGE_VIEW_TYPE_CUBE) sp = {4, 4, 6};
                            if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) sp = {4, 4, 12};
                            if (vt == VK_IMAGE_VIEW_TYPE_3D) sp = {4, 4, 4};
                            if (!b.arrayed && vt == VK_IMAGE_VIEW_TYPE_2D) sp.layers = 1;
                            VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
                            mi = madeImg.emplace(key, ctx.image(vt, fmtOf(b), sp.w, sp.h, sp.layers, 1, u, b.depth)).first;
                        }
                        v = mi->second.view;
                    }
                    (*ii)[e].imageView = v;
                    (*ii)[e].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    (*ii)[e].sampler = b.depth ? sampCmp : sampLin;
                }
                w.descriptorCount = n;
                w.pImageInfo = ii->data();
                iInfos.push_back(std::move(ii));
            }
            writes.push_back(w);
        }
    vkUpdateDescriptorSets(ctx.dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    // (perf-4k60, --refl) the probe cubes get content (a colour per cube face / layer), so the probe walk's arithmetic
    // shows in the REFLECT outputs the verification compares (zero placeholders would hide any difference)
    if (opt.refl)
        for (auto& kv : madeImg)
            if (kv.first.rfind("uProbeRadiance#", 0) == 0 || kv.first.rfind("uProbeIrradiance#", 0) == 0)
            {
                const VqbImage& im = kv.second;
                ctx.oneShot([&](VkCommandBuffer c)
                {
                    for (uint32_t l = 0; l < im.layers; ++l)
                    {
                        VkClearColorValue col{};
                        col.float32[0] = 0.15f + 0.11f * (float)(l % 5); col.float32[1] = 0.25f + 0.07f * (float)(l % 7);
                        col.float32[2] = 0.35f + 0.05f * (float)(l % 3); col.float32[3] = 1.f;
                        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, l, 1};
                        vkCmdClearColorImage(c, im.img, VK_IMAGE_LAYOUT_GENERAL, &col, 1, &r);
                    }
                });
            }
    std::vector<VqbImage> stateImgs;
    for (const char* nm : {"uGiIn", "uGiGeom", "uGiMot", "uPdnIn", "uPdnGeom", "uPdnMot", "uPdnOut"})
        for (auto& kv : madeImg)
            if (kv.first.rfind(std::string(nm) + "#", 0) == 0) stateImgs.push_back(kv.second);

    // ======================= the uniform blocks (by name, the viewer's declarations) =======================
    fillUbo = [&](uint32_t s, uint32_t binding, const std::function<void(Block&)>& f)
    {
        auto it = uboOf.find({s, binding});
        if (it == uboOf.end()) { std::fprintf(stderr, "vqbench: set %u binding %u is not a placeholder UBO\n", s, binding); std::exit(1); }
        Block bl;
        bl.ks = &ks; bl.set = s; bl.binding = binding;
        bl.bytes.assign(it->second.size, 0);
        f(bl);
        ctx.upload(it->second, 0, bl.bytes.data(), bl.bytes.size());
    };
    fillAll();

    // ======================= pipelines (spec 1 = kDebugViews off, as vqLitSpec / giLitSpec / pdnLitSpec) =======================
    VkPipelineCache pcache;
    const std::string pcPath = opt.spvDir + "/vqbench.pcache";
    {
        std::ifstream f(pcPath, std::ios::binary);
        std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = d.size();
        ci.pInitialData = d.empty() ? nullptr : d.data();
        if (vkCreatePipelineCache(ctx.dev, &ci, nullptr, &pcache) != VK_SUCCESS)
        {
            ci.initialDataSize = 0; ci.pInitialData = nullptr;
            VQB_CK(vkCreatePipelineCache(ctx.dev, &ci, nullptr, &pcache));
        }
    }
    {
        const double t0 = nowS();
        const VkBool32 off = VK_FALSE;
        const VkSpecializationMapEntry me{1u, 0u, sizeof(VkBool32)};
        const VkSpecializationInfo si{1u, &me, sizeof(VkBool32), &off};
        for (Kernel& k : ks)
        {
            if (k.code.empty()) continue;
            // (reflection / the layout above keep every kernel; no driver compile for an unused consumer's)
            if (!opt.gi && k.sym.find("DeferShadow") == std::string::npos) continue;
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = k.code.size() * 4;
            smci.pCode = k.code.data();
            VkShaderModule sm;
            VQB_CK(vkCreateShaderModule(ctx.dev, &smci, nullptr, &sm));
            VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.flags = opt.stats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0;
            cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", &si};
            cpci.layout = pl;
            VQB_CK(vkCreateComputePipelines(ctx.dev, pcache, 1, &cpci, nullptr, &k.pipe));
            vkDestroyShaderModule(ctx.dev, sm, nullptr);
            if (opt.stats && ctx.getExe && ctx.getStat)
            {
                VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
                pi.pipeline = k.pipe;
                uint32_t ne = 0;
                ctx.getExe(ctx.dev, &pi, &ne, nullptr);
                for (uint32_t e = 0; e < ne; ++e)
                {
                    VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
                    ei.pipeline = k.pipe;
                    ei.executableIndex = e;
                    uint32_t ns = 0;
                    ctx.getStat(ctx.dev, &ei, &ns, nullptr);
                    std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
                    ctx.getStat(ctx.dev, &ei, &ns, st.data());
                    for (auto& x : st)
                    {
                        const uint64_t v = x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? x.value.u64
                                         : (x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? (uint64_t)x.value.i64 : 0u);
                        if (std::strstr(x.name, "Register Count")) k.regs = v;
                        else if (std::strstr(x.name, "Local Memory")) k.lmem = v & 0xFFFFFFFFull;   // (NVIDIA sets bit 36: the low word is the size)
                        else if (std::strstr(x.name, "Binary Size")) k.binSize = v;
                    }
                }
            }
        }
        size_t n = 0;
        vkGetPipelineCacheData(ctx.dev, pcache, &n, nullptr);
        std::vector<char> d(n);
        vkGetPipelineCacheData(ctx.dev, pcache, &n, d.data());
        std::ofstream(pcPath, std::ios::binary).write(d.data(), (std::streamsize)n);
        std::printf("pipelines: %u kernels in %.1f s\n", (uint32_t)K_COUNT, nowS() - t0);
        for (const Kernel& k : ks)
            std::printf("  %-38s regs %3llu  lmem %5llu  spv %7zu words\n", k.sym.c_str() + 6, (unsigned long long)k.regs,
                        (unsigned long long)k.lmem, k.code.size());
    }

    // ======================= the G-buffer generator (the bench's own pipeline) =======================
    {
        const std::vector<uint32_t> code = readWords(opt.spvDir + "/vqb_gbuf.spv");
        VkDescriptorSetLayoutBinding bs[12] = {};
        const VkDescriptorType ty[12] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        for (uint32_t i = 0; i < 12; ++i) bs[i] = {i, ty[i], i == 4 ? 4u : 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 12; lci.pBindings = bs;
        VkDescriptorSetLayout gl;
        VQB_CK(vkCreateDescriptorSetLayout(ctx.dev, &lci, nullptr, &gl));
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1; plci.pSetLayouts = &gl;
        VkPipelineLayout gpl;
        VQB_CK(vkCreatePipelineLayout(ctx.dev, &plci, nullptr, &gpl));
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4; smci.pCode = code.data();
        VkShaderModule sm;
        VQB_CK(vkCreateShaderModule(ctx.dev, &smci, nullptr, &sm));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
        cpci.layout = gpl;
        VkPipeline gp;
        VQB_CK(vkCreateComputePipelines(ctx.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &gp));
        vkDestroyShaderModule(ctx.dev, sm, nullptr);
        struct { float vp[16], ivp[16], eye[4], ext[4]; } cam;
        std::memcpy(cam.vp, scene.vpRel, 64); std::memcpy(cam.ivp, scene.invVpRel, 64);
        cam.eye[0] = E.x; cam.eye[1] = E.y; cam.eye[2] = E.z; cam.eye[3] = 0;
        cam.ext[0] = (float)RW; cam.ext[1] = (float)RH; cam.ext[2] = cam.ext[3] = 0;
        VqbBuffer bCam = ctx.bufferWith(sizeof(cam), SSBO, &cam, sizeof(cam));
        VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 4; pci.pPoolSizes = ps;
        VkDescriptorPool gpool;
        VQB_CK(vkCreateDescriptorPool(ctx.dev, &pci, nullptr, &gpool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = gpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &gl;
        VkDescriptorSet gs;
        VQB_CK(vkAllocateDescriptorSets(ctx.dev, &ai, &gs));
        VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asw.accelerationStructureCount = 1; asw.pAccelerationStructures = &scene.tlas.as;
        VkDescriptorBufferInfo bi[5] = {{scene.bGbInst.buf, 0, VK_WHOLE_SIZE}, {scene.bGbVerts.buf, 0, VK_WHOLE_SIZE}, {scene.bGbIdx.buf, 0, VK_WHOLE_SIZE},
                                        {scene.bMaterials.buf, 0, VK_WHOLE_SIZE}, {bCam.buf, 0, VK_WHOLE_SIZE}};
        VkDescriptorImageInfo ti[4], oi[5];
        for (int i = 0; i < 4; ++i) ti[i] = {sampLin, scene.textures[i].view, VK_IMAGE_LAYOUT_GENERAL};
        for (int i = 0; i < 5; ++i) oi[i] = {VK_NULL_HANDLE, gb[i].view, VK_IMAGE_LAYOUT_GENERAL};
        std::vector<VkWriteDescriptorSet> w;
        auto wr = [&](uint32_t b, VkDescriptorType t, const VkDescriptorBufferInfo* pb, const VkDescriptorImageInfo* pi, uint32_t n, const void* next)
        {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, next};
            x.dstSet = gs; x.dstBinding = b; x.descriptorType = t; x.descriptorCount = n; x.pBufferInfo = pb; x.pImageInfo = pi;
            w.push_back(x);
        };
        wr(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, 1, &asw);
        wr(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[0], nullptr, 1, nullptr);
        wr(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[1], nullptr, 1, nullptr);
        wr(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[2], nullptr, 1, nullptr);
        wr(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, ti, 4, nullptr);
        wr(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[3], nullptr, 1, nullptr);
        wr(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bi[4], nullptr, 1, nullptr);
        for (uint32_t i = 0; i < 5; ++i) wr(7 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &oi[i], 1, nullptr);
        vkUpdateDescriptorSets(ctx.dev, (uint32_t)w.size(), w.data(), 0, nullptr);
        ctx.oneShot([&](VkCommandBuffer c)
        {
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, gp);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, gpl, 0, 1, &gs, 0, nullptr);
            vkCmdDispatch(c, (RW + 7) / 8, (RH + 7) / 8, 1);
        });
        // coverage (the receivers the consumers have)
        std::vector<uint16_t> g2((size_t)RW * RH * 4);
        VqbBuffer rb = ctx.buffer(g2.size() * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        ctx.oneShot([&](VkCommandBuffer c)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(c, &d);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {RW, RH, 1};
            vkCmdCopyImageToBuffer(c, gb[2].img, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &r);
        });
        std::memcpy(g2.data(), rb.mapped, g2.size() * 2);
        uint64_t surf = 0;
        for (size_t i = 0; i < (size_t)RW * RH; ++i) surf += g2[i * 4 + 3] != 0;
        std::printf("G-buffer: %.1f %% of the pixels hold a surface\n", 100.0 * surf / ((double)RW * RH));
    }

    // ======================= the frame =======================
    VkQueryPool qp;
    const uint32_t kMaxMarks = 96, kBatch = 8;   // (8 frames per submit: the GPU never idles between frames -- stable clocks)
    uint32_t qBase = 0;
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kMaxMarks * kBatch;
        VQB_CK(vkCreateQueryPool(ctx.dev, &qi, nullptr, &qp));
    }
    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = ctx.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VQB_CK(vkAllocateCommandBuffers(ctx.dev, &ai, &cmd));
    }
    VkFence fence;
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VQB_CK(vkCreateFence(ctx.dev, &fi, nullptr, &fence));
    }
    uint32_t frameNo = 0;
    std::vector<std::string> marks;
    bool curSer = false;   // (perf-4k60: config bit 65536 = this config serialized; 32768 = a no-op label for A/A)
    auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = src; m.srcAccessMask = sa; m.dstStageMask = dst; m.dstAccessMask = da;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    // the chains' RAW (recordGI / recordProjStoch raw(indirect))
    auto raw = [&](bool indirect)
    {
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | (indirect ? VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT : 0),
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | (indirect ? VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT : 0));
    };
    auto mark = [&](const std::string& nm)
    {
        if (marks.size() + 1 >= kMaxMarks) return;
        if ((opt.serialize || curSer) && nm != "prep")
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        marks.push_back(nm);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, qBase + (uint32_t)marks.size());
    };
    auto push = [&](const RtReflPush& p) { vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p); };
    auto bind = [&](K k) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ks[k].pipe); };
    auto grid = [&](K k) { bind(k); vkCmdDispatch(cmd, (DW + 7) / 8, (DH + 7) / 8, 1); };
    auto ind = [&](K k, const VqbBuffer& b, VkDeviceSize off) { bind(k); vkCmdDispatchIndirect(cmd, b.buf, off); };
    auto setAddr = [](uint32_t* w2, VkDeviceAddress a) { w2[0] = (uint32_t)(a & 0xFFFFFFFFu); w2[1] = (uint32_t)(a >> 32); };
    // the GI region's header (visQueueHdrWrite): (0, 0, 0, 1), the capacities, word 2 0, the counters zeroed, word 5
    // the producer policy
    auto regHdr = [&](uint32_t reqPer, uint32_t slots, uint32_t budgetK, const uint32_t pol[2])
    {
        const uint32_t hdr[24] = {0u, 0u, 0u, 1u, N * reqPer, N, slots, budgetK, 0u, 0u, 0u, 0u,
                                  0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, pol[0], pol[1], 0u, 0u};
        vkCmdUpdateBuffer(cmd, bVq.buf, 0, sizeof(hdr), hdr);
    };

    // THE VIEWER'S CHAIN (recordDeferLight with a queued consumer: the merged pass inline, then the frame walk's
    // levels over the GI's producers, ONE walk wave, the GI's HIT-LIGHT head, then REUSE and its tail)
    auto record = [&](uint32_t mask)
    {
        const bool m2 = (mask & 64u) == 0u;   // (64: the GI hits on M1)
        curSer = (mask & 65536u) != 0u;
        const uint32_t frame = frameNo & 0xFFFFFu, parity = frameNo & 1u;
        marks.clear();
        vkCmdResetQueryPool(cmd, qp, qBase, kMaxMarks);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, qBase);
        const VkDescriptorSet* ds = sets.data();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, nSets, ds, 0, nullptr);

        // ---- the frame's resets + headers (THE LIGHT PREP's list headers, the GI header upload, pdn_vis, the region) ----
        barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (opt.gi)
        {
            const uint32_t lists[8] = {0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u};
            vkCmdUpdateBuffer(cmd, bGiHit.buf, 16, sizeof(lists), lists);
            // (bit 1024: #985 RenderVKGIReuseVisSkip)
            const std::vector<uint8_t> h = giHeader(frame, parity, frameNo > 0, (mask & 1024u) != 0u);
            vkCmdUpdateBuffer(cmd, bGiState.buf, 0, h.size(), h.data());
            // (the GI producer policy, header word 5: bits 512 = the min share, 256 = force-opaque sun rays)
            uint32_t pol[2] = {0u, (mask & 256u) != 0u ? 1u : 0u};
            const float ms = (mask & 512u) != 0u ? opt.giMinShare : 0.f;
            std::memcpy(&pol[0], &ms, sizeof(ms));
            regHdr(m2 ? 1u + K2 : 1u, m2 ? 2u + K2 : 1u, m2 ? K2 : 0u, pol);
        }
        if (opt.shadow)
        {
            const uint32_t vh[4] = {0u, 0u, 1u, 1u};
            vkCmdUpdateBuffer(cmd, bPdnVis.buf, 0, sizeof(vh), vh);
        }
        if (opt.refl)
        {
            // (perf-4k60) recordRtMarkInit's header at the defaults: (0, 0, 1, n), the receiver distance 48 m, sharp
            // 0.10, no budget, floor 1; rho_min 1/16, curve 1, cutoff 0.6; strength full 0.25 / min 0.03 / cut 0.02
            uint32_t h[68] = {};
            h[2] = 1u; h[3] = nRtTiles;
            const float f16[4] = {48.f, 0.10f, 0.f, 0.f};
            std::memcpy(&h[16], &f16[0], 8); h[18] = (uint32_t)(0.25 * (double)RW * (double)RH); h[19] = 1u;   // (RenderVKRTReflectionsRayBudget 0.25)
            const float p2[3] = {1.f / 16.f, 1.f, 0.6f};
            std::memcpy(&h[21], p2, 12);
            const float p3[3] = {0.25f, 0.03f, 0.02f};
            std::memcpy(&h[53], p3, 12);
            vkCmdUpdateBuffer(cmd, bRtTiles.buf, 0, sizeof(h), h);
            vkCmdFillBuffer(cmd, bRtTiles.buf, sizeof(h), VK_WHOLE_SIZE, 0u);
            // the ray list's header at word 2n: (count 0, rays 0, dispatch (0, 0, 1), capacity 64 n)
            const uint32_t rayl[6] = {0u, 0u, 0u, 0u, 1u, nRtTiles * 64u};
            vkCmdUpdateBuffer(cmd, bRtTiles.buf, sizeof(h) + (VkDeviceSize)nRtTiles * 2u * 4u, sizeof(rayl), rayl);
        }
        barrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        mark("prep");
        if (opt.refl)
        {
            // (perf-4k60) THE REFLECT PAIR (recordDeferLight reflectStep, RenderVKDeferReflectHalfRes 1): the half-
            // resolution blocks, then the sharp pixels at full resolution (+ the RT mark / tile vote); the push is the
            // split lighting's: DEFER_FLAG_REFL_HALF | the sharp roughness byte | DEFER_FLAG_RT_MARK | the RT cutoff byte
            RtReflPush rp{};
            rp.d.view = 0; rp.d.width = RW; rp.d.height = RH;
            std::memcpy(rp.d.inv_vp_rel, scene.invVpRel, 64);
            rp.d.flags = 4u | ((uint32_t)(0.6f * 255.f + 0.5f) << 8) | (1u << 16) | ((uint32_t)(0.6f * 255.f + 0.5f) << 24);
            if (const char* ef = std::getenv("VQB_REFL_FLAGS")) rp.d.flags = (uint32_t)std::strtoul(ef, nullptr, 0);   // (experiments)
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
            bind(K_REFL_HALF); vkCmdDispatch(cmd, (DW + 7) / 8, (DH + 7) / 8, 1); mark("c_defer_refl_half");
            bind(K_REFL_SHARP); vkCmdDispatch(cmd, (RW + 7) / 8, (RH + 7) / 8, 1); mark("c_defer_refl_sharp");
            if (ks[K_REFL_VOTE].pipe != VK_NULL_HANDLE)
            {
                raw(false);
                bind(K_REFL_VOTE); vkCmdDispatch(cmd, (RW + 7) / 8, (RH + 7) / 8, 1); mark("c_defer_refl_vote");
            }
            if (ks[K_RT_SEL].pipe != VK_NULL_HANDLE && ks[K_RT_LIST].pipe != VK_NULL_HANDLE)
            {
                // (recordRtReflect, ray list on, split off) SELECT over the listed tiles -> barrier -> the fused list
                // TRACE over the ray list; the push: rtReflPush at the owner's settings (max distance 32 m, blend
                // layers, 1 hit shadow ray, HW cull, back-face stop), the receiver gate 48 m, anisotropy 16
                raw(true);
                RtReflPush tp = rp;
                tp.rtr[0] = 32.f; tp.rtr[1] = (float)(1u | (1u << 1) | 32u | 16384u); tp.rtr[2] = 48.f; tp.rtr[3] = 16.f;
                vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
                bind(K_RT_SEL); vkCmdDispatchIndirect(cmd, bRtTiles.buf, 0); mark("c_defer_rtrefl_sel");
                raw(true);
                bind(K_RT_LIST); vkCmdDispatchIndirect(cmd, bRtTiles.buf, 68u * 4u + ((VkDeviceSize)nRtTiles * 2u + 2u) * 4u);
                mark("c_defer_rtrefl_trace");
            }
            raw(true);
        }

        // ---- the pushes ----
        RtReflPush base{};
        base.d.view = 0;
        base.d.flags = DEFER_FLAG_PSTOCH | DEFER_FLAG_PSTOCH_HALF;
        base.d.width = RW; base.d.height = RH;
        std::memcpy(base.d.inv_vp_rel, scene.invVpRel, 64);
        // GI (recordGI): rtr = (ray length, opts: K << 1 -- no light grid --, 0, anisotropy), rtr2 = (D, frame, parity)
        RtReflPush gp = base;
        gp.rtr[0] = 64.f; gp.rtr[1] = (float)(K2 << 1); gp.rtr[2] = 0.f; gp.rtr[3] = 16.f;
        gp.rtr2[0] = (float)DW; gp.rtr2[1] = (float)DH; gp.rtr2[2] = (float)frame; gp.rtr2[3] = (float)parity;
        // the shadow core (recordProjStoch): rtr = (D, frame, K | parity << 4), rtr2 = (sun weight, slope bias,
        // PDN_OPT bits merged: BLEED | FACING | SUN_AWAY | OFIRST... at the defaults 41, ReSTIR reservoirs 0),
        // rtr3 = (no certificates, splitK 0, the visibility slots)
        RtReflPush sp = base;
        sp.rtr[0] = (float)DW; sp.rtr[1] = (float)DH; sp.rtr[2] = (float)frame; sp.rtr[3] = (float)(1u | (parity << 4));
        sp.rtr2[0] = 1.f; sp.rtr2[1] = 0.25f; sp.rtr2[2] = 41.f; sp.rtr2[3] = 0.f;
        sp.rtr3[0] = 0u; sp.rtr3[1] = 0u; sp.rtr3[2] = 0u; sp.rtr3[3] = S;
        RtReflPush gv = gp;            // (the GI hit region in rtr3.zw)
        setAddr(&gv.rtr3[2], bVq.addr);
        RtReflPush gw = gp;            // (visQueueWalk: rtr3 = (0, 0, the region))
        gw.rtr3[0] = 0u; gw.rtr3[1] = 0u;
        setAddr(&gw.rtr3[2], bVq.addr);

        // ---- (perf-4k60, bit 16384) LOCKSTEP INTERLEAVE: the shadow core and the GI chain share no data, so each
        //      shadow stage runs inside a GI stage's barrier window -- the same barriers, no new ones, both chains'
        //      kernels side by side on the SMs (different register footprints fill each other's idle warps) ----
        if ((mask & 16384u) != 0u && opt.shadow && opt.gi)
        {
            RtReflPush gtp = gp; gtp.rtr[2] = 1.f;
            auto level = [&]()
            {
                barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                        | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
            };
            // window 1: shadow SELECT || GI TRACE
            push(sp); grid(K_SH_SELECT); mark("c_defer_shadowsel");
            push(gtp); grid(K_GI_TRACE); mark("c_gi_trace");
            level();
            // window 2: shadow SUNVIS + VIS || GI MISS + HIT-MAT
            push(sp);
            grid(opt.isoLod ? K_SH_SUNVIS_ISO : K_SH_SUNVIS); mark("c_defer_shadowsun");
            ind(opt.isoLod ? K_SH_VIS_ISO : K_SH_VIS, bPdnVis, 4u); mark("c_defer_shadowvis");
            push(gp); ind(K_GI_MISS, bGiHit, 2u * 16u + 4u); mark("c_gi_miss");
            const bool m2i = (mask & 64u) == 0u;
            push(gv); ind(m2i ? K_GI_HITMAT : K_GI_HITMAT_VQ, bGiHit, 1u * 16u + 4u); mark("c_gi_hitmat");
            level();
            // window 3: shadow SHADE || GI HIT-REQ
            push(sp); grid(K_SH_SHADE); mark("c_defer_shadowmerge");
            if (m2i) { push(gv); ind(K_GI_HITREQ_VQ2, bGiHit, 1u * 16u + 4u); mark("c_gi_hitreq"); }
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
            mark("c_vq_prod");
            push(gw); ind(K_VIS_WALK, bVq, 4u); mark("c_gi_walk");
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            push(gv); ind(m2i ? K_GI_HITLIGHT_VQ2 : K_GI_HITLIGHT_VQ, bGiHit, 1u * 16u + 4u); mark("c_gi_hitlight");
            raw(false);
            push(gp);
            grid(K_GI_REUSE); mark("c_gi_reuse");
            raw(true);
            ind(K_GI_RESHADE, bGiHit, 4u); mark("c_gi_reshade");
            raw(true);
            RtReflPush tp = gp; tp.rtr[2] = 1.f;
            push(tp); ind(K_GI_REUSE, bGiHit, 4u); mark("c_gi_tail");
            return;
        }
        // ---- the merged shadow pass (recordShadowMergeFirst: recorded whole, ahead of the frame walk) ----
        if (opt.shadow)
        {
            push(sp);
            grid(K_SH_SELECT); mark("c_defer_shadowsel");
            raw(true);
            grid(opt.isoLod ? K_SH_SUNVIS_ISO : K_SH_SUNVIS); mark("c_defer_shadowsun");
            ind(opt.isoLod ? K_SH_VIS_ISO : K_SH_VIS, bPdnVis, 4u); mark("c_defer_shadowvis");
            push(sp); raw(false);
            grid(K_SH_SHADE); mark("c_defer_shadowmerge");
        }
        if (!opt.gi) return;
        // ---- THE LEVELS (visQueueFrameWalk): recordGI's producer stages {TRACE, MISS + HIT-MAT, [HIT-REQ (M2)]},
        //      ONE barrier between two (the union of the consumers' own: compute + indirect + the #1128 copy) ----
        if (opt.shadow) raw(false);   // (the merged pass ends at SHADE: recordShadowMergeFirst's barrier)
        RtReflPush gtp = gp;   // (TRACE: push rtr.z = 1, the HIT / MISS lists)
        gtp.rtr[2] = 1.f;
        auto level = [&]()
        {
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        };
        push(gtp); grid(K_GI_TRACE); mark("c_gi_trace");
        level();
        push(gp); ind(K_GI_MISS, bGiHit, 2u * 16u + 4u); mark("c_gi_miss");
        // (M2: the fused stage's HIT-MAT -- the viewer binds it with the queue's push)
        push(gv); ind(m2 ? K_GI_HITMAT : K_GI_HITMAT_VQ, bGiHit, 1u * 16u + 4u); mark("c_gi_hitmat");
        if (m2) { level(); push(gv); ind(K_GI_HITREQ_VQ2, bGiHit, 1u * 16u + 4u); mark("c_gi_hitreq"); }
        // ---- THE FRAME WALK: one barrier, the walk, one barrier, the head (HIT-LIGHT) ----
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        mark("c_vq_prod");
        push(gw); ind(K_VIS_WALK, bVq, 4u); mark("c_gi_walk");
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        push(gv); ind(m2 ? K_GI_HITLIGHT_VQ2 : K_GI_HITLIGHT_VQ, bGiHit, 1u * 16u + 4u); mark("c_gi_hitlight");
        // ---- the GI REUSE (recordGISplitReuse): REUSE -> RESHADE -> TAIL ----
        raw(false);
        push(gp);
        grid(K_GI_REUSE); mark("c_gi_reuse");
        raw(true);
        ind(K_GI_RESHADE, bGiHit, 4u); mark("c_gi_reshade");
        raw(true);
        RtReflPush tp = gp;
        tp.rtr[2] = 1.f;
        push(tp); ind(K_GI_REUSE, bGiHit, 4u); mark("c_gi_tail");
    };

    // `count` frames (<= kBatch) in one submit; out: each frame's marks (ms)
    using Marks = std::vector<std::pair<std::string, double>>;
    auto runBatch = [&](uint32_t mask, uint32_t count, std::vector<Marks>* out)
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VQB_CK(vkBeginCommandBuffer(cmd, &bi));
        std::vector<std::vector<std::string>> names(count);
        for (uint32_t f = 0; f < count; ++f)
        {
            qBase = f * kMaxMarks;
            record(mask);
            names[f] = marks;
            ++frameNo;
        }
        VQB_CK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VQB_CK(vkQueueSubmit(ctx.queue, 1, &si, fence));
        VQB_CK(vkWaitForFences(ctx.dev, 1, &fence, VK_TRUE, ~0ull));
        VQB_CK(vkResetFences(ctx.dev, 1, &fence));
        if (!out) return;
        out->assign(count, Marks{});
        for (uint32_t f = 0; f < count; ++f)
        {
            std::vector<uint64_t> ts(names[f].size() + 1);
            VQB_CK(vkGetQueryPoolResults(ctx.dev, qp, f * kMaxMarks, (uint32_t)ts.size(), ts.size() * 8, ts.data(), 8,
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            for (size_t i = 0; i < names[f].size(); ++i) (*out)[f].push_back({names[f][i], (ts[i + 1] - ts[i]) * ctx.tsPeriodNs * 1e-6});
        }
    };
    auto runFrame = [&](uint32_t mask) { runBatch(mask, 1, nullptr); };
    auto resetState = [&]()
    {
        ctx.oneShot([&](VkCommandBuffer c)
        {
            for (const VqbBuffer& b : stateBufs) vkCmdFillBuffer(c, b.buf, 0, VK_WHOLE_SIZE, 0u);
            VkClearColorValue z{};
            for (const VqbImage& im : stateImgs)
            {
                VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, im.mips, 0, im.layers};
                vkCmdClearColorImage(c, im.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
            }
        });
        frameNo = 0;
    };

    // ======================= sanity + VERIFY (fused vs queued, the same frames from the same state) =======================
    auto readU32 = [&](const VqbBuffer& b, VkDeviceSize off, uint32_t n)
    {
        std::vector<uint32_t> v(n);
        ctx.download(b, off, v.data(), n * 4);
        return v;
    };
    auto imageRaw = [&](const VqbImage& im)
    {
        const size_t texel = (im.format == VK_FORMAT_R32G32B32A32_UINT) ? 16 : 8;
        const size_t bytes = (size_t)im.w * im.h * im.layers * texel;
        VqbBuffer rb = ctx.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        ctx.oneShot([&](VkCommandBuffer c)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(c, &d);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers};
            r.imageExtent = {im.w, im.h, 1};
            vkCmdCopyImageToBuffer(c, im.img, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &r);
        });
        std::vector<uint16_t> v(bytes / 2);
        std::memcpy(v.data(), rb.mapped, bytes);
        return v;
    };
    struct Fp { uint64_t giRec = 0, giRes = 0, pdnVis = 0, pdnIn = 0, giIn = 0; uint32_t hits = 0, misses = 0, jobs = 0;
                uint32_t req = 0, rays = 0, reused = 0;
                std::vector<uint16_t> pdnInRaw, giInRaw; std::vector<uint32_t> giResRaw, recRaw; std::vector<uint16_t> geomRaw; };
    auto fingerprint = [&](uint32_t mask)
    {
        Fp f;
        if (opt.gi)
        {
            // the per-texel records' q0..q3 (TRACE's ray + the shaded sample: no atomic order in them)
            const uint64_t first = 3ull + (N + 3) / 4;
            std::vector<uint32_t> rec((size_t)N * 24);
            ctx.download(bGiHit, first * 16, rec.data(), rec.size() * 4);
            uint64_t h = 1469598103934665603ull;
            for (uint32_t t = 0; t < N; ++t) h = fnv(&rec[(size_t)t * 24], 64, h);
            f.giRec = h;
            if (std::getenv("VQB_DUMP")) f.recRaw = rec;
            std::vector<uint32_t> res((size_t)N * 16);
            ctx.download(bGiRes, 0, res.data(), res.size() * 4);
            f.giRes = fnv(res.data(), res.size() * 4);
            f.giResRaw = std::move(res);
            const std::vector<uint32_t> hdr = readU32(bGiHit, 0, 12);
            f.hits = hdr[4]; f.misses = hdr[8];
            const std::vector<uint32_t> st = readU32(bGiState, 0, 64);
            Block b; b.ks = &ks; b.set = 3; b.binding = 23;
            const VqbField fs = b.find("gi.stat"), fs2 = b.find("gi.stat2");
            if (fs.ok) f.rays = st[fs.offset / 4];
            if (fs2.ok) f.reused = st[fs2.offset / 4 + 2];
        }
        if (opt.shadow)
        {
            const std::vector<uint32_t> vh = readU32(bPdnVis, 0, 4);
            f.jobs = vh[0];
            std::vector<uint32_t> vis((size_t)N * S);
            ctx.download(bPdnVis, 16ull + 4ull * N * S, vis.data(), vis.size() * 4);
            f.pdnVis = fnv(vis.data(), vis.size() * 4);
            if (!opt.dumpDir.empty())
            {
                std::ofstream file(opt.dumpDir+"/mask-"+std::to_string(mask)+".vis",std::ios::binary);
                file.write(reinterpret_cast<const char*>(vis.data()),vis.size()*4);
            }
        }
        for (auto& kv : madeImg)
        {
            if (kv.first.rfind("uPdnIn#", 0) == 0) { f.pdnInRaw = imageRaw(kv.second); f.pdnIn = fnv(f.pdnInRaw.data(), f.pdnInRaw.size() * 2); }
            if (kv.first.rfind("uGiGeom#", 0) == 0 && std::getenv("VQB_DUMP")) f.geomRaw = imageRaw(kv.second);
            if (kv.first.rfind("uGiIn#", 0) == 0) { f.giInRaw = imageRaw(kv.second); f.giIn = fnv(f.giInRaw.data(), f.giInRaw.size() * 2); }
        }
        if (opt.gi) f.req = readU32(bVq, 0, 1)[0];
        return f;
    };
    if (opt.verify)
    {
        std::printf("\nVERIFY: each config from the zeroed state, frames 0..3 (the GI reuse needs history), then fingerprints\n");
        std::printf("  %-6s %10s %10s %10s %8s %8s %18s %18s %18s %18s %18s  %s\n", "mask", "hits", "misses", "sh jobs", "rays",
                    "rejected", "gi records", "gi reservoirs", "pdn_vis", "uPdnIn", "uGiIn", "queue requests");
        Fp ref{};
        bool first = true;
        std::vector<std::pair<uint32_t, Fp>> all;
        std::vector<uint32_t> vcfg = opt.configs;
        vcfg.push_back(opt.configs[0]);   // (the first config again: the harness's own determinism)
        for (uint32_t mask : vcfg)
        {
            resetState();
            static const int s_vf = std::getenv("VQB_VERIFY_FRAMES") ? std::max(1, std::atoi(std::getenv("VQB_VERIFY_FRAMES"))) : 4;
            for (int f = 0; f < s_vf; ++f) runFrame(mask);
            Fp fp = fingerprint(mask);
            auto same = [&](uint64_t a, uint64_t b) { return first || a == b ? "" : "*"; };
            std::printf("  %-6u %10u %10u %10u %8u %8u %16llx%1s %16llx%1s %16llx%1s %16llx%1s %16llx%1s  %u\n", mask, fp.hits,
                        fp.misses, fp.jobs, fp.rays, fp.reused, (unsigned long long)fp.giRec, same(fp.giRec, ref.giRec),
                        (unsigned long long)fp.giRes, same(fp.giRes, ref.giRes), (unsigned long long)fp.pdnVis, same(fp.pdnVis, ref.pdnVis),
                        (unsigned long long)fp.pdnIn, same(fp.pdnIn, ref.pdnIn), (unsigned long long)fp.giIn, same(fp.giIn, ref.giIn),
                        fp.req);
            if (first) { ref = fp; first = false; }
            all.push_back({mask, std::move(fp)});
        }
        // how far apart: texels of the rgba16f images that differ, the largest difference; reservoir records that differ
        auto h2f = [](uint16_t h) -> float
        {
            const uint32_t sg = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
            float v = e == 0 ? std::ldexp((float)m, -24) : (e == 31 ? 65504.f : std::ldexp((float)(m | 1024u), (int)e - 25));
            return sg ? -v : v;
        };
        auto imgDiff = [&](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, uint64_t& n, float& mx, float& rel)
        {
            n = 0; mx = 0; rel = 0;
            if (a.size() != b.size()) return;
            for (size_t t = 0; t + 3 < a.size(); t += 4)
            {
                bool d = false;
                for (int c = 0; c < 4; ++c)
                    if (a[t + c] != b[t + c])
                    {
                        d = true;
                        const float x = h2f(a[t + c]), y = h2f(b[t + c]), df = std::fabs(x - y);
                        mx = std::max(mx, df);
                        rel = std::max(rel, df / std::max(std::max(std::fabs(x), std::fabs(y)), 1e-3f));
                    }
                n += d;
            }
        };
        std::printf("  differences from mask %u (texels / largest abs / largest rel; reservoir records):\n", opt.configs[0]);
        for (size_t i = 1; i < all.size(); ++i)
        {
            const Fp& fp = all[i].second;
            uint64_t n1, n2;
            float m1, m2, r1, r2;
            imgDiff(ref.pdnInRaw, fp.pdnInRaw, n1, m1, r1);
            imgDiff(ref.giInRaw, fp.giInRaw, n2, m2, r2);
            uint64_t nr = 0;
            for (size_t k = 0; k + 3 < std::min(ref.giResRaw.size(), fp.giResRaw.size()); k += 4)
            {
                const bool dif = std::memcmp(&ref.giResRaw[k], &fp.giResRaw[k], 16) != 0;
                if (dif && std::getenv("VQB_DUMP") && nr < 12)
                {
                    const uint32_t* a = &ref.giResRaw[k]; const uint32_t* b = &fp.giResRaw[k];
                    const size_t rec = k / 4, texel = rec / 2;
                    const size_t tx = texel % ((size_t)DW * DH);
                    std::printf("      res rec %zu (layer %zu texel %zu word %zu): %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                                rec, texel / ((size_t)DW * DH), tx, rec & 1, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                    if (!fp.recRaw.empty())
                    {
                        const uint32_t* q = &fp.recRaw[tx * 24];
                        const uint32_t* q0r = &ref.recRaw[tx * 24];
                        if (!fp.geomRaw.empty() && fp.geomRaw.size() == ref.geomRaw.size())
                        {
                            const size_t per = (size_t)DW * DH * 8;   // (uvec4 = 8 u16 per texel per layer)
                            for (size_t L = 0; L * per < fp.geomRaw.size(); ++L)
                            {
                                const uint16_t* ga = &ref.geomRaw[L * per + tx * 8]; const uint16_t* gb = &fp.geomRaw[L * per + tx * 8];
                                std::printf("        geom L%zu vz %04x%04x | %04x%04x  N %04x%04x | %04x%04x\n", L, ga[1], ga[0], gb[1], gb[0],
                                            ga[3], ga[2], gb[3], gb[2]);
                            }
                        }
                        std::printf("        rec4 q0w %08x  q4 %08x %08x %08x %08x  q5 %08x %08x %08x %08x | ref q4 %08x %08x %08x %08x q5 %08x %08x %08x %08x\n",
                                    q[3], q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23],
                                    q0r[16], q0r[17], q0r[18], q0r[19], q0r[20], q0r[21], q0r[22], q0r[23]);
                    }
                }
                nr += dif;
            }
            std::printf("    %s%-4u uPdnIn %8llu / %.3g / %.3g   uGiIn %8llu / %.3g / %.3g   reservoirs %llu\n",
                        i + 1 == all.size() ? "repeat " : "", all[i].first, (unsigned long long)n1, m1, r1, (unsigned long long)n2, m2, r2,
                        (unsigned long long)nr);
            // where: per image layer and channel (uPdnIn layer 2 = the sun channel; uGiIn the GI samples)
            for (int which = 0; which < 2; ++which)
            {
                const std::vector<uint16_t>& A = which ? ref.giInRaw : ref.pdnInRaw;
                const std::vector<uint16_t>& B = which ? fp.giInRaw : fp.pdnInRaw;
                if (A.size() != B.size() || A.empty()) continue;
                const size_t per = (size_t)DW * DH * 4;
                for (size_t L0 = 0; L0 * per < A.size(); ++L0)
                    for (int c = 0; c < 4; ++c)
                    {
                        uint64_t n = 0;
                        float mx = 0;
                        for (size_t t = L0 * per + c; t < std::min(A.size(), (L0 + 1) * per); t += 4)
                            if (A[t] != B[t]) { ++n; mx = std::max(mx, std::fabs(h2f(A[t]) - h2f(B[t]))); }
                        if (n) std::printf("        %s layer %zu channel %c: %llu texels, largest %.3g\n", which ? "uGiIn " : "uPdnIn",
                                           L0, "rgba"[c], (unsigned long long)n, mx);
                    }
            }
        }
        std::printf("  (* = differs from the first config: 64 must reproduce it bit for bit (the same functions on the same rays);\n"
                    "   256 / 512 / 1024 change the image. A zero hit / job / request count means a gate is closed.)\n");
    }

    // ======================= TIMING =======================
    resetState();
    for (int f = 0; f < 4; ++f) runFrame(opt.configs[0]);   // (history: the reuse's steady state)
    std::map<uint32_t, std::map<std::string, std::vector<double>>> samples;
    std::map<uint32_t, std::vector<std::string>> order2;
    std::ofstream csv;
    if (!opt.csv.empty()) { csv.open(opt.csv); csv << "round,mask,frame,mark,ms\n"; }
    std::vector<Marks> batch;
    for (uint32_t round = 0; round < opt.rounds; ++round)
        for (uint32_t mask : opt.configs)
        {
            for (uint32_t f = 0; f < opt.warmup; f += kBatch) runBatch(mask, std::min(kBatch, opt.warmup - f), nullptr);
            for (uint32_t f0 = 0; f0 < opt.frames; f0 += kBatch)
            for (const Marks& fr : (runBatch(mask, std::min(kBatch, opt.frames - f0), &batch), batch))
            {
                const uint32_t f = f0;
                double gi = 0, sh = 0, all = 0;
                if (order2[mask].empty()) for (auto& m : fr) order2[mask].push_back(m.first);
                for (auto& m : fr)
                {
                    samples[mask][m.first].push_back(m.second);
                    all += m.second;
                    if (m.first.rfind("c_gi", 0) == 0) gi += m.second;
                    else if (m.first.rfind("c_defer_shadow", 0) == 0) sh += m.second;
                    if (csv.is_open()) csv << round << "," << mask << "," << f << "," << m.first << "," << m.second << "\n";
                }
                samples[mask]["GI total"].push_back(gi);
                samples[mask]["shadow total"].push_back(sh);
                samples[mask]["frame total"].push_back(all);
            }
        }
    std::printf("\nTIMING (median ms over %u frames x %u rounds per config; warm-up %u; GPU timestamps after every dispatch)\n",
                opt.frames, opt.rounds, opt.warmup);
    for (uint32_t mask : opt.configs)
    {
        std::printf("  RenderVKVisQueue %u\n", mask);
        std::vector<std::string> names = order2[mask];
        for (const char* t : {"GI total", "shadow total", "frame total"}) names.push_back(t);
        for (const std::string& nm : names)
        {
            double med;
            median(samples[mask][nm], med);
            std::vector<double> v = samples[mask][nm];
            std::sort(v.begin(), v.end());
            const double p10 = v.empty() ? 0 : v[v.size() / 10], p90 = v.empty() ? 0 : v[v.size() * 9 / 10];
            std::printf("    %-24s %8.3f   (p10 %.3f  p90 %.3f)\n", nm.c_str(), med, p10, p90);
        }
    }
    double base0[3] = {0, 0, 0};
    std::printf("\n  %-8s %10s %10s %10s   (delta vs the first config)\n", "mask", "GI", "shadow", "frame");
    for (size_t i = 0; i < opt.configs.size(); ++i)
    {
        const uint32_t mask = opt.configs[i];
        double v[3];
        median(samples[mask]["GI total"], v[0]);
        median(samples[mask]["shadow total"], v[1]);
        median(samples[mask]["frame total"], v[2]);
        if (i == 0) for (int k = 0; k < 3; ++k) base0[k] = v[k];
        std::printf("  %-8u %10.3f %10.3f %10.3f   %+8.3f %+8.3f %+8.3f\n", mask, v[0], v[1], v[2], v[0] - base0[0], v[1] - base0[1], v[2] - base0[2]);
    }
    // (perf-4k60) VQB_REFL_DUMP=FILE: after one more frame from a reset state, the REFLECT pair's outputs as raw
    // bytes -- the RT tile buffer (header + flags + list), then uReflRtMark, uReflSpec, uReflIrr, uReflEnv, uReflHalf --
    // for a byte comparison between two shader sets (scratchpad perf4k60/reflcmp.py)
    if (const char* df = std::getenv("VQB_REFL_DUMP"); df && opt.refl)
    {
        resetState();
        for (int f = 0; f < 3; ++f) runFrame(opt.configs[0]);
        std::ofstream o(df, std::ios::binary);
        auto put = [&](const void* p, size_t n) { const uint64_t nn = n; o.write((const char*)&nn, 8); o.write((const char*)p, (std::streamsize)n); };
        {
            std::vector<uint8_t> v(bRtTiles.size);
            ctx.download(bRtTiles, 0, v.data(), v.size());
            put(v.data(), v.size());
        }
        for (const char* nm : {"uReflRtMark", "uReflSpec", "uReflIrr", "uReflEnv", "uReflHalf"})
            for (auto& kv : madeImg)
                if (kv.first.rfind(std::string(nm) + "#", 0) == 0)
                {
                    const VqbImage& im = kv.second;
                    const size_t texel = im.format == VK_FORMAT_R32G32B32A32_UINT ? 16 : 4;
                    const size_t bytes = (size_t)im.w * im.h * im.layers * texel;
                    VqbBuffer rb = ctx.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                    ctx.oneShot([&](VkCommandBuffer c)
                    {
                        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                        m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
                        vkCmdPipelineBarrier2(c, &d);
                        VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers}; r.imageExtent = {im.w, im.h, 1};
                        vkCmdCopyImageToBuffer(c, im.img, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &r);
                    });
                    put(rb.mapped, bytes);
                    std::printf("refl dump: %s %ux%u x %zu B\n", nm, im.w, im.h, texel);
                }
        std::printf("refl dump -> %s (tiles %u)\n", df, nRtTiles);
    }
    ctx.shutdown();
    return 0;
}
