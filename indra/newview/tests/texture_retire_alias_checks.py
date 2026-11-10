#!/usr/bin/env python3
"""Extract strict texture-retirement production methods into a bounded CPU fixture.

Run without --run to generate source only. --run compiles and executes the fixture;
use only when the serialized CPU validation token is granted.
"""
import argparse
import hashlib
import os
import re
from pathlib import Path
import shlex
import subprocess


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    at = brace
    while at < len(source):
        ch = source[at]
        nxt = source[at + 1] if at + 1 < len(source) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; at += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"): state = "code"
        elif ch == "/" and nxt == "/": state = "line"; at += 1
        elif ch == "/" and nxt == "*": state = "block"; at += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return source[start:at + 1]
        at += 1
    raise ValueError(f"unterminated production method: {signature}")


def generate(root: Path, out: Path) -> Path:
    source = (root / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
    context = (root / "indra/newview/vkgfx/llvulkancontext.cpp").read_text()
    handle_key = "template<typename Handle>\n" + method(source, "uint64_t texStageHandleKey(")
    helper = "namespace {\n" + handle_key + "\n" + method(source, "bool texStageHasView(") + "\n}"
    reserved_prune = method(source, "void LLCleanRendererVK::forgetTexStageBorrowedView(")
    reserved_alias = method(source, "bool LLCleanRendererVK::reservedTexStageDescriptorAlias(")
    late_defer = method(source, "bool LLCleanRendererVK::texStageLateShouldDefer(")
    signatures = (
        "void LLCleanRendererVK::destroyRetired(",
        "bool LLCleanRendererVK::reserveTexStageRetire(",
        "void LLCleanRendererVK::releaseTexStageRetire(",
        "bool LLCleanRendererVK::transferTexStageRetire(",
        "bool LLCleanRendererVK::texStageGpuUsePins(",
        "void LLCleanRendererVK::retireTexStageGpuUse(",
        "void LLCleanRendererVK::noteTexStageDescriptorRewrite(",
        "void LLCleanRendererVK::drainTexStageRetire()",
    )
    methods = reserved_prune + "\n\n" + "\n\n".join(method(source, sig) for sig in signatures)
    if "bool LLCleanRendererVK::texStageRetireHandlesFit(" in source:
        methods = method(source, "bool LLCleanRendererVK::texStageRetireHandlesFit(") + "\n\n" + methods
    guarded = method(source, "void LLCleanRendererVK::destroyRetiredGuarded(")
    idle = method(source, "void LLCleanRendererVK::destroyRetired(")
    if "forgetTexStageBorrowedView(l.view)" not in guarded or "forgetTexStageBorrowedView(v)" not in idle:
        raise ValueError("ordinary and idle view-free paths must prune borrowed views")
    if ("reservedTexStageDescriptorAlias(dimg[i], dview[i])" not in guarded
        or "const bool gpuUsePin = texStageGpuUsePins(l);" not in guarded
        or "if (gpuUsePin) defer[c] = 1;" not in guarded
        or "texStageLateShouldDefer(l.begins, strictPin, reservedPin[c])" not in guarded):
        raise ValueError("guard must pin late-created aliases found in the existing descriptor sweep")
    record_ready = method(source, "void LLCleanRendererVK::recordReadyTexStages(")
    retire_slot = method(source, "void LLCleanRendererVK::retireTexStageSlot(")
    shutdown = method(source, "void LLCleanRendererVK::shutdown()")
    if ("ticket.gpuUsesExpectedOld = true;" not in record_ready
        or "copiesResidentTail = item.copy_src != VK_NULL_HANDLE;" not in record_ready
        or "ticket.gpuUsesExpectedOld = copiesResidentTail;" not in record_ready):
        raise ValueError("recording must mark GPU-only source reads and compressed resident-tail copies")
    if "record.gpuReadTicketId != 0" not in methods or "record.gpuReadTicketId = ticket.id" not in methods:
        raise ValueError("strict retirement must preserve GPU source-use slots through fence completion")
    if "retireTexStageGpuUse(slot);" not in retire_slot:
        raise ValueError("GPU source-use release must be tied to the slot-retirement path")
    if "ticket.life != TexStageTicket::Life::Quarantine" not in methods:
        raise ValueError("slot completion must not clear uncertain GPU-use pins")
    deferred_shutdown = shutdown.find("mShutdownDeferred = true;")
    retire_drain = shutdown.find("destroyRetired(mPendRetire);")
    if ("if (!textureTicketsQuiesced && !vkDeviceIsLost())" not in shutdown
        or deferred_shutdown < 0 or retire_drain < 0 or deferred_shutdown > retire_drain):
        raise ValueError("failed device idle must defer renderer teardown before retire-list destruction")
    offscreen = method(context, "bool LLVulkanContext::renderOffscreen(")
    idle_guard = offscreen.find("if (idleResult != VK_SUCCESS || vkDeviceIsLost())")
    failed_return = offscreen.find("return false;", idle_guard)
    first_image = offscreen.find("GpuImage img;")
    if ("const VkResult idleResult = vkDeviceWaitIdle(dev);" not in offscreen
        or idle_guard < 0 or failed_return < 0 or first_image < 0 or failed_return > first_image):
        raise ValueError("offscreen rendering must stop on failed device-idle proof")
    context_shutdown = method(context, "void LLVulkanContext::shutdown()")
    if ("if (mImpl->mWorld->shutdownDeferred())" not in context_shutdown
        or "if (mImpl->mDeferredWorld->shutdownDeferred())" not in context_shutdown
        or context_shutdown.count("if (mImpl->mDeferredWorld->shutdownDeferred())") != 1):
        raise ValueError("context teardown must retain the live and single deferred renderer owners")
    render_frame = method(context, "bool LLVulkanContext::renderFrame(")
    init_failure = render_frame.find("w->shutdown();", render_frame.find("World renderer init failed"))
    init_delete = render_frame.find("delete w;", init_failure)
    if (init_failure < 0 or init_delete < 0
        or "mImpl->mDeferredWorld = w;" not in render_frame[init_failure:init_delete]
        or "mImpl->retryDeferredWorldShutdown(worldInitNow)" not in render_frame
        or "mImpl->mWorld = w;" in render_frame[init_failure:init_delete]
        or "!mImpl->mDeferredWorld && !mImpl->mWorldInitTried" not in render_frame):
        raise ValueError("failed world init must use a separate bounded owner, not expose it as a live renderer")
    synth = method(context, "bool LLVulkanContext::runWorldSynthTest(")
    if ("retryDeferredWorldShutdown(LLFrameTimer::getElapsedSeconds())" not in synth
        or "mImpl->mDeferredWorld = w;" not in synth
        or "submitImmediate's private fence completed before readback" not in synth
        or "retryDeferredWorldShutdown(worldInitNow)" not in render_frame):
        raise ValueError("synth teardown must retain at most one renderer and only release its completed target")
    if (context.count("LLCleanRendererVK* mDeferredWorld = nullptr;") != 1
        or "mDeferredInitWorld" in context or "mDeferredSynthWorld" in context):
        raise ValueError("failed init and synth cleanup must share one fixed, non-renderable owner slot")
    retry_deferred = method(context, "bool retryDeferredWorldShutdown(double now)")
    renderer_shutdown = method(source, "void LLCleanRendererVK::shutdown()")
    if ("vkDeviceWaitIdle(dev) == VK_SUCCESS" not in renderer_shutdown
        or "mDeferredWorld->shutdown();" not in retry_deferred
        or "mImpl->mWorldInitRetryAt = worldInitNow + 2.0;" not in render_frame):
        raise ValueError("deferred ownership may clear only after a fresh successful idle retry")
    if ("if (now < mDeferredWorldRetryAt) return false;" not in retry_deferred
        or "mDeferredWorld->shutdown();" not in retry_deferred
        or "if (mDeferredWorld->shutdownDeferred()) return false;" not in retry_deferred
        or "mDeferredWorld = nullptr;" not in retry_deferred):
        raise ValueError("deferred retry must retain its sole owner until shutdown proves idle")
    header = (root / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()
    if "kRetireLateMaxBegins = 3u * kTexDescSets" not in header:
        raise ValueError("fixture expects the production bounded legacy age cap")
    body = r'''#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
using VkDeviceSize=uint64_t; using VkImage=uint64_t; using VkImageView=uint64_t;
static uint64_t viewOperations=0;
// Instrument only the standard-library find boundary. Production method bodies
// retain their control flow and real numeric handle type.
template<typename Iterator, typename Value>
Iterator countedFind(Iterator first,Iterator last,const Value& value) {
 for(;first!=last;++first){++viewOperations;if(*first==value)return first;}
 return last;
}
using VkDeviceMemory=uint64_t; using VkFormat=uint32_t;
using VkDevice=uint64_t; using VkSampler=uint64_t; using VkPipeline=uint64_t;
constexpr uint64_t VK_NULL_HANDLE=0;
void vkDestroySampler(VkDevice,VkSampler,const void*) {}
void vkForgetPipeline(VkPipeline) {}
void vkDestroyPipeline(VkDevice,VkPipeline,const void*) {}
struct VkExtent2D { uint32_t width=0,height=0; };
struct GpuImage { VkImage image=0; VkDeviceMemory memory=0; VkImageView view=0; VkFormat format=0; VkExtent2D extent{}; };
struct GpuBuffer { uint64_t buffer=0; };
struct SharedTex { VkImage image=0; VkDeviceMemory memory=0; VkFormat fmt=0; uint32_t w=0,h=0; uint32_t slot[2]={0,0}; };
struct TexRetire {
 struct CowRet { uint64_t set=0; uint32_t member=0,gen=0; };
 std::vector<VkImageView> views; std::vector<GpuImage> images; std::vector<GpuBuffer> buffers;
 std::vector<VkSampler> samplers; std::vector<VkPipeline> pipelines; std::vector<CowRet> dsets;
};
struct TexLate { GpuImage img{}; VkImageView view=0; uint32_t begins=0; };
struct FakeContext {
 std::map<VkImage,VkDeviceSize> sizes; std::vector<uint64_t> destroyed;
 VkDevice device() const { return 1; }
 VkDeviceSize allocBytes(const GpuImage& g) const { auto i=sizes.find(g.image); return i==sizes.end()?0:i->second; }
 void destroyView(VkImageView v) { destroyed.push_back(100000+v); }
 void destroyImage(const GpuImage& g) { destroyed.push_back(g.image); }
 void destroyBuffer(GpuBuffer&) {}
};
static bool deviceLost=false; bool vkDeviceIsLost(){return deviceLost;}
class LLCleanRendererVK {
public:
 static constexpr uint32_t kTexDescSets=3, kMaxBindlessTextures=65536;
 static constexpr uint32_t kTexStageRetireCap=RETIRE_CAP_EXPRESSION;
 static constexpr uint32_t kRetireLateMaxBegins=3u*kTexDescSets;
 static constexpr VkDeviceSize kTexStageRetireBytes=128ull<<20;
 static constexpr uint32_t kTexStageRetireViewCap=kMaxBindlessTextures*(kTexDescSets+1);
 static constexpr uint64_t kTexStageRetireHandleCap=RETIRE_HANDLE_EXPRESSION;
 struct TexStageTicket { enum class Life:uint8_t {Free,Quarantine}; Life life=Life::Free;
  uint64_t id=0,key=0; uint32_t retireIndex=~0u,recordedSlot=0; VkDeviceSize retireBytes=0;
  bool gpuUsesExpectedOld=false; VkImage expectedOld=0; VkImageView gpuSourceView=0; };
 struct TexStageRetire {
  enum class State:uint8_t { Free,Reserved,Retiring };
  State state=State::Free; GpuImage image{}; std::vector<VkImageView> views,legacyViews;
  uint64_t key=0,gpuReadTicketId=0; uint32_t descriptorRefs[kTexDescSets]={},gpuReadSlot=~0u; VkDeviceSize bytes=0;
 };
 struct CowMember { uint32_t gen=0; std::vector<uint64_t> spare; };
 struct TexStageCpuTimer {
  TexStageCpuTimer(std::atomic<uint64_t>&,std::atomic<uint32_t>&) noexcept {}
  ~TexStageCpuTimer() noexcept {}
 };
 FakeContext* mCtx=nullptr; std::array<TexStageRetire,kTexStageRetireCap> mTexStageRetire{};
 std::array<TexStageTicket,64> mTexStageTickets{};
 struct { uint64_t retireRefuseBytes=0,retireRefuseSlots=0,retireRefuseDuplicate=0,retireRefuseHandles=0; } mTexStageInterval;
 VkDeviceSize mTexStageRetireBytesUsed=0; std::vector<GpuImage> mTextures;
 std::atomic<uint64_t> mTexStageRetireCpuNs{0}; std::atomic<uint32_t> mTexStageCpuUnavailable{0};
 std::vector<VkImage> mDescImg[kTexDescSets]; std::vector<VkImageView> mDescView[kTexDescSets];
 std::map<uint64_t,SharedTex> mSharedTex; TexRetire mPendRetire; TexRetire mRetire[kTexDescSets];
 std::vector<TexLate> mRetireLate; std::vector<CowMember> mCow;
 void destroyRetired(TexRetire&);
 bool reserveTexStageRetire(TexStageTicket&,const SharedTex&);
 void releaseTexStageRetire(TexStageTicket&);
 bool texStageRetireHandlesFit(uint64_t,const TexStageRetire* =nullptr) const;
 void forgetTexStageBorrowedView(VkImageView);
 bool reservedTexStageDescriptorAlias(VkImage,VkImageView) const;
 bool texStageLateShouldDefer(uint32_t,bool,bool) const;
 bool transferTexStageRetire(TexStageTicket&,const SharedTex&);
 bool texStageGpuUsePins(const TexLate&) const; void retireTexStageGpuUse(uint32_t);
 void noteTexStageDescriptorRewrite(uint32_t,uint32_t,VkImage,VkImageView,VkImage,VkImageView);
 void drainTexStageRetire();
};
'''
    cap = re.search(r"kTexStageRetireCap\s*=\s*([^;]+);", header).group(1)
    body = body.replace("RETIRE_CAP_EXPRESSION", cap)
    handles = re.search(r"kTexStageRetireHandleCap\s*=\s*([^;]+);", header)
    body = body.replace("RETIRE_HANDLE_EXPRESSION", handles.group(1) if handles else "16ull*kTexStageRetireViewCap")
    body = "#define HAS_RETIRE_HANDLE_BUDGET " + ("1" if "texStageRetireHandlesFit(" in source else "0") + "\n" + body
    # Count numeric handle comparisons used by lower_bound/sort as well as find.
    helper = helper.replace("return handle;", "return (++viewOperations, handle);")
    methods = methods.replace("std::find(", "countedFind(")
    body += helper + "\n\n" + reserved_alias + "\n\n" + late_defer + "\n\n" + methods + r'''
static SharedTex shared(uint64_t image,uint32_t a,uint32_t b) { SharedTex s{};s.image=image;s.memory=image+1;s.fmt=1;s.w=s.h=8;s.slot[0]=a;s.slot[1]=b;return s; }
static GpuImage gpu(uint64_t image,uint64_t view) { GpuImage g{};g.image=image;g.memory=image+1;g.view=view;g.format=1;g.extent={8,8};return g; }
static void rewrite(LLCleanRendererVK& r,uint32_t bank,uint32_t slot,VkImage im,VkImageView view,VkImage newIm,VkImageView newView) {
 r.noteTexStageDescriptorRewrite(bank,slot,im,view,newIm,newView);r.mDescImg[bank][slot]=newIm;r.mDescView[bank][slot]=newView;
}
static void detached_alias_waits_for_bank_and_legacy_view() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx;r.mTextures.resize(3);auto old=shared(77,0,2);
 ctx.sizes[77]=8192;r.mTextures[2]=gpu(77,82);r.mPendRetire.views.push_back(81);
 for(uint32_t b=0;b<3;b++){r.mDescImg[b].resize(3);r.mDescView[b].resize(3);r.mDescImg[b][2]=77;r.mDescView[b][2]=82;}
 r.mDescImg[0][1]=77;r.mDescView[0][1]=81;
 LLCleanRendererVK::TexStageTicket t{};t.key=66;assert(r.reserveTexStageRetire(t,old));assert(r.transferTexStageRetire(t,old));
 assert(r.mTexStageRetire[0].descriptorRefs[0]==2);assert(r.mTexStageRetire[0].descriptorRefs[1]==1);assert(r.mTexStageRetire[0].descriptorRefs[2]==1);
 assert(r.mTexStageRetire[0].views.size()==1&&r.mTexStageRetire[0].views[0]==82);
 assert(r.mTexStageRetire[0].legacyViews.size()==1&&r.mTexStageRetire[0].legacyViews[0]==81);
 r.mSharedTex[66]=shared(78,2,0);r.mTextures[2]=gpu(78,83);
 for(uint32_t b=0;b<3;b++)rewrite(r,b,2,77,82,78,83);
 for(uint32_t n=0;n<20;n++)r.drainTexStageRetire();
 assert(r.mTexStageRetire[0].descriptorRefs[0]==1);assert(ctx.destroyed.empty());
 rewrite(r,0,1,77,81,0,0);r.drainTexStageRetire();
 assert(ctx.destroyed.empty()); // legacy still owns view 81; image must remain alive
 r.mPendRetire.views.clear();ctx.destroyView(81);r.drainTexStageRetire();
 assert((ctx.destroyed==std::vector<uint64_t>{100081,100082,77}));
 assert(!r.mTexStageRetireBytesUsed);
}
static void orphaned_alias_is_adopted_once() {
 FakeContext ctx; LLCleanRendererVK r;r.mCtx=&ctx;r.mTextures.resize(3);auto old=shared(91,0,2);ctx.sizes[91]=4096;r.mTextures[2]=gpu(91,92);
 for(uint32_t b=0;b<3;b++){r.mDescImg[b].resize(3);r.mDescView[b].resize(3);r.mDescImg[b][2]=91;r.mDescView[b][2]=92;}
 r.mDescImg[0][1]=91;r.mDescView[0][1]=93;
 LLCleanRendererVK::TexStageTicket t{};t.key=7;assert(r.reserveTexStageRetire(t,old));assert(r.transferTexStageRetire(t,old));
 assert(r.mTexStageRetire[0].views.size()==2);r.mSharedTex[7]=shared(94,2,0);r.mTextures[2]=gpu(94,95);
 for(uint32_t b=0;b<3;b++) { rewrite(r,b,2,91,92,94,95); }
 rewrite(r,0,1,91,93,0,0);r.drainTexStageRetire();
 assert((ctx.destroyed==std::vector<uint64_t>{100092,100093,91}));
}
static void freed_reserved_alias_is_not_transferred_after_handle_reuse() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx; r.mTextures.resize(5); auto old=shared(77,2,3); ctx.sizes[77]=8192;
 r.mTextures[2]=gpu(77,82); r.mTextures[3]=gpu(77,84);
 for(uint32_t b=0;b<3;b++) {
  r.mDescImg[b].resize(4); r.mDescView[b].resize(4);
  r.mDescImg[b][2]=77; r.mDescView[b][2]=82; r.mDescImg[b][3]=77; r.mDescView[b][3]=84;
 }
 r.mDescImg[0][1]=77; r.mDescView[0][1]=81; r.mPendRetire.views.push_back(81);
 LLCleanRendererVK::TexStageTicket t{}; t.key=66; assert(r.reserveTexStageRetire(t,old));
 assert(r.mTexStageRetire[0].legacyViews.size()==1 && r.mTexStageRetire[0].legacyViews[0]==81);
 // A's descriptor aliases are rewritten while the worker is delayed. The ordinary legacy
 // retire owner then frees A; model handle reuse by publishing that same numeric view for a
 // different image before the texture-stage transfer is accepted.
 rewrite(r,0,1,77,81,99,81); TexRetire legacy{}; legacy.views.push_back(81); r.destroyRetired(legacy);
 assert(legacy.views.empty()); r.mTextures[4]=gpu(99,81);
 assert(r.transferTexStageRetire(t,old));
 const auto& rec=r.mTexStageRetire[0];
 assert(std::find(rec.views.begin(),rec.views.end(),81)==rec.views.end());
 assert(std::find(rec.legacyViews.begin(),rec.legacyViews.end(),81)==rec.legacyViews.end());
 r.mSharedTex[66]=shared(88,2,3); r.mTextures[2]=gpu(88,83); r.mTextures[3]=gpu(88,85);
 for(uint32_t b=0;b<3;b++) { rewrite(r,b,2,77,82,88,83); rewrite(r,b,3,77,84,88,85); }
 r.drainTexStageRetire();
 assert((ctx.destroyed==std::vector<uint64_t>{100081,100082,100084,77}));
}
static void late_created_alias_stays_pinned_past_age_cap() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx; r.mTextures.resize(4); auto old=shared(117,2,3); ctx.sizes[117]=4096;
 r.mTextures[2]=gpu(117,122); r.mTextures[3]=gpu(117,124);
 for(uint32_t b=0;b<3;b++) { r.mDescImg[b].resize(4); r.mDescView[b].resize(4); r.mDescImg[b][2]=117; r.mDescView[b][2]=122; r.mDescImg[b][3]=117; r.mDescView[b][3]=124; }
 LLCleanRendererVK::TexStageTicket t{}; t.key=18; assert(r.reserveTexStageRetire(t,old));
 const auto& reserved=r.mTexStageRetire[0]; assert(std::find(reserved.views.begin(),reserved.views.end(),125)==reserved.views.end());
 // C appears after reservation and is detached into ordinary retirement while an
 // in-flight descriptor bank still pairs C with the old image. The age cap must not force it.
 r.mDescImg[1][1]=117; r.mDescView[1][1]=125; r.mPendRetire.views.push_back(125);
 const uint32_t aged=LLCleanRendererVK::kRetireLateMaxBegins+4u;
 const bool descriptorPin=r.reservedTexStageDescriptorAlias(r.mDescImg[1][1],r.mDescView[1][1]);
 assert(aged>=LLCleanRendererVK::kRetireLateMaxBegins && descriptorPin);
 const bool wouldDefer=r.texStageLateShouldDefer(aged,false,descriptorPin);
 assert(wouldDefer);
 assert(!r.texStageLateShouldDefer(aged,false,false));
}
static void retiring_legacy_handle_reuse_does_not_pin_old_image() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx; r.mTextures.resize(4); auto old=shared(137,2,0); ctx.sizes[137]=4096;
 r.mTextures[2]=gpu(137,142);
 for(uint32_t b=0;b<3;b++) { r.mDescImg[b].resize(4); r.mDescView[b].resize(4); r.mDescImg[b][2]=137; r.mDescView[b][2]=142; }
 r.mDescImg[0][1]=137; r.mDescView[0][1]=141; r.mRetire[0].views.push_back(141);
 r.mDescImg[1][1]=137; r.mDescView[1][1]=143; r.mRetire[1].views.push_back(143);
 LLCleanRendererVK::TexStageTicket t{}; t.key=28; assert(r.reserveTexStageRetire(t,old)); assert(r.transferTexStageRetire(t,old));
 assert(r.mTexStageRetire[0].legacyViews.size()==2);
 r.mSharedTex[28]=shared(148,2,0); r.mTextures[2]=gpu(148,149);
 rewrite(r,0,1,137,141,0,0); rewrite(r,1,1,137,143,0,0);
 for(uint32_t b=0;b<3;b++) rewrite(r,b,2,137,142,148,149);
 r.mTextures[3]=gpu(199,141); // allocator reuses A's numeric handle for an unrelated live image
 r.destroyRetired(r.mRetire[0]);
 assert(std::find(r.mTexStageRetire[0].legacyViews.begin(),r.mTexStageRetire[0].legacyViews.end(),141)==r.mTexStageRetire[0].legacyViews.end());
 assert(std::find(r.mTexStageRetire[0].views.begin(),r.mTexStageRetire[0].views.end(),142)!=r.mTexStageRetire[0].views.end());
 r.drainTexStageRetire(); assert(r.mTexStageRetire[0].state==LLCleanRendererVK::TexStageRetire::State::Retiring);
 r.destroyRetired(r.mRetire[1]); r.drainTexStageRetire();
 assert((ctx.destroyed==std::vector<uint64_t>{100141,100143,100142,137}));
 assert(r.mTexStageRetire[0].state==LLCleanRendererVK::TexStageRetire::State::Free);
}
static void source_read_waits_for_recorded_slot_fence_without_descriptor_refs() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx; r.mTextures.resize(3);
 auto old=shared(501,2,0); ctx.sizes[501]=4096; r.mTextures[2]=gpu(501,502);
 LLCleanRendererVK::TexStageTicket ticket{}; ticket.id=77; ticket.key=55; ticket.expectedOld=501;
 assert(r.reserveTexStageRetire(ticket,old));
 ticket.gpuUsesExpectedOld=true; ticket.recordedSlot=1;
 assert(r.transferTexStageRetire(ticket,old));
 for(uint32_t set=0;set<LLCleanRendererVK::kTexDescSets;++set) assert(r.mTexStageRetire[0].descriptorRefs[set]==0);
 assert(r.mTexStageRetire[0].gpuReadTicketId==ticket.id && r.mTexStageRetire[0].gpuReadSlot==1);
 r.mSharedTex[55]=shared(511,2,0); r.mTextures[2]=gpu(511,512);
 r.drainTexStageRetire(); assert(ctx.destroyed.empty());
 r.retireTexStageGpuUse(0); r.drainTexStageRetire(); assert(ctx.destroyed.empty());
 r.retireTexStageGpuUse(1); r.drainTexStageRetire();
 assert((ctx.destroyed==std::vector<uint64_t>{100502,501}));
}
static void trial_source_is_pinned_without_a_replacement_retire_record() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx; r.mTextures.resize(3);
 r.mTexStageTickets[0].gpuUsesExpectedOld=true;
 r.mTexStageTickets[0].recordedSlot=1; r.mTexStageTickets[0].expectedOld=601; r.mTexStageTickets[0].gpuSourceView=602;
 TexLate late{}; late.img.image=601; late.img.view=602;
 assert(r.texStageGpuUsePins(late));
 r.retireTexStageGpuUse(0); assert(r.texStageGpuUsePins(late));
 r.retireTexStageGpuUse(r.mTexStageTickets[0].recordedSlot);
 assert(!r.texStageGpuUsePins(late));
}
static void quarantined_source_use_survives_empty_slot_completion() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx; r.mTextures.resize(3);
 auto& ticket=r.mTexStageTickets[0]; ticket.id=808; ticket.life=LLCleanRendererVK::TexStageTicket::Life::Quarantine;
 ticket.gpuUsesExpectedOld=true; ticket.recordedSlot=1; ticket.expectedOld=701;
 auto& record=r.mTexStageRetire[0]; record.state=LLCleanRendererVK::TexStageRetire::State::Retiring;
 record.image=gpu(701,0); record.key=88; record.bytes=4096; record.gpuReadTicketId=808; record.gpuReadSlot=1;
 r.mTexStageRetireBytesUsed=4096; ctx.sizes[701]=4096; r.mSharedTex[88]=shared(702,0,0);
 TexLate late{}; late.img.image=701;
 assert(r.texStageGpuUsePins(late));
 r.retireTexStageGpuUse(1); // an empty replacement fence is not proof for quarantined partial work
 assert(ticket.gpuUsesExpectedOld && record.gpuReadTicketId==808);
 r.drainTexStageRetire(); assert(ctx.destroyed.empty()); assert(record.state==LLCleanRendererVK::TexStageRetire::State::Retiring);
}
static void handle_key_supports_pointer_and_integer_vk_handles() {
 VkImageView integerHandle=37;assert(texStageHandleKey(integerHandle)==37);
 int object=0;const uint64_t pointerKey=texStageHandleKey(&object);
 assert(pointerKey==reinterpret_cast<std::uintptr_t>(&object));
}
static void many_aliases_have_bounded_lookup_work() {
 constexpr uint32_t aliases=10000;
 FakeContext ctx; LLCleanRendererVK r;r.mCtx=&ctx;r.mTextures.resize(3);
 auto old=shared(301,2,0);ctx.sizes[301]=4096;r.mTextures[2]=gpu(301,302);
 for(uint32_t bank=0;bank<3;++bank) {
  r.mDescImg[bank].resize(aliases+1);r.mDescView[bank].resize(aliases+1);
  for(uint32_t slot=1;slot<=aliases;++slot) {
   r.mDescImg[bank][slot]=301;r.mDescView[bank][slot]=1000+slot;
  }
 }
 // Duplicated bank mirrors must collapse to one borrowed handle per alias.
 LLCleanRendererVK::TexStageTicket ticket{};ticket.key=31;
 viewOperations=0;
 assert(r.reserveTexStageRetire(ticket,old));
 const uint64_t reserveWork=viewOperations;
 assert(r.mTexStageRetire[0].legacyViews.size()==aliases);
 assert(reserveWork<4000000); // old growing std::find needs >150M comparisons here
 for(uint32_t slot=1;slot<=aliases;++slot)r.mPendRetire.views.push_back(1000+slot);
 viewOperations=0;
 assert(r.transferTexStageRetire(ticket,old));
 assert(viewOperations<8000000);
 assert(r.mTexStageRetire[0].legacyViews.size()==aliases);
 r.mSharedTex[31]=shared(401,2,0);r.mTextures[2]=gpu(401,402);
 // References still in a bank should bypass the whole live/legacy lookup.
 viewOperations=0;r.drainTexStageRetire();assert(viewOperations==0);
 for(uint32_t bank=0;bank<3;++bank)
  for(uint32_t slot=1;slot<=aliases;++slot)rewrite(r,bank,slot,301,1000+slot,0,0);
 // No borrowed handle is actually queued any more. Thousands of unrelated retired
 // handles must be scanned once, not once for every borrowed handle.
 r.mPendRetire.views.clear();
 for(uint32_t slot=1;slot<=aliases;++slot)r.mPendRetire.views.push_back(1000000+slot);
 viewOperations=0;r.drainTexStageRetire();assert(viewOperations<1000000);
 assert(r.mTexStageRetire[0].state==LLCleanRendererVK::TexStageRetire::State::Free);
 assert(ctx.destroyed.size()==2); // only owned view302 plus image301, no borrowed frees
}
// DEFERRED_OWNER_SLOT_TESTS
static void overlapping_batches_preserve_fences_and_bounds() {
 FakeContext ctx; LLCleanRendererVK r; r.mCtx=&ctx;
 constexpr uint32_t count=16u*(LLCleanRendererVK::kTexDescSets+1u);
 r.mTextures.resize(count+1);
 for(uint32_t b=0;b<3;++b){r.mDescImg[b].resize(count+1);r.mDescView[b].resize(count+1);}
 for(uint32_t i=1;i<=count;++i){
  const uint64_t im=1000+i,view=2000+i;ctx.sizes[im]=1u<<20;r.mTextures[i]=gpu(im,view);
  for(uint32_t b=0;b<3;++b){r.mDescImg[b][i]=im;r.mDescView[b][i]=view;}
  auto old=shared(im,i,0);LLCleanRendererVK::TexStageTicket t{};t.key=i;
  assert(r.reserveTexStageRetire(t,old));assert(r.transferTexStageRetire(t,old));
  r.mSharedTex[i]=shared(3000+i,i,0);r.mTextures[i]=gpu(3000+i,4000+i);
 }
 auto extra=shared(9000,0,0);ctx.sizes[9000]=1u<<20;LLCleanRendererVK::TexStageTicket t{};
 assert(!r.reserveTexStageRetire(t,extra));assert(r.mTexStageRetireBytesUsed==(uint64_t)count*(1u<<20));
 for(uint32_t b=0;b<3;++b){
  for(uint32_t i=1;i<=count;++i)rewrite(r,b,i,1000+i,2000+i,3000+i,4000+i);
  r.drainTexStageRetire();if(b<2)assert(ctx.destroyed.empty());
 }
 assert(r.mTexStageRetireBytesUsed==0&&ctx.destroyed.size()==count*2);
 assert(r.reserveTexStageRetire(t,extra));r.releaseTexStageRetire(t);assert(r.mTexStageRetireBytesUsed==0);
 FakeContext bc;LLCleanRendererVK bytes;bytes.mCtx=&bc;
 for(uint32_t i=0;i<8;++i){bc.sizes[5000+i]=16u<<20;auto old=shared(5000+i,0,0);LLCleanRendererVK::TexStageTicket bt{};assert(bytes.reserveTexStageRetire(bt,old));}
 bc.sizes[6000]=1u<<20;auto old=shared(6000,0,0);LLCleanRendererVK::TexStageTicket bt{};assert(!bytes.reserveTexStageRetire(bt,old));
 assert(bytes.mTexStageRetireBytesUsed==LLCleanRendererVK::kTexStageRetireBytes);
 std::cout<<"PASS four16-image batches overlap; all descriptor banks still gate frees; count/128MiB caps and reuse enforced\n";
}
static void shared_retained_handles_remain_bounded() {
#if HAS_RETIRE_HANDLE_BUDGET
 assert(LLCleanRendererVK::kTexStageRetireHandleCap*sizeof(VkImageView)==(32ull<<20));
 FakeContext ctx;LLCleanRendererVK r;r.mCtx=&ctx;
 for(uint32_t i=0;i<8;++i){auto& rec=r.mTexStageRetire[i];rec.state=LLCleanRendererVK::TexStageRetire::State::Reserved;rec.image.image=10+i;rec.views.reserve(r.kTexStageRetireViewCap);rec.legacyViews.reserve(r.kTexStageRetireViewCap);}
 assert(r.texStageRetireHandlesFit(0));assert(!r.texStageRetireHandlesFit(1));
 assert(r.texStageRetireHandlesFit(2ull*r.kTexStageRetireViewCap,&r.mTexStageRetire[0]));
 assert(!r.texStageRetireHandlesFit(2ull*r.kTexStageRetireViewCap+1,&r.mTexStageRetire[0]));
 r.mTextures.resize(2);r.mTextures[1]=gpu(7000,8000);ctx.sizes[7000]=4096;auto old=shared(7000,1,0);LLCleanRendererVK::TexStageTicket t{};
 assert(!r.reserveTexStageRetire(t,old));assert(r.mTexStageInterval.retireRefuseHandles==1&&r.mTexStageRetireBytesUsed==0);
#endif
}
int main(){shared_retained_handles_remain_bounded();overlapping_batches_preserve_fences_and_bounds();many_aliases_have_bounded_lookup_work();handle_key_supports_pointer_and_integer_vk_handles();detached_alias_waits_for_bank_and_legacy_view();orphaned_alias_is_adopted_once();freed_reserved_alias_is_not_transferred_after_handle_reuse();late_created_alias_stays_pinned_past_age_cap();retiring_legacy_handle_reuse_does_not_pin_old_image();source_read_waits_for_recorded_slot_fence_without_descriptor_refs();trial_source_is_pinned_without_a_replacement_retire_record();quarantined_source_use_survives_empty_slot_completion();deferred_world_owner_waits_for_fresh_idle_retry();}
'''
    deferred_method = method(context, "bool retryDeferredWorldShutdown(double now)")
    deferred_fixture = r'''struct DeferredRenderer {
 bool deferred=false; unsigned shutdownCalls=0; unsigned* destroyed=nullptr;
 void shutdown(){++shutdownCalls;}
 bool shutdownDeferred()const{return deferred;}
 ~DeferredRenderer(){if(destroyed)++*destroyed;}
};
struct DeferredOwner {
 DeferredRenderer* mDeferredWorld=nullptr; double mDeferredWorldRetryAt=0;
 bool mWorldInitTried=true; double mWorldInitRetryAt=0;
@DEFERRED_METHOD@
};
static void deferred_world_owner_waits_for_fresh_idle_retry(){
 DeferredOwner owner; assert(owner.retryDeferredWorldShutdown(0.0));
 unsigned destroyed=0; auto* retained=new DeferredRenderer; retained->deferred=true; retained->destroyed=&destroyed;
 owner.mDeferredWorld=retained; owner.mDeferredWorldRetryAt=5.0;
 assert(!owner.retryDeferredWorldShutdown(4.99)); assert(retained->shutdownCalls==0);
 assert(!owner.retryDeferredWorldShutdown(5.0)); assert(retained->shutdownCalls==1);
 assert(owner.mDeferredWorld==retained && owner.mDeferredWorldRetryAt==7.0 && destroyed==0);
 assert(!owner.retryDeferredWorldShutdown(6.99)); assert(retained->shutdownCalls==1);
 retained->deferred=false;
 assert(owner.retryDeferredWorldShutdown(7.0));
 assert(owner.mDeferredWorld==nullptr && destroyed==1);
 assert(!owner.mWorldInitTried && owner.mWorldInitRetryAt==9.0);
 assert(owner.retryDeferredWorldShutdown(9.0)); assert(destroyed==1);
}
'''
    body = body.replace("// DEFERRED_OWNER_SLOT_TESTS", deferred_fixture.replace("@DEFERRED_METHOD@", deferred_method))
    body = body.replace("quarantined_source_use_survives_empty_slot_completion();}",
                        "quarantined_source_use_survives_empty_slot_completion();deferred_world_owner_waits_for_fresh_idle_retry();}")
    out.mkdir(parents=True, exist_ok=True)
    target = out / "texture_retire_alias_checks.cpp"
    target.write_text(body)
    cpp_sha = hashlib.sha256(target.read_bytes()).hexdigest()
    renderer = root / "indra/newview/vkgfx/llcleanrenderervk.cpp"
    header = root / "indra/newview/vkgfx/llcleanrenderervk.h"
    (out / "source-receipt.txt").write_text(
        f"llcleanrenderervk.cpp sha256 {hashlib.sha256(renderer.read_bytes()).hexdigest()}\n"
        f"llcleanrenderervk.h sha256 {hashlib.sha256(header.read_bytes()).hexdigest()}\n"
        f"llvulkancontext.cpp sha256 {hashlib.sha256((root / 'indra/newview/vkgfx/llvulkancontext.cpp').read_bytes()).hexdigest()}\n"
        f"generated sha256 {cpp_sha}\n"
        "GPU source-use lifetime: strict retirement waits for the recorded slot fence; trial source is pinned through its slot\n"
        "extracted production helpers: texStageHandleKey, texStageHasView\n"
        "tested handle kinds: pointer and integer\n"
        "operation instrumentation: numeric handle key conversions and std::find equality comparisons\n")
    return target


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--run", action="store_true")
    a = ap.parse_args()
    cpp = generate(a.source_root.resolve(), a.out.resolve())
    print(cpp)
    if a.run:
        exe = cpp.with_suffix("")
        out = a.out.resolve()
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
        cmd = ["/usr/bin/c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-pthread",
               str(cpp), "-o", str(exe)]
        log = out / "compile-run.log"
        with log.open("w") as f:
            f.write("compile argv: " + shlex.join(cmd) + "\n")
            f.write(f"cwd: {out}\ncore dumps: disabled where supported\n")
            subprocess.run(cmd, check=True, cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT,
                           timeout=60, preexec_fn=preexec_fn)
            f.write("run argv: " + shlex.join([str(exe)]) + "\n")
            subprocess.run([str(exe)], check=True, cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT,
                           timeout=10, preexec_fn=preexec_fn)
        print(f"PASS; see {log}")
        print("PASS: detached aliases, retirement ownership, and reserved-handle reuse")


if __name__ == "__main__":
    main()
