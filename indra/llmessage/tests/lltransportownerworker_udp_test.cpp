// Actual owner worker + native wait + loopback UDP, with fake ring/template
// boundaries. The peer reads only its own socket; it never reads owner UDP.
#include "lltransportownerworker.h"
#include "lltransportowneradmission.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
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
struct Wake final : LLTransportMainWake
{
    std::atomic<uint64_t> sequence{0};
    bool observe(uint64_t& out) const noexcept override { out=sequence.load(); return true; }
    void signal() noexcept override { sequence.fetch_add(1); }
    LLTransportWakeResult waitUntil(uint64_t,LLTransportSteadyUsec) noexcept override { return LLTransportWakeResult::Unavailable; }
};
static sockaddr_in bindLoopback(int fd)
{
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    socklen_t length=sizeof(addr); CHECK(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&length)==0);
    CHECK(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK)==0); return addr;
}
int main()
{
    using namespace std::chrono;
    const int ownerSocket=socket(AF_INET,SOCK_DGRAM,0), peerSocket=socket(AF_INET,SOCK_DGRAM,0);
    CHECK(ownerSocket>=0 && peerSocket>=0);
    const auto ownerAddr=bindLoopback(ownerSocket), peerAddr=bindLoopback(peerSocket);
    const LLTransportEndpoint peer{peerAddr.sin_addr.s_addr,ntohs(peerAddr.sin_port)};
    auto state=std::make_unique<LLTransportCircuitState>(); uint64_t charged=0;
    CHECK(state->init({1,16,128,128,16,64,8388608},charged));
    auto channels=std::make_unique<LLTransportOwnerChannels>();
    auto events=std::make_unique<LLTransportIngressQueue>();
    auto arena=std::make_unique<LLTransportDecodedArena>(); CHECK(arena->initialize(2048,512));
    LLMessageVariable byte{MVT_FIXED,1},u32{MVT_FIXED,4};
    LLMessageBlock pingBlock,appBlock; pingBlock.mMemberVariables={&byte,&u32}; appBlock.mMemberVariables={&byte};
    LLMessageTemplate ping,app; ping.mMessageNumber=1; ping.mMemberBlocks={&pingBlock};
    app.mMessageNumber=42; app.mMemberBlocks={&appBlock};
    LLTransportStartupBudget budget; CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema pingSchema,appSchema;
    CHECK(freezeTransportSchema(ping,8,budget,pingSchema)==LLTransportFreezeStatus::Complete);
    CHECK(freezeTransportSchema(app,8,budget,appSchema)==LLTransportFreezeStatus::Complete);
    const LLTransportSchemaEntry schemas[]={{1,&pingSchema,false,true},{42,&appSchema,false,false}};
    LLPacketRing ring; std::atomic<bool> poison{false};
    auto pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,schemas,2);
    pump->testBeginMainStall();
    auto directory=std::make_unique<LLTransportOwnerLoop::Directory>(); auto wake=std::make_shared<Wake>();
    auto loop=std::make_unique<LLTransportOwnerLoop>(*state,*pump,*channels,poison,wake);
    CHECK(loop->initializePending() && loop->initializeWatchdogs(1));
    LLTransportOwnerAdmission admission(*channels,schemas,2); LLTransportRingPublication stats;
    LLTransportOwnerConfig config; config.socket=ownerSocket; config.loginGeneration=7; config.schemas=schemas;
    config.schemaCount=2; config.heartbeatUsec=1000000; config.timeoutUsec=30000000; config.mainWake=wake;
    auto native=llCreateTransportOwnerWait(); CHECK(native);
    LLTransportOwnerWorker worker(*loop,*directory,ring,stats,config,native); CHECK(worker.startOnMain());
    auto offer=[&](const LLTransportOwnerCommand& command) {
        CHECK(channels->commands.beginOfferOnMain()); const auto result=admission.offerOnMain(command);
        channels->commands.endOfferOnMain(); CHECK(result==LLTransportCommandOffer::Accepted); worker.signalOnMain(); };
    LLTransportOwnerCommand enable; enable.kind=LLTransportOwnerCommand::Kind::Enable;
    enable.circuit.kind=LLTransportCircuitCommandKind::Enable; enable.circuit.endpoint=peer;
    enable.circuit.loginGeneration=7; enable.circuit.operationId=1; enable.circuit.trusted=true; offer(enable);
    const auto setupDeadline=steady_clock::now()+seconds(2);
    while (!channels->circuitTerminals.peekConsumer() && steady_clock::now()<setupDeadline)
        std::this_thread::sleep_for(milliseconds(1));
    const auto* terminal=channels->circuitTerminals.peekConsumer(); CHECK(terminal);
    const auto incarnation=terminal->circuitIncarnation;
    CHECK(admission.matchesCircuitOnMain(*terminal)); const auto enabled=*terminal;
    CHECK(channels->circuitTerminals.releaseConsumer() && admission.releaseCircuitCreditOnMain(enabled));
    LLTransportOwnerCommand send; send.kind=LLTransportOwnerCommand::Kind::Send; send.reliable=true;
    send.sendToken=77; send.schemaGeneration=8; send.circuit.endpoint=peer;
    send.circuit.loginGeneration=7; send.circuit.targetIncarnation=incarnation;
    send.retry={3,600000,600000,false}; send.byteCount=8; send.bytes[6]=42; send.bytes[7]=7; offer(send);
    std::atomic<bool> running{true}; std::atomic<unsigned> pingReplies{0}, stalledReplies{0}, retransmits{0};
    const auto start=steady_clock::now();
    std::thread simulatedPeer([&] {
        auto nextPing=steady_clock::now(); uint32_t nextPacket=1, outgoing=0; bool acked=false;
        while (running.load())
        {
            const auto now=steady_clock::now();
            if (now>=nextPing)
            {
                const uint32_t id=nextPacket++;
                uint8_t packet[]={uint8_t(acked ? 0x10 : 0),uint8_t(id>>24),uint8_t(id>>16),uint8_t(id>>8),uint8_t(id),0,1,9,0,0,0,0,0,0,0,0,1};
                if (acked) { packet[12]=uint8_t(outgoing>>24); packet[13]=uint8_t(outgoing>>16);
                    packet[14]=uint8_t(outgoing>>8); packet[15]=uint8_t(outgoing); }
                const auto size=acked ? sizeof(packet) : 12u;
                (void)sendto(peerSocket,packet,size,0,reinterpret_cast<const sockaddr*>(&ownerAddr),sizeof(ownerAddr));
                nextPing+=seconds(1);
            }
            for (unsigned i=0;i<8;++i)
            {
                const uint32_t id=nextPacket++;
                const uint8_t appPacket[]={0,uint8_t(id>>24),uint8_t(id>>16),uint8_t(id>>8),uint8_t(id),0,42,1};
                (void)sendto(peerSocket,appPacket,sizeof(appPacket),0,reinterpret_cast<const sockaddr*>(&ownerAddr),sizeof(ownerAddr));
            }
            uint8_t wire[NET_BUFFER_SIZE];
            for (;;)
            {
                const auto n=recv(peerSocket,wire,sizeof(wire),0); if (n<0) break;
                if (n>=8 && wire[6]==2) { ++pingReplies; if (steady_clock::now()<start+seconds(3)) ++stalledReplies; }
                if (n>=8 && wire[6]==42) { outgoing=llTransportReadNetworkU32(wire+1); if (wire[0]&0x20) { ++retransmits; acked=true; } }
            }
            std::this_thread::sleep_for(milliseconds(5));
        }
    });
    uint64_t consumed=0;
    // Deliberately stall main for three seconds, then service its application
    // queue at one-second intervals while the peer continues traffic.
    for (int tick=3;tick<=7;++tick)
    {
        std::this_thread::sleep_until(start+seconds(tick));
        if (tick==3)
        {
            // Do not drain until the owner has observed the phase boundary;
            // every counted phase-1 backpressure preceded this handshake.
            pump->testEndMainStall();
            const auto deadline=steady_clock::now()+seconds(1);
            while (!pump->testMainStallEnded() && steady_clock::now()<deadline)
                std::this_thread::sleep_for(milliseconds(1));
            CHECK(pump->testMainStallEnded());
        }
        while (const auto* event=events->peekConsumer())
        { CHECK(arena->release(event->arenaRecord)); CHECK(events->releaseConsumer()); ++consumed; }
        CHECK(!worker.failed());
    }
    running.store(false); simulatedPeer.join(); worker.requestStopOnMain(); worker.joinOnMain();
    while (const auto* event=events->peekConsumer())
    { CHECK(arena->release(event->arenaRecord)); CHECK(events->releaseConsumer()); ++consumed; }
    const auto* completion=channels->sendTerminals.peekConsumer(); CHECK(completion && completion->operationId==77 && completion->originalStatus==0);
    const auto copied=*completion; CHECK(channels->sendTerminals.releaseConsumer() && admission.releaseReliableCreditOnMain(copied));
    const auto backpressure=pump->testAppPublishBackpressureAfterJoin();
    const auto stalledBackpressure=pump->testStalledAppBackpressureAfterJoin();
    CHECK(!worker.failed() && pingReplies.load()>=4 && stalledReplies.load()>=2 && retransmits.load()>=1 &&
          consumed>0 && stalledBackpressure>0 && ring.appPacketsAfterJoin()>consumed);
    std::printf("pings=%u stalled_pings=%u retransmits=%u app_received=%llu app_consumed=%llu app_backpressure=%llu stalled_backpressure=%llu\n",pingReplies.load(),stalledReplies.load(),retransmits.load(),
        static_cast<unsigned long long>(ring.appPacketsAfterJoin()),static_cast<unsigned long long>(consumed),
        static_cast<unsigned long long>(backpressure),static_cast<unsigned long long>(stalledBackpressure));
    close(peerSocket); close(ownerSocket);
}
