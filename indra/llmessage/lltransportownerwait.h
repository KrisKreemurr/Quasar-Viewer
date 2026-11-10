/** Socket-owner command/readiness wake. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTOWNERWAIT_H
#define LL_LLTRANSPORTOWNERWAIT_H
#include "lltransportclock.h"
#include <memory>
#include <cstddef>

// Conservative allocator-payload reservation for the single shared wait object.
// The factory caps its allocation request at this value; graph allocator metadata
// is charged separately. OS wake resources: one eventfd (Linux), two events (Win).
inline constexpr std::size_t LLTransportOwnerWaitHeapBudgetBytes = 4096;

struct LLTransportOwnerWaitResult
{
    bool readable = false;
    bool writable = false;
    bool signalled = false;
    bool deadline = false;
    bool error = false;
    // Queued datagram error consumed by readiness (SO_ERROR / WSA event).
    // Route nativeError through the same bounded receive-error policy. It is
    // not proof of a drained socket, nor automatically a fatal wait failure.
    bool socketErrorPending = false;
    unsigned nativeError = 0;
};
class LLTransportOwnerWait
{
public:
    virtual ~LLTransportOwnerWait() = default;
    // Main publishes commands/stop before signal; no callbacks or allocation.
    virtual void signal() noexcept = 0;
    // Owner only, before startup reports success. Socket is already nonblocking
    // and exclusively held through detach + join. Failure must abort startup.
    virtual bool bindSocketOnOwner(int socket) noexcept = 0;
    virtual void detachSocketOnOwner() noexcept = 0;
    // Owner only. Drain runnable commands BEFORE waiting; a bounded drain with
    // remaining commands must not wait. Flags may be simultaneous. The wake
    // stays pending across publication/wait races; no sequence token is needed.
    // wantReadable=false suppresses read readiness during bounded backoff or
    // held-control backpressure; command/stop and timer wakes remain enabled.
    // A terminal error takes precedence over readiness; after error the caller
    // must stop the owner rather than consume any other result flags.
    virtual LLTransportOwnerWaitResult waitUntil(int socket,
        LLTransportSteadyUsec deadline, bool wantWritable, bool wantReadable = true) noexcept = 0;
};
// Main startup: allocate OS wake resources before handing the socket to worker.
// All signal producers and waiter retain shared ownership through worker join.
// Destroy before closing the socket; detach on owner before reporting joined.
std::shared_ptr<LLTransportOwnerWait> llCreateTransportOwnerWait() noexcept;
#endif
