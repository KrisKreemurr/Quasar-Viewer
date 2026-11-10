/** Source-only owner state + actual packed arena pressure. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportcircuitstate.h"
#include "../lltransportingress.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
static void check(bool value, int line) { if (!value) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State = LLTransportCircuitState;
    State state; uint64_t charged = 0;
    CHECK(state.init({2,2,64,128,2,32,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const auto enabled = state.enable(endpoint,7,true,100,{1000000,60000000,0.f,0.5f});
    CHECK(enabled.status == State::Status::Ok);
    auto scratch = std::make_unique<LLTransportIngressScratch>();
    auto queue = std::make_unique<LLTransportIngressQueue>();
    auto arena = std::make_unique<LLTransportDecodedArena>();
    CHECK(arena->initialize(1024,512));
    const uint8_t byte=42;
    CHECK(scratch->appendBlock(0,0) && scratch->appendField(0,&byte,1));
    std::atomic<bool> poison{false};
    LLTransportIngressEvent event;
    event.sender=endpoint; event.circuitIncarnation=enabled.incarnation; event.schemaGeneration=8; event.wireBytes=12;
    uint32_t packet = 1;
    for (; packet < 60; ++packet)
    {
        const auto arrival = state.observeValidatedArrival(endpoint,enabled.incarnation,7,packet,false,12,100+packet);
        CHECK(arrival.status == State::ArrivalStatus::Observed && arrival.observation);
        const auto recv = state.reserveObservedReceive(endpoint,enabled.incarnation,7,packet,false,true,100+packet,arrival.observation);
        CHECK(recv.status == State::ReceiveStatus::New);
        event.appSequence=packet; event.packetId=packet;
        const auto status = llTransportPublishDecoded(state,recv.reservation,LLTransportSteadyUsec(100+packet),8,poison,*scratch,*arena,*queue,event);
        if (status == LLTransportPublishStatus::Backpressure) break;
        CHECK(status == LLTransportPublishStatus::Published);
    }
    CHECK(packet > 1 && packet < 60 && !poison.load());
    auto snap = state.lookup(endpoint,200);
    CHECK(snap.packetsIn == packet && snap.lastPacketMonotonicUsec == 100 + packet);
    auto pending = state.advance(201,0);
    CHECK(pending.kind == State::ActionKind::Ack && pending.ackCount == packet-1);
    for (uint32_t i=0; i<pending.ackCount; ++i) CHECK(pending.ackIds[i] != packet);
    uint32_t ackId=0;
    CHECK(state.assignActionPacketId(pending.token,ackId) == State::Status::Ok);
    CHECK(state.commitAttempt(pending.token,201,0,true,16) == State::Status::Ok);
    const auto* oldest=queue->peekConsumer(); CHECK(oldest);
    const auto record=oldest->arenaRecord;
    CHECK(arena->release(record) && queue->releaseConsumer());
    const auto resent=state.observeValidatedArrival(endpoint,enabled.incarnation,7,packet,true,12,202);
    CHECK(resent.status == State::ArrivalStatus::Observed && resent.observation);
    const auto recv=state.reserveObservedReceive(endpoint,enabled.incarnation,7,packet,true,true,202,resent.observation);
    CHECK(recv.status == State::ReceiveStatus::New);
    event.appSequence=packet; event.packetId=packet;
    CHECK(llTransportPublishDecoded(state,recv.reservation,LLTransportSteadyUsec(202),8,poison,*scratch,*arena,*queue,event) == LLTransportPublishStatus::Published);
    const auto dup=state.observeValidatedArrival(endpoint,enabled.incarnation,7,packet,true,12,203);
    CHECK(dup.status == State::ArrivalStatus::Duplicate);
    CHECK(state.acknowledgeCommittedDuplicate(endpoint,enabled.incarnation,7,packet,203) == State::ReceiveStatus::Duplicate);
    snap=state.lookup(endpoint,204);
    CHECK(snap.packetsIn == packet+1 && snap.lastPacketMonotonicUsec == 202);
}
