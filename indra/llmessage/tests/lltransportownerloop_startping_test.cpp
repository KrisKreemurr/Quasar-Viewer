// Actual pump/state/control-response slice; fake socket and template source.
#include "lltransportownerloop.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>
#include <vector>
using U32=uint32_t;
constexpr int MBT_SINGLE=1,MBT_MULTIPLE=2,MBT_VARIABLE=3,MVT_VARIABLE=1,MVT_FIXED=2;
struct LLMessageVariable { int type,size; int getType() const { return type; } int getSize() const { return size; } };
struct LLMessageBlock { int mType=MBT_SINGLE,mNumber=1; std::vector<LLMessageVariable*> mMemberVariables; };
struct LLMessageTemplate { std::vector<LLMessageBlock*> mMemberBlocks; U32 mMessageNumber=1; };
#include "transport_schema_freeze.inc"
static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({2,2,16,16,2,8,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const auto enabled=state->enable(endpoint,7,true,100,{1000000,60000000,0.f,0.5f});
    CHECK(enabled.status==State::Status::Ok);
    LLMessageVariable id{MVT_FIXED,1},oldest{MVT_FIXED,4}; LLMessageBlock block;
    block.mMemberVariables={&id,&oldest}; LLMessageTemplate source; source.mMemberBlocks={&block};
    LLTransportStartupBudget budget;
    CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema schema; CHECK(freezeTransportSchema(source,8,budget,schema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry entries[]={{1,&schema,false,true}};
    auto channels=std::make_unique<LLTransportOwnerChannels>(); auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,entries,1);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison); CHECK(loop->initializePending());
    const auto ack=state->reserveReceive(endpoint,99,false,true,101);
    CHECK(ack.status==State::ReceiveStatus::New && state->commitReceive(ack.reservation,8,101)==State::Status::Ok);
    ring.packet={0,0,0,0,100,0,1,7,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(102))==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardStaleHeldControl(7,LLTransportSteadyUsec(102))==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop->discardStaleHeldControl(0,LLTransportSteadyUsec(102))==LLTransportOwnerLoop::Step::Blocked);
    CHECK(loop->prepareStartPingResponse(LLTransportSteadyUsec(102),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(102))==LLTransportRxStatus::Idle);
    // Legacy accepts the full U32 OldestUnacked. A second circuit's
    // 0xffffffff input must produce a reply and leave RX available.
    const LLTransportEndpoint other{0x0200007f,5678};
    const auto b=state->enable(other,7,true,102,{1000000,60000000,0.f,0.5f});
    CHECK(b.status==State::Status::Ok);
    ring.sender=LLHost(other.addressNetworkOrder,other.portHostOrder);
    ring.packet={0,0,0,0,1,0,1,55,0xff,0xff,0xff,0xff};
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(102))==LLTransportRxStatus::ControlPending);
    CHECK(loop->prepareStartPingResponse(LLTransportSteadyUsec(102),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    CHECK(state->lookup(other,102).packetsOut==1);
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(102))==LLTransportRxStatus::Idle);
    ring.sender=LLHost(endpoint.addressNetworkOrder,endpoint.portHostOrder);
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(103))==LLTransportOwnerLoop::Step::Blocked);
    CHECK(ring.sentPackets.size()==1 && ring.sentPackets[0].size()==13);
    const auto first=ring.sentPackets[0];
    CHECK((first[0]&0x10)!=0 && first[6]==2 && first[7]==7 && first[11]==99 && first[12]==1);
    const auto newer=state->reserveReceive(endpoint,101,false,true,104);
    CHECK(newer.status==State::ReceiveStatus::New && state->commitReceive(newer.reservation,8,104)==State::Status::Ok);
    ring.sendResult={NetSendStatus::Sent,0};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(105),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==2 && ring.sentPackets[1][6]==0xff);
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(106))==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==3 && ring.sentPackets[2]==first && !ring.firstAttempts[2]);
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(106))==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==4 && ring.sentPackets[3][6]==2 && ring.sentPackets[3][7]==55);
    CHECK(state->disable(other,b.incarnation,0).status==LLTransportCircuitCommandStatus::Applied);
    ring.packet={0,0,0,0,102,0,1,8,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(107))==LLTransportRxStatus::ControlPending);
    CHECK(loop->prepareStartPingResponse(LLTransportSteadyUsec(107),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(108))==LLTransportOwnerLoop::Step::Blocked);
    LLTransportOwnerCommand close; close.kind=LLTransportOwnerCommand::Kind::Disable;
    close.circuit.kind=LLTransportCircuitCommandKind::Disable; close.circuit.endpoint=endpoint;
    close.circuit.loginGeneration=7; close.circuit.targetIncarnation=enabled.incarnation;
    CHECK(channels->commands.beginOfferOnMain()); auto* cmd=channels->commands.reserveProducer(); CHECK(cmd); *cmd=close; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(109),{1000000,60000000,0.f,0.5f})==LLTransportOwnerLoop::Step::Progress);
    const auto count=ring.sentPackets.size();
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(110))==LLTransportOwnerLoop::Step::Idle);
    CHECK(ring.sentPackets.size()==count);
    const auto reopened=state->enable(endpoint,8,true,111,{1000000,60000000,0.f,0.5f});
    CHECK(reopened.status==State::Status::Ok && reopened.incarnation!=enabled.incarnation);
    ring.packet={0,0,0,0,1,0,1,9,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(112))==LLTransportRxStatus::ControlPending);
    CHECK(loop->prepareStartPingResponse(LLTransportSteadyUsec(112),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    ring.sendResult={NetSendStatus::Sent,0};
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(113))==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.back().size()==8 && ring.sentPackets.back()[4]==first[4] && ring.sentPackets.back()[7]==9);
    // The prototype never ACKs a held reliable control. Keep it while live,
    // then discard the uncommitted old incarnation after close/reopen.
    ring.packet={0x40,0,0,0,2,0,1,10,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(114))==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardStaleHeldControl(8,LLTransportSteadyUsec(114))==LLTransportOwnerLoop::Step::Blocked);
    CHECK(!pump->releaseControl());
    close.circuit.loginGeneration=8; close.circuit.targetIncarnation=reopened.incarnation;
    CHECK(channels->commands.beginOfferOnMain()); cmd=channels->commands.reserveProducer(); CHECK(cmd); *cmd=close; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(115),{1000000,60000000,0.f,0.5f})==LLTransportOwnerLoop::Step::Progress);
    const auto again=state->enable(endpoint,8,true,116,{1000000,60000000,0.f,0.5f});
    CHECK(again.status==State::Status::Ok && again.incarnation!=reopened.incarnation);
    CHECK(loop->discardStaleHeldControl(8,LLTransportSteadyUsec(116))==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(116))==LLTransportRxStatus::Idle);
    CHECK(state->advance(116,0).kind==State::ActionKind::None); // no old reliable ACK
    ring.packet={0,0,0,0,1,0,1,11,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(117))==LLTransportRxStatus::ControlPending);
    CHECK(loop->discardStaleHeldControl(9,LLTransportSteadyUsec(117))==LLTransportOwnerLoop::Step::Progress); // changed login
    // Reliable StartPing reserves and commits its inbound ACK only after the
    // reply is retained; the committed duplicate is visible before send.
    ring.packet={0x40,0,0,0,200,0,1,13,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(117))==LLTransportRxStatus::ControlPending);
    CHECK(loop->prepareStartPingResponse(LLTransportSteadyUsec(117),1000000,true)==LLTransportOwnerLoop::Step::Progress);
    CHECK(state->classifyResentDuplicate(endpoint,again.incarnation,8,200,true)==State::DuplicateClass::Duplicate);
    CHECK(loop->sendControlResponseOnce(ring,0,proxy,LLTransportSteadyUsec(117))==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.back()[6]==2 && ring.sentPackets.back()[7]==13);
    ring.packet={0,0,0,0,2,0,1,12,0,0,0,0};
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(118))==LLTransportRxStatus::ControlPending);
    State::TimeoutEvent dead;
    CHECK(state->beginWatchdogTimeout(endpoint,again.incarnation,60000117,60000000,true,0,dead)==State::Status::Ok);
    CHECK(loop->discardStaleHeldControl(8,LLTransportSteadyUsec(60000117))==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->pollReceive(0,proxy,8,LLTransportSteadyUsec(60000117))==LLTransportRxStatus::Idle);
}
