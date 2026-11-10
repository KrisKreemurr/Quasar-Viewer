// Regression fixture for the rigged pre-skin capacity (indra/newview/vkgfx/llcleanrigsizing.h).
// Built and run by scripts/check/checks/rig_sizing.py (every `make`).
//
// 2026-11-07 "popen disassembly": rigged mesh parts went missing on a calm RTX 3060 laptop
// (#VKRigDrop "rigged_capacity 14 (3914 verts) ... pressure 0"). The capacity followed the GPU's
// visible demand, read back kFrameSlots frames late, with a headroom the CPU ceiling clamped, so
// a crowd arriving between two readbacks dropped faces for several frames. This simulates the
// renderer's loop frame by frame -- the CPU scan, the request, rigPrestep applying it at the next
// frame, the cull's allocation (cache arena first, the spill into per-frame ranges, the
// deterministic drop), the lagged readback -- around the REAL target / capacity code, with
// demand ramps shaped like the report's logs, and checks:
//   1. calm: a face is dropped for capacity only on the frame its instance arrived (the request
//      made that frame lands at the next prestep) -- never two frames running, never after;
//   2. the same ramps with the old (demand-following) policy DO drop for several frames running
//      (the reproduction: the fixture sees the bug it guards against);
//   3. every capacity stays inside the rigged budget (no unbounded growth), pressed or calm;
//   4. under pressure faces may drop, but every drawn face's range lies inside the capacity and
//      no two drawn ranges overlap (never wrong data), and once the pressure ends they return.
#include "vkgfx/llcleanrigsizing.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace
{
    int g_fail = 0;
    void check(bool ok, const char* what, int frame)
    {
        if (!ok)
        {
            std::printf("FAIL (frame %d): %s\n", frame, what);
            ++g_fail;
        }
    }

    struct Inst
    {
        uint32_t verts;
        bool cached;    // skin-cache eligible (reduced no-shadow / #632 held-frame wearer)
        bool visible;   // in some pass this frame (the cull's visible demand)
        int arrive;     // first frame it exists
    };

    struct Caps { uint64_t dyn = 4097, cache = 0, items = 65; };   // = the log's gen 1

    struct Result { int drop_frames = 0, max_run = 0, late_drops = 0; uint64_t max_total = 0; };

    // One session: `calm` selects the policy under test (false = the pre-fix demand-following
    // targets, the pressed path). `budget_verts` bounds everything.
    Result simulate(const std::vector<Inst>& world, int frames, bool calm_floor, uint64_t budget_verts,
                    int pressure_until = -1)
    {
        constexpr int kLag = 3;   // kFrameSlots: the readback is this many frames old
        Result res;
        Caps caps;
        LLCleanRigSizing::Targets pending{};
        bool have_pending = false;
        std::deque<uint64_t> rb_v, rb_w;   // GPU demand words in flight
        uint64_t peak_v = 0, peak_w = 0;
        uint64_t ub_v = 0, ub_c = 0, ub_i = 0;
        uint32_t last_count = 0;
        int run = 0;
        for (int f = 0; f < frames; ++f)
        {
            // rigPrestep: last frame's request, applied before this frame records.
            if (have_pending)
            {
                const uint64_t max_verts = budget_verts;
                uint64_t dyn = LLCleanRigSizing::dynCapacityFor(caps.dyn, pending.dyn, false);
                uint64_t cache = LLCleanRigSizing::cacheCapacityFor(caps.cache, pending.cache, false);
                dyn = std::min(dyn, max_verts);
                cache = std::min(cache, max_verts - dyn);
                caps.dyn = dyn;
                caps.cache = cache;
                caps.items = std::max(caps.items, pending.items);
                have_pending = false;
            }
            const bool pressed = f < pressure_until;
            // The world this frame.
            uint32_t count = 0;
            bool arrived = false;
            for (const Inst& in : world)
                if (in.arrive <= f) { ++count; arrived |= in.arrive == f; }
            // buildRiggedFrame's scan (skin count change, every 15 frames).
            if (count != last_count || f % 15 == 0)
            {
                last_count = count;
                ub_v = ub_c = ub_i = 0;
                for (const Inst& in : world)
                    if (in.arrive <= f) { (in.cached ? ub_c : ub_v) += in.verts; ++ub_i; }
            }
            // The cull: cache-eligible visible instances take arena ranges (bump; the overflow
            // spills into per-frame ranges), then the per-frame ranges in order; past the
            // capacity a face is dropped -- and a drawn face's range must fit and not overlap.
            uint64_t bump = 0, dyn_need = 0, items = 0, drawn_end = 0, dropped = 0;
            for (const Inst& in : world)
            {
                if (in.arrive > f || !in.visible) continue;
                ++items;
                if (in.cached && bump + in.verts <= caps.cache) { bump += in.verts; continue; }
                dyn_need += in.verts;
                if (drawn_end + in.verts <= caps.dyn && items <= caps.items)
                {
                    const uint64_t base = drawn_end;   // ranges handed out in order: disjoint
                    drawn_end += in.verts;
                    check(base + in.verts <= caps.dyn, "a drawn range runs past the capacity", f);
                }
                else ++dropped;
            }
            // (drops while pressed are the intended degradation: not counted against the policy)
            if (dropped && !pressed)
            {
                ++res.drop_frames;
                ++run;
                if (!arrived) ++res.late_drops;
            }
            else run = 0;
            res.max_run = std::max(res.max_run, run);
            res.max_total = std::max(res.max_total, caps.dyn + caps.cache);
            // The readback this frame (kLag frames old).
            rb_v.push_back(dyn_need);
            rb_w.push_back(items);
            if ((int)rb_v.size() > kLag)
            {
                peak_v = std::max(peak_v, rb_v.front());
                peak_w = std::max(peak_w, rb_w.front());
                rb_v.pop_front();
                rb_w.pop_front();
            }
            // The request (this frame's targets, applied at the next prestep).
            LLCleanRigSizing::Inputs in;
            in.ub_verts = ub_v;
            in.ub_cache = ub_c;
            in.ub_items = ub_i;
            in.have_gpu = peak_w != 0;
            in.peak_v = peak_v;
            in.peak_w = peak_w;
            in.cache_measured = caps.cache != 0 && in.have_gpu;
            in.cache_compact = bump;
            in.cache_used = bump;
            in.cache_cap = caps.cache;
            in.headroom_pct = pressed ? 25 : 50;
            in.calm = calm_floor && !pressed;
            in.budget_verts = budget_verts;
            in.budget_items = 1u << 20;
            pending = LLCleanRigSizing::targets(in);
            have_pending = true;
            check(pending.dyn + pending.cache <= budget_verts + 65536u, "targets exceed the budget", f);
            check(caps.dyn + caps.cache <= budget_verts, "capacity exceeds the budget", f);
        }
        return res;
    }

    // A venue filling up like the 1.5.0.6 log (5k -> 418k -> 542k -> 740k visible skinned
    // vertices over ~1400 frames): avatars of 15-40 rigged parts arriving in bursts, some
    // cache-eligible, a few parts out of every view.
    std::vector<Inst> crowd(uint32_t seed)
    {
        std::vector<Inst> w;
        uint32_t s = seed;
        auto rnd = [&](uint32_t n) { s = s * 1664525u + 1013904223u; return (s >> 8) % n; };
        // Arrivals come in BURSTS (a region's avatars resolving together after login / a
        // teleport, a group walking in): the log's 76k -> 125k in 4 frames, 400k -> 536k in 37.
        int f = 2;
        for (int av = 0; av < 60; ++av)
        {
            if (rnd(4) == 0) f += (av < 20) ? 2 + (int)rnd(8) : 40 + (int)rnd(200);
            const uint32_t parts = 15 + rnd(26);
            const bool held = rnd(3) == 0;   // a #632 held-frame avatar: all its parts cache-eligible
            for (uint32_t p = 0; p < parts; ++p)
                w.push_back({200u + rnd(1200), held, rnd(10) != 0, f + (int)rnd(4)});
        }
        return w;
    }
}

