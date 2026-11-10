/** Main-side terminal credit admission for the sole transport owner. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTOWNERADMISSION_H
#define LL_LLTRANSPORTOWNERADMISSION_H
#include "lltransportownerapi.h"
#include "lltransportreceivepump.h"
#include <array>
#include <cstring>
#include <cstdint>

// Lives in the shared owner graph. Exactly one nonreentrant main producer uses
// this object and consumes terminals. The worker never accesses its counters.
// The caller holds a shared graph reference and commands.beginOfferOnMain()
// across callback reservation, this offer, and every rollback/last access;
// endOfferOnMain() happens only after those operations complete. The owner can
// close the command queue concurrently without waiting for the main producer.
class LLTransportOwnerAdmission
{
public:
    static constexpr uint32_t TerminalCapacity = 256;
    // The containing shared graph owns this sorted frozen schema table and
    // retains it across this offer, worker use, join and inbound drain.
    LLTransportOwnerAdmission(LLTransportOwnerChannels& channels, const LLTransportSchemaEntry* schemas,
                              std::size_t schemaCount) noexcept
        : mChannels(channels), mSchemas(schemas), mSchemaCount(schemaCount),
          mSchemaTableValid(validateSchemaTable(schemas,schemaCount)) {}
    LLTransportOwnerAdmission(const LLTransportOwnerAdmission&) = delete;
    LLTransportOwnerAdmission& operator=(const LLTransportOwnerAdmission&) = delete;
    bool schemaTableValidOnMain() const noexcept { return mSchemaTableValid; }

    // Called only inside the outer offer. The corresponding terminal credit is
    // reserved before the command's atomic publish and retained until main has
    // successfully released that terminal. Failed publish rolls back credit and
    // is never Accepted. Caller separately rolls back any callback reservation.
    LLTransportCommandOffer offerOnMain(const LLTransportOwnerCommand& command) noexcept
    {
        if (!mChannels.commands.offerActiveOnMain()) return LLTransportCommandOffer::Unavailable;
        auto* used=creditFor(command);
        if (!used) return LLTransportCommandOffer::Invalid;
        if (command.kind==LLTransportOwnerCommand::Kind::Send && !mSchemaTableValid)
            return LLTransportCommandOffer::Unavailable;
        if (command.kind==LLTransportOwnerCommand::Kind::Send && !validSendSchema(command))
            return LLTransportCommandOffer::Invalid;
        if (mChannels.commands.isClosed()) return LLTransportCommandOffer::Unavailable;
        if (*used>=TerminalCapacity) return LLTransportCommandOffer::Full;
        auto& records=recordsFor(used);
        const auto record=makeRecord(command);
        Credit* vacant=nullptr;
        for (auto& item:records)
        {
            if (item.operationId==record.operationId) return LLTransportCommandOffer::Invalid;
            if (!item.operationId && !vacant) vacant=&item;
        }
        if (!vacant) return LLTransportCommandOffer::Full;
        *vacant=record;
        ++*used;
        auto* slot=mChannels.commands.reserveProducer();
        if (!slot)
        {
            --*used;
            *vacant={};
            return mChannels.commands.isClosed() ? LLTransportCommandOffer::Unavailable : LLTransportCommandOffer::Full;
        }
        *slot=command;
        if (!mChannels.commands.publishProducer())
        {
            --*used;
            *vacant={};
            return LLTransportCommandOffer::Unavailable;
        }
        return LLTransportCommandOffer::Accepted;
    }

    // Validate identity BEFORE releasing the queue slot; only then release
    // the credit. An unknown/stale terminal must remain retained and fault.
    bool matchesCircuitOnMain(const LLTransportCircuitCompletion& value) const noexcept
    { return findCircuit(value)!=nullptr; }
    bool matchesReliableOnMain(const LLTransportSendCompletion& value) const noexcept
    { return findSend(mReliableRecords,value.endpoint,value.operationId,value.loginGeneration,value.circuitIncarnation)!=nullptr; }
    bool matchesUnreliableOnMain(const LLTransportUnreliableCompletion& value) const noexcept
    { return findSend(mUnreliableRecords,value.endpoint,value.operationId,value.loginGeneration,value.circuitIncarnation)!=nullptr; }
    bool releaseCircuitCreditOnMain(const LLTransportCircuitCompletion& value) noexcept
    { return release(findCircuit(value),mCircuit); }
    bool releaseReliableCreditOnMain(const LLTransportSendCompletion& value) noexcept
    { return release(findSend(mReliableRecords,value.endpoint,value.operationId,value.loginGeneration,value.circuitIncarnation),mReliable); }
    bool releaseUnreliableCreditOnMain(const LLTransportUnreliableCompletion& value) noexcept
    { return release(findSend(mUnreliableRecords,value.endpoint,value.operationId,value.loginGeneration,value.circuitIncarnation),mUnreliable); }
    bool emptyOnMain() const noexcept { return !mCircuit && !mReliable && !mUnreliable; }
    uint32_t circuitOutstandingOnMain() const noexcept { return mCircuit; }
    uint32_t reliableOutstandingOnMain() const noexcept { return mReliable; }
    uint32_t unreliableOutstandingOnMain() const noexcept { return mUnreliable; }
    // Main producer owns these records; a successful enable stays recorded
    // until its matched terminal is consumed. A pending enable masks absence.
    bool hasPendingEnableOnMain(LLTransportEndpoint endpoint) const noexcept
    {
        for (const auto& item : mCircuitRecords)
            if (item.operationId && item.kind==LLTransportOwnerCommand::Kind::Enable &&
                item.endpoint.addressNetworkOrder==endpoint.addressNetworkOrder &&
                item.endpoint.portHostOrder==endpoint.portHostOrder) return true;
        return false;
    }
private:
    static bool validateSchemaTable(const LLTransportSchemaEntry* schemas,std::size_t count) noexcept
    {
        if (!schemas || !count) return false;
        for (std::size_t i=0;i<count;++i)
        {
            const auto& entry=schemas[i];
            if (!entry.schema || !entry.schema->isFrozen() || !entry.schema->generation() ||
                entry.messageNumber!=entry.schema->messageNumber() ||
                (i && schemas[i-1].messageNumber>=entry.messageNumber)) return false;
        }
        return true;
    }
    bool validSendSchema(const LLTransportOwnerCommand& command) const noexcept
    {
        if (!mSchemas || !mSchemaCount) return false;
        // Match the worker's pre-compression header normalization and lookup.
        // This is bounded stack work and does not access the mutable builder.
        auto wire=command.bytes;
        std::memset(wire.data(),0,5);
        std::array<uint8_t,LLTransportCircuitState::MaxPacketBytes> expanded{};
        const auto envelope=llTransportParseEnvelope(wire.data(),command.byteCount,expanded.data(),expanded.size());
        if (envelope.status!=LLTransportEnvelope::Status::Complete) return false;
        std::size_t lo=0,hi=mSchemaCount;
        while (lo<hi)
        {
            const auto mid=lo+(hi-lo)/2;
            if (mSchemas[mid].messageNumber<envelope.messageNumber) lo=mid+1; else hi=mid;
        }
        if (lo==mSchemaCount) return false;
        const auto& entry=mSchemas[lo];
        return entry.messageNumber==envelope.messageNumber && entry.schema && entry.schema->isFrozen() &&
               entry.schema->messageNumber()==envelope.messageNumber && entry.schema->generation()==command.schemaGeneration;
    }
    struct Credit
    {
        uint64_t operationId=0, loginGeneration=0, incarnation=0, eventId=0, barrier=0;
        LLTransportEndpoint endpoint{};
        LLTransportOwnerCommand::Kind kind=LLTransportOwnerCommand::Kind::Stop;
    };
    using Records=std::array<Credit,TerminalCapacity>;
    static bool sameEndpoint(LLTransportEndpoint a,LLTransportEndpoint b) noexcept
    { return a.addressNetworkOrder==b.addressNetworkOrder && a.portHostOrder==b.portHostOrder; }
    Records& recordsFor(const uint32_t* count) noexcept
    { return count==&mCircuit ? mCircuitRecords : count==&mReliable ? mReliableRecords : mUnreliableRecords; }
    static Credit makeRecord(const LLTransportOwnerCommand& command) noexcept
    {
        using Kind=LLTransportOwnerCommand::Kind;
        Credit value; value.kind=command.kind;
        if (command.kind==Kind::Send)
        { value.operationId=command.sendToken; value.endpoint=command.circuit.endpoint; value.loginGeneration=command.circuit.loginGeneration; value.incarnation=command.circuit.targetIncarnation; }
        else if (command.kind==Kind::FinishWatchdogTimeout)
        { value.operationId=command.circuit.operationId; value.endpoint=command.timeout.endpoint; value.loginGeneration=command.timeout.loginGeneration; value.incarnation=command.timeout.incarnation; value.eventId=command.timeout.eventId; value.barrier=command.timeout.lastPublishedAppSequence; }
        else if (command.kind==Kind::SetAckCollectTime)
        { value.operationId=command.configOperationId; value.endpoint=command.circuit.endpoint; value.loginGeneration=command.circuit.loginGeneration; value.incarnation=command.circuit.targetIncarnation; }
        else
        { value.operationId=command.circuit.operationId; value.endpoint=command.circuit.endpoint; value.loginGeneration=command.circuit.loginGeneration; value.incarnation=command.circuit.targetIncarnation; }
        return value;
    }
    template<class RecordsType>
    static auto findSend(RecordsType& records, LLTransportEndpoint endpoint, uint64_t id, uint64_t login, uint64_t incarnation) noexcept
        -> decltype(&records[0])
    {
        if (!id) return nullptr;
        for (auto& item:records)
            if (item.operationId==id && item.loginGeneration==login && item.incarnation==incarnation && sameEndpoint(item.endpoint,endpoint)) return &item;
        return nullptr;
    }
    const Credit* findCircuit(const LLTransportCircuitCompletion& value) const noexcept
    { return const_cast<LLTransportOwnerAdmission*>(this)->findCircuit(value); }
    Credit* findCircuit(const LLTransportCircuitCompletion& value) noexcept
    {
        using Kind=LLTransportOwnerCommand::Kind;
        if (!value.operationId) return nullptr;
        for (auto& item:mCircuitRecords)
        {
            if (item.operationId!=value.operationId) continue;
            if (item.kind==Kind::SetAckCollectTime)
                return !value.timeoutEventId && sameEndpoint(item.endpoint,value.endpoint) &&
                    item.loginGeneration==value.loginGeneration && item.incarnation==value.circuitIncarnation ? &item : nullptr;
            if (!sameEndpoint(item.endpoint,value.endpoint) || item.loginGeneration!=value.loginGeneration) return nullptr;
            if (item.kind==Kind::Enable)
                return !value.timeoutEventId && (value.status!=LLTransportCircuitCommandStatus::Applied || value.circuitIncarnation) ? &item : nullptr;
            if (item.incarnation!=value.circuitIncarnation) return nullptr;
            if (item.kind==Kind::FinishWatchdogTimeout)
                return value.timeoutEventId==item.eventId && value.lastPublishedAppSequence==item.barrier ? &item : nullptr;
            return !value.timeoutEventId ? &item : nullptr;
        }
        return nullptr;
    }
    static bool release(Credit* record,uint32_t& count) noexcept
    { if (!record || !count) return false; *record={}; --count; return true; }
    uint32_t* creditFor(const LLTransportOwnerCommand& command) noexcept
    {
        using Kind=LLTransportOwnerCommand::Kind;
        const auto validEndpoint=[](LLTransportEndpoint endpoint) noexcept {
            return endpoint.addressNetworkOrder && endpoint.portHostOrder;
        };
        switch (command.kind)
        {
            case Kind::Send:
                if (!command.sendToken || !command.schemaGeneration || command.byteCount<7 ||
                    command.byteCount>LLTransportCircuitState::MaxPacketBytes ||
                    !validEndpoint(command.circuit.endpoint) || !command.circuit.loginGeneration ||
                    !command.circuit.targetIncarnation ||
                    (command.encoding!=LLTransportEncoding::Unencoded && command.encoding!=LLTransportEncoding::ZeroCoded)) return nullptr;
                return command.reliable ? &mReliable : &mUnreliable;
            case Kind::Enable:
                return command.circuit.operationId && command.circuit.loginGeneration &&
                    validEndpoint(command.circuit.endpoint) && command.circuit.kind==LLTransportCircuitCommandKind::Enable ? &mCircuit : nullptr;
            case Kind::Disable:
                return command.circuit.operationId && command.circuit.loginGeneration && command.circuit.targetIncarnation &&
                    validEndpoint(command.circuit.endpoint) && command.circuit.kind==LLTransportCircuitCommandKind::Disable ? &mCircuit : nullptr;
            case Kind::DisableIfUnchanged:
                return command.circuit.operationId && command.circuit.loginGeneration && command.circuit.targetIncarnation &&
                    validEndpoint(command.circuit.endpoint) && command.circuit.kind==LLTransportCircuitCommandKind::DisableIfUnchanged ? &mCircuit : nullptr;
            case Kind::FinishWatchdogTimeout:
                return command.circuit.operationId && command.timeout.eventId && command.timeout.loginGeneration &&
                    command.timeout.incarnation && validEndpoint(command.timeout.endpoint) ? &mCircuit : nullptr;
            case Kind::SetAckCollectTime:
                return command.configOperationId && command.ackCollectUsec<=2000000 &&
                    !command.circuit.endpoint.addressNetworkOrder && !command.circuit.endpoint.portHostOrder &&
                    !command.circuit.operationId && !command.circuit.loginGeneration && !command.circuit.targetIncarnation ? &mCircuit : nullptr;
            case Kind::Stop: return nullptr; // stop is an independent wake, not queued admission
        }
        return nullptr;
    }
    LLTransportOwnerChannels& mChannels;
    const LLTransportSchemaEntry* mSchemas=nullptr;
    std::size_t mSchemaCount=0;
    bool mSchemaTableValid=false;
    Records mCircuitRecords{},mReliableRecords{},mUnreliableRecords{};
    uint32_t mCircuit=0, mReliable=0, mUnreliable=0;
};
#endif
