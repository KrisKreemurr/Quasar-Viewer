// Actual owner iteration/state/pump/packed queue; fake socket/template input.
// Manually stepped: not a real worker, viewer or 1 Hz acceptance test.
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
struct LLMessageTemplate { std::vector<LLMessageBlock*> mMemberBlocks; U32 mMessageNumber=0; };
#include "transport_schema_freeze.inc"
static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
static std::vector<uint8_t> packet(uint8_t flags,uint32_t id,uint8_t number,uint8_t value)
{ return {flags,uint8_t(id>>24),uint8_t(id>>16),uint8_t(id>>8),uint8_t(id),0,number,value}; }
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({1,2,8192,8192,2,32,8388608},charged));
    auto channels=std::make_unique<LLTransportOwnerChannels>(); auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1048576,512));
    LLMessageVariable byte{MVT_FIXED,1}; LLMessageBlock block; block.mMemberVariables={&byte};
    LLMessageTemplate app,ping; app.mMessageNumber=42; ping.mMessageNumber=2;
    app.mMemberBlocks={&block}; ping.mMemberBlocks={&block}; LLTransportStartupBudget budget;
    CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema appSchema,pingSchema;
    CHECK(freezeTransportSchema(app,8,budget,appSchema)==LLTransportFreezeStatus::Complete);
    CHECK(freezeTransportSchema(ping,8,budget,pingSchema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry schemas[]={{2,&pingSchema,false,true},{42,&appSchema,false,false}};
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,schemas,2);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>();
    CHECK(directory->activateOnOwner());
    CHECK(loop->initializePending() && loop->initializeWatchdogs(1));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    LLTransportOwnerCommand enable; enable.kind=LLTransportOwnerCommand::Kind::Enable;
    enable.circuit.kind=LLTransportCircuitCommandKind::Enable; enable.circuit.endpoint=endpoint;
    enable.circuit.loginGeneration=7; enable.circuit.trusted=true;
    CHECK(channels->commands.beginOfferOnMain()); auto* command=channels->commands.reserveProducer(); CHECK(command); *command=enable; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    const State::WatchdogConfig watch{1000000,60000000,0.f,0.5f};
    CHECK(loop->initializeAckCollectTime(2000000));
    auto tick=[&](uint64_t now) { return loop->driveOnce(*directory,ring,0,proxy,LLTransportSteadyUsec(now),7,schemas,2,watch,true); };
    CHECK(tick(100)==LLTransportOwnerLoop::Step::Progress);
    LLTransportCircuitSnapshot firstPublished;
    CHECK(directory->get(endpoint,firstPublished) && firstPublished.lossQuality==LLTransportLossQuality::Complete);
    const auto* result=channels->circuitTerminals.peekConsumer(); CHECK(result && result->status==LLTransportCircuitCommandStatus::Applied);
    const auto incarnation=result->circuitIncarnation; CHECK(channels->circuitTerminals.releaseConsumer());
    // Fill the actual queue with actual decoded/packed, committed APP events.
    for (uint32_t id=1;id<=4096;++id)
    {
        ring.packet=packet(0x40,id,42,uint8_t(id));
        CHECK(loop->pollReceive(0,proxy,7,LLTransportSteadyUsec(100+id))==LLTransportRxStatus::Published);
    }
    CHECK(!events->reserveProducer());
    ring.packet=packet(0x40,4097,42,7);
    loop->notifySocketReadable();
    CHECK(tick(1000101)==LLTransportOwnerLoop::Step::Progress);
    CHECK(state->classifyResentDuplicate(endpoint,incarnation,7,4097,true)==State::DuplicateClass::NotDuplicate);
    CHECK(!ring.sentPackets.empty() && ring.sentPackets[0][6]==1); // actual watchdog ping action
    const auto before=state->lookup(endpoint,1000101);
    ring.packet=packet(0x60,1,99,0); // committed duplicate before unknown schema lookup
    CHECK(tick(1000102)==LLTransportOwnerLoop::Step::Progress);
    CHECK(state->lookup(endpoint,1000102).packetsIn==before.packetsIn);
    ring.packet=packet(0,4098,2,0);
    CHECK(tick(1000103)==LLTransportOwnerLoop::Step::Progress); // control retained
    CHECK(tick(1000104)==LLTransportOwnerLoop::Step::Progress); // control applied/released
    CHECK(state->lookup(endpoint,1000104).lastPacketMonotonicUsec==1000103);
    CHECK(!events->reserveProducer() && !poison.load());
    // An actual turn republishes loss bookkeeping exhaustion as Partial.
    auto lossState=std::make_unique<State>(); uint64_t lossCharge=0;
    CHECK(lossState->init({1,2,8,8,2,1,8388608},lossCharge));
    auto lossChannels=std::make_unique<LLTransportOwnerChannels>();
    auto lossEvents=std::make_unique<LLTransportIngressQueue>();
    auto lossArena=std::make_unique<LLTransportDecodedArena>(); CHECK(lossArena->initialize(65536,512));
    LLPacketRing lossRing; std::atomic<bool> lossPoison{false};
    auto lossPump=std::make_unique<LLTransportReceivePump>(lossRing,*lossState,*lossArena,*lossEvents,lossPoison,schemas,2);
    auto lossLoop=std::make_unique<LLTransportOwnerLoop>(*lossState,*lossPump,*lossChannels,lossPoison);
    auto lossDirectory=std::make_unique<LLTransportOwnerLoop::Directory>();
    CHECK(lossDirectory->activateOnOwner() && lossLoop->initializePending() && lossLoop->initializeWatchdogs(1));
    CHECK(lossChannels->commands.beginOfferOnMain()); auto* lossCommand=lossChannels->commands.reserveProducer(); CHECK(lossCommand); *lossCommand=enable;
    CHECK(lossChannels->commands.publishProducer()); lossChannels->commands.endOfferOnMain();
    auto lossTick=[&](uint64_t now) { return lossLoop->driveOnce(*lossDirectory,lossRing,0,proxy,LLTransportSteadyUsec(now),7,schemas,2,watch,true); };
    CHECK(lossTick(1)==LLTransportOwnerLoop::Step::Progress);
    LLTransportCircuitSnapshot lossSnapshot;
    CHECK(lossDirectory->get(endpoint,lossSnapshot) && lossSnapshot.lossQuality==LLTransportLossQuality::Complete);
    for (uint32_t id : {1u,3u,5u})
    {
        lossRing.packet=packet(0,id,42,uint8_t(id)); lossLoop->notifySocketReadable();
        CHECK(lossTick(2+id)==LLTransportOwnerLoop::Step::Progress);
    }
    CHECK(lossDirectory->get(endpoint,lossSnapshot) && lossSnapshot.lossQuality==LLTransportLossQuality::Partial);
    lossRing.injectReceive=true;
    lossRing.injectedReceive={LLPacketRing::OwnerReceiveStatus::Interrupted,0,EINTR};
    lossLoop->notifySocketReadable();
    (void)lossTick(100);
    CHECK(!lossLoop->needsReceiveTurn(LLTransportSteadyUsec(101)) &&
          lossLoop->needsReceiveTurn(LLTransportSteadyUsec(10100)));
    const auto beforeRetry=lossRing.receiveAttempts;
    (void)lossTick(101);
    CHECK(lossRing.receiveAttempts==beforeRetry);
    (void)lossTick(10100);
    CHECK(lossRing.receiveAttempts==beforeRetry+1 && !lossLoop->needsReceiveTurn(LLTransportSteadyUsec(10100)));
#if !LL_WINDOWS
    CHECK(lossLoop->noteSocketError(ECONNREFUSED,LLTransportSteadyUsec(11000)));
    CHECK(!lossLoop->needsReceiveTurn(LLTransportSteadyUsec(11001)) &&
          lossLoop->needsReceiveTurn(LLTransportSteadyUsec(21000)));
    CHECK(lossLoop->noteSocketError(0,LLTransportSteadyUsec(11001)) &&
          lossLoop->needsReceiveTurn(LLTransportSteadyUsec(11001)));
    CHECK(!lossLoop->noteSocketError(EBADF,LLTransportSteadyUsec(11001)));
#endif
}
