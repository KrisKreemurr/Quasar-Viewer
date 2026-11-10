/**
 * @file vk_pipeworker.cpp
 * @brief See vk_pipeworker.h.
 */
#include "linden_common.h"

#include "vk_pipeworker.h"
#include "vk_device.h"
#include "vk_pipeline.h"
#include "vk_pipecache.h"

#include "llfile.h"
#include "llmemory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if LL_LINUX
#include <sys/prctl.h>
#endif
extern char** environ;
#endif

// ======================================================================= layout registry
namespace
{
struct SetLayoutRec
{
    uint32_t flags = 0;
    std::vector<VkDescriptorSetLayoutBinding> bindings;   // (pImmutableSamplers dropped)
    std::vector<VkDescriptorBindingFlags> bindingFlags;
    bool immutable = false;                               // has immutable samplers: not describable
};
struct PipeLayoutRec
{
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> push;
};
constexpr size_t kMaxLayouts = 4096;
std::mutex& layoutMutex() { static std::mutex m; return m; }
std::unordered_map<VkDescriptorSetLayout, SetLayoutRec>& setLayouts() { static std::unordered_map<VkDescriptorSetLayout, SetLayoutRec> m; return m; }
std::unordered_map<VkPipelineLayout, PipeLayoutRec>& pipeLayouts() { static std::unordered_map<VkPipelineLayout, PipeLayoutRec> m; return m; }
}

void vkPipelineNoteSetLayout(VkDescriptorSetLayout l, const VkDescriptorSetLayoutCreateInfo* ci)
{
    if (!l || !ci) return;
    SetLayoutRec r;
    r.flags = ci->flags;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo* bfi = nullptr;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            bfi = (const VkDescriptorSetLayoutBindingFlagsCreateInfo*)p;
    for (uint32_t i = 0; i < ci->bindingCount; ++i)
    {
        VkDescriptorSetLayoutBinding b = ci->pBindings[i];
        if (b.pImmutableSamplers) r.immutable = true;
        b.pImmutableSamplers = nullptr;
        r.bindings.push_back(b);
        r.bindingFlags.push_back((bfi && i < bfi->bindingCount && bfi->pBindingFlags) ? bfi->pBindingFlags[i] : 0);
    }
    std::lock_guard<std::mutex> lk(layoutMutex());
    if (setLayouts().size() >= kMaxLayouts) setLayouts().clear();
    setLayouts()[l] = std::move(r);
}

void vkPipelineNotePipelineLayout(VkPipelineLayout l, const VkPipelineLayoutCreateInfo* ci)
{
    if (!l || !ci) return;
    PipeLayoutRec r;
    r.sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
    r.push.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
    std::lock_guard<std::mutex> lk(layoutMutex());
    if (pipeLayouts().size() >= kMaxLayouts) pipeLayouts().clear();
    pipeLayouts()[l] = std::move(r);
}

void vkPipelineForgetSetLayout(VkDescriptorSetLayout l)
{
    std::lock_guard<std::mutex> lk(layoutMutex());
    setLayouts().erase(l);
}

