// Actual pump+state EmptyMessage handling; fake socket/template source only.
#include "lltransportownerloop.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>
#include <vector>
using U32=uint32_t;
constexpr int MBT_SINGLE=1,MBT_MULTIPLE=2,MBT_VARIABLE=3,MVT_VARIABLE=1,MVT_FIXED=2;
struct LLMessageVariable { int type,size; int getType() const { return type; } int getSize() const { return size; } };
struct LLMessageBlock { int mType=MBT_VARIABLE,mNumber=1; std::vector<LLMessageVariable*> mMemberVariables; };
struct LLMessageTemplate { std::vector<LLMessageBlock*> mMemberBlocks; U32 mMessageNumber=0xfffffffbu; };
#include "transport_schema_freeze.inc"
static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({1,2,16,16,2,8,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const auto enabled=state->enable(endpoint,7,true,100,{1000000,60000000,0.f,0.5f});
    CHECK(enabled.status==State::Status::Ok);
    LLMessageVariable id{MVT_FIXED,4}; LLMessageBlock block; block.mMemberVariables={&id};
    LLMessageTemplate source; source.mMemberBlocks={&block};
    LLTransportStartupBudget budget;
    CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema schema; CHECK(freezeTransportSchema(source,8,budget,schema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry entries[]={{0xfffffffbu,&schema,false,true}};
    auto channels=std::make_unique<LLTransportOwnerChannels>(); auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,entries,1);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    auto packet=[&](uint8_t flags,uint8_t idValue,uint8_t count) {
        ring.packet={flags,0,0,0,idValue,0,0xff,0xff,0xff,0xfb,count};
    };
    packet(0,1,0);
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::ControlPending);
    CHECK(!loop->needsReceiveTurn(LLTransportSteadyUsec(101)));
    CHECK(loop->discardEmptyUnreliableControl(false)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->needsReceiveTurn(LLTransportSteadyUsec(101)));
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(101))==LLTransportRxStatus::Idle);
    packet(0,2,1); ring.packet.insert(ring.packet.end(),{42,0,0,0});
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(102))==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardEmptyUnreliableControl(true)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop->servicePendingControl(LLTransportSteadyUsec(102),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    packet(0,3,1); // valid block count, truncated U32 requires diagnostics, not empty-discard
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(103))==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardEmptyUnreliableControl(true)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop->servicePendingControl(LLTransportSteadyUsec(103),1000000,false)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop->servicePendingControl(LLTransportSteadyUsec(103),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    // A partial appended ACK is not an empty-control discard, even if the
    // message body itself has zero variable blocks.
    CHECK(state->reserveReliableSlot(endpoint,enabled.incarnation,7,10)==State::Status::Ok);
    CHECK(state->acceptReliableSlot(10)==State::Status::Ok);
    uint32_t idOut=0; CHECK(state->allocateReliablePacketId(10,idOut)==State::Status::Ok);
    const uint8_t outgoing[]={0x40,0,0,0,1,0,42};
    CHECK(state->prepareReliable(10,idOut,outgoing,sizeof(outgoing),{1,10000000,10000000,false},104)==State::Status::Ok);
    const auto action=state->advance(104,1000000); CHECK(action.kind==State::ActionKind::InitialSend);
    packet(0x10,4,0);
    ring.packet.insert(ring.packet.end(),{uint8_t(idOut>>24),uint8_t(idOut>>16),uint8_t(idOut>>8),uint8_t(idOut),1});
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(104))==LLTransportRxStatus::ControlPending);
    CHECK(pump->pendingKind()==LLTransportPendingKind::BusyAppendedAck);
    CHECK(loop->discardEmptyUnreliableControl(true)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(state->defer(action.token)==State::Status::Ok);
    CHECK(loop->resumeBusyAcknowledgements(7)==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardEmptyUnreliableControl(true)==LLTransportOwnerLoop::Step::Progress);
    packet(0x40,5,0);
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(105))==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardEmptyUnreliableControl(true)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(!pump->releaseControl()); // reliable uncommitted input remains held
}
