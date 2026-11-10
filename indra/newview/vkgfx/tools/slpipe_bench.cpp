/**
 * @file slpipe_bench.cpp
 * @brief #1411 PIPELINE COMPILE BENCH (dev tool; target slpipe-bench, never packaged).
 *
 * Creates the viewer's Vulkan device (VkDeviceContext, the same feature chain) headless, loads
 * the viewer's SPIR-V (one <symbol>.spv file per embedded array, extracted from the generated
 * vk_*_shaders_spv.h), builds layouts / vertex input / formats by reflection (compatible, not the
 * renderer's exact ones) and compiles through the REAL scheduler (vk_pipeline.cpp). Modes:
 *
 *   one   <spec>...            each pipeline alone, cold (fresh VkPipelineCache), ms + RSS growth
 *   set   <list file>          a recipe list through the scheduler, by phase policy (see below)
 *   cache <list file>          cache save -> simulated version bump -> load -> warm compile
 *   worker ...                 the P4 out-of-process worker mode (vk_pipeworker.h)
 *
 * COLD numbers need the driver's own disk cache off: run with __GL_SHADER_DISK_CACHE=0 (NVIDIA)
 * or MESA_SHADER_CACHE_DISABLE=true (Mesa) in THIS process's environment only
 * (scripts: scratchpad runner). A pipeline spec on the command line / in a list:
 *   name=<label> vert=<sym> frag=<sym> | comp=<sym>  [spec=none|nodebug|floor] [blend=1]
 *   [noopt=1] [class=t0|gate|late|deferred] [rigged=1]
 */
#include "linden_common.h"

#include "vk_device.h"
#include "vk_pipeline.h"
#include "vk_pipecache.h"
#include "vk_pipeworker.h"
#include "vk_spvreflect.h"

#include "llmemory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if LL_LINUX
#include <sys/resource.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
uint64_t rss() { return LLMemory::getCurrentRSS(); }
constexpr uint64_t MB = 1ull << 20;

std::string gSpvDir;
VkDeviceContext gDev;
uint32_t gPushMax = 128;
std::vector<std::string> gLastEvents;   // (selftest: the scheduler event lines; bounded)
VkPipelineBatchReport gLastReport;      // (selftest: the last batch report)

std::string utcNow()
{
    char b[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return b;
}

struct Module
{
    std::vector<uint32_t> words;
    SpvModuleInfo info;
    VkShaderModule handle = VK_NULL_HANDLE;
};
std::map<std::string, std::unique_ptr<Module>> gModules;

Module* module(const std::string& sym)
{
    if (sym.empty()) return nullptr;
    auto it = gModules.find(sym);
    if (it != gModules.end()) return it->second.get();
    const std::string path = gSpvDir + "/" + sym + ".spv";
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "bench: no module %s\n", path.c_str()); return nullptr; }
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::unique_ptr<Module> m(new Module);
    m->words.resize(bytes.size() / 4);
    std::memcpy(m->words.data(), bytes.data(), m->words.size() * 4);
    m->info = spvReflect(m->words.data(), m->words.size());
    if (!m->info.ok) { std::fprintf(stderr, "bench: %s: %s\n", sym.c_str(), m->info.error.c_str()); return nullptr; }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = m->words.size() * 4;
    ci.pCode = m->words.data();
    if (vkCreateShaderModule(gDev.device(), &ci, nullptr, &m->handle) != VK_SUCCESS) return nullptr;
    vkNoteShaderModule(m->handle, m->words.data(), ci.codeSize, sym.c_str());
    Module* r = m.get();
    gModules[sym] = std::move(m);
    return r;
}

// One pipeline of a recipe list.
struct Spec
{
    std::string name, vert, frag, comp, spec = "nodebug", cls = "gate";
    bool blend = false, noopt = false;
};

bool parseSpec(const std::string& line, Spec& s)
{
    std::istringstream in(line);
    std::string tok;
    bool any = false;
    while (in >> tok)
    {
        if (tok[0] == '#') break;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        any = true;
        if (k == "name") s.name = v;
        else if (k == "vert") s.vert = v;
        else if (k == "frag") s.frag = v;
        else if (k == "comp") s.comp = v;
        else if (k == "spec") s.spec = v;
        else if (k == "class") s.cls = v;
        else if (k == "blend") s.blend = v == "1";
        else if (k == "noopt") s.noopt = v == "1";
    }
    if (s.name.empty()) s.name = !s.comp.empty() ? s.comp : s.frag;
    return any;
}

// Specialization data by mode (constant ids from clean_frame_info.glsl / kSpecTable):
//   nodebug: kDebugViews (1) = false -- what the renderer's generic pipelines use
//   floor:   + every setting constant (8..19) at its minimum, sky mode (18) = 2 (run time)
//   none:    no specialization (the module's defaults)
struct SpecData
{
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<int32_t> data;
    VkSpecializationInfo info{};
};
void fillSpec(const std::string& mode, const SpvModuleInfo& mi, SpecData& sd)
{
    auto add = [&](uint32_t id, int32_t v) {
        if (std::find(mi.specIds.begin(), mi.specIds.end(), id) == mi.specIds.end()) return;
        sd.entries.push_back({id, (uint32_t)(sd.data.size() * 4), 4});
        sd.data.push_back(v);
    };
    if (mode == "none") return;
    add(1, 0);
    if (mode == "floor")
        for (uint32_t id = 8; id <= 19; ++id) add(id, id == 18 ? 2 : 0);
    sd.info.mapEntryCount = (uint32_t)sd.entries.size();
    sd.info.pMapEntries = sd.entries.data();
    sd.info.dataSize = sd.data.size() * 4;
    sd.info.pData = sd.data.data();
}

