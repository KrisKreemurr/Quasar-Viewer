/**
 * @file llmeshpreparation.h
 * @brief Bounded ownership of mesh capture preparation results.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 * SPDX-License-Identifier: LGPL-2.1-only
 * $/LicenseInfo$
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

// The budget bounds the number of retained results and their conservative byte
// reservations. It does not pretend to measure allocator/container overhead.
struct LLMeshPrepState
{
    static constexpr uint32_t MAX_RESULTS = 16;
    static constexpr uint32_t MAX_ACTIVE = 2;
    static constexpr uint64_t MAX_BYTES = 64ull * 1024 * 1024;
    static constexpr uint64_t MAX_JOB_BYTES = 16ull * 1024 * 1024;

    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> generation{1};
    std::atomic<uint32_t> results{0}, active{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> accepted{0}, refused{0}, failed{0}, stale{0}, commitCalls{0};
    std::atomic<uint64_t> workerCpuNs{0}, mainCpuNs{0}, workerCpuSamples{0}, mainCpuSamples{0}, cpuUnavailable{0};

    // Main thread owns enable/disable. Old results are never published into a
    // later renderer epoch, even when their worker finishes after a transition.
    void setEnabled(bool value)
    {
        if (enabled.exchange(value, std::memory_order_acq_rel) != value)
            generation.fetch_add(1, std::memory_order_acq_rel);
    }
};

class LLMeshPrepLease
{
public:
    LLMeshPrepLease() = default;
    LLMeshPrepLease(const LLMeshPrepLease&) = delete;
    LLMeshPrepLease& operator=(const LLMeshPrepLease&) = delete;
    LLMeshPrepLease(LLMeshPrepLease&& other) noexcept { *this = std::move(other); }
    LLMeshPrepLease& operator=(LLMeshPrepLease&& other) noexcept
    {
        if (this != &other)
        {
            release();
            mState = std::move(other.mState);
            mBytes = std::exchange(other.mBytes, 0);
            mGeneration = other.mGeneration;
            mWorking = std::exchange(other.mWorking, false);
        }
        return *this;
    }
    ~LLMeshPrepLease() { release(); }

    bool acquire(const std::shared_ptr<LLMeshPrepState>& state, uint64_t requested)
    {
        if (mState) return false; // one live reservation per lease
        if (!state || !state->enabled.load(std::memory_order_acquire) || !requested ||
            requested > LLMeshPrepState::MAX_JOB_BYTES)
        {
            if (state) state->refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t epoch = state->generation.load(std::memory_order_acquire);
        if (!reserveCount(state->results, LLMeshPrepState::MAX_RESULTS))
        {
            state->refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!reserveCount(state->active, LLMeshPrepState::MAX_ACTIVE))
        {
            state->results.fetch_sub(1, std::memory_order_release);
            state->refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        uint64_t usedBytes = state->bytes.load(std::memory_order_relaxed);
        bool reserved = false;
        // Admission never waits indefinitely when several decoders finish at once.
        for (unsigned attempt = 0; attempt < 8 && usedBytes <= LLMeshPrepState::MAX_BYTES - requested; ++attempt)
        {
            if (state->bytes.compare_exchange_weak(usedBytes, usedBytes + requested, std::memory_order_acq_rel))
            {
                reserved = true;
                break;
            }
        }
        if (!reserved)
        {
            state->active.fetch_sub(1, std::memory_order_release);
            state->results.fetch_sub(1, std::memory_order_release);
            state->refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        mState = state;
        mBytes = requested;
        mGeneration = epoch;
        mWorking = true;
        if (!current())
        {
            state->stale.fetch_add(1, std::memory_order_relaxed);
            release();
            return false;
        }
        state->accepted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool current() const
    {
        return mState && mState->enabled.load(std::memory_order_acquire) &&
               mState->generation.load(std::memory_order_acquire) == mGeneration;
    }
    void finishWorker()
    {
        if (mWorking)
        {
            mState->active.fetch_sub(1, std::memory_order_release);
            mWorking = false;
        }
    }
    uint64_t reservedBytes() const { return mBytes; }
private:
    static bool reserveCount(std::atomic<uint32_t>& count, uint32_t limit)
    {
        uint32_t value = count.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 8 && value < limit; ++attempt)
            if (count.compare_exchange_weak(value, value + 1, std::memory_order_acq_rel)) return true;
        return false;
    }
    void release()
    {
        if (!mState) return;
        finishWorker();
        mState->bytes.fetch_sub(mBytes, std::memory_order_release);
        mState->results.fetch_sub(1, std::memory_order_release);
        mState.reset();
        mBytes = 0;
    }
    std::shared_ptr<LLMeshPrepState> mState;
    uint64_t mBytes = 0, mGeneration = 0;
    bool mWorking = false;
};
