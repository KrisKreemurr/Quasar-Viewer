/**
 * @file vk_pipeworker.h
 * @brief #1411 P4: OUT-OF-PROCESS PIPELINE COMPILE WORKER (+ P5 first-launch precompile).
 *
 * WHY: a driver's shader compiler can need GBs per heavy pipeline (AMD proprietary: ~3-4 GB on a
 * 32 GB box, the whole system paged for minutes) and quitting the viewer abandoned minutes of
 * compile work. The heavy (upgrade) group is compiled by a CHILD process instead: the same
 * executable in worker mode (`<exe> worker ...`; the viewer's main and slpipe-bench dispatch to
 * vkPipelineWorkerMain). It creates the same Vulkan device (VkDeviceContext, headless), rebuilds
 * the pipelines from a RECIPE (exact SPIR-V, set / pipeline layouts, every desc field and the
 * specialization data, so the driver's pipeline-cache keys match the viewer's), compiles them
 * through the same scheduler into its own VkPipelineCache and writes it to a file after every
 * heavy completion (atomic, vk_pipecache.h format). The viewer then creates the same pipelines
 * from that cache: cache hits, milliseconds. A driver out-of-memory or crash kills the child, not
 * the viewer; the viewer's RSS stays its own.
 *
 * LIFETIME (no leaked processes):
 *  - the worker reads commands on stdin; stdin at EOF (the viewer exited or crashed: its end of
 *    the pipe closed) ends it at once, unless the viewer DELIBERATELY sent "finish" first
 *    (shutdown with heavy work left): then it completes the recipe -- at most --finish-max-sec --
 *    and exits, persisting the cache for the next launch;
 *  - the pipe's EOF is the parent-death signal on every platform (no PR_SET_PDEATHSIG: per thread,
 *    it killed a deliberate finish); Windows: the job object only bounds memory [Windows UNVERIFIED];
 *  - one worker per cache directory: a lock file with its pid; a new viewer stops a finishing
 *    worker of an older session before it starts its own (the cache file is written atomically,
 *    so stopping it never corrupts it);
 *  - memory: RLIMIT_DATA (Linux; never RLIMIT_AS: drivers map huge address ranges) /
 *    JOB_OBJECT_LIMIT_PROCESS_MEMORY (Windows), from --mem-mb; an out-of-memory compile inside
 *    the worker takes the scheduler's normal #672 requeue path there.
 *
 * RECIPE (binary, native endianness -- the same machine reads it):
 *   "SLPR" u32 version | u32 vendorID deviceID driverVersion | u8[16] pipelineCacheUUID
 *   u32 nModules  { u32 words; u32 code[words]; str name }
 *   u32 nSetLayouts { u32 flags; u32 n; { u32 binding type count stages bindingFlags } }
 *   u32 nPipeLayouts { u32 nSets; i32 set[nSets]; u32 nPush; { u32 stages offset size } }
 *   u32 nPipelines { str name; u32 compute; i32 vert frag task mesh comp; i32 layout; u32 compFlags;
 *                    GraphicsPipelineDesc fields (vk_pipeworker.cpp writeDesc); spec entries + data }
 *   (str = u32 length + bytes)
 */
#pragma once

#include "vk_common.h"

#include <cstdint>
#include <string>
#include <vector>

struct PipelineJob;

// Worker mode entry (argv[0] = "worker"): the viewer binary and slpipe-bench both dispatch here.
//   worker --recipe <file> --cache <file> [--threads N] [--mem-mb N] [--finish-max-sec N]
//          [--ray-tracing 0|1] [--lock <file>] [--respin]
// (An installer / updater may run exactly this, with --respin, on the previous session's
//  <cache>.recipe: the first launch then finds a warm cache.)
// Exit 0 = every pipeline compiled (or the recipe empty); 2 = bad arguments / recipe / device.
int vkPipelineWorkerMain(int argc, char** argv);

// ---- viewer side ----
// The recipe of an UNSTARTED job (pipelineBatchTakeUpgrade): empty when a pipeline of it cannot be
// described (a layout never registered -- vkPipelineNoteLayouts -- or immutable samplers).
std::string vkPipelineJobRecipe(const PipelineJob* job, VkPhysicalDevice phys);
// Layout registry the recipes read (vk_descpool.cpp vkdCreateSetLayout / vkdCreatePipelineLayout
// call these after a successful create). Bounded (cleared wholesale past 4096 entries).
void vkPipelineNoteSetLayout(VkDescriptorSetLayout l, const VkDescriptorSetLayoutCreateInfo* ci);
void vkPipelineNotePipelineLayout(VkPipelineLayout l, const VkPipelineLayoutCreateInfo* ci);
void vkPipelineForgetSetLayout(VkDescriptorSetLayout l);

