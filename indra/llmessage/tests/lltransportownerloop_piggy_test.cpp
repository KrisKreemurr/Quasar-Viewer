// Actual state/scheduler piggyback cancellation; fake socket throws before send.
#include "lltransportownerloop.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({1,2,4,4,2,4,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const auto circuit=state->enable(endpoint,7,true,0,{1000000,60000000,0.f,0.5f});
    CHECK(circuit.status==State::Status::Ok);
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,nullptr,0);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    CHECK(loop->initializePending());
    CHECK(state->reserveReliableSlot(endpoint,circuit.incarnation,7,11)==State::Status::Ok);
    CHECK(state->acceptReliableSlot(11)==State::Status::Ok);
    uint32_t id=0; CHECK(state->allocateReliablePacketId(11,id)==State::Status::Ok);
    const uint8_t bytes[]={0x40,0,0,0,1,0,42,1};
    CHECK(state->prepareReliable(11,id,bytes,sizeof(bytes),{1,1000000,1000000,false},0)==State::Status::Ok);
    const auto ack=state->reserveReceive(endpoint,9,false,true,1);
    CHECK(ack.status==State::ReceiveStatus::New && state->commitReceive(ack.reservation,8,1)==State::Status::Ok);
    ring.throwBeforeAttempt=true;
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(2),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.empty()); // no fake socket call completed
    ring.throwBeforeAttempt=false;
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(3),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==1 && ring.sentPackets[0][6]==0xff && ring.sentPackets[0][10]==1);
    CHECK(ring.sentPackets[0][11]==9); // template U32 little-endian ID
}
