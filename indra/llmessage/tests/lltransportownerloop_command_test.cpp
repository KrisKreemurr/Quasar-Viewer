// Actual owner circuit-command methods, state, receive pump and queues.
// Test-only template objects and packet ring stand in for external boundaries.
#include "lltransportownerloop.h"
#include "lltransportowneradmission.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>
#include <vector>
using U32=uint32_t;
constexpr int MBT_SINGLE=1, MBT_MULTIPLE=2, MBT_VARIABLE=3, MVT_VARIABLE=1, MVT_FIXED=2;
struct LLMessageVariable { int type,size; int getType() const { return type; } int getSize() const { return size; } };
struct LLMessageBlock { int mType=MBT_SINGLE,mNumber=1; std::vector<LLMessageVariable*> mMemberVariables; };
struct LLMessageTemplate { std::vector<LLMessageBlock*> mMemberBlocks; U32 mMessageNumber=42; };
#include "transport_schema_freeze.inc"
static void check(bool v,int line) { if (!v) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
struct Wake final : LLTransportMainWake
{
    uint64_t count=0;
    bool observe(uint64_t& out) const noexcept override { out=count; return true; }
    void signal() noexcept override { ++count; }
    LLTransportWakeResult waitUntil(uint64_t,LLTransportSteadyUsec) noexcept override { return LLTransportWakeResult::Unavailable; }
};
int main()
{
    using State=LLTransportCircuitState; using Command=LLTransportOwnerCommand;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({2,4,32,32,4,16,8388608},charged));
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLMessageVariable field{MVT_FIXED,1}; LLMessageBlock block; block.mMemberVariables.push_back(&field);
    LLMessageTemplate source; source.mMemberBlocks.push_back(&block);
    LLTransportStartupBudget budget;
    CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema schema,pingSchema; CHECK(freezeTransportSchema(source,8,budget,schema)==LLTransportFreezeStatus::Complete);
    source.mMessageNumber=2; CHECK(freezeTransportSchema(source,8,budget,pingSchema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry entries[]={{2,&pingSchema,false,true},{42,&schema,false,false}};
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poisoned{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poisoned,entries,2);
    auto wake=std::make_shared<Wake>();
    LLTransportOwnerLoop loop(*state,*pump,*channels,poisoned,wake);
    CHECK(loop.initializeWatchdogs(2) && !loop.initializeWatchdogs(2));
    CHECK(LLTransportOwnerLoop::watchdogChargeBytes(2)>0);
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const State::WatchdogConfig watchdog{1000000,60000000,0.25f,0.5f};
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>(); CHECK(directory->activateOnOwner());
    auto processHead=[&](LLTransportSteadyUsec now,State::WatchdogConfig value) {
        CHECK(directory->beginStateMutationOnOwner());
        const auto result=loop.processCircuitHead(now,value,directory.get());
        CHECK(directory->endStateMutationOnOwner()); return result;
    };
    auto submit=[&](const Command& cmd) { CHECK(channels->commands.beginOfferOnMain()); auto* slot=channels->commands.reserveProducer(); CHECK(slot); *slot=cmd; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain(); };
    auto pop=[&]() { const auto* slot=channels->circuitTerminals.peekConsumer(); CHECK(slot); auto value=*slot; CHECK(channels->circuitTerminals.releaseConsumer()); return value; };
    Command cmd; cmd.kind=Command::Kind::Enable; cmd.circuit.kind=LLTransportCircuitCommandKind::Enable;
    cmd.circuit.endpoint=endpoint; cmd.circuit.loginGeneration=7; cmd.circuit.operationId=1; cmd.circuit.trusted=true;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(100),watchdog)==LLTransportOwnerLoop::Step::Progress);
    auto enabled=pop(); CHECK(enabled.status==LLTransportCircuitCommandStatus::Applied && enabled.circuitIncarnation && wake->count==1);
    ring.packet={0x40,0,0,0,1,0,42,3};
    CHECK(loop.pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::Published && wake->count==2);
    CHECK(pump->affectedCircuit().valid && pump->affectedCircuit().incarnation==enabled.circuitIncarnation);
    CHECK(loop.initializePending());
    Command outbound; outbound.kind=Command::Kind::Send; outbound.reliable=true;
    outbound.sendToken=10; outbound.schemaGeneration=8; outbound.circuit.endpoint=endpoint;
    outbound.circuit.targetIncarnation=enabled.circuitIncarnation; outbound.circuit.loginGeneration=7;
    outbound.retry={1,10000000,10000000,false}; outbound.byteCount=8;
    outbound.bytes[6]=42; outbound.bytes[7]=7;
    submit(outbound);
    CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(101),7,entries,2)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(101),1000000)==LLTransportOwnerLoop::Step::Progress);
    CHECK(!ring.sentPackets.empty()); const auto sentId=llTransportReadNetworkU32(ring.sentPackets.back().data()+1);
    CHECK(state->consumeAck(endpoint,enabled.circuitIncarnation,sentId)==State::Status::Ok);
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* sendDone=channels->sendTerminals.peekConsumer(); CHECK(sendDone && sendDone->operationId==10 && sendDone->originalStatus==0);
    CHECK(channels->sendTerminals.releaseConsumer());
    // An old live circuit can persist while main advances to a new login.
    // Matching only the current login argument + old incarnation is unsafe.
    outbound.sendToken=14; outbound.circuit.loginGeneration=8;
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(101),8,entries,2)==LLTransportOwnerLoop::Step::Progress);
    const auto* wrongLogin=channels->sendTerminals.peekConsumer();
    CHECK(wrongLogin && wrongLogin->operationId==14 && wrongLogin->originalStatus==-23017);
    CHECK(channels->sendTerminals.releaseConsumer());
    outbound.circuit.loginGeneration=7;
    const auto before=state->lookup(endpoint,101);
    // First trailer ACK completes an earlier send; second hits the currently
    // outstanding initial action. Resumption must continue at the second ID.
    auto prepare=[&](uint64_t token) {
        CHECK(state->reserveReliableSlot(endpoint,enabled.circuitIncarnation,token)==State::Status::Ok);
        CHECK(state->acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(state->allocateReliablePacketId(token,id)==State::Status::Ok);
        const uint8_t packet[]={0x40,0,0,0,1,0,42,1};
        CHECK(state->prepareReliable(token,id,packet,sizeof(packet),{1,10000000,10000000,false},101)==State::Status::Ok);
        return id;
    };
    const auto firstId=prepare(11); auto firstAction=state->advance(101,1000000);
    CHECK(firstAction.kind==State::ActionKind::InitialSend && firstAction.reliableToken==11);
    CHECK(state->commitAttempt(firstAction.token,101,0,true,8)==State::Status::Ok);
    const auto secondId=prepare(12); auto secondAction=state->advance(101,1000000);
    CHECK(secondAction.kind==State::ActionKind::InitialSend && secondAction.reliableToken==12);
    ring.packet={0x50,0,0,0,2,0,42,5};
    auto trailer=[&](uint32_t id) { ring.packet.push_back(uint8_t(id>>24)); ring.packet.push_back(uint8_t(id>>16)); ring.packet.push_back(uint8_t(id>>8)); ring.packet.push_back(uint8_t(id)); };
    trailer(secondId); trailer(firstId); ring.packet.push_back(2); // reverse trailer order
    CHECK(loop.pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::ControlPending);
    CHECK(pump->pendingKind()==LLTransportPendingKind::BusyAppendedAck);
    CHECK(pump->affectedCircuit().valid && pump->affectedCircuit().incarnation==enabled.circuitIncarnation);
    CHECK(!pump->releaseControl());
    CHECK(!pump->discardPendingIfStale(8,LLTransportSteadyUsec(101))); // partial appended ACK remains held
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    auto* firstDone=channels->sendTerminals.peekConsumer(); CHECK(firstDone && firstDone->operationId==11);
    CHECK(channels->sendTerminals.releaseConsumer());
    CHECK(state->defer(secondAction.token)==State::Status::Ok);
    const auto wakeBeforeResume=wake->count;
    CHECK(loop.resumeBusyAcknowledgements(7)==LLTransportRxStatus::Published && wake->count==wakeBeforeResume+1);
    CHECK(pump->affectedCircuit().valid && pump->affectedCircuit().incarnation==enabled.circuitIncarnation);
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    auto* secondDone=channels->sendTerminals.peekConsumer(); CHECK(secondDone && secondDone->operationId==12);
    CHECK(channels->sendTerminals.releaseConsumer());
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Idle);
    cmd.kind=Command::Kind::DisableIfUnchanged; cmd.circuit.kind=LLTransportCircuitCommandKind::DisableIfUnchanged;
    cmd.circuit.targetIncarnation=enabled.circuitIncarnation; cmd.circuit.operationId=2;
    cmd.circuit.observedHasLastPacket=before.hasLastPacket; cmd.circuit.observedLastPacketMonotonicUsec=before.lastPacketMonotonicUsec;
    cmd.circuit.observedPublicationSequence=before.publicationSequence;
    ring.packet={0x40,0,0,0,3,0,42,4};
    CHECK(loop.pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::Published);
    ring.packet={0,0,0,0,4,0,2,0};
    CHECK(loop.pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::ControlPending);
    CHECK(loop.servicePendingControl(LLTransportSteadyUsec(101),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pump->affectedCircuit().valid && pump->affectedCircuit().incarnation==enabled.circuitIncarnation);
    CHECK(loop.pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::Idle);
    CHECK(!pump->affectedCircuit().valid);
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(102),watchdog)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pop().status==LLTransportCircuitCommandStatus::PacketChanged); // same-tick arrival still invalidates stale close
    cmd.kind=Command::Kind::Disable; cmd.circuit.kind=LLTransportCircuitCommandKind::Disable; cmd.circuit.operationId=3;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(103),watchdog)==LLTransportOwnerLoop::Step::Progress);
    const auto closed=pop(); CHECK(closed.status==LLTransportCircuitCommandStatus::Applied && closed.lastPublishedAppSequence==3);
    cmd.kind=Command::Kind::Enable; cmd.circuit.kind=LLTransportCircuitCommandKind::Enable; cmd.circuit.loginGeneration=8; cmd.circuit.operationId=4;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(104),watchdog)==LLTransportOwnerLoop::Step::Progress);
    const auto next=pop(); CHECK(next.status==LLTransportCircuitCommandStatus::Applied && next.circuitIncarnation!=enabled.circuitIncarnation);
    cmd.kind=Command::Kind::Disable; cmd.circuit.kind=LLTransportCircuitCommandKind::Disable;
    cmd.circuit.loginGeneration=7; cmd.circuit.targetIncarnation=enabled.circuitIncarnation; cmd.circuit.operationId=5;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(105),watchdog)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pop().status==LLTransportCircuitCommandStatus::IncarnationChanged);
    CHECK(state->lookup(endpoint,105).incarnation==next.circuitIncarnation);
    outbound.sendToken=15; // old login/incarnation cannot send to a reopened circuit
    submit(outbound);
    CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(105),8,entries,2)==LLTransportOwnerLoop::Step::Progress);
    const auto* staleSend=channels->sendTerminals.peekConsumer();
    CHECK(staleSend && staleSend->operationId==15 && staleSend->originalStatus==-23017);
    CHECK(channels->sendTerminals.releaseConsumer());
    outbound.sendToken=16; outbound.circuit.loginGeneration=8; outbound.circuit.targetIncarnation=next.circuitIncarnation;
    outbound.encoding=static_cast<LLTransportEncoding>(255); // inject post-accept encoding failure
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(105),8,entries,2)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.lastPreparationFailure()==LLTransportOwnerLoop::PreparationFailure::EncodeFailed);
    poisoned.store(true); // Completion draining remains available under poison.
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* encodeFailure=channels->sendTerminals.peekConsumer();
    CHECK(encodeFailure && encodeFailure->operationId==16 && encodeFailure->originalStatus==-23016);
    CHECK(channels->sendTerminals.releaseConsumer()); poisoned.store(false); // test-only isolation
    outbound.encoding=LLTransportEncoding::Unencoded; outbound.sendToken=17;
    const auto heldReceive=state->reserveReceive(endpoint,1,false,false,105);
    CHECK(heldReceive.status==State::ReceiveStatus::New);
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(105),8,entries,2)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(!channels->sendTerminals.peekConsumer());
    state->cancelReceive(heldReceive.reservation);
    CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(105),8,entries,2)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(105),1000000)==LLTransportOwnerLoop::Step::Progress);
    const auto resumedId=llTransportReadNetworkU32(ring.sentPackets.back().data()+1);
    CHECK(state->consumeAck(endpoint,next.circuitIncarnation,resumedId)==State::Status::Ok);
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* allocationFailure=channels->sendTerminals.peekConsumer();
    CHECK(allocationFailure && allocationFailure->operationId==17 && allocationFailure->originalStatus==0);
    CHECK(channels->sendTerminals.releaseConsumer());
    CHECK(events->peekConsumer()->circuitIncarnation==enabled.circuitIncarnation);
    // Full close-terminal lane cannot block a reliable completion; a new close
    // must wait for terminal storage before it changes circuit state.
    for (unsigned i=0;i<256;++i)
    { auto* slot=channels->circuitTerminals.reserveProducer(); CHECK(slot); *slot={}; CHECK(channels->circuitTerminals.publishProducer()); }
    CHECK(state->reserveReliableSlot(endpoint,next.circuitIncarnation,99)==State::Status::Ok);
    CHECK(state->acceptReliableSlot(99)==State::Status::Ok);
    CHECK(state->failReliable(99,-23)==State::Status::Ok);
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* send=channels->sendTerminals.peekConsumer();
    CHECK(send && send->operationId==99 && send->originalStatus==-23 && send->circuitIncarnation==next.circuitIncarnation);
    CHECK(channels->sendTerminals.releaseConsumer());
    cmd.circuit.loginGeneration=8; cmd.circuit.targetIncarnation=next.circuitIncarnation; cmd.circuit.operationId=6;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(106),watchdog)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(state->lookup(endpoint,106).incarnation==next.circuitIncarnation);
    (void)pop();
    CHECK(processHead(LLTransportSteadyUsec(107),watchdog)==LLTransportOwnerLoop::Step::Progress);
    CHECK(state->lookup(endpoint,107).state==LLTransportSnapshotState::Absent);
    for (unsigned i=0;i<256;++i) (void)pop();
    const auto third=state->enable(endpoint,9,true,108,watchdog); CHECK(third.status==State::Status::Ok);
    auto pendingAck=[&](uint64_t incarnation,uint64_t token,uint64_t now) {
        CHECK(state->reserveReliableSlot(endpoint,incarnation,token)==State::Status::Ok);
        CHECK(state->acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(state->allocateReliablePacketId(token,id)==State::Status::Ok);
        const uint8_t packet[]={0x40,0,0,0,1,0,42,1};
        CHECK(state->prepareReliable(token,id,packet,sizeof(packet),{1,10000000,10000000,false},now)==State::Status::Ok);
        auto action=state->advance(now,1000000); CHECK(action.kind==State::ActionKind::InitialSend && action.reliableToken==token);
        ring.packet={0x10,0,0,0,1,0,42,5}; trailer(id); ring.packet.push_back(1);
        CHECK(loop.pollReceive(0,proxy,9,LLTransportSteadyUsec(now+1))==LLTransportRxStatus::ControlPending);
        return action;
    };
    auto thirdAction=pendingAck(third.incarnation,101,108);
    const auto sequenceBefore=pump->lastPublishedAppSequence();
    poisoned.store(true);
    CHECK(loop.resumeBusyAcknowledgements(9)==LLTransportRxStatus::Poisoned);
    CHECK(!pump->releaseControl() && pump->lastPublishedAppSequence()==sequenceBefore);
    poisoned.store(false); // Test-only reset to exercise independent stale-incarnation branch.
    CHECK(state->disable(endpoint,third.incarnation,sequenceBefore).status==LLTransportCircuitCommandStatus::Applied);
    const auto fourth=state->enable(endpoint,9,true,110,watchdog); CHECK(fourth.status==State::Status::Ok && fourth.incarnation!=third.incarnation);
    CHECK(loop.resumeBusyAcknowledgements(9)==LLTransportRxStatus::Rejected);
    CHECK(!pump->affectedCircuit().valid);
    CHECK(pump->lastPublishedAppSequence()==sequenceBefore);
    (void)thirdAction;
    auto fourthAction=pendingAck(fourth.incarnation,102,111);
    CHECK(loop.resumeBusyAcknowledgements(10)==LLTransportRxStatus::Rejected); // stale login
    CHECK(pump->lastPublishedAppSequence()==sequenceBefore);
    CHECK(state->defer(fourthAction.token)==State::Status::Ok);
    CHECK(loop.pollWatchdog(endpoint,fourth.incarnation,LLTransportSteadyUsec(61000112),0.5f)==LLTransportOwnerLoop::Step::Progress);
    const auto* timed=channels->controls.peekConsumer();
    CHECK(timed && timed->circuitIncarnation==fourth.incarnation && timed->lastPublishedAppSequence==sequenceBefore);
    State::TimeoutEvent timeout{timed->endpoint,timed->loginGeneration,timed->circuitIncarnation,timed->eventId,timed->lastPublishedAppSequence};
    CHECK(channels->controls.releaseConsumer());
    outbound.sendToken=18; outbound.circuit.loginGeneration=9; outbound.circuit.targetIncarnation=fourth.incarnation;
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(61000112),9,entries,2)==LLTransportOwnerLoop::Step::Progress);
    const auto* deadSend=channels->sendTerminals.peekConsumer();
    CHECK(deadSend && deadSend->operationId==18 && deadSend->originalStatus==-23017 && !poisoned.load());
    CHECK(channels->sendTerminals.releaseConsumer());
    cmd={}; cmd.kind=Command::Kind::FinishWatchdogTimeout; cmd.timeout=timeout; cmd.circuit.operationId=19;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(61000113),watchdog)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pop().status==LLTransportCircuitCommandStatus::Applied && state->lookup(endpoint,61000113).state==LLTransportSnapshotState::Absent);
    // Earlier pendingAck test values are real accepted sends. Their circuit
    // closures produce terminals; consume both before starting the next case.
    bool closed101=false,closed102=false;
    while (loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress)
    {
        const auto* terminal=channels->sendTerminals.peekConsumer(); CHECK(terminal && terminal->originalStatus==-23017);
        if (terminal->operationId==101) { CHECK(!closed101); closed101=true; }
        else if (terminal->operationId==102) { CHECK(!closed102); closed102=true; }
        else CHECK(false);
        CHECK(channels->sendTerminals.releaseConsumer());
    }
    CHECK(closed101 && closed102);
    // An admitted head blocked on a live receive reservation must not be
    // re-reserved; closing the circuit settles the one existing state slot.
    const auto fifth=state->enable(endpoint,10,true,61000114,watchdog); CHECK(fifth.status==State::Status::Ok);
    outbound.sendToken=19; outbound.circuit.loginGeneration=10; outbound.circuit.targetIncarnation=fifth.incarnation;
    const auto busy=state->reserveReceive(endpoint,1,false,false,61000114); CHECK(busy.status==State::ReceiveStatus::New);
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(61000114),10,entries,2)==LLTransportOwnerLoop::Step::Blocked);
    constexpr uint64_t timeoutNow=121000115;
    CHECK(loop.pollWatchdog(endpoint,fifth.incarnation,LLTransportSteadyUsec(timeoutNow),0.5f)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(state->lookup(endpoint,timeoutNow).alive);
    state->cancelReceive(busy.reservation);
    CHECK(loop.pollWatchdog(endpoint,fifth.incarnation,LLTransportSteadyUsec(timeoutNow),0.5f)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(timeoutNow),10,entries,2)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.pollWatchdog(endpoint,fifth.incarnation,LLTransportSteadyUsec(timeoutNow),0.5f)==LLTransportOwnerLoop::Step::Progress);
    const auto* fifthControl=channels->controls.peekConsumer(); CHECK(fifthControl);
    State::TimeoutEvent fifthTimeout{fifthControl->endpoint,fifthControl->loginGeneration,fifthControl->circuitIncarnation,
                                     fifthControl->eventId,fifthControl->lastPublishedAppSequence};
    CHECK(channels->controls.releaseConsumer());
    // Main callback revival must precede its matching ordered Finish command.
    cmd={}; cmd.kind=Command::Kind::Enable; cmd.circuit.kind=LLTransportCircuitCommandKind::Enable;
    cmd.circuit.endpoint=endpoint; cmd.circuit.loginGeneration=10; cmd.circuit.trusted=true; cmd.circuit.operationId=20;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(timeoutNow+1),watchdog)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pop().status==LLTransportCircuitCommandStatus::Applied);
    cmd={}; cmd.kind=Command::Kind::FinishWatchdogTimeout; cmd.timeout=fifthTimeout; cmd.circuit.operationId=21;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(timeoutNow+2),watchdog)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pop().status==LLTransportCircuitCommandStatus::Applied && state->lookup(endpoint,timeoutNow+2).alive);
    outbound.sendToken=20;
    const auto busyClose=state->reserveReceive(endpoint,2,false,false,timeoutNow+3); CHECK(busyClose.status==State::ReceiveStatus::New);
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(timeoutNow+3),10,entries,2)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(state->disable(endpoint,fifth.incarnation,sequenceBefore).status==LLTransportCircuitCommandStatus::Applied);
    // driveOnce services the queued head first, then publishes the prior
    // cancelled first attempt; the following turn can advance token 20.
    CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(timeoutNow+4),10,entries,2)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* prior=channels->sendTerminals.peekConsumer();
    CHECK(prior && prior->operationId==19 && prior->originalStatus==-23017);
    CHECK(channels->sendTerminals.releaseConsumer());
    CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(timeoutNow+4),10,entries,2)==LLTransportOwnerLoop::Step::Progress);
    // The stale queued head publishes its terminal directly, not through the
    // state-completion publisher. Validate it before releasing its queue slot.
    const auto* closedHead=channels->sendTerminals.peekConsumer();
    CHECK(closedHead && closedHead->operationId==20 && closedHead->originalStatus==-23017);
    CHECK(channels->sendTerminals.releaseConsumer());
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Idle);
    // Stop can settle that same blocked-head state without main draining.
    const auto sixth=state->enable(endpoint,11,true,timeoutNow+5,watchdog); CHECK(sixth.status==State::Status::Ok);
    outbound.sendToken=21; outbound.circuit.loginGeneration=11; outbound.circuit.targetIncarnation=sixth.incarnation;
    const auto busyStop=state->reserveReceive(endpoint,1,false,false,timeoutNow+5); CHECK(busyStop.status==State::ReceiveStatus::New);
    submit(outbound); CHECK(loop.processReliableSendHead(LLTransportSteadyUsec(timeoutNow+5),11,entries,2)==LLTransportOwnerLoop::Step::Blocked);
    loop.cancelInFlightForStop(); CHECK(!channels->commands.peekConsumer());
    CHECK(loop.publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* stopped=channels->sendTerminals.peekConsumer();
    CHECK(stopped && stopped->operationId==21 && stopped->originalStatus==-23017);
    CHECK(channels->sendTerminals.releaseConsumer());
    cmd={}; cmd.kind=Command::Kind::SetAckCollectTime; cmd.configOperationId=24; cmd.ackCollectUsec=1500000;
    cmd.circuit.endpoint=endpoint; cmd.circuit.loginGeneration=11; cmd.circuit.targetIncarnation=sixth.incarnation;
    submit(cmd); CHECK(processHead(LLTransportSteadyUsec(timeoutNow+6),watchdog)==LLTransportOwnerLoop::Step::Progress);
    const auto configNormal=pop();
    CHECK(configNormal.operationId==24 && configNormal.status==LLTransportCircuitCommandStatus::Applied &&
          !configNormal.endpoint.addressNetworkOrder && !configNormal.endpoint.portHostOrder &&
          !configNormal.loginGeneration && !configNormal.circuitIncarnation && !configNormal.lastPublishedAppSequence);
    // Accepted queued work is cancelled into its pre-reserved independent
    // terminal lanes without requiring main to consume either lane first.
    cmd={}; cmd.kind=Command::Kind::Enable; cmd.circuit.kind=LLTransportCircuitCommandKind::Enable;
    cmd.circuit.endpoint=endpoint; cmd.circuit.loginGeneration=11; cmd.circuit.operationId=22;
    submit(cmd);
    outbound.sendToken=23; submit(outbound);
    outbound.reliable=false; outbound.sendToken=26; submit(outbound); outbound.reliable=true;
    CHECK(sequenceBefore>0);
    cmd={}; cmd.kind=Command::Kind::SetAckCollectTime; cmd.configOperationId=25; cmd.ackCollectUsec=1;
    cmd.circuit.endpoint=endpoint; cmd.circuit.loginGeneration=11; cmd.circuit.targetIncarnation=sixth.incarnation; submit(cmd);
    cmd={}; cmd.kind=Command::Kind::FinishWatchdogTimeout; cmd.circuit.operationId=27;
    cmd.timeout={endpoint,11,sixth.incarnation,77,sequenceBefore}; submit(cmd);
    cmd={}; cmd.kind=Command::Kind::Stop; submit(cmd);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Idle);
    const auto circuitStop=pop();
    CHECK(circuitStop.operationId==22 && circuitStop.status==LLTransportCircuitCommandStatus::Cancelled);
    const auto configStop=pop();
    CHECK(configStop.operationId==25 && configStop.status==LLTransportCircuitCommandStatus::Cancelled &&
          !configStop.endpoint.addressNetworkOrder && !configStop.endpoint.portHostOrder &&
          !configStop.loginGeneration && !configStop.circuitIncarnation && !configStop.lastPublishedAppSequence);
    const auto timeoutStop=pop();
    CHECK(timeoutStop.operationId==27 && timeoutStop.timeoutEventId==77 &&
          timeoutStop.circuitIncarnation==sixth.incarnation && timeoutStop.lastPublishedAppSequence==sequenceBefore &&
          timeoutStop.status==LLTransportCircuitCommandStatus::Cancelled);
    const auto* queuedStop=channels->sendTerminals.peekConsumer();
    CHECK(queuedStop && queuedStop->operationId==23 && queuedStop->originalStatus==-23017);
    CHECK(channels->sendTerminals.releaseConsumer());
    const auto* unreliableStop=channels->unreliableTerminals.peekConsumer();
    CHECK(unreliableStop && unreliableStop->operationId==26 && unreliableStop->status==LLTransportUnreliableStatus::Cancelled);
    CHECK(channels->unreliableTerminals.releaseConsumer());
    // Exercise the actual producer credit matcher against the actual owner
    // terminal paths after an APP event has already established a barrier.
    CHECK(pump->lastPublishedAppSequence()>0);
    LLTransportOwnerAdmission admission(*channels,entries,2);
    cmd={}; cmd.kind=Command::Kind::SetAckCollectTime; cmd.configOperationId=71; cmd.ackCollectUsec=12;
    CHECK(channels->commands.beginOfferOnMain());
    cmd.circuit.endpoint=endpoint;
    CHECK(admission.offerOnMain(cmd)==LLTransportCommandOffer::Invalid);
    cmd.circuit.endpoint={};
    CHECK(admission.offerOnMain(cmd)==LLTransportCommandOffer::Accepted);
    channels->commands.endOfferOnMain();
    CHECK(processHead(LLTransportSteadyUsec(timeoutNow+7),watchdog)==LLTransportOwnerLoop::Step::Progress);
    const auto* matched=channels->circuitTerminals.peekConsumer();
    CHECK(matched && matched->status==LLTransportCircuitCommandStatus::Applied && admission.matchesCircuitOnMain(*matched));
    const auto applied=*matched; CHECK(channels->circuitTerminals.releaseConsumer());
    CHECK(admission.releaseCircuitCreditOnMain(applied));
    cmd.configOperationId=72;
    CHECK(channels->commands.beginOfferOnMain());
    CHECK(admission.offerOnMain(cmd)==LLTransportCommandOffer::Accepted);
    channels->commands.endOfferOnMain();
    CHECK(loop.settleQueuedForStop()==LLTransportOwnerLoop::Step::Progress);
    matched=channels->circuitTerminals.peekConsumer();
    CHECK(matched && matched->status==LLTransportCircuitCommandStatus::Cancelled && admission.matchesCircuitOnMain(*matched));
    const auto cancelled=*matched; CHECK(channels->circuitTerminals.releaseConsumer());
    CHECK(admission.releaseCircuitCreditOnMain(cancelled) && admission.emptyOnMain());
    // This validates value barriers and stale close rejection; main dispatch and crossing handlers are separate.
}
