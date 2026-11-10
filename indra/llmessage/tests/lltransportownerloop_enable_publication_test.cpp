// Real owner Enable path and real directory; fake wake inspects the terminal
// at the precise publication hook. No actual worker, UDP or viewer runtime.
#include "lltransportownerloop.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
struct Witness final : LLTransportMainWake
{
    LLTransportOwnerLoop::Directory& directory;
    LLTransportOwnerChannels& channels;
    LLTransportEndpoint endpoint;
    bool sawTerminal=false, sawSnapshot=false;
    Witness(LLTransportOwnerLoop::Directory& d,LLTransportOwnerChannels& c,LLTransportEndpoint e):directory(d),channels(c),endpoint(e) {}
    bool observe(uint64_t& out) const noexcept override { out=0; return true; }
    void signal() noexcept override
    {
        const auto* terminal=channels.circuitTerminals.peekConsumer();
        if (!terminal) return;
        sawTerminal=terminal->status==LLTransportCircuitCommandStatus::Applied;
        LLTransportCircuitSnapshot snapshot;
        sawSnapshot=directory.get(endpoint,snapshot) && snapshot.state==LLTransportSnapshotState::Present &&
                    snapshot.incarnation==terminal->circuitIncarnation && snapshot.publicationSequence;
    }
    LLTransportWakeResult waitUntil(uint64_t,LLTransportSteadyUsec) noexcept override { return LLTransportWakeResult::Unavailable; }
};
int main()
{
    using State=LLTransportCircuitState;
    const LLTransportEndpoint endpoint{0x0100007f,1234};
    auto state=std::make_unique<State>(); uint64_t charge=0; CHECK(state->init({1,1,1,1,1,1,8388608},charge));
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(1024,512));
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>(); CHECK(directory->activateOnOwner());
    LLPacketRing ring; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,nullptr,0);
    auto wake=std::make_shared<Witness>(*directory,*channels,endpoint);
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison,wake); CHECK(loop->initializeWatchdogs(1));
    LLTransportOwnerCommand command; command.kind=LLTransportOwnerCommand::Kind::Enable;
    command.circuit.kind=LLTransportCircuitCommandKind::Enable; command.circuit.endpoint=endpoint;
    command.circuit.loginGeneration=7; command.circuit.operationId=1;
    CHECK(channels->commands.beginOfferOnMain()); auto* slot=channels->commands.reserveProducer(); CHECK(slot);
    *slot=command; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    const State::WatchdogConfig watchdog{1000000,60000000,0.f,0.5f};
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(1),watchdog)==LLTransportOwnerLoop::Step::Blocked);
    CHECK(!channels->circuitTerminals.peekConsumer());
    CHECK(directory->beginStateMutationOnOwner());
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(1),watchdog,directory.get())==LLTransportOwnerLoop::Step::Progress);
    CHECK(wake->sawTerminal && wake->sawSnapshot);
    CHECK(directory->endStateMutationOnOwner());
    CHECK(channels->circuitTerminals.peekConsumer() && channels->circuitTerminals.releaseConsumer());
    CHECK(!poison.load());
    // Missing owner transaction makes reconciliation fail closed; no success
    // terminal may escape even if state enable itself has already succeeded.
    command.circuit.operationId=2;
    CHECK(channels->commands.beginOfferOnMain()); slot=channels->commands.reserveProducer(); CHECK(slot);
    *slot=command; CHECK(channels->commands.publishProducer()); channels->commands.endOfferOnMain();
    CHECK(loop->processCircuitHead(LLTransportSteadyUsec(2),watchdog,directory.get())==LLTransportOwnerLoop::Step::Poisoned);
    LLTransportCircuitSnapshot ignored;
    CHECK(poison.load() && !channels->circuitTerminals.peekConsumer() && !directory->get(endpoint,ignored));
}
