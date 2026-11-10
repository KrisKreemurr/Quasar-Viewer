#pragma once
#include "vk_display_calibration.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace vkdisplay
{
struct CalibrationJob
{
    std::atomic<bool> released{false};
    uint64_t generation = 0;
    uint32_t display = 0;
    void* window = nullptr;
    LogicalBounds bounds{};
    ReportedLuminance report;
    bool ran = false; // published by the lease's release-store
};
struct CalibrationJobLease
{
    explicit CalibrationJobLease(std::shared_ptr<CalibrationJob> value) : job(std::move(value)) {}
    ~CalibrationJobLease() { job->released.store(true, std::memory_order_release); }
    std::shared_ptr<CalibrationJob> job;
};

enum class CalibrationPollDecision { Wait, Apply, Retry };
inline CalibrationPollDecision decideCalibrationPoll(bool released, bool ran, bool current,
                                                       bool matchingGeneration, bool pending) noexcept
{
    if (!released) return CalibrationPollDecision::Wait;
    return ran && current && matchingGeneration && !pending ? CalibrationPollDecision::Apply
                                                               : CalibrationPollDecision::Retry;
}
}
