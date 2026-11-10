#include "linden_common.h"

#include "llvktextureprepservice.h"

#include "lltexturedata.h"
#include "threadpool.h"
#include "vk_device.h"
#include "llvktextureprepworker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#if LL_WINDOWS
#include "llwin32headers.h"
#elif defined(__linux__)
#include <time.h>
#endif

namespace
{
uint64_t texturePrepMonotonicNs() noexcept
{
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return value > 0 ? static_cast<uint64_t>(value) : 0;
}

bool texturePrepThreadCpuNs(uint64_t& value) noexcept
{
#if LL_WINDOWS
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER kernelTime{}, userTime{};
    kernelTime.LowPart = kernel.dwLowDateTime; kernelTime.HighPart = kernel.dwHighDateTime;
    userTime.LowPart = user.dwLowDateTime; userTime.HighPart = user.dwHighDateTime;
    const uint64_t limitValue = (std::numeric_limits<uint64_t>::max)();
    if (kernelTime.QuadPart > limitValue - userTime.QuadPart) return false;
    const uint64_t ticks = kernelTime.QuadPart + userTime.QuadPart;
    if (ticks > limitValue / 100ull) return false;
    value = ticks * 100ull;
    return true;
#elif defined(__linux__) && defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0 || now.tv_sec < 0
        || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return false;
    const uint64_t seconds = static_cast<uint64_t>(now.tv_sec);
    const uint64_t limitValue = (std::numeric_limits<uint64_t>::max)();
    if (seconds > limitValue / 1000000000ull) return false;
    const uint64_t ns = seconds * 1000000000ull;
    if (static_cast<uint64_t>(now.tv_nsec) > limitValue - ns) return false;
    value = ns + static_cast<uint64_t>(now.tv_nsec);
    return true;
#else
    (void)value;
    return false;
#endif
}

void addSaturating(uint64_t& total, uint64_t amount) noexcept
{
    const uint64_t limitValue = (std::numeric_limits<uint64_t>::max)();
    total = amount > limitValue - total ? limitValue : total + amount;
}

void destroyBufferNoThrow(VkDeviceContext* context, GpuBuffer& buffer) noexcept
{
    if (!context || !buffer.buffer) return;
    try { context->destroyBuffer(buffer); }
    catch (...) { buffer = GpuBuffer{}; }
}
}

struct LLVKTexturePrepService::Impl
{
    struct Work
    {
        std::atomic<uint8_t> state{(uint8_t)State::Queued};
        std::atomic<bool> cancel{false};
        uint64_t queuedNs = 0, startedNs = 0, readyNs = 0;
        uint64_t workerCpuStartNs = 0, workerCpuEndNs = 0;
        bool workerCpuStartValid = false, workerCpuEndValid = false;
        FailureReason failureReason = FailureReason::None;
        ClientDomain domain = ClientDomain::World;
        LLVKTexturePrep::OutputLayout output{};
    };

    struct Slot
    {
        uint32_t generation = 0;
        bool used = false;
        bool releaseRequested = false;
        bool recordPending = false;
        bool quarantined = false;
        bool retired = false;
        bool inputReleased = false;
        bool metricsCollected = false;
        uint64_t key = 0, serial = 0;
        uint32_t sourceGeneration = 0;
        ClientDomain domain = ClientDomain::World;
        uint64_t inputBytes = 0, stageBytes = 0, scratchBytes = 0;
        uint64_t retirementSerial = 0;
        LLVKTexturePrep::Priority priority = LLVKTexturePrep::Priority::Background;
        LLVKTexturePrep::Recipe recipe{};
        LLVKTexturePrep::SourceSnapshot source;
        LLVKCpuLease lease;
        std::shared_ptr<Work> work;
        GpuBuffer stage{};
        std::unique_ptr<uint8_t[]> scratch;
    };

    struct CachedStage
    {
        GpuBuffer buffer{};
        uint64_t lastUsed = 0;
    };

    explicit Impl(VkDeviceContext* device) : context(device) {}
    VkDeviceContext* context = nullptr; // main-thread owner only; never captured by workers
    mutable std::mutex mutex;
    std::array<Slot, kTicketCount> slots{};
    std::array<CachedStage, kTicketCount> cachedStages{};
    std::unique_ptr<LL::ThreadPool> pool;
    uint64_t inputUsed = 0, stageUsed = 0, activeStageUsed = 0, scratchUsed = 0;
    uint64_t stageCacheClock = 0;
    IntervalStats interval{};
    bool accepting = true;
    bool closing = false;

