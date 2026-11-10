#!/usr/bin/env python3
"""Exercise production dynamic-media recording, submit and publication without a GPU.

Allocator, dynamic producer and Vulkan copy endpoints are fakes. The recording
method and submit callback are extracted intact; publication/drop/retirement use
their production media/ticket blocks. No driver or actual fence behavior is tested.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from texture_prep_service_release_checks import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if root not in out.parents:
        parser.error('output must be below the owned source root')
    out.mkdir(parents=True, exist_ok=True)
    path = root / 'indra/newview/vkgfx/llcleanrenderervk.cpp'
    source = path.read_text()
    record = extract(source, 'void LLCleanRendererVK::recordMediaUpdates(')
    submit = extract(source, 'void LLCleanRendererVK::onTextureFrameSubmitted(')
    commit = extract(source, 'void LLCleanRendererVK::commitAcceptedTexStages(')
    media_commit = extract(commit, 'if (ticket.mediaTarget)')
    drop = extract(source, 'void LLCleanRendererVK::onFrameDropped(')
    drop = drop[drop.index('{') + 1:drop.index('    mFrameOom = false;')]
    retire = extract(source, 'void LLCleanRendererVK::retireTexStageSlot(')
    target = extract(source, 'bool LLCleanRendererVK::texStageMediaTargetCurrent(')
    gate_start = source.index('        if (have || share')
    gate_line = source[gate_start:].splitlines()[0].strip()
    gate = gate_line[len('if ('):-1]
    methods = '\n'.join((record, submit, target, retire)).replace('LLCleanRendererVK::', 'Fixture::')
    prelude = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <climits>
#include <iostream>
#include <map>
#include <vector>
using VkImage = uint64_t; using VkImageView = uint64_t; using VkCommandBuffer = uint64_t;
using S32 = int32_t; using VkFormat = int;
constexpr uint64_t VK_NULL_HANDLE = 0;
constexpr int VK_IMAGE_USAGE_SAMPLED_BIT = 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2,
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 4, VK_IMAGE_ASPECT_COLOR_BIT = 1,
    VK_FORMAT_R8G8B8A8_SRGB = 43, VK_FORMAT_R8G8B8A8_UNORM = 37,
    VKMEM_STAGING = 1, VKMEM_TEXTURE = 2, TEXENC_NONE = 0;
struct LLUUID { int value = 0; bool operator==(const LLUUID&) const = default; };
struct GpuImage { VkImage image = 0; uint64_t memory = 0; VkImageView view = 0; int format = 43; };
struct LLTextureData { int getVKGeneration() { return 0; } struct Raw* getVKCpuData() { return nullptr; } };
struct Raw { void* getData() { return nullptr; } int getDataSize() { return 0; } };
struct LLViewerMediaTexture { LLTextureData* getTextureData() { return nullptr; } };
struct LLViewerTextureManager { static LLViewerMediaTexture* findMediaTexture(LLUUID) { return nullptr; } };
struct LLVKDynTexSource { VkImage image = 100; uint32_t width = 512, height = 512; int gen = 7; };
static LLVKDynTexSource producer; static bool producerPresent = true, deviceLost = false;
static unsigned copies = 0;
bool llvkFindDynamicTexture(LLUUID, LLVKDynTexSource& out) { out = producer; return producerPresent; }
bool vkDeviceIsLost() { return deviceLost; }
void llvkRecordDynamicTextureCopy(VkCommandBuffer, const LLVKDynTexSource&, VkImage dst, uint32_t) { assert(dst); ++copies; }
uint32_t texMipLevels(uint32_t w, uint32_t h) { uint32_t n = 0; for (auto v = std::max(w,h); v; v >>= 1) ++n; return n; }
int texAltFormat(int fmt) { return fmt; }
using LLImageRaw = Raw;
struct TexStageCpuTimer { TexStageCpuTimer(unsigned&, unsigned&) {} };
struct VkMemScope { explicit VkMemScope(int) {} };
struct Context {
    struct SoftAllocScope { explicit SoftAllocScope(Context*) {} };
    struct NoReliefScope { ~NoReliefScope() {} };
    bool fail = false; unsigned allocations = 0, destroyed = 0; uint64_t nextImage = 1000;
    GpuImage createImage2D(uint32_t, uint32_t, int fmt, int, int, uint32_t, int) {
        ++allocations; return fail ? GpuImage{} : GpuImage{++nextImage, 1, nextImage + 1, fmt};
    }
    void destroyImage(GpuImage& image) { if (image.image) ++destroyed; image = {}; }
    void destroyView(VkImageView) {}
};
using VkDeviceContext = Context;
struct LLVKHitch { static void count(const char*, unsigned) {} };
struct LLVKTexturePrepService {
    struct Ticket { bool valid() const { return false; } };
    bool gpuUseRetired(Ticket) { return true; }
};
struct LLVulkanContext {
    static LLVulkanContext& instance() { static LLVulkanContext c; return c; }
    LLVKTexturePrepService* texturePrepService() { return nullptr; }
};
struct Fixture {
    static constexpr uint32_t kTexStageTicketCap = 128, kTexDescSets = 3;
    struct MediaSlot { LLUUID id{1}; int gen = -1; };
    struct TexStageTicket {
        enum class Life { Free, Worker, Ready, GpuQueued, Recorded, Accepted, AcceptedStale, Committed, Quarantine };
        enum class GpuOp { None, Reencode, Trial };
        Life life = Life::Free; GpuOp gpuOp = GpuOp::None;
        uint64_t id = 0, key = 0, stageBytes = 0; LLUUID textureId;
        bool mediaTarget = false, gpuOnlyMedia = false, gpuRetired = false, gpuUsesExpectedOld = false;
        uint32_t mediaSlot = 0, width = 0, height = 0, levels = 0, recordedSlot = 0;
        int generation = -1, destinationFormat = 43; GpuImage candidate; VkImage expectedOld = 0;
        VkImageView gpuSourceView = 0;
        struct Recipe { int encode = 0; } prepRecipe;
        LLVKTexturePrepService::Ticket serviceTicket;
    };
    struct Encoder { bool wasRecorded(VkImage, uint32_t) { return true; } };
    struct TexFrameMediaGen { uint32_t slot; LLUUID id; VkImage image; int generation; };
    struct Stats { unsigned refuseTickets=0, refuseAllocation=0, admitted=0, recorded=0, submitted=0, published=0, refuseEncoder=0; } mTexStageInterval;
    unsigned mTexStageRetireCpuNs = 0, mTexStageCpuUnavailable = 0;
    Context context; Context* mCtx = &context; Encoder* mTexEnc = nullptr;
    std::vector<GpuImage> mTextures = std::vector<GpuImage>(1);
    std::vector<uint16_t> mVkTexW{0}, mVkTexH{0}; std::vector<int> mVkTexGen{-1};
    std::vector<uint64_t> mVkTexShared{0}; std::map<uint32_t, MediaSlot> mMediaSlots{{0, MediaSlot{}}};
    std::array<TexStageTicket, kTexStageTicketCap> mTexStageTickets{};
    uint64_t mTexStageNextId=1, mTexFrameStageIds[kTexStageTicketCap]{};
    uint64_t mTexRetireStageIds[kTexDescSets][kTexStageTicketCap]{};
    uint32_t mTexRetireStageCount[kTexDescSets]{}, mTexFrameStageCount=0, mFrameSlot=0;
    TexFrameMediaGen mTexFrameMediaGen[64]{}; uint32_t mTexFrameMediaGenCount=0, mMediaScanAfter=0;
    unsigned mTexAlphaGen=0, touches=0, retired=0; bool mLastFrameDropQuiesced=false;
    void recordMediaUpdates(VkCommandBuffer); void onTextureFrameSubmitted(); void retireTexStageSlot(uint32_t);
    bool texStageMediaTargetCurrent(const TexStageTicket&) const;
    void startImmutableMediaStage(uint32_t, MediaSlot&, LLTextureData*, Raw*) { assert(false); }
    void texOomNote(uint64_t) {} void touchTexSlot(uint32_t) { ++touches; }
    void releaseTexSlot(uint32_t slot) { assert(mTextures[slot].image); ++retired; mTextures[slot]={}; }
    void releaseTexStageService(TexStageTicket&, bool) {} void releaseTexStageRetire(TexStageTicket&) {}
    void retireTexStageGpuUse(uint32_t) {} void drainTexStageRetire() {}
    void discardTexStageCandidate(TexStageTicket& ticket) { mCtx->destroyImage(ticket.candidate); }
    void commitAcceptedTexStages() {
        for (auto& ticket : mTexStageTickets) {
            if (ticket.life != TexStageTicket::Life::Accepted) continue;
'''
    generated = prelude + media_commit + '\n        }\n    }\n    void drop(bool submissionsQuiesced) {\n' + drop + '\n    }\n};\n' + methods
    generated += '\nbool admitsLogicalSlot(bool have, bool share, bool is_dyn, LLTextureData* glimg, const LLVKDynTexSource& dyn) { (void)is_dyn; (void)glimg; (void)dyn; return ' + gate + '; }\n'
    generated += r'''
int main() {
    LLTextureData gpuOnlySource;
    assert(admitsLogicalSlot(false, false, true, &gpuOnlySource, producer));
    assert(!admitsLogicalSlot(false, false, false, &gpuOnlySource, producer));
    using Life = Fixture::TexStageTicket::Life;
    Fixture initial;
    initial.recordMediaUpdates(1);
    assert(initial.mTexFrameStageCount == 1); // first GPU-only composite must progress without raw pixels
    assert(initial.mTextures[0].image == 0 && initial.mMediaSlots[0].gen == -1);
    const auto candidate = initial.mTexStageTickets[0].candidate.image; assert(candidate);
    initial.onTextureFrameSubmitted();
    assert(initial.mTextures[0].image == candidate && initial.mMediaSlots[0].gen == producer.gen);
    assert(initial.mVkTexW[0] == producer.width && initial.touches == 1);
    initial.retireTexStageSlot(0); assert(initial.mTexStageTickets[0].life == Life::Free);
    const auto count = copies; initial.recordMediaUpdates(1); assert(copies == count); // unchanged generation
    ++producer.gen; initial.recordMediaUpdates(1);
    assert(copies > count && initial.mMediaSlots[0].gen != producer.gen);
    initial.onTextureFrameSubmitted(); assert(initial.mMediaSlots[0].gen == producer.gen);
    ++producer.gen; initial.recordMediaUpdates(1); initial.drop(true);
    assert(initial.mMediaSlots[0].gen != producer.gen); // dropped in-place copy must retry
    initial.recordMediaUpdates(1); initial.onTextureFrameSubmitted();
    assert(initial.mMediaSlots[0].gen == producer.gen);
    producer.width = producer.height = 1024;
    initial.context.fail = true; initial.recordMediaUpdates(1);
    assert(initial.mTextures[0].image == candidate && initial.mTexFrameStageCount == 0);
    initial.context.fail = false; initial.recordMediaUpdates(1);
    assert(initial.mTextures[0].image == candidate && initial.retired == 0);
    initial.drop(true); assert(initial.context.destroyed == 1 && initial.mTextures[0].image == candidate);
    assert(initial.mTexStageTickets[0].life == Life::Free);
    initial.recordMediaUpdates(1); initial.onTextureFrameSubmitted();
    assert(initial.mTextures[0].image != candidate && initial.retired == 1);
    initial.retireTexStageSlot(0);
    Fixture uncertain; uncertain.recordMediaUpdates(1); uncertain.drop(false);
    assert(uncertain.mTexStageTickets[0].life == Life::Quarantine && uncertain.context.destroyed == 0);
    uncertain.recordMediaUpdates(1); assert(uncertain.context.allocations == 1); // bounded retained ownership
    Fixture remapped; remapped.recordMediaUpdates(1); remapped.mMediaSlots[0].id.value = 2;
    remapped.onTextureFrameSubmitted(); assert(!remapped.mTextures[0].image);
    remapped.retireTexStageSlot(0); assert(remapped.context.destroyed == 1);
    Fixture absent; producer.image = 0; absent.recordMediaUpdates(1); assert(!absent.context.allocations);
    producer.image = 100;
    Fixture saturated; for (auto& ticket : saturated.mTexStageTickets) ticket.life = Life::Quarantine;
    saturated.recordMediaUpdates(1); assert(!saturated.context.allocations);
    for (auto fmt : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}) {
        Fixture view; view.mTextures[0].format=fmt; view.recordMediaUpdates(1); view.onTextureFrameSubmitted();
        assert(view.mTextures[0].image && view.mTextures[0].format==fmt);
    }
    std::cout << "PASS GPU-only media first-load/resize/submit/drop/remap/capacity and both view formats\n";
}
'''
    cpp = out / 'dynamic_media_publication.cpp'
    cpp.write_text(generated)
    (out / 'sources.json').write_text(json.dumps({str(path): hashlib.sha256(path.read_bytes()).hexdigest()}, indent=2)+'\n')
    if args.run:
        exe = out / 'dynamic_media_publication'
        subprocess.run(['/usr/bin/c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', str(cpp), '-o', str(exe)], check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()