int main()
{
    const uint64_t calm_budget = 40ull << 20;   // ~1.1 GB of 20-40 B vertices: a calm 6 GB card
    for (uint32_t seed = 1; seed <= 8; ++seed)
    {
        const std::vector<Inst> w = crowd(seed);
        // 1. calm, the fix: drops only on arrival frames, never two running.
        const Result fixed = simulate(w, 2000, true, calm_budget);
        check(fixed.late_drops == 0, "calm: a face stayed dropped after the frame it arrived", (int)seed);
        check(fixed.max_run <= 1, "calm: faces dropped on consecutive frames", (int)seed);
        const Result old = simulate(w, 2000, false, calm_budget);   // (reported for seed 1)
        // 3. budget bound, also when the budget is far under the crowd.
        const uint64_t tight = 300000;
        const Result squeezed = simulate(w, 2000, true, tight);
        check(squeezed.max_total <= tight, "capacity grew past a tight budget", (int)seed);
        // 4. pressure for the first 600 frames: drops allowed then; once calm again, the
        //    dropped faces return within a frame (one grow) and none drops late.
        const Result pressed = simulate(w, 2000, true, calm_budget, 600);
        check(pressed.late_drops == 0 && pressed.max_run <= 1, "faces did not return once the pressure ended", (int)seed);
        if (seed == 1)
            std::printf("seed 1: fixed %d drop frames (max run %d, late %d) | old %d (max run %d, late %d)\n",
                        fixed.drop_frames, fixed.max_run, fixed.late_drops, old.drop_frames, old.max_run,
                        old.late_drops);
    }
    // 2. THE REPRODUCTION (the report's 05:06:23 line: demand 417989 over a 414445 capacity):
    //    a settled crowd of ~100k visible skinned vertices, then a burst of ~300k (a group's
    //    meshes resolving together). The old policy drops for the readback lag; the fix never
    //    drops past the burst's own frame.
    {
        std::vector<Inst> w;
        for (int i = 0; i < 120; ++i) w.push_back({850u, i % 4 == 0, true, 1});
        for (int i = 0; i < 350; ++i) w.push_back({860u, i % 4 == 0, true, 400});
        const Result old = simulate(w, 800, false, calm_budget);
        check(old.max_run >= 2 && old.late_drops > 0,
              "the fixture no longer reproduces the old lagged-capacity drops (model broken?)", 0);
        const Result fixed = simulate(w, 800, true, calm_budget);
        check(fixed.late_drops == 0 && fixed.max_run <= 1, "calm: the burst dropped faces past its own frame", 0);
        std::printf("burst: old %d drop frames (max run %d) | fixed %d (max run %d)\n", old.drop_frames, old.max_run,
                    fixed.drop_frames, fixed.max_run);
    }
    // 4b. a crowd present from frame 0 (a login into a full venue), then pressure for a while:
    //    calm frames never drop late, before or after it.
    {
        std::vector<Inst> w;
        for (int i = 0; i < 400; ++i) w.push_back({900u, i % 3 == 0, true, 0});
        const Result calm = simulate(w, 1200, true, 40ull << 20, -1);
        check(calm.late_drops == 0 && calm.max_run <= 1, "a static crowd dropped faces while calm", 0);
        const Result after = simulate(w, 1200, true, 40ull << 20, 600);
        check(after.late_drops == 0 && after.max_run <= 1, "a static crowd dropped faces after the pressure", 0);
    }
    // 5. a refused grow's backoff: capped (~5 s) while calm, the doubling (to ~80 s) only pressed.
    for (uint32_t k = 0; k < 40; ++k)
        check(LLCleanRigSizing::growBackoffFrames(k, true) <= LLCleanRigSizing::kCalmGrowBackoffFrames,
              "calm: a refused grow backs off past the calm cap", (int)k);
    check(LLCleanRigSizing::growBackoffFrames(0, false) == 300u && LLCleanRigSizing::growBackoffFrames(9, false) == 4800u,
          "pressed: the refused-grow backoff no longer doubles 300 -> 4800 frames", 0);
    if (g_fail)
    {
        std::printf("%d rigged sizing check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("rigged sizing: OK\n");
    return 0;
}
