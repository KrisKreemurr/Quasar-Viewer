// Actual owner/state send path with a fake socket; no viewer or UDP runtime.
#include "lltransportownerloop.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <vector>
using U32=uint32_t;
constexpr int MBT_SINGLE=1,MBT_MULTIPLE=2,MBT_VARIABLE=3,MVT_VARIABLE=1,MVT_FIXED=2;
struct LLMessageVariable { int type,size; int getType() const { return type; } int getSize() const { return size; } };
struct LLMessageBlock { int mType=MBT_SINGLE,mNumber=1; std::vector<LLMessageVariable*> mMemberVariables; };
struct LLMessageTemplate { std::vector<LLMessageBlock*> mMemberBlocks; U32 mMessageNumber=0; };
#include "transport_schema_freeze.inc"
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({1,2,8,8,2,8,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const auto circuit=state->enable(endpoint,7,true,1,{1000000,60000000,0.f,0.5f});
    CHECK(circuit.status==State::Status::Ok);
    LLMessageVariable byte{MVT_FIXED,1}; LLMessageBlock block; block.mMemberVariables={&byte};
    LLMessageTemplate app; app.mMessageNumber=42; app.mMemberBlocks={&block};
    LLTransportStartupBudget budget; CHECK((LLTransportStartupBudget::initialize<16384,512,2048,1>(budget,1024,1000000,1000000)));
    LLTransportSchema schema; CHECK(freezeTransportSchema(app,8,budget,schema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry schemas[]={{42,&schema,false,false}};
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,schemas,1);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison); CHECK(loop->initializePending());
    const auto received=state->reserveReceive(endpoint,9,false,true,2);
    CHECK(received.status==State::ReceiveStatus::New && state->commitReceive(received.reservation,8,2)==State::Status::Ok);
    LLTransportOwnerCommand send; send.kind=LLTransportOwnerCommand::Kind::Send; send.reliable=false;
    send.sendToken=50; send.schemaGeneration=8; send.circuit.endpoint=endpoint;
    send.circuit.loginGeneration=7; send.circuit.targetIncarnation=circuit.incarnation;
    const uint8_t body[]={0,0,0,0,0,0,42,1}; send.byteCount=sizeof(body);
    std::copy(std::begin(body),std::end(body),send.bytes.begin());
    CHECK(channels->commands.beginOfferOnMain()); auto* slot=channels->commands.reserveProducer(); CHECK(slot); *slot=send; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    LLTransportOwnerCommand setting; setting.kind=LLTransportOwnerCommand::Kind::SetAckCollectTime;
    setting.configOperationId=51; setting.ackCollectUsec=100;
    CHECK(channels->commands.beginOfferOnMain()); slot=channels->commands.reserveProducer(); CHECK(slot); *slot=setting; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processUnreliableSendHead(LLTransportSteadyUsec(3),7,schemas,1)==LLTransportOwnerLoop::Step::Progress);
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendUnreliableOnce(ring,0,proxy,LLTransportSteadyUsec(4))==LLTransportOwnerLoop::Step::Blocked);
    CHECK(ring.sentPackets.size()==1 && (ring.sentPackets[0][0]&0x10));
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(5),{1000000,60000000,0.f,0.5f})==LLTransportOwnerLoop::Step::Progress);
    const auto* config=channels->circuitTerminals.peekConsumer(); CHECK(config && config->operationId==51);
    CHECK(channels->circuitTerminals.releaseConsumer());
    send.sendToken=52;
    CHECK(channels->commands.beginOfferOnMain()); slot=channels->commands.reserveProducer(); CHECK(slot); *slot=send;
    CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processUnreliableSendHead(LLTransportSteadyUsec(5),7,schemas,1)==LLTransportOwnerLoop::Step::Blocked);
    ring.sendResult={NetSendStatus::Sent,0};
    CHECK(loop->sendUnreliableOnce(ring,0,proxy,LLTransportSteadyUsec(6))==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==2 && ring.sentPackets[0]==ring.sentPackets[1] && ring.firstAttempts[0] && !ring.firstAttempts[1]);
    const auto* done=channels->unreliableTerminals.peekConsumer();
    CHECK(done && done->operationId==50 && done->status==LLTransportUnreliableStatus::Sent);
    CHECK(channels->unreliableTerminals.releaseConsumer());
    CHECK(loop->processUnreliableSendHead(LLTransportSteadyUsec(7),7,schemas,1)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->cancelOneUnreliableForStop()==LLTransportOwnerLoop::Step::Progress);
    done=channels->unreliableTerminals.peekConsumer();
    CHECK(done && done->operationId==52 && done->status==LLTransportUnreliableStatus::Cancelled);
    CHECK(channels->unreliableTerminals.releaseConsumer() && !poison.load());
    // A blocked reliable first attempt orders a later unreliable send, but an
    // ACK is not required before the later first attempt becomes admissible.
    send.reliable=true; send.sendToken=53; send.retry={1,1000000,1000000,false};
    CHECK(channels->commands.beginOfferOnMain()); slot=channels->commands.reserveProducer(); CHECK(slot); *slot=send;
    CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processReliableSendHead(LLTransportSteadyUsec(8),7,schemas,1)==LLTransportOwnerLoop::Step::Progress);
    ring.sendResult={NetSendStatus::WouldBlock,EAGAIN};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(9),1000000)==LLTransportOwnerLoop::Step::Blocked);
    send.reliable=false; send.sendToken=54;
    CHECK(channels->commands.beginOfferOnMain()); slot=channels->commands.reserveProducer(); CHECK(slot); *slot=send;
    CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processUnreliableSendHead(LLTransportSteadyUsec(10),7,schemas,1)==LLTransportOwnerLoop::Step::Blocked);
    ring.sendResult={NetSendStatus::Sent,0};
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(11),1000000)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->processUnreliableSendHead(LLTransportSteadyUsec(12),7,schemas,1)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->cancelOneUnreliableForStop()==LLTransportOwnerLoop::Step::Progress);
}
