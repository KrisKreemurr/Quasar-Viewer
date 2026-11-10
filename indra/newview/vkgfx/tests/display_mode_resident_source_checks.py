#!/usr/bin/env python3
"""Source-bound invariants for output-only mode changes (no viewer/Vulkan run)."""
import pathlib
import sys


def body(source: str, signature: str) -> str:
    start = source.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = source.find("{", start)
    assert brace >= 0
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : i]
    raise AssertionError(f"unterminated function: {signature}")


def main(root: pathlib.Path) -> None:
    vk = root / "indra/newview/vkgfx"
    renderer = (vk / "llcleanrenderervk.cpp").read_text()
    context = (vk / "llvulkancontext.cpp").read_text()
    # A settings/monitor event must not orphan a renderer-owned pending build.
    for begin, end in (("void LLVulkanContext::setDisplayHdr(",
                        "LLVulkanContext::DisplayHdrInfo LLVulkanContext::displayHdrInfo("),
                       ("void LLVulkanContext::onDisplayChanged()",
                        "void LLVulkanContext::refreshDisplayCalibration()")):
        setter = context[context.index(begin):context.index(end, context.index(begin))]
        assert "mOutputPreviewValid = false" in setter
        assert "mOutputRequestValid = false" not in setter
        assert "mOutputRequestValid=false" not in setter

    device = (vk / "vk_device.cpp").read_text()
    header = (vk / "vk_device.h").read_text()

    relief = body(device, "bool VkDeviceContext::runRelief(")
    assert "reliefSuppressedForCurrentThread()" in relief
    assert relief.index("reliefSuppressedForCurrentThread()") < relief.index("mRelief()")
    assert "runRelief(" in body(device, "VkResult VkDeviceContext::allocateMemory(")
    assert "runRelief(" in body(device, "bool VkDeviceContext::heapBudgetPreflight(")
    soft_fail = body(device, "bool VkDeviceContext::softFail(")
    assert "++mAllocFailures" in soft_fail
    assert "if (!reliefSuppressedForCurrentThread())" in soft_fail
    assert "++mPressureAllocFailures" in soft_fail and "mPressureLastFailBytes = bytes" in soft_fail
    assert "pressureRelevantAllocFailures()" in header
    assert "pressureRelevantLastFailBytes()" in header
    assert "pressureRelevantAllocFailures()" in renderer
    assert "const uint32_t fails = mCtx->pressureRelevantAllocFailures()" in renderer
    assert "in.failBytes = mCtx->pressureRelevantLastFailBytes()" in renderer
    assert "inline static thread_local bool sSuppressed" in header
    assert "mPrevious(sSuppressed)" in header and "sSuppressed = mPrevious" in header

    target_prep = body(renderer, "bool LLCleanRendererVK::prepareDisplayTargets(")
    assert "NoReliefScope" in target_prep and "SoftAllocScope" in target_prep
    assert "destroyDisplayTargetStage()" in target_prep
    for key in ("keyAA", "keyAAImage", "keyAAW", "keyFinal", "keyFinalImage", "keyFinalW",
                "keySphere", "keySphereImage", "keySphereW"):
        assert key in target_prep
    target_key = body(renderer, "bool LLCleanRendererVK::displayTargetKeyMatchesCurrent()")
    for key in ("keyAA", "keyAAImage", "keyAAW", "keyFinal", "keyFinalImage", "keyFinalW",
                "keySphere", "keySphereImage", "keySphereW"):
        assert key in target_key
    hdr_prep = body(context, "bool stageHdrOutput(")
    assert hdr_prep.index("NoReliefScope") < hdr_prep.index("mHdrStageFrame.prepare(")
    assert "mHdrStageEncode.prepare(" in hdr_prep
    assert "mDisplayFrame.prepare(" not in hdr_prep
    hdr_sdr_path = body(context, "if (status.encoding==vkdisplay::Encoding::Sdr)")
    assert "mHdrOutputActive" not in hdr_sdr_path
    assert "validForExtent" in context and "validForTarget" in context
    assert "compositionCanRecord(state)" in context
    composition = body(context, "bool hdrCompositionValidForCurrentChain() const")
    assert "mSwapchain.valid()" in composition
    assert "mDisplayFrame.linear().image!=VK_NULL_HANDLE" in composition
    assert "mDisplayFrame.linear().view!=VK_NULL_HANDLE" in composition
    assert "mDisplayEncode.validForTarget(status.actual)" in composition
    assert "mayRetireSwapchain(hdrCandidate,hdrStageReady)" in context
    preflight = context.index("mayRetireSwapchain(hdrCandidate,hdrStageReady)")
    recreate_wsi = context.index("mImpl->mSwapchain.recreate(mImpl->mDevice", preflight)
    assert preflight < recreate_wsi
    assert "mImpl->commitHdrOutputStage(actual,extent)" in context
    stage = body(context, "bool stageHdrOutput(const vkdisplay::Status& status, VkExtent2D extent)")
    assert "if (hdrOutputStageMatches(status,extent)) return true" in stage
    assert "mHdrStageReady = hdrOutputStageResourcesMatch(status,extent)" in stage
    assert "mHdrStageReady = hdrOutputStageMatches(status,extent)" not in stage
    assert "makeStagePlan(liveFrameReady,liveEncodeReady)" in stage
    assert "stageNeedsHeapAdmission(plan)" in stage
    assert "stageBudget(plan,mHdrBudget,available)" in stage
    resources = body(context, "bool hdrOutputStageResourcesMatch(const vkdisplay::Status& status, VkExtent2D extent) const")
    assert "stageResourcesMatch(" in resources
    assert "mHdrStageReady" not in resources
    assert "releaseHdrOutputAfterSdrCommit()" in context
    assert context.count("mImpl->mHdrOutputActive || mImpl->mHdrStageReady") == 3
    successful_commit = context.index("mWorld->commitDisplayOutput(")
    assert context.index("releaseHdrOutputAfterSdrCommit()", successful_commit) > successful_commit
    swapchain = (vk / "vk_swapchain.cpp").read_text()
    recreate = body(swapchain, "bool VkSwapchain::recreate(")
    assert "mLastDeviceIdleWaitMs" in recreate and "vkDeviceWaitIdle" in recreate
    assert "lastDeviceIdleWaitMs()" in (vk / "vk_swapchain.h").read_text()
    assert "output_pipeline_wall_ms=" in renderer
    assert "device_idle_wait_ms=" in context

    prereq = body(renderer, "bool LLCleanRendererVK::displayPipelinePrerequisitesSettled()")
    assert "mPipelineJob" in prereq and "mPostJob.pending()" in prereq and "mRezFxJob" in prereq
    assert "mRtPipelineJob" not in prereq and "mLitDbgJob" not in prereq
    init_aa = body(renderer, "void LLCleanRendererVK::initAA()")
    assert "fmt == mColorFormat" not in init_aa
    assert "if (outputTarget || linearDisplay)" in init_aa
    assert '"SMAA edge", false, false' in init_aa
    assert '"SMAA weight", false, false' in init_aa
    assert '"linear HDR SMAA edge", true, false' in init_aa
    assert '"FXAA", false, true' in init_aa
    assert '"SMAA neighbour", false, true' in init_aa
    # The recipe selection is semantic, not inferred from the active SDR format;
    # it therefore behaves the same for B8G8R8A8_UNORM and R8G8B8A8_UNORM.
    register = body(renderer, "bool LLCleanRendererVK::registerDisplayOutputPipeline(")
    assert "registerDisplayPipeline(" in register
    assert "!mDisplayRecipeUnavailable" in register and "before + 1" in register
    private_register = body(renderer, "void LLCleanRendererVK::registerDisplayPipeline(")
    assert "if (mDisplayRecipeUnavailable) return" in private_register
    assert "kDisplayPipelineCapacity" in private_register

    supersession = context[context.index("else if (mImpl->mOutputRequestValid)",
                                         context.index("const bool calibrationDiffers")):]
    supersession = supersession[: supersession.index("\n        }\n        else if (mImpl->mWorld)")]
    assert "pollDisplayOutput()" in supersession
    assert "cancelDisplayOutput" in supersession
    assert "deferSwapchainRecreate = true" in supersession
    assert "mOutputRequestValid = false" in supersession

    request = body(renderer, "bool LLCleanRendererVK::requestDisplayOutput(")
    assert "canRetagPipelineStage" in request
    poll = body(renderer, "void LLCleanRendererVK::pollDisplayOutput()")
    assert "pipelineStageMatches" in poll
    stale = poll.index("pipelineStageMatches")
    assert "destroyDisplayPipelineStage()" in poll[stale : stale + 350]
    assert "!mDisplayPipeStage[i]" in poll
    assert '"output pipeline batch was incomplete"' in poll
    assert "destroyDisplayPipelineStage();" in poll[poll.index('"output pipeline batch was incomplete"') - 140 :]

    ready = body(renderer, "bool LLCleanRendererVK::displayOutputReady(")
    assert "generation != mDisplayRequestGeneration" in ready
    assert "mDisplayRequestValid" in ready and "mDisplayStageReady" in ready
    assert "mDisplayPipeJob) return false" in ready
    assert "displayPipelinePrerequisitesSettled()" in ready
    assert "displayTargetKeyMatchesCurrent()" in ready
    assert "mDisplayTargetRetryPending" in ready
    assert "displayTargetKeyMatchesCurrent()" in poll
    assert "mDisplayTargetRetryAtNs" in poll
    assert "prepareDisplayTargets(mDisplayRequestFormat)" in poll
    assert "mDisplayStageRecipeGeneration = mDisplayRecipeGeneration" in renderer
    assert "++mDisplayRecipeGeneration" in renderer
    cache_hit = poll[poll.index("mDisplayPipeCache.valid") :]
    assert "cachedPipelineStage(" in cache_hit
    assert "mDisplayStageGeneration=cachedStage.request" in cache_hit
    assert "mDisplayStageRecipeGeneration=cachedStage.recipe" in cache_hit

    commit = body(renderer, "bool LLCleanRendererVK::commitDisplayOutput(")
    assert "if (!displayOutputReady(generation)) return false" in commit
    assert "incoming = mDisplayPipeCache" in commit and "mDisplayPipeCache = {}" in commit
    assert "mDisplayPipeCache = previous" in commit
    assert "mColorFormat = mDisplayRequestFormat" in commit
    assert "shutdown()" not in commit and "mDisplayGeneration" not in commit

    mode_start = context.index("bool deferSwapchainRecreate = false;")
    mode_end = context.index("fseAfterRecreate();", mode_start)
    context_switch = context[mode_start:mode_end]
    # The mode transition itself may replace WSI/output resources only; scene generation,
    # world object, and mirrored CPU/GPU scene stores are not reset here.
    assert "mWorld->shutdown" not in context_switch
    assert "delete mImpl->mWorld" not in context_switch
    assert "mDisplayGeneration++" not in context_switch
    assert "mWorldSourceGeneration" not in context_switch

    print("display mode resident source invariants: PASS")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve())
