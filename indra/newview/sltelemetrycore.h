/**
 * @file sltelemetrycore.h
 * @author SkyLine
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * $/LicenseInfo$
 */
#pragma once
#include <array>
#include "sltelemetrysettings.h"
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>

namespace SLTelemetry
{
constexpr int CONSENT_VERSION = 1;
constexpr std::size_t BODY_LIMIT = 16 * 1024;
constexpr std::size_t QUEUE_LIMIT = 8;
constexpr bool consentEnabled(int value) noexcept { return value == CONSENT_VERSION; }
constexpr bool needsPrompt(int value) noexcept { return value == 0 || (value > 0 && value < CONSENT_VERSION); }

// IEEE exponent + 16-entry mantissa approximation of log2. Constant cost,
// 64 logarithmic bins between 1 and 2000 ms, plus underflow and overflow.
inline unsigned frameBin(double milliseconds) noexcept
{
    if (!(milliseconds >= 1.0)) return 0;
    if (milliseconds >= 2000.0) return 65;
    static constexpr unsigned fraction[16] = {0,5,10,15,19,23,27,31,34,38,41,44,47,50,53,56};
    const float sample = static_cast<float>(milliseconds);
    std::uint32_t bits;
    std::memcpy(&bits, &sample, sizeof(bits));
    const unsigned exponent = ((bits >> 23) & 255u) - 127u;
    const unsigned logarithm = exponent * 64u + fraction[(bits >> 19) & 15u];
    const unsigned index = logarithm * 64u / 702u;
    return 1u + (index < 64u ? index : 63u);
}
inline void increment(std::uint32_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint32_t>::max)()) ++value;
}
struct Histogram
{
    std::array<std::uint32_t, 66> bins{};
    std::uint32_t x2Median = 0;
    double runningMedian = 16.0;
    std::uint32_t frames = 0, over50 = 0, over100 = 0, over250 = 0, over1000 = 0;
    void add(double milliseconds) noexcept
    {
        if (!std::isfinite(milliseconds) || milliseconds < 0 || frames == (std::numeric_limits<std::uint32_t>::max)()) return;
        increment(bins[frameBin(milliseconds)]); increment(frames);
        if (milliseconds > runningMedian*2) increment(x2Median);
        if (milliseconds > 50) increment(over50);
        if (milliseconds > 100) increment(over100);
        if (milliseconds > 250) increment(over250);
        if (milliseconds > 1000) increment(over1000);
    }
};
inline double percentile(const Histogram& histogram, double fraction) noexcept
{
    if (!histogram.frames) return 0;
    const auto rank = static_cast<std::uint64_t>(std::ceil(histogram.frames*fraction));
    std::uint64_t cumulative = 0;
    for (unsigned i=0;i<66;++i)
    {
        cumulative+=histogram.bins[i];
        if (cumulative>=rank)
        {
            if (!i) return 0.5;
            if (i==65) return 2000;
            return std::exp2((i-0.5)*10.965784284662087/64.0);
        }
    }
    return 2000;
}
inline void addSaturated(std::uint32_t& value,std::uint32_t delta) noexcept
{
    const auto limit=(std::numeric_limits<std::uint32_t>::max)();
    value=limit-value<delta?limit:value+delta;
}
inline unsigned durationBucket(double seconds) noexcept
{
    return seconds<2?0:seconds<5?1:seconds<10?2:seconds<20?3:seconds<40?4:seconds<80?5:6;
}
struct Gauge
{
    std::array<std::uint32_t,33> bins{};
    std::uint32_t samples=0;
    unsigned peak=0;
    void add(std::uint64_t value) noexcept
    {
        if (samples==(std::numeric_limits<std::uint32_t>::max)()) return;
        unsigned bucket=0;
        while(value && bucket<32){++bucket;value>>=1;}
        increment(bins[bucket]);increment(samples);
        if(bucket>peak)peak=bucket;
    }
    unsigned median() const noexcept
    {
        std::uint64_t sum=0;
        for(unsigned i=0;i<33;++i){sum+=bins[i];if(sum>=(std::uint64_t(samples)+1)/2)return i;}
        return 0;
    }
};
// Initial report timing is process-local. It is armed only by a consented,
// connected login frame; no pre-consent or logged-out elapsed time is retained.
struct InitialSegmentGate
{
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kSettleDelay{120};
    static constexpr std::chrono::seconds kFallbackDelay{300};
    Clock::time_point anchor{};
    bool armed = false;
    bool handedOff = false;

