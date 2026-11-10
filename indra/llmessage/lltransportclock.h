/** Explicit transport clock domains. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTCLOCK_H
#define LL_LLTRANSPORTCLOCK_H
#include <chrono>
#include <cstdint>
#include <limits>

// Circuit state, watchdogs and snapshot ages use this domain only. Its epoch is
// std::chrono::steady_clock::time_since_epoch(), never the legacy wall epoch.
struct LLTransportSteadyUsec
{
    uint64_t value = 0;
    explicit constexpr LLTransportSteadyUsec(uint64_t usec) noexcept : value(usec) {}
};
// The anchored legacy domain exists solely for comparing old message times.
struct LLTransportLegacyUsec
{
    uint64_t value = 0;
    explicit constexpr LLTransportLegacyUsec(uint64_t usec) noexcept : value(usec) {}
};
inline LLTransportSteadyUsec llTransportSteadyNow() noexcept
{
    const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return LLTransportSteadyUsec(usec > 0 ? static_cast<uint64_t>(usec) : 0);
}
inline LLTransportLegacyUsec llTransportLegacyAt(LLTransportLegacyUsec base,
    LLTransportSteadyUsec anchor, LLTransportSteadyUsec now) noexcept
{
    if (now.value <= anchor.value) return base;
    const uint64_t delta = now.value - anchor.value;
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return LLTransportLegacyUsec(delta > maximum - base.value ? maximum : base.value + delta);
}
#endif