    Slot* find(Ticket ticket) noexcept
    {
        if (!ticket.valid()) return nullptr;
        Slot& slot = slots[ticket.slot];
        return slot.used && slot.generation == ticket.generation ? &slot : nullptr;
    }
    const Slot* find(Ticket ticket) const noexcept
    {
        if (!ticket.valid()) return nullptr;
        const Slot& slot = slots[ticket.slot];
        return slot.used && slot.generation == ticket.generation ? &slot : nullptr;
    }
    static State stateOf(const Slot& slot) noexcept
    {
        return slot.work ? (State)slot.work->state.load(std::memory_order_acquire) : State::Invalid;
    }
    static bool workerDone(State state) noexcept
    {
        return state == State::Ready || state == State::Recorded || state == State::Quarantined
            || state == State::Submitted
            || state == State::Failed || state == State::Cancelled;
    }
    static bool sameRecipe(const LLVKTexturePrep::Recipe& a,
                           const LLVKTexturePrep::Recipe& b) noexcept
    {
        return a.kind == b.kind && a.targetWidth == b.targetWidth && a.targetHeight == b.targetHeight
            && a.targetLevels == b.targetLevels && a.payloadLevels == b.payloadLevels
            && a.residentTailLevels == b.residentTailLevels
            && a.targetStorageFormat == b.targetStorageFormat
            && a.preparedStorageFormat == b.preparedStorageFormat && a.encode == b.encode
            && a.trialEncode == b.trialEncode && a.alphaCutoff == b.alphaCutoff && a.srgb == b.srgb
            && a.uiFormat == b.uiFormat;
    }
    void collectMetrics(Slot& slot) noexcept
    {
        if (slot.metricsCollected || !slot.work) return;
        const State result = stateOf(slot);
        if (!workerDone(result)) return;
        slot.metricsCollected = true;
        addSaturating(interval.completedJobs, 1);
        const Work& work = *slot.work;
        switch (result)
        {
        case State::Ready: case State::Recorded: case State::Quarantined: case State::Submitted:
            addSaturating(interval.workerReadyJobs, 1);
            break;
        case State::Failed:
            addSaturating(interval.workerFailedJobs, 1);
            if (work.domain == ClientDomain::Ui) addSaturating(interval.uiWorkerFailedJobs, 1);
            else addSaturating(interval.worldWorkerFailedJobs, 1);
            switch (work.failureReason)
            {
            case FailureReason::UiInputRejected: addSaturating(interval.workerUiRejects, 1); break;
            case FailureReason::PixelInputRejected: addSaturating(interval.workerPixelRejects, 1); break;
            case FailureReason::CompressedInputRejected: addSaturating(interval.workerCompressedRejects, 1); break;
            case FailureReason::OutputLayoutRejected: addSaturating(interval.workerOutputRejects, 1); break;
            case FailureReason::Exception: addSaturating(interval.workerExceptions, 1); break;
            case FailureReason::None: break;
            }
            break;
        case State::Cancelled:
            addSaturating(interval.workerCancelledJobs, 1);
            break;
        default:
            break;
        }
        if (work.queuedNs && work.startedNs && work.startedNs >= work.queuedNs)
            addSaturating(interval.workerQueueWaitNs, work.startedNs - work.queuedNs);
        else
            addSaturating(interval.workerTimingUnavailable, 1);
        if (work.startedNs && work.readyNs && work.readyNs >= work.startedNs)
            addSaturating(interval.workerElapsedNs, work.readyNs - work.startedNs);
        else
            addSaturating(interval.workerTimingUnavailable, 1);
        if (work.workerCpuStartValid && work.workerCpuEndValid
            && work.workerCpuEndNs >= work.workerCpuStartNs)
        {
            addSaturating(interval.workerThreadCpuNs,
                          work.workerCpuEndNs - work.workerCpuStartNs);
            addSaturating(interval.workerCpuSamples, 1);
        }
        else
            addSaturating(interval.workerCpuUnavailable, 1);
    }
    void releaseInput(Slot& slot) noexcept
    {
        if (!workerDone(stateOf(slot))) return;
        collectMetrics(slot);
        if (slot.inputReleased) return;
        slot.source = LLVKTexturePrep::SourceSnapshot{};
        slot.lease.reset(); // main-thread lease release only, after acquire of worker completion
        inputUsed -= std::min(inputUsed, slot.inputBytes);
        slot.inputBytes = 0;
        slot.scratch.reset();
        scratchUsed -= std::min(scratchUsed, slot.scratchBytes);
        slot.scratchBytes = 0;
        slot.inputReleased = true;
    }
    int findCachedStage(VkDeviceSize required, uint64_t activeLimit) const noexcept
    {
        int best = -1;
        for (uint32_t i = 0; i < cachedStages.size(); ++i)
        {
            const GpuBuffer& candidate = cachedStages[i].buffer;
            if (!candidate.buffer || !candidate.mapped || candidate.size < required
                || activeStageUsed > activeLimit
                || candidate.allocationSize > activeLimit - activeStageUsed)
                continue;
            if (best < 0 || candidate.allocationSize < cachedStages[(uint32_t)best].buffer.allocationSize)
                best = (int)i;
        }
        return best;
    }
    void destroyCachedStage(uint32_t index) noexcept
    {
        if (index >= cachedStages.size()) return;
        CachedStage& cached = cachedStages[index];
        if (!cached.buffer.buffer) return;
        const uint64_t bytes = cached.buffer.allocationSize;
        destroyBufferNoThrow(context, cached.buffer);
        stageUsed -= std::min(stageUsed, bytes);
        cached.lastUsed = 0;
    }
    void makeStageRoom(VkDeviceSize required) noexcept
    {
        while (stageUsed > kStageLimit || required > kStageLimit - stageUsed)
        {
            uint32_t oldest = (uint32_t)cachedStages.size();
            for (uint32_t i = 0; i < cachedStages.size(); ++i)
            {
                if (!cachedStages[i].buffer.buffer) continue;
                if (oldest == cachedStages.size()
                    || cachedStages[i].lastUsed < cachedStages[oldest].lastUsed)
                    oldest = i;
            }
            if (oldest == cachedStages.size()) break;
            destroyCachedStage(oldest);
        }
    }
    void evictCachedStages() noexcept
    {
        for (uint32_t i = 0; i < cachedStages.size(); ++i)
            destroyCachedStage(i);
    }
    void keepStage(GpuBuffer& buffer) noexcept
    {
        if (!buffer.buffer) return;
        for (CachedStage& cached : cachedStages)
        {
            if (cached.buffer.buffer) continue;
            cached.buffer = buffer;
            buffer = GpuBuffer{};
            if (stageCacheClock != (std::numeric_limits<uint64_t>::max)()) ++stageCacheClock;
            cached.lastUsed = stageCacheClock;
            return;
        }
        const uint64_t bytes = buffer.allocationSize;
        destroyBufferNoThrow(context, buffer);
        stageUsed -= std::min(stageUsed, bytes);
    }
    void freeSlot(Slot& slot) noexcept
    {
        const State state = stateOf(slot);
        if (!workerDone(state) || slot.recordPending || slot.quarantined || slot.retirementSerial) return;
        releaseInput(slot);
        activeStageUsed -= std::min(activeStageUsed, slot.stageBytes);
        keepStage(slot.stage);
        slot.stageBytes = 0;
        const uint32_t generation = slot.generation;
        slot = Slot{};
        slot.generation = generation;
        slot.retired = generation == (std::numeric_limits<uint32_t>::max)();
    }
};