// ======================================================================= recipe I/O
namespace
{
constexpr char kRecipeMagic[4] = {'S', 'L', 'P', 'R'};
constexpr uint32_t kRecipeVersion = 1;

struct Writer
{
    std::string s;
    void u32(uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
    void i32(int32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
    void f32(float v) { s.append(reinterpret_cast<const char*>(&v), 4); }
    void bytes(const void* p, size_t n) { s.append(static_cast<const char*>(p), n); }
    void str(const std::string& v) { u32((uint32_t)v.size()); s += v; }
};
struct Reader
{
    const std::string& s;
    size_t pos = 0;
    bool bad = false;
    explicit Reader(const std::string& d) : s(d) {}
    bool take(void* p, size_t n)
    {
        if (bad || pos + n > s.size()) { bad = true; std::memset(p, 0, n); return false; }
        std::memcpy(p, s.data() + pos, n);
        pos += n;
        return true;
    }
    uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; take(&v, 4); return v; }
    float f32() { float v = 0; take(&v, 4); return v; }
    std::string str()
    {
        const uint32_t n = u32();
        if (bad || n > (64u << 20) || pos + n > s.size()) { bad = true; return std::string(); }
        std::string r = s.substr(pos, n);
        pos += n;
        return r;
    }
    // A count bounded by what is left (malformed input never allocates without bound).
    uint32_t count(size_t minEach)
    {
        const uint32_t n = u32();
        if (bad || (size_t)n * std::max<size_t>(minEach, 1) > s.size() - pos) { bad = true; return 0; }
        return n;
    }
};

// Every GraphicsPipelineDesc field that reaches the driver (the handles are indices elsewhere).
void writeDesc(Writer& w, const GraphicsPipelineDesc& d)
{
    w.u32(d.colorFormat); w.u32(d.depthFormat); w.u32(d.depthTest); w.u32(d.depthWrite); w.u32(d.blend);
    w.u32(d.additive); w.u32(d.glowFormat); w.u32(d.glowWrite); w.u32(d.reactFormat); w.u32(d.reactWriteMask);
    w.u32(d.mrtCount);
    for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) w.u32(d.mrtFormats[i]);
    w.u32(d.polygonMode); w.u32(d.depthCompare); w.u32(d.relaxedDepthCompare);
    w.f32(d.depthBiasConstant); w.f32(d.depthBiasSlope); w.f32(d.lineWidth);
    w.u32(d.cullMode); w.u32(d.frontFace); w.u32(d.noColor); w.u32(d.depthClamp); w.u32(d.customBlend);
    w.u32(d.srcColor); w.u32(d.dstColor); w.u32(d.partAccum); w.u32(d.glowAdditive); w.u32(d.alphaOne);
    w.u32(d.colorWrite); w.u32(d.dynamicDepthCull); w.u32(d.dynamicShadingRate); w.u32(d.fsrAttachment);
    w.u32(d.reverseZ); w.u32(d.views); w.u32(d.disableOptimization);
    w.u32((uint32_t)d.bindings.size());
    for (const auto& b : d.bindings) { w.u32(b.binding); w.u32(b.stride); w.u32(b.inputRate); }
    w.u32((uint32_t)d.attributes.size());
    for (const auto& a : d.attributes) { w.u32(a.location); w.u32(a.binding); w.u32(a.format); w.u32(a.offset); }
}
void readDesc(Reader& r, GraphicsPipelineDesc& d)
{
    d.colorFormat = (VkFormat)r.u32(); d.depthFormat = (VkFormat)r.u32(); d.depthTest = r.u32(); d.depthWrite = r.u32();
    d.blend = r.u32(); d.additive = r.u32(); d.glowFormat = (VkFormat)r.u32(); d.glowWrite = r.u32();
    d.reactFormat = (VkFormat)r.u32(); d.reactWriteMask = r.u32(); d.mrtCount = std::min(r.u32(), GraphicsPipelineDesc::kMaxMrt);
    for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) d.mrtFormats[i] = (VkFormat)r.u32();
    d.polygonMode = (VkPolygonMode)r.u32(); d.depthCompare = (VkCompareOp)r.u32(); d.relaxedDepthCompare = (VkCompareOp)r.u32();
    d.depthBiasConstant = r.f32(); d.depthBiasSlope = r.f32(); d.lineWidth = r.f32();
    d.cullMode = r.u32(); d.frontFace = (VkFrontFace)r.u32(); d.noColor = r.u32(); d.depthClamp = r.u32();
    d.customBlend = r.u32(); d.srcColor = (VkBlendFactor)r.u32(); d.dstColor = (VkBlendFactor)r.u32();
    d.partAccum = r.u32(); d.glowAdditive = r.u32(); d.alphaOne = r.u32(); d.colorWrite = r.u32();
    d.dynamicDepthCull = r.u32(); d.dynamicShadingRate = r.u32(); d.fsrAttachment = r.u32(); d.reverseZ = r.u32();
    d.views = std::max(1u, r.u32()); d.disableOptimization = r.u32();
    const uint32_t nb = r.count(12);
    d.bindings.resize(nb);
    for (auto& b : d.bindings) { b.binding = r.u32(); b.stride = r.u32(); b.inputRate = (VkVertexInputRate)r.u32(); }
    const uint32_t na = r.count(16);
    d.attributes.resize(na);
    for (auto& a : d.attributes) { a.location = r.u32(); a.binding = r.u32(); a.format = (VkFormat)r.u32(); a.offset = r.u32(); }
}

void writeHeader(Writer& w, VkPhysicalDevice phys)
{
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    w.bytes(kRecipeMagic, 4);
    w.u32(kRecipeVersion);
    w.u32(p.vendorID); w.u32(p.deviceID); w.u32(p.driverVersion);
    w.bytes(p.pipelineCacheUUID, VK_UUID_SIZE);
}
}

