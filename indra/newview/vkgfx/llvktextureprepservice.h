#pragma once

#include "llvktextureprep.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

class LLVKCpuLease;
class VkDeviceContext;

// Context-owned CPU-only preparation service shared by the world renderer and UI.
// Ticket state and all limits are fixed; callers retain a ticket until its submitted
// upload has retired. The service owns the main-thread CPU lease after acceptance.
class LLVKTexturePrepService
{
public:
    static constexpr uint32_t kTicketCount = 128;
    static constexpr uint32_t kPriorityReserve = 8;
    static constexpr uint32_t kVisibleReserve = 24;
    static constexpr uint64_t kInputLimit = 192ull << 20;
    static constexpr uint64_t kPriorityInputReserve = 128ull << 20;
    static constexpr uint64_t kVisibleInputReserve = 32ull << 20;
    static constexpr uint64_t kStageLimit = 256ull << 20;
    static constexpr uint64_t kPriorityStageReserve = 128ull << 20;
    static constexpr uint64_t kVisibleStageReserve = 64ull << 20;
    static constexpr uint64_t kScratchLimit = 192ull << 20;
    static constexpr uint64_t kPriorityScratchReserve = 128ull << 20;
    static constexpr uint64_t kVisibleScratchReserve = 32ull << 20;

    struct Ticket
    {
        uint32_t generation = 0;
        uint16_t slot = 0xffffu;
        uint16_t reserved = 0;
        bool valid() const noexcept { return slot < kTicketCount && generation != 0; }
    };

    enum class State : uint8_t
    {
        Invalid, Queued, Running, Ready, Recorded, Quarantined, Submitted, Failed, Cancelled
    };

    enum class FailureReason : uint8_t
    {
        None, UiInputRejected, PixelInputRejected, CompressedInputRejected,
        OutputLayoutRejected, Exception
    };

    enum class ClientDomain : uint8_t { World, Ui };

