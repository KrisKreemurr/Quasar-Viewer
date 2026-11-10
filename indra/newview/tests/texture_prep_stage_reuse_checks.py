#!/usr/bin/env python3
"""Exercise the production texture-prep retired-stage reuse/accounting helpers.

The fixture extracts the exact Impl helpers. It models Vulkan allocation failure at
the VkMemoryRequirements boundary; it is not a Vulkan-driver test.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys


def extract(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"missing production helper: {marker}")
    brace = source.find("{", start)
    depth = 0
    state = "code"
    i = brace
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and n == "/": state = "line"; i += 2; continue
            if c == "/" and n == "*": state = "block"; i += 2; continue
            if c == '"': state = "string"
            elif c == "'": state = "char"
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0: return source[start:i + 1]
        elif state == "line":
            if c == "\n": state = "code"
        elif state == "block":
            if c == "*" and n == "/": state = "code"; i += 2; continue
        elif state in ("string", "char"):
            if c == "\\": i += 2; continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
        i += 1
    raise RuntimeError(f"unterminated production helper: {marker}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--run", action="store_true")
    args = ap.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if out == root or root not in out.parents:
        raise SystemExit("output must be a dedicated directory under the owned source root")
    source_path = root / "indra/newview/vkgfx/llvktextureprepservice.cpp"
    source = source_path.read_text()
    markers = (
        "int findCachedStage(VkDeviceSize required, uint64_t activeLimit) const noexcept",
        "void destroyCachedStage(uint32_t index) noexcept",
        "void makeStageRoom(VkDeviceSize required) noexcept",
        "void evictCachedStages() noexcept",
        "void keepStage(GpuBuffer& buffer) noexcept",
        "void freeSlot(Slot& slot) noexcept",
    )
    methods = "\n".join(extract(source, marker) for marker in markers)
    submit = source[source.find("LLVKTexturePrepService::submit("):]
    if submit.count("mImpl->evictCachedStages();") < 2:
        raise RuntimeError("bounded allocation failure paths must evict idle stage buffers")
    if "candidate.allocationSize > activeLimit - activeStageUsed" not in source:
        raise RuntimeError("reuse must observe the active priority lane cap")
    generated = r'''#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using VkDeviceSize = uint64_t;
struct GpuBuffer { bool buffer=false; uint64_t size=0, allocationSize=0; void* mapped=nullptr; };
struct VkDeviceContext { uint64_t destroyed=0; };
void destroyBufferNoThrow(VkDeviceContext* context, GpuBuffer& buffer) noexcept {
  if (!context || !buffer.buffer) return;
  context->destroyed += buffer.allocationSize;
  buffer = GpuBuffer{};
}
enum class State : uint8_t { Queued, Running, Ready, Recorded, Quarantined, Submitted, Failed, Cancelled };
struct Slot {
  uint32_t generation=1; bool retired=false, recordPending=false, quarantined=false;
  uint64_t retirementSerial=0, stageBytes=0; State state=State::Ready; GpuBuffer stage{};
};
struct CachedStage { GpuBuffer buffer{}; uint64_t lastUsed=0; };
struct Impl {
  static constexpr uint32_t kTicketCount=4;
  static constexpr uint64_t kStageLimit=256ull<<20;
  VkDeviceContext* context=nullptr;
  std::array<CachedStage,kTicketCount> cachedStages{};
  uint64_t stageUsed=0, activeStageUsed=0, stageCacheClock=0;
  State stateOf(const Slot& slot) const noexcept { return slot.state; }
  static bool workerDone(State state) noexcept {
    return state==State::Ready || state==State::Recorded || state==State::Quarantined
      || state==State::Submitted || state==State::Failed || state==State::Cancelled;
  }
  void releaseInput(Slot&) noexcept {}
'''+methods+r'''
};
int main() {
  constexpr uint64_t MiB=1ull<<20;
  VkDeviceContext context;
  Impl impl; impl.context=&context;

  // A slot is not reusable until worker completion and all GPU-use guards clear.
  Slot active; active.state=State::Ready; active.stageBytes=4*MiB;
  active.stage={true,4*MiB,4*MiB,reinterpret_cast<void*>(1)};
  impl.stageUsed=impl.activeStageUsed=4*MiB;
  active.recordPending=true; impl.freeSlot(active);
  assert(active.stage.buffer && !impl.cachedStages[0].buffer.buffer);
  active.recordPending=false; active.retirementSerial=9; impl.freeSlot(active);
  assert(active.stage.buffer && impl.activeStageUsed==4*MiB);
  active.retirementSerial=0; active.state=State::Running; impl.freeSlot(active);
  assert(active.stage.buffer && impl.activeStageUsed==4*MiB);
  active.state=State::Ready; impl.freeSlot(active);
  assert(!active.stage.buffer && impl.activeStageUsed==0 && impl.stageUsed==4*MiB);

  // Best-fit reuse respects both logical buffer size and the active lane cap.
  impl.cachedStages[1].buffer={true,8*MiB,10*MiB,reinterpret_cast<void*>(1)};
  impl.cachedStages[1].lastUsed=2; impl.stageUsed+=10*MiB;
  impl.cachedStages[2].buffer={true,6*MiB,7*MiB,reinterpret_cast<void*>(1)};
  impl.cachedStages[2].lastUsed=3; impl.stageUsed+=7*MiB;
  assert(impl.findCachedStage(5*MiB,64*MiB)==2);
  assert(impl.findCachedStage(5*MiB,5*MiB)==-1);

  // Near-cap alignment case: logical 5 MiB fits in 6 MiB, but a driver requirement
  // rounded to 8 MiB does not. The first room check keeps cache; failed bounded
  // allocation then evicts idle buffers so the next frame has the full headroom.
  Impl nearCap; nearCap.context=&context;
  nearCap.stageUsed=Impl::kStageLimit-6*MiB;
  nearCap.activeStageUsed=Impl::kStageLimit-8*MiB;
  nearCap.cachedStages[0].buffer={true,2*MiB,2*MiB,reinterpret_cast<void*>(1)};
  nearCap.cachedStages[0].lastUsed=1;
  nearCap.makeStageRoom(5*MiB);
  assert(nearCap.cachedStages[0].buffer.buffer);
  assert(8*MiB > Impl::kStageLimit-nearCap.stageUsed); // injected rounded requirement fails
  nearCap.evictCachedStages();
  assert(!nearCap.cachedStages[0].buffer.buffer);
  assert(Impl::kStageLimit-nearCap.stageUsed==8*MiB);
  assert(context.destroyed>=2*MiB);
  std::cout << "stage reuse lifecycle, best-fit, lane cap, and near-cap recovery passed\n";
}
'''
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / "texture_prep_stage_reuse_checks.cpp"
    cpp.write_text(generated)
    print(f"service_sha256={hashlib.sha256(source_path.read_bytes()).hexdigest()}")
    print(f"fixture_sha256={hashlib.sha256(cpp.read_bytes()).hexdigest()}")
    if not args.run:
        print(f"generated={cpp}")
        return 0
    exe = out / "texture_prep_stage_reuse_checks"
    cmd = ["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)]
    result = subprocess.run(cmd, cwd=out, check=False)
    if result.returncode: return result.returncode
    return subprocess.run([str(exe)], cwd=out, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