std::string vkPipelineJobRecipe(const PipelineJob* job, VkPhysicalDevice phys)
{
    const size_t n = vkPipelineJobSize(job);
    if (!n || !phys) return std::string();
    std::vector<VkPipelineRecipeItem> items(n);
    for (size_t i = 0; i < n; ++i) vkPipelineJobItem(job, i, items[i]);
    std::vector<VkShaderModule> mods;
    std::vector<VkPipelineLayout> pls;
    std::vector<VkDescriptorSetLayout> sls;
    auto idxOf = [](auto& v, auto h) -> int32_t {
        if (!h) return -1;
        auto it = std::find(v.begin(), v.end(), h);
        if (it != v.end()) return (int32_t)(it - v.begin());
        v.push_back(h);
        return (int32_t)v.size() - 1;
    };
    Writer body;
    body.u32((uint32_t)n);
    std::vector<int32_t> plIdx(n);
    for (const VkPipelineRecipeItem& it : items)
    {
        body.str(it.name);
        body.u32(it.compute);
        body.i32(idxOf(mods, it.desc.vert)); body.i32(idxOf(mods, it.desc.frag)); body.i32(idxOf(mods, it.desc.task));
        body.i32(idxOf(mods, it.desc.mesh)); body.i32(idxOf(mods, it.comp));
        body.i32(idxOf(pls, it.desc.layout));
        body.u32(it.compFlags);
        writeDesc(body, it.desc);
        body.u32((uint32_t)it.specEntries.size());
        for (const auto& e : it.specEntries) { body.u32(e.constantID); body.u32(e.offset); body.u32((uint32_t)e.size); }
        body.u32((uint32_t)it.specData.size());
        body.bytes(it.specData.data(), it.specData.size());
    }
    Writer w;
    writeHeader(w, phys);
    w.u32((uint32_t)mods.size());
    for (VkShaderModule m : mods)
    {
        const uint32_t* code = nullptr;
        uint32_t words = 0;
        std::string name;
        if (!vkPipelineModuleCode(m, code, words, name)) return std::string();   // (an unnoted module)
        w.u32(words);
        w.bytes(code, (size_t)words * 4);
        w.str(name);
    }
    std::vector<std::string> plBlobs;
    {
        std::lock_guard<std::mutex> lk(layoutMutex());
        for (VkPipelineLayout pl : pls)
        {
            auto it = pipeLayouts().find(pl);
            if (it == pipeLayouts().end()) return std::string();
            Writer pw;
            pw.u32((uint32_t)it->second.sets.size());
            for (VkDescriptorSetLayout sl : it->second.sets) pw.i32(idxOf(sls, sl));
            pw.u32((uint32_t)it->second.push.size());
            for (const auto& r : it->second.push) { pw.u32(r.stageFlags); pw.u32(r.offset); pw.u32(r.size); }
            plBlobs.push_back(pw.s);
        }
        w.u32((uint32_t)sls.size());
        for (VkDescriptorSetLayout sl : sls)
        {
            auto it = setLayouts().find(sl);
            if (it == setLayouts().end() || it->second.immutable) return std::string();
            w.u32(it->second.flags);
            w.u32((uint32_t)it->second.bindings.size());
            for (size_t k = 0; k < it->second.bindings.size(); ++k)
            {
                const VkDescriptorSetLayoutBinding& b = it->second.bindings[k];
                w.u32(b.binding); w.u32(b.descriptorType); w.u32(b.descriptorCount); w.u32(b.stageFlags);
                w.u32(it->second.bindingFlags[k]);
            }
        }
    }
    w.u32((uint32_t)plBlobs.size());
    for (const std::string& b : plBlobs) w.s += b;
    w.s += body.s;
    return w.s;
}

// ======================================================================= worker
namespace
{
std::atomic<VkPipelineRespinFn> s_respin{nullptr};
std::atomic<bool> s_finishing{false};
std::atomic<int64_t> s_finishSince{0};
VkDeviceContext* s_workerDev = nullptr;
std::string s_workerCache;
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void workerSay(const char* fmt, unsigned a, unsigned b)
{
    std::printf(fmt, a, b);
    std::fflush(stdout);
}
void workerProgress()
{
    static int64_t last = 0;
    unsigned done = 0, queued = 0;
    double idle = 0;
    vkPipelineCompileProgress(done, queued, idle);
    workerSay("done %u %u\n", done, queued);
    const int64_t t = nowMs();
    if (s_workerDev && t - last >= 1000)   // (a heavy completion forces this hook too: at most once a second)
    {
        last = t;
        s_workerDev->savePipelineCache("worker progress");
        size_t n = 0;
        vkPipelineCacheGetDataGated(s_workerDev->device(), s_workerDev->pipelineCache(), &n, nullptr, "worker progress");
        workerSay("saved %u KB %u\n", (unsigned)(n >> 10), 0u);
    }
}
void workerEvent(bool warn, const char* line) { std::fprintf(stderr, "[pipeworker]%s %s\n", warn ? " WARN" : "", line); }
uint64_t workerRss() { return LLMemory::getCurrentRSS(); }
}

static int workerRun(int argc, char** argv);

int vkPipelineWorkerMain(int argc, char** argv)
{
    // The stdin reader thread sits in fgets holding stdin's lock: a normal exit() would block
    // forever in the stdio cleanup (measured: the worker hung after its last pipeline). Every
    // result is on disk / in stdout by now: flush and leave without the exit-time cleanup.
    const int rc = workerRun(argc, argv);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}

static int workerRun(int argc, char** argv)
{
    std::string recipePath, cacheFile, lockPath, costsPath;
    unsigned threads = 2, memMB = 0, finishMax = 600, memCapMB = 0;
    bool rayTracing = true, respin = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--recipe") recipePath = next();
        else if (a == "--cache") cacheFile = next();
        else if (a == "--lock") lockPath = next();
        else if (a == "--threads") threads = (unsigned)std::max(1, std::atoi(next()));
        else if (a == "--mem-mb") memMB = (unsigned)std::max(0, std::atoi(next()));
        else if (a == "--finish-max-sec") finishMax = (unsigned)std::max(0, std::atoi(next()));
        else if (a == "--ray-tracing") rayTracing = std::atoi(next()) != 0;
        else if (a == "--respin") respin = true;
        else if (a == "--mem-cap-mb") memCapMB = (unsigned)std::max(0, std::atoi(next()));
        else if (a == "--costs") costsPath = next();
    }
    if (recipePath.empty() || cacheFile.empty()) { std::fprintf(stderr, "[pipeworker] usage: worker --recipe F --cache F\n"); return 2; }