LLVKTexturePrepService::LLVKTexturePrepService(VkDeviceContext* context) noexcept
{
    try { mImpl.reset(new Impl(context)); }
    catch (...) { mImpl.reset(); }
}

LLVKTexturePrepService::~LLVKTexturePrepService()
{
    shutdown();
}

LLVKTexturePrep::QueueResult LLVKTexturePrepService::submit(
    uint64_t key, LLVKTexturePrep::SourceSnapshot&& source,
    const LLVKTexturePrep::Recipe& recipe, LLVKTexturePrep::Priority priority,
    LLVKCpuLease&& mainLease, Ticket& ticket, ClientDomain domain) try
{
    using namespace LLVKTexturePrep;
    static_assert(std::is_nothrow_move_assignable<SourceSnapshot>::value,
                  "accepted worker source must transfer into its fixed ticket without throwing");
    static_assert(std::is_nothrow_move_assignable<LLVKCpuLease>::value,
                  "accepted CPU lease must transfer into its fixed ticket without throwing");
    ticket = {};
    if (!mImpl || !key || !source.valid() || !mImpl->context || !mainLease.get())
        return QueueResult::Ineligible;

    WorkspaceSize workspace;
    if (!workspaceSize(source, recipe, workspace) || !workspace.outputBytes)
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        if (domain == ClientDomain::Ui) addSaturating(mImpl->interval.uiPreflightRejectedJobs, 1);
        else addSaturating(mImpl->interval.worldPreflightRejectedJobs, 1);
        return QueueResult::Ineligible;
    }
    if (workspace.outputBytes > (std::numeric_limits<VkDeviceSize>::max)()
        || workspace.scratchBytes > (std::numeric_limits<size_t>::max)()
        || source.retainedBackingBytes > kInputLimit || workspace.outputBytes > kStageLimit
        || workspace.scratchBytes > kScratchLimit)
        return QueueResult::Ineligible;
    const uint64_t sourceCharge = source.retainedBackingBytes;
    constexpr uint64_t inputMetadata = sizeof(SourceSnapshot) + sizeof(Impl::Slot) + sizeof(Impl::Work);
    if (sourceCharge > (std::numeric_limits<uint64_t>::max)() - inputMetadata)
        return QueueResult::Ineligible;
    const uint64_t inputCharge = sourceCharge + inputMetadata;

    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (!mImpl->accepting || mImpl->closing) return QueueResult::Deferred;
    for (uint32_t i = 0; i < kTicketCount; ++i)
    {
        Impl::Slot& existing = mImpl->slots[i];
        if (!existing.used || existing.key != key || existing.domain != domain) continue;
        if (existing.serial == source.serial && existing.sourceGeneration == source.generation
            && existing.priority == priority && Impl::sameRecipe(existing.recipe, recipe)
            && !existing.releaseRequested
            && Impl::stateOf(existing) != State::Failed
            && Impl::stateOf(existing) != State::Cancelled
            && Impl::stateOf(existing) != State::Quarantined)
        {
            ticket.slot = (uint16_t)i;
            ticket.generation = existing.generation;
            return QueueResult::AlreadyQueued;
        }
        existing.releaseRequested = true;
        if (existing.work) existing.work->cancel.store(true, std::memory_order_release);
        mImpl->releaseInput(existing);
        mImpl->freeSlot(existing);
    }

    uint32_t slotIndex = kTicketCount;
    if (priority == Priority::SelfOrHud)
    {
        for (uint32_t i = kTicketCount - kPriorityReserve; i < kTicketCount; ++i)
            if (!mImpl->slots[i].used && !mImpl->slots[i].retired
                && mImpl->slots[i].generation != (std::numeric_limits<uint32_t>::max)())
            { slotIndex = i; break; }
    }
    else if (priority == Priority::Visible)
    {
        for (uint32_t i = kTicketCount - kPriorityReserve - kVisibleReserve;
             i < kTicketCount - kPriorityReserve; ++i)
            if (!mImpl->slots[i].used && !mImpl->slots[i].retired
                && mImpl->slots[i].generation != (std::numeric_limits<uint32_t>::max)())
            { slotIndex = i; break; }
    }
    if (slotIndex == kTicketCount)
    {
        const uint32_t ordinaryEnd = priority == Priority::SelfOrHud
            ? kTicketCount : priority == Priority::Visible
            ? kTicketCount - kPriorityReserve
            : kTicketCount - kPriorityReserve - kVisibleReserve;
        for (uint32_t i = 0; i < ordinaryEnd; ++i)
            if (!mImpl->slots[i].used && !mImpl->slots[i].retired
                && mImpl->slots[i].generation != (std::numeric_limits<uint32_t>::max)())
            { slotIndex = i; break; }
    }
    if (slotIndex == kTicketCount) return QueueResult::Deferred;
    const uint64_t stageLimit = priority == Priority::SelfOrHud
        ? kStageLimit : kStageLimit - kPriorityStageReserve
            - (priority == Priority::Visible ? 0 : kVisibleStageReserve);
    const uint64_t inputLimit = priority == Priority::SelfOrHud
        ? kInputLimit : kInputLimit - kPriorityInputReserve
            - (priority == Priority::Visible ? 0 : kVisibleInputReserve);
    const uint64_t scratchLimit = priority == Priority::SelfOrHud
        ? kScratchLimit : kScratchLimit - kPriorityScratchReserve
            - (priority == Priority::Visible ? 0 : kVisibleScratchReserve);
    if (mImpl->inputUsed > inputLimit || inputCharge > inputLimit - mImpl->inputUsed
        || mImpl->activeStageUsed > stageLimit
        || workspace.outputBytes > stageLimit - mImpl->activeStageUsed
        || mImpl->scratchUsed > scratchLimit
        || workspace.scratchBytes > scratchLimit - mImpl->scratchUsed)
        return QueueResult::Deferred;

    Impl::Slot& slot = mImpl->slots[slotIndex];
    uint32_t generation = slot.generation + 1u;
    if (!generation) generation = 1;
    std::shared_ptr<Impl::Work> work;
    try { work = std::make_shared<Impl::Work>(); }
    catch (...) { return QueueResult::Deferred; }
    work->domain = domain;
    std::unique_ptr<uint8_t[]> scratch;
    if (workspace.scratchBytes)
    {
        scratch.reset(new uint8_t[workspace.scratchBytes]);
        if (!scratch) return QueueResult::Deferred;
    }
    GpuBuffer stage;
    const int cachedStageIndex = mImpl->findCachedStage(workspace.outputBytes, stageLimit);
    GpuBuffer* stageBuffer = cachedStageIndex >= 0
        ? &mImpl->cachedStages[(uint32_t)cachedStageIndex].buffer : &stage;
    if (cachedStageIndex < 0)
    {
        if (mImpl->stageUsed > kStageLimit
            || workspace.outputBytes > kStageLimit - mImpl->stageUsed)
            mImpl->makeStageRoom(workspace.outputBytes);
        if (mImpl->stageUsed > kStageLimit
            || workspace.outputBytes > kStageLimit - mImpl->stageUsed)
            return QueueResult::Deferred;

        const uint64_t laneRemaining = stageLimit - mImpl->activeStageUsed;
        const uint64_t totalRemaining = kStageLimit - mImpl->stageUsed;
        const VkDeviceSize allocationLimit = (VkDeviceSize)std::min(laneRemaining, totalRemaining);
        uint64_t stageCpuStartNs = 0, stageCpuEndNs = 0;
        const bool stageCpuStartValid = texturePrepThreadCpuNs(stageCpuStartNs);
        addSaturating(mImpl->interval.stageAllocationCalls, 1);
        try
        {
            VkDeviceContext::SoftAllocScope soft(mImpl->context);
            VkDeviceContext::NoReliefScope noRelief;
            VkMemScope memcat(VKMEM_STAGING);
            stage = mImpl->context->createBufferBounded((VkDeviceSize)workspace.outputBytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                allocationLimit, false, "VKTexturePrepService");
        }
        catch (...)
        {
            if (stageCpuStartValid && texturePrepThreadCpuNs(stageCpuEndNs)
                && stageCpuEndNs >= stageCpuStartNs)
                addSaturating(mImpl->interval.stageAllocationThreadCpuNs,
                              stageCpuEndNs - stageCpuStartNs);
            else
                addSaturating(mImpl->interval.stageAllocationCpuUnavailable, 1);
            // VkMemoryRequirements may exceed the logical output size used by the
            // preflight. Drop idle cache entries so this request can retry next frame
            // with the complete remaining physical budget.
            mImpl->evictCachedStages();
            return QueueResult::Deferred;
        }
        if (stageCpuStartValid && texturePrepThreadCpuNs(stageCpuEndNs)
            && stageCpuEndNs >= stageCpuStartNs)
            addSaturating(mImpl->interval.stageAllocationThreadCpuNs,
                          stageCpuEndNs - stageCpuStartNs);
        else
            addSaturating(mImpl->interval.stageAllocationCpuUnavailable, 1);
    }
    if (!stageBuffer->buffer || !stageBuffer->mapped || !stageBuffer->allocationSize
        || stageBuffer->size < workspace.outputBytes
        || stageBuffer->allocationSize > stageLimit - mImpl->activeStageUsed)
    {
        destroyBufferNoThrow(mImpl->context, stage);
        if (cachedStageIndex < 0) mImpl->evictCachedStages();
        return QueueResult::Deferred;
    }
    if (cachedStageIndex < 0 && stageBuffer->allocationSize > kStageLimit - mImpl->stageUsed)
    {
        destroyBufferNoThrow(mImpl->context, stage);
        mImpl->evictCachedStages();
        return QueueResult::Deferred;
    }

    if (!mImpl->pool)
    {
        try
        {
            mImpl->pool = std::make_unique<LL::ThreadPool>("VKTexturePrep", 3, kTicketCount);
            mImpl->pool->start();
        }
        catch (...)
        {
            if (mImpl->pool) { mImpl->pool->close(); mImpl->pool.reset(); }
            destroyBufferNoThrow(mImpl->context, stage);
            return QueueResult::Deferred;
        }
    }

    SourceSnapshot workerSource;
    try { workerSource = source; }
    catch (...) { destroyBufferNoThrow(mImpl->context, stage); return QueueResult::Deferred; }
    const Recipe workerRecipe = recipe;
    const auto workSignal = work;
    uint8_t* const destination = static_cast<uint8_t*>(stageBuffer->mapped);
    uint8_t* const scratchData = scratch.get();
    const uint64_t outBytes = workspace.outputBytes, tempBytes = workspace.scratchBytes;
    work->queuedNs = texturePrepMonotonicNs();
    LL::WorkQueue::Work worker;
    try
    {
        worker = [workerSource = std::move(workerSource), workerRecipe, workSignal,
                  destination, outBytes, scratchData, tempBytes]() mutable
        {
            workSignal->startedNs = texturePrepMonotonicNs();
            workSignal->workerCpuStartValid = texturePrepThreadCpuNs(workSignal->workerCpuStartNs);
            workSignal->state.store((uint8_t)State::Running, std::memory_order_relaxed);
            State result = State::Failed;
            if (workSignal->cancel.load(std::memory_order_acquire)) result = State::Cancelled;
            else
            {
                try
                {
                    const bool prepared = prepareTextureInput(workerSource, workerRecipe, destination, outBytes,
                        scratchData, tempBytes, workSignal->output);
                    if (prepared && preparedTextureOutputMatches(workerRecipe, workSignal->output,
                        workerRecipe.targetWidth ? workerRecipe.targetWidth : workerSource.width,
                        workerRecipe.targetHeight ? workerRecipe.targetHeight : workerSource.height, outBytes))
                        result = State::Ready;
                    else if (prepared)
                        workSignal->failureReason = FailureReason::OutputLayoutRejected;
                    else if (workerRecipe.uiFormat != LLVKTexturePrep::UiFormat::None)
                        workSignal->failureReason = FailureReason::UiInputRejected;
                    else if (workerSource.kind == LLVKTexturePrep::SourceKind::CompressedLevels)
                        workSignal->failureReason = FailureReason::CompressedInputRejected;
                    else
                        workSignal->failureReason = FailureReason::PixelInputRejected;
                }
                catch (...)
                {
                    workSignal->failureReason = FailureReason::Exception;
                    result = State::Failed;
                }
            }
            if (workSignal->cancel.load(std::memory_order_acquire)) result = State::Cancelled;
            workerSource = SourceSnapshot{};
            workSignal->workerCpuEndValid = texturePrepThreadCpuNs(workSignal->workerCpuEndNs);
            workSignal->readyNs = texturePrepMonotonicNs();
            workSignal->state.store((uint8_t)result, std::memory_order_release);
        };
    }
    catch (...)
    {
        destroyBufferNoThrow(mImpl->context, stage);
        return QueueResult::Deferred;
    }
    bool posted = false;
    try { posted = mImpl->pool->getQueue().tryPost(worker); }
    catch (...) { posted = false; }
    if (!posted)
    {
        destroyBufferNoThrow(mImpl->context, stage);
        return QueueResult::Deferred;
    }

    slot.generation = generation;
    slot.used = true;
    slot.key = key; slot.serial = source.serial; slot.sourceGeneration = source.generation;
    slot.domain = domain;
    slot.priority = priority;
    slot.recipe = recipe;
    slot.inputBytes = inputCharge;
    slot.stageBytes = stageBuffer->allocationSize;
    slot.scratchBytes = workspace.scratchBytes;
    slot.source = std::move(source);
    slot.lease = std::move(mainLease);
    slot.work = std::move(work);
    if (cachedStageIndex >= 0)
    {
        Impl::CachedStage& cached = mImpl->cachedStages[(uint32_t)cachedStageIndex];
        slot.stage = cached.buffer;
        cached.buffer = GpuBuffer{};
        cached.lastUsed = 0;
        if (mImpl->stageCacheClock != (std::numeric_limits<uint64_t>::max)())
            ++mImpl->stageCacheClock;
        addSaturating(mImpl->interval.stageReuseCalls, 1);
    }
    else
    {
        slot.stage = stage;
        stage = GpuBuffer{};
        mImpl->stageUsed += slot.stageBytes;
    }
    slot.scratch = std::move(scratch);
    mImpl->inputUsed += slot.inputBytes;
    mImpl->activeStageUsed += slot.stageBytes;
    mImpl->scratchUsed += slot.scratchBytes;
    ticket.slot = (uint16_t)slotIndex;
    ticket.generation = generation;
    return QueueResult::Accepted;
}
catch (...)
{
    ticket = {};
    return LLVKTexturePrep::QueueResult::Deferred;
}

