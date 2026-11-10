// Actual owner turns with a competing circuit publication at sequence exhaustion.
// Fake packet boundary only; not a live worker or network-performance test.
#include "lltransportownerloop.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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
    auto state=std::make_unique<State>(); uint64_t charge=0;
    CHECK(state->init({2,2,16,16,4,16,8388608},charge));
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(65536,512));
    LLMessageVariable byte{MVT_FIXED,1};
    LLMessageBlock appBlock,ackBlock; appBlock.mMemberVariables={&byte};
    ackBlock.mMemberVariables={&byte};
    LLMessageTemplate app,ack; app.mMessageNumber=42; app.mMemberBlocks={&appBlock};
    ack.mMessageNumber=2; ack.mMemberBlocks={&ackBlock};
    LLTransportStartupBudget budget; CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema appSchema,ackSchema;
    CHECK(freezeTransportSchema(app,8,budget,appSchema)==LLTransportFreezeStatus::Complete);
    CHECK(freezeTransportSchema(ack,8,budget,ackSchema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry schemas[]={{2,&ackSchema,false,true},{42,&appSchema,false,false}};
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,schemas,2);
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>(); CHECK(directory->activateOnOwner());
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    CHECK(loop->initializePending() && loop->initializeWatchdogs(1));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    const LLTransportEndpoint other{0x0200007f,1235};
    const auto otherEnabled=state->enable(other,7,true,1,{1000000,60000000,0.f,0.5f});
    CHECK(otherEnabled.status==State::Status::Ok);
    LLTransportOwnerCommand command; command.kind=LLTransportOwnerCommand::Kind::Enable;
    command.circuit.kind=LLTransportCircuitCommandKind::Enable; command.circuit.endpoint=endpoint;
    command.circuit.loginGeneration=7; command.circuit.operationId=1; command.circuit.trusted=true;
    auto offer=[&](const LLTransportOwnerCommand& value) { CHECK(channels->commands.beginOfferOnMain());
        auto* slot=channels->commands.reserveProducer(); CHECK(slot); *slot=value;
        CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain(); };
    const State::WatchdogConfig watch{1000000,60000000,0.f,0.5f};
    auto turn=[&](uint64_t now) { return loop->driveOnce(*directory,ring,0,proxy,LLTransportSteadyUsec(now),7,schemas,2,watch,false); };
    offer(command); CHECK(turn(1)==LLTransportOwnerLoop::Step::Progress);
    const auto* enabled=channels->circuitTerminals.peekConsumer(); CHECK(enabled);
    const auto incarnation=enabled->circuitIncarnation; CHECK(channels->circuitTerminals.releaseConsumer());
    LLTransportOwnerCommand send; send.kind=LLTransportOwnerCommand::Kind::Send; send.reliable=true;
    send.sendToken=77; send.schemaGeneration=8; send.circuit.endpoint=endpoint;
    send.circuit.loginGeneration=7; send.circuit.targetIncarnation=incarnation;
    send.retry={1,10000000,10000000,false}; send.byteCount=8; send.bytes[6]=42; send.bytes[7]=1;
    offer(send); (void)turn(2); CHECK(!ring.sentPackets.empty());
    CHECK(state->testSetSequence(UINT64_MAX-3));
    // Poll a reliable CompletePing: preflight and observed arrival succeed.
    ring.packet={0x40,0,0,0,7,0,2,1};
    loop->notifySocketReadable(); (void)turn(3); CHECK(!poison.load());
    // Another circuit performs a real publication before this held input's
    // next owner turn, consuming the previously available headroom.
    CHECK(state->notePingReply(other,otherEnabled.incarnation,0,1000000,3)==State::Status::Ok);
    CHECK(turn(4)==LLTransportOwnerLoop::Step::Poisoned && poison.load());
    CHECK(state->classifyResentDuplicate(endpoint,incarnation,7,7,true)==State::DuplicateClass::Busy);
    CHECK(state->advance(4,0).kind!=State::ActionKind::Ack);
    loop->cancelInFlightForStop(); loop->cancelInFlightForStop();
    CHECK(state->lookup(endpoint,5).state==LLTransportSnapshotState::Absent);
    CHECK(state->advance(5,0).kind==State::ActionKind::None);
    CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Progress);
    const auto* done=channels->sendTerminals.peekConsumer();
    CHECK(done && done->operationId==77 && done->originalStatus!=0);
    CHECK(channels->sendTerminals.releaseConsumer());
    CHECK(loop->publishReliableCompletion()==LLTransportOwnerLoop::Step::Idle);
}
