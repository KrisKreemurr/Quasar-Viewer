#!/usr/bin/env python3
"""Exercise the production context teardown latch and service admission close.

The generated CPU fixture extracts the context's teardown/getter methods and the
service's stopAdmissions() method unchanged. Vulkan handles are represented by
fixed counters; this checks that a failed idle proof leaves the same service and
all budget charges retained while repeated reset/shutdown attempts cannot admit
more work. It is not a Vulkan/device-idle integration test.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys


def extract(source: str, marker: str, start_at: int = 0) -> tuple[str, int]:
    start = source.find(marker, start_at)
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
                if depth == 0: return source[start:i + 1], start
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
    root, out = args.source_root.resolve(), args.out.resolve()
    if out == root or root not in out.parents:
        raise SystemExit("output must be a dedicated directory under the owned source root")

    context_path = root / "indra/newview/vkgfx/llvulkancontext.cpp"
    service_path = root / "indra/newview/vkgfx/llvktextureprepservice.cpp"
    context = context_path.read_text()
    service = service_path.read_text()
    teardown, _ = extract(context, "void LLVulkanContext::beginTexturePrepTeardown() noexcept")
    getter, _ = extract(context, "LLVKTexturePrepService* LLVulkanContext::texturePrepService() noexcept")
    impl_start = context.find("struct LLVulkanContext::Impl")
    if impl_start < 0: raise RuntimeError("missing context Impl")
    impl_latch, _ = extract(context, "void beginTexturePrepTeardown() noexcept", impl_start)
    stop, _ = extract(service, "void LLVKTexturePrepService::stopAdmissions() noexcept")
    service_shutdown, _ = extract(service, "void LLVKTexturePrepService::shutdown() noexcept")
    shutdown, _ = extract(context, "void LLVulkanContext::shutdown()")
    lost_branch, _ = extract(shutdown, "if (vkDeviceIsLost())")
    idle_branch, _ = extract(shutdown, "if (idleResult != VK_SUCCESS || vkDeviceIsLost())")
    compile_branch, _ = extract(shutdown, "if (LLFxRuntimeVK::instance().compileInDriver())")
    submit_gate = "if (!mImpl->accepting || mImpl->closing) return QueueResult::Deferred;"
    if submit_gate not in service:
        raise RuntimeError("submit no longer refuses work after stopAdmissions")

    def ordered(*needles: str) -> bool:
        pos = -1
        for needle in needles:
            pos = shutdown.find(needle, pos + 1)
            if pos < 0: return False
        return True

    if not ordered("beginTexturePrepTeardown();", "if (vkDeviceIsLost())", "vkDeviceWaitIdle(",
                   "if (LLFxRuntimeVK::instance().compileInDriver())",
                   "mImpl->mTexturePrepService->shutdown();", "mImpl->mTexturePrepService.reset();",
                   "mImpl->mDevice.shutdown();"):
        raise RuntimeError("shutdown no longer latches first and finalizes the service only after the proof gates")
    for name, branch in (("device-lost", lost_branch), ("idle-unproven", idle_branch),
                         ("driver-compile", compile_branch)):
        if "return;" not in branch or "mTexturePrepService->shutdown()" in branch \
                or "mTexturePrepService.reset();" in branch:
            raise RuntimeError(f"{name} early return must retain service slots and budget charges")
    if "mInitialized = false;" not in teardown or "mTexturePrepTeardownStarted" not in getter:
        raise RuntimeError("context teardown/getter no longer hides the service after the latch")
    if context.count("mTexturePrepService.reset();") != 2:
        raise RuntimeError("unexpected runtime service reset site; only init failure and device teardown are allowed")
    if "mTexturePrepService" in (root / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text():
        raise RuntimeError("world renderer must not own/reset the context service")
    if "mImpl->context->destroyBuffer(slot.stage);" not in service_shutdown \
            or "slot.stage = GpuBuffer{}" in service_shutdown:
        raise RuntimeError("proofed service cleanup must destroy, not forget, every remaining stage handle")

    cpp = r'''#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
using uint32_t = std::uint32_t; using uint64_t = std::uint64_t;
enum class QueueResult { Accepted, Deferred };

class LLVKTexturePrepService {
public:
  struct Impl { std::mutex mutex; bool accepting=true,closing=false; uint32_t tickets=0,accepted=0; uint64_t input=0,stage=0,scratch=0; };
  std::unique_ptr<Impl> mImpl=std::make_unique<Impl>();
  void stopAdmissions() noexcept PLACE_STOP
};
struct LLVulkanContext {
  struct Impl {
    std::unique_ptr<LLVKTexturePrepService> mTexturePrepService;
    bool mTexturePrepTeardownStarted=false;
    void beginTexturePrepTeardown() noexcept PLACE_IMPL_LATCH
  };
  Impl* mImpl=nullptr; bool mInitialized=false;
  LLVKTexturePrepService* texturePrepService() noexcept PLACE_GETTER
  void beginTexturePrepTeardown() noexcept PLACE_TEARDOWN
};
QueueResult submitGate(LLVKTexturePrepService::Impl* mImpl) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
PLACE_SUBMIT_GATE
  ++mImpl->accepted;
  return QueueResult::Accepted;
}
int main() {
  auto serviceOwner=std::make_unique<LLVKTexturePrepService>();
  LLVKTexturePrepService* service=serviceOwner.get();
  service->mImpl->tickets=3; service->mImpl->input=4096; service->mImpl->stage=8192; service->mImpl->scratch=2048;
  LLVulkanContext::Impl impl; impl.mTexturePrepService=std::move(serviceOwner);
  LLVulkanContext context; context.mImpl=&impl; context.mInitialized=true;
  const auto* identity=context.texturePrepService();
  assert(identity==service && service->mImpl->accepting);
  assert(submitGate(service->mImpl.get())==QueueResult::Accepted && service->mImpl->accepted==1);
  context.beginTexturePrepTeardown(); // actual shutdown preamble, before lost/idle checks
  const auto held=service->mImpl->tickets; const auto input=service->mImpl->input;
  const auto stage=service->mImpl->stage; const auto scratch=service->mImpl->scratch;
  assert(!context.mInitialized && context.texturePrepService()==nullptr);
  assert(!service->mImpl->accepting && held==3 && input==4096 && stage==8192 && scratch==2048);
  assert(submitGate(service->mImpl.get())==QueueResult::Deferred && service->mImpl->accepted==1);
  context.beginTexturePrepTeardown(); // failed idle/device-loss retry: idempotent, no new service
  assert(context.texturePrepService()==nullptr && impl.mTexturePrepService.get()==service);
  assert(service->mImpl->tickets==held && service->mImpl->input==input && service->mImpl->stage==stage
         && service->mImpl->scratch==scratch);
}
'''
    cpp = cpp.replace("PLACE_STOP", stop[stop.find("{"):])
    cpp = cpp.replace("PLACE_IMPL_LATCH", impl_latch[impl_latch.find("{"):])
    cpp = cpp.replace("PLACE_GETTER", getter[getter.find("{"):])
    cpp = cpp.replace("PLACE_TEARDOWN", teardown[teardown.find("{"):])
    cpp = cpp.replace("PLACE_SUBMIT_GATE", submit_gate)
    out.mkdir(parents=True, exist_ok=True)
    generated = out / "texture_prep_context_lifetime_checks.cpp"
    generated.write_text(cpp)
    receipt = out / "source-receipt.txt"
    receipt.write_text(
        f"context_sha256={hashlib.sha256(context_path.read_bytes()).hexdigest()}\n"
        f"service_sha256={hashlib.sha256(service_path.read_bytes()).hexdigest()}\n"
        f"generated_sha256={hashlib.sha256(generated.read_bytes()).hexdigest()}\n"
        "extracted production methods: context getter, context teardown preamble, Impl latch, service stopAdmissions\n"
        "structural checks: latch before lost/idle gates; compile guard before service finalize; service finalize before device shutdown; only init-failure/device-teardown reset sites\n"
        "fixture scope: same service identity and slot/input/stage/scratch charges survive a failed-proof teardown latch; no growth or getter access on repeated latch\n"
    )
    print(f"generated={generated}")
    print(f"receipt={receipt}")
    if not args.run: return 0
    exe = out / "texture_prep_context_lifetime_checks"
    cmd = ["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", str(generated), "-o", str(exe)]
    try:
        result = subprocess.run(cmd, cwd=out, timeout=30, check=False)
        if result.returncode: return result.returncode
        result = subprocess.run([str(exe)], cwd=out, timeout=10, check=False)
        return result.returncode
    except subprocess.TimeoutExpired:
        return 124


if __name__ == "__main__":
    sys.exit(main())