LLVKTexturePrepService::State LLVKTexturePrepService::poll(Ticket ticket) const noexcept
{
    if (!mImpl) return State::Invalid;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    const Impl::Slot* slot = mImpl->find(ticket);
    return slot ? Impl::stateOf(*slot) : State::Invalid;
}

LLVKTexturePrepService::FailureReason LLVKTexturePrepService::failureReason(Ticket ticket) const noexcept
{
    if (!mImpl) return FailureReason::None;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    const Impl::Slot* slot = mImpl->find(ticket);
    return slot && Impl::stateOf(*slot) == State::Failed && slot->work
        ? slot->work->failureReason : FailureReason::None;
}

bool LLVKTexturePrepService::viewReady(Ticket ticket, ResultView& view) const noexcept
{
    if (!mImpl) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    const Impl::Slot* slot = mImpl->find(ticket);
    if (!slot || Impl::stateOf(*slot) != State::Ready || !slot->stage.buffer || !slot->work
        || !slot->work->output.byteCount || slot->work->output.byteCount > slot->stage.size)
        return false;
    ResultView result;
    result.buffer = slot->stage.buffer;
    result.offset = 0;
    result.byteCount = slot->work->output.byteCount;
    result.layout = slot->work->output;
    result.key = slot->key; result.serial = slot->serial;
    result.generation = slot->sourceGeneration;
    view = result;
    return true;
}