// Layout cache by binding signature.
std::map<std::string, VkPipelineLayout> gLayouts;
VkPipelineLayout layoutFor(const std::vector<const SpvModuleInfo*>& mods, VkShaderStageFlags stages)
{
    std::map<std::pair<uint32_t, uint32_t>, SpvBinding> all;
    bool push = false;
    for (const SpvModuleInfo* m : mods)
    {
        if (!m) continue;
        push |= m->pushConstants;
        for (const SpvBinding& b : m->bindings)
        {
            auto& e = all[{b.set, b.binding}];
            if (e.type == VK_DESCRIPTOR_TYPE_MAX_ENUM) e = b;
            else e.count = std::max(e.count, b.count);
        }
    }
    std::string key = std::to_string(stages) + (push ? "p" : "");
    uint32_t maxSet = 0;
    for (const auto& kv : all)
    {
        key += "|" + std::to_string(kv.first.first) + "." + std::to_string(kv.first.second) + ":" +
               std::to_string(kv.second.type) + "x" + std::to_string(kv.second.count);
        maxSet = std::max(maxSet, kv.first.first + 1);
    }
    auto it = gLayouts.find(key);
    if (it != gLayouts.end()) return it->second;
    std::vector<VkDescriptorSetLayout> sets(maxSet);
    for (uint32_t s = 0; s < maxSet; ++s)
    {
        std::vector<VkDescriptorSetLayoutBinding> bs;
        std::vector<VkDescriptorBindingFlags> flags;
        bool uab = false;
        for (const auto& kv : all)
        {
            if (kv.first.first != s) continue;
            VkDescriptorSetLayoutBinding b{};
            b.binding = kv.first.second;
            b.descriptorType = kv.second.type;
            b.descriptorCount = kv.second.count;
            b.stageFlags = stages;
            bs.push_back(b);
            VkDescriptorBindingFlags f = 0;
            if (kv.second.runtimeArray)
            {
                f = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                uab = true;
            }
            flags.push_back(f);
        }
        if (uab)   // every binding of an UPDATE_AFTER_BIND layout must allow it (types that can)
            for (size_t k = 0; k < bs.size(); ++k)
                if (bs[k].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
                    bs[k].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC &&
                    bs[k].descriptorType != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                    flags[k] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = (uint32_t)flags.size();
        bf.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &bf;
        ci.flags = uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        ci.bindingCount = (uint32_t)bs.size();
        ci.pBindings = bs.data();
        if (vkCreateDescriptorSetLayout(gDev.device(), &ci, nullptr, &sets[s]) != VK_SUCCESS)
        {
            std::fprintf(stderr, "bench: set layout %u failed\n", s);
            return VK_NULL_HANDLE;
        }
        vkPipelineNoteSetLayout(sets[s], &ci);
    }
    VkPushConstantRange pr{stages, 0, gPushMax};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = maxSet;
    li.pSetLayouts = sets.data();
    li.pushConstantRangeCount = push ? 1 : 0;
    li.pPushConstantRanges = &pr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(gDev.device(), &li, nullptr, &l) != VK_SUCCESS) l = VK_NULL_HANDLE;
    else vkPipelineNotePipelineLayout(l, &li);
    gLayouts[key] = l;
    return l;
}

// A built pipeline request (desc / compute) the scheduler can queue.
struct Built
{
    Spec spec;
    bool compute = false;
    GraphicsPipelineDesc desc;
    VkShaderModule comp = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::unique_ptr<SpecData> sd{new SpecData};
    VkPipeline out = VK_NULL_HANDLE;
    uint32_t words = 0;
};

