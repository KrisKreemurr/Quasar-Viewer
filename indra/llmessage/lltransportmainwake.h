/** Main-loop wake contract for transport and ordered event producers.
 * SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTMAINWAKE_H
#define LL_LLTRANSPORTMAINWAKE_H
#include "lltransportclock.h"
#include <cstdint>
#include <memory>

// A shared lifetime-owned wake object can be signalled by the socket owner and
// by the HTTP/event producer after they publish main-thread work. signal() must
// not execute viewer callbacks, allocate, or wait for a mutex held by main.
// The concrete implementation must not lose a notification racing the wait;
// sequence wrap/exhaustion must fail awake, never sleep on a reused token.
// Native wake support does not itself integrate the main dispatch/pacing loop.
enum class LLTransportWakeResult : uint8_t { Signalled, Deadline, Unavailable };
class LLTransportMainWake
{
public:
    virtual ~LLTransportMainWake() = default;
    // Failure preserves out; all producers publish work before signal().
    virtual bool observe(uint64_t& out) const noexcept = 0;
    virtual void signal() noexcept = 0;
    // Main-thread only. If a signal occurs after observe(), return Signalled
    // even if it races the call to waitUntil. Deadline is in steady epoch;
    // spurious Signalled is permitted, early Deadline is not. Wait never runs
    // any event handler and cannot substitute for draining the ordered queue.
    virtual LLTransportWakeResult waitUntil(uint64_t observed,
                                             LLTransportSteadyUsec deadline) noexcept = 0;
};
// Main-thread startup only; null means allocation/platform-resource failure. Observe
// BEFORE draining main work, then wait with that token so a racing publication wakes
// the wait. Keep shared ownership through all producers and the final main wait.
std::shared_ptr<LLTransportMainWake> llCreateTransportMainWake() noexcept;
#endif