    void arm(Clock::time_point now) noexcept
    {
        if (!armed && !handedOff)
        {
            anchor = now;
            armed = true;
        }
    }
    void restartAfterLogin(Clock::time_point now) noexcept
    {
        if (armed && !handedOff) anchor = now;
    }
    void consentRevoked() noexcept
    {
        if (!handedOff)
        {
            armed = false;
            anchor = {};
        }
    }
    bool ready(Clock::time_point now, bool loggedIn, bool connected,
               bool teleporting, bool settled, bool settleBarrierClear = true) const noexcept
    {
        if (!armed || handedOff || !loggedIn || !connected || teleporting || now < anchor) return false;
        const auto elapsed = now - anchor;
        return elapsed >= kSettleDelay &&
            (settled || (settleBarrierClear && elapsed >= kFallbackDelay));
    }
    template <typename Schedule>
    bool tryHandoff(Clock::time_point now, bool loggedIn, bool connected,
                    bool teleporting, bool settled, Schedule&& schedule,
                    bool settleBarrierClear = true)
    {
        if (!ready(now, loggedIn, connected, teleporting, settled, settleBarrierClear) || !schedule()) return false;
        armed = false;
        handedOff = true;
        return true;
    }
};
constexpr unsigned DISK_ACTION_INITIAL = 5;

// Fixed-value baseline snapshots let a successful segment seal advance its
// counters to exactly the totals polled into that segment.
template<class Allocation, class Cache, class Data>
struct CounterBaselines
{
    Allocation allocation{};
    Cache cache{};
    Data data{};
};