bool LLVKTexturePrepService::gpuUseRetired(Ticket ticket) const noexcept
{
    if (!mImpl) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    const Impl::Slot* slot = mImpl->find(ticket);
    // A missing generation was freed only after safe completion/retirement.
    return !slot || (!slot->recordPending && !slot->quarantined && !slot->retirementSerial);
}

bool LLVKTexturePrepService::cancel(Ticket ticket) noexcept
{
    if (!mImpl) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    Impl::Slot* slot = mImpl->find(ticket);
    if (!slot) return false;
    slot->releaseRequested = true;
    if (slot->work) slot->work->cancel.store(true, std::memory_order_release);
    if (!slot->recordPending && !slot->retirementSerial) mImpl->freeSlot(*slot);
    return true;
}

bool LLVKTexturePrepService::cancelKey(uint64_t key, uint64_t serial, uint32_t generation,
                                      ClientDomain domain) noexcept
{
    if (!mImpl || !key) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    bool found = false;
    for (Impl::Slot& slot : mImpl->slots)
    {
        if (!slot.used || slot.key != key || slot.serial != serial
            || slot.sourceGeneration != generation || slot.domain != domain) continue;
        found = true;
        slot.releaseRequested = true;
        if (slot.work) slot.work->cancel.store(true, std::memory_order_release);
        if (!slot.recordPending && !slot.retirementSerial) mImpl->freeSlot(slot);
    }
    return found;
}

