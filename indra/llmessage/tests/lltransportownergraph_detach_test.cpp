// Actual graph/worker/holder with loopback socket; ring and template are fakes.
#include "lltransportownergraph.h"
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
static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
struct Wake final : LLTransportMainWake
{
    bool observe(uint64_t& out) const noexcept override { out=sequence.load(); return true; }
    void signal() noexcept override { ++sequence; }
    LLTransportWakeResult waitUntil(uint64_t,LLTransportSteadyUsec) noexcept override { return LLTransportWakeResult::Unavailable; }
    std::atomic<uint64_t> sequence{0};
};
struct Lease final : LLTransportSchemaLease
{
    LLTransportSchemaEntry entriesArray[2]{};
    const LLTransportSchemaEntry* entries() const noexcept override { return entriesArray; }
    uint32_t count() const noexcept override { return 2; }
    uint64_t retainedBytes() const noexcept override { return 8192; }
};
int main()
{
    const int fd=socket(AF_INET,SOCK_DGRAM,0); CHECK(fd>=0);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    socklen_t addressLength=sizeof(address); CHECK(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&addressLength)==0);
    CHECK(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK)==0);
    const int peer=socket(AF_INET,SOCK_DGRAM,0); CHECK(peer>=0);
    sockaddr_in peerAddress{}; peerAddress.sin_family=AF_INET; peerAddress.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(peer,reinterpret_cast<sockaddr*>(&peerAddress),sizeof(peerAddress))==0);
    socklen_t peerLength=sizeof(peerAddress); CHECK(getsockname(peer,reinterpret_cast<sockaddr*>(&peerAddress),&peerLength)==0);
    LLMessageVariable byte{MVT_FIXED,1},u32{MVT_FIXED,4}; LLMessageBlock block,pingBlock;
    block.mMemberVariables={&byte}; pingBlock.mMemberVariables={&byte,&u32};
    LLMessageTemplate source; source.mMessageNumber=42; source.mMemberBlocks={&block};
    LLMessageTemplate ping; ping.mMessageNumber=1; ping.mMemberBlocks={&pingBlock};
    LLTransportStartupBudget budget; CHECK((LLTransportStartupBudget::initialize<16384,512,2048,2>(budget,1024,1000000,1000000)));
    LLTransportSchema schema; CHECK(freezeTransportSchema(source,8,budget,schema)==LLTransportFreezeStatus::Complete);
    LLTransportSchema pingSchema; CHECK(freezeTransportSchema(ping,8,budget,pingSchema)==LLTransportFreezeStatus::Complete);
    auto lease=std::make_shared<Lease>(); lease->entriesArray[0]={1,&pingSchema,false,true};
    lease->entriesArray[1]={42,&schema,false,false}; auto wake=std::make_shared<Wake>();
    LLTransportOwnerConfig config; config.socket=fd; config.mainWake=wake; config.schemaLease=lease;
    config.schemas=lease->entries(); config.schemaCount=2; config.loginGeneration=7;
    config.controlTruncationCallbacksAbsent=true; config.schemaBytes=lease->retainedBytes();
    config.totalBudgetBytes=128*1024*1024; config.allocatorOverheadBytes=65536;
    config.arenaCapacityBytes=1024*1024; config.maxArenaRecordBytes=256*1024;
    config.stateLimits={2,16,128,128,16,64,8388608}; config.heartbeatUsec=1000000; config.timeoutUsec=30000000;
    auto ring=std::make_unique<LLPacketRing>(); std::shared_ptr<LLTransportOwnerGraph> graph;
    auto tooSmall=config; tooSmall.totalBudgetBytes=1;
    CHECK(LLTransportOwnerGraph::prepareOnMain(tooSmall,*ring,graph)==LLTransportOwnerGraph::Prepare::Capacity && !graph);
    CHECK(LLTransportOwnerGraph::prepareOnMain(config,*ring,graph)==LLTransportOwnerGraph::Prepare::Prepared && graph);
    auto facade=std::make_unique<LLTransportOwnerFacade>(graph);
    CHECK(!facade->inboundEmptyAfterJoinOnMain());
    CHECK(graph->startForTestOnMain()==LLTransportOwnerGraph::Start::Started);
    CHECK(!facade->inboundEmptyAfterJoinOnMain()); // empty while producer can publish
    LLTransportBacklog backlog;
    bool pending=true; CHECK(facade->ingressPending(pending) && !pending);
    CHECK(facade->backlog(backlog) && backlog.loginGeneration==7 && backlog.pendingPackets==0 &&
          backlog.pendingBytes==0 && backlog.loadRate==0.f);
    LLTransportCircuitCommand enable; enable.kind=LLTransportCircuitCommandKind::Enable;
    enable.endpoint={peerAddress.sin_addr.s_addr,ntohs(peerAddress.sin_port)};
    enable.loginGeneration=7; enable.operationId=1; enable.trusted=true;
    CHECK(facade->submitCircuitCommand(enable)==LLTransportCommandOffer::Accepted);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    LLTransportCircuitSnapshot published;
    while ((!facade->getCircuitSnapshot(enable.endpoint,published) || published.state!=LLTransportSnapshotState::Present) &&
           std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(published.state==LLTransportSnapshotState::Present);
    const uint8_t app[]={0,0,0,0,1,0,42,7};
    CHECK(sendto(peer,app,sizeof(app),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==static_cast<ssize_t>(sizeof(app)));
    LLTransportIngressEvent event; LLTransportDecodedView view;
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!facade->peekInbound(event,view) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(view.data && event.packetId==1);
    CHECK(facade->ingressPending(pending) && pending);
    // The ring is empty even though one application record is held; the
    // legacy throttle signal must not silently become ingress occupancy.
    CHECK(facade->backlog(backlog) && backlog.pendingPackets==0 && backlog.pendingBytes==0 && backlog.loadRate==0.f);
    const uint8_t truncatedPing[]={0,0,0,0,2,0,1,7};
    CHECK(sendto(peer,truncatedPing,sizeof(truncatedPing),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==static_cast<ssize_t>(sizeof(truncatedPing)));
    LLTransportControlWarningStats runningWarning;
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!facade->controlWarningStats(1,runningWarning) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(runningWarning.events==1);
    CHECK(facade->beginOfferOnMain()); CHECK(!graph->requestStopOnMain()); facade->endOfferOnMain();
    CHECK(graph->requestStopOnMain() && graph->joinOnMain() && graph->joinedOnMain());
    CHECK(!facade->inboundEmptyAfterJoinOnMain()); // joined, but payload is borrowed
    LLTransportControlWarningStats warning{}; bool present=true;
    CHECK(graph->finalWarningSnapshotOnMain(1,warning,present) && present && warning.events==1);
    CHECK(graph->finalWarningSnapshotOnMain(2,warning,present) && !present);
    CHECK(graph->finalWarningSnapshotOnMain(0xfffffffbu,warning,present) && !present);
    CHECK(graph->acknowledgeFinalWarningsOnMain());
    CHECK(!graph->detachDrainedRuntimeOnMain()); // accepted terminal/credit must be consumed
    LLTransportCircuitCompletion completion; CHECK(facade->peekCircuitCompletion(completion));
    LLTransportCircuitCompletion again; CHECK(facade->peekCircuitCompletion(again) && again.operationId==completion.operationId);
    CHECK(!graph->detachDrainedRuntimeOnMain());
    CHECK(facade->releaseCircuitCompletion());
    CHECK(completion.operationId==1 && !graph->detachDrainedRuntimeOnMain()); // reader still borrows arena
    view={}; CHECK(facade->releaseInbound());
    CHECK(facade->inboundEmptyAfterJoinOnMain());
    CHECK(facade->ingressPending(pending) && !pending);
    CHECK(graph->detachDrainedRuntimeOnMain());
    CHECK(graph->requestStopOnMain() && graph->joinOnMain() && graph->detachDrainedRuntimeOnMain());
    ring.reset(); // retained facade/graph must not touch the old ring
    LLTransportCircuitSnapshot snapshot; CHECK(!facade->getCircuitSnapshot(enable.endpoint,snapshot));
    backlog.pendingPackets=123; CHECK(!facade->backlog(backlog) && backlog.pendingPackets==123);
    pending=true; CHECK(!facade->ingressPending(pending) && pending);
    CHECK(!facade->beginOfferOnMain()); facade->failCompletion();
    CHECK(!facade->inboundEmptyAfterJoinOnMain()); // detached facade is inert
    facade.reset(); graph.reset(); close(peer); close(fd);
}
