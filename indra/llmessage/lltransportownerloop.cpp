/** Disabled owner-loop implementation, under construction. SPDX-License-Identifier: LGPL-2.1-only */
#include "linden_common.h"
#include "lltransportownerloop.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#if LL_WINDOWS
#include <winsock2.h>
#else
#include <cerrno>
#endif

namespace
{
// Actual message-template forms: PacketAck Fixed 0xFFFFFFFB and
// StartPingCheck High 1. Message header packet IDs are network order; U32
// template fields are little-endian (LLMsgVarData::addData/htolememcpy).
// The surrounding owner loop must bind a state action before using its ID,
// then reserve piggyback ACKs and apply send-attempt/credit semantics.
struct EncodedAction
{
    std::array<uint8_t, LLTransportCircuitState::MaxPacketBytes> bytes{};
    uint32_t size = 0;
};

void putNetworkU32(uint8_t* dst, uint32_t value) noexcept
{
    dst[0]=uint8_t(value>>24); dst[1]=uint8_t(value>>16);
    dst[2]=uint8_t(value>>8); dst[3]=uint8_t(value);
}
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::processCircuitHead(
    LLTransportSteadyUsec now, LLTransportCircuitState::WatchdogConfig watchdog, Directory* directory) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    const LLTransportOwnerCommand* command=mChannels.commands.peekConsumer();
    if (!command) return Step::Idle;
    using Kind=LLTransportOwnerCommand::Kind;
    if (command->kind==Kind::Send) return Step::SendPending;
    if (command->kind==Kind::Stop) return Step::StopPending;
    if (command->kind==Kind::Enable && (!mWatches || !directory)) return Step::Blocked;
    auto* terminal=mChannels.circuitTerminals.reserveProducer();
    if (!terminal) return Step::Blocked;
    LLTransportCircuitCompletion result{};
    const auto& circuit=command->circuit;
    result.endpoint=circuit.endpoint; result.operationId=circuit.operationId;
    result.loginGeneration=circuit.loginGeneration; result.circuitIncarnation=circuit.targetIncarnation;
    if (command->kind==Kind::SetAckCollectTime)
    {
        // Configuration completions have no circuit identity or APP barrier;
        // irrelevant fields in the command must not leak into the terminal.
        result={};
        result.operationId=command->configOperationId;
        if (!command->configOperationId || command->ackCollectUsec>2000000)
            result.status=LLTransportCircuitCommandStatus::Invalid;
        else
        {
            mAckCollectUsec=command->ackCollectUsec;
            result.status=LLTransportCircuitCommandStatus::Applied;
        }
    }
    else if (command->kind==Kind::FinishWatchdogTimeout)
    {
        const auto& event=command->timeout;
        result.timeoutEventId=event.eventId;
        result.endpoint=event.endpoint; result.loginGeneration=event.loginGeneration;
        result.circuitIncarnation=event.incarnation; result.lastPublishedAppSequence=event.lastPublishedAppSequence;
        const auto status=(!result.operationId || !event.eventId)
            ? LLTransportCircuitState::Status::Invalid
            : mState.finishWatchdogTimeout(event,watchdog.initialJitterDraw);
        using Status=LLTransportCircuitState::Status;
        result.status=status==Status::Ok ? LLTransportCircuitCommandStatus::Applied :
            (status==Status::Stale || status==Status::Absent) ? LLTransportCircuitCommandStatus::IncarnationChanged :
            status==Status::Invalid ? LLTransportCircuitCommandStatus::Invalid : LLTransportCircuitCommandStatus::Unavailable;
        if (status==Status::Ok)
        {
            const auto current=mState.lookup(event.endpoint,now.value);
            if (current.state!=LLTransportSnapshotState::Present || current.incarnation!=event.incarnation)
                discardIncarnation(event.incarnation);
        }
    }
    else if (command->kind==Kind::Enable && circuit.kind==LLTransportCircuitCommandKind::Enable)
    {
        Watch* watch=nullptr; Watch* vacant=nullptr;
        for (uint32_t i=0;i<mWatchCapacity;++i)
        {
            auto& item=mWatches[i];
            if (!item.incarnation) { if (!vacant) vacant=&item; continue; }
            if (item.endpoint.addressNetworkOrder==circuit.endpoint.addressNetworkOrder &&
                item.endpoint.portHostOrder==circuit.endpoint.portHostOrder) { watch=&item; break; }
        }
        if (!watch) watch=vacant;
        if (!watch) result.status=LLTransportCircuitCommandStatus::Unavailable;
        else
        {
            const auto enabled=mState.enable(circuit.endpoint,circuit.loginGeneration,circuit.trusted,now.value,watchdog);
            result.circuitIncarnation=enabled.incarnation;
            using Status=LLTransportCircuitState::Status;
            result.status=enabled.status==Status::Ok ? LLTransportCircuitCommandStatus::Applied :
                enabled.status==Status::Stale ? LLTransportCircuitCommandStatus::IncarnationChanged :
                enabled.status==Status::Invalid ? LLTransportCircuitCommandStatus::Invalid :
                LLTransportCircuitCommandStatus::Unavailable;
            if (enabled.status==Status::Ok) *watch={circuit.endpoint,enabled.incarnation};
        }
    }
    else if (command->kind==Kind::Disable && circuit.kind==LLTransportCircuitCommandKind::Disable)
        result=mState.disable(circuit,mReceive.lastPublishedAppSequence());
    else if (command->kind==Kind::DisableIfUnchanged && circuit.kind==LLTransportCircuitCommandKind::DisableIfUnchanged)
        result=mState.conditionalClose(circuit,mReceive.lastPublishedAppSequence());
    else result.status=LLTransportCircuitCommandStatus::Invalid;
    if (result.status==LLTransportCircuitCommandStatus::Applied &&
        (command->kind==Kind::Disable || command->kind==Kind::DisableIfUnchanged))
        discardIncarnation(result.circuitIncarnation);
    if (command->kind==Kind::Enable && result.status==LLTransportCircuitCommandStatus::Applied)
    {
        LLTransportCircuitSnapshot snapshot;
        bool complete=false;
        const auto status=mState.lookupWithLossTracking(result.endpoint,now.value,snapshot,complete);
        if (status!=LLTransportCircuitState::Status::Ok || snapshot.incarnation!=result.circuitIncarnation)
        { mChannels.circuitTerminals.cancelProducer(); directory->failClosedOnOwner(); mPoisoned.store(true); return Step::Poisoned; }
        snapshot.lossQuality=complete ? LLTransportLossQuality::Complete : LLTransportLossQuality::Partial;
        if (!directory->reconcileStateOnOwner(result.endpoint,snapshot))
        { mChannels.circuitTerminals.cancelProducer(); directory->failClosedOnOwner(); mPoisoned.store(true); return Step::Poisoned; }
    }
    *terminal=result;
    // Publication is allocation-free after reserve; an invariant failure is
    // terminal because the circuit mutation may already have happened.
    if (!mChannels.circuitTerminals.publishProducer() || !mChannels.commands.releaseConsumer())
    { mPoisoned.store(true); return Step::Poisoned; }
    if (mWake) mWake->signal();
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::publishReliableCompletion() noexcept
{
    // Completion delivery is also needed after poison/stop; it performs no
    // socket I/O and must not strand already accepted callback credits.
    if (!mState.hasCompletion()) return Step::Idle;
    auto* terminal=mChannels.sendTerminals.reserveProducer();
    if (!terminal) return Step::Blocked;
    LLTransportCircuitState::Completion value;
    if (!mState.takeCompletion(value)) { mChannels.sendTerminals.cancelProducer(); return Step::Idle; }
    discardReliable(value.incarnation,value.token);
    if (mFirstAttemptReliable && mFirstAttemptToken==value.token && mFirstAttemptIncarnation==value.incarnation)
    { mFirstAttemptToken=0; mFirstAttemptIncarnation=0; mFirstAttemptReliable=false; }
    *terminal={value.endpoint,value.token,value.loginGeneration,value.incarnation,value.result};
    if (!mChannels.sendTerminals.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
    if (mWake) mWake->signal();
    return Step::Progress;
}

LLTransportRxStatus LLTransportOwnerLoop::pollReceive(int socket,
    const LLPacketRing::UDPProxySnapshot& proxy, uint64_t loginGeneration, LLTransportSteadyUsec now) noexcept
{
    if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
    const auto result=mReceive.pollOne(socket,proxy,loginGeneration,now);
    mReceiveHeld=result==LLTransportRxStatus::ControlPending;
    if (result==LLTransportRxStatus::Published && mWake) mWake->signal();
    return result;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::servicePendingControl(
    LLTransportSteadyUsec now, uint64_t heartbeatUsec, bool diagnosticsHandled) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    if (mReceive.pendingKind()!=LLTransportPendingKind::InternalControl)
        return Step::Blocked;
    LLTransportControlValues values;
    LLTransportDecodeResult diagnostics;
    if (!mReceive.decodePendingControl(values,diagnostics)) return Step::Blocked;
    if (values.kind!=LLTransportControlValues::Kind::CompletePing && values.kind!=LLTransportControlValues::Kind::PacketAck)
        return Step::Blocked;
    const auto status=mReceive.applyPendingControl(now,heartbeatUsec,false,diagnosticsHandled);
    if (status==LLTransportControlApply::Poisoned) return Step::Poisoned;
    if (status==LLTransportControlApply::Backpressure) return finishControlBackpressure();
    if (status!=LLTransportControlApply::Applied) return Step::Blocked;
    if (mReceive.commitPendingControl(now,false)!=LLTransportControlApply::Applied)
    { mPoisoned.store(true); return Step::Poisoned; }
    mReceiveHeld=false;
    mReadTurnNeeded=true;
    return Step::Progress;
}

LLTransportRxStatus LLTransportOwnerLoop::resumeBusyAcknowledgements(uint64_t loginGeneration) noexcept
{
    if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
    const auto result=mReceive.resumeBusyAcknowledgements(loginGeneration);
    mReceiveHeld=result==LLTransportRxStatus::ControlPending;
    if (result==LLTransportRxStatus::Published && mWake) mWake->signal();
    return result;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::pollWatchdog(LLTransportEndpoint endpoint,
    uint64_t incarnation, LLTransportSteadyUsec now, float jitterDraw) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    // A dead circuit would make an already-admitted head wait for revival,
    // while revival is ordered behind that head. The owner resolves the held
    // receive transaction and retries the head before timing this circuit out.
    if (mAdmittedHeadToken && mAdmittedHeadIncarnation==incarnation) return Step::Blocked;
    if (mReceiveHeld && mReceive.controlIncarnation()==incarnation) return Step::Blocked;
    auto* slot=mChannels.controls.reserveProducer();
    const auto result=mState.pollWatchdog(endpoint,incarnation,now.value,slot!=nullptr,
                                           mReceive.lastPublishedAppSequence(),jitterDraw);
    using Status=LLTransportCircuitState::WatchdogStatus;
    if (result.status==Status::Timeout)
    {
        if (!slot) { mPoisoned.store(true); return Step::Poisoned; }
        const auto& event=result.event;
        *slot={LLTransportOwnerControl::Kind::Timeout,event.endpoint,event.loginGeneration,
               event.incarnation,event.eventId,event.lastPublishedAppSequence};
        discardControlIncarnation(event.incarnation);
        if (!mChannels.controls.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
        if (mWake) mWake->signal();
        return Step::Progress;
    }
    if (slot) mChannels.controls.cancelProducer();
    if (result.status==Status::PingReady) return Step::Progress;
    if (result.status==Status::NotDue || result.status==Status::Stale) return Step::Idle;
    return Step::Blocked;
}

std::size_t LLTransportOwnerLoop::watchdogChargeBytes(uint32_t capacity) noexcept
{ return capacity<=1024 ? std::size_t(capacity)*sizeof(Watch) : 0; }

bool LLTransportOwnerLoop::initializeWatchdogs(uint32_t capacity) noexcept
{
    if (!capacity || capacity>1024 || mWatches) return false;
    auto watches=std::unique_ptr<Watch[]>(new(std::nothrow) Watch[capacity]);
    if (!watches) return false;
    mWatches=std::move(watches); mWatchCapacity=capacity; mWatchCursor=0;
    return true;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::pollNextWatchdog(LLTransportSteadyUsec now, float jitterDraw) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    if (!mWatches || !mWatchCapacity) return Step::Blocked;
    const auto index=mWatchCursor;
    mWatchCursor=(mWatchCursor+1)%mWatchCapacity;
    const auto watch=mWatches[index];
    if (!watch.incarnation) return Step::Idle;
    const auto current=mState.lookup(watch.endpoint,now.value);
    if (current.state!=LLTransportSnapshotState::Present || current.incarnation!=watch.incarnation)
    { mWatches[index]={}; return Step::Idle; }
    return pollWatchdog(watch.endpoint,watch.incarnation,now,jitterDraw);
}
namespace
{
void putLittleU32(uint8_t* dst, uint32_t value) noexcept
{
    dst[0]=uint8_t(value); dst[1]=uint8_t(value>>8);
    dst[2]=uint8_t(value>>16); dst[3]=uint8_t(value>>24);
}

// Match the legacy pre-compression packet-ID point. Zero coding runs only
// when the frozen template requires it and the encoded form is smaller;
// output is owner-local, never the legacy process-global scratch buffer.
[[maybe_unused]] bool encodeOwnerCommand(const LLTransportOwnerCommand& command,
                                         uint32_t assignedPacket, EncodedAction& out) noexcept
{
    if (command.kind!=LLTransportOwnerCommand::Kind::Send || command.byteCount<7 ||
        command.byteCount>out.bytes.size() ||
        (command.encoding!=LLTransportEncoding::Unencoded && command.encoding!=LLTransportEncoding::ZeroCoded))
        return false;
    std::memcpy(out.bytes.data(),command.bytes.data(),command.byteCount);
    std::memset(out.bytes.data(),0,5);
    putNetworkU32(out.bytes.data()+1,assignedPacket);
    out.size=command.byteCount;
    if (command.encoding==LLTransportEncoding::ZeroCoded)
    {
        std::array<uint8_t,2*LLTransportCircuitState::MaxPacketBytes> encoded{};
        std::memcpy(encoded.data(),out.bytes.data(),6);
        std::size_t used=6;
        for (std::size_t i=6;i<out.size;)
        {
            if (out.bytes[i]) { encoded[used++]=out.bytes[i++]; continue; }
            uint8_t run=0;
            while (i<out.size && !out.bytes[i] && run<255) { ++run; ++i; }
            encoded[used++]=0; encoded[used++]=run;
        }
        if (used<out.size)
        {
            std::memcpy(out.bytes.data(),encoded.data(),used);
            out.bytes[0]|=0x80;
            out.size=static_cast<uint32_t>(used);
        }
    }
    if (command.reliable) out.bytes[0]|=0x40;
    return true;
}

[[maybe_unused]] bool encodeStateAction(const LLTransportCircuitState::Action& action,
                                         uint32_t assignedPacket, EncodedAction& out) noexcept
{
    using Kind=LLTransportCircuitState::ActionKind;
    if (action.kind==Kind::InitialSend || action.kind==Kind::Resend)
    {
        if (action.size<7 || action.size>out.bytes.size()) return false;
        std::memcpy(out.bytes.data(),action.bytes.data(),action.size);
        out.size=action.size;
        return true;
    }
    if (action.kind!=Kind::Ack && action.kind!=Kind::Ping) return false;
    std::memset(out.bytes.data(),0,6);
    putNetworkU32(out.bytes.data()+1,assignedPacket);
    if (action.kind==Kind::Ack)
    {
        if (!action.ackCount || action.ackCount>LLTransportCircuitState::MaxAckBatch) return false;
        out.bytes[6]=0xff; out.bytes[7]=0xff; out.bytes[8]=0xff; out.bytes[9]=0xfb;
        out.bytes[10]=static_cast<uint8_t>(action.ackCount);
        for (uint32_t i=0;i<action.ackCount;++i) putLittleU32(out.bytes.data()+11+4*i,action.ackIds[i]);
        out.size=11+4*action.ackCount;
    }
    else
    {
        out.bytes[6]=1; out.bytes[7]=action.pingId;
        putLittleU32(out.bytes.data()+8,action.oldestUnacked);
        out.size=12;
    }
    return true;
}
}

std::size_t LLTransportOwnerLoop::pendingChargeBytes() noexcept
{ return 2u*PendingPerLane*sizeof(Pending); }

LLTransportOwnerLoop::Step LLTransportOwnerLoop::processReliableSendHead(
    LLTransportSteadyUsec now, uint64_t loginGeneration,
    const LLTransportSchemaEntry* schemas, std::size_t schemaCount) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    const auto* command=mChannels.commands.peekConsumer();
    if (!command) return Step::Idle;
    if (command->kind!=LLTransportOwnerCommand::Kind::Send || !command->reliable) return Step::SendPending;
    if (mFirstAttemptToken) return Step::Blocked;
    if (!schemas || !schemaCount || !loginGeneration || !command->sendToken || !command->schemaGeneration ||
        command->byteCount<7 || command->byteCount>LLTransportCircuitState::MaxPacketBytes)
    { mPoisoned.store(true); return Step::Poisoned; }
    // The command is main-frozen uncompressed template data. Derive its
    // template number without accepting caller-supplied compressed flags.
    auto wire=command->bytes;
    std::memset(wire.data(),0,5);
    std::array<uint8_t,LLTransportCircuitState::MaxPacketBytes> expanded{};
    const auto envelope=llTransportParseEnvelope(wire.data(),command->byteCount,expanded.data(),expanded.size());
    if (envelope.status!=LLTransportEnvelope::Status::Complete)
    { mPoisoned.store(true); return Step::Poisoned; }
    std::size_t lo=0, hi=schemaCount;
    while (lo<hi)
    {
        const auto mid=lo+(hi-lo)/2;
        if (schemas[mid].messageNumber<envelope.messageNumber) lo=mid+1; else hi=mid;
    }
    const LLTransportSchemaEntry* schema=lo<schemaCount && schemas[lo].messageNumber==envelope.messageNumber ? &schemas[lo] : nullptr;
    if (!schema || !schema->schema || !schema->schema->isFrozen() ||
        schema->schema->messageNumber()!=envelope.messageNumber || schema->schema->generation()!=command->schemaGeneration)
    { mPoisoned.store(true); return Step::Poisoned; }
    using State=LLTransportCircuitState;
    const auto& target=command->circuit;
    auto completeGone=[&]() noexcept -> Step
    {
        auto* terminal=mChannels.sendTerminals.reserveProducer();
        if (!terminal) return Step::Blocked;
        // LLCircuitData uses LL_ERR_CIRCUIT_GONE (-23017) for an accepted
        // reliable operation whose circuit was closed before execution.
        *terminal={target.endpoint,command->sendToken,target.loginGeneration,target.targetIncarnation,-23017};
        if (!mChannels.sendTerminals.publishProducer() || !mChannels.commands.releaseConsumer())
        { mPoisoned.store(true); return Step::Poisoned; }
        if (mWake) mWake->signal();
        return Step::Progress;
    };
    if (!target.endpoint.addressNetworkOrder || !target.endpoint.portHostOrder ||
        !target.targetIncarnation || !target.loginGeneration)
    { mPoisoned.store(true); return Step::Poisoned; }
    if (mAdmittedHeadToken && mAdmittedHeadToken!=command->sendToken)
    { mPoisoned.store(true); return Step::Poisoned; }
    const auto current=mState.lookup(target.endpoint,now.value);
    const bool changed=target.loginGeneration!=loginGeneration || current.state==LLTransportSnapshotState::Absent ||
        (current.state==LLTransportSnapshotState::Present && current.incarnation!=target.targetIncarnation);
    if (changed && mAdmittedHeadToken)
    {
        // A close/shutdown settles an admitted state entry; if a login change
        // precedes close, explicitly settle it without a second terminal.
        const auto settled=mState.failReliable(command->sendToken,-23017);
        if (settled!=State::Status::Ok && settled!=State::Status::Stale)
        { mPoisoned.store(true); return Step::Poisoned; }
        if (!mChannels.commands.releaseConsumer()) { mPoisoned.store(true); return Step::Poisoned; }
        mAdmittedHeadToken=0; mAdmittedHeadIncarnation=0;
        return Step::Progress;
    }
    if (changed) return completeGone();
    if (current.state!=LLTransportSnapshotState::Present) { mPoisoned.store(true); return Step::Poisoned; }
    if (!current.alive) return mAdmittedHeadToken ? Step::Blocked : completeGone();
    if (!mAdmittedHeadToken)
    {
        const auto reserved=mState.reserveReliableSlot(target.endpoint,target.targetIncarnation,
                                                        target.loginGeneration,command->sendToken);
        if (reserved==State::Status::Full) return Step::Blocked;
        if (reserved==State::Status::Absent || reserved==State::Status::Stale) return completeGone();
        if (reserved!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
        if (mState.acceptReliableSlot(command->sendToken)!=State::Status::Ok)
        {
            if (mState.cancelReliableSlot(command->sendToken)!=State::Status::Ok)
            { mPoisoned.store(true); return Step::Poisoned; }
            return completeGone();
        }
        mAdmittedHeadToken=command->sendToken; mAdmittedHeadIncarnation=target.targetIncarnation;
    }
    // An admitted head remains ordered and owned across transient Busy. Other
    // RX/control/timer steps may run; no second state slot or callback exists.
    auto failAdmitted=[&]() noexcept -> Step
    {
        if (mState.failReliable(command->sendToken,-23016)!=State::Status::Ok ||
            !mChannels.commands.releaseConsumer())
        { mPoisoned.store(true); return Step::Poisoned; }
        mAdmittedHeadToken=0; mAdmittedHeadIncarnation=0;
        return Step::Progress;
    };
    uint32_t packet=0;
    const auto allocated=mState.allocateReliablePacketId(command->sendToken,packet);
    if (allocated==State::Status::Busy) return Step::Blocked;
    if (allocated!=State::Status::Ok)
    {
        mLastPreparationFailure.store(PreparationFailure::PacketIdUnavailable);
        return failAdmitted();
    }
    EncodedAction encoded;
    if (!encodeOwnerCommand(*command,packet,encoded))
    {
        mLastPreparationFailure.store(PreparationFailure::EncodeFailed);
        return failAdmitted();
    }
    if (mState.prepareReliable(command->sendToken,packet,encoded.bytes.data(),encoded.size,command->retry,now.value)!=State::Status::Ok)
    {
        mLastPreparationFailure.store(PreparationFailure::StatePrepareFailed);
        return failAdmitted();
    }
    mFirstAttemptToken=command->sendToken; mFirstAttemptIncarnation=target.targetIncarnation; mFirstAttemptReliable=true;
    if (!mChannels.commands.releaseConsumer()) { mPoisoned.store(true); return Step::Poisoned; }
    mAdmittedHeadToken=0; mAdmittedHeadIncarnation=0;
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::processUnreliableSendHead(
    LLTransportSteadyUsec now, uint64_t loginGeneration,
    const LLTransportSchemaEntry* schemas, std::size_t schemaCount) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    const auto* command=mChannels.commands.peekConsumer();
    if (!command) return Step::Idle;
    if (command->kind!=LLTransportOwnerCommand::Kind::Send || command->reliable) return Step::SendPending;
    if (mFirstAttemptToken) return Step::Blocked;
    if (!schemas || !schemaCount || !loginGeneration || !command->sendToken || !command->schemaGeneration ||
        command->byteCount<7 || command->byteCount>LLTransportCircuitState::MaxPacketBytes || !mDataPending)
    { mPoisoned.store(true); return Step::Poisoned; }
    auto wire=command->bytes;
    std::memset(wire.data(),0,5);
    std::array<uint8_t,LLTransportCircuitState::MaxPacketBytes> expanded{};
    const auto envelope=llTransportParseEnvelope(wire.data(),command->byteCount,expanded.data(),expanded.size());
    if (envelope.status!=LLTransportEnvelope::Status::Complete) { mPoisoned.store(true); return Step::Poisoned; }
    std::size_t lo=0,hi=schemaCount;
    while (lo<hi) { const auto mid=lo+(hi-lo)/2; if (schemas[mid].messageNumber<envelope.messageNumber) lo=mid+1; else hi=mid; }
    const auto* schema=lo<schemaCount && schemas[lo].messageNumber==envelope.messageNumber ? &schemas[lo] : nullptr;
    if (!schema || !schema->schema || !schema->schema->isFrozen() ||
        schema->schema->messageNumber()!=envelope.messageNumber || schema->schema->generation()!=command->schemaGeneration)
    { mPoisoned.store(true); return Step::Poisoned; }
    const auto& target=command->circuit;
    if (!target.endpoint.addressNetworkOrder || !target.endpoint.portHostOrder ||
        !target.targetIncarnation || !target.loginGeneration)
    { mPoisoned.store(true); return Step::Poisoned; }
    const auto current=mState.lookup(target.endpoint,now.value);
    if (current.state==LLTransportSnapshotState::Unavailable) { mPoisoned.store(true); return Step::Poisoned; }
    if (target.loginGeneration!=loginGeneration || current.state!=LLTransportSnapshotState::Present ||
        !current.alive || current.incarnation!=target.targetIncarnation)
    {
        auto* terminal=mChannels.unreliableTerminals.reserveProducer();
        if (!terminal) return Step::Blocked;
        *terminal={target.endpoint,command->sendToken,target.loginGeneration,target.targetIncarnation,
                   LLTransportUnreliableStatus::Cancelled,0};
        if (!mChannels.unreliableTerminals.publishProducer() || !mChannels.commands.releaseConsumer())
        { mPoisoned.store(true); return Step::Poisoned; }
        if (mWake) mWake->signal();
        return Step::Progress;
    }
    Pending* vacant=nullptr;
    for (uint32_t i=0;i<PendingPerLane;++i)
        if (!mDataPending[i].unreliable && !mDataPending[i].controlReply &&
            mDataPending[i].kind==LLTransportCircuitState::ActionKind::None) { vacant=&mDataPending[i]; break; }
    if (!vacant) return Step::Blocked;
    if (mUnreliableSequence==UINT64_MAX) { mPoisoned.store(true); return Step::Poisoned; }
    uint32_t packet=0;
    const auto allocated=mState.allocatePacketId(target.endpoint,target.targetIncarnation,packet);
    if (allocated==LLTransportCircuitState::Status::Busy) return Step::Blocked;
    if (allocated!=LLTransportCircuitState::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
    EncodedAction encoded;
    if (!encodeOwnerCommand(*command,packet,encoded)) { mPoisoned.store(true); return Step::Poisoned; }
    *vacant=Pending{};
    vacant->unreliable=true; vacant->endpoint=target.endpoint; vacant->incarnation=target.targetIncarnation;
    // Legacy excludes PacketAck itself from piggyback ACK attachment.
    vacant->allowPiggyback=envelope.messageNumber!=0xfffffffbU;
    vacant->loginGeneration=target.loginGeneration; vacant->unreliableToken=command->sendToken;
    vacant->unreliableSequence=++mUnreliableSequence; vacant->packetId=packet; vacant->size=encoded.size;
    std::memcpy(vacant->bytes.data(),encoded.bytes.data(),encoded.size);
    mFirstAttemptToken=command->sendToken; mFirstAttemptIncarnation=target.targetIncarnation; mFirstAttemptReliable=false;
    if (!mChannels.commands.releaseConsumer()) { mPoisoned.store(true); return Step::Poisoned; }
    return Step::Progress;
}

bool LLTransportOwnerLoop::initializePending() noexcept
{
    if (mControlPending || mDataPending) return false;
    auto control=std::unique_ptr<Pending[]>(new(std::nothrow) Pending[PendingPerLane]);
    if (!control) return false;
    auto data=std::unique_ptr<Pending[]>(new(std::nothrow) Pending[PendingPerLane]);
    if (!data) return false;
    mControlPending=std::move(control); mDataPending=std::move(data);
    return true;
}

bool LLTransportOwnerLoop::wantsWritableWake() const noexcept
{
    for (const Pending* lane : {mControlPending.get(),mDataPending.get()})
        if (lane) for (uint32_t i=0;i<PendingPerLane;++i)
            if ((lane[i].kind!=LLTransportCircuitState::ActionKind::None || lane[i].controlReply || lane[i].unreliable) &&
                lane[i].attempts) return true;
    return false;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::cancelOneUnreliableForStop() noexcept
{
    if (!mDataPending) return Step::Idle;
    for (uint32_t i=0;i<PendingPerLane;++i)
    {
        auto& pending=mDataPending[i];
        if (!pending.unreliable) continue;
        auto* terminal=mChannels.unreliableTerminals.reserveProducer();
        if (!terminal) return Step::Blocked;
        *terminal={pending.endpoint,pending.unreliableToken,pending.loginGeneration,pending.incarnation,
                   LLTransportUnreliableStatus::Cancelled,0};
        if (!mChannels.unreliableTerminals.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
        if (!mFirstAttemptReliable && mFirstAttemptToken==pending.unreliableToken && mFirstAttemptIncarnation==pending.incarnation)
        { mFirstAttemptToken=0; mFirstAttemptIncarnation=0; }
        pending=Pending{};
        if (mWake) mWake->signal();
        return Step::Progress;
    }
    return Step::Idle;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::sendUnreliableOnce(LLPacketRing& ring, int socket,
    const LLPacketRing::UDPProxySnapshot& proxy, LLTransportSteadyUsec now) noexcept
{
    mLastUnreliableAffected={};
    if (mPoisoned.load()) return Step::Poisoned;
    if (!mDataPending) return Step::Blocked;
    Pending* pending=nullptr;
    for (uint32_t i=0;i<PendingPerLane;++i)
        if (mDataPending[i].unreliable && (!pending || mDataPending[i].unreliableSequence<pending->unreliableSequence))
            pending=&mDataPending[i];
    if (!pending) return Step::Idle;
    mLastUnreliableAffected={pending->endpoint,pending->incarnation,true};
    auto* terminal=mChannels.unreliableTerminals.reserveProducer();
    if (!terminal) return Step::Blocked;
    const auto current=mState.lookup(pending->endpoint,now.value);
    if (current.state!=LLTransportSnapshotState::Present || !current.alive || current.incarnation!=pending->incarnation)
    {
        *terminal={pending->endpoint,pending->unreliableToken,pending->loginGeneration,pending->incarnation,
                   LLTransportUnreliableStatus::Cancelled,0};
    }
    else
    {
        uint64_t piggybackToken=0;
        if (pending->allowPiggyback && !pending->attempts && pending->size+4<=static_cast<uint32_t>(MTUBYTES))
        {
            LLTransportCircuitState::Piggyback piggyback;
            const auto status=mState.reservePiggyback(pending->endpoint,pending->incarnation,
                (static_cast<uint32_t>(MTUBYTES)-pending->size)/4,piggyback);
            if (status==LLTransportCircuitState::Status::Busy || status==LLTransportCircuitState::Status::Full)
            { mChannels.unreliableTerminals.cancelProducer(); return Step::Blocked; }
            if (status!=LLTransportCircuitState::Status::Ok && status!=LLTransportCircuitState::Status::NotDue)
            { mChannels.unreliableTerminals.cancelProducer(); mPoisoned.store(true); return Step::Poisoned; }
            if (status==LLTransportCircuitState::Status::Ok)
            {
                if (!piggyback.count || piggyback.count>250 || pending->size+4*piggyback.count+1>pending->bytes.size())
                { mState.finishPiggyback(piggyback.token,false); mChannels.unreliableTerminals.cancelProducer(); mPoisoned.store(true); return Step::Poisoned; }
                pending->bytes[0]|=0x10;
                for (uint32_t i=0;i<piggyback.count;++i)
                { putNetworkU32(pending->bytes.data()+pending->size,piggyback.ackIds[i]); pending->size+=4; }
                pending->bytes[pending->size++]=static_cast<uint8_t>(piggyback.count);
                piggybackToken=piggyback.token;
            }
        }
        NetSendResult result{NetSendStatus::Failed,0}; bool attempted=false;
        try
        {
            const LLHost destination(pending->endpoint.addressNetworkOrder,pending->endpoint.portHostOrder);
            result=ring.sendPacketOnce(socket,reinterpret_cast<const char*>(pending->bytes.data()),
                                       static_cast<S32>(pending->size),destination,proxy,pending->attempts==0);
            attempted=true;
        }
        catch (...) { result={NetSendStatus::Failed,0}; }
        if (piggybackToken && mState.finishPiggyback(piggybackToken,attempted)!=LLTransportCircuitState::Status::Ok)
        { mChannels.unreliableTerminals.cancelProducer(); mPoisoned.store(true); return Step::Poisoned; }
        if (pending->attempts!=UINT32_MAX) ++pending->attempts;
        bool success=result.status==NetSendStatus::Sent; bool retry=false;
#if LL_WINDOWS
        retry=result.status==NetSendStatus::WouldBlock;
        if (result.status==NetSendStatus::Refused && result.osError==WSAECONNRESET) success=true;
#else
        retry=(result.osError==EAGAIN || result.osError==ECONNREFUSED) && pending->attempts<3;
        if (pending->attempts>=3) success=false;
#endif
        if (retry) { mChannels.unreliableTerminals.cancelProducer(); return Step::Blocked; }
        if (mState.noteOrdinaryAttempt(pending->endpoint,pending->incarnation,success,success ? pending->size : 0)
            !=LLTransportCircuitState::Status::Ok)
        { mChannels.unreliableTerminals.cancelProducer(); mPoisoned.store(true); return Step::Poisoned; }
        *terminal={pending->endpoint,pending->unreliableToken,pending->loginGeneration,pending->incarnation,
                   success ? LLTransportUnreliableStatus::Sent : LLTransportUnreliableStatus::Failed,result.osError};
    }
    if (!mChannels.unreliableTerminals.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
    if (!mFirstAttemptReliable && mFirstAttemptToken==pending->unreliableToken && mFirstAttemptIncarnation==pending->incarnation)
    { mFirstAttemptToken=0; mFirstAttemptIncarnation=0; }
    *pending=Pending{};
    if (mWake) mWake->signal();
    return Step::Progress;
}

void LLTransportOwnerLoop::cancelInFlightForStop() noexcept
{
    mReceive.cancelHeldForStop();
    mReceiveHeld=false;
    for (Pending* lane : {mControlPending.get(),mDataPending.get()})
        if (lane) for (uint32_t i=0;i<PendingPerLane;++i) if (!lane[i].unreliable) lane[i]=Pending{};
    mState.shutdown();
    if (mWatches) for (uint32_t i=0;i<mWatchCapacity;++i) mWatches[i]={};
    if (mAdmittedHeadToken)
    {
        const auto* command=mChannels.commands.peekConsumer();
        if (!command || command->kind!=LLTransportOwnerCommand::Kind::Send ||
            command->sendToken!=mAdmittedHeadToken || !mChannels.commands.releaseConsumer())
        { mPoisoned.store(true); return; }
        mAdmittedHeadToken=0; mAdmittedHeadIncarnation=0;
    }
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::settleQueuedForStop() noexcept
{
    const auto* command=mChannels.commands.peekConsumer();
    if (!command) return Step::Idle;
    using Kind=LLTransportOwnerCommand::Kind;
    if (command->kind==Kind::Stop)
    {
        if (!mChannels.commands.releaseConsumer()) { mPoisoned.store(true); return Step::Poisoned; }
        return Step::Progress;
    }
    if (command->kind==Kind::Send)
    {
        // Reliable commands have a main callback/terminal credit. A send
        // already admitted into state was removed by cancelInFlightForStop;
        // its sole completion is emitted by publishReliableCompletion.
        if (command->reliable)
        {
            auto* terminal=mChannels.sendTerminals.reserveProducer();
            if (!terminal) return Step::Blocked;
            const auto& target=command->circuit;
            *terminal={target.endpoint,command->sendToken,target.loginGeneration,target.targetIncarnation,-23017};
            if (!mChannels.sendTerminals.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
        }
        else
        {
            auto* terminal=mChannels.unreliableTerminals.reserveProducer();
            if (!terminal) return Step::Blocked;
            const auto& target=command->circuit;
            *terminal={target.endpoint,command->sendToken,target.loginGeneration,target.targetIncarnation,
                       LLTransportUnreliableStatus::Cancelled,0};
            if (!mChannels.unreliableTerminals.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
        }
    }
    else
    {
        auto* terminal=mChannels.circuitTerminals.reserveProducer();
        if (!terminal) return Step::Blocked;
        *terminal={};
        if (command->kind==Kind::SetAckCollectTime) terminal->operationId=command->configOperationId;
        else
        {
            terminal->endpoint=command->circuit.endpoint;
            terminal->operationId=command->circuit.operationId;
            terminal->loginGeneration=command->circuit.loginGeneration;
            terminal->circuitIncarnation=command->circuit.targetIncarnation;
            terminal->lastPublishedAppSequence=mReceive.lastPublishedAppSequence();
        }
        terminal->status=LLTransportCircuitCommandStatus::Cancelled;
        if (command->kind==Kind::FinishWatchdogTimeout)
        {
            terminal->endpoint=command->timeout.endpoint;
            terminal->loginGeneration=command->timeout.loginGeneration;
            terminal->circuitIncarnation=command->timeout.incarnation;
            terminal->lastPublishedAppSequence=command->timeout.lastPublishedAppSequence;
            terminal->timeoutEventId=command->timeout.eventId;
        }
        if (!mChannels.circuitTerminals.publishProducer()) { mPoisoned.store(true); return Step::Poisoned; }
    }
    if (!mChannels.commands.releaseConsumer()) { mPoisoned.store(true); return Step::Poisoned; }
    if (mWake) mWake->signal();
    return Step::Progress;
}

void LLTransportOwnerLoop::discardIncarnation(uint64_t incarnation) noexcept
{
    if (!incarnation) return;
    if (mWatches) for (uint32_t i=0;i<mWatchCapacity;++i)
        if (mWatches[i].incarnation==incarnation) mWatches[i]={};
    for (Pending* lane : {mControlPending.get(),mDataPending.get()})
        if (lane) for (uint32_t i=0;i<PendingPerLane;++i)
            if (lane[i].incarnation==incarnation && !lane[i].unreliable) lane[i]=Pending{};
}

void LLTransportOwnerLoop::discardControlIncarnation(uint64_t incarnation) noexcept
{
    if (!incarnation || !mControlPending) return;
    for (uint32_t i=0;i<PendingPerLane;++i)
        if (mControlPending[i].incarnation==incarnation) mControlPending[i]=Pending{};
}

void LLTransportOwnerLoop::discardReliable(uint64_t incarnation, uint64_t token) noexcept
{
    if (!incarnation || !token || !mDataPending) return;
    for (uint32_t i=0;i<PendingPerLane;++i)
        if (mDataPending[i].incarnation==incarnation && mDataPending[i].reliableToken==token)
            mDataPending[i]=Pending{};
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::sendStateActionOnce(LLPacketRing& ring, int socket,
    const LLPacketRing::UDPProxySnapshot& proxy, LLTransportSteadyUsec now,
    uint64_t ackCollectUsec) noexcept
{
    mLastActionAffected={}; mLastAdvanceDirty.count=0; mLastAdvanceDirty.overflow=false;
    if (mPoisoned.load()) return Step::Poisoned;
    if (!mControlPending || !mDataPending) return Step::Blocked;
    auto action=mState.advance(now.value,ackCollectUsec,mLastAdvanceDirty);
    if (mLastAdvanceDirty.overflow) { mPoisoned.store(true); return Step::Poisoned; }
    using State=LLTransportCircuitState;
    using Kind=State::ActionKind;
    if (action.kind==Kind::None) return mLastAdvanceDirty.count ? Step::Progress : Step::Idle;
    mLastActionAffected={action.endpoint,action.incarnation,true};
    const bool control=action.kind==Kind::Ack || action.kind==Kind::Ping;
    Pending* lane=control ? mControlPending.get() : mDataPending.get();
    Pending* retained=nullptr;
    Pending* vacant=nullptr;
    for (uint32_t i=0;i<PendingPerLane;++i)
    {
        auto& item=lane[i];
        if (item.kind==Kind::None && !item.controlReply && !item.unreliable) { if (!vacant) vacant=&item; continue; }
        if (item.controlReply || item.unreliable) continue;
        if (item.incarnation!=action.incarnation || item.kind!=action.kind) continue;
        if ((!control && item.reliableToken==action.reliableToken) ||
            (control && action.packetAssigned && item.packetId==action.packetId))
        { retained=&item; break; }
    }
    uint64_t piggybackToken=0;
    if (!retained)
    {
        if (!vacant)
        {
            if (mState.defer(action.token)!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
            return Step::Blocked;
        }
        uint32_t packet=action.packetId;
        if (control && !action.packetAssigned && mState.assignActionPacketId(action.token,packet)!=State::Status::Ok)
        {
            if (mState.defer(action.token)!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
            return Step::Blocked;
        }
        EncodedAction encoded;
        if (!encodeStateAction(action,packet,encoded))
        { mState.defer(action.token); mPoisoned.store(true); return Step::Poisoned; }
        // The selected resend's own circuit supplies the interval at its
        // first attempt. Preserve it and that time across would-block retries.
        int64_t selectedInterval=0;
        if (action.kind==Kind::Resend &&
            mState.selectedRetryInterval(action.token,now.value,selectedInterval)!=State::Status::Ok)
        { mState.defer(action.token); mPoisoned.store(true); return Step::Poisoned; }
        // Legacy appends ACKs to first sends and StartPing, but excludes
        // PacketAck and resends. Its selected ACKs are erased before the
        // blocking send. Retain exact encoded bytes before the first syscall.
        if ((action.kind==Kind::InitialSend || action.kind==Kind::Ping) && encoded.size<static_cast<uint32_t>(MTUBYTES))
        {
            const uint32_t available=(static_cast<uint32_t>(MTUBYTES)-encoded.size)/4;
            if (available)
            {
                State::Piggyback piggyback;
                const auto status=mState.reservePiggyback(action.endpoint,action.incarnation,available,piggyback);
                if (status==State::Status::Busy || status==State::Status::Full)
                {
                    if (mState.defer(action.token)!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
                    return Step::Blocked;
                }
                if (status!=State::Status::Ok && status!=State::Status::NotDue)
                { mState.defer(action.token); mPoisoned.store(true); return Step::Poisoned; }
                if (status==State::Status::Ok)
                {
                    if (!piggyback.count || piggyback.count>250 ||
                        encoded.size+4*piggyback.count+1>encoded.bytes.size())
                    {
                        mState.finishPiggyback(piggyback.token,false); mState.defer(action.token);
                        mPoisoned.store(true); return Step::Poisoned;
                    }
                    encoded.bytes[0]|=0x10;
                    for (uint32_t i=0;i<piggyback.count;++i)
                    { putNetworkU32(encoded.bytes.data()+encoded.size,piggyback.ackIds[i]); encoded.size+=4; }
                    encoded.bytes[encoded.size++]=static_cast<uint8_t>(piggyback.count);
                    piggybackToken=piggyback.token;
                }
            }
        }
        vacant->kind=action.kind; vacant->incarnation=action.incarnation;
        vacant->reliableToken=action.reliableToken; vacant->packetId=packet; vacant->size=encoded.size;
        vacant->firstAttemptUsec=now.value;
        vacant->nextRetryIntervalUsec=selectedInterval;
        std::memcpy(vacant->bytes.data(),encoded.bytes.data(),encoded.size);
        retained=vacant;
    }
    NetSendResult result{NetSendStatus::Failed,0};
    bool attempted=false;
    try
    {
        const LLHost destination(action.endpoint.addressNetworkOrder,action.endpoint.portHostOrder);
        result=ring.sendPacketOnce(socket,reinterpret_cast<const char*>(retained->bytes.data()),
                                   static_cast<S32>(retained->size),destination,proxy,retained->attempts==0);
        attempted=true;
    }
    catch (...) { result={NetSendStatus::Failed,0}; }
    if (piggybackToken && mState.finishPiggyback(piggybackToken,attempted)!=State::Status::Ok)
    { mPoisoned.store(true); return Step::Poisoned; }
    if (retained->attempts!=UINT32_MAX) ++retained->attempts;
    bool success=result.status==NetSendStatus::Sent;
    bool retry=false;
#if LL_WINDOWS
    retry=result.status==NetSendStatus::WouldBlock;
    if (result.status==NetSendStatus::Refused && result.osError==WSAECONNRESET) success=true;
#else
    retry=(result.osError==EAGAIN || result.osError==ECONNREFUSED) && retained->attempts<3;
    if (retained->attempts>=3) success=false;
#endif
    if (retry)
    {
        if (mState.defer(action.token)!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
        return Step::Blocked;
    }
    // Legacy samples resend/watchdog time before its blocking send attempt;
    // interleaved WouldBlock retries must not silently extend that deadline.
    const auto status=mState.commitAttempt(action.token,retained->firstAttemptUsec,retained->nextRetryIntervalUsec,
                                            success,success ? retained->size : 0);
    *retained=Pending{};
    if (status!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
    if (action.kind==Kind::InitialSend && mFirstAttemptReliable &&
        mFirstAttemptToken==action.reliableToken && mFirstAttemptIncarnation==action.incarnation)
    { mFirstAttemptToken=0; mFirstAttemptIncarnation=0; mFirstAttemptReliable=false; }
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::prepareStartPingResponse(
    LLTransportSteadyUsec now, uint64_t heartbeatUsec, bool diagnosticsHandled) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    if (!mControlPending || mReceive.pendingKind()!=LLTransportPendingKind::InternalControl) return Step::Blocked;
    LLTransportControlValues values; LLTransportDecodeResult diagnostics;
    if (!mReceive.decodePendingControl(values,diagnostics) || values.kind!=LLTransportControlValues::Kind::StartPing)
        return Step::Blocked;
    const auto endpoint=mReceive.controlSender();
    const auto incarnation=mReceive.controlIncarnation();
    const auto observation=mReceive.controlObservation();
    const auto current=mState.lookup(endpoint,now.value);
    if (current.state!=LLTransportSnapshotState::Present || !current.alive || current.incarnation!=incarnation || !observation)
        return Step::Blocked; // stale-held input needs the explicit cancellation path
    Pending* reply=nullptr; Pending* vacant=nullptr;
    for (uint32_t i=0;i<PendingPerLane;++i)
    {
        auto& item=mControlPending[i];
        if (item.controlReply && item.incarnation==incarnation && item.replyObservation==observation)
        { reply=&item; break; }
        if (!item.controlReply && item.kind==LLTransportCircuitState::ActionKind::None && !vacant) vacant=&item;
    }
    if (!reply)
    {
        if (!vacant || mReplySequence==UINT64_MAX) return Step::Blocked;
        uint32_t packet=0;
        const auto allocated=mState.allocatePacketId(endpoint,incarnation,packet);
        if (allocated==LLTransportCircuitState::Status::Busy || allocated==LLTransportCircuitState::Status::Full) return Step::Blocked;
        if (allocated!=LLTransportCircuitState::Status::Ok) return Step::Blocked;
        *vacant=Pending{}; vacant->controlReply=true; vacant->endpoint=endpoint;
        vacant->incarnation=incarnation; vacant->packetId=packet; vacant->replyObservation=observation;
        vacant->replySequence=++mReplySequence; vacant->size=8;
        putNetworkU32(vacant->bytes.data()+1,packet);
        vacant->bytes[6]=2; vacant->bytes[7]=values.pingId; // CompletePingCheck, High 2
        reply=vacant;
    }
    const auto applied=mReceive.applyPendingControl(now,heartbeatUsec,true,diagnosticsHandled);
    if (applied==LLTransportControlApply::Poisoned) return Step::Poisoned;
    if (applied==LLTransportControlApply::Backpressure) return finishControlBackpressure();
    if (applied!=LLTransportControlApply::Applied) return Step::Blocked;
    if (mReceive.commitPendingControl(now,true)!=LLTransportControlApply::Applied)
    { mPoisoned.store(true); return Step::Poisoned; }
    mReceiveHeld=false;
    mReadTurnNeeded=true;
    reply->replyReady=true;
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::finishControlBackpressure() noexcept
{
    const auto incarnation=mReceive.controlIncarnation();
    const auto observation=mReceive.controlObservation();
    if (mControlPending) for (uint32_t i=0;i<PendingPerLane;++i)
    {
        auto& item=mControlPending[i];
        if (item.controlReply && item.incarnation==incarnation && item.replyObservation==observation)
        {
            if (item.replyReady) { mPoisoned.store(true); return Step::Poisoned; }
            item=Pending{};
        }
    }
    mReceiveHeld=false;
    mReadTurnNeeded=true;
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::discardStaleHeldControl(
    uint64_t currentLoginGeneration, LLTransportSteadyUsec now) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    const auto incarnation=mReceive.controlIncarnation();
    const auto observation=mReceive.controlObservation();
    if (!mReceive.discardPendingIfStale(currentLoginGeneration,now)) return Step::Blocked;
    mReceiveHeld=false;
    mReadTurnNeeded=true;
    // Only an unready reply belongs to this unapplied input. Other retained
    // replies remain subject to their own incarnation and send checks.
    if (mControlPending) for (uint32_t i=0;i<PendingPerLane;++i)
    {
        auto& item=mControlPending[i];
        if (item.controlReply && !item.replyReady && item.incarnation==incarnation && item.replyObservation==observation)
            item=Pending{};
    }
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::discardEmptyUnreliableControl(bool diagnosticsHandled) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    if (!mReceive.discardEmptyUnreliableControl(diagnosticsHandled)) return Step::Blocked;
    mReceiveHeld=false;
    mReadTurnNeeded=true;
    return Step::Progress;
}

bool LLTransportOwnerLoop::noteSocketError(int nativeError, LLTransportSteadyUsec now) noexcept
{
    mReadTurnNeeded=true;
    mLastReceiveNativeError=nativeError;
    if (!nativeError) { mReceiveRetryAt=0; return true; }
#if LL_WINDOWS
    const bool retry=nativeError==WSAECONNRESET || nativeError==WSAENOBUFS || nativeError==WSAEINTR;
#else
    const bool retry=nativeError==ECONNREFUSED || nativeError==ENOBUFS || nativeError==ENOMEM || nativeError==EINTR;
#endif
    if (!retry) return false;
    constexpr uint64_t delay=10000;
    const auto maximum=(std::numeric_limits<uint64_t>::max)();
    mReceiveRetryAt=now.value>maximum-delay ? maximum : now.value+delay;
    return true;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::sendControlResponseOnce(LLPacketRing& ring, int socket,
    const LLPacketRing::UDPProxySnapshot& proxy, LLTransportSteadyUsec now) noexcept
{
    mLastReplyAffected={};
    if (mPoisoned.load()) return Step::Poisoned;
    if (!mControlPending) return Step::Blocked;
    Pending* reply=nullptr;
    for (uint32_t i=0;i<PendingPerLane;++i)
    {
        auto& item=mControlPending[i];
        if (item.controlReply && item.replyReady && (!reply || item.replySequence<reply->replySequence)) reply=&item;
    }
    if (!reply) return Step::Idle;
    mLastReplyAffected={reply->endpoint,reply->incarnation,true};
    const auto current=mState.lookup(reply->endpoint,now.value);
    if (current.state!=LLTransportSnapshotState::Present || !current.alive || current.incarnation!=reply->incarnation)
    { *reply=Pending{}; return Step::Progress; }
    using State=LLTransportCircuitState;
    uint64_t piggybackToken=0;
    if (!reply->attempts && reply->size<static_cast<uint32_t>(MTUBYTES))
    {
        State::Piggyback piggyback;
        const uint32_t available=(static_cast<uint32_t>(MTUBYTES)-reply->size)/4;
        const auto status=mState.reservePiggyback(reply->endpoint,reply->incarnation,available,piggyback);
        if (status==State::Status::Busy || status==State::Status::Full) return Step::Blocked;
        if (status!=State::Status::Ok && status!=State::Status::NotDue) { mPoisoned.store(true); return Step::Poisoned; }
        if (status==State::Status::Ok)
        {
            if (!piggyback.count || piggyback.count>250 || reply->size+4*piggyback.count+1>reply->bytes.size())
            { mState.finishPiggyback(piggyback.token,false); mPoisoned.store(true); return Step::Poisoned; }
            reply->bytes[0]|=0x10;
            for (uint32_t i=0;i<piggyback.count;++i)
            { putNetworkU32(reply->bytes.data()+reply->size,piggyback.ackIds[i]); reply->size+=4; }
            reply->bytes[reply->size++]=static_cast<uint8_t>(piggyback.count);
            piggybackToken=piggyback.token;
        }
    }
    NetSendResult result{NetSendStatus::Failed,0}; bool attempted=false;
    try
    {
        const LLHost destination(reply->endpoint.addressNetworkOrder,reply->endpoint.portHostOrder);
        result=ring.sendPacketOnce(socket,reinterpret_cast<const char*>(reply->bytes.data()),
                                   static_cast<S32>(reply->size),destination,proxy,reply->attempts==0);
        attempted=true;
    }
    catch (...) { result={NetSendStatus::Failed,0}; }
    if (piggybackToken && mState.finishPiggyback(piggybackToken,attempted)!=State::Status::Ok)
    { mPoisoned.store(true); return Step::Poisoned; }
    if (reply->attempts!=UINT32_MAX) ++reply->attempts;
    bool success=result.status==NetSendStatus::Sent; bool retry=false;
#if LL_WINDOWS
    retry=result.status==NetSendStatus::WouldBlock;
    if (result.status==NetSendStatus::Refused && result.osError==WSAECONNRESET) success=true;
#else
    retry=(result.osError==EAGAIN || result.osError==ECONNREFUSED) && reply->attempts<3;
    if (reply->attempts>=3) success=false;
#endif
    if (retry) return Step::Blocked;
    const auto status=mState.noteOrdinaryAttempt(reply->endpoint,reply->incarnation,success,success ? reply->size : 0);
    *reply=Pending{};
    if (status!=State::Status::Ok) { mPoisoned.store(true); return Step::Poisoned; }
    return Step::Progress;
}

LLTransportOwnerLoop::Step LLTransportOwnerLoop::driveOnce(Directory& directory, LLPacketRing& ring, int socket,
    const LLPacketRing::UDPProxySnapshot& proxy, LLTransportSteadyUsec now, uint64_t loginGeneration,
    const LLTransportSchemaEntry* schemas, std::size_t schemaCount, LLTransportCircuitState::WatchdogConfig watchdog,
    bool diagnosticsHandled) noexcept
{
    if (mPoisoned.load()) return Step::Poisoned;
    if (!directory.beginStateMutationOnOwner()) { mPoisoned.store(true); return Step::Poisoned; }
    // A single owner turn may mutate one command circuit, one receive/control
    // circuit, one watchdog and up to 1024 maintenance circuits. Gather each
    // report immediately, before a later pump call overwrites it. Main sees
    // the directory as unavailable until all fresh snapshots are published.
    std::array<LLTransportEndpoint,1032> touched{};
    std::size_t count=0;
    bool overflow=false;
    auto add=[&](LLTransportEndpoint endpoint) noexcept {
        if (!endpoint.addressNetworkOrder || !endpoint.portHostOrder) return;
        for (std::size_t i=0;i<count;++i)
            if (touched[i].addressNetworkOrder==endpoint.addressNetworkOrder &&
                touched[i].portHostOrder==endpoint.portHostOrder) return;
        if (count==touched.size()) { overflow=true; return; }
        touched[count++]=endpoint;
    };
    auto finish=[&](Step step) noexcept {
        if (overflow || step==Step::Poisoned || mPoisoned.load())
        { directory.failClosedOnOwner(); mPoisoned.store(true); return Step::Poisoned; }
        for (std::size_t i=0;i<count;++i)
        {
            LLTransportCircuitSnapshot snapshot;
            bool complete=false;
            const auto status=mState.lookupWithLossTracking(touched[i],now.value,snapshot,complete);
            if (status==LLTransportCircuitState::Status::Ok)
                snapshot.lossQuality=complete ? LLTransportLossQuality::Complete : LLTransportLossQuality::Partial;
            else if (status!=LLTransportCircuitState::Status::Absent)
            { directory.failClosedOnOwner(); mPoisoned.store(true); return Step::Poisoned; }
            if (!directory.reconcileStateOnOwner(touched[i],snapshot))
            { directory.failClosedOnOwner(); mPoisoned.store(true); return Step::Poisoned; }
        }
        if (!directory.endStateMutationOnOwner()) { mPoisoned.store(true); return Step::Poisoned; }
        return step;
    };
    bool progress=false, blocked=false;
    if (const auto* head=mChannels.commands.peekConsumer())
    {
        if (head->kind==LLTransportOwnerCommand::Kind::FinishWatchdogTimeout) add(head->timeout.endpoint);
        else if (head->kind!=LLTransportOwnerCommand::Kind::SetAckCollectTime &&
                 head->kind!=LLTransportOwnerCommand::Kind::Stop) add(head->circuit.endpoint);
    }
    auto command=processCircuitHead(now,watchdog,&directory);
    if (command==Step::SendPending) command=processReliableSendHead(now,loginGeneration,schemas,schemaCount);
    if (command==Step::SendPending) command=processUnreliableSendHead(now,loginGeneration,schemas,schemaCount);
    if (command==Step::Poisoned || command==Step::StopPending) return finish(command);
    progress|=command==Step::Progress; blocked|=command==Step::Blocked || command==Step::SendPending;
    const auto completion=publishReliableCompletion();
    if (completion==Step::Poisoned) return finish(completion);
    progress|=completion==Step::Progress; blocked|=completion==Step::Blocked;
    if (mReceiveHeld)
    {
        if (mReceive.pendingKind()==LLTransportPendingKind::BusyAppendedAck)
        {
            const auto result=resumeBusyAcknowledgements(loginGeneration);
            if (mReceive.affectedCircuit().valid) add(mReceive.affectedCircuit().endpoint);
            if (result==LLTransportRxStatus::Poisoned) return finish(Step::Poisoned);
            progress|=result!=LLTransportRxStatus::ControlPending && result!=LLTransportRxStatus::Idle;
            blocked|=result==LLTransportRxStatus::ControlPending;
        }
        else
        {
            add(mReceive.controlSender());
            // The graph admits this path only with the frozen no-callback
            // policy. Publish the exact warning before allowing its control
            // effects; retaining it also prevents duplicate warnings on retry.
            LLTransportControlWarning warning;
            if (!mReceive.controlWarningRetainedOnOwner() && mReceive.pendingControlWarning(warning))
            {
                if (!mChannels.controlWarnings.publishOnOwner(warning,now.value) ||
                    !mReceive.markControlWarningRetainedOnOwner(warning))
                { mPoisoned.store(true); return finish(Step::Poisoned); }
                if (mWake) mWake->signal();
                progress=true;
            }
            auto control=servicePendingControl(now,watchdog.heartbeatUsec,diagnosticsHandled);
            if (control==Step::Blocked) control=prepareStartPingResponse(now,watchdog.heartbeatUsec,diagnosticsHandled);
            if (control==Step::Blocked) control=discardStaleHeldControl(loginGeneration,now);
            if (control==Step::Blocked) control=discardEmptyUnreliableControl(diagnosticsHandled);
            if (control==Step::Poisoned) return finish(control);
            progress|=control==Step::Progress; blocked|=control==Step::Blocked;
        }
    }
    else if (needsReceiveTurn(now))
    {
        const auto result=pollReceive(socket,proxy,loginGeneration,now);
        mReadTurnNeeded=result!=LLTransportRxStatus::Idle;
        const bool retryDelayed=result==LLTransportRxStatus::Interrupted || result==LLTransportRxStatus::SocketError;
        if (retryDelayed)
        {
            if (!noteSocketError(mReceive.lastReceiveNativeError(),now))
                return finish(Step::Poisoned);
        }
        else mReceiveRetryAt=0;
        if (mReceive.affectedCircuit().valid) add(mReceive.affectedCircuit().endpoint);
        if (result==LLTransportRxStatus::Poisoned) return finish(Step::Poisoned);
        progress|=result!=LLTransportRxStatus::Idle && !retryDelayed;
        blocked|=retryDelayed;
    }
    if (mWatches && mWatchCapacity) add(mWatches[mWatchCursor].endpoint);
    const auto timer=pollNextWatchdog(now,watchdog.initialJitterDraw);
    if (timer==Step::Poisoned) return finish(timer);
    progress|=timer==Step::Progress; blocked|=timer==Step::Blocked;
    const auto action=sendStateActionOnce(ring,socket,proxy,now,mAckCollectUsec);
    for (uint32_t i=0;i<mLastAdvanceDirty.count;++i) add(mLastAdvanceDirty.values[i].endpoint);
    if (mLastActionAffected.valid) add(mLastActionAffected.endpoint);
    if (action==Step::Poisoned) return finish(action);
    progress|=action==Step::Progress; blocked|=action==Step::Blocked;
    const auto reply=sendControlResponseOnce(ring,socket,proxy,now);
    if (mLastReplyAffected.valid) add(mLastReplyAffected.endpoint);
    if (reply==Step::Poisoned) return finish(reply);
    progress|=reply==Step::Progress; blocked|=reply==Step::Blocked;
    const auto unreliable=sendUnreliableOnce(ring,socket,proxy,now);
    if (mLastUnreliableAffected.valid) add(mLastUnreliableAffected.endpoint);
    if (unreliable==Step::Poisoned) return finish(unreliable);
    progress|=unreliable==Step::Progress; blocked|=unreliable==Step::Blocked;
    return finish(progress ? Step::Progress : blocked ? Step::Blocked : Step::Idle);
}
