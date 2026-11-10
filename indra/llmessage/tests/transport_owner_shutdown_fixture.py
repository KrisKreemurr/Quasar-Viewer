#!/usr/bin/env python3
"""Extract actual message owner shutdown methods with a fake backend boundary."""
import hashlib,pathlib,sys
m=pathlib.Path(__file__).resolve().parents[1]; root=m.parents[1]; src=(m/'message.cpp').read_text()
def extract(name):
 start=src.index('bool LLMessageSystem::'+name+'('); opening=src.index('{',start); depth=0
 for i in range(opening,len(src)):
  if src[i]=='{':depth+=1
  elif src[i]=='}':
   depth-=1
   if not depth:return src[start:i+1]
 raise ValueError(name)
methods='\n'.join(extract(n) for n in ('requestTransportOwnerStopOnMain','joinTransportOwnerOnMain','detachDrainedTransportOwnerOnMain'))
prefix=r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
struct LLTransportMainFacade { virtual ~LLTransportMainFacade()=default; };
struct LLTransportControlWarningStats {};
struct Graph {
 int stops=0,joins=0, snapshots=0,acks=0,detaches=0; bool drained=false, failSnapshot=false, warnings=false;
 bool requestStopOnMain() noexcept { ++stops; return true; }
 bool joinOnMain() noexcept { ++joins; return true; }
 bool finalWarningSnapshotOnMain(unsigned,LLTransportControlWarningStats&,bool& present) const noexcept {
  auto* self=const_cast<Graph*>(this); ++self->snapshots; if(failSnapshot) return false; present=warnings; return true;
 }
 bool acknowledgeFinalWarningsOnMain() noexcept { ++acks; return true; }
 bool detachDrainedRuntimeOnMain() noexcept { ++detaches; return drained; }
};
bool pending=false; bool messagingTeardownPendingOnMain() noexcept { return pending; }
struct Registry { std::size_t count=0; std::size_t size() const { return count; } };
struct LLMessageSystem {
 std::unique_ptr<LLTransportMainFacade> mTransportMainFacade;
 std::shared_ptr<Graph> mTransportOwnerGraph;
 bool mTransportStopRequested=false,mTransportOwnerJoined=false,readers=false,mTransportDispatching=false,mTransportInboundHeld=false,mTransportInboundFatal=false;
 unsigned mTransportCallbacksExecuting=0;
 int warningLogs=0; bool failWarningLog=false;
 Registry mTransportCallbacks;
 bool hasMessageReaderUsersOnMain() const noexcept { return readers; }
 bool consumeTransportControlWarningOnMain(unsigned,const LLTransportControlWarningStats&,bool& emitted) { if(failWarningLog) return false; ++warningLogs; emitted=true; return true; }
 bool requestTransportOwnerStopOnMain() noexcept; bool joinTransportOwnerOnMain() noexcept; bool detachDrainedTransportOwnerOnMain() noexcept;
};
'''
suffix=r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed %d\n",__LINE__); std::abort(); } } while(false)
int main() {
 LLMessageSystem m; auto p=std::make_shared<Graph>(); m.mTransportOwnerGraph=p; m.mTransportMainFacade=std::make_unique<LLTransportMainFacade>();
 CHECK(m.requestTransportOwnerStopOnMain() && m.requestTransportOwnerStopOnMain() && p->stops==1);
 m.readers=true; CHECK(!m.joinTransportOwnerOnMain() && p->joins==0); m.readers=false;
 CHECK(m.joinTransportOwnerOnMain() && m.joinTransportOwnerOnMain() && p->joins==1);
 CHECK(!m.detachDrainedTransportOwnerOnMain()); pending=true; CHECK(!m.detachDrainedTransportOwnerOnMain());
 p->drained=true; m.mTransportInboundHeld=true; CHECK(!m.detachDrainedTransportOwnerOnMain()); m.mTransportInboundHeld=false;
 m.mTransportCallbacks.count=1; CHECK(!m.detachDrainedTransportOwnerOnMain()); m.mTransportCallbacks.count=0;
 m.mTransportInboundFatal=true; CHECK(!m.detachDrainedTransportOwnerOnMain()); m.mTransportInboundFatal=false;
 m.mTransportCallbacksExecuting=1; const int prior=p->snapshots; CHECK(!m.detachDrainedTransportOwnerOnMain() && p->snapshots==prior); m.mTransportCallbacksExecuting=0;
 const int priorAck=p->acks; p->failSnapshot=true; CHECK(!m.detachDrainedTransportOwnerOnMain() && p->acks==priorAck); p->failSnapshot=false;
 p->warnings=true; m.failWarningLog=true; CHECK(!m.detachDrainedTransportOwnerOnMain() && p->acks==priorAck); m.failWarningLog=false;
 CHECK(m.detachDrainedTransportOwnerOnMain() && !m.mTransportMainFacade && !m.mTransportOwnerGraph && p->acks==priorAck+1 && m.warningLogs==3);
}
'''
out=pathlib.Path(sys.argv[1]).resolve()
if not out.is_relative_to(root): raise SystemExit('output must be inside worktree')
out.parent.mkdir(parents=True,exist_ok=True);text=prefix+methods+suffix;out.write_text(text)
print('methods_sha256',hashlib.sha256(methods.encode()).hexdigest());print('fixture_sha256',hashlib.sha256(text.encode()).hexdigest())
