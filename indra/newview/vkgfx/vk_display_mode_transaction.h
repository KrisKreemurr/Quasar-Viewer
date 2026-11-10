// Small pure state predicates shared by the live display transaction and its
// fault-injection fixture. This prevents mode flags from making invalid GPU
// resources appear eligible for recording.
#pragma once

#include <cstdint>

namespace vkdisplay::transaction
{
struct CompositionState
{
    bool chainValid = false;
    bool chainHdr = false;
    bool rendererHdr = false;
    bool outputActive = false;
    bool frameImage = false;
    bool frameView = false;
    bool encoder = false;
    bool extentMatches = false;
    bool targetMatches = false;
};

constexpr bool compositionCanRecord(const CompositionState& state) noexcept
{
    if (!state.chainValid) return false;
    if (!state.chainHdr) return !state.outputActive && !state.rendererHdr;
    return state.rendererHdr && state.outputActive && state.frameImage && state.frameView &&
        state.encoder && state.extentMatches && state.targetMatches;
}

constexpr bool mayRetireSwapchain(bool hdrCandidate, bool hdrCompositionStaged) noexcept
{
    return !hdrCandidate || hdrCompositionStaged;
}

constexpr bool stageResourcesMatch(bool extentMatches, bool targetMatches,
                                   bool frameMatches, bool encoderMatches) noexcept
{
    return extentMatches && targetMatches && frameMatches && encoderMatches;
}

struct StagePlan
{
    bool reuseFrame;
    bool reuseEncoder;
};

constexpr StagePlan makeStagePlan(bool liveFrameReady, bool liveEncodeReady) noexcept
{
    return {liveFrameReady,liveEncodeReady};
}

constexpr bool stageNeedsHeapAdmission(StagePlan plan) noexcept
{
    return !plan.reuseFrame;
}

constexpr uint64_t stageBudget(StagePlan plan, uint64_t activeBudget,
                               uint64_t admittedCandidateBudget) noexcept
{
    return plan.reuseFrame ? activeBudget : admittedCandidateBudget;
}

constexpr bool canRetagPipelineStage(uint64_t recipeGeneration,
                                     uint64_t stagedRecipeGeneration) noexcept
{
    return recipeGeneration != 0 && recipeGeneration == stagedRecipeGeneration;
}

constexpr bool pipelineStageMatches(uint64_t requestGeneration,
                                   uint64_t stagedRequestGeneration,
                                   uint64_t recipeGeneration,
                                   uint64_t stagedRecipeGeneration) noexcept
{
    return requestGeneration != 0 && requestGeneration == stagedRequestGeneration &&
        recipeGeneration != 0 && recipeGeneration == stagedRecipeGeneration;
}

struct PipelineStageGeneration
{
    uint64_t request;
    uint64_t recipe;
};

constexpr PipelineStageGeneration cachedPipelineStage(uint64_t requestGeneration,
                                                       uint64_t cacheRecipeGeneration) noexcept
{
    return {requestGeneration,cacheRecipeGeneration};
}
} // namespace vkdisplay::transaction
