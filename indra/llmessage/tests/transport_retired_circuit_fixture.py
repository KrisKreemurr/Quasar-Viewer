#!/usr/bin/env python3
"""Extract actual main close/terminal/shutdown and world peer methods.

The fixture links actual LLTransportCircuitState. Socket/command publication,
viewer sends and inbound handler execution are explicit fake boundaries.
Generation alone never invokes a compiler.
"""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
if not out.is_relative_to(root / '.validation'):
    raise SystemExit('output must be in this worktree validation directory')
message = (root / 'indra/llmessage/message.cpp').read_text()
header = (root / 'indra/llmessage/message.h').read_text()
world = (root / 'indra/newview/llworld.cpp').read_text()
world_header = (root / 'indra/newview/llworld.h').read_text()


def method(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if not depth:
                return source[start:i + 1]
    raise ValueError(signature)


signatures = [
    'bool LLMessageSystem::transportCircuitRetiredOnMain(',
    'void LLMessageSystem::cancelTransportPeerEnableOnMain(',
    'void LLMessageSystem::retireTransportCircuitOnMain(',
    'LLMessageSystem::TransportRetiredTerminal LLMessageSystem::consumeTransportRetiredTerminalOnMain(',
    'uint32_t LLMessageSystem::serviceTransportRetiredCircuitsOnMain(',
    'LLTransportCommandOffer LLMessageSystem::submitTransportCircuitCommand(',
    'LLTransportCommandOffer LLMessageSystem::requestTransportEnableOnMain(',
    'bool LLMessageSystem::peekTransportCircuitCompletionOnMain(',
    'bool LLMessageSystem::releaseTransportCircuitCompletionOnMain(',
    'LLMessageSystem::TransportShutdownStep LLMessageSystem::drainTransportShutdownOnMain(',
]
actual = '\n'.join(method(message, s) for s in signatures)
actual_world = '\n'.join(method(world, s) for s in [
    'void LLWorld::cancelTransportPeerContinuationsHook(',
    'void LLWorld::pumpTransportPeerContinuations(',
])
records = header[header.index('    struct TransportEnableOwner'):header.index('    bool mTransportPingBasedInitial')]
timeouts = header[header.index('    struct TransportTimeoutCallback'):header.index('    bool mTransportTimeoutDispatching')]
peers = world_header[world_header.index('    struct TransportPeerContinuation'):world_header.index('    void clearHoleWaterObjects')]

prefix = r'''
#include "lltransportcircuitstate.h"
#include "lltransportmainfacade.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
using U32=uint32_t; using U64=uint64_t;
struct LLUUID {};
struct LLHost {
 uint32_t address=0,port=0;
 LLHost()=default; LLHost(uint32_t a,uint32_t p):address(a),port(p){}
 uint32_t getAddress()const{return address;} uint32_t getPort()const{return port;}
 bool isOk()const{return address&&port&&port<=65535;}
 bool operator==(const LLHost&)const=default;
};
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::abort();} } while(false)
struct Log { template<class T> Log& operator<<(const T&){return *this;} };
#define LL_WARNS_ONCE(...) Log()
#define LL_ENDL 0
constexpr int _PREHASH_UseCircuitCode=1,_PREHASH_CircuitCode=2,_PREHASH_Code=3,_PREHASH_SessionID=4,
 _PREHASH_ID=5,_PREHASH_CompleteAgentMovement=6,_PREHASH_AgentData=7,_PREHASH_AgentID=8;
namespace LLCrossingTrace { void onCompleteMovementSent(){} }
struct LLTransportOwnerFatal {};
struct LLTransportOwnerControl {
 enum class Kind {Timeout}; Kind kind=Kind::Timeout; LLTransportEndpoint endpoint{};
 uint64_t loginGeneration=0,circuitIncarnation=0,eventId=0,lastPublishedAppSequence=0;
};
static bool pendingTeardown=false;
bool messagingTeardownPendingOnMain(){return pendingTeardown;}
struct Facade final : LLTransportMainFacade {
 LLTransportCircuitState state; uint64_t now=100,published=0; bool full=false,unavailable=false,failed=false;
 std::deque<LLTransportCircuitCommand> commands; std::deque<LLTransportCircuitCompletion> terminals;
 Facade(){uint64_t bytes=0;CHECK(state.init({4,16,64,64,16,64,8388608},bytes));}
 bool getCircuitSnapshot(LLTransportEndpoint e,LLTransportCircuitSnapshot& out)const noexcept override {
  if(unavailable){return false;}out=state.lookup(e,now);return true;
 }
 LLTransportCommandOffer submitCircuitCommand(const LLTransportCircuitCommand& c)noexcept override {
  if(unavailable){return LLTransportCommandOffer::Unavailable;}if(full){return LLTransportCommandOffer::Full;}
  commands.push_back(c);return LLTransportCommandOffer::Accepted;
 }
 bool peekCircuitCompletion(LLTransportCircuitCompletion& out)noexcept override {
  if(terminals.empty()){return false;}out=terminals.front();return true;
 }
 bool releaseCircuitCompletion()noexcept override {if(terminals.empty())return false;terminals.pop_front();return true;}
 bool popCircuitCompletion(LLTransportCircuitCompletion& out)noexcept override {
  return peekCircuitCompletion(out)&&releaseCircuitCompletion();
 }
 void failCompletion()noexcept override {failed=true;}
 void apply(bool stopping=false) {
  while(!commands.empty()) {
   const auto c=commands.front();commands.pop_front();LLTransportCircuitCompletion t;
   t.endpoint=c.endpoint;t.operationId=c.operationId;t.loginGeneration=c.loginGeneration;t.circuitIncarnation=c.targetIncarnation;
   if(stopping){t.status=LLTransportCircuitCommandStatus::Cancelled;t.lastPublishedAppSequence=published;}
   else if(c.kind==LLTransportCircuitCommandKind::Enable){
    const auto r=state.enable(c.endpoint,c.loginGeneration,c.trusted,now,{1000000,10000000,0.f,0.f});
    CHECK(r.status==LLTransportCircuitState::Status::Ok);t.status=LLTransportCircuitCommandStatus::Applied;t.circuitIncarnation=r.incarnation;
   }else {CHECK(c.kind==LLTransportCircuitCommandKind::Disable);t=state.disable(c,published);}
   terminals.push_back(t);
  }
  if(stopping)state.shutdown();
 }
 uint64_t enable(LLTransportEndpoint e) {
  auto r=state.enable(e,7,true,now,{1000000,10000000,0.f,0.f});CHECK(r.status==LLTransportCircuitState::Status::Ok);return r.incarnation;
 }
};
struct LLMessageSystem {
 std::unique_ptr<Facade> mTransportMainFacade=std::make_unique<Facade>();
 uint64_t mTransportLoginGeneration=7,mTransportNextCircuitOperation=1,mTransportLastReleasedAppSequence=0;
RECORDS
TIMEOUTS
 using TransportCircuitRetireHook=void(*)(const LLHost&,void*)noexcept;
 TransportCircuitRetireHook mTransportCircuitRetireHook=nullptr;void* mTransportCircuitRetireContext=nullptr;
 bool mTransportStopRequested=false,mTransportOwnerJoined=false,mTransportInboundFatal=false,mTransportDispatching=false,mTransportTimeoutDispatching=false;
 uint32_t mTransportCallbacksExecuting=0,sends=0;std::shared_ptr<uint8_t> mTransportCallbackLifetime;
 struct Registry{size_t size()const{return 0;}}mTransportCallbacks;
 bool hasMessageReaderUsersOnMain()const{return false;}
 bool transportOwnerActive()const{return bool(mTransportMainFacade);}
 uint64_t transportLoginGenerationOnMain()const{return mTransportLoginGeneration;}
 bool transportCircuitRetiringOnMain(const LLHost& h)const{return transportCircuitRetiredOnMain({h.getAddress(),static_cast<uint16_t>(h.getPort())});}
 LLTransportCommandOffer submitTransportCircuitCommand(const LLTransportCircuitCommand&)noexcept;
 LLTransportCommandOffer requestTransportEnableOnMain(const LLHost&,bool,uint64_t&)noexcept;
 void cancelTransportPeerEnableOnMain(const LLHost&,uint64_t)noexcept;
 bool peekTransportCircuitCompletionOnMain(LLTransportCircuitCompletion&)noexcept;
 bool releaseTransportCircuitCompletionOnMain()noexcept;
 enum class TransportShutdownStep{Progress,Done,Blocked,Fatal};
 TransportShutdownStep drainTransportShutdownOnMain();
 enum class ShutdownInboundStep{Progress,Empty,Barrier,Retry,Fatal,Unavailable};
 ShutdownInboundStep dispatchTransportInboundForShutdownOnMain(uint64_t barrier){
  const auto through=std::min(barrier,mTransportMainFacade->published);
  if(mTransportLastReleasedAppSequence<through){++mTransportLastReleasedAppSequence;return ShutdownInboundStep::Progress;}
  return ShutdownInboundStep::Empty;
 }
 uint32_t pumpTransportCompletions(uint32_t){return 0;}
 bool detachDrainedTransportOwnerOnMain(){
  for(const auto& r:mTransportRetiredCircuits)if(r.generation)return false;
  for(const auto& r:mTransportEnableOwners)if(r.operation)return false;
  return mTransportMainFacade->commands.empty()&&mTransportMainFacade->terminals.empty();
 }
 void newMessageFast(int){}void nextBlockFast(int){}void addU32Fast(int,U32){}void addUUIDFast(int,const LLUUID&){}
 int sendReliable(const LLHost&){++sends;return 1;}
};
LLMessageSystem* gMessageSystem=nullptr;
struct LLWorld {
PEERS
 static LLWorld* instance;
 static bool instanceExists(){return instance!=nullptr;}static LLWorld* getInstance(){return instance;}
 static void cancelTransportPeerContinuationsHook(const LLHost&,void*)noexcept;
 void pumpTransportPeerContinuations();
};
LLWorld* LLWorld::instance=nullptr;
'''.replace('RECORDS', records).replace('TIMEOUTS', timeouts).replace('PEERS', peers)

suffix = r'''
static LLTransportEndpoint endpoint{0x0100007f,1234};static LLHost host{endpoint.addressNetworkOrder,endpoint.portHostOrder};
static void drain(LLMessageSystem& m){
 pendingTeardown=true;m.mTransportStopRequested=true;m.mTransportOwnerJoined=true;m.mTransportMainFacade->apply(true);
 for(unsigned n=0;n<100;++n){const auto s=m.drainTransportShutdownOnMain();if(s==LLMessageSystem::TransportShutdownStep::Done){pendingTeardown=false;return;}CHECK(s==LLMessageSystem::TransportShutdownStep::Progress);}
 CHECK(false);
}
int main(){
 // Full and unavailable must retain the close. Accepted terminal holds its APP
 // barrier; new same-host Enable cannot overtake it, even after owner deletion.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;const auto old=f.enable(endpoint);f.full=true;
  m.mTransportTimeoutCallbacks[0]={host,7,old,[](const LLHost&,void*){},reinterpret_cast<void*>(1)};
  m.retireTransportCircuitOnMain(host);CHECK(m.transportCircuitRetiringOnMain(host));CHECK(!m.mTransportTimeoutCallbacks[0].callback);
  uint64_t op=0;CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Full&&op==0);CHECK(f.commands.empty());
  f.full=false;f.unavailable=true;m.serviceTransportRetiredCircuitsOnMain(64);CHECK(f.commands.empty()&&m.transportCircuitRetiringOnMain(host));
  f.unavailable=false;m.serviceTransportRetiredCircuitsOnMain(64);CHECK(f.commands.size()==1&&f.commands.front().targetIncarnation==old);
  f.published=7;f.apply();m.serviceTransportRetiredCircuitsOnMain(64);CHECK(m.transportCircuitRetiringOnMain(host));
  CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Full);
  m.mTransportLastReleasedAppSequence=7;m.serviceTransportRetiredCircuitsOnMain(64);CHECK(!m.transportCircuitRetiringOnMain(host));
  CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Accepted);f.apply();CHECK(f.state.lookup(endpoint,f.now).incarnation!=old);
 }
 // CloseCircuit direct entry: cancel both accepted Enable and operation-zero
 // world jobs BEFORE consuming their Enable terminal. No later resurrection.
 {
  LLMessageSystem m;LLWorld w;LLWorld::instance=&w;gMessageSystem=&m;m.mTransportCircuitRetireHook=LLWorld::cancelTransportPeerContinuationsHook;
  auto& f=*m.mTransportMainFacade;uint64_t op=0;CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Accepted);
  w.mTransportPeerCount=2;for(unsigned i=0;i<2;++i){w.mTransportPeers[i].used=true;w.mTransportPeers[i].host=host;w.mTransportPeers[i].loginGeneration=7;w.mTransportPeers[i].useCircuit=true;}
  w.mTransportPeers[0].operation=op;
  m.retireTransportCircuitOnMain(host);CHECK(w.mTransportPeers[0].cancelled&&w.mTransportPeers[1].cancelled);
  f.apply();m.serviceTransportRetiredCircuitsOnMain(64);CHECK(f.terminals.empty()&&f.commands.size()==1);
  f.apply();m.serviceTransportRetiredCircuitsOnMain(64);CHECK(!m.transportCircuitRetiringOnMain(host));
  w.pumpTransportPeerContinuations();CHECK(w.mTransportPeerCount==0&&m.sends==0&&f.commands.empty());
  LLWorld::instance=nullptr;gMessageSystem=nullptr;
 }
 // Startup's first Enable is not owned by world: its consumer must still see
 // the terminal and retiring=true, release it and take the fail-login branch.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;uint64_t op=0;
  CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Accepted);
  m.retireTransportCircuitOnMain(host);f.apply();m.serviceTransportRetiredCircuitsOnMain(64);
  LLTransportCircuitCompletion t;CHECK(m.peekTransportCircuitCompletionOnMain(t)&&t.operationId==op);
  CHECK(m.transportCircuitRetiringOnMain(host));CHECK(m.releaseTransportCircuitCompletionOnMain());
  m.serviceTransportRetiredCircuitsOnMain(64);CHECK(f.commands.size()==1);drain(m);
 }
 // A stale close targets its original incarnation; authoritative owner Disable
 // cannot close a replacement, even if the packet/publication token changed.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;const auto old=f.enable(endpoint);
  m.retireTransportCircuitOnMain(host);CHECK(f.commands.size()==1);
  CHECK(f.state.disable(endpoint,old,0).status==LLTransportCircuitCommandStatus::Applied);
  const auto replacement=f.enable(endpoint);CHECK(replacement!=old);f.apply();m.serviceTransportRetiredCircuitsOnMain(64);
  CHECK(f.state.lookup(endpoint,f.now).incarnation==replacement&&!m.transportCircuitRetiringOnMain(host));
 }
 // Stop/join cancels unadmitted Full closes without requiring another command.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;f.enable(endpoint);f.full=true;m.retireTransportCircuitOnMain(host);
  CHECK(f.commands.empty()&&m.transportCircuitRetiringOnMain(host));drain(m);CHECK(!m.transportCircuitRetiringOnMain(host));
 }
 // Accepted close and pending Enable both settle using existing terminals;
 // the real shutdown coordinator first advances its retained APP barrier.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;f.enable(endpoint);m.retireTransportCircuitOnMain(host);f.published=5;
  CHECK(f.commands.size()==1);drain(m);CHECK(m.mTransportLastReleasedAppSequence==5);
 }
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;uint64_t op=0;CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Accepted);
  m.retireTransportCircuitOnMain(host);drain(m);CHECK(f.terminals.empty());
 }
 // Never steal an unrelated Enable terminal at the shared FIFO head.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;uint64_t op=0;CHECK(m.requestTransportEnableOnMain(host,true,op)==LLTransportCommandOffer::Accepted);
  f.apply();m.serviceTransportRetiredCircuitsOnMain(64);CHECK(f.terminals.size()==1&&f.terminals.front().operationId==op);
 }
 // Exhaustion is explicit fail-closed, not a lost best-effort close.
 {
  LLMessageSystem m;auto& f=*m.mTransportMainFacade;f.enable(endpoint);
  for(auto& item:m.mTransportRetiredCircuits){item.endpoint={2,3};item.generation=7;}
  m.retireTransportCircuitOnMain(host);CHECK(m.mTransportInboundFatal&&f.failed);
 }
 std::puts("actual retained-close/world/terminal/shutdown methods + real circuit state: PASS");
}
'''
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(prefix + actual + actual_world + suffix)
print('actual_methods_sha256', hashlib.sha256((actual + actual_world).encode()).hexdigest())
print('fixture_sha256', hashlib.sha256(out.read_bytes()).hexdigest())
