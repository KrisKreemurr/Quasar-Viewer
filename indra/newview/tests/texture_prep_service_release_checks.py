#!/usr/bin/env python3
"""Extract and exercise production service release accounting.

The generated C++ uses the exact workerDone() and releaseInput() methods from
llvktextureprepservice.cpp. The small fixture owns only the service state/lease
endpoints; it is not a Vulkan integration test.

Usage (once a CPU validation slot is granted):
  python3 <source>/indra/newview/tests/texture_prep_service_release_checks.py \
      --source-root <source> --out <owned-validation-dir> --run
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
        raise RuntimeError(f"missing production method: {marker}")
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
    raise RuntimeError(f"unterminated production method: {marker}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--run", action="store_true")
    args = ap.parse_args()
    root = args.source_root.resolve()
    out = args.out.resolve()
    if out == root or root not in out.parents:
        raise SystemExit("output must be a dedicated directory under the owned source root")
    source_path = root / "indra/newview/vkgfx/llvktextureprepservice.cpp"
    source = source_path.read_text()
    done = extract(source, "static bool workerDone(State state) noexcept")
    release = extract(source, "void releaseInput(Slot& slot) noexcept")
    stop = extract(source, "void LLVKTexturePrepService::stopAdmissions() noexcept")
    gate = "if (!mImpl->accepting || mImpl->closing) return QueueResult::Deferred;"
    if gate not in source:
        raise RuntimeError("submit admission refusal guard changed; update the exact-source fixture")
    stop_body = stop[stop.find("{"):]
    generated = r'''#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

enum class State : uint8_t { Ready, Recorded, Quarantined, Submitted, Failed, Cancelled };
struct Work { std::atomic<uint8_t> state{static_cast<uint8_t>(State::Ready)}; };
namespace LLVKTexturePrep { struct SourceSnapshot { int value = 1; }; }
using LLVKTexturePrep::SourceSnapshot;
struct Lease { unsigned resets = 0; void reset() noexcept { ++resets; } };
enum class QueueResult { Accepted, Deferred };
struct Impl { std::mutex mutex; bool accepting = true, closing = false; unsigned accepted = 0; };
struct Slot {
  bool inputReleased = false;
  uint64_t inputBytes = 0, scratchBytes = 0;
  SourceSnapshot source{};
  Lease lease{};
  std::shared_ptr<Work> work = std::make_shared<Work>();
  std::unique_ptr<uint8_t[]> scratch;
};
struct Service {
  uint64_t inputUsed = 0, scratchUsed = 0;
  std::unique_ptr<Impl> mImpl = std::make_unique<Impl>();
  static State stateOf(const Slot& slot) noexcept {
    return slot.work ? static_cast<State>(slot.work->state.load(std::memory_order_acquire)) : State::Failed;
  }
  // Metrics are outside this fixture's release-accounting scope.
  void collectMetrics(Slot&) noexcept {}
'''
    generated += "  void stopAdmissions() noexcept " + stop_body + "\n"
    generated += done + "\n" + release + r'''
};

QueueResult submitGate(Impl* mImpl) {
'''+gate+r'''
  ++mImpl->accepted;
  return QueueResult::Accepted;
}

int main() {
  Service service;
  const unsigned before = service.mImpl->accepted;
  service.stopAdmissions();
  assert(!service.mImpl->accepting);
  assert(submitGate(service.mImpl.get()) == QueueResult::Deferred);
  assert(service.mImpl->accepted == before); // teardown refuses without growing admission

  Slot slot;
  slot.inputBytes = 128; slot.scratchBytes = 64;
  slot.scratch.reset(new uint8_t[64]);
  service.inputUsed = slot.inputBytes; service.scratchUsed = slot.scratchBytes;

  // Mirror the actual transition order: Ready -> Recorded (noteRecorded releases the
  // input) -> Submitted (markRecorded attaches the fence). A later reap/release is
  // idempotent and cannot strand either byte counter.
  assert(Service::workerDone(Service::stateOf(slot)));
  slot.work->state.store(static_cast<uint8_t>(State::Recorded), std::memory_order_release);
  service.releaseInput(slot);
  assert(slot.inputReleased && service.inputUsed == 0 && service.scratchUsed == 0);
  assert(slot.lease.resets == 1 && !slot.scratch);
  slot.work->state.store(static_cast<uint8_t>(State::Submitted), std::memory_order_release);
  assert(Service::workerDone(Service::stateOf(slot)));
  service.releaseInput(slot);
  assert(slot.lease.resets == 1 && service.inputUsed == 0 && service.scratchUsed == 0);

  std::cout << "submitted-ticket release accounting passed\n";
}
'''
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / "texture_prep_service_release_checks.cpp"
    cpp.write_text(generated)
    print(f"source_sha256={hashlib.sha256(source_path.read_bytes()).hexdigest()}")
    print(f"generated_sha256={hashlib.sha256(cpp.read_bytes()).hexdigest()}")
    if not args.run:
        print(f"generated={cpp}")
        return 0
    exe = out / "texture_prep_service_release_checks"
    cmd = ["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
           str(cpp), "-o", str(exe)]
    result = subprocess.run(cmd, cwd=out, check=False)
    if result.returncode: return result.returncode
    return subprocess.run([str(exe)], cwd=out, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