#if !LL_WINDOWS
    // One worker per cache: the lock is held for the process lifetime (released by the kernel on exit).
    if (!lockPath.empty())
    {
        const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) { std::fprintf(stderr, "[pipeworker] another worker holds %s\n", lockPath.c_str()); return 3; }
        char pid[32];
        const int n = std::snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
        if (::ftruncate(fd, 0) == 0 && ::write(fd, pid, (size_t)n) != n) { /* best effort */ }
    }
    ::signal(SIGPIPE, SIG_IGN);              // a finishing worker outlives its reader: progress writes just fail
    // (No PR_SET_PDEATHSIG: it is a per-thread setting that a finishing worker could not drop for
    // the main thread from its stdin reader -- measured: the deliberate finish was killed with the
    // viewer. The stdin pipe's EOF is the parent-death signal instead, on every platform.)
    if (memMB)
    {
        // RLIMIT_DATA, never RLIMIT_AS: drivers reserve huge address ranges they never touch.
        rlimit rl{(rlim_t)memMB << 20, (rlim_t)memMB << 20};
        ::setrlimit(RLIMIT_DATA, &rl);
    }
    ::setpriority(PRIO_PROCESS, 0, 10);      // below the viewer (its render / asset threads win the cores)
#else
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
    // Commands on stdin. EOF without "finish" = the viewer is gone: stop at once (no cleanup:
    // the cache file on disk is always a complete, atomically written one).
    std::thread([finishMax]() {
        char line[64];
        while (std::fgets(line, sizeof(line), stdin))
        {
            if (std::strncmp(line, "finish", 6) == 0)
            {
                s_finishSince = nowMs();
                s_finishing = true;
                std::fputs("finishing\n", stdout);   // (the host waits for this before it lets the viewer exit)
                std::fflush(stdout);
            }
            else if (std::strncmp(line, "quit", 4) == 0) break;
        }
        std::fprintf(stderr, "[pipeworker] viewer gone (stdin closed)%s\n", s_finishing.load() ? "; finishing the recipe" : "; exiting");
        if (!s_finishing.load()) std::_Exit(0);
        // Finishing: bounded.
        while (nowMs() - s_finishSince.load() < (int64_t)finishMax * 1000) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::fprintf(stderr, "[pipeworker] finish time limit reached; exiting\n");
        std::_Exit(0);
    }).detach();

    std::string recipe;
    if (LLFILE* f = LLFile::fopen(recipePath, "rb"))
    {
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && recipe.size() < (512u << 20)) recipe.append(buf, n);
        LLFile::close(f);
    }
    Reader r(recipe);
    char magic[4];
    r.take(magic, 4);
    const uint32_t ver = r.u32();
    const uint32_t vendor = r.u32(), device = r.u32(), driver = r.u32();
    uint8_t uuid[VK_UUID_SIZE];
    r.take(uuid, VK_UUID_SIZE);
    if (r.bad || std::memcmp(magic, kRecipeMagic, 4) != 0 || ver != kRecipeVersion)
    {
        std::fprintf(stderr, "[pipeworker] bad recipe %s\n", recipePath.c_str());
        return 2;
    }
    VkDeviceContext dev;
    dev.setRayTracingRequested(rayTracing);
    dev.setPipelineCacheFile(cacheFile, "worker");
    if (!dev.init(false)) { std::fprintf(stderr, "[pipeworker] device init failed\n"); return 2; }
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(dev.physicalDevice(), &p);
        if (p.vendorID != vendor || p.deviceID != device || p.driverVersion != driver
            || std::memcmp(p.pipelineCacheUUID, uuid, VK_UUID_SIZE) != 0)
        {
            std::fprintf(stderr, "[pipeworker] recipe is for another GPU / driver\n");
            dev.shutdown();
            return 2;
        }
    }
    const VkDevice d = dev.device();
    // Modules (the code stays in `codes`, alive for the whole run: vkNoteShaderModule's rule).
    std::vector<std::vector<uint32_t>> codes;
    std::vector<VkShaderModule> mods;
    const uint32_t nm = r.count(8);
    for (uint32_t i = 0; i < nm && !r.bad; ++i)
    {
        const uint32_t words = r.count(4);
        codes.emplace_back(words);
        r.take(codes.back().data(), (size_t)words * 4);
        const std::string name = r.str();
        if (respin)
            if (VkPipelineRespinFn fn = s_respin.load())
            {
                std::vector<uint32_t> fresh;
                if (fn(name, fresh) && !fresh.empty()) codes.back().swap(fresh);
            }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = codes.back().size() * 4;
        ci.pCode = codes.back().data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (r.bad || codes.back().empty() || vkCreateShaderModule(d, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        if (m) vkNoteShaderModule(m, ci.pCode, ci.codeSize, name.c_str());
        mods.push_back(m);
    }
    std::vector<VkDescriptorSetLayout> sls;
    const uint32_t ns = r.count(8);
    for (uint32_t i = 0; i < ns && !r.bad; ++i)
    {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = r.u32();
        const uint32_t nb = r.count(20);
        std::vector<VkDescriptorSetLayoutBinding> bs(nb);
        std::vector<VkDescriptorBindingFlags> fl(nb);
        for (uint32_t k = 0; k < nb; ++k)
        {
            bs[k].binding = r.u32(); bs[k].descriptorType = (VkDescriptorType)r.u32(); bs[k].descriptorCount = r.u32();
            bs[k].stageFlags = r.u32(); fl[k] = r.u32();
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = nb;
        bf.pBindingFlags = fl.data();
        ci.pNext = &bf;
        ci.bindingCount = nb;
        ci.pBindings = bs.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        if (r.bad || vkCreateDescriptorSetLayout(d, &ci, nullptr, &l) != VK_SUCCESS) l = VK_NULL_HANDLE;
        sls.push_back(l);
    }
    std::vector<VkPipelineLayout> pls;
    const uint32_t np = r.count(8);
    for (uint32_t i = 0; i < np && !r.bad; ++i)
    {
        const uint32_t nsets = r.count(4);
        std::vector<VkDescriptorSetLayout> sets(nsets);
        for (auto& s : sets) { const int32_t k = r.i32(); s = (k >= 0 && (size_t)k < sls.size()) ? sls[k] : VK_NULL_HANDLE; }
        const uint32_t npush = r.count(12);
        std::vector<VkPushConstantRange> push(npush);
        for (auto& p : push) { p.stageFlags = r.u32(); p.offset = r.u32(); p.size = r.u32(); }
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = nsets; ci.pSetLayouts = sets.data();
        ci.pushConstantRangeCount = npush; ci.pPushConstantRanges = push.data();
        VkPipelineLayout l = VK_NULL_HANDLE;
        if (r.bad || vkCreatePipelineLayout(d, &ci, nullptr, &l) != VK_SUCCESS) l = VK_NULL_HANDLE;
        pls.push_back(l);
    }
    struct Item { GraphicsPipelineDesc desc; bool compute; VkShaderModule comp; uint32_t compFlags;
                  std::vector<VkSpecializationMapEntry> e; std::vector<uint8_t> data; VkSpecializationInfo si{}; std::string name;
                  VkPipeline out = VK_NULL_HANDLE; };
    std::vector<Item> items;
    const uint32_t npl = r.count(16);
    items.resize(npl);
    auto mod = [&](int32_t k) { return (k >= 0 && (size_t)k < mods.size()) ? mods[k] : VK_NULL_HANDLE; };
    for (Item& it : items)
    {
        if (r.bad) break;
        it.name = r.str();
        it.compute = r.u32() != 0;
        it.desc.vert = mod(r.i32()); it.desc.frag = mod(r.i32()); it.desc.task = mod(r.i32()); it.desc.mesh = mod(r.i32());
        it.comp = mod(r.i32());
        const int32_t li = r.i32();
        it.compFlags = r.u32();
        readDesc(r, it.desc);
        it.desc.layout = (li >= 0 && (size_t)li < pls.size()) ? pls[li] : VK_NULL_HANDLE;
        const uint32_t ne = r.count(12);
        it.e.resize(ne);
        for (auto& e : it.e) { e.constantID = r.u32(); e.offset = r.u32(); e.size = r.u32(); }
        const uint32_t nd = r.count(1);
        it.data.resize(nd);
        r.take(it.data.data(), nd);
        for (const auto& e : it.e) if ((size_t)e.offset + e.size > it.data.size()) r.bad = true;   // (malformed)
    }
    if (r.bad) { std::fprintf(stderr, "[pipeworker] malformed recipe\n"); dev.shutdown(); return 2; }

    s_workerDev = &dev;
    // The viewer's admission policy, here where the compiles now run (a child without it would
    // just move the #1411 paging into another process): the learned costs (read-only: the viewer
    // owns that file), AMD's 3 GB default per unknown pipeline and its heavy-lane start of 1-2,
    // the process's resident set as the memory probe, and an in-flight cap from the host.
    {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(dev.physicalDevice(), &p2);
        const bool amd = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY;
        const size_t learned = costsPath.empty() ? 0 : vkpipecache::loadCosts(dev.physicalDevice(), costsPath, true);
        vkSetPipelineDefaultCost(amd ? (3072ull << 20) : (512ull << 20));
        const unsigned cap = amd ? (learned ? 2u : 1u) : 0u;
        vkSetPipelineConcurrencyStart(cap);
        vkSetPipelineColdStartCap(cap);
        vkSetPipelineMemProbe(&workerRss);
        if (memCapMB) vkSetPipelineMemCap((uint64_t)memCapMB << 20);
    }
    vkSetPipelineCompileThreads(threads);
    vkSetPipelineEventHook(&workerEvent);
    vkSetPipelineProgressHook(&workerProgress, 1);
    vkSetPipelineCacheWarm(dev.pipelineCacheWarm());
    std::printf("start %zu %u\n", items.size(), threads);
    std::fflush(stdout);
    {
        PipelineBatchScope scope(d);
        for (Item& it : items)
        {
            if (!it.e.empty()) it.si = VkSpecializationInfo{(uint32_t)it.e.size(), it.e.data(), it.data.size(), it.data.data()};
            const VkSpecializationInfo* spec = it.e.empty() ? nullptr : &it.si;
            if (it.compute) createComputePipelineInto(d, it.comp, it.desc.layout, it.compFlags, spec, &it.out, it.name.c_str());
            else
            {
                it.desc.fragSpec = spec;
                createGraphicsPipelineInto(d, it.desc, &it.out, it.name.c_str());
            }
        }
    }   // (the scope's flush compiles them, workers below normal priority -- the process is)
    unsigned made = 0;
    for (Item& it : items)
        if (it.out) { ++made; vkForgetPipeline(it.out); vkDestroyPipeline(d, it.out, nullptr); }
    vkSetPipelineProgressHook(nullptr, 0);
    dev.savePipelineCache("worker done");
    {
        size_t n = 0;
        vkPipelineCacheGetDataGated(d, dev.pipelineCache(), &n, nullptr, "worker done");
        workerSay("saved %u KB %u\n", (unsigned)(n >> 10), 1u);
    }
    std::printf("all %u %u\n", made, (unsigned)items.size());
    std::fflush(stdout);
    s_workerDev = nullptr;
    for (VkPipelineLayout l : pls) if (l) vkDestroyPipelineLayout(d, l, nullptr);
    for (VkDescriptorSetLayout l : sls) if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
    for (VkShaderModule m : mods) if (m) vkDestroyShaderModule(d, m, nullptr);
    dev.shutdown();
    return made == items.size() ? 0 : 1;
}

// ======================================================================= host
std::string vkPipelineWorkerExePath()
{
#if LL_WINDOWS
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (!n) return std::string();
    return ll_convert_wide_to_string(std::wstring(buf, n));
#elif LL_LINUX
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
#else
    return std::string();
#endif
}

namespace
{
bool writeFileAtomic(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (LLFile::close(f) == 0) && ok;
#if LL_WINDOWS
    if (ok) LLFile::remove(path, ENOENT);
#endif
    ok = ok && LLFile::rename(tmp, path) == 0;
    if (!ok) LLFile::remove(tmp, ENOENT);
    return ok;
}

#if !LL_WINDOWS
// A worker of an older session still finishing on the same cache: stop it (only a process whose
// command line says it is one of our workers), so two never write one file.
void stopStaleWorker(const std::string& lockPath)
{
    const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { ::flock(fd, LOCK_UN); ::close(fd); return; }   // nobody holds it
    char buf[32] = {};
    const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    const int pid = n > 0 ? std::atoi(buf) : 0;
#if LL_LINUX
    bool isWorker = false;
    if (pid > 1)
    {
        char path[64], cmd[512] = {};
        std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        if (FILE* c = std::fopen(path, "rb"))
        {
            const size_t k = std::fread(cmd, 1, sizeof(cmd) - 1, c);
            std::fclose(c);
            for (size_t i = 0; i < k; ++i) if (!cmd[i]) cmd[i] = ' ';
            isWorker = std::strstr(cmd, " worker --recipe ") != nullptr;
        }
    }
    if (isWorker) ::kill(pid, SIGTERM);
#endif
    for (int i = 0; i < 40; ++i)   // up to 2 s for it to go (the kernel releases its lock)
    {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { ::flock(fd, LOCK_UN); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::close(fd);
}
#endif
}

bool VkPipelineWorkerHost::start(const std::string& exe, const std::string& recipe, const std::string& cacheFile,
                                 unsigned threads, unsigned memMB, bool rayTracing)
{
    if (mPid) { mWhy = "already running"; return false; }
    if (exe.empty() || recipe.empty() || cacheFile.empty()) { mWhy = "no executable / recipe / cache path"; return false; }
    mCache = cacheFile;
    mRecipePath = cacheFile + ".recipe";
    if (!writeFileAtomic(mRecipePath, recipe)) { mWhy = "cannot write " + mRecipePath; return false; }
    const std::string lockPath = cacheFile + ".lock";
    const std::string th = std::to_string(std::max(1u, threads)), mem = std::to_string(memMB), rt = rayTracing ? "1" : "0";
    // In-flight compile cap: half the process limit (the viewer's own rule is a quarter of RAM; the
    // limit given is half of RAM). The learned costs: the viewer's file next to the cache, read-only.
    const std::string capS = std::to_string(memMB / 2);
    const size_t slash = cacheFile.find_last_of("/\\");
    const std::string costs = (slash == std::string::npos ? std::string() : cacheFile.substr(0, slash + 1)) + "vk_pipeline_costs.txt";
#if LL_WINDOWS
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) { mWhy = "CreatePipe failed"; return false; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + ll_convert_string_to_wide(exe) + L"\" worker --recipe \"" + ll_convert_string_to_wide(mRecipePath)
                     + L"\" --cache \"" + ll_convert_string_to_wide(cacheFile) + L"\" --threads " + ll_convert_string_to_wide(th)
                     + L" --mem-mb " + ll_convert_string_to_wide(mem) + L" --ray-tracing " + ll_convert_string_to_wide(rt) + L" --respin"
                     + L" --mem-cap-mb " + ll_convert_string_to_wide(capS) + L" --costs \"" + ll_convert_string_to_wide(costs) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); mWhy = "CreateProcess failed"; return false; }
    // Memory bound (job object; NOT kill-on-close: a deliberate finish outlives the viewer -- the
    // stdin pipe's EOF ends it otherwise).
    if (memMB)
    {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            li.ProcessMemoryLimit = (SIZE_T)memMB << 20;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
            AssignProcessToJobObject(job, pi.hProcess);
            mJob = (long long)(intptr_t)job;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    mPid = (long long)(intptr_t)pi.hProcess;
    mInH = inW;
    mOutH = outR;
#else
    stopStaleWorker(lockPath);
    // Close-on-exec from creation: a process another thread spawns meanwhile must not inherit our
    // end of the worker's stdin (it would keep the pipe open past a viewer crash). adddup2 clears
    // the flag on the child's fds 0 / 1.
    int in[2], out[2];
#if LL_LINUX
    if (::pipe2(in, O_CLOEXEC) != 0) { mWhy = "pipe failed"; return false; }
    if (::pipe2(out, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); mWhy = "pipe failed"; return false; }
#else
    if (::pipe(in) != 0) { mWhy = "pipe failed"; return false; }
    if (::pipe(out) != 0) { ::close(in[0]); ::close(in[1]); mWhy = "pipe failed"; return false; }
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::vector<std::string> args = {exe, "worker", "--recipe", mRecipePath, "--cache", cacheFile, "--lock", lockPath,
                                     "--threads", th, "--mem-mb", mem, "--ray-tracing", rt, "--respin", "--mem-cap-mb", capS, "--costs", costs};
    std::vector<char*> av;
    for (std::string& a : args) av.push_back(&a[0]);
    av.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) { ::close(in[1]); ::close(out[0]); mWhy = std::string("posix_spawn: ") + std::strerror(rc); return false; }
    ::fcntl(in[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(out[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(out[0], F_SETFL, ::fcntl(out[0], F_GETFL) | O_NONBLOCK);
    ::signal(SIGPIPE, SIG_IGN);   // (a write to a dead worker's stdin must not end the viewer)
    mPid = pid;
    mIn = in[1];
    mOut = out[0];
#endif
    mExited = false;
    mExitCode = -1;
    mDone = 0;
    return true;
}

void VkPipelineWorkerHost::readOutput()
{
    char buf[4096];
    for (;;)
    {
#if LL_WINDOWS
        DWORD avail = 0, n = 0;
        if (!mOutH || !PeekNamedPipe((HANDLE)mOutH, nullptr, 0, nullptr, &avail, nullptr) || !avail) return;
        if (!ReadFile((HANDLE)mOutH, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || !n) return;
#else
        if (mOut < 0) return;
        const ssize_t n = ::read(mOut, buf, sizeof(buf));
        if (n <= 0) return;
#endif
        mLine.append(buf, (size_t)n);
        size_t eol;
        while ((eol = mLine.find('\n')) != std::string::npos)
        {
            unsigned a = 0, b = 0;
            if (std::sscanf(mLine.c_str(), "done %u %u", &a, &b) == 2) mDone = a;
            else if (mLine.compare(0, 9, "finishing") == 0) mFinishAck = true;
            mLine.erase(0, eol + 1);
        }
        if (mLine.size() > 4096) mLine.clear();   // (bounded)
    }
}

bool VkPipelineWorkerHost::poll()
{
    if (!mPid) return mExited;
    readOutput();
    if (mExited) return true;
#if LL_WINDOWS
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeProcess((HANDLE)(intptr_t)mPid, &code) && code != STILL_ACTIVE)
    {
        mExited = true;
        mExitCode = (int)code;
    }
#else
    int st = 0;
    if (::waitpid((pid_t)mPid, &st, WNOHANG) == (pid_t)mPid)
    {
        mExited = true;
        mExitCode = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
#endif
    if (mExited) readOutput();
    return mExited;
}

void VkPipelineWorkerHost::stop(bool finish)
{
    if (!mPid) return;
    poll();
    if (!mExited)
    {
#if LL_WINDOWS
        if (finish && mInH) { DWORD w = 0; WriteFile((HANDLE)mInH, "finish\n", 7, &w, nullptr); }
        if (mInH) { CloseHandle((HANDLE)mInH); mInH = nullptr; }   // (EOF: a non-finishing worker exits)
        if (!finish)
        {
            if (WaitForSingleObject((HANDLE)(intptr_t)mPid, 2000) == WAIT_TIMEOUT) TerminateProcess((HANDLE)(intptr_t)mPid, 1);
        }
#else
        if (finish && mIn >= 0 && ::write(mIn, "finish\n", 7) != 7) finish = false;
        // A finishing worker must have dropped its parent-death signal before we may exit: wait
        // (bounded) for its acknowledgement.
        for (int i = 0; finish && !mFinishAck && i < 100 && !mExited; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            poll();
        }
        if (mIn >= 0) { ::close(mIn); mIn = -1; }   // (EOF: a non-finishing worker exits)
        if (!finish)
        {
            for (int i = 0; i < 40 && !poll(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!mExited) { ::kill((pid_t)mPid, SIGKILL); ::waitpid((pid_t)mPid, nullptr, 0); }
        }
#endif
    }
#if LL_WINDOWS
    if (mInH) CloseHandle((HANDLE)mInH);
    if (mOutH) CloseHandle((HANDLE)mOutH);
    CloseHandle((HANDLE)(intptr_t)mPid);
    if (mJob) CloseHandle((HANDLE)(intptr_t)mJob);
    mInH = mOutH = nullptr;
    mJob = 0;
#else
    if (mIn >= 0) ::close(mIn);
    if (mOut >= 0) ::close(mOut);
    mIn = mOut = -1;
#endif
    mPid = 0;
}

void VkPipelineWorkerHost::killHard()
{
    if (!mPid) return;
#if LL_WINDOWS
    TerminateProcess((HANDLE)(intptr_t)mPid, 9);
    WaitForSingleObject((HANDLE)(intptr_t)mPid, 5000);
#else
    ::kill((pid_t)mPid, SIGKILL);
    int st = 0;
    ::waitpid((pid_t)mPid, &st, 0);
#endif
    mExited = true;
    mExitCode = 137;
    stop(false);
}

VkPipelineWorkerHost::~VkPipelineWorkerHost() { stop(false); }

VkPipelineCache vkPipelineWorkerLoadCache(VkDevice dev, VkPhysicalDevice phys, const std::string& file)
{
    std::vector<uint8_t> blob;
    if (!vkpipecache::load(phys, file, "worker", blob) || blob.empty()) return VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    VkPipelineCache c = VK_NULL_HANDLE;
    if (vkPipelineCacheCreateGated(dev, &ci, &c, "worker cache load") != VK_SUCCESS) c = VK_NULL_HANDLE;
    return c;
}

bool vkPipelineWorkerMerge(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& file, bool consume)
{
    if (!cache) return false;
    VkPipelineCache wc = vkPipelineWorkerLoadCache(dev, phys, file);
    if (!wc) return false;
    const bool ok = vkPipelineCacheMergeGated(dev, cache, 1, &wc, "worker cache merge") == VK_SUCCESS;
    vkPipelineCacheDestroyGated(dev, wc, "worker cache merge");
    if (ok && consume) LLFile::remove(file, ENOENT);
    return ok;
}

// ======================================================================= P5
void vkPipelineWorkerSetRespin(VkPipelineRespinFn fn) { s_respin.store(fn); }

namespace
{
std::mutex s_earlyMutex;
VkPipelineWorkerHost* s_early = nullptr;
}

void vkPipelineWorkerSaveRecipe(const std::string& cacheFile, const std::string& recipe)
{
    if (cacheFile.empty() || recipe.empty()) return;
    const std::string path = cacheFile + ".recipe";
    if (LLFILE* f = LLFile::fopen(path, "rb"))
    {
        std::string old;
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && old.size() <= recipe.size()) old.append(buf, n);
        LLFile::close(f);
        if (old == recipe) return;   // unchanged: no write
    }
    writeFileAtomic(path, recipe);
}

void vkPipelineEarlyWorkerStart(const std::string& cacheFile, bool rayTracing)
{
    if (cacheFile.empty() || !LLFile::isfile(cacheFile + ".recipe")) return;
    std::string recipe;
    if (LLFILE* f = LLFile::fopen(cacheFile + ".recipe", "rb"))
    {
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && recipe.size() < (512u << 20)) recipe.append(buf, n);
        LLFile::close(f);
    }
    std::lock_guard<std::mutex> lk(s_earlyMutex);
    if (s_early || recipe.empty()) return;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    s_early = new VkPipelineWorkerHost();
    // (the host re-writes the recipe file it was read from: same bytes)
    if (!s_early->start(vkPipelineWorkerExePath(), recipe, cacheFile + ".worker", std::max(1u, hw / 4), 0, rayTracing))
    {
        delete s_early;
        s_early = nullptr;
    }
}

VkPipelineWorkerHost* vkPipelineEarlyWorkerTake()
{
    std::lock_guard<std::mutex> lk(s_earlyMutex);
    VkPipelineWorkerHost* h = s_early;
    s_early = nullptr;
    return h;
}
