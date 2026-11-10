#!/usr/bin/env python3
"""Extract actual graph detach methods into a bounded fake-resource fixture."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve()
if not out.is_relative_to(root/'.validation'):
    raise SystemExit('output must be inside this worktree validation directory')
source=(root/'indra/llmessage/lltransportownergraph.cpp').read_text()
start=source.index('bool LLTransportOwnerGraph::finalWarningSnapshotOnMain(')
end=source.index('\nbool LLTransportOwnerFacade::getCircuitSnapshot(',start)
body=source[start:end]
prefix=r'''#include <cstdint>
#include <cstdlib>
#include <memory>
struct LLTransportControlWarningStats { uint64_t value=0; };
struct Queue { bool empty=true; bool emptyAfterJoinOnMain() const { return empty; } };
struct Admission { bool empty=true; bool emptyOnMain() const { return empty; } };
struct Warnings {
 bool readFinalOnMain(uint32_t kind,LLTransportControlWarningStats& out,bool& present) const {
  if (kind==1) { out.value=7; present=true; } else present=false; return true;
 }
};
struct Worker { bool failed() const { return false; } };
static int destroyed=0;
struct Runtime { Worker worker; ~Runtime() { ++destroyed; } };
struct LLTransportOwnerGraph {
 bool mActiveOnMain=true,mJoinedOnMain=true,mDetachedOnMain=false,mFinalWarningsAcknowledged=false,mFinalFailed=true;
 mutable uint8_t mFinalWarningSnapshotMask=0;
 std::unique_ptr<Runtime> mRuntime=std::make_unique<Runtime>();
 Admission mAdmission; struct Channels { Queue commands,circuitTerminals,sendTerminals,unreliableTerminals,controls,fatal; Warnings controlWarnings; } mChannels;
 Queue mIngress,mArena; std::shared_ptr<int> mWait=std::make_shared<int>(1);
 struct { int socket=1; } mConfig; bool mRingTransferred=true;
 bool finalWarningSnapshotOnMain(uint32_t,LLTransportControlWarningStats&,bool&) const noexcept;
 bool acknowledgeFinalWarningsOnMain() noexcept;
 bool detachDrainedRuntimeOnMain() noexcept;
};
'''
suffix=r'''
int main() {
 LLTransportOwnerGraph g; LLTransportControlWarningStats value; bool present=false;
 if (g.detachDrainedRuntimeOnMain() || g.acknowledgeFinalWarningsOnMain()) std::abort();
 if (!g.finalWarningSnapshotOnMain(1,value,present) || !present || value.value!=7) std::abort();
 if (!g.finalWarningSnapshotOnMain(2,value,present) || present || value.value!=7) std::abort();
 if (!g.finalWarningSnapshotOnMain(0xfffffffbu,value,present) || present) std::abort();
 if (!g.acknowledgeFinalWarningsOnMain()) std::abort();
 Queue* queues[]={&g.mChannels.commands,&g.mChannels.circuitTerminals,&g.mChannels.sendTerminals,
  &g.mChannels.unreliableTerminals,&g.mChannels.controls,&g.mChannels.fatal,&g.mIngress,&g.mArena};
 for (auto* q:queues) { q->empty=false; if (g.detachDrainedRuntimeOnMain()) std::abort(); q->empty=true; }
 g.mAdmission.empty=false; if (g.detachDrainedRuntimeOnMain()) std::abort(); g.mAdmission.empty=true;
 if (!g.detachDrainedRuntimeOnMain() || g.mActiveOnMain || g.mRuntime || g.mConfig.socket!=-1 || destroyed!=1) std::abort();
 if (!g.detachDrainedRuntimeOnMain() || destroyed!=1) std::abort();
}
'''
out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(prefix+body+suffix)