bool LLVKTexturePrepService::noteRecordedForFrame(Ticket ticket) noexcept
{
    if (!mImpl) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    Impl::Slot* slot = mImpl->find(ticket);
    if (!slot || Impl::stateOf(*slot) != State::Ready || slot->releaseRequested
        || slot->recordPending || slot->retirementSerial) return false;
    slot->recordPending = true;
    slot->work->state.store((uint8_t)State::Recorded, std::memory_order_release);
    mImpl->releaseInput(*slot);
    return true;
}

bool LLVKTexturePrepService::markRecorded(Ticket ticket, uint64_t graphicsSubmitSerial) noexcept
{
    if (!mImpl || !graphicsSubmitSerial) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    Impl::Slot* slot = mImpl->find(ticket);
    if (!slot || !slot->recordPending || slot->retirementSerial
        || Impl::stateOf(*slot) != State::Recorded) return false;
    slot->recordPending = false;
    slot->retirementSerial = graphicsSubmitSerial;
    slot->work->state.store((uint8_t)State::Submitted, std::memory_order_release);
    return true;
}

void LLVKTexturePrepService::quarantineRecorded(Ticket ticket) noexcept
{
    if (!mImpl) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    Impl::Slot* slot = mImpl->find(ticket);
    if (!slot || !slot->recordPending) return;
    // Unknown queue completion is not a normal dropped frame. Keep both the mapped
    // stage and its aggregate-budget charge unavailable until a later explicit
    // quiescence proof or context/device teardown.
    slot->quarantined = true;
    if (slot->work) slot->work->state.store((uint8_t)State::Quarantined,
                                             std::memory_order_release);
}

