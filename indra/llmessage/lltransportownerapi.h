/** Disabled owner-loop value boundary. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTOWNERAPI_H
#define LL_LLTRANSPORTOWNERAPI_H
#include "lltransportcircuitstate.h"
#include "lltransportclock.h"
#include "llpacketring.h"
#include "lltransportmainfacade.h"
#include "lltransportmainwake.h"
#include "lltransportspscqueue.h"
#include "lltransportcommandqueue.h"
#include "lltransportcontrolwarningstats.h"
#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

// One producer (main), one consumer (owner). No callback, viewer pointer or
// mutable main builder crosses this boundary. Main retains callback userdata.
// Send owns serialized uncompressed template bytes; the owner assigns the
// circuit packet ID once at the legacy pre-compression point. A command is
// accepted only after all required terminal credits and storage are reserved.
enum class LLTransportEncoding : uint8_t { Unencoded, ZeroCoded };

struct LLTransportOwnerCommand
{
    enum class Kind : uint8_t { Enable, Disable, DisableIfUnchanged, FinishWatchdogTimeout, Send, SetAckCollectTime, Stop };
    Kind kind = Kind::Enable;
    LLTransportCircuitCommand circuit{};
    // FinishWatchdogTimeout carries the exact owner-emitted identity/barrier.
    // Main may enqueue it only after processing all prior accepted APP events
    // through the barrier and running the callback; any revival Enable must be
    // ordered before it. The owner supplies its next watchdog RNG draw.
    LLTransportCircuitState::TimeoutEvent timeout{};
    uint64_t sendToken = 0;
    uint64_t schemaGeneration = 0;
    LLTransportCircuitState::RetryPolicy retry{};
    // SetAckCollectTime only: a main-generated nonzero operation ID and the
    // already-clamped [0, 2000000] microsecond setting. Admission reserves a
    // circuit-terminal credit; the owner applies it in command order and emits
    // one matching terminal. A full queue leaves main's update pending.
    uint64_t configOperationId = 0, ackCollectUsec = 0;
    uint32_t byteCount = 0;
    bool reliable = false;
    LLTransportEncoding encoding = LLTransportEncoding::Unencoded;
    std::array<uint8_t, LLTransportCircuitState::MaxPacketBytes> bytes{};
};

// Terminal credits are held across inflight and queued states until main pops
// them. Circuit and reliable-send terminals use separate queues so a close
// waiting on an application barrier cannot block reliable completion delivery.

// Separate bounded timeout/control and fatal capacity from application data.
// Timeout delivery waits on lastPublishedAppSequence on main, never on worker.
struct LLTransportOwnerControl
{
    enum class Kind : uint8_t { Timeout };
    Kind kind = Kind::Timeout;
    LLTransportEndpoint endpoint{};
    uint64_t loginGeneration = 0, circuitIncarnation = 0, eventId = 0;
    uint64_t lastPublishedAppSequence = 0;
};
struct LLTransportOwnerFatal { uint32_t code = 0; uint64_t sequence = 0; };

// Allocate as part of the startup graph on owned heap, never on the main stack.
// Budget actual sizeof(channels), scratch, arena, schemas, state, and overhead.
// Timeout/control/fatal credits are independent of the application event queue.
struct LLTransportOwnerChannels
{
    LLTransportCommandQueue<LLTransportOwnerCommand, 256> commands;
    LLTransportSpscQueue<LLTransportCircuitCompletion, 256> circuitTerminals;
    LLTransportSpscQueue<LLTransportSendCompletion, 256> sendTerminals;
    LLTransportSpscQueue<LLTransportUnreliableCompletion, 256> unreliableTerminals;
    LLTransportSpscQueue<LLTransportOwnerControl, 256> controls;
    // Three fixed, coalesced warning cells; main never blocks the owner.
    LLTransportControlWarningPublication controlWarnings;
    LLTransportSpscQueue<LLTransportOwnerFatal, 1> fatal;
};
static_assert(std::is_trivially_copyable_v<LLTransportOwnerCommand>);
static_assert(std::is_trivially_copyable_v<LLTransportCircuitCompletion>);
static_assert(std::is_trivially_copyable_v<LLTransportSendCompletion>);
static_assert(std::is_trivially_copyable_v<LLTransportUnreliableCompletion>);
static_assert(std::is_trivially_copyable_v<LLTransportOwnerControl>);
static_assert(std::is_trivially_copyable_v<LLTransportControlWarning>);

struct LLTransportSchemaEntry;
// A main-built immutable schema table and its backing arrays. The concrete
// builder owns every schema referenced by entries() and reports requested
// retained bytes, including the entry array. A graph retains this lease until
// worker join AND all borrowed inbound views are released.
class LLTransportSchemaLease
{
public:
    virtual ~LLTransportSchemaLease() = default;
    virtual const LLTransportSchemaEntry* entries() const noexcept = 0;
    virtual uint32_t count() const noexcept = 0;
    virtual uint64_t retainedBytes() const noexcept = 0;
};
// Built and frozen on main. The schema table and all schemas are immutable and
// retained through worker join AND all inbound reader/event drains. The socket
// remains caller-owned but is exclusively used by the worker after success.
// totalBudgetBytes covers requested allocations in the complete owner graph,
// including schemaBytes and allocator-overhead allowance supplied by caller.
struct LLTransportOwnerConfig
{
    // Explicit session-only A/B opt-in. Default construction never enables
    // the experimental owner; startup also requires the integration gate.
    bool experimentalEnabled = false;
    int socket = -1;
    LLPacketRing::UDPProxySnapshot proxy{};
    // Shared with the ordered main event producers; retained until joined
    // and all producers/drains stop. No worker-side viewer callback.
    std::shared_ptr<LLTransportMainWake> mainWake;
    const LLTransportSchemaEntry* schemas = nullptr;
    uint32_t schemaCount = 0;
    std::shared_ptr<const LLTransportSchemaLease> schemaLease;
    // Main supplies separate seeds for independent owner-local RNG streams;
    // the worker never calls LLUUID/global RNG to seed them.
    uint32_t dropSeed = 0, watchdogSeed = 0;
    uint64_t totalBudgetBytes = 0, schemaBytes = 0, allocatorOverheadBytes = 0;
    uint64_t arenaCapacityBytes = 0, maxArenaRecordBytes = 0;
    LLTransportCircuitState::Limits stateLimits{};
    // Main supplies a nonzero login generation for this owner's lifetime.
    // Restart or ordered login handoff is required for a later generation.
    uint64_t loginGeneration = 0;
    // Main snapshots the exception callback map. Activation is unsupported
    // when MX_RAN_OFF_END_OF_PACKET has a callback until ordered callback
    // delivery is implemented. The policy must stay frozen while active.
    bool controlTruncationCallbacksAbsent = false;
    // Main snapshots AckCollectTime, clamped to the legacy [0, 2s] range.
    // Subsequent changes use the ordered SetAckCollectTime command above.
    uint64_t initialAckCollectUsec = 0;
    uint64_t heartbeatUsec = 0, timeoutUsec = 0;
    // Initial reliable deadlines are per-send, not owner-global: each Send
    // supplies RetryPolicy::firstIntervalUsec from legacy main-side policy.
    LLTransportLegacyUsec legacyAnchor{0};
    LLTransportSteadyUsec steadyAnchor{0};
};

// Source boundary only; no concrete backend is enabled by this declaration.
// startOnMain requires all immutable schemas, buffers, credits and socket
// ownership configured before entry; failure leaves the legacy owner intact.
// Accepted commands remain ordered; stop drains/cancels accepted work without
// waiting for main to consume terminals. Main then drains callbacks/data and
// clears readers before destroying schemas/arena/transport globals.
enum class LLTransportOwnerStart : uint8_t { Started, Invalid, Capacity, AllocationFailed, ThreadFailed, Unavailable };
class LLTransportOwnerBackend : public LLTransportMainFacade
{
public:
    virtual LLTransportOwnerStart startOnMain(const LLTransportOwnerConfig&) noexcept = 0;
    virtual LLTransportCommandOffer submitOrdered(const LLTransportOwnerCommand&) noexcept = 0;
    virtual void requestStopOnMain() noexcept = 0;
    virtual void joinOnMain() noexcept = 0;
    // True only after join and after main drains every terminal/control/data
    // event and clears all readers; no accepted operation may be abandoned.
    virtual bool drainedOnMain() const noexcept = 0;
};
#endif
