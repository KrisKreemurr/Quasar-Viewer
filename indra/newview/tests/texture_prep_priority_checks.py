#!/usr/bin/env python3
"""Exercise exact service admission code with a background-first flood.

Constants, Priority, slot selection and byte refusal come from production source.
The fixture supplies occupancy/byte ledgers and commits accepted charges; it does
not run workers, allocate Vulkan buffers, or prove GPU fence completion.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--run", action="store_true")
    args = ap.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if out == root or root not in out.parents:
        raise SystemExit("output must be a dedicated directory under the owned source root")
    service = root / "indra/newview/vkgfx/llvktextureprepservice.cpp"
    header = root / "indra/newview/vkgfx/llvktextureprepservice.h"
    contract = root / "indra/newview/vkgfx/llvktextureprep.h"
    source = service.read_text()
    constants = "\n".join(re.findall(r"    static constexpr uint(?:32|64)_t k\w+ = [^;]+;", header.read_text()))
    priority = re.search(r"enum class Priority : uint8_t\s*\{[^}]+\};", contract.read_text())
    if not constants or not priority:
        raise RuntimeError("production constants or priority definition missing")
    start = source.index("    uint32_t slotIndex = kTicketCount;", source.index("LLVKTexturePrepService::submit("))
    end = source.index("    Impl::Slot& slot = mImpl->slots[slotIndex];", start)
    admission = source[start:end]
    generated = r'''#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
void require(bool value, const char* name) {
  if (!value) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
'''+priority.group()+r'''
enum class QueueResult { Accepted, Deferred };
struct Service {
'''+constants+r'''
  struct Slot { bool used=false, retired=false; uint32_t generation=0; };
  struct Impl {
    std::array<Slot,kTicketCount> slots{};
    uint64_t inputUsed=0, activeStageUsed=0, scratchUsed=0;
  } ledger;
  Impl* mImpl=&ledger;
  uint32_t selected=kTicketCount;
  QueueResult submit(Priority priority, uint64_t inputCharge=1,
                     uint64_t stageCharge=1, uint64_t scratchCharge=1) {
    struct { uint64_t outputBytes, scratchBytes; } workspace{stageCharge,scratchCharge};
'''+admission+r'''
    // Endpoint model: production has already checked every shared ledger above.
    selected=slotIndex;
    auto& slot=mImpl->slots[slotIndex];
    slot.used=true; ++slot.generation;
    mImpl->inputUsed+=inputCharge;
    mImpl->activeStageUsed+=workspace.outputBytes;
    mImpl->scratchUsed+=workspace.scratchBytes;
    return QueueResult::Accepted;
  }
};
int main() {
  constexpr uint64_t MiB=1ull<<20;
  constexpr auto accepted=QueueResult::Accepted, deferred=QueueResult::Deferred;
  Service service;
  require(Service::kTicketCount==128 && Service::kPriorityReserve==8, "original total/self caps");
  for (unsigned i=0; i<96; ++i) {
    require(service.submit(Priority::Background)==accepted, "background capacity");
    require(service.selected==i, "background shared partition");
  }
  for (unsigned i=0; i<256; ++i)
    require(service.submit(Priority::Background)==deferred, "background flood leaves visible reserve");
  for (unsigned i=96; i<120; ++i) {
    require(service.submit(Priority::Visible)==accepted, "visible admitted after background flood");
    require(service.selected==i, "visible partition");
  }
  require(service.submit(Priority::Visible)==deferred, "visible cannot consume self reserve");
  for (unsigned i=120; i<128; ++i) {
    require(service.submit(Priority::SelfOrHud)==accepted, "self admitted after world flood");
    require(service.selected==i, "self partition");
  }
  require(service.submit(Priority::SelfOrHud)==deferred, "total ticket bound");
  require(service.ledger.inputUsed==128 && service.ledger.activeStageUsed==128
          && service.ledger.scratchUsed==128, "refusals do not charge or evict occupied slots");

  Service visibleFirst;
  for (unsigned i=0; i<120; ++i)
    require(visibleFirst.submit(Priority::Visible)==accepted, "visible borrows shared slots");
  require(visibleFirst.submit(Priority::Visible)==deferred, "visible borrowing stays bounded");
  Service selfFirst;
  for (unsigned i=0; i<128; ++i)
    require(selfFirst.submit(Priority::SelfOrHud)==accepted, "self borrows world slots");
  require(selfFirst.submit(Priority::SelfOrHud)==deferred, "self borrowing stays bounded");
  Service retired;
  retired.ledger.slots[96].retired=true;
  retired.ledger.slots[97].generation=(std::numeric_limits<uint32_t>::max)();
  require(retired.submit(Priority::Visible)==accepted && retired.selected==98,
          "retired/exhausted generations are unavailable");

  // Independently fill each byte ledger with background before any visible work.
  const uint64_t backgroundCaps[]={32*MiB,64*MiB,32*MiB};
  const uint64_t visibleCaps[]={64*MiB,128*MiB,64*MiB};
  const uint64_t totalCaps[]={Service::kInputLimit,Service::kStageLimit,Service::kScratchLimit};
  for (unsigned lane=0; lane<3; ++lane) {
    Service bytes;
    uint64_t input=1, stage=1, scratch=1;
    (lane==0 ? input : lane==1 ? stage : scratch)=backgroundCaps[lane];
    require(bytes.submit(Priority::Background,input,stage,scratch)==accepted, "background byte boundary");
    require(bytes.submit(Priority::Background)==deferred, "background bytes preserve visible headroom");
    require(bytes.submit(Priority::Visible)==accepted, "visible bytes admitted after background saturation");
    require(bytes.submit(Priority::SelfOrHud)==accepted, "self bytes admitted after background saturation");

    Service upper;
    auto& ledger=upper.ledger;
    uint64_t& used=lane==0 ? ledger.inputUsed : lane==1 ? ledger.activeStageUsed : ledger.scratchUsed;
    used=visibleCaps[lane];
    require(upper.submit(Priority::Visible)==deferred, "existing visible byte cap");
    require(upper.submit(Priority::SelfOrHud)==accepted, "existing self byte reserve");
    used=totalCaps[lane];
    require(upper.submit(Priority::SelfOrHud)==deferred, "original aggregate byte cap");
    used=(std::numeric_limits<uint64_t>::max)();
    require(upper.submit(Priority::Background)==deferred
            && upper.submit(Priority::Visible)==deferred
            && upper.submit(Priority::SelfOrHud)==deferred, "over-limit subtraction cannot wrap");
  }
  std::cout << "background flood -> 24 visible + 8 self tickets; byte reserves and bounds passed\n";
}
'''
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / "texture_prep_priority_checks.cpp"
    cpp.write_text(generated)
    for path in (service, header, contract, cpp):
        print(f"{path.name}_sha256={hashlib.sha256(path.read_bytes()).hexdigest()}", flush=True)
    if not args.run:
        print(f"generated={cpp}")
        return 0
    exe = out / "texture_prep_priority_checks"
    cmd = ["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)]
    result = subprocess.run(cmd, cwd=out, check=False)
    if result.returncode:
        return result.returncode
    return subprocess.run([str(exe)], cwd=out, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
