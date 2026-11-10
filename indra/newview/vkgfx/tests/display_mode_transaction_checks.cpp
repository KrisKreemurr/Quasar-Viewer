// CPU-only fault-injection checks for the predicates used by the live HDR mode
// transaction. No Vulkan instance/device or viewer objects are required.
#include "../vk_display_mode_transaction.h"

#include <cassert>

int main()
{
    using namespace vkdisplay::transaction;

    // Current HDR frame is valid. A resize candidate allocation refusal must stop
    // before WSI retirement and leave this old chain/frame pair recordable.
    CompositionState activeHdr{true,true,true,true,true,true,true,true,true};
    assert(compositionCanRecord(activeHdr));
    const bool resizeImageAllocationSucceeded=false; // injected device-budget refusal
    const bool hdrStageReady=resizeImageAllocationSucceeded;
    assert(!mayRetireSwapchain(true,hdrStageReady));
    assert(compositionCanRecord(activeHdr));

    // stageHdrOutput derives ready from resource identity without consulting the
    // published-ready bit (which is false for every newly allocated candidate).
    assert(stageResourcesMatch(true,true,true,true));
    assert(!stageResourcesMatch(true,true,false,true));
    assert(!stageResourcesMatch(true,false,true,true));

    // Exercise the same staging plan used by stageHdrOutput: full live-resource
    // reuse performs no heap admission, and encoder-only rebuild preserves the
    // active image reservation rather than replacing it with current free bytes.
    const uint64_t activeImageBudget=192ull<<20;
    auto plan=makeStagePlan(true,true);
    assert(!stageNeedsHeapAdmission(plan));
    assert(stageBudget(plan,activeImageBudget,0)==activeImageBudget);
    plan=makeStagePlan(true,false);
    assert(!stageNeedsHeapAdmission(plan));
    assert(stageBudget(plan,activeImageBudget,16ull<<20)==activeImageBudget);

    // A failed admission for a new extent leaves the committed live image state
    // untouched; the caller does not replace it with the candidate budget.
    plan=makeStagePlan(false,true);
    assert(stageNeedsHeapAdmission(plan));
    const bool candidateImageAdmitted=false;
    assert(!candidateImageAdmitted);
    assert(compositionCanRecord(activeHdr));
    assert(stageBudget(plan,activeImageBudget,0)==0);

    // Defensive fallback after an unexpected WSI change cannot record using the
    // old HDR world against SDR, even if the previous HDR frame still exists.
    CompositionState sdrChainWithHdrWorld{true,false,true,true,true,true,true,true,true};
    assert(!compositionCanRecord(sdrChainWithHdrWorld));

    // Nor may a lost image/view, stale extent, or wrong-format encoder satisfy the
    // HDR path after a failed/recreated allocation.
    CompositionState nullImage=activeHdr; nullImage.frameImage=false;
    CompositionState nullView=activeHdr; nullView.frameView=false;
    CompositionState staleExtent=activeHdr; staleExtent.extentMatches=false;
    CompositionState wrongEncoder=activeHdr; wrongEncoder.targetMatches=false;
    assert(!compositionCanRecord(nullImage));
    assert(!compositionCanRecord(nullView));
    assert(!compositionCanRecord(staleExtent));
    assert(!compositionCanRecord(wrongEncoder));

    // A fully committed SDR chain/world pair is eligible and has no HDR target flag.
    CompositionState committedSdr{true,false,false,false,false,false,false,false,false};
    assert(compositionCanRecord(committedSdr));

    // A failed WSI recreation cannot leave the previous status/resource flags
    // eligible for recording against an invalidated chain.
    CompositionState missingChain=activeHdr; missingChain.chainValid=false;
    assert(!compositionCanRecord(missingChain));

    // A newly registered recipe invalidates the in-flight stage independently of
    // same-key paper-white/peak generation changes.
    const uint64_t recipeBefore=7, stagedRecipe=7, requestBefore=10;
    const uint64_t recipeAfter=8, requestAfter=11;
    uint64_t stagedRequest=requestBefore;
    assert(canRetagPipelineStage(recipeBefore,stagedRecipe));
    if (canRetagPipelineStage(recipeAfter,stagedRecipe)) stagedRequest=requestAfter;
    assert(stagedRequest==requestBefore);
    assert(!pipelineStageMatches(requestAfter,stagedRequest,recipeAfter,stagedRecipe));

    // Rebuilding against the new recipe set permits the latest same-key request.
    const uint64_t rebuiltRecipe=recipeAfter, rebuiltRequest=requestAfter;
    assert(pipelineStageMatches(requestAfter,rebuiltRequest,recipeAfter,rebuiltRecipe));

    // Returning to a cached format after a prior stage was destroyed must latch
    // both current request and cached recipe generations before declaring ready.
    uint64_t staleStageRequest=0, staleStageRecipe=0;
    const auto cached=cachedPipelineStage(22,recipeAfter);
    staleStageRequest=cached.request;
    staleStageRecipe=cached.recipe;
    assert(pipelineStageMatches(22,staleStageRequest,recipeAfter,staleStageRecipe));
}
