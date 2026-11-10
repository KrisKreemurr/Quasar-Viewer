/**
 * @file lltransportcircuittypes.h
 * @brief Owned value types for the transport/main circuit boundary.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTCIRCUITTYPES_H
#define LL_LLTRANSPORTCIRCUITTYPES_H

#include <cstdint>

// IPv4 as returned by net's receive metadata: address in network byte order,
// port in host byte order. These types never carry LLCircuitData pointers.
struct LLTransportEndpoint
{
    uint32_t addressNetworkOrder = 0;
    uint16_t portHostOrder = 0;
};

enum class LLTransportSnapshotState : uint8_t
{
    Unavailable, // no current coherent publication; not equivalent to absent
    Absent,
    Present
};

// Numeric loss counts are meaningful only with their per-incarnation quality.
enum class LLTransportLossQuality : uint8_t { Unknown, Complete, Partial };

// A single owner-produced coherent value. Publish by owned queue/banks or
// atomic fields plus bounded coherence checks: a seqlock around non-atomic
// fields is a C++ data race. The owner must not wait for a main-held mutex.
// The caller must enforce its freshness policy and rebase counters on
// incarnation change; present is not isAlive.
struct LLTransportCircuitSnapshot
{
    LLTransportEndpoint endpoint{};
    LLTransportSnapshotState state = LLTransportSnapshotState::Unavailable;
    uint64_t incarnation = 0;
    uint64_t publicationSequence = 0;
    // Both timestamps use steady_clock::time_since_epoch(), in microseconds;
    // main may compare to that same clock domain for an advancing packet age.
    uint64_t ownerMonotonicUsec = 0;
    uint64_t lastPacketMonotonicUsec = 0;
    // Creation time remains valid even before the first packet arrives.
    uint64_t birthMonotonicUsec = 0;
    uint32_t packetsIn = 0;
    int32_t bytesIn = 0;
    uint32_t packetsOut = 0;
    uint32_t packetsLost = 0;
    LLTransportLossQuality lossQuality = LLTransportLossQuality::Unknown;
    uint32_t pingMilliseconds = 0;
    bool alive = false;
    bool blocked = false;
    bool trusted = false;
    bool hasLastPacket = false;
};

// The caller supplies steady_clock::time_since_epoch() in microseconds.
// Return false for unavailable/absent snapshots or a sample from the future;
// callers must not use this estimate to authorize stale destructive actions.
inline bool llTransportCircuitAgeUsec(const LLTransportCircuitSnapshot& value,
                                       uint64_t nowUsec, uint64_t& ageUsec) noexcept
{
    if (value.state != LLTransportSnapshotState::Present || nowUsec < value.ownerMonotonicUsec)
        return false;
    const uint64_t origin = value.hasLastPacket ? value.lastPacketMonotonicUsec : value.birthMonotonicUsec;
    if (origin > value.ownerMonotonicUsec) return false;
    ageUsec = nowUsec - origin;
    return true;
}

// Commands and datagram sends must share one ordered owner stream. An enable
// identifies the login incarnation; close requests identify the circuit
// incarnation. A conditional close additionally carries the observed packet
// state so the owner can refuse it if a packet arrived since observation.
enum class LLTransportCircuitCommandKind : uint8_t
{
    Enable, Disable, DisableIfUnchanged
};
struct LLTransportCircuitCommand
{
    LLTransportCircuitCommandKind kind = LLTransportCircuitCommandKind::Enable;
    LLTransportEndpoint endpoint{};
    uint64_t operationId = 0;
    uint64_t loginGeneration = 0;
    uint64_t targetIncarnation = 0;
    uint64_t observedLastPacketMonotonicUsec = 0;
    uint64_t observedPublicationSequence = 0;
    bool observedHasLastPacket = false;
    bool trusted = false;
};

enum class LLTransportCircuitCommandStatus : uint8_t
{
    Applied, Absent, IncarnationChanged, PacketChanged, Invalid, Unavailable, Cancelled
};
struct LLTransportCircuitCompletion
{
    LLTransportEndpoint endpoint{};
    uint64_t operationId = 0;
    uint64_t loginGeneration = 0;
    uint64_t circuitIncarnation = 0;
    // Main defers close/removal until all accepted application events for
    // this incarnation through this sequence are consumed.
    uint64_t lastPublishedAppSequence = 0;
    LLTransportCircuitCommandStatus status = LLTransportCircuitCommandStatus::Unavailable;
    // Nonzero for FinishWatchdogTimeout terminal, including cancellation.
    // Main matches this with endpoint/login/incarnation and event barrier.
    uint64_t timeoutEventId = 0;
};

// Owner-side comparison only: the caller must process all preceding ingress
// before calling this and must atomically perform close before completing.
// Main must wait for matching Applied before removing its region. A timestamp
// alone is not a safe activity token: more than one packet can arrive in the
// same clock tick, so publicationSequence must advance for every admitted packet.
inline LLTransportCircuitCommandStatus llTransportCheckConditionalClose(
    const LLTransportCircuitCommand& command, const LLTransportCircuitSnapshot& current) noexcept
{
    if (command.kind != LLTransportCircuitCommandKind::DisableIfUnchanged ||
        !command.endpoint.addressNetworkOrder || !command.endpoint.portHostOrder ||
        !command.observedPublicationSequence)
        return LLTransportCircuitCommandStatus::Invalid;
    if (current.state == LLTransportSnapshotState::Unavailable)
        return LLTransportCircuitCommandStatus::Unavailable;
    if (current.state == LLTransportSnapshotState::Absent)
        return LLTransportCircuitCommandStatus::Absent;
    if (command.endpoint.addressNetworkOrder != current.endpoint.addressNetworkOrder ||
        command.endpoint.portHostOrder != current.endpoint.portHostOrder ||
        !command.targetIncarnation || current.incarnation != command.targetIncarnation)
        return LLTransportCircuitCommandStatus::IncarnationChanged;
    if (current.publicationSequence != command.observedPublicationSequence ||
        current.hasLastPacket != command.observedHasLastPacket ||
        (current.hasLastPacket && current.lastPacketMonotonicUsec != command.observedLastPacketMonotonicUsec))
        return LLTransportCircuitCommandStatus::PacketChanged;
    return LLTransportCircuitCommandStatus::Applied;
}

// Values are delivered exactly once to main when the implementation accepts a
// reliable send; callbacks and their user data remain in a main-owned registry.
// originalStatus preserves the existing callback's S32 result convention.
struct LLTransportSendCompletion
{
    LLTransportEndpoint endpoint{};
    uint64_t operationId = 0;
    uint64_t loginGeneration = 0;
    uint64_t circuitIncarnation = 0;
    int32_t originalStatus = 0;
};

// One terminal per accepted unreliable send. This is the local socket-attempt
// outcome, not a peer acknowledgement. Terminal credit is held from command
// admission through main-thread consumption, including during shutdown.
enum class LLTransportUnreliableStatus : uint8_t { Sent, Failed, Cancelled };
struct LLTransportUnreliableCompletion
{
    LLTransportEndpoint endpoint{};
    uint64_t operationId = 0, loginGeneration = 0, circuitIncarnation = 0;
    LLTransportUnreliableStatus status = LLTransportUnreliableStatus::Failed;
    int32_t osError = 0;
};

#endif // LL_LLTRANSPORTCIRCUITTYPES_H
