#!/usr/bin/env python3
"""Extract the actual worker stop body into a finite fake-loop boundary fixture.

Generation only: no compiler, subprocess, or temporary files are launched.
Pass an output path inside the caller's owned validation directory.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
if not out.is_relative_to(root / '.validation'):
    raise SystemExit('output must be inside this worktree validation directory')
source = (root / 'indra/llmessage/lltransportownerworker.cpp').read_text()
start = source.index('void LLTransportOwnerWorker::settleForStop() noexcept')
end = source.index('\nvoid LLTransportOwnerWorker::run()', start)
body = source[start:end]
prefix = r'''#include <atomic>
#include <cstdlib>
#include <vector>
struct LLTransportOwnerLoop {
 enum class Step { Idle, Progress, Blocked, Poisoned };
 std::vector<int> calls; bool failCancel=false, failQueued=false; unsigned completions=1;
 void closeAdmissionForStop() { calls.push_back(1); }
 Step cancelOneUnreliableForStop() { calls.push_back(3); return failCancel ? Step::Blocked : Step::Idle; }
 void cancelInFlightForStop() { calls.push_back(4); }
 Step settleQueuedForStop() { calls.push_back(5); return failQueued ? Step::Poisoned : Step::Idle; }
 Step publishReliableCompletion() { calls.push_back(6); return completions-- ? Step::Progress : Step::Idle; }
};
struct Directory { std::vector<int>& calls; void failClosedOnOwner() { calls.push_back(2); } };
struct LLTransportOwnerWorker {
 LLTransportOwnerLoop mLoop; Directory mDirectory{mLoop.calls}; std::atomic<bool> mFailed{false};
 void settleForStop() noexcept;
};
'''
suffix = r'''
int main() {
 for (int mode=0;mode<3;++mode) {
  LLTransportOwnerWorker w; w.mLoop.failCancel=mode==1; w.mLoop.failQueued=mode==2;
  w.settleForStop();
  if (w.mLoop.calls != std::vector<int>{1,2,3,4,5,6,6} || w.mFailed.load() != (mode!=0)) std::abort();
 }
}
'''
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(prefix + body + suffix)
