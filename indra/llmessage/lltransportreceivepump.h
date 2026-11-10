/** Disabled owner RX integration prototype. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTRECEIVEPUMP_H
#define LL_LLTRANSPORTRECEIVEPUMP_H

#include "llpacketring.h"
#include "lltransportcircuitstate.h"
#include "lltransportcontrol.h"
#include "lltransportdecoder.h"
#include "lltransportenvelope.h"
#include "lltransportingress.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

// Main freezes this sorted table and its schemas before the worker starts and
// retains them until every event is drained. Handler registrations remain main.
struct LLTransportSchemaEntry
{
    uint32_t messageNumber = 0;
    const LLTransportSchema* schema = nullptr;
    bool requiresTrusted = false;
    bool internalControl = false;
};

enum class LLTransportPendingKind : uint8_t { InternalControl, BusyAppendedAck };
enum class LLTransportControlApply : uint8_t { Ready, Applied, NeedReplyCapacity, NeedDiagnostics, Deferred, Backpressure, Rejected, Poisoned };

enum class LLTransportRxStatus : uint8_t
{
    Idle, ConsumedDiscarded, Interrupted, SocketError, Published, Duplicate, DataFull, Rejected, ControlPending, Poisoned
};

// Socket owner only. This is a source-integration prototype, NOT an enabled
// viewer path: control servicing, off-circuit UseCircuitCode, the validated
// arrival observation API, timer/fairness scheduling and main reader delivery
// are not implemented here. Do not install it until those paths are complete.
// The containing owner allocates this object and all storage under its ledger.
class LLTransportReceivePump
{
public:
    struct AffectedCircuit
    {
        LLTransportEndpoint endpoint{};
        uint64_t incarnation = 0;
        bool valid = false;
    };

    LLTransportReceivePump(LLPacketRing& ring, LLTransportCircuitState& circuits,
        LLTransportDecodedArena& arena, LLTransportIngressQueue& events,
        std::atomic<bool>& poisoned, const LLTransportSchemaEntry* schemas, std::size_t schemaCount) noexcept
        : mRing(ring), mCircuits(circuits), mArena(arena), mEvents(events), mPoisoned(poisoned),
          mSchemas(schemas), mSchemaCount(schemaCount) {}

    LLTransportRxStatus pollOne(int socket, const LLPacketRing::UDPProxySnapshot& proxy,
                                uint64_t loginGeneration, LLTransportSteadyUsec steadyNow) noexcept
    {
        mAffectedCircuit = {};
        mLastReceiveNativeError = 0;
        // In particular, never call reserveReceive for duplicates after poison:
        // it can add an ACK before any data-slot reservation.
        if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
        if (mControlPending) return LLTransportRxStatus::ControlPending;
        mControlTrailerEffects = false;
        const auto received = mRing.receivePacketOnce(socket, reinterpret_cast<char*>(mWire.data()), proxy);
        mLastReceiveNativeError = received.nativeError;
        using RingStatus = LLPacketRing::OwnerReceiveStatus;
        if (received.status == RingStatus::WouldBlock) return LLTransportRxStatus::Idle;
        if (received.status == RingStatus::ConsumedDiscarded) return LLTransportRxStatus::ConsumedDiscarded;
        if (received.status == RingStatus::Interrupted) return LLTransportRxStatus::Interrupted;
        if (received.status != RingStatus::Delivered) return LLTransportRxStatus::SocketError;
        const S32 size = received.bytes;
        if (size <= 0 || size > NET_BUFFER_SIZE) return LLTransportRxStatus::SocketError;
        const LLHost senderHost = mRing.getLastSender();
        const LLHost interfaceHost = mRing.getLastReceivingInterface();
        if (senderHost.getPort() > 65535u || interfaceHost.getPort() > 65535u)
            return LLTransportRxStatus::Rejected;
        const LLTransportEndpoint sender{senderHost.getAddress(), static_cast<uint16_t>(senderHost.getPort())};
        const LLTransportEndpoint receiving{interfaceHost.getAddress(), static_cast<uint16_t>(interfaceHost.getPort())};
        const auto envelope = llTransportParseEnvelope(mWire.data(), static_cast<std::size_t>(size),
                                                        mExpanded.data(), mExpanded.size());
        if (envelope.status != LLTransportEnvelope::Status::Complete) return LLTransportRxStatus::Rejected;
        return processParsed(envelope, sender, receiving, static_cast<uint32_t>(size),
                             loginGeneration, steadyNow, 0, 0);
    }

    // Owner only, after deferring/resolving the outstanding action. The same
    // packet and its unread ACK index remain held while Busy. Main login and
    // circuit incarnation are revalidated; stale packets never affect a new
    // circuit. Do not service another packet on this circuit while it is held.
    LLTransportRxStatus resumeBusyAcknowledgements(uint64_t currentLoginGeneration) noexcept
    {
        mAffectedCircuit = {};
        if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
        if (!mControlPending || mPendingKind != LLTransportPendingKind::BusyAppendedAck)
            return LLTransportRxStatus::Rejected;
        if (currentLoginGeneration != mControlLoginGeneration)
        {
            mControlPending = false; mControlApplied = false;
            return LLTransportRxStatus::Rejected;
        }
        const auto envelope = mControlEnvelope;
        const auto sender = mControlSender;
        const auto receiving = mControlReceiving;
        const auto size = mControlWireSize;
        const auto incarnation = mControlIncarnation;
        const auto index = mPendingAckIndex;
        const LLTransportSteadyUsec received(mControlReceivedUsec);
        mControlPending = false;
        return processParsed(envelope, sender, receiving, size, currentLoginGeneration,
                             received, incarnation, index);
    }

private:
    LLTransportRxStatus processParsed(const LLTransportEnvelope& envelope, LLTransportEndpoint sender,
        LLTransportEndpoint receiving, uint32_t size, uint64_t loginGeneration,
        LLTransportSteadyUsec steadyNow, uint64_t expectedIncarnation, uint16_t firstAck) noexcept
    {
        const auto circuit = mCircuits.lookup(sender, steadyNow.value);
        // First viewer A/B supports expected peers enabled before their
        // outgoing UseCircuitCode. Generic inbound off-circuit handshakes are
        // not implemented; unknown senders are rejected and not ACKed.
        if (circuit.state != LLTransportSnapshotState::Present ||
            (expectedIncarnation && circuit.incarnation != expectedIncarnation)) return LLTransportRxStatus::Rejected;
        // Reliable control capacity is checked before ANY appended-ACK effect.
        // The sole owner holds this packet until reservation; no other receive
        // can consume the preflighted receive/dedup/ACK slots in between.
        const auto* preflightEntry=findSchema(envelope.messageNumber);
        const bool guardedControl=envelope.reliable && preflightEntry && preflightEntry->internalControl && preflightEntry->schema &&
            preflightEntry->schema->isFrozen() && preflightEntry->schema->messageNumber()==envelope.messageNumber &&
            (!preflightEntry->requiresTrusted || circuit.trusted);
        if (guardedControl)
        {
            const auto available=mCircuits.preflightReliableControl(sender,circuit.incarnation,loginGeneration,
                                                                     envelope.packetId,envelope.resent);
            if (available==LLTransportCircuitState::ReceiveStatus::Full ||
                available==LLTransportCircuitState::ReceiveStatus::DuplicateFull)
            {
                if (mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
                if (available==LLTransportCircuitState::ReceiveStatus::DuplicateFull)
                { incrementSaturating(mControlDuplicateAckBackpressure); }
                else noteControlBackpressure();
                return LLTransportRxStatus::DataFull;
            }
            if (available==LLTransportCircuitState::ReceiveStatus::Invalid) return LLTransportRxStatus::Rejected;
        }
        // A later failure can follow consumed ACKs or an arrival update.
        // Report conservatively before the first possible state mutation.
        mAffectedCircuit = {sender, circuit.incarnation, true};
        // Legacy applies appended ACKs to the matching circuit before schema
        // validation. State completion capacity must already be reserved by
        // its outgoing admission; failure is terminal, never silently ignored.
        for (uint16_t i = firstAck; i < envelope.ackCount; ++i)
        {
            uint32_t id = 0;
            if (!llTransportAppendedAck(mWire.data(), static_cast<std::size_t>(size), envelope, static_cast<uint8_t>(i), id))
            { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            const auto result = mCircuits.consumeAck(sender, circuit.incarnation, id);
            if (result==LLTransportCircuitState::Status::Ok) mControlTrailerEffects=true;
            if (result == LLTransportCircuitState::Status::Busy)
            {
                // Keep the original bytes and next unconsumed ACK index.
                // Never replay an earlier ACK against a later reused ID.
                mControlEnvelope = envelope; mControlSender = sender;
                mControlIncarnation = circuit.incarnation;
                mControlReceiving = receiving; mControlWireSize = static_cast<uint32_t>(size);
                mControlLoginGeneration = loginGeneration; mControlReceivedUsec = steadyNow.value;
                mPendingAckIndex = i;
                mPendingKind = LLTransportPendingKind::BusyAppendedAck;
                mControlPending = true;
                return LLTransportRxStatus::ControlPending;
            }
            // Unknown/out-of-range IDs have no matching legacy reliable
            // packet. A bad remote ACK must not poison the entire owner.
            if (result != LLTransportCircuitState::Status::Ok &&
                result != LLTransportCircuitState::Status::Absent &&
                result != LLTransportCircuitState::Status::Invalid)
                return LLTransportRxStatus::Rejected;
        }
        // Legacy discards committed resent IDs before template/trust lookup.
        const auto classification = mCircuits.classifyResentDuplicate(
            sender, circuit.incarnation, loginGeneration, envelope.packetId, envelope.resent);
        if (classification == LLTransportCircuitState::DuplicateClass::Duplicate)
        {
            if (!envelope.reliable) return LLTransportRxStatus::Duplicate;
            if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
            const auto ack = mCircuits.acknowledgeCommittedDuplicate(
                sender, circuit.incarnation, loginGeneration, envelope.packetId, steadyNow.value);
            if (ack == LLTransportCircuitState::ReceiveStatus::Duplicate) return LLTransportRxStatus::Duplicate;
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return ack == LLTransportCircuitState::ReceiveStatus::Full
                ? LLTransportRxStatus::DataFull : LLTransportRxStatus::Rejected;
        }
        if (classification == LLTransportCircuitState::DuplicateClass::Busy)
        {
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return LLTransportRxStatus::DataFull;
        }
        if (classification != LLTransportCircuitState::DuplicateClass::NotDuplicate)
        {
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return LLTransportRxStatus::Rejected;
        }
        const LLTransportSchemaEntry* entry = findSchema(envelope.messageNumber);
        if (!entry || !entry->schema || !entry->schema->isFrozen() ||
            entry->schema->messageNumber() != envelope.messageNumber ||
            (entry->requiresTrusted && !circuit.trusted)) return LLTransportRxStatus::Rejected;
        // Record validated nonduplicate traffic before attempting data
        // retention: even a full APP queue must refresh activity, while never
        // ACKing or retaining a fresh packet until admission succeeds.
        const auto observed = mCircuits.observeValidatedArrival(sender, circuit.incarnation, loginGeneration,
            envelope.packetId, envelope.resent, static_cast<uint32_t>(size), steadyNow.value);
        if (observed.status == LLTransportCircuitState::ArrivalStatus::Duplicate)
        {
            if (!envelope.reliable) return LLTransportRxStatus::Duplicate;
            if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
            const auto duplicate = mCircuits.acknowledgeCommittedDuplicate(
                sender, circuit.incarnation, loginGeneration, envelope.packetId, steadyNow.value);
            if (duplicate == LLTransportCircuitState::ReceiveStatus::Duplicate) return LLTransportRxStatus::Duplicate;
            return duplicate == LLTransportCircuitState::ReceiveStatus::Full
                ? LLTransportRxStatus::DataFull : LLTransportRxStatus::Rejected;
        }
        if ((observed.status != LLTransportCircuitState::ArrivalStatus::Observed &&
             observed.status != LLTransportCircuitState::ArrivalStatus::PartialLoss) || !observed.observation)
        {
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return observed.status == LLTransportCircuitState::ArrivalStatus::Busy
                ? LLTransportRxStatus::DataFull : LLTransportRxStatus::Rejected;
        }
        if (entry->internalControl)
        {
            // One owner-held control datagram, not an application queue slot.
            // The owner must consume it before the next poll; no viewer pointer
            // or handler is called here. No ACK for the new message is implied.
            mPendingKind = LLTransportPendingKind::InternalControl;
            mControlEnvelope = envelope;
            mControlSender = sender;
            mControlIncarnation = circuit.incarnation;
            mControlLoginGeneration = loginGeneration; mControlObservation = observed.observation;
            mControlReceiving = receiving; mControlWireSize = static_cast<uint32_t>(size);
            mControlReceivedUsec = steadyNow.value;
            mControlAckIndex = 0; mControlReservation = 0; mControlRequiresReply = false;
            mControlWarningRetained = false;
            mControlApplied = false;
            mControlPending = true;
            return LLTransportRxStatus::ControlPending;
        }
        if (mNextAppSequence == 0) return LLTransportRxStatus::DataFull;
        const auto receive = mCircuits.reserveObservedReceive(sender, circuit.incarnation, loginGeneration,
            envelope.packetId, envelope.resent, envelope.reliable, steadyNow.value, observed.observation);
        if (receive.status == LLTransportCircuitState::ReceiveStatus::Full) return LLTransportRxStatus::DataFull;
        if (receive.status != LLTransportCircuitState::ReceiveStatus::New || !receive.reservation)
            return LLTransportRxStatus::Rejected;
        const auto decoded = decodeTransportFields(*entry->schema, mExpanded.data(), envelope.expandedSize,
                                                   envelope.bodyOffset, mScratch);
        if (decoded.status != LLTransportDecodeStatus::Complete)
        {
            mCircuits.cancelReceive(receive.reservation);
            return LLTransportRxStatus::Rejected;
        }
        LLTransportIngressEvent event;
        event.sender = sender; event.receivingInterface = receiving;
        event.circuitIncarnation = circuit.incarnation; event.appSequence = mNextAppSequence;
        event.schemaGeneration = entry->schema->generation(); event.packetId = envelope.packetId;
        event.messageNumber = envelope.messageNumber; event.wireBytes = static_cast<uint32_t>(size);
        event.expandedBytes = static_cast<uint32_t>(envelope.expandedSize);
        event.compressedBytes = envelope.compressed ? static_cast<uint32_t>(envelope.wireMessageSize) : 0;
        event.reliable = envelope.reliable;
        event.firstTruncationOffset = decoded.firstTruncationOffset;
        event.firstTruncationWanted = decoded.firstTruncationWanted; event.truncations = decoded.truncations;
        const auto result = llTransportPublishDecoded(mCircuits, receive.reservation, steadyNow,
            entry->schema->generation(), mPoisoned, mScratch, mArena, mEvents, event);
        if (result == LLTransportPublishStatus::Published)
        {
            mLastPublishedAppSequence = mNextAppSequence;
            mNextAppSequence = mNextAppSequence == (std::numeric_limits<uint64_t>::max)() ? 0 : mNextAppSequence + 1;
            return LLTransportRxStatus::Published;
        }
        if (result == LLTransportPublishStatus::InvariantFailure || result == LLTransportPublishStatus::Poisoned)
            return LLTransportRxStatus::Poisoned;
#if defined(LL_TRANSPORT_PUMP_TESTING)
        if (result == LLTransportPublishStatus::Backpressure)
        {
            ++mTestAppPublishBackpressure;
            const auto phase=mTestStallPhase.load(std::memory_order_acquire);
            if (phase==1) ++mTestStalledAppBackpressure;
            else if (phase==2) mTestStallPhase.store(3,std::memory_order_release);
        }
#endif
        return result == LLTransportPublishStatus::Backpressure ? LLTransportRxStatus::DataFull : LLTransportRxStatus::Rejected;
    }

public:

    // Owner-only: decode an already held internal-control packet without
    // dispatching a handler. Nonzero truncation diagnostics must be routed
    // according to the main exception ordering contract before treating the
    // message as accepted. The caller retains this packet until response
    // capacity and circuit identity are revalidated.
    bool decodePendingControl(LLTransportControlValues& values, LLTransportDecodeResult& diagnostic) noexcept
    {
        if (mPoisoned.load() || !mControlPending || mPendingKind != LLTransportPendingKind::InternalControl)
            return false;
        const auto* entry = findSchema(mControlEnvelope.messageNumber);
        if (!entry || !entry->schema || !entry->schema->isFrozen() ||
            entry->schema->messageNumber() != mControlEnvelope.messageNumber) return false;
        const auto result = decodeTransportFields(*entry->schema, mExpanded.data(), mControlEnvelope.expandedSize,
                                                   mControlEnvelope.bodyOffset, mScratch);
        if (result.status != LLTransportDecodeStatus::Complete) return false;
        LLTransportControlValues decoded;
        if (!llTransportDecodeControl(mControlEnvelope.messageNumber, mScratch.view(), decoded)) return false;
        diagnostic = result; values = decoded;
        return true;
    }

    // Owner folds this value into independent bounded warning statistics.
    // No viewer callback, logging or template pointer crosses the boundary.
    bool pendingControlWarning(LLTransportControlWarning& out) noexcept
    {
        LLTransportControlValues values; LLTransportDecodeResult diagnostic;
        if (!decodePendingControl(values,diagnostic) || !diagnostic.truncations || !mControlObservation) return false;
        LLTransportControlWarning warning;
        warning.endpoint=mControlSender; warning.loginGeneration=mControlLoginGeneration;
        warning.incarnation=mControlIncarnation; warning.observation=mControlObservation;
        warning.firstOffset=diagnostic.firstTruncationOffset; warning.firstWanted=diagnostic.firstTruncationWanted;
        warning.messageNumber=mControlEnvelope.messageNumber; warning.packetId=mControlEnvelope.packetId;
        warning.truncations=diagnostic.truncations; out=warning; return true;
    }
    bool controlWarningRetainedOnOwner() const noexcept { return mControlWarningRetained; }
    // Owner shutdown only, after RX has stopped. This is deliberately usable
    // after poison and is idempotent; it never commits an ACK or replays any
    // already applied trailer. The pump must not be polled after shutdown.
    void cancelHeldForStop() noexcept
    {
        if (mControlReservation) mCircuits.cancelReceive(mControlReservation);
        mControlReservation=0; mControlPending=false; mControlApplied=false;
        mControlRequiresReply=false; mControlWarningRetained=false;
        mControlAckIndex=0; mPendingAckIndex=0; mControlTrailerEffects=false;
        mControlObservation=0; mAffectedCircuit={};
    }
    // Only after that exact warning has been accounted in the coalesced
    // publication, and only under the frozen no-callback policy. Failure
    // preserves the held input.
    bool markControlWarningRetainedOnOwner(const LLTransportControlWarning& warning) noexcept
    {
        LLTransportControlWarning current;
        if (mControlApplied || mControlWarningRetained || !pendingControlWarning(current) ||
            current.endpoint.addressNetworkOrder!=warning.endpoint.addressNetworkOrder ||
            current.endpoint.portHostOrder!=warning.endpoint.portHostOrder || current.loginGeneration!=warning.loginGeneration ||
            current.incarnation!=warning.incarnation || current.observation!=warning.observation ||
            current.messageNumber!=warning.messageNumber || current.packetId!=warning.packetId ||
            current.firstOffset!=warning.firstOffset || current.firstWanted!=warning.firstWanted ||
            current.truncations!=warning.truncations) return false;
        mControlWarningRetained=true; return true;
    }

    // Reserve before effects. replyReserved means a bounded StartPing reply
    // slot is already retained. The legacy diagnosticsHandled boolean is not
    // accepted as event identity; a truncated control requires its exact
    // warning to have been published under the frozen no-callback policy.
    // A reservation stays attached to the held event across Deferred retries.
    LLTransportControlApply preparePendingControl(LLTransportSteadyUsec steadyNow,
                                                   bool replyReserved, bool diagnosticsHandled) noexcept
    {
        (void)diagnosticsHandled; // a global boolean cannot identify a retained event
        if (mPoisoned.load()) return LLTransportControlApply::Poisoned;
        if (mControlApplied) return LLTransportControlApply::Rejected;
        LLTransportControlValues value; LLTransportDecodeResult diagnostic;
        if (!decodePendingControl(value,diagnostic)) return LLTransportControlApply::Rejected;
        if (diagnostic.truncations && !mControlWarningRetained) return LLTransportControlApply::NeedDiagnostics;
        const auto current=mCircuits.lookup(mControlSender,steadyNow.value);
        if (current.state!=LLTransportSnapshotState::Present || !current.alive ||
            current.incarnation!=mControlIncarnation) return LLTransportControlApply::Rejected;
        mControlRequiresReply=value.kind==LLTransportControlValues::Kind::StartPing;
        if (mControlRequiresReply && !replyReserved) return LLTransportControlApply::NeedReplyCapacity;
        if (!mControlEnvelope.reliable || mControlReservation) return LLTransportControlApply::Ready;
        const auto receive=mCircuits.reserveObservedReceive(mControlSender,mControlIncarnation,
            mControlLoginGeneration,mControlEnvelope.packetId,mControlEnvelope.resent,true,
            steadyNow.value,mControlObservation);
        if (receive.status==LLTransportCircuitState::ReceiveStatus::Full)
        {
            // Unexpected loss of preflight capacity must never replay an
            // already applied trailer/body ACK against a later reused ID.
            if (mControlTrailerEffects || mControlAckIndex)
            { mPoisoned.store(true); return LLTransportControlApply::Poisoned; }
            noteControlBackpressure(); mControlPending=false; mControlRequiresReply=false;
            mControlWarningRetained=false;
            return LLTransportControlApply::Backpressure;
        }
        if (receive.status!=LLTransportCircuitState::ReceiveStatus::New || !receive.reservation)
            return LLTransportControlApply::Rejected;
        mControlReservation=receive.reservation;
        return LLTransportControlApply::Ready;
    }

    // Apply only after reply/diagnostic prerequisites and reliable receive
    // admission. Partial PacketAck progress survives Busy/Full retry.
    LLTransportControlApply applyPendingControl(LLTransportSteadyUsec steadyNow, uint64_t heartbeatUsec,
                                                bool replyReserved, bool diagnosticsHandled) noexcept
    {
        mAffectedCircuit = {};
        const auto prepared=preparePendingControl(steadyNow,replyReserved,diagnosticsHandled);
        if (prepared!=LLTransportControlApply::Ready) return prepared;
        LLTransportControlValues value; LLTransportDecodeResult diagnostic;
        if (!decodePendingControl(value, diagnostic)) return LLTransportControlApply::Rejected;
        if (diagnostic.truncations && !mControlWarningRetained) return LLTransportControlApply::NeedDiagnostics;
        const auto current = mCircuits.lookup(mControlSender, steadyNow.value);
        if (current.state != LLTransportSnapshotState::Present || current.incarnation != mControlIncarnation)
            return LLTransportControlApply::Rejected;
        mAffectedCircuit = {mControlSender, mControlIncarnation, true};
        using Status = LLTransportCircuitState::Status;
        if (value.kind == LLTransportControlValues::Kind::StartPing)
        {
            if (!replyReserved) return LLTransportControlApply::NeedReplyCapacity;
            const auto status = mCircuits.forgetReceivedBefore(mControlSender, mControlIncarnation,
                                                                value.oldestUnacked, steadyNow.value);
            if (status == Status::Busy || status == Status::Full) return LLTransportControlApply::Deferred;
            if (status != Status::Ok) return LLTransportControlApply::Rejected;
        }
        else if (value.kind == LLTransportControlValues::Kind::CompletePing)
        {
            const auto status = mCircuits.notePingReply(mControlSender, mControlIncarnation,
                                                         value.pingId, heartbeatUsec, steadyNow.value);
            // Other circuits can publish after preflight. Sequence exhaustion
            // cannot resolve by retrying this held control: fail closed and
            // let owner shutdown cancel its uncommitted reservation.
            if (status == Status::Full) { mPoisoned.store(true); return LLTransportControlApply::Poisoned; }
            if (status == Status::Busy) return LLTransportControlApply::Deferred;
            if (status != Status::Ok) return LLTransportControlApply::Rejected;
        }
        else if (value.kind == LLTransportControlValues::Kind::PacketAck)
        {
            for (; mControlAckIndex < value.ackCount; ++mControlAckIndex)
            {
                const auto status = mCircuits.consumeAck(mControlSender, mControlIncarnation, value.acknowledgements[mControlAckIndex]);
                if (status == Status::Busy || status == Status::Full) return LLTransportControlApply::Deferred;
                if (status != Status::Ok && status != Status::Absent && status != Status::Invalid)
                    return LLTransportControlApply::Rejected;
            }
        }
        else return LLTransportControlApply::Rejected;
        mControlApplied = true;
        return LLTransportControlApply::Applied;
    }

    // Call immediately after successful apply, in the SAME owner turn and
    // with no interleaved close/timeout. StartPing's bounded reply must still
    // be retained; only after this returns Applied may it become sendable.
    // Any impossible commit failure poisons rather than ACKing/forgetting.
    LLTransportControlApply commitPendingControl(LLTransportSteadyUsec steadyNow, bool replyRetained) noexcept
    {
        if (mPoisoned.load()) return LLTransportControlApply::Poisoned;
        if (!mControlPending || mPendingKind!=LLTransportPendingKind::InternalControl || !mControlApplied)
            return LLTransportControlApply::Rejected;
        if (mControlRequiresReply && !replyRetained) return LLTransportControlApply::NeedReplyCapacity;
        if (mControlEnvelope.reliable)
        {
            if (!mControlReservation || mCircuits.commitReceive(mControlReservation,mControlWireSize,steadyNow.value)!=LLTransportCircuitState::Status::Ok)
            { mPoisoned.store(true); return LLTransportControlApply::Poisoned; }
            mControlReservation=0;
        }
        mControlPending=false; mControlApplied=false; mControlAckIndex=0; mControlRequiresReply=false; mControlWarningRetained=false;
        return LLTransportControlApply::Applied;
    }

    // Owner must interpret control without a viewer handler, preserve identity,
    // and clear only after it has retained every required control response.
    LLTransportPendingKind pendingKind() const noexcept { return mPendingKind; }
    // Value for the most recent poll; meaningful on Interrupted/SocketError.
    S32 lastReceiveNativeError() const noexcept { return mLastReceiveNativeError; }
    // Owner-only value report for the immediately preceding poll, resume or
    // control application. Republish this circuit before ending the directory
    // mutation bracket, even when that operation returned a failure.
    AffectedCircuit affectedCircuit() const noexcept { return mAffectedCircuit; }
    const LLTransportEnvelope& controlEnvelope() const noexcept { return mControlEnvelope; }
    LLTransportEndpoint controlSender() const noexcept { return mControlSender; }
    LLTransportEndpoint controlReceivingInterface() const noexcept { return mControlReceiving; }
    const uint8_t* pendingWireBytes() const noexcept { return mWire.data(); }
    uint32_t pendingWireSize() const noexcept { return mControlWireSize; }
    uint64_t controlIncarnation() const noexcept { return mControlIncarnation; }
    uint64_t controlLoginGeneration() const noexcept { return mControlLoginGeneration; }
    uint64_t controlObservation() const noexcept { return mControlObservation; }
    // Independently sampled monotonic counters, including for main-thread
    // diagnostics after join. One owner writes; neither counter can wrap.
    uint64_t controlBackpressureOnOwner() const noexcept { return mControlBackpressure.load(std::memory_order_relaxed); }
    uint64_t controlDuplicateAckBackpressureOnOwner() const noexcept { return mControlDuplicateAckBackpressure.load(std::memory_order_relaxed); }
#if defined(LL_TRANSPORT_PUMP_TESTING)
    // Test reads only after the owner has joined.
    uint64_t testAppPublishBackpressureAfterJoin() const noexcept { return mTestAppPublishBackpressure; }
    uint64_t testStalledAppBackpressureAfterJoin() const noexcept { return mTestStalledAppBackpressure; }
    void testBeginMainStall() noexcept { mTestStallPhase.store(1,std::memory_order_release); }
    void testEndMainStall() noexcept { mTestStallPhase.store(2,std::memory_order_release); }
    bool testMainStallEnded() const noexcept { return mTestStallPhase.load(std::memory_order_acquire)==3; }
#endif
    const uint8_t* controlBytes() const noexcept { return mExpanded.data(); }
    // Legacy decodeData returns false for a template with blocks but no
    // decoded blocks. Only that proven terminal result is discardable here;
    // capacity, invalid schema and control-layout errors need separate fault
    // handling, and reliable or partially processed input must stay held.
    bool discardEmptyUnreliableControl(bool diagnosticsHandled) noexcept
    {
        mAffectedCircuit = {};
        if (mPoisoned.load() || !mControlPending || mControlApplied ||
            mPendingKind != LLTransportPendingKind::InternalControl || mControlEnvelope.reliable) return false;
        const auto* entry = findSchema(mControlEnvelope.messageNumber);
        if (!entry || !entry->schema || !entry->schema->isFrozen() ||
            entry->schema->messageNumber() != mControlEnvelope.messageNumber) return false;
        const auto result = decodeTransportFields(*entry->schema, mExpanded.data(), mControlEnvelope.expandedSize,
                                                   mControlEnvelope.bodyOffset, mScratch);
        if (result.status != LLTransportDecodeStatus::EmptyMessage ||
            (result.truncations && !diagnosticsHandled)) return false;
        if (mControlReservation) { mCircuits.cancelReceive(mControlReservation); mControlReservation=0; }
        mControlAckIndex=0; mControlRequiresReply=false; mControlWarningRetained=false; mControlPending = false;
        return true;
    }
    // Owner-only. Discard only an unapplied control whose identity is no
    // longer current. Cancel its uncommitted receive/ACK reservation; prior
    // applied PacketAck entries are never replayed in this incarnation.
    bool discardPendingIfStale(uint64_t currentLoginGeneration, LLTransportSteadyUsec now) noexcept
    {
        mAffectedCircuit = {};
        if (mPoisoned.load() || !currentLoginGeneration || !mControlLoginGeneration ||
            !mControlPending || mControlApplied ||
            mPendingKind != LLTransportPendingKind::InternalControl) return false;
        const auto current = mCircuits.lookup(mControlSender, now.value);
        if (currentLoginGeneration == mControlLoginGeneration &&
            current.state == LLTransportSnapshotState::Unavailable) return false;
        if (currentLoginGeneration == mControlLoginGeneration &&
            current.state == LLTransportSnapshotState::Present && current.alive &&
            current.incarnation == mControlIncarnation) return false;
        if (mControlReservation) { mCircuits.cancelReceive(mControlReservation); mControlReservation=0; }
        mControlAckIndex=0; mControlRequiresReply=false; mControlPending = false;
        return true;
    }
    // Compatibility for the existing unreliable-only loop. Reliable controls
    // must use commitPendingControl so the reserved ACK is committed once.
    bool releaseControl() noexcept
    {
        if (!mControlPending || mPendingKind != LLTransportPendingKind::InternalControl ||
            !mControlApplied || mControlEnvelope.reliable) return false;
        mControlPending = false; mControlApplied = false; mControlAckIndex=0; mControlRequiresReply=false; mControlWarningRetained=false; return true;
    }
    uint64_t lastPublishedAppSequence() const noexcept { return mLastPublishedAppSequence; }

private:
    static void incrementSaturating(std::atomic<uint64_t>& counter) noexcept
    {
        const auto value=counter.load(std::memory_order_relaxed);
        if (value!=(std::numeric_limits<uint64_t>::max)()) counter.store(value+1,std::memory_order_relaxed);
    }
    void noteControlBackpressure() noexcept { incrementSaturating(mControlBackpressure); }
    const LLTransportSchemaEntry* findSchema(uint32_t number) const noexcept
    {
        if (!mSchemas) return nullptr;
        std::size_t lo = 0, hi = mSchemaCount;
        while (lo < hi) { const auto mid = lo + (hi-lo)/2; if (mSchemas[mid].messageNumber < number) lo=mid+1; else hi=mid; }
        return lo < mSchemaCount && mSchemas[lo].messageNumber == number ? &mSchemas[lo] : nullptr;
    }
    LLPacketRing& mRing;
    LLTransportCircuitState& mCircuits;
    LLTransportDecodedArena& mArena;
    LLTransportIngressQueue& mEvents;
    std::atomic<bool>& mPoisoned;
    const LLTransportSchemaEntry* mSchemas;
    std::size_t mSchemaCount;
    std::array<uint8_t, NET_BUFFER_SIZE> mWire{}, mExpanded{};
    LLTransportIngressScratch mScratch{};
    LLTransportEnvelope mControlEnvelope{};
    S32 mLastReceiveNativeError = 0;
    AffectedCircuit mAffectedCircuit{};
    LLTransportEndpoint mControlSender{}, mControlReceiving{};
    uint32_t mControlWireSize = 0;
    uint64_t mControlIncarnation = 0, mControlLoginGeneration = 0, mControlObservation = 0;
    uint64_t mControlReceivedUsec = 0;
    uint64_t mControlReservation = 0;
    std::atomic<uint64_t> mControlBackpressure{0}, mControlDuplicateAckBackpressure{0};
#if defined(LL_TRANSPORT_PUMP_TESTING)
    std::atomic<uint8_t> mTestStallPhase{0};
    uint64_t mTestAppPublishBackpressure=0, mTestStalledAppBackpressure=0; // read after join
#endif
    uint32_t mControlAckIndex = 0;
    uint16_t mPendingAckIndex = 0;
    uint64_t mNextAppSequence = 1, mLastPublishedAppSequence = 0;
    LLTransportPendingKind mPendingKind = LLTransportPendingKind::InternalControl;
    bool mControlPending = false, mControlApplied = false, mControlRequiresReply = false, mControlWarningRetained = false;
    bool mControlTrailerEffects = false;
};
#endif