bool build(const Spec& s, Built& b)
{
    b.spec = s;
    if (!s.comp.empty())
    {
        Module* c = module(s.comp);
        if (!c) return false;
        b.compute = true;
        b.comp = c->handle;
        b.words = (uint32_t)c->words.size();
        b.layout = layoutFor({&c->info}, VK_SHADER_STAGE_COMPUTE_BIT);
        fillSpec(s.spec, c->info, *b.sd);
        return b.layout != VK_NULL_HANDLE;
    }
    Module* v = module(s.vert);
    Module* f = s.frag.empty() ? nullptr : module(s.frag);
    if (!v || (!s.frag.empty() && !f)) return false;
    b.words = (uint32_t)(v->words.size() + (f ? f->words.size() : 0));
    GraphicsPipelineDesc& d = b.desc;
    d.vert = v->handle;
    d.frag = f ? f->handle : VK_NULL_HANDLE;
    d.layout = b.layout = layoutFor({&v->info, f ? &f->info : nullptr},
                                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    uint32_t off = 0;
    for (const SpvAttr& a : v->info.inputs)
    {
        d.attributes.push_back({a.location, 0, a.format, off});
        off += a.bytes;
    }
    if (!d.attributes.empty()) d.bindings.push_back({0, std::max(off, 4u), VK_VERTEX_INPUT_RATE_VERTEX});
    d.depthFormat = VK_FORMAT_D32_SFLOAT;
    d.depthTest = !s.blend;
    d.depthWrite = !s.blend;
    d.dynamicDepthCull = true;
    if (!f) d.noColor = true;
    else
    {
        const std::vector<SpvAttr>& o = f->info.outputs;
        if (s.blend && o.size() <= 3)
        {
            d.blend = true;
            d.colorFormat = o.size() > 0 ? o[0].format : VK_FORMAT_R16G16B16A16_SFLOAT;
            if (o.size() > 1) d.glowFormat = o[1].format;
            if (o.size() > 2) { d.reactFormat = VK_FORMAT_R8G8B8A8_UNORM; d.reactWriteMask = 0xF; }
        }
        else if (o.size() == 1) d.colorFormat = o[0].format;
        else if (!o.empty())
        {
            d.mrtCount = (uint32_t)std::min<size_t>(o.size(), GraphicsPipelineDesc::kMaxMrt);
            for (uint32_t k = 0; k < d.mrtCount; ++k) d.mrtFormats[k] = o[k].format;
        }
        fillSpec(s.spec, f->info, *b.sd);
        if (b.sd->info.mapEntryCount) d.fragSpec = &b.sd->info;
    }
    return b.layout != VK_NULL_HANDLE;
}

// RSS sampler: peak RSS while `on`.
struct RssPeak
{
    std::atomic<bool> on{true};
    std::atomic<uint64_t> peak{0};
    std::thread t;
    RssPeak()
    {
        peak = rss();
        t = std::thread([this] {
            while (on) { peak = std::max<uint64_t>(peak, rss()); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        });
    }
    uint64_t stop() { on = false; t.join(); return peak; }
};

// ---- mode: one ----
int modeOne(const std::vector<std::string>& specs, int repeat)
{
    std::printf("# one: %zu pipeline(s), each in a fresh VkPipelineCache; repeat %d (2nd = same cache)\n", specs.size(), repeat);
    std::printf("%-44s %8s %9s %9s %8s %s\n", "name", "words", "cold_ms", "warm_ms", "rss+MB", "spec/flags");
    for (const std::string& line : specs)
    {
        Spec s;
        if (!parseSpec(line, s)) continue;
        Built b;
        if (!build(s, b)) { std::printf("%-44s build failed\n", s.name.c_str()); continue; }
        VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache cache = VK_NULL_HANDLE;
        vkCreatePipelineCache(gDev.device(), &pci, nullptr, &cache);
        double ms[2] = {0, 0};
        uint64_t grow = 0;
        for (int r = 0; r < std::max(1, std::min(repeat, 2)); ++r)
        {
            const uint64_t r0 = rss();
            RssPeak pk;
            const auto t0 = Clock::now();
            VkPipeline p = vkBenchCreateOne(gDev.device(), b.compute, b.desc, b.comp, b.layout,
                                            b.sd->info.mapEntryCount ? &b.sd->info : nullptr, cache, s.noopt);
            ms[r] = msSince(t0);
            const uint64_t peak = pk.stop();
            if (r == 0) grow = peak > r0 ? peak - r0 : 0;
            if (p && r == 0 && vkPipelineStatsCapture()) vkLogPipelineStats(gDev.device(), p, s.name.c_str());
            if (p) vkDestroyPipeline(gDev.device(), p, nullptr);
            else std::printf("  (create failed)\n");
        }
        vkDestroyPipelineCache(gDev.device(), cache, nullptr);
        std::printf("%-44s %8u %9.1f %9.1f %8llu %s%s%s\n", s.name.c_str(), b.words, ms[0], ms[1],
                    (unsigned long long)(grow / MB), s.spec.c_str(), s.noopt ? " noopt" : "", s.blend ? " blend" : "");
        std::fflush(stdout);
    }
    return 0;
}

// ---- mode: set ----
// The list through the real scheduler under a phase policy, reporting time-to-world (the gate
// job: what the login waits for) and time-to-full-quality (+ the upgrade job under p2):
//   p0 = one lane (the pre-#1411 admission), everything in the gate
//   p1 = two lanes, everything in the gate
//   p2 = two lanes + the #1411 upgrade group: the heavy lit / lighting modules compile behind
//        floor-specialized placeholders (graphics lit FS, deferred uber kernel) or none (probe
//        capture FS, split / feature lighting kernels)
struct SetOpts
{
    std::string policy = "p2";
    int floorTopN = -1;  // >= 0: also merge the floor setting constants (kSpecTopN = this) into the placeholders
    std::string heavyExtra;   // comma list of extra heavy modules (no placeholder)
    bool afterGate = false;   // the upgrade job starts only once the gate (+ first-use) jobs are done (the viewer's rule)
};

// The renderer's placeholder table (llcleanrenderervk.cpp: the CLEAN_BASIC / CLEAN_DEFER_BASIC modules).
std::string placeholderOf(const std::string& m)
{
    if (m == "kWorldFragSpv") return "kWorldFragBasicSpv";
    if (m == "kWorldFragBlendSpv") return "kWorldFragBlendBasicSpv";
    if (m == "kWorldDeferredLightSpv") return "kWorldDeferBasicSpv";
    if (m == "kWorldDeferredLightG11Spv") return "kWorldDeferBasicG11Spv";
    return std::string();
}
bool isHeavyNoPlaceholder(const std::string& m)
{
    static const char* k[] = {"kWorldFragCapSpv", "kWorldDeferShadowSpv", "kWorldDeferReflectSpv", "kWorldDeferCombineSpv",
                              "kWorldDeferCombineG11Spv", "kWorldDeferReflectHalfSpv", "kWorldDeferReflectSharpSpv",
                              "kWorldDeferBaseSpv", "kWorldDeferBaseG11Spv", "kWorldDeferLocalSpv", "kWorldDeferLocalG11Spv",
                              "kWorldDeferTileClassifySpv", "kWorldDeferShadowClassifySpv", "kWorldWaterCapFragSpv",
                              "kWorldProbeDeferSpv", "kWorldFragGbufCapSpv", "kWorldPartFragSpv",
                              "kWorldPartFragRtSpv", "kWorldPartLightRtSpv"};
    for (const char* s : k) if (m == s) return true;
    return false;
}

int modeSet(const std::vector<std::string>& lines, const SetOpts& so)
{
    std::vector<std::unique_ptr<Built>> all;
    for (const std::string& line : lines)
    {
        Spec s;
        if (!parseSpec(line, s)) continue;
        std::unique_ptr<Built> b(new Built);
        if (!build(s, *b)) { std::printf("  skip %s (build failed)\n", s.name.c_str()); continue; }
        // var=N: a distinct pipeline of the same modules (the renderer's state variants)
        const size_t vp = line.find(" var=");
        const int var = vp == std::string::npos ? 0 : std::atoi(line.c_str() + vp + 5);
        if (!b->compute && var) b->desc.depthBiasConstant = 0.0001f * (float)var;
        all.push_back(std::move(b));
    }
    vkPipelineSetLanesForBench(so.policy != "p0");
    // Floor placeholder constants (llcleanrenderervk.cpp kPlaceholderSpec): every CAP setting 0,
    // filter 0, AO mode 0, AO upsample 0, sky mode run time, top-N as asked.
    std::vector<VkSpecializationMapEntry> fe;
    std::vector<int32_t> fd;
    for (uint32_t id = 8; id <= 19; ++id)
    {
        fe.push_back({id, (uint32_t)(fd.size() * 4), 4});
        fd.push_back(id == 18 ? -1 : id == 15 ? so.floorTopN : 0);
    }
    const VkSpecializationInfo floorSpec{(uint32_t)fe.size(), fe.data(), fd.size() * 4, fd.data()};
    const VkDevice dev = gDev.device();
    size_t nPlace = 0, nHeavy = 0;
    if (so.policy == "p2")
    {
        std::vector<std::string> names;
        for (auto& kv : gModules) names.push_back(kv.first);
        for (const std::string& n : names)
        {
            const std::string ph = placeholderOf(n);
            Module* pm = ph.empty() ? nullptr : module(ph);
            if (pm) { vkPipelineUpgradeModule(gModules[n]->handle, pm->handle, so.floorTopN >= 0 ? &floorSpec : nullptr); ++nPlace; }
            else if (isHeavyNoPlaceholder(n) || (!so.heavyExtra.empty() && so.heavyExtra.find(n) != std::string::npos))
            {
                vkPipelineUpgradeModule(gModules[n]->handle, VK_NULL_HANDLE, nullptr);
                ++nHeavy;
            }
        }
    }
    const uint64_t r0 = rss();
    RssPeak pk;
    const auto t0 = Clock::now();
    PipelineJob* gateJob = nullptr;
    PipelineJob* upJob = nullptr;
    {
        PipelineBatchScope scope(dev);
        for (auto& b : all)
        {
            if (b->compute)
                createComputePipelineInto(dev, b->comp, b->layout, 0, b->sd->info.mapEntryCount ? &b->sd->info : nullptr,
                                          &b->out, b->spec.name.c_str());
            else
                createGraphicsPipelineInto(dev, b->desc, &b->out, b->spec.name.c_str());
        }
        upJob = pipelineBatchTakeUpgrade(dev, VKPIPE_PRIO_UPGRADE);
        PipelineJobOpts o;
        o.what = "gate";
        gateJob = pipelineBatchFlushAsync(dev, o);
        if (!so.afterGate) pipelineJobStart(upJob);
    }
    // The viewer's FIRST-USE batch (warmFirstUse, queued when the world batch lands; the login waits
    // for it too): 24 light pipelines, timed from the gate's end.
    std::vector<std::unique_ptr<Built>> firstUse;
    for (int i = 0; i < 24; ++i)
    {
        Spec s;
        parseSpec("name=firstuse vert=kWorldVertSpv frag=kWorldPrepassMotionMaskFragSpv", s);
        std::unique_ptr<Built> b(new Built);
        if (build(s, *b)) { b->desc.depthBiasConstant = 0.37f + 0.001f * (float)i; firstUse.push_back(std::move(b)); }
    }
    PipelineJob* fuJob = nullptr;
    double fuStart = 0, fuMs = -1;
    double worldMs = -1, fullMs = -1;
    std::vector<VkPipeline> replaced;
    while (gateJob || upJob || fuJob)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (gateJob && pipelineJobDone(gateJob))
        {
            pipelineJobFinish(gateJob);
            gateJob = nullptr;
            worldMs = msSince(t0);
            std::printf("  world ready (gate done) at %.0f ms: %s\n", worldMs, vkPipelineSchedulerText().c_str());
            if (!upJob) fullMs = worldMs;
            PipelineBatchScope fs(dev);
            for (auto& b : firstUse) createGraphicsPipelineInto(dev, b->desc, &b->out, "firstuse");
            PipelineJobOpts fo;
            fo.what = "first use";
            fuJob = pipelineBatchFlushAsync(dev, fo);
            fuStart = msSince(t0);
        }
        if (fuJob && pipelineJobDone(fuJob))
        {
            pipelineJobFinish(fuJob);
            fuJob = nullptr;
            fuMs = msSince(t0) - fuStart;
            std::printf("  first-use batch done %.0f ms after the gate\n", fuMs);
            if (so.afterGate && upJob) pipelineJobStart(upJob);
        }
        if (!gateJob && !fuJob && upJob && pipelineJobDone(upJob))
        {
            const size_t n = pipelineJobInstall(upJob, &replaced);
            pipelineJobFinish(upJob);
            upJob = nullptr;
            fullMs = msSince(t0);
            std::printf("  full quality at %.0f ms (%zu upgraded, %zu placeholders retired)\n", fullMs, n, replaced.size());
        }
    }
    const uint64_t peak = pk.stop();
    size_t made = 0, missing = 0;
    for (auto& b : all) (b->out ? made : missing)++;
    std::printf("RESULT set policy=%s pipelines=%zu made=%zu missing=%zu placeholder_modules=%zu heavy_modules=%zu "
                "world_ms=%.0f firstuse_ms=%.0f login_ms=%.0f full_ms=%.0f rss_peak_growth_mb=%llu\n",
                so.policy.c_str(), all.size(), made, missing, nPlace, nHeavy, worldMs, fuMs, worldMs + fuMs, fullMs,
                (unsigned long long)((peak > r0 ? peak - r0 : 0) / MB));
    for (VkPipeline p : replaced) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
    for (auto& b : all) if (b->out) { vkForgetPipeline(b->out); vkDestroyPipeline(dev, b->out, nullptr); }
    for (auto& b : firstUse) if (b->out) { vkForgetPipeline(b->out); vkDestroyPipeline(dev, b->out, nullptr); }
    return missing ? 1 : 0;
}

// ---- mode: wtest (P4 out-of-process worker) ----
// The p2 upgrade group of the list goes to a child worker (this binary, `worker` mode) through a
// real recipe; then the same job is created in-process from the worker's cache file (expected:
// all hits). --kill-after MS: SIGKILL the worker mid-compile, check its cache file still loads and
// fall back to compiling in-process. --finish-after MS: deliberate finish (stop(true)) and return
// at once -- the script then watches the detached worker complete and exit.
int modeWorkerTest(const std::vector<std::string>& lines, const SetOpts& so, const std::string& cacheFile, int killAfter,
                   int finishAfter, unsigned workerThreads, unsigned memMB)
{
    std::vector<std::unique_ptr<Built>> all;
    for (const std::string& line : lines)
    {
        Spec s;
        if (!parseSpec(line, s)) continue;
        std::unique_ptr<Built> b(new Built);
        if (!build(s, *b)) continue;
        const size_t vp = line.find(" var=");
        const int var = vp == std::string::npos ? 0 : std::atoi(line.c_str() + vp + 5);
        if (!b->compute && var) b->desc.depthBiasConstant = 0.0001f * (float)var;
        all.push_back(std::move(b));
    }
    const VkDevice dev = gDev.device();
    std::vector<std::string> names;
    for (auto& kv : gModules) names.push_back(kv.first);
    for (const std::string& n : names)
        if (!placeholderOf(n).empty() || isHeavyNoPlaceholder(n)) vkPipelineUpgradeModule(gModules[n]->handle, VK_NULL_HANDLE, nullptr);
    if (std::getenv("SLPIPE_NO_GATE"))   // (discriminator: the parent compiles nothing of its own first)
    {
        std::vector<std::unique_ptr<Built>> keep;
        for (auto& b : all)
        {
            std::string fm = b->compute ? b->spec.comp : b->spec.frag;
            if (!placeholderOf(fm).empty() || isHeavyNoPlaceholder(fm)) keep.push_back(std::move(b));
        }
        all.swap(keep);
        std::printf("  SLPIPE_NO_GATE: %zu heavy pipelines only\n", all.size());
    }
    PipelineJob* up = nullptr;
    {
        PipelineBatchScope scope(dev);
        for (auto& b : all)
        {
            if (b->compute)
                createComputePipelineInto(dev, b->comp, b->layout, 0, b->sd->info.mapEntryCount ? &b->sd->info : nullptr,
                                          &b->out, b->spec.name.c_str());
            else
                createGraphicsPipelineInto(dev, b->desc, &b->out, b->spec.name.c_str());
        }
        up = pipelineBatchTakeUpgrade(dev, VKPIPE_PRIO_UPGRADE);
        // (the scope's end compiles the light rest in-process: not measured here)
    }
    const std::string recipe = vkPipelineJobRecipe(up, gDev.physicalDevice());
    std::printf("  recipe: %zu pipelines, %zu bytes\n", vkPipelineJobSize(up), recipe.size());
    if (recipe.empty()) { pipelineJobFinish(up); return 1; }
    VkPipelineWorkerHost host;
    const uint64_t r0 = rss();
    RssPeak pk;
    const auto t0 = Clock::now();
    if (!host.start(vkPipelineWorkerExePath(), recipe, cacheFile, workerThreads, memMB, true))
    {
        std::printf("  worker start failed: %s\n", host.why().c_str());
        pipelineJobFinish(up);
        return 1;
    }
    bool killed = false;
    while (!host.poll())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (killAfter > 0 && msSince(t0) >= killAfter) { host.killHard(); killed = true; break; }
        if (finishAfter > 0 && msSince(t0) >= finishAfter)
        {
            host.stop(true);
            pk.stop();
            std::printf("RESULT wtest finish-requested at %.0f ms (%u done); parent exits now\n", msSince(t0), host.done());
            pipelineJobFinish(up);
            return 0;
        }
    }
    const double workerMs = msSince(t0);
    const uint64_t parentPeak = pk.stop();
    std::printf("  worker %s after %.0f ms (exit %d, %u done reported); parent RSS growth meanwhile %llu MB\n",
                killed ? "KILLED" : (host.ok() ? "finished" : "FAILED"), workerMs, host.ok() ? 0 : -1, host.done(),
                (unsigned long long)((parentPeak > r0 ? parentPeak - r0 : 0) / MB));
    VkPipelineCache wc = vkPipelineWorkerLoadCache(dev, gDev.physicalDevice(), cacheFile);
    size_t wcBytes = 0;
    if (wc) vkGetPipelineCacheData(dev, wc, &wcBytes, nullptr);
    std::printf("  worker cache file %s (%zu KB)\n", wc ? "loads" : "absent / rejected", wcBytes / 1024);
    if (wc) pipelineJobSetCache(up, wc);
    const auto t1 = Clock::now();
    pipelineJobStart(up);
    while (!pipelineJobDone(up)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<VkPipeline> replaced;
    const size_t n = pipelineJobInstall(up, &replaced);
    pipelineJobFinish(up);
    const double inMs = msSince(t1);
    size_t made = 0;
    for (auto& b : all) made += b->out ? 1 : 0;
    std::printf("RESULT wtest worker_ms=%.0f killed=%d worker_cache_kb=%zu inprocess_ms=%.0f upgraded=%zu made=%zu/%zu\n",
                workerMs, killed ? 1 : 0, wcBytes / 1024, inMs, n, made, all.size());
    for (VkPipeline p : replaced) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
    for (auto& b : all) if (b->out) { vkForgetPipeline(b->out); vkDestroyPipeline(dev, b->out, nullptr); }
    if (wc) vkDestroyPipelineCache(dev, wc, nullptr);
    return made == all.size() ? 0 : 1;
}

// ---- mode: selftest (#1411 scheduler / cache / upgrade / holds / cancel checks on the real GPU) ----
int gFails = 0;
void check(bool ok, const char* what)
{
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++gFails;
}

double runBatchWall(std::vector<std::unique_ptr<Built>>& v)
{
    const auto t0 = Clock::now();
    PipelineJob* j = nullptr;
    {
        PipelineBatchScope scope(gDev.device());
        for (auto& b : v)
        {
            if (b->compute)
                createComputePipelineInto(gDev.device(), b->comp, b->layout, 0, b->sd->info.mapEntryCount ? &b->sd->info : nullptr,
                                          &b->out, b->spec.name.c_str());
            else
                createGraphicsPipelineInto(gDev.device(), b->desc, &b->out, b->spec.name.c_str());
        }
        j = pipelineBatchFlushAsync(gDev.device(), PipelineJobOpts());
    }
    pipelineJobFinish(j);
    return msSince(t0);
}

void destroyAll(std::vector<std::unique_ptr<Built>>& v)
{
    for (auto& b : v) if (b->out) { vkForgetPipeline(b->out); vkDestroyPipeline(gDev.device(), b->out, nullptr); b->out = VK_NULL_HANDLE; }
}

std::unique_ptr<Built> mk(const std::string& line, int var)
{
    Spec s;
    parseSpec(line, s);
    std::unique_ptr<Built> b(new Built);
    if (!build(s, *b)) return nullptr;
    if (!b->compute) b->desc.depthBiasConstant = 0.0001f * (float)(var + 1);
    return b;
}

int modeSelfTest(const std::string& dir)
{
    const VkDevice dev = gDev.device();
    const std::string light = "name=light vert=kWorldVertSpv frag=kWorldPrepassMotionMaskFragSpv";
    const char* heavy[4] = {"name=heavyA comp=kWorldDeferBasicSpv", "name=heavyB comp=kWorldDeferBasicG11Spv",
                            "name=heavyC vert=kWorldVertSpv frag=kWorldFragBlendBasicSpv",
                            "name=heavyD vert=kWorldVertSpv frag=kWorldFragBasicSpv"};
    // 1. LANES: heavy lane capped at 1 (the AMD cold start): the 24 light pipelines must all finish
    //    before the heavy lane does (they never queue behind it). One batch, one process: the
    //    driver's in-process shader cache would serve a second pass.
    {
        std::printf("# lanes\n");
        vkSetPipelineConcurrencyStart(1);
        gLastReport = VkPipelineBatchReport();
        std::vector<std::unique_ptr<Built>> v;
        // (the basic modules are LIGHT by size -- ~0.5 s; real heavies: the combine kernels, ~3 s each)
        for (const char* h : {"name=heavy1 comp=kWorldDeferCombineSpv", "name=heavy2 comp=kWorldDeferCombineG11Spv"})
            if (auto b = mk(h, 0)) v.push_back(std::move(b));
        for (int i = 0; i < 24; ++i) if (auto b = mk(light, 100 + i)) v.push_back(std::move(b));
        runBatchWall(v);
        size_t made = 0;
        for (auto& b : v) made += b->out ? 1 : 0;
        check(made == v.size(), "every pipeline made");
        double lightEnd = 0, heavyEnd = 0;
        unsigned nl = 0, nh = 0;
        for (const VkPipelineCompileStat& s : gLastReport.items)
        {
            if (s.light) { lightEnd = std::max(lightEnd, s.endMs); ++nl; }
            else { heavyEnd = std::max(heavyEnd, s.endMs); ++nh; }
        }
        std::printf("  %u light done by %.0f ms, %u heavy done by %.0f ms\n", nl, lightEnd, nh, heavyEnd);
        check(nl >= 20 && nh >= 2 && lightEnd < heavyEnd * 0.5, "light pipelines finish while the capped heavy lane is still busy");
        destroyAll(v);
        vkSetPipelineConcurrencyStart(0);
    }
    // 2. UPGRADE + MODULE HOLDS: the placeholder lands in the slot from the main job, the full one
    //    replaces it on install; a module both jobs use outlives the first job to finish.
    {
        std::printf("# upgrade group / module holds\n");
        Module* full = module("kWorldFragBlendBasicSpv");
        Module* ph = module("kWorldPrepassMotionMaskFragSpv");
        std::unique_ptr<Built> b = mk("name=up vert=kWorldVertSpv frag=kWorldFragBlendBasicSpv", 500);
        // a private module copy so destroying it is observable through the registry
        VkShaderModule own = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = full->words.size() * 4;
        ci.pCode = full->words.data();
        vkCreateShaderModule(dev, &ci, nullptr, &own);
        vkNoteShaderModule(own, full->words.data(), ci.codeSize, "selftest-own");
        b->desc.frag = own;
        vkPipelineUpgradeModule(own, ph->handle, nullptr);
        PipelineJob *up = nullptr, *mainJob = nullptr;
        {
            PipelineBatchScope scope(dev);
            createGraphicsPipelineInto(dev, b->desc, &b->out, "up");
            destroyShaderModuleDeferred(dev, own, nullptr);   // released by its owner: alive while held
            up = pipelineBatchTakeUpgrade(dev, VKPIPE_PRIO_UPGRADE);
            mainJob = pipelineBatchFlushAsync(dev, PipelineJobOpts());
        }
        check(up != nullptr && mainJob != nullptr, "a main (placeholder) job and an upgrade job");
        pipelineJobFinish(mainJob);
        const VkPipeline placeholder = b->out;
        check(placeholder != VK_NULL_HANDLE, "the placeholder is in the slot after the main job");
        const uint32_t* code = nullptr;
        uint32_t words = 0;
        std::string nm;
        check(vkPipelineModuleCode(own, code, words, nm), "the full module is still alive (held by the unstarted upgrade job)");
        pipelineJobStart(up);
        while (!pipelineJobDone(up)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::vector<VkPipeline> replaced;
        const size_t n = pipelineJobInstall(up, &replaced);
        pipelineJobFinish(up);
        check(n == 1 && b->out != VK_NULL_HANDLE && b->out != placeholder, "install writes the full pipeline over the slot");
        check(replaced.size() == 1 && replaced[0] == placeholder, "install returns the placeholder for retirement");
        check(!vkPipelineModuleCode(own, code, words, nm), "the module is destroyed when its last holder finished");
        for (VkPipeline p : replaced) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
        vkForgetPipeline(b->out);
        vkDestroyPipeline(dev, b->out, nullptr);
    }
    // 3. CANCEL: a started job stops starting compiles; finish returns once the ones in flight end.
    {
        std::printf("# cancel\n");
        std::vector<std::unique_ptr<Built>> v;
        for (int i = 0; i < 40; ++i) if (auto b = mk(heavy[i % 4], 1000 + i)) v.push_back(std::move(b));
        vkSetPipelineConcurrencyStart(1);
        PipelineJob* j = nullptr;
        {
            PipelineBatchScope scope(dev);
            for (auto& b : v)
            {
                if (b->compute) createComputePipelineInto(dev, b->comp, b->layout, 0, nullptr, &b->out, "cancel");
                else createGraphicsPipelineInto(dev, b->desc, &b->out, "cancel");
            }
            j = pipelineBatchFlushAsync(dev, PipelineJobOpts());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0 = Clock::now();
        pipelineJobCancel(j);
        pipelineJobFinish(j);
        const double ms = msSince(t0);
        size_t made = 0;
        for (auto& b : v) made += b->out ? 1 : 0;
        std::printf("  cancelled: %zu of %zu made, finish took %.0f ms\n", made, v.size(), ms);
        check(made < v.size() && ms < 3000, "cancel leaves the rest unstarted and finishes promptly");
        destroyAll(v);
        vkSetPipelineConcurrencyStart(0);
    }
    // 4. CACHE ACROSS A VERSION BUMP (P1.3) and corruption handling.
    {
        std::printf("# cache file\n");
        const std::string path = dir + "/selftest_cache.bin";
        std::vector<std::unique_ptr<Built>> v;
        if (auto b = mk(heavy[0], 2000)) v.push_back(std::move(b));
        runBatchWall(v);
        size_t last = 0;
        check(vkpipecache::save(dev, gDev.physicalDevice(), vkSharedPipelineCache(), path, "Viewer 1.5.0.1", last, "selftest"),
              "save");
        std::vector<uint8_t> blob;
        check(vkpipecache::load(gDev.physicalDevice(), path, "Viewer 1.5.0.2", blob) && !blob.empty(),
              "a cache written by another viewer build still loads");
        // truncate the payload: must be rejected (and removed)
        if (LLFILE* f = LLFile::fopen(path, "r+b")) { LLFile::close(f); }
        {
            std::ifstream in(path, std::ios::binary);
            std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(all.data(), (std::streamsize)(all.size() / 2));
        }
        blob.clear();
        check(!vkpipecache::load(gDev.physicalDevice(), path, "Viewer 1.5.0.2", blob) && blob.empty(), "a truncated file is rejected");
        check(!LLFile::isfile(path), "and removed");
        destroyAll(v);
    }
    // 5. COVERAGE: a loaded cache whose keys were never hits is treated cold (event line).
    {
        std::printf("# coverage\n");
        vkSetPipelineCacheWarm(true);
        std::vector<std::unique_ptr<Built>> v;
        for (int i = 0; i < 10; ++i) if (auto b = mk(light, 3000 + i)) v.push_back(std::move(b));
        gLastEvents.clear();
        runBatchWall(v);
        bool cold = false;
        for (const std::string& e : gLastEvents) cold |= e.find("loaded but COLD for this batch") != std::string::npos;
        check(cold, "a warm-flagged cache with 0% expected hits runs the batch cold");
        destroyAll(v);
        vkSetPipelineCacheWarm(false);
    }
    std::printf("RESULT selftest %s (%d failure(s))\n", gFails ? "FAILED" : "passed", gFails);
    return gFails ? 1 : 0;
}

// ---- mode: gpl (P3 measurement: graphics pipeline libraries for the lit FS family) ----
// For each listed graphics pipeline: a fragment-shader library per distinct (frag module, blend)
// -- compiled ONCE -- plus a pre-rasterization library per vertex module, then every pipeline
// linked fast (no LTO) and with link-time optimization. Prints the library compile, both link times.
struct GplLibs { VkPipeline vi = VK_NULL_HANDLE, pre = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, fo = VK_NULL_HANDLE; };

VkPipeline gplLib(const Built& b, VkGraphicsPipelineLibraryFlagsEXT part, double& ms)
{
    const GraphicsPipelineDesc& d = b.desc;
    VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    lib.flags = part;
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = (uint32_t)d.bindings.size(); vin.pVertexBindingDescriptions = d.bindings.data();
    vin.vertexAttributeDescriptionCount = (uint32_t)d.attributes.size(); vin.pVertexAttributeDescriptions = d.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.f; rs.depthBiasEnable = d.depthBiasConstant != 0.f; rs.depthBiasConstantFactor = d.depthBiasConstant;
    VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba[4]{};
    for (auto& a : cba)
    {
        a.colorWriteMask = 0xF;
        if (d.blend) { a.blendEnable = VK_TRUE; a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                       a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; }
    }
    const bool mrt = d.mrtCount > 0;
    const VkFormat fmts[4] = {mrt ? d.mrtFormats[0] : d.colorFormat, mrt ? d.mrtFormats[1] : d.glowFormat,
                              mrt ? d.mrtFormats[2] : d.reactFormat, mrt ? d.mrtFormats[3] : VK_FORMAT_UNDEFINED};
    uint32_t nColor = mrt ? d.mrtCount : (d.reactFormat ? 3u : d.glowFormat ? 2u : 1u);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = nColor; cb.pAttachments = cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 7; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = nColor; rend.pColorAttachmentFormats = fmts; rend.depthAttachmentFormat = d.depthFormat;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    rend.pNext = &lib;
    pci.pNext = &rend;
    pci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    pci.pDynamicState = &dy;
    if (part & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) { pci.pVertexInputState = &vin; pci.pInputAssemblyState = &ia; }
    if (part & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)
    {
        st.stage = VK_SHADER_STAGE_VERTEX_BIT; st.module = d.vert;
        pci.stageCount = 1; pci.pStages = &st; pci.pViewportState = &vp; pci.pRasterizationState = &rs; pci.layout = d.layout;
    }
    if (part & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT)
    {
        st.stage = VK_SHADER_STAGE_FRAGMENT_BIT; st.module = d.frag; st.pSpecializationInfo = d.fragSpec;
        pci.stageCount = 1; pci.pStages = &st; pci.pDepthStencilState = &ds; pci.pMultisampleState = &ms1; pci.layout = d.layout;
    }
    if (part & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) { pci.pColorBlendState = &cb; pci.pMultisampleState = &ms1; }
    VkPipeline p = VK_NULL_HANDLE;
    const auto t0 = Clock::now();
    if (vkCreateGraphicsPipelines(gDev.device(), vkSharedPipelineCache(), 1, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    ms = msSince(t0);
    return p;
}

VkPipeline gplLink(const GplLibs& l, VkPipelineLayout layout, bool lto, double& ms)
{
    VkPipeline libs[4] = {l.vi, l.pre, l.fs, l.fo};
    VkPipelineLibraryCreateInfoKHR li{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    li.libraryCount = 4; li.pLibraries = libs;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &li;
    pci.layout = layout;
    pci.flags = lto ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    VkPipeline p = VK_NULL_HANDLE;
    const auto t0 = Clock::now();
    if (vkCreateGraphicsPipelines(gDev.device(), vkSharedPipelineCache(), 1, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    ms = msSince(t0);
    return p;
}

int modeGpl(const std::vector<std::string>& lines)
{
    if (!gDev.graphicsPipelineLibraryEnabled()) { std::printf("RESULT gpl unsupported on this device\n"); return 1; }
    std::map<std::string, VkPipeline> fsLibs, preLibs;
    std::vector<VkPipeline> made;
    double sumLib = 0, sumFast = 0, sumLto = 0;
    std::printf("%-36s %9s %9s %9s %9s\n", "pipeline", "fs_lib_ms", "pre_ms", "fast_ms", "lto_ms");
    for (const std::string& line : lines)
    {
        Spec s;
        if (!parseSpec(line, s) || !s.comp.empty()) continue;
        Built b;
        if (!build(s, b)) continue;
        GplLibs l;
        double t = 0, tfs = 0, tpre = 0, tf = 0, tl = 0;
        l.vi = gplLib(b, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, t);
        l.fo = gplLib(b, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, t);
        const std::string fk = s.frag + (s.blend ? "+b" : "");
        if (!fsLibs.count(fk)) { fsLibs[fk] = gplLib(b, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, tfs); sumLib += tfs; }
        if (!preLibs.count(s.vert)) { preLibs[s.vert] = gplLib(b, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, tpre); sumLib += tpre; }
        l.fs = fsLibs[fk];
        l.pre = preLibs[s.vert];
        VkPipeline f = gplLink(l, b.layout, false, tf);
        VkPipeline o = gplLink(l, b.layout, true, tl);
        sumFast += tf;
        sumLto += tl;
        std::printf("%-36s %9.0f %9.0f %9.1f %9.0f%s\n", s.name.c_str(), tfs, tpre, tf, tl, (f && o) ? "" : " (link FAILED)");
        std::fflush(stdout);
        for (VkPipeline p : {f, o, l.vi, l.fo}) if (p) made.push_back(p);
    }
    for (auto& kv : fsLibs) if (kv.second) made.push_back(kv.second);
    for (auto& kv : preLibs) if (kv.second) made.push_back(kv.second);
    std::printf("RESULT gpl libraries %.0f ms, fast links %.0f ms, LTO links %.0f ms (%zu fs libs, %zu pre libs)\n", sumLib, sumFast,
                sumLto, fsLibs.size(), preLibs.size());
    for (VkPipeline p : made) vkDestroyPipeline(gDev.device(), p, nullptr);
    return 0;
}

std::vector<std::string> readList(const std::string& path)
{
    std::vector<std::string> v;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty() && line[0] != '#') v.push_back(line);
    return v;
}

void batchReport(const VkPipelineBatchReport& r)
{
    gLastReport = r;
    std::printf("  batch (%s): %zu pipelines (%zu compiled + %zu twins), wall %.0f ms, sum %.0f ms, %u threads, "
                "at most %u at once, peak +%lld MB\n",
                r.what, r.total, r.compiled, r.fromCache, r.wallMs, r.sumMs, r.threads, r.maxConcurrency,
                (long long)(((int64_t)r.peakBytes - (int64_t)r.baseBytes) / (int64_t)MB));
    std::vector<const VkPipelineCompileStat*> v;
    for (const VkPipelineCompileStat& s : r.items) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](const VkPipelineCompileStat* a, const VkPipelineCompileStat* b) { return a->ms > b->ms; });
    for (size_t i = 0; i < v.size() && i < 30; ++i)
        std::printf("    %8.0f ms %5llu MB c%u%s %s\n", v[i]->ms, (unsigned long long)(v[i]->attribBytes / MB), v[i]->concurrency, v[i]->light ? " L" : "",
                    v[i]->name.c_str());
    std::fflush(stdout);
}
void eventHook(bool warn, const char* line)
{
    std::printf("  %s%s\n", warn ? "WARN " : "", line);
    std::fflush(stdout);
    if (gLastEvents.size() < 256) gLastEvents.push_back(line);
}
uint64_t memProbe() { return rss(); }
}

int benchMain(int argc, char** argv);

int main(int argc, char** argv)
{
    // P4: the out-of-process compile worker is this same binary (and the viewer's) in worker mode.
    if (argc > 1 && std::strcmp(argv[1], "worker") == 0) return vkPipelineWorkerMain(argc - 1, argv + 1);
    return benchMain(argc, argv);
}

int benchMain(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: slpipe-bench <spv dir> one <spec>... | set <list> [opts] | cache <list> <dir>\n");
        return 2;
    }
    gSpvDir = argv[1];
    const std::string mode = argv[2];
    unsigned threads = 8;
    int repeat = 1;
    SetOpts so;
    std::string cacheFile, tag = "bench", saveWhy;
    unsigned coldCap = 0, workerThreads = 4, memMB = 0;
    int killAfter = 0, finishAfter = 0;
    std::string workerCache;
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) threads = (unsigned)std::atoi(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (a == "--policy" && i + 1 < argc) so.policy = argv[++i];
        else if (a == "--floor-topn" && i + 1 < argc) so.floorTopN = std::atoi(argv[++i]);
        else if (a == "--heavy" && i + 1 < argc) so.heavyExtra = argv[++i];
        else if (a == "--upgrade-after-gate") so.afterGate = true;
        else if (a == "--cache-file" && i + 1 < argc) cacheFile = argv[++i];
        else if (a == "--tag" && i + 1 < argc) tag = argv[++i];
        else if (a == "--save") saveWhy = "bench";
        else if (a == "--cold-cap" && i + 1 < argc) coldCap = (unsigned)std::atoi(argv[++i]);
        else if (a == "--stats") gDev.setPipelineStatsRequested(true);
        else if (a == "--kill-after" && i + 1 < argc) killAfter = std::atoi(argv[++i]);
        else if (a == "--finish-after" && i + 1 < argc) finishAfter = std::atoi(argv[++i]);
        else if (a == "--worker-threads" && i + 1 < argc) workerThreads = (unsigned)std::atoi(argv[++i]);
        else if (a == "--mem-mb" && i + 1 < argc) memMB = (unsigned)std::atoi(argv[++i]);
        else if (a == "--worker-cache" && i + 1 < argc) workerCache = argv[++i];
        else rest.push_back(a);
    }
    std::printf("# slpipe-bench %s start %s (pid %d)\n", mode.c_str(), utcNow().c_str(), (int)getpid());
    gDev.setRayTracingRequested(true);
    if (mode == "gpl") gDev.setGraphicsPipelineLibraryRequested(true);
    if (!cacheFile.empty()) gDev.setPipelineCacheFile(cacheFile, tag);
    if (!gDev.init(false)) { std::fprintf(stderr, "bench: device init failed\n"); return 1; }
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(gDev.physicalDevice(), &p);
        gPushMax = std::min<uint32_t>(p.limits.maxPushConstantsSize, 256);
        std::printf("# device %s driver %u.%u ray query %d\n", p.deviceName, p.driverVersion >> 22,
                    (p.driverVersion >> 14) & 0xFF, gDev.rayQuerySupported() ? 1 : 0);
    }
    vkSetPipelineCompileThreads(threads);
    vkSetPipelineMemProbe(&memProbe);
    vkSetPipelineBatchHook(&batchReport);
    vkSetPipelineEventHook(&eventHook);
    vkSetPipelineCacheWarm(gDev.pipelineCacheWarm());
    vkSetPipelineConcurrencyStart(gDev.pipelineCacheWarm() ? 0 : coldCap);
    vkSetPipelineColdStartCap(coldCap);
    std::printf("# pipeline cache %s (%s)\n", cacheFile.empty() ? "in memory" : cacheFile.c_str(),
                gDev.pipelineCacheWarm() ? "loaded" : "empty");
    int rc = 0;
    if (mode == "one" && rest.size() == 1 && rest[0][0] == '@') rc = modeOne(readList(rest[0].substr(1)), repeat);
    else if (mode == "one") rc = modeOne(rest, repeat);
    else if (mode == "set" && !rest.empty()) rc = modeSet(readList(rest[0]), so);
    else if (mode == "wtest" && !rest.empty() && !workerCache.empty())
        rc = modeWorkerTest(readList(rest[0]), so, workerCache, killAfter, finishAfter, workerThreads, memMB);
    else if (mode == "selftest" && !rest.empty()) rc = modeSelfTest(rest[0]);
    else if (mode == "gpl" && !rest.empty()) rc = modeGpl(readList(rest[0]));
    else { std::fprintf(stderr, "bench: unknown mode %s\n", mode.c_str()); rc = 2; }
    if (!saveWhy.empty()) gDev.savePipelineCache("bench");
    for (auto& kv : gModules) if (kv.second->handle) vkDestroyShaderModule(gDev.device(), kv.second->handle, nullptr);
    gDev.shutdown();
    std::printf("# slpipe-bench %s end %s\n", mode.c_str(), utcNow().c_str());
    return rc;
}
