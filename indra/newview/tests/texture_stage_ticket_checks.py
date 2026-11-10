#!/usr/bin/env python3
"""Generate a CPU harness from the production immutable texture ticket methods.

Usage after the serial CPU slot is granted:
  python3 indra/newview/tests/texture_stage_ticket_checks.py \
      --source-root <this-worktree> --out <owned-validation-dir> --run

The generated C++ contains the production start/poll/cancel/discard/fillRGBA bodies
verbatim. Only the texture lookup, worker queue, counters, and Vulkan allocation
endpoints are small fakes. This is deliberately not registered in viewer CMake.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shlex
import subprocess
import sys


def extract_braced(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"missing production source marker: {marker}")
    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError(f"missing body for: {marker}")
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
    raise RuntimeError(f"unterminated production body: {marker}")


def source_sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    root = args.source_root.resolve()
    out = args.out.resolve()
    if out == root:
        raise SystemExit("output must be a dedicated validation directory, not the source root")

    cpp_path = root / "indra/newview/vkgfx/llcleanrenderervk.cpp"
    hdr_path = root / "indra/newview/vkgfx/llcleanrenderervk.h"
    cpp = cpp_path.read_text()
    hdr = hdr_path.read_text()
    if "void fillRGBA(" not in cpp:
        # The immutable-only worker was removed by unified preparation. Exercise
        # the replacement service/submit lifetime contracts at this entry point.
        if "LLVKTexturePrepService" not in cpp:
            raise RuntimeError("neither legacy immutable nor unified preparation exists")
        print("Unified route: checking shared service retirement and submit lifetime", flush=True)
        for name in ("texture_prep_context_lifetime_checks.py", "texture_prep_service_release_checks.py"):
            command = [sys.executable, str(pathlib.Path(__file__).with_name(name)),
                       "--source-root", str(root), "--out", str(out / name.removesuffix(".py"))]
            if args.run:
                command.append("--run")
            subprocess.run(command, check=True)
        return 0
    snippets = [
        extract_braced(cpp, "void fillRGBA("),
        extract_braced(cpp, "void LLCleanRendererVK::discardTexStageCandidate("),
        extract_braced(cpp, "LLCleanRendererVK::TexStageStartResult LLCleanRendererVK::startImmutableTexStage("),
        extract_braced(cpp, "void LLCleanRendererVK::pollImmutableTexStages("),
        extract_braced(cpp, "void LLCleanRendererVK::cancelTexStage("),
    ]
    residency_start = cpp.find("// Re-evaluate active CPU tickets")
    residency_end = cpp.find("// Upgrades: self/HUD first", residency_start)
    if residency_start < 0 or residency_end < 0:
        raise RuntimeError("missing production active-ticket demand recheck block")
    residency_recheck = cpp[residency_start:residency_end]
    shared_tex = extract_braced(hdr, "struct SharedTex {")
    ticket = extract_braced(hdr, "struct TexStageTicket\n")
    enum = extract_braced(hdr, "enum class TexStageStartResult : uint8_t")
    caps = {}
    for name in ("kTexStageTicketCap", "kTexStageTicketBytes", "kTexStageInputBytes"):
        match = re.search(rf"static constexpr (?:uint32_t|VkDeviceSize) {name} = (\d+)(?:ull)?(?:\s*<<\s*(\d+))?;", hdr)
        if not match:
            raise RuntimeError(f"missing production capacity definition {name}")
        caps[name] = int(match.group(1)) << int(match.group(2) or 0)
    if "targetSkip(" in snippets[3] or "covWant(" in snippets[3]:
        raise RuntimeError("poll method captured residency-local policy; source scope regression")

    support = r'''#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using uint8_t = std::uint8_t; using uint32_t = std::uint32_t;
using uint64_t = std::uint64_t; using VkDeviceSize = std::uint64_t;
using VkImage = std::uint64_t; using VkImageView = std::uint64_t;
using VkDeviceMemory = std::uint64_t; using VkBufferHandle = std::uint64_t;
using VkFormat = std::uint32_t;
constexpr VkImage VK_NULL_HANDLE = 0;
constexpr VkFormat VK_FORMAT_R8G8B8A8_SRGB = 43;
constexpr uint32_t VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1;
constexpr uint32_t VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2;
constexpr uint32_t VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4;
constexpr uint32_t VKM_TEXTURE = 1;
constexpr uint32_t VKMEM_TEXTURE = 1;
enum class LLPixelType : uint8_t { NONE, UBYTE };
enum class LLPixelFormat : uint8_t { NONE, RGB, SRGB, RGBA, SRGB_ALPHA };
enum { TEXENC_NONE = 0 };
struct LLUUID { uint64_t value = 0; };
struct LLImageRaw {
  std::vector<uint8_t> bytes; uint32_t w=0,h=0,comps=0;
  bool isBufferInvalid() const { return bytes.empty(); }
  const uint8_t* getData() const { return bytes.data(); }
  uint32_t getWidth() const { return w; } uint32_t getHeight() const { return h; }
  int getComponents() const { return (int)comps; } int getDataSize() const { return (int)bytes.size(); }
};
struct LLVKImmutablePixelSnapshot {
  std::shared_ptr<const LLImageRaw> raw; uint32_t w=0,h=0,comps=0;
  LLPixelFormat internal=LLPixelFormat::RGBA, primary=LLPixelFormat::RGBA;
  LLPixelType type=LLPixelType::UBYTE; int32_t gen=1; uint64_t ser=1;
  bool valid() const { return raw && !raw->bytes.empty(); }
  uint32_t width() const { return w; } uint32_t height() const { return h; }
  int components() const { return (int)comps; } uint64_t byteCount() const { return raw?raw->bytes.size():0; }
  LLPixelFormat internalFormat() const { return internal; }
  LLPixelFormat primaryFormat() const { return primary; }
  LLPixelType typeFormat() const { return type; }
  int32_t generation() const { return gen; } uint64_t serial() const { return ser; }
  const LLImageRaw* pixels() const { return valid()?raw.get():nullptr; }
};
struct LLTextureData {
  static constexpr int VKLEASE_STREAM=1;
  bool writable=false, captureOK=true; uint32_t leases=0;
  LLVKImmutablePixelSnapshot snap;
  bool isVKCpuWritable() const { return writable; }
};
struct LLVKImmutablePixelLease {
  LLTextureData* owner=nullptr; LLVKImmutablePixelSnapshot snap;
  LLVKImmutablePixelLease()=default; LLVKImmutablePixelLease(const LLVKImmutablePixelLease&)=delete;
  LLVKImmutablePixelLease& operator=(const LLVKImmutablePixelLease&)=delete;
  LLVKImmutablePixelLease(LLVKImmutablePixelLease&& o) noexcept : owner(std::exchange(o.owner,nullptr)),snap(std::move(o.snap)) {}
  LLVKImmutablePixelLease& operator=(LLVKImmutablePixelLease&& o) noexcept {
    if(this!=&o){reset();owner=std::exchange(o.owner,nullptr);snap=std::move(o.snap);} return *this;
  }
  ~LLVKImmutablePixelLease(){reset();}
  bool capture(LLTextureData* d,int){if(!d||!d->captureOK||d->writable)return false;owner=d;++owner->leases;snap=d->snap;return true;}
  const LLVKImmutablePixelSnapshot& snapshot() const{return snap;}
  void reset(){if(owner){--owner->leases;owner=nullptr;}snap={};}
};
struct GpuBuffer { VkBufferHandle buffer=0; void* mapped=nullptr; VkDeviceSize allocationSize=0; };
struct GpuImage { VkImage image=0; VkDeviceMemory memory=0; VkImageView view=0; uint32_t format=0; struct{uint32_t width=0,height=0;} extent{}; };
struct VkDeviceContext {
  bool allocOK=true; uint64_t next=1; uint32_t creates=0,destroys=0;
  std::map<uint64_t,std::unique_ptr<uint8_t[]>> storage;
  struct SoftAllocScope { explicit SoftAllocScope(VkDeviceContext*){} };
  GpuBuffer createBufferBounded(VkDeviceSize n,uint32_t,uint32_t,VkDeviceSize,bool,const char*){
    ++creates;if(!allocOK||!n)return {}; auto p=std::make_unique<uint8_t[]>((size_t)n);auto raw=p.get();auto id=next++;storage.emplace(id,std::move(p));return {id,raw,n};}
  void destroyBuffer(GpuBuffer& b){if(b.buffer){storage.erase(b.buffer);++destroys;}b={};}
  void destroyView(VkImageView&){} void destroyImage(GpuImage& i){i={};}
  VkDeviceSize allocBytes(const GpuImage&) const{return 4096;}
};
struct VkMemScope { explicit VkMemScope(uint32_t){} ~VkMemScope(){} };
struct TexStageCpuTimer {
  TexStageCpuTimer(std::atomic<uint64_t>&,std::atomic<uint32_t>&){}
};
bool texStageThreadCpuNs(uint64_t&){return false;}
namespace LL { struct ThreadPool { struct Queue {
  bool accept=true; std::vector<std::function<void()>> jobs;
  template<class F> bool post(F&& f){if(!accept)return false;jobs.emplace_back(std::forward<F>(f));return true;}
  void run(){for(auto& f:jobs)f();jobs.clear();}
 } q; Queue& getQueue(){return q;} }; }
struct LLVKHitch { static void add(const char*,double){} static void count(const char*,uint64_t){} };
bool gDeviceLost=false; bool vkDeviceIsLost(){return gDeviceLost;}
uint32_t texMipLevels(uint32_t,uint32_t){return 1;}
'''
    # Extract production state layouts rather than keeping a second ticket model.
    support += "\n" + shared_tex + ";\n" + ticket + ";\n" + enum + ";\n"
    support += r'''struct TexEncPick { uint8_t enc = TEXENC_NONE; };
static uint32_t gTargetSkip=0; static uint8_t gPickEncoding=TEXENC_NONE;
static bool gCoverageWanted=false, gConstantCollapse=false;
uint32_t targetSkip(uint32_t,uint32_t,uint32_t,float){return gTargetSkip;}
float texBias(uint32_t){return 0.0f;}
TexEncPick texEncFor(const SharedTex&,uint32_t,uint32_t){return {gPickEncoding};}
bool covWant(const SharedTex&){return gCoverageWanted;}
bool texConstCollapse(const SharedTex&,uint32_t,uint32_t){return gConstantCollapse;}
'''
    support += r'''struct TexPendingItem { uint64_t stage_ticket=0; };
class LLCleanRendererVK {
public:
  using SharedTex=::SharedTex; using TexStageTicket=::TexStageTicket;
  using TexStageStartResult=::TexStageStartResult;
  static constexpr uint32_t kTexStageTicketCap=CAP_TICKETS;
  static constexpr VkDeviceSize kTexStageTicketBytes=CAP_OUTPUT, kTexStageInputBytes=CAP_INPUT;
  std::array<TexStageTicket,kTexStageTicketCap> mTexStageTickets{};
  std::map<uint64_t,SharedTex> mSharedTex; std::vector<TexPendingItem> mPendUploads;
  void* mStagePool=nullptr; VkDeviceContext* mCtx=nullptr;
  uint64_t mTexStageNextId=1; VkDeviceSize mTexStageBytesUsed=0,mTexStageInputBytesUsed=0;
  std::atomic<uint64_t> mTexStagePrepareCpuNs{0},mTexStageWorkerCpuNs{0};
  std::atomic<uint32_t> mTexStageCpuUnavailable{0},mTexStageWorkerCpuUnavailable{0};
  uint32_t mTexPass=1;
  bool sourceCurrent=true, reserveOK=true; uint32_t reserveCount=0,releaseCount=0;
  bool deviceLost(){return vkDeviceIsLost();}
  bool texStageSourceCurrent(const TexStageTicket&) const{return sourceCurrent;}
  bool reserveTexStageRetire(TexStageTicket& t,const SharedTex&){if(!reserveOK)return false;t.retireIndex=0;t.retireBytes=4096;++reserveCount;return true;}
  void releaseTexStageRetire(TexStageTicket& t){if(t.retireIndex!=~0u){t.retireIndex=~0u;t.retireBytes=0;++releaseCount;}}
  void discardTexStageCandidate(TexStageTicket&);
  TexStageStartResult startImmutableTexStage(uint64_t,const SharedTex&,LLTextureData*,uint32_t,uint32_t);
  void pollImmutableTexStages(); void cancelTexStage(uint64_t);
  void reevaluateActiveTickets() {
RECHECK_BLOCK
  }
};
'''
    support = support.replace("CAP_TICKETS", str(caps["kTexStageTicketCap"]))
    support = support.replace("CAP_OUTPUT", str(caps["kTexStageTicketBytes"]))
    support = support.replace("CAP_INPUT", str(caps["kTexStageInputBytes"]))
    support = support.replace("RECHECK_BLOCK", residency_recheck)
    support += snippets[0] + "\n"
    support += "\n".join(snippets[1:]) + "\n"
    support += r'''static void expect(bool ok,const char* label){if(!ok){std::cerr<<"FAIL "<<label<<"\n";std::exit(2);} }
static LLTextureData makeSource(){LLTextureData d;auto raw=std::make_shared<LLImageRaw>();raw->w=2;raw->h=2;raw->comps=4;raw->bytes.assign(16,127);d.snap.raw=raw;d.snap.w=2;d.snap.h=2;d.snap.comps=4;return d;}
int main(){
  VkDeviceContext ctx; LL::ThreadPool pool; LLCleanRendererVK r;r.mCtx=&ctx;r.mStagePool=&pool;
  SharedTex resident;resident.image=11;resident.slot[0]=4;resident.id.value=7;r.mSharedTex.emplace(7,resident);
  LLTextureData src=makeSource();
  // Unsupported mutable source is Ineligible and allocates neither retirement nor staging.
  src.writable=true; auto result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Ineligible&&ctx.creates==0,"mutable-ineligible");src.writable=false;
  // Input cap pressure is Deferred before Vulkan allocation and leaves the old image resident.
  r.mTexStageInputBytesUsed=LLCleanRendererVK::kTexStageInputBytes;
  result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Deferred&&ctx.creates==0&&r.mSharedTex[7].image==11,"input-cap-deferred");
  r.mTexStageInputBytesUsed=0;
  // Output admission uses the allocator's returned allocation size, not only pixel bytes.
  r.mTexStageBytesUsed=LLCleanRendererVK::kTexStageTicketBytes-8;
  const uint32_t beforeStageCap=ctx.creates;
  result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Deferred&&ctx.creates==beforeStageCap+1&&r.mTexStageBytesUsed==LLCleanRendererVK::kTexStageTicketBytes-8&&r.mSharedTex[7].image==11,"allocator-requirement-cap-deferred");
  r.mTexStageBytesUsed=0;
  // Reservation and Vulkan allocation refusal are both Deferred, preserving the resident.
  r.reserveOK=false; result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Deferred&&ctx.creates==beforeStageCap+1&&r.mSharedTex[7].image==11,"retirement-reservation-deferred");r.reserveOK=true;
  ctx.allocOK=false; result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Deferred&&r.mSharedTex[7].image==11,"staging-allocation-oom-deferred");ctx.allocOK=true;
  for(uint32_t i=0;i<r.mTexStageTickets.size();++i){r.mTexStageTickets[i].life=TexStageTicket::Life::Ready;r.mTexStageTickets[i].key=1000+i;}
  const uint32_t beforeTicketCap=ctx.creates; result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Deferred&&ctx.creates==beforeTicketCap&&r.mSharedTex[7].image==11,"ticket-count-cap-deferred");
  for(auto& ticket:r.mTexStageTickets) ticket=TexStageTicket{};
  // Worker-post refusal rolls back the acquired lease, staging, and reservation.
  pool.q.accept=false; result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Deferred&&src.leases==0&&r.mTexStageBytesUsed==0&&r.mTexStageInputBytesUsed==0&&r.mSharedTex[7].image==11,"post-refusal-rollback");
  pool.q.accept=true;
  // Accepted Worker cancellation must retain both leases and mapped output until completion.
  const uint32_t destroysBeforeWorkerCancel=ctx.destroys;
  result=r.startImmutableTexStage(7,resident,&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Accepted&&src.leases==1&&r.mTexStageInputBytesUsed==16,"worker-accepted");
  r.cancelTexStage(7);auto* t=&r.mTexStageTickets[0];
  expect(t->life==TexStageTicket::Life::Worker&&t->cancelWhenReady&&src.leases==1&&ctx.destroys==destroysBeforeWorkerCancel,"worker-cancel-retains");
  pool.q.run();r.pollImmutableTexStages();
  expect(t->life==TexStageTicket::Life::Free&&src.leases==0&&r.mTexStageInputBytesUsed==0&&r.mTexStageBytesUsed==0,"worker-cancel-completes");
  // Extracted production residency predicate: wanted Worker continues; demand shrink cancels,
  // preserving the source lease/output until the worker's acquire-observed completion.
  r.mSharedTex[7].w=1; gTargetSkip=0; gPickEncoding=TEXENC_NONE; gCoverageWanted=false; gConstantCollapse=false;
  result=r.startImmutableTexStage(7,r.mSharedTex[7],&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Accepted,"wanted-worker-start");t=&r.mTexStageTickets[0];
  r.reevaluateActiveTickets();expect(t->life==TexStageTicket::Life::Worker&&!t->cancelWhenReady&&src.leases==1,"wanted-worker-retained");
  pool.q.run();r.pollImmutableTexStages();expect(t->life==TexStageTicket::Life::Ready&&src.leases==0,"wanted-worker-ready");
  r.mSharedTex[7].w=2; r.reevaluateActiveTickets();
  expect(t->life==TexStageTicket::Life::Free&&r.mTexStageBytesUsed==0&&r.mSharedTex[7].image==11,"ready-demand-shrink-released");
  r.mSharedTex[7].w=1;
  result=r.startImmutableTexStage(7,r.mSharedTex[7],&src,2,2);
  expect(result==LLCleanRendererVK::TexStageStartResult::Accepted,"shrinking-worker-start");t=&r.mTexStageTickets[0];
  r.mSharedTex[7].w=2; r.reevaluateActiveTickets();
  expect(t->life==TexStageTicket::Life::Worker&&t->cancelWhenReady&&src.leases==1&&r.mSharedTex[7].image==11,"worker-demand-shrink-retains-until-complete");
  pool.q.run();r.pollImmutableTexStages();
  expect(t->life==TexStageTicket::Life::Free&&src.leases==0&&r.mTexStageBytesUsed==0&&r.mSharedTex[7].image==11,"worker-demand-shrink-completes-cleanly");
  // Ready ticket source replacement invalidates and releases the staged output.
  result=r.startImmutableTexStage(7,resident,&src,2,2);expect(result==LLCleanRendererVK::TexStageStartResult::Accepted,"replacement-start");
  pool.q.run();r.pollImmutableTexStages();t=&r.mTexStageTickets[0];
  expect(t->life==TexStageTicket::Life::Ready&&src.leases==0,"worker-ready");
  r.sourceCurrent=false;r.pollImmutableTexStages();
  expect(t->life==TexStageTicket::Life::Free&&r.mTexStageBytesUsed==0,"ready-stale-released");r.sourceCurrent=true;
  // Queued is still unrecorded: cancellation removes its pending alias before freeing.
  t=&r.mTexStageTickets[0];t->life=TexStageTicket::Life::Queued;t->key=7;t->id=9;t->stageBytes=16;t->staging=ctx.createBufferBounded(16,0,0,16,false,"seed");
  r.mTexStageBytesUsed=16;r.mPendUploads.push_back({9});t->retireIndex=0;t->retireBytes=4096;
  r.cancelTexStage(7);expect(t->life==TexStageTicket::Life::Free&&r.mPendUploads.empty()&&r.mTexStageBytesUsed==0,"queued-cancel-removes-alias");
  // Recorded/accepted states are left for fence retirement, never synchronously freed.
  for(auto life:{TexStageTicket::Life::Recorded,TexStageTicket::Life::Accepted}){
    t=&r.mTexStageTickets[0];t->life=life;t->key=7;t->stageBytes=16;t->staging=ctx.createBufferBounded(16,0,0,16,false,"seed");r.mTexStageBytesUsed+=16;
    r.cancelTexStage(7);expect(t->life==life&&t->staging.buffer,"recorded-retained");
    ctx.destroyBuffer(t->staging);r.mTexStageBytesUsed-=16;*t=TexStageTicket{};
  }
  std::cout<<"PASS ticket start/poll/cancel production extraction\n";
}
'''
    out.mkdir(parents=True, exist_ok=True)
    generated = out / "texture_stage_ticket_checks.cpp"
    generated.write_text(support)
    manifest = out / "source-receipt.txt"
    manifest.write_text(
        f"llcleanrenderervk.cpp sha256 {source_sha(cpp_path)}\n"
        f"llcleanrenderervk.h sha256 {source_sha(hdr_path)}\n"
        f"generated sha256 {source_sha(generated)}\n"
        "production bodies: fillRGBA, discardTexStageCandidate, startImmutableTexStage, pollImmutableTexStages, cancelTexStage\n"
        "production residency block: active-ticket recheck in uploadRealMaterialTextures\n"
        f"source capacities: tickets={caps['kTexStageTicketCap']} output={caps['kTexStageTicketBytes']} input={caps['kTexStageInputBytes']}\n"
    )
    print(f"generated {generated}")
    print(f"receipt {manifest}")
    if args.run:
        exe = out / "texture_stage_ticket_checks"
        cmd = ["/usr/bin/c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
               str(generated), "-o", str(exe)]
        log = out / "compile-run.log"
        env = os.environ.copy()
        for name in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME"):
            path = out / name.lower()
            path.mkdir(parents=True, exist_ok=True)
            env[name] = str(path)

        def disable_core_dump() -> None:
            try:
                import resource
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            except (ImportError, OSError, ValueError):
                pass

        preexec_fn = disable_core_dump if os.name == "posix" else None
        with log.open("w") as f:
            f.write("compile argv: " + shlex.join(cmd) + "\n")
            f.write(f"cwd: {out}\ncore dumps: disabled where supported\n")
            try:
                proc = subprocess.run(cmd, cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT,
                                      check=False, timeout=60, preexec_fn=preexec_fn)
            except subprocess.TimeoutExpired:
                f.write("compile timeout after 60 seconds\n")
                return 124
            if proc.returncode: return proc.returncode
            f.write("run argv: " + shlex.join([str(exe)]) + "\n")
            try:
                proc = subprocess.run([str(exe)], cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT,
                                      check=False, timeout=10, preexec_fn=preexec_fn)
            except subprocess.TimeoutExpired:
                f.write("fixture timeout after 10 seconds\n")
                return 124
            if proc.returncode: return proc.returncode
        print(f"PASS; see {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
