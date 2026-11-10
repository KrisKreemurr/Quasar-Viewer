// Actual owner turns publish a warning before applying a truncated StartPing.
// Fake packet boundary only; not a live worker or network-performance test.
#include "lltransportownerloop.h"
#include <array>
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
int main(int argc,char**)
{
    using State=LLTransportCircuitState;
    auto state=std::make_unique<State>(); uint64_t charge=0;
    CHECK(state->init({1,2,16,16,4,16,8388608},charge));
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(65536,512));
    LLMessageVariable byte{MVT_FIXED,1},u32{MVT_FIXED,4};
    LLMessageBlock appBlock,ackBlock; appBlock.mMemberVariables={&byte};
    ackBlock.mMemberVariables={&byte,&u32};
    LLMessageTemplate app,ack; app.mMessageNumber=42; app.mMemberBlocks={&appBlock};
    ack.mMessageNumber=1; ack.mMemberBlocks={&ackBlock};
    LLTransportStartupBudget budget; CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema appSchema,ackSchema;
    CHECK(freezeTransportSchema(app,8,budget,appSchema)==LLTransportFreezeStatus::Complete);
    CHECK(freezeTransportSchema(ack,8,budget,ackSchema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry schemas[]={{1,&ackSchema,false,true},{42,&appSchema,false,false}};
    LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,schemas,2);
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>(); CHECK(directory->activateOnOwner());
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison);
    CHECK(loop->initializePending() && loop->initializeWatchdogs(1));
    const LLTransportEndpoint endpoint{0x0100007f,1234};
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
    // Ordinary unreliable pings isolate warning coalescing. Reliable mode
    // checks unacknowledged admission refusal and later control progress.
    const bool reliable=argc>1;
    uint32_t outgoing=0;
    if (reliable)
    {
        LLTransportOwnerCommand send; send.kind=LLTransportOwnerCommand::Kind::Send; send.reliable=true;
        send.sendToken=77; send.schemaGeneration=8; send.circuit.endpoint=endpoint;
        send.circuit.loginGeneration=7; send.circuit.targetIncarnation=incarnation;
        send.retry={1,10000000,10000000,false}; send.byteCount=8; send.bytes[6]=42; send.bytes[7]=1;
        offer(send); (void)turn(2); CHECK(!ring.sentPackets.empty());
        outgoing=llTransportReadNetworkU32(ring.sentPackets.back().data()+1);
    }
    for (unsigned n=0;n<(reliable ? 17u : 80u);++n)
    {
        const auto before=ring.sentPackets.size();
        ring.packet={uint8_t(reliable ? 0x40 : 0),0,0,0,uint8_t(n+3),0,1,uint8_t(n)};
        if (reliable && n==16)
        {
            ring.packet[0] |= 0x10;
            ring.packet.insert(ring.packet.end(),{10,0,0,0,uint8_t(outgoing>>24),uint8_t(outgoing>>16),
                uint8_t(outgoing>>8),uint8_t(outgoing),1});
        }
        loop->notifySocketReadable(); (void)turn(3+n*2); (void)turn(4+n*2);
        CHECK(!poison.load() && !ring.sentPackets.empty());
        if (reliable && n==16)
        {
            for (auto i=before;i<ring.sentPackets.size();++i)
                CHECK(!(ring.sentPackets[i].size()>=8 && ring.sentPackets[i][6]==2 && ring.sentPackets[i][7]==16));
            CHECK(state->classifyResentDuplicate(endpoint,incarnation,7,3,true)==State::DuplicateClass::Duplicate);
            CHECK(state->classifyResentDuplicate(endpoint,incarnation,7,19,true)==State::DuplicateClass::NotDuplicate);
            CHECK(!channels->sendTerminals.peekConsumer()); // full preflight precedes trailer ACK
            for (const auto& wire:ring.sentPackets)
            {
                std::array<uint8_t,16384> expanded{};
                const auto envelope=llTransportParseEnvelope(wire.data(),wire.size(),expanded.data(),expanded.size());
                CHECK(envelope.status==LLTransportEnvelope::Status::Complete);
                for (uint8_t i=0;i<envelope.ackCount;++i)
                { uint32_t id=0; CHECK(llTransportAppendedAck(wire.data(),wire.size(),envelope,i,id)); CHECK(id!=19); }
                if (wire.size()>=11 && wire[6]==0xff && wire[7]==0xff && wire[8]==0xff && wire[9]==0xfb)
                    for (unsigned i=0;i<wire[10];++i)
                    { const auto k=11+i*4; CHECK(k+4<=wire.size());
                      const uint32_t id=uint32_t(wire[k]) | (uint32_t(wire[k+1])<<8) |
                          (uint32_t(wire[k+2])<<16) | (uint32_t(wire[k+3])<<24); CHECK(id!=19); }
            }
            break;
        }
        CHECK(ring.sentPackets.back()[6]==2 && ring.sentPackets.back()[7]==uint8_t(n));
    }
    if (reliable)
    {
        CHECK(pump->controlBackpressureOnOwner()==1);
        // A committed resend still gets its duplicate ACK while history is full.
        const auto beforeDuplicate=ring.sentPackets.size();
        ring.packet={0x60,0,0,0,3,0,1,0,0,0,0,0};
        loop->notifySocketReadable(); (void)turn(37); (void)turn(38);
        bool duplicateAck=false;
        for (auto i=beforeDuplicate;i<ring.sentPackets.size();++i)
        {
            const auto& wire=ring.sentPackets[i]; std::array<uint8_t,16384> expanded{};
            const auto env=llTransportParseEnvelope(wire.data(),wire.size(),expanded.data(),expanded.size());
            CHECK(env.status==LLTransportEnvelope::Status::Complete);
            for (uint8_t j=0;j<env.ackCount;++j)
            { uint32_t id=0; CHECK(llTransportAppendedAck(wire.data(),wire.size(),env,j,id)); duplicateAck|=id==3; }
            if (wire.size()>=15 && wire[6]==0xff && wire[7]==0xff && wire[8]==0xff && wire[9]==0xfb)
                for (unsigned j=0;j<wire[10];++j)
                { const auto k=11+j*4; CHECK(k+4<=wire.size());
                  const uint32_t id=uint32_t(wire[k]) | (uint32_t(wire[k+1])<<8) |
                      (uint32_t(wire[k+2])<<16) | (uint32_t(wire[k+3])<<24); duplicateAck|=id==3; }
        }
        CHECK(duplicateAck && pump->controlBackpressureOnOwner()==1);
        // An independent valid ping with an appended ACK must still run.
        ring.packet={0x10,0,0,0,100,0,1,99,0,0,0,0,
            uint8_t(outgoing>>24),uint8_t(outgoing>>16),uint8_t(outgoing>>8),uint8_t(outgoing),1};
        loop->notifySocketReadable(); (void)turn(39); (void)turn(40); (void)turn(41);
        CHECK(!poison.load() && ring.sentPackets.back()[6]==2 && ring.sentPackets.back()[7]==99);
        const auto* completed=channels->sendTerminals.peekConsumer();
        CHECK(completed && completed->operationId==77 && completed->originalStatus==0);
        return 0;
    }
    ring.packet={0,0,0,0,100,0,1,99,0,0,0,0};
    loop->notifySocketReadable(); (void)turn(163); (void)turn(164);
    CHECK(!poison.load() && ring.sentPackets.back()[6]==2 && ring.sentPackets.back()[7]==99);
    LLTransportControlWarningStats stats;
    CHECK(channels->controlWarnings.readOnMain(1,stats));
    CHECK(stats.events==80 && stats.coalescedEvents==79 && stats.truncations>=80);
    CHECK(stats.firstTimeUsec==4 && stats.lastTimeUsec==162);
    CHECK(stats.last.packetId==82 && stats.last.endpoint.addressNetworkOrder==endpoint.addressNetworkOrder);
    CHECK(!stats.saturated && stats.unaccountedEvents==0);
}