constexpr unsigned ELAPSED_HOUR_UNKNOWN = 25;
struct Window
{
    unsigned elapsedHour = ELAPSED_HOUR_UNKNOWN; // Local sentinel is omitted from the wire.
    std::array<Histogram, 4> frames{};
    Histogram jitter,gpuTime,rigTime;
    std::array<std::uint32_t,16> vramRatio{};
    std::array<std::uint32_t,3> vramSeconds{},boundSeconds{};
    std::array<std::uint32_t,4> upscalerSeconds{};
    std::uint32_t vramHalfGB=0,overheadQuarterGB=0,rungMax=0;
    std::array<std::uint32_t,4> classSeconds{};
    std::array<Gauge,5> assetGauges{};
    std::array<Gauge,10> loadingGauges{};
    std::array<std::uint32_t,8> teleportDuration{};
    std::array<std::uint32_t,3> cache{}, dataMBLog2{};
    std::uint32_t regionCrossings=0, disconnects=0, loginBucket=7, readyBucket=7, longFrames=0, stutterSeconds=0; // steady, loading, background, VR
    std::array<std::uint32_t, 3> ramSeconds{};
    std::uint32_t seconds = 0, teleports = 0, objects = 0, avatars = 0, textures = 0;
    std::uint32_t peakRSSHalfGB = 0;
    std::array<std::uint32_t, 16> availableRatio{};
    std::array<std::uint32_t, 9> allocFailures{}, allocRetried{}, allocDropped{};
    std::uint32_t psiMax5pct = 0, sheds = 0, reserveReleased = 0;
    std::uint32_t decodePending = 0, waiters = 0, inflightMB = 0;
};
// Clock values never leave this process. The owner starts this only after consent/purge.
// Supplying time points keeps the exact production boundary policy testable offline.
struct CollectionClock
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point origin{};
    bool started = false;
    void start(Clock::time_point now) noexcept { origin=now; started=true; }
    unsigned hour(Clock::time_point now) const noexcept
    {
        if (!started || now<origin) return ELAPSED_HOUR_UNKNOWN;
        const auto elapsed=std::chrono::duration_cast<std::chrono::hours>(now-origin).count();
        return elapsed>=24?24:static_cast<unsigned>(elapsed);
    }
    template<typename Flush> bool beforeSample(const Window& window, Clock::time_point now, Flush flush) const
    {
        const unsigned currentHour=hour(now);
        if (currentHour==ELAPSED_HOUR_UNKNOWN) return false;
        if (window.elapsedHour<=24 && window.elapsedHour!=currentHour)
        {
            flush(); // Refusal must retain the old window; never put the boundary frame in it.
            return false; // Also drop this frame after success; next frame starts the new bin.
        }
        return true;
    }
    bool stamp(Window& window, Clock::time_point now, Clock::time_point& nextSecond) const noexcept
    {
        if (window.elapsedHour!=ELAPSED_HOUR_UNKNOWN || hour(now)>24) return false;
        window.elapsedHour=hour(now);
        nextSecond=now+std::chrono::seconds(1); // No catch-up duration before first accepted data.
        return true;
    }
    bool acceptsEvent(const Window& window, Clock::time_point now) const noexcept
    {
        return window.elapsedHour<=24 && window.elapsedHour==hour(now);
    }
};
struct Snapshot
{
    std::array<char, 65> cpu{}, gpu{};
    std::array<char,257> rawCPU{},rawGPU{};
    std::array<char, 21> version{};
    unsigned threads = 0, ramGB = 0, vramGB = 0, vendor = 4;
    std::array<double, SETTING_COUNT> settings{};
    std::array<bool, SETTING_COUNT> hasSetting{};
    unsigned channel = 4, build = 3, isa = 3, os = 3;
    unsigned previousEnd = 0, install = 6;
    unsigned resolution = 9, refresh = 0, scaleQuarters = 0, monitors = 0, windowMode = 4;
    bool hdr = false, displayAvailable = false, swapPresent = false, psi = false, flatpak = false;
    unsigned windowsBuild = 0, distro = 28, distroMajor = 0, desktop = 11, session = 3;
    std::array<unsigned, 2> kernel{}, glibc{};
    std::array<bool, 7> features{};
    unsigned grid = 2;
};
inline unsigned logCount(std::uint64_t value) noexcept
{
    unsigned result = 0;
    while (value && result < 32) { ++result; value >>= 1; }
    return result;
}
template<std::size_t N>
inline unsigned nearestBucket(unsigned value, const unsigned (&buckets)[N]) noexcept
{
    unsigned choice = buckets[0], distance = value > choice ? value - choice : choice - value;
    for (unsigned candidate : buckets)
    {
        const unsigned delta = value > candidate ? value - candidate : candidate - value;
        if (delta < distance) { choice = candidate; distance = delta; }
    }
    return choice;
}
// Reject the entire model on disallowed bytes; never turn a path into a model.
// Strip clock suffixes and collapse spaces; identifiers and control bytes fail closed.
inline bool normalizeModel(const char* input, std::array<char, 65>& output) noexcept
{
    output.fill(0);
    if (!input) return false;
    std::size_t used = 0;
    for (std::size_t pos = 0; ; ++pos)
    {
        if (pos >= 256) { output.fill(0); return false; }
        const char ch = input[pos];
        if (!ch) break;
        if (ch == '@') { output.fill(0); return false; }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == ' ' || ch == '(' || ch == ')' ||
              ch == '.' || ch == ',' || ch == '+' || ch == '-' || ch == '_')) { output.fill(0); return false; }
        if (ch == ' ' && (!used || output[used-1] == ' ')) continue;
        if (used == 64) { output.fill(0); return false; }
        output[used++] = ch;
    }
    while (used && output[used-1] == ' ') output[--used] = 0;
    return used != 0;
}
}
