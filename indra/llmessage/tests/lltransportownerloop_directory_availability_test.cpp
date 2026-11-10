// Actual owner turns and directory with a fake packet boundary. Reports
// observed availability, not a viewer, GPU or production throughput claim.
#include "lltransportownerloop.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
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
    CHECK(state->init({1,2,8192,8192,2,32,8388608},charge));
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1048576,512));
    LLMessageVariable byte{MVT_FIXED,1}; LLMessageBlock block; block.mMemberVariables={&byte};
    LLMessageTemplate app; app.mMessageNumber=42; app.mMemberBlocks={&block};
    LLTransportStartupBudget budget; CHECK((LLTransportStartupBudget::initialize<16384,512,2048,1>(budget,1024,1000000,1000000)));
    LLTransportSchema schema; CHECK(freezeTransportSchema(app,8,budget,schema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry schemas[]={{42,&schema,false,false}};
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,schemas,1);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>();
    CHECK(loop->initializePending() && loop->initializeWatchdogs(1));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    LLTransportOwnerCommand enable; enable.kind=LLTransportOwnerCommand::Kind::Enable;
    enable.circuit.kind=LLTransportCircuitCommandKind::Enable; enable.circuit.endpoint=endpoint;
    enable.circuit.loginGeneration=7; enable.circuit.trusted=true; enable.circuit.operationId=1;
    CHECK(channels->commands.beginOfferOnMain()); auto* slot=channels->commands.reserveProducer(); CHECK(slot);
    *slot=enable; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    std::atomic<bool> ready{false},go{false},done{false};
    constexpr uint32_t turns=100000;
    std::thread owner([&] {
        CHECK(directory->activateOnOwner());
        const State::WatchdogConfig watch{1000000000,2000000000,0.f,0.5f};
        CHECK(loop->driveOnce(*directory,ring,0,proxy,LLTransportSteadyUsec(1),7,schemas,1,watch,true)==LLTransportOwnerLoop::Step::Progress);
        ready.store(true); while (!go.load()) std::this_thread::yield();
        for (uint32_t i=1;i<=turns;++i)
        {
            ring.packet={0,uint8_t(i>>24),uint8_t(i>>16),uint8_t(i>>8),uint8_t(i),0,42,1};
            loop->notifySocketReadable();
            CHECK(loop->driveOnce(*directory,ring,0,proxy,LLTransportSteadyUsec(i+1),7,schemas,1,watch,true)!=LLTransportOwnerLoop::Step::Poisoned);
        }
        done.store(true);
    });
    while (!ready.load()) std::this_thread::yield();
    LLTransportCircuitSnapshot snapshot; unsigned idleGood=0;
    for (unsigned i=0;i<1000;++i) if (directory->get(endpoint,snapshot)) ++idleGood;
    CHECK(idleGood==1000 && snapshot.state==LLTransportSnapshotState::Present);
    uint64_t good=0,bad=0,run=0,longest=0,maxNs=0;
    go.store(true);
    while (!done.load())
    {
        const auto start=std::chrono::steady_clock::now();
        const bool available=directory->get(endpoint,snapshot);
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        if (elapsed>0 && static_cast<uint64_t>(elapsed)>maxNs) maxNs=static_cast<uint64_t>(elapsed);
        if (available) { CHECK(snapshot.state==LLTransportSnapshotState::Present); ++good; run=0; }
        else { ++bad; if (++run>longest) longest=run; }
    }
    owner.join();
    CHECK(!poison.load() && good+bad>0);
    uint64_t queued=0;
    while (events->peekConsumer()) { ++queued; CHECK(events->releaseConsumer()); }
    std::printf("idle_available=%u idle_samples=1000 busy_available=%llu busy_unavailable=%llu longest_unavailable=%llu max_lookup_ns=%llu turns=%u receive_attempts=%u retained_events=%llu\n",
        idleGood,static_cast<unsigned long long>(good),static_cast<unsigned long long>(bad),
        static_cast<unsigned long long>(longest),static_cast<unsigned long long>(maxNs),turns,
        ring.receiveAttempts,static_cast<unsigned long long>(queued));
}