void LLVKTexturePrepService::dropRecorded(Ticket ticket) noexcept
{
    if (!mImpl) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    Impl::Slot* slot = mImpl->find(ticket);
    if (!slot || !slot->recordPending) return;
    slot->recordPending = false;
    slot->quarantined = false; // caller only invokes after proving submissions quiesced
    if (slot->work) slot->work->state.store((uint8_t)State::Ready, std::memory_order_release);
    mImpl->releaseInput(*slot);
    if (slot->releaseRequested) mImpl->freeSlot(*slot);
}

void LLVKTexturePrepService::retireGpuUse(uint64_t completedSerial) noexcept
{
    if (!mImpl || !completedSerial) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    for (Impl::Slot& slot : mImpl->slots)
    {
        if (!slot.used || !slot.retirementSerial || slot.retirementSerial > completedSerial) continue;
        slot.retirementSerial = 0;
        mImpl->releaseInput(slot);
        if (slot.releaseRequested) mImpl->freeSlot(slot);
    }
}

bool LLVKTexturePrepService::release(Ticket ticket) noexcept
{
    if (!mImpl) return false;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    Impl::Slot* slot = mImpl->find(ticket);
    if (!slot) return false;
    slot->releaseRequested = true;
    if (slot->work && !Impl::workerDone(Impl::stateOf(*slot)))
        slot->work->cancel.store(true, std::memory_order_release);
    mImpl->releaseInput(*slot);
    if (!slot->recordPending && !slot->retirementSerial) mImpl->freeSlot(*slot);
    return true;
}