    struct ResultView
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, byteCount = 0;
        LLVKTexturePrep::OutputLayout layout{};
        uint64_t key = 0, serial = 0;
        uint32_t generation = 0;
    };

    struct Usage
    {
        // stageBytes is the active-ticket charge used by admission callers. Retired cached
        // VkBuffers remain in the service's physical 256 MiB ledger but are reclaimable.
        uint64_t inputBytes = 0, stageBytes = 0, scratchBytes = 0;
        uint32_t liveTickets = 0, running = 0;
    };

    struct IntervalStats
    {
        uint64_t completedJobs = 0;
        uint64_t workerReadyJobs = 0, workerFailedJobs = 0, workerCancelledJobs = 0;
        uint64_t uiWorkerFailedJobs = 0, worldWorkerFailedJobs = 0;
        uint64_t uiPreflightRejectedJobs = 0, worldPreflightRejectedJobs = 0;
        uint64_t workerUiRejects = 0, workerPixelRejects = 0, workerCompressedRejects = 0;
        uint64_t workerOutputRejects = 0, workerExceptions = 0;
        uint64_t workerQueueWaitNs = 0, workerElapsedNs = 0, workerThreadCpuNs = 0;
        uint64_t workerCpuSamples = 0, workerCpuUnavailable = 0, workerTimingUnavailable = 0;
        uint64_t sourceCaptureCalls = 0, privateCopyBytes = 0, privateCopyThreadCpuNs = 0;
        uint64_t privateCopyCpuSamples = 0, privateCopyCpuUnavailable = 0;
        uint64_t uiSourceCaptureCalls = 0, uiPrivateCopyBytes = 0, uiPrivateCopyThreadCpuNs = 0;
        uint64_t uiPrivateCopyCpuSamples = 0, uiPrivateCopyCpuUnavailable = 0;
        uint64_t worldSourceCaptureCalls = 0, worldPrivateCopyBytes = 0;
        uint64_t worldPrivateCopyThreadCpuNs = 0, worldPrivateCopyCpuSamples = 0;
        uint64_t worldPrivateCopyCpuUnavailable = 0;
        uint64_t stageAllocationCalls = 0, stageAllocationThreadCpuNs = 0;
        uint64_t stageReuseCalls = 0;
        uint64_t stageAllocationCpuUnavailable = 0;
    };

    explicit LLVKTexturePrepService(VkDeviceContext* context) noexcept;
    ~LLVKTexturePrepService();
    LLVKTexturePrepService(const LLVKTexturePrepService&) = delete;
    LLVKTexturePrepService& operator=(const LLVKTexturePrepService&) = delete;

    // Admission priority, not worker ordering: the worker queue is FIFO. Background
    // may use slots [0,96); Visible prefers [96,120) then borrows [0,96); SelfOrHud
    // prefers [120,128) then borrows world slots. Shared input/stage/scratch ledgers
    // have cumulative ceilings (MiB): Background 32/64/32, Visible 64/128/64,
    // SelfOrHud 192/256/192. Higher-priority work can still fill shared capacity;
    // reserves prevent lower-priority-first floods, without preemption or eviction.
    LLVKTexturePrep::QueueResult submit(uint64_t key,
        LLVKTexturePrep::SourceSnapshot&& source,
        const LLVKTexturePrep::Recipe& recipe,
        LLVKTexturePrep::Priority priority,
        LLVKCpuLease&& mainLease,
        Ticket& ticket,
        ClientDomain domain = ClientDomain::World);
    State poll(Ticket ticket) const noexcept;
    FailureReason failureReason(Ticket ticket) const noexcept;
    bool viewReady(Ticket ticket, ResultView& view) const noexcept;
    // True only when a recorded upload is unsubmitted/no longer referenced by any
    // command buffer, or its submit serial has passed a proven fence.
    bool gpuUseRetired(Ticket ticket) const noexcept;

    // cancel() prevents publication but keeps the source and main-thread lease until the
    // worker has stopped reading. release() drops resources only after worker completion
    // and any recorded GPU use has retired. The ticket's GPU buffer is borrowed by the
    // command buffer until the context reports the matching graphics-submit fence complete.
    // If completion is unknown, quarantineRecorded pins the stage and budget charge; it
    // is never treated as an ordinary retryable dropped command.
    bool cancel(Ticket ticket) noexcept;
    bool cancelKey(uint64_t key, uint64_t serial, uint32_t generation,
                   ClientDomain domain = ClientDomain::World) noexcept;
    bool noteRecordedForFrame(Ticket ticket) noexcept;
    bool markRecorded(Ticket ticket, uint64_t graphicsSubmitSerial) noexcept;
    // Unknown completion (device loss or failed idle/fence proof) pins the mapped stage
    // and its aggregate charge. Only a later explicit quiescence proof may drop it.
    void quarantineRecorded(Ticket ticket) noexcept;
    void dropRecorded(Ticket ticket) noexcept;
    void retireGpuUse(uint64_t completedRetirementValue) noexcept;
    bool release(Ticket ticket) noexcept;
    void reapCompleted() noexcept; // call on main even when the world renderer is absent
    Usage usage() const noexcept;
    // Main-thread interval consumption. Worker completions are harvested once by reap /
    // input release; source-capture measurements are reported once by the caller after
    // each capture attempt, including refusal. SourceSnapshot copy fields are not also
    // aggregated during submit. CPU fields use per-thread clocks and report unavailable.
    IntervalStats consumeIntervalStats() noexcept;
    void noteSourceCapture(ClientDomain domain, uint64_t privateCopyBytes,
                           uint64_t privateCopyThreadCpuNs, bool cpuValid) noexcept;
    // Close new admissions without releasing CPU leases, staging, or ticket state. The
    // context calls this as soon as teardown starts, before testing device/fence status.
    void stopAdmissions() noexcept;
    // Final destruction is valid only after the owning context has proven device idle
    // and is about to destroy this VkDeviceContext.
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
