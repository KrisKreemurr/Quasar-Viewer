/** WIP owner receive metadata; do not enable in the viewer.
 * Copyright (c) 2026 Slipstream Viewer Project. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTINGRESS_H
#define LL_LLTRANSPORTINGRESS_H

#include "lltransportcircuittypes.h"
#include "lltransportclock.h"
#include "lltransportdecodedpayload.h"
// Defined by the independently owned arena commit; this WIP child is not built
// until that commit is reviewed and imported.
#include "lltransportdecodedarena.h"
#include "lltransportspscqueue.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Current 479 nondeprecated templates require at most 2,041 blocks, 11,732
// fields and 52,562 data bytes. The decoder uses exactly one such scratch
// payload on the owner thread, allocated as part of the startup memory ledger;
// it must never be embedded in each queued event or allocated on the main stack.
using LLTransportIngressScratch = LLTransportDecodedPayload<65536, 2048, 16384>;

// The arena owns exactly the used data, block and field ranges. The complete
// identity-bearing Record is retained, including serial, layout and counts.
struct LLTransportIngressEvent
{
    LLTransportEndpoint sender{};
    LLTransportEndpoint receivingInterface{};
    uint64_t circuitIncarnation = 0;
    // Monotonic within this circuit incarnation; never wrap/reuse.
    uint64_t appSequence = 0;
    uint64_t schemaGeneration = 0;
    LLTransportDecodedArena::Record arenaRecord{};
    uint64_t firstTruncationOffset = 0;
    uint64_t firstTruncationWanted = 0;
    uint32_t packetId = 0;
    uint32_t messageNumber = 0;
    uint32_t wireBytes = 0;
    uint32_t expandedBytes = 0;
    uint32_t compressedBytes = 0;
    uint32_t truncations = 0;
    bool reliable = false;
};

// 4,096 compact metadata cells; payload bytes live in one separately bounded
// packed arena. Both this queue and the single scratch allocation are part of
// the same startup ledger as arena bytes, schema copies and other allocations.
using LLTransportIngressQueue = LLTransportSpscQueue<LLTransportIngressEvent, 4096>;
static_assert(std::is_trivially_copyable<LLTransportIngressEvent>::value, "owned metadata must be a value");
static_assert(sizeof(LLTransportIngressEvent) <= 128, "metadata cells must remain compact");

// Caller owns one successful state receive reservation and a completed decode
// in scratch. Duplicate control handling, schema/trust validation and budgeted
// event/scratch/arena startup allocation remain the actor's responsibility.
// This transaction must execute on the sole owner without re-entry.
enum class LLTransportPublishStatus : uint8_t
{
    Published, Backpressure, InvalidRecord, StateRejected, Poisoned, InvariantFailure
};

template <typename CircuitState, typename Arena, typename Queue>
LLTransportPublishStatus llTransportPublishDecoded(
    CircuitState& circuits, uint64_t receiveReservation, LLTransportSteadyUsec steadyNow,
    uint64_t expectedSchemaGeneration, std::atomic<bool>& actorPoisoned,
    const LLTransportIngressScratch& scratch, Arena& arena,
    Queue& queue, const LLTransportIngressEvent& metadata) noexcept
{
    if (!receiveReservation) return LLTransportPublishStatus::StateRejected;
    if (actorPoisoned.load())
    {
        circuits.cancelReceive(receiveReservation);
        return LLTransportPublishStatus::Poisoned;
    }
    // An ACKed message must have an incarnation and a nonzero ordering
    // sequence before state commits. The actor owns monotonic allocation.
    if (!metadata.circuitIncarnation || !metadata.appSequence ||
        metadata.schemaGeneration != expectedSchemaGeneration)
    {
        circuits.cancelReceive(receiveReservation);
        return LLTransportPublishStatus::InvalidRecord;
    }
    LLTransportIngressEvent* slot = queue.reserveProducer();
    if (!slot)
    {
        circuits.cancelReceive(receiveReservation);
        return LLTransportPublishStatus::Backpressure;
    }
    LLTransportDecodedArena::Record record;
    const auto copied = arena.tryCopy(scratch.view(), record);
    if (copied != LLTransportDecodedArena::Result::Ready)
    {
        queue.cancelProducer();
        circuits.cancelReceive(receiveReservation);
        return copied == LLTransportDecodedArena::Result::Full ||
               copied == LLTransportDecodedArena::Result::Busy
            ? LLTransportPublishStatus::Backpressure : LLTransportPublishStatus::InvalidRecord;
    }
    *slot = metadata;
    slot->arenaRecord = record;
    if (circuits.commitReceive(receiveReservation, metadata.wireBytes, steadyNow.value) != CircuitState::Status::Ok)
    {
        arena.cancel(record);
        queue.cancelProducer();
        circuits.cancelReceive(receiveReservation);
        return LLTransportPublishStatus::StateRejected;
    }
    // Both calls operate on reservations held by this sole producer. After
    // state commit, they are infallible unless the actor's ownership invariant
    // is broken. Such failure poisons the actor; it must never service ACKs.
    if (!arena.commit(record) || !queue.publishProducer())
    {
        actorPoisoned.store(true);
        return LLTransportPublishStatus::InvariantFailure;
    }
    return LLTransportPublishStatus::Published;
}

// Main acquires the event before arena.view(event.arenaRecord, view). Its
// borrowed view remains valid through the handler AND reader clearMessage().
// Only then may main release the arena record FIFO, followed by the event cell.
// This disabled WIP does not read sockets, dispatch, or implement ACK service.
#endif