void LLVKTexturePrepService::reapCompleted() noexcept
{
    if (!mImpl) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    for (Impl::Slot& slot : mImpl->slots)
    {
        if (!slot.used) continue;
        mImpl->releaseInput(slot);
        if (slot.releaseRequested && !slot.recordPending && !slot.retirementSerial)
            mImpl->freeSlot(slot);
    }
}

LLVKTexturePrepService::Usage LLVKTexturePrepService::usage() const noexcept
{
    Usage result{};
    if (!mImpl) return result;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    result.inputBytes = mImpl->inputUsed;
    // Preserve admission semantics for clients: idle cached buffers are physically charged
    // against stageUsed internally, but they are reclaimable and must not make a client
    // reject work before submit() can reuse or evict them.
    result.stageBytes = mImpl->activeStageUsed;
    result.scratchBytes = mImpl->scratchUsed;
    for (const Impl::Slot& slot : mImpl->slots)
    {
        if (!slot.used) continue;
        ++result.liveTickets;
        if (Impl::stateOf(slot) == State::Running) ++result.running;
    }
    return result;
}

LLVKTexturePrepService::IntervalStats LLVKTexturePrepService::consumeIntervalStats() noexcept
{
    IntervalStats result{};
    if (!mImpl) return result;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    result = mImpl->interval;
    mImpl->interval = IntervalStats{};
    return result;
}

void LLVKTexturePrepService::noteSourceCapture(ClientDomain domain, uint64_t bytes,
                                                uint64_t threadCpuNs, bool cpuValid) noexcept
{
    if (!mImpl) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    IntervalStats& stats = mImpl->interval;
    addSaturating(stats.sourceCaptureCalls, 1);
    addSaturating(stats.privateCopyBytes, bytes);
    if (cpuValid)
    {
        addSaturating(stats.privateCopyThreadCpuNs, threadCpuNs);
        addSaturating(stats.privateCopyCpuSamples, 1);
    }
    else
        addSaturating(stats.privateCopyCpuUnavailable, 1);

    if (domain == ClientDomain::Ui)
    {
        addSaturating(stats.uiSourceCaptureCalls, 1);
        addSaturating(stats.uiPrivateCopyBytes, bytes);
        if (cpuValid)
        {
            addSaturating(stats.uiPrivateCopyThreadCpuNs, threadCpuNs);
            addSaturating(stats.uiPrivateCopyCpuSamples, 1);
        }
        else
            addSaturating(stats.uiPrivateCopyCpuUnavailable, 1);
    }
    else
    {
        addSaturating(stats.worldSourceCaptureCalls, 1);
        addSaturating(stats.worldPrivateCopyBytes, bytes);
        if (cpuValid)
        {
            addSaturating(stats.worldPrivateCopyThreadCpuNs, threadCpuNs);
            addSaturating(stats.worldPrivateCopyCpuSamples, 1);
        }
        else
            addSaturating(stats.worldPrivateCopyCpuUnavailable, 1);
    }
}

void LLVKTexturePrepService::stopAdmissions() noexcept
{
    if (!mImpl) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    mImpl->accepting = false;
}

void LLVKTexturePrepService::shutdown() noexcept
{
    if (!mImpl) return;
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        if (mImpl->closing) return;
        mImpl->accepting = false;
        mImpl->closing = true;
        for (Impl::Slot& slot : mImpl->slots)
            if (slot.used && slot.work) slot.work->cancel.store(true, std::memory_order_release);
    }
    if (mImpl->pool)
    {
        mImpl->pool->close();
        mImpl->pool.reset(); // joins the lazy worker pool before touching source/leases
    }
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    for (Impl::Slot& slot : mImpl->slots)
    {
        if (!slot.used) continue;
        mImpl->releaseInput(slot);
        // This method is only called by the owning context after a successful
        // vkDeviceWaitIdle proof and immediately before VkDevice teardown. At that point
        // even quarantined/record-pending stages are no longer in use and must be
        // explicitly destroyed rather than forgotten while the device remains live.
        if (slot.stage.buffer && mImpl->context)
            mImpl->context->destroyBuffer(slot.stage);
        slot.lease.reset();
        slot.source = LLVKTexturePrep::SourceSnapshot{};
        slot.scratch.reset();
        slot = Impl::Slot{};
    }
    for (uint32_t i = 0; i < mImpl->cachedStages.size(); ++i)
        mImpl->destroyCachedStage(i);
    mImpl->inputUsed = mImpl->stageUsed = mImpl->scratchUsed = 0;
    mImpl->activeStageUsed = 0;
}
