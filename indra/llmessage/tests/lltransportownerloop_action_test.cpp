// Actual owner state-action scheduling, state and queues; fake socket only.
#include "lltransportownerloop.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({2,4,32,32,4,16,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const auto enabled=state->enable(endpoint,7,true,100,{1000000,60000000,0.25f,0.5f});
    CHECK(enabled.status==State::Status::Ok);
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,nullptr,0);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    CHECK(loop->initializePending() && !loop->initializePending());
    CHECK(LLTransportOwnerLoop::pendingChargeBytes()>0);
    auto reliable=[&](uint64_t token,uint64_t now) {
        CHECK(state->reserveReliableSlot(endpoint,enabled.incarnation,token)==State::Status::Ok);
        CHECK(state->acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(state->allocateReliablePacketId(token,id)==State::Status::Ok);
        const uint8_t bytes[]={0x40,0,0,0,uint8_t(id),0,42,0x78};
        CHECK(state->prepareReliable(token,id,bytes,sizeof(bytes),{1,1000000,1000000,false},now)==State::Status::Ok);
        return id;
    };
    const auto firstId=reliable(11,101);
    const auto receive=state->reserveReceive(endpoint,9,false,true,102);
    CHECK(receive.status==State::ReceiveStatus::New && state->commitReceive(receive.reservation,8,102)==State::Status::Ok);
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(103),0)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(ring.sentPackets.size()==1 && ring.firstAttempts[0]);
    const auto original=ring.sentPackets[0];
    CHECK((original[0]&0x10)!=0 && original.back()==1 && original[original.size()-2]==9);
    // ACK 9 was consumed exactly once at the first socket attempt. A newer
    // ACK can progress independently while that exact datagram is retained.
    // Legacy message.cpp erases piggyback ACKs without resetting the creation
    // age, and LLCircuit::sendAcks does not reset it on an empty list. ACK 10
    // inherits ACK 9's older age; collect=5 is due at 110, despite its age 1.
    const auto newer=state->reserveReceive(endpoint,10,false,true,109);
    CHECK(newer.status==State::ReceiveStatus::New && state->commitReceive(newer.reservation,8,109)==State::Status::Ok);
    ring.sendResult={NetSendStatus::Sent,0};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(110),5)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==2 && ring.sentPackets[1][6]==0xff && ring.sentPackets[1][11]==10); // ACK while data deferred
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(111),0)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(ring.sentPackets.size()==3 && ring.sentPackets[2]==original && !ring.firstAttempts[2]);
    const auto latest=state->reserveReceive(endpoint,11,false,true,111);
    CHECK(latest.status==State::ReceiveStatus::New && state->commitReceive(latest.reservation,8,111)==State::Status::Ok);
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(112))==LLTransportRxStatus::Idle);
    ring.sendResult={NetSendStatus::Sent,0};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(113),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==4 && ring.sentPackets[3][6]==0xff); // ACK 11 was not consumed again
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(114),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==5 && ring.sentPackets[4]==original && !ring.firstAttempts[4]);
    // POSIX's legacy third-attempt convention is retained. After the resend
    // is deferred, an actual trailer ACK resolves it before reacquisition.
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(1000102),0)==LLTransportOwnerLoop::Step::Blocked);
    ring.packet={0x10,0,0,0,2,0,42,1,uint8_t(firstId>>24),uint8_t(firstId>>16),uint8_t(firstId>>8),uint8_t(firstId),1};
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(1000103))==LLTransportRxStatus::Rejected); // ACK precedes absent schema
    CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    auto* firstCompletion=channels->sendTerminals.peekConsumer(); CHECK(firstCompletion && firstCompletion->operationId==11);
    CHECK(channels->sendTerminals.releaseConsumer());
    const auto afterAck=ring.sentPackets.size();
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(1000104),0)==LLTransportOwnerLoop::Step::Idle);
    CHECK(ring.sentPackets.size()==afterAck);
    (void)reliable(12,1000105); ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(1000106),0)==LLTransportOwnerLoop::Step::Blocked);
    LLTransportOwnerCommand close; close.kind=LLTransportOwnerCommand::Kind::Disable;
    close.circuit.kind=LLTransportCircuitCommandKind::Disable; close.circuit.endpoint=endpoint;
    close.circuit.targetIncarnation=enabled.incarnation; close.circuit.loginGeneration=7; close.circuit.operationId=77;
    CHECK(channels->commands.beginOfferOnMain()); auto* slot=channels->commands.reserveProducer(); CHECK(slot); *slot=close; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(1000107),{1000000,60000000,0.25f,0.5f})==LLTransportOwnerLoop::Step::Progress);
    const auto count=ring.sentPackets.size();
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(1000108),0)==LLTransportOwnerLoop::Step::Idle);
    CHECK(ring.sentPackets.size()==count);
    CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* completion=channels->sendTerminals.peekConsumer(); CHECK(completion && completion->originalStatus!=0);
    CHECK(channels->sendTerminals.releaseConsumer());
    // Isolated worker-side shutdown slice: 252 already queued terminal values
    // plus four accepted real state sends exhaust the 256-credit lane. Main
    // remains paused while cancellation settles all four into their credits.
    const auto reopened=state->enable(endpoint,8,true,1000200,{1000000,60000000,0.25f,0.5f});
    CHECK(reopened.status==State::Status::Ok);
    for (uint64_t i=0;i<252;++i)
    {
        auto* terminal=channels->sendTerminals.reserveProducer(); CHECK(terminal);
        *terminal={endpoint,1000+i,8,reopened.incarnation,0}; CHECK(channels->sendTerminals.publishProducer());
    }
    for (uint64_t token=201;token<=204;++token)
    {
        CHECK(state->reserveReliableSlot(endpoint,reopened.incarnation,token)==State::Status::Ok);
        CHECK(state->acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(state->allocateReliablePacketId(token,id)==State::Status::Ok);
        const uint8_t bytes[]={0x40,0,0,0,uint8_t(id),0,42,1};
        CHECK(state->prepareReliable(token,id,bytes,sizeof(bytes),{1,1000000,1000000,false},1000201)==State::Status::Ok);
    }
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(1000202),0)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(ring.sentPackets.back()!=original && ring.sentPackets.back()[7]==1); // reopened circuit reuses ID, not old bytes
    const auto sentBeforeStop=ring.sentPackets.size();
    poison.store(true); // Terminal delivery is allowed during fatal teardown.
    loop->cancelInFlightForStop();
    // Deliberately violate the credit invariant to prove a pending state
    // completion cannot be consumed when its terminal lane is already full.
    for (unsigned i=0;i<4;++i)
    {
        auto* extra=channels->sendTerminals.reserveProducer(); CHECK(extra);
        *extra={endpoint,9000+i,8,reopened.incarnation,0}; CHECK(channels->sendTerminals.publishProducer());
    }
    CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Blocked);
    for (unsigned i=0;i<4;++i) { CHECK(channels->sendTerminals.peekConsumer()); CHECK(channels->sendTerminals.releaseConsumer()); }
    for (unsigned i=0;i<4;++i) CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Idle); // full lane, no state completion
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(1000203),0)==LLTransportOwnerLoop::Step::Poisoned);
    CHECK(ring.sentPackets.size()==sentBeforeStop);
    for (unsigned i=0;i<256;++i)
    {
        const auto* item=channels->sendTerminals.peekConsumer(); CHECK(item);
        if (i>=252) CHECK(item->operationId>=201 && item->operationId<=204 && item->originalStatus==-23017);
        CHECK(channels->sendTerminals.releaseConsumer());
    }
    // A delayed resend keeps the first-attempt time AND interval. A later
    // scheduler iteration must not extend its next deadline on WouldBlock.
    auto delayed=std::make_unique<State>(); uint64_t delayedCharge=0;
    CHECK(delayed->init({1,1,1,1,1,1,8388608},delayedCharge));
    const auto delayedCircuit=delayed->enable(endpoint,22,true,0,{1000000,60000000,0.f,0.5f});
    CHECK(delayedCircuit.status==State::Status::Ok);
    auto delayedChannels=std::make_unique<LLTransportOwnerChannels>();
    auto delayedEvents=std::make_unique<LLTransportIngressQueue>();
    auto delayedArena=std::make_unique<LLTransportDecodedArena>(); CHECK(delayedArena->initialize(1024,512));
    LLPacketRing delayedRing; std::atomic<bool> delayedPoison{false};
    auto delayedPump=std::make_unique<LLTransportReceivePump>(delayedRing,*delayed,*delayedArena,*delayedEvents,delayedPoison,nullptr,0);
    auto delayedLoop=std::make_unique<LLTransportOwnerLoop>(*delayed,*delayedPump,*delayedChannels,delayedPoison);
    CHECK(delayedLoop->initializePending());
    CHECK(delayed->reserveReliableSlot(endpoint,delayedCircuit.incarnation,22,90)==State::Status::Ok);
    CHECK(delayed->acceptReliableSlot(90)==State::Status::Ok);
    uint32_t delayedId=0; CHECK(delayed->allocateReliablePacketId(90,delayedId)==State::Status::Ok);
    const uint8_t delayedBytes[]={0x40,0,0,0,1,0,42,1};
    CHECK(delayed->prepareReliable(90,delayedId,delayedBytes,sizeof(delayedBytes),{2,1,10,false},0)==State::Status::Ok);
    CHECK(delayedLoop->sendStateActionOnce(delayedRing,0,proxy,LLTransportSteadyUsec(0),0)==LLTransportOwnerLoop::Step::Progress);
    delayedRing.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(delayedLoop->sendStateActionOnce(delayedRing,0,proxy,LLTransportSteadyUsec(2),0)==LLTransportOwnerLoop::Step::Blocked);
    delayedRing.sendResult={NetSendStatus::Sent,0};
    CHECK(delayedLoop->sendStateActionOnce(delayedRing,0,proxy,LLTransportSteadyUsec(20),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(delayed->advance(20,0).kind==State::ActionKind::Resend); // original deadline 12 is already past

    // Ping-based interval is sampled from this action's circuit at the first
    // resend attempt, then survives a later RTT change during WouldBlock.
    auto pingState=std::make_unique<State>(); uint64_t pingCharge=0;
    CHECK(pingState->init({1,1,1,1,1,1,8388608},pingCharge));
    const auto pingCircuit=pingState->enable(endpoint,23,true,100,{1000000,60000000,0.f,0.5f});
    CHECK(pingCircuit.status==State::Status::Ok);
    CHECK(pingState->notePingReply(endpoint,pingCircuit.incarnation,0,1000000,100100)==State::Status::Ok);
    auto pingChannels=std::make_unique<LLTransportOwnerChannels>();
    auto pingEvents=std::make_unique<LLTransportIngressQueue>();
    auto pingArena=std::make_unique<LLTransportDecodedArena>(); CHECK(pingArena->initialize(1024,512));
    LLPacketRing pingRing; std::atomic<bool> pingPoison{false};
    auto pingPump=std::make_unique<LLTransportReceivePump>(pingRing,*pingState,*pingArena,*pingEvents,pingPoison,nullptr,0);
    auto pingLoop=std::make_unique<LLTransportOwnerLoop>(*pingState,*pingPump,*pingChannels,pingPoison);
    CHECK(pingLoop->initializePending());
    CHECK(pingState->reserveReliableSlot(endpoint,pingCircuit.incarnation,23,91)==State::Status::Ok);
    CHECK(pingState->acceptReliableSlot(91)==State::Status::Ok);
    uint32_t pingId=0; CHECK(pingState->allocateReliablePacketId(91,pingId)==State::Status::Ok);
    CHECK(pingState->prepareReliable(91,pingId,delayedBytes,sizeof(delayedBytes),{2,1,17,false,true},2000000)==State::Status::Ok);
    CHECK(pingLoop->sendStateActionOnce(pingRing,0,proxy,LLTransportSteadyUsec(2000000),0)==LLTransportOwnerLoop::Step::Progress);
    pingRing.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(pingLoop->sendStateActionOnce(pingRing,0,proxy,LLTransportSteadyUsec(2000002),0)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(pingState->notePingReply(endpoint,pingCircuit.incarnation,0,1000000,3000000)==State::Status::Ok);
    pingRing.sendResult={NetSendStatus::Sent,0};
    CHECK(pingLoop->sendStateActionOnce(pingRing,0,proxy,LLTransportSteadyUsec(3000001),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(pingState->advance(6200000,0).kind==State::ActionKind::Resend);

    // The scheduler exposes every circuit changed by an action-less timeout
    // scan; directory publication itself is a separate integration step.
    auto scanState=std::make_unique<State>(); uint64_t scanCharge=0;
    CHECK(scanState->init({2,2,2,2,2,2,8388608},scanCharge));
    const LLTransportEndpoint secondEndpoint{0x0200007f,5678};
    const auto scanA=scanState->enable(endpoint,24,true,0,{1000000,60000000,0.f,0.5f});
    const auto scanB=scanState->enable(secondEndpoint,24,true,0,{1000000,60000000,0.f,0.5f});
    CHECK(scanA.status==State::Status::Ok && scanB.status==State::Status::Ok);
    auto scanChannels=std::make_unique<LLTransportOwnerChannels>();
    auto scanEvents=std::make_unique<LLTransportIngressQueue>();
    auto scanArena=std::make_unique<LLTransportDecodedArena>(); CHECK(scanArena->initialize(1024,512));
    LLPacketRing scanRing; std::atomic<bool> scanPoison{false};
    auto scanPump=std::make_unique<LLTransportReceivePump>(scanRing,*scanState,*scanArena,*scanEvents,scanPoison,nullptr,0);
    auto scanLoop=std::make_unique<LLTransportOwnerLoop>(*scanState,*scanPump,*scanChannels,scanPoison);
    CHECK(scanLoop->initializePending());
    auto scanPrepare=[&](LLTransportEndpoint host,uint64_t incarnation,uint64_t token) {
        CHECK(scanState->reserveReliableSlot(host,incarnation,24,token)==State::Status::Ok);
        CHECK(scanState->acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(scanState->allocateReliablePacketId(token,id)==State::Status::Ok);
        CHECK(scanState->prepareReliable(token,id,delayedBytes,sizeof(delayedBytes),{0,1,1,false},0)==State::Status::Ok);
    };
    scanPrepare(endpoint,scanA.incarnation,201); scanPrepare(secondEndpoint,scanB.incarnation,202);
    for (unsigned i=0;i<2;++i)
        CHECK(scanLoop->sendStateActionOnce(scanRing,0,proxy,LLTransportSteadyUsec(0),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(scanLoop->sendStateActionOnce(scanRing,0,proxy,LLTransportSteadyUsec(2),0)==LLTransportOwnerLoop::Step::Progress);
    const auto& dirty=scanLoop->lastAdvanceDirty();
    CHECK(!dirty.overflow && dirty.count==2 && !scanLoop->lastActionAffected().valid);
    CHECK((dirty.values[0].incarnation==scanA.incarnation && dirty.values[1].incarnation==scanB.incarnation) ||
          (dirty.values[1].incarnation==scanA.incarnation && dirty.values[0].incarnation==scanB.incarnation));
}