// One child worker. Not copyable; the destructor stops a worker that is not finishing.
class VkPipelineWorkerHost
{
public:
    VkPipelineWorkerHost() = default;
    ~VkPipelineWorkerHost();
    VkPipelineWorkerHost(const VkPipelineWorkerHost&) = delete;
    VkPipelineWorkerHost& operator=(const VkPipelineWorkerHost&) = delete;
    // Writes the recipe next to `cacheFile` and starts `exe worker ...`. False (logged in why())
    // when it cannot (no exe path, spawn failed, a recipe it cannot write).
    bool start(const std::string& exe, const std::string& recipe, const std::string& cacheFile, unsigned threads,
               unsigned memMB, bool rayTracing);
    // Non-blocking: true once the child exited (ok() = exit 0, every pipeline compiled).
    bool poll();
    bool running() const { return mPid != 0 && !mExited; }
    bool ok() const { return mExited && mExitCode == 0; }
    unsigned done() const { return mDone; }        // "done" lines read so far
    // Shutdown: finish = the child completes the recipe in the background (bounded) and persists
    // its cache; otherwise it is stopped now. Either way the host forgets it.
    void stop(bool finish);
    // Testing (slpipe-bench): kill -9 now (the kill-mid-compile check).
    void killHard();
    const std::string& why() const { return mWhy; }
    const std::string& cacheFile() const { return mCache; }
private:
    void readOutput();
    long long mPid = 0;            // pid / process handle value (0 = none)
    long long mJob = 0;            // Windows job object handle
    int mIn = -1, mOut = -1;       // our ends of the child's stdin / stdout (POSIX fds; Windows: handles in mInH/mOutH)
    void* mInH = nullptr;
    void* mOutH = nullptr;
    bool mExited = false;
    bool mFinishAck = false;       // the worker confirmed "finishing" (parent-death signal dropped)
    int mExitCode = -1;
    unsigned mDone = 0;
    std::string mLine, mWhy, mCache, mRecipePath;
};

// The worker's cache file merged into `cache` (validated like the main cache: device, driver,
// UUID); true when something was merged. Call before compiles start on `cache` (vkMergePipelineCaches'
// destination is externally synchronized). The file is removed after a successful merge when
// `consume` (its entries now live in the main cache, which the viewer saves).
bool vkPipelineWorkerMerge(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& file,
                           bool consume);
// The worker's cache file as a new VkPipelineCache (VK_NULL_HANDLE when absent / stale).
VkPipelineCache vkPipelineWorkerLoadCache(VkDevice dev, VkPhysicalDevice phys, const std::string& file);
// This process's executable path (UTF-8; "" if unknown).
std::string vkPipelineWorkerExePath();

// ---- P5: first-launch / install-time precompile ----
// RESPIN: `worker --respin` replaces each recipe module's SPIR-V with THIS binary's module of the
// same name when it has one (the viewer: its heavy world modules, llcleanrenderervk.cpp; the bench:
// its .spv directory). A recipe saved by the previous viewer build (<cache>.recipe, written at
// every launch that compiled an upgrade group) thereby describes the NEW build's pipelines --
// layouts and states rarely change between builds, and a pipeline that did simply misses.
using VkPipelineRespinFn = bool (*)(const std::string& name, std::vector<uint32_t>& code);
void vkPipelineWorkerSetRespin(VkPipelineRespinFn fn);
// The worker started EARLY (LLVulkanContext, right after device creation, before login) from the
// saved recipe -- the login screen's seconds become compile time; the renderer adopts it for its
// upgrade group (vkPipelineEarlyWorkerTake) instead of starting another. nullptr when none.
void vkPipelineEarlyWorkerStart(const std::string& cacheFile, bool rayTracing);
VkPipelineWorkerHost* vkPipelineEarlyWorkerTake();
// The recipe saved for the next launch (only rewritten when it changed).
void vkPipelineWorkerSaveRecipe(const std::string& cacheFile, const std::string& recipe);
