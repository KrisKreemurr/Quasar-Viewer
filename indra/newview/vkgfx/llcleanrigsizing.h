/**
 * @file llcleanrigsizing.h
 * @brief The rigged pre-skin capacity targets (#559): how many per-frame skinned vertices,
 *        skin-cache arena vertices and work items LLCleanRendererVK::buildRiggedFrame asks
 *        rigPrestep for. Pure arithmetic (no Vulkan, no viewer state) so the regression
 *        fixture scripts/check/test_rig_sizing.cpp drives exactly the code the viewer runs.
 *
 * WHY THE CALM FLOOR (2026-11-07, "popen disassembly": rigged parts not drawing on a calm
 * RTX 3060 laptop, #VKRigDrop "rigged_capacity 14 (3914 verts) ... pressure 0"). The targets
 * followed the GPU's VISIBLE demand, which the CPU reads back kFrameSlots frames late, plus a
 * headroom that the CPU ceiling (peak + 1/8) clamped. A crowd arriving (avatars loading in,
 * finer LODs landing) raised the visible demand past the capacity between two readbacks, and
 * R_RIGCAP dropped the farthest tier's faces until a readback reported it and the next grow
 * landed -- frames of missing mesh pieces with no VRAM pressure at all. The CPU scan already
 * knows every rigged instance in range at the level the cull picks (an upper bound of what
 * any frame can ask for, cache-eligible instances counted as spilled), so while the VRAM
 * ladder is calm the per-frame capacity is never below it: the request made by the frame
 * whose scan saw the crowd lands at the next frame's rigPrestep. Under pressure (ladder rung,
 * #613 rigged rung) the old demand-following targets apply and the cull's deterministic drop
 * is the intended degradation. Every target stays inside the rigged budget (no unbounded
 * growth: budget_verts / budget_items bound the sum).
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace LLCleanRigSizing
{
    struct Inputs
    {
        // The CPU scan (buildRiggedFrame's rescan): every live rigged instance in range, at the
        // level the cull picks, undecimated. ub_cache = the skin-cache-eligible share.
        uint64_t ub_verts = 0;
        uint64_t ub_cache = 0;
        uint64_t ub_items = 0;
        // The GPU readback (kFrameSlots frames old): peak visible demand of the last windows.
        bool     have_gpu = false;
        uint64_t peak_v = 0;
        uint64_t peak_w = 0;
        // The skin cache arena: measured (compact = right after an epoch reset; used = the max of
        // the recent samples), its current capacity, and the bootstrap hold (a measured arena
        // that went back to 0 waits ~30 s before the CPU estimate grows it again).
        bool     cache_measured = false;
        uint64_t cache_compact = 0;
        uint64_t cache_used = 0;
        uint64_t cache_cap = 0;
        bool     cache_boot_hold = false;
        // Policy.
        uint64_t headroom_pct = 50;    // RenderVKRigGrowHeadroomPct (calm), 25 pressed
        bool     shrink_tight = false; // #613 rigged rung engaged
        bool     held_cache = true;    // RenderVKRigHeldSkip (#632): the arena may take 2/3
        bool     calm = true;          // !rigPressed(): no VRAM ladder / rigged rung engaged
        uint64_t budget_verts = 0;     // RigBudget::verts
        uint64_t budget_items = 0;     // RigBudget::items
    };

    struct Targets
    {
        uint64_t cache = 0;   // skin cache arena vertices
        uint64_t dyn = 0;     // per-frame skinned vertices
        uint64_t items = 0;   // work items
        bool     floor_dyn = false;     // the calm floor raised the per-frame target
        bool     budget_clamped = false;// the budget cut the per-frame target under the floor
    };

    // Every in-range rigged vertex at once, the cache-eligible ones spilled into per-frame
    // ranges (a full arena), + 1/8 + 64k: no frame's visible demand can exceed it.
    inline uint64_t calmFloorVerts(uint64_t ub_verts, uint64_t ub_cache)
    {
        const uint64_t all = ub_verts + ub_cache;
        return all + all / 8u + 65536u;
    }
    inline uint64_t calmFloorItems(uint64_t ub_items) { return ub_items + ub_items / 8u + 256u; }

    inline Targets targets(const Inputs& in)
    {
        Targets t;
        // Skin cache arena: the CPU estimate of the cached share (+1/8, it leaks until a new
        // epoch); once measured, the GPU's compact demand (+headroom) grows it and the latest
        // use shrinks it (clamped to the capacity: a full arena resets, it does not ratchet).
        uint64_t want_cache = in.ub_cache ? in.ub_cache + in.ub_cache / 8u + 4096u : 0u;
        if (!in.cache_measured && in.cache_cap == 0u && want_cache != 0u && in.cache_boot_hold)
            want_cache = 0u;
        if (in.cache_measured)
        {
            const uint64_t c_pct = in.shrink_tight ? 12u : in.headroom_pct;
            const uint64_t grow_t = in.cache_compact ? in.cache_compact + in.cache_compact * c_pct / 100u + 4096u : 0u;
            const uint64_t used = std::max(in.cache_used, in.cache_compact);
            const uint64_t shrink_t = used ? used + used * c_pct / 100u + 4096u : 0u;
            const uint64_t cache_gpu = std::max<uint64_t>(grow_t, std::min<uint64_t>(shrink_t, in.cache_cap));
            want_cache = std::min(want_cache, cache_gpu);
        }
        want_cache = std::min(want_cache, in.held_cache ? in.budget_verts / 3u * 2u : in.budget_verts / 3u);

        // Per-frame ranges: the GPU's visible demand + headroom + the arena-full fallbacks, never
        // past the CPU ceiling (it guards against a runaway estimate) or what the budget leaves.
        // Fallbacks: an eighth of the MEASURED cache need once known, else a quarter of the CPU's
        // cached share (the CPU term alone held 2.2M legacy verts at a PEAK where the GPU asked
        // for 29k, 2026-10-14). 2026-10-14 LOD-OFF VANISHING AVATARS: the ceiling (a) counts what
        // the arena cannot hold as per-frame demand (with the arena at 0 the cull skins every
        // cache-eligible instance into a per-frame range) and (b) never clamps under the GPU's
        // measured visible demand (the GPU is authoritative about what it skins).
        const uint64_t fb_v = in.cache_measured ? want_cache / 8u : in.ub_cache / 4u;
        const uint64_t spill_v = in.ub_cache > in.cache_cap ? in.ub_cache - in.cache_cap : 0u;
        const uint64_t ceil_v = std::max<uint64_t>(in.ub_verts + spill_v + fb_v + (in.ub_verts + spill_v) / 8u + 65536u,
                                                   in.have_gpu ? in.peak_v + in.peak_v / 8u + 65536u : 0u);
        uint64_t want_dyn = in.have_gpu ? in.peak_v + in.peak_v * in.headroom_pct / 100u + fb_v + 65536u
                                        : std::min<uint64_t>(ceil_v, std::max<uint64_t>(in.budget_verts / 4u, 4000000ull));
        want_dyn = std::min(want_dyn, ceil_v);
        uint64_t want_items = in.have_gpu ? in.peak_w + in.peak_w * in.headroom_pct / 100u + 256u
                                          : in.ub_items + in.ub_items / 8u + 256u;
        want_items = std::min(want_items, in.ub_items + in.ub_items / 8u + 256u);

        // THE CALM FLOOR (see the file comment): a calm ladder never drops a rigged face for
        // capacity -- the per-frame capacity covers every in-range instance, spilled.
        if (in.calm)
        {
            const uint64_t fv = calmFloorVerts(in.ub_verts, in.ub_cache);
            if (fv > want_dyn) { want_dyn = fv; t.floor_dyn = true; }
            want_items = std::max(want_items, calmFloorItems(in.ub_items));
        }
        const uint64_t dyn_room = in.budget_verts > want_cache ? in.budget_verts - want_cache : 65536u;
        if (want_dyn > dyn_room)
        {
            if (t.floor_dyn) t.budget_clamped = true;
            want_dyn = dyn_room;
        }
        want_items = std::min(want_items, in.budget_items);

        t.cache = want_cache;
        t.dyn = want_dyn;
        t.items = want_items;
        return t;
    }

    // ensureRiggedBuffers' skinned-vertex part: the capacity it re-creates at for a target --
    // a grow takes 1/16 headroom (min 4096: the visible demand creeps up while a venue fills,
    // and an exact fit re-created a 2+ GB buffer several times a second); a shrink takes the
    // target as is; otherwise the capacity is unchanged.
    inline uint64_t dynCapacityFor(uint64_t cap, uint64_t target, bool shrink)
    {
        if (cap < target || shrink) return target + std::max<uint64_t>(4096u, shrink ? 0u : target / 16u);
        return cap;
    }
    inline uint64_t cacheCapacityFor(uint64_t cap, uint64_t target, bool shrink)
    {
        if (cap < target || shrink) return target ? target + 4096u : 0u;
        return cap;
    }

    // A refused rigged grow's backoff (ensureRiggedBuffers), in frames, for the fail_streak-th
    // consecutive refusal: 300 doubling to 4800 (~80 s at 60 fps) while the VRAM ladder is
    // engaged (each attempt may cost a device wait and fail again); while it is calm at most
    // kCalmGrowBackoffFrames (~5 s) -- the faces past the capacity stay dropped for the backoff.
    constexpr uint32_t kCalmGrowBackoffFrames = 300u;
    inline uint32_t growBackoffFrames(uint32_t fail_streak, bool calm)
    {
        const uint32_t f = 300u << std::min<uint32_t>(fail_streak, 4u);
        return calm ? std::min(f, kCalmGrowBackoffFrames) : f;
    }

    // RUNTIME INVARIANT (buildRiggedFrame): with the ladder calm, a capacity drop (R_RIGCAP)
    // may last only the frames between the scan that saw the demand and the grow it requested
    // -- the readback lag plus the prestep. Longer = the capacity failed to follow the demand.
    constexpr uint32_t kCalmDropGraceFrames = 3u /* kFrameSlots readback lag */ + 2u;
}
