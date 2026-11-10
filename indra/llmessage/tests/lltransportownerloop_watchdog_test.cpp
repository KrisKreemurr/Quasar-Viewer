// Actual state/watchdog/action methods with full real queues; fake socket only.
// Manually driven steps, not a worker-thread timing or 1 Hz test.
#include "lltransportownerloop.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
struct Wake final : LLTransportMainWake
{
    uint64_t signals=0;
    bool observe(uint64_t& out) const noexcept override { out=signals; return true; }
    void signal() noexcept override { ++signals; }
    LLTransportWakeResult waitUntil(uint64_t,LLTransportSteadyUsec) noexcept override { return LLTransportWakeResult::Unavailable; }
};
int main()
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charged=0;
    CHECK(state->init({2,4,32,32,4,16,8388608},charged));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,nullptr,0);
    auto wake=std::make_shared<Wake>();
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison,wake);
    CHECK(loop->initializePending());
    CHECK(loop->initializeWatchdogs(2));
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>(); CHECK(directory->activateOnOwner());
    LLTransportOwnerCommand enable; enable.kind=LLTransportOwnerCommand::Kind::Enable;
    enable.circuit.kind=LLTransportCircuitCommandKind::Enable; enable.circuit.endpoint=endpoint;
    enable.circuit.loginGeneration=7; enable.circuit.trusted=true; enable.circuit.operationId=1;
    CHECK(channels->commands.beginOfferOnMain()); auto* command=channels->commands.reserveProducer(); CHECK(command); *command=enable; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(directory->beginStateMutationOnOwner());
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(100),{100,1000,0.f,0.5f},directory.get())==LLTransportOwnerLoop::Step::Progress);
    CHECK(directory->endStateMutationOnOwner());
    const auto* enabled=channels->circuitTerminals.peekConsumer(); CHECK(enabled && enabled->status==LLTransportCircuitCommandStatus::Applied);
    const auto incarnation=enabled->circuitIncarnation; CHECK(channels->circuitTerminals.releaseConsumer());
    // Synthetic occupancy of actual queues; no claim of retained payloads.
    for (unsigned i=0;i<4096;++i)
    { auto* event=events->reserveProducer(); CHECK(event); *event={}; CHECK(events->publishProducer()); }
    CHECK(!events->reserveProducer());
    CHECK(loop->pollNextWatchdog(LLTransportSteadyUsec(201),0.5f)==LLTransportOwnerLoop::Step::Progress);
    CHECK(loop->sendStateActionOnce(ring,0,proxy,LLTransportSteadyUsec(201),0)==LLTransportOwnerLoop::Step::Progress);
    CHECK(ring.sentPackets.size()==1 && ring.sentPackets[0].size()==12 && ring.sentPackets[0][6]==1);
    for (unsigned i=0;i<256;++i)
    { auto* control=channels->controls.reserveProducer(); CHECK(control); *control={}; CHECK(channels->controls.publishProducer()); }
    CHECK(loop->pollNextWatchdog(LLTransportSteadyUsec(1101),0.5f)==LLTransportOwnerLoop::Step::Idle);
    CHECK(loop->pollNextWatchdog(LLTransportSteadyUsec(1101),0.5f)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(state->lookup(endpoint,1101).alive && wake->signals==1);
    CHECK(channels->controls.peekConsumer() && channels->controls.releaseConsumer());
    CHECK(loop->pollNextWatchdog(LLTransportSteadyUsec(1101),0.5f)==LLTransportOwnerLoop::Step::Idle);
    CHECK(loop->pollNextWatchdog(LLTransportSteadyUsec(1101),0.5f)==LLTransportOwnerLoop::Step::Progress);
    CHECK(!state->lookup(endpoint,1101).alive && wake->signals==2);
    for (unsigned i=0;i<255;++i) CHECK(channels->controls.peekConsumer() && channels->controls.releaseConsumer());
    const auto* timeout=channels->controls.peekConsumer();
    CHECK(timeout && timeout->circuitIncarnation==incarnation && timeout->lastPublishedAppSequence==0);
    CHECK(channels->controls.releaseConsumer());
    // A successful close immediately frees a cap-one watch for another
    // endpoint without waiting for a future round-robin scan.
    auto secondState=std::make_unique<State>(); uint64_t secondCharge=0;
    CHECK(secondState->init({1,2,8,8,2,4,8388608},secondCharge));
    auto secondChannels=std::make_unique<LLTransportOwnerChannels>();
    auto secondPump=std::make_unique<LLTransportReceivePump>(ring,*secondState,*arena,*events,poison,nullptr,0);
    auto secondLoop=std::make_unique<LLTransportOwnerLoop>(*secondState,*secondPump,*secondChannels,poison);
    CHECK(secondLoop->initializeWatchdogs(1));
    auto secondDirectory=std::make_unique<LLTransportOwnerLoop::Directory>(); CHECK(secondDirectory->activateOnOwner());
    auto submit=[&](const LLTransportOwnerCommand& value) {
        CHECK(secondChannels->commands.beginOfferOnMain()); auto* slot=secondChannels->commands.reserveProducer(); CHECK(slot); *slot=value; CHECK(secondChannels->commands.publishProducer()); secondChannels->commands.endOfferOnMain();
    };
    auto pop=[&]() { const auto* slot=secondChannels->circuitTerminals.peekConsumer(); CHECK(slot); auto value=*slot;
                    CHECK(secondChannels->circuitTerminals.releaseConsumer()); return value; };
    submit(enable); CHECK(secondDirectory->beginStateMutationOnOwner());
    CHECK(secondLoop->processCircuitHead(LLTransportSteadyUsec(100),{100,1000,0.f,0.5f},secondDirectory.get())==LLTransportOwnerLoop::Step::Progress);
    CHECK(secondDirectory->endStateMutationOnOwner());
    const auto first=pop(); CHECK(first.status==LLTransportCircuitCommandStatus::Applied);
    LLTransportOwnerCommand close; close.kind=LLTransportOwnerCommand::Kind::Disable;
    close.circuit.kind=LLTransportCircuitCommandKind::Disable; close.circuit.endpoint=endpoint;
    close.circuit.loginGeneration=7; close.circuit.targetIncarnation=first.circuitIncarnation;
    submit(close); CHECK(secondLoop->processCircuitHead(LLTransportSteadyUsec(101),{100,1000,0.f,0.5f})==LLTransportOwnerLoop::Step::Progress);
    CHECK(pop().status==LLTransportCircuitCommandStatus::Applied);
    enable.circuit.endpoint={0x0200007f,5678};
    submit(enable); CHECK(secondDirectory->beginStateMutationOnOwner());
    CHECK(secondLoop->processCircuitHead(LLTransportSteadyUsec(102),{100,1000,0.f,0.5f},secondDirectory.get())==LLTransportOwnerLoop::Step::Progress);
    CHECK(secondDirectory->endStateMutationOnOwner());
    CHECK(pop().status==LLTransportCircuitCommandStatus::Applied);
}
