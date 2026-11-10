/**
 * @file llgeomcompact_test.cpp
 * @brief #1426: the in-place geometry compaction planner (vkgfx/llgeomcompact.h) against a
 *        simulated arena: random placements, frees and re-placements between plans, plans
 *        whose copies are recorded a frame late, then compaction to a fixed point. Checked
 *        after every recorded plan: no copy overlaps another (the vkCmdCopyBuffer rules), no
 *        range's bytes are lost, rows / live index / free list / held ranges partition the
 *        arena exactly, the arena end never grows and the plan stays inside its budgets.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../vkgfx/llgeomcompact.h"

#include "../test/lltut.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using namespace LLGeomCompact;

const uint32_t kNone = 0xFFFFFFFFu;

// One arena + the GPU buffer it describes (a word per element: slot id and element index).
struct Sim
{
    FreeList fl;
    LiveIndex li;
    State st;
    uint32_t end = 0;
    struct Row { uint32_t off = kNone, len = 0; };
    std::vector<Row> rows;               // slot -> its range (kNone = not placed)
    std::vector<uint64_t> mem;           // the buffer
    std::vector<uint32_t> uploads;       // placed since the last record (bytes come at record)
    Plan pending;                        // planned, copies not recorded yet
    std::string err;
    uint64_t peakEnd = 0;

    static uint64_t word(uint32_t slot, uint32_t i) { return ((uint64_t)slot << 32) | i; }

    void fail(const std::string& what) { if (err.empty()) err = what; }

    uint32_t alloc(uint32_t slot, uint32_t len)
    {
        uint32_t off = 0;
        if (!fl.take(len, off)) { off = end; end += len; }
        if (end > mem.size()) mem.resize((size_t)end + 1024, 0);
        peakEnd = std::max<uint64_t>(peakEnd, end);
        if (slot >= rows.size()) rows.resize((size_t)slot + 1);
        rows[slot].off = off; rows[slot].len = len;
        li.add(off, len, slot);
        uploads.push_back(slot);
        return off;
    }
    void drop(uint32_t slot)
    {
        Row& r = rows[slot];
        if (r.off == kNone) return;
        if (!fl.release(r.off, r.len)) fail("release refused (overlap) for slot " + std::to_string(slot));
        li.remove(r.off, slot);
        r.off = kNone; r.len = 0;
    }
    bool valid(uint32_t slot, uint32_t off, uint32_t len) const
    {
        return slot < rows.size() && rows[slot].off == off && rows[slot].len == len;
    }

    static bool overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }

    // The planner's step + the caller's row rewrite (the fill).
    void planStep(const Params& p)
    {
        if (!pending.empty()) return;   // (the renderer plans only once the last plan is recorded)
        fl.trimTail(end);
        const uint32_t end0 = end;
        pending.clear();
        plan(fl, li, end, st, p, [this](uint32_t s, uint32_t o, uint32_t l) { return valid(s, o, l); }, pending);
        if (end != end0) fail("plan changed the arena end");
        if (pending.moved > p.budget) fail("plan exceeds its budget");
        if (pending.rounds > p.maxChunks) fail("a slide needs more copy rounds than allowed");
        for (const Move& m : pending.rows)
        {
            if (m.to >= m.from) fail("a move did not lower an offset");
            if (!valid(m.slot, m.from, m.len)) fail("a move's source is not the slot's current range");
            rows[m.slot].off = m.to;
        }
        lastRounds = pending.rounds;
    }
    uint32_t lastRounds = 0;

    // The copy batches as the renderer records them: round 0 = the direct spans + chunk 0 of
    // every slide, round k = chunk k of every slide, a barrier between rounds.
    std::vector<Span> roundRegions(uint32_t k) const
    {
        std::vector<Span> r;
        if (k == 0) r = pending.direct;
        for (const Span& s : pending.slides)
        {
            Span c;
            if (Plan::chunk(s, k, c)) r.push_back(c);
        }
        return r;
    }

    // One vkCmdCopyBuffer within one buffer: the source regions and the destination regions
    // must not overlap (VUID-vkCmdCopyBuffer-pRegions-00117), so the copies may run in any order.
    void checkRound(const std::vector<Span>& r)
    {
        for (size_t a = 0; a < r.size(); ++a)
            for (size_t b = 0; b < r.size(); ++b)
            {
                const uint64_t sa = r[a].from, sae = sa + r[a].len, da = r[a].to, dae = da + r[a].len;
                const uint64_t sb = r[b].from, sbe = sb + r[b].len, db = r[b].to, dbe = db + r[b].len;
                if (a != b && overlaps(sa, sae, sb, sbe)) fail("sources of one copy batch overlap");
                if (a != b && overlaps(da, dae, db, dbe)) fail("destinations of one copy batch overlap");
                if (overlaps(sa, sae, db, dbe)) fail("a source overlaps a destination in one copy batch");
            }
    }

    // recordGeomUploads: the copy rounds, then the uploads of this frame's placements, then the
    // holds return to the free list.
    void record()
    {
        uint32_t rounds = 0;
        for (uint32_t k = 0;; ++k)
        {
            const std::vector<Span> r = roundRegions(k);
            if (r.empty()) break;
            ++rounds;
            checkRound(r);
            std::vector<uint64_t> snap;   // (any order within a batch: read every source first)
            for (const Span& s : r)
                snap.insert(snap.end(), mem.begin() + s.from, mem.begin() + s.from + s.len);
            size_t at = 0;
            for (const Span& s : r)
                for (uint32_t i = 0; i < s.len; ++i) mem[(size_t)s.to + i] = snap[at++];
        }
        if (rounds != pending.rounds) fail("Plan::rounds disagrees with the chunks");
        for (const uint32_t s : uploads)
        {
            const Row& r = rows[s];
            if (r.off == kNone) continue;
            for (uint32_t i = 0; i < r.len; ++i) mem[(size_t)r.off + i] = word(s, i);
        }
        uploads.clear();
        release(fl, pending.holds, end);
        pending.clear();
        if ((++records % verifyEvery) == 0u) verify();
    }
    uint32_t records = 0, verifyEvery = 1;

    void verify()
    {
        // every live range holds its own bytes, the index knows it
        std::vector<uint8_t> cover(end, 0);
        auto mark = [&](uint64_t a, uint64_t n, const char* what)
        {
            if (a + n > end) { fail(std::string(what) + " past the arena end"); return; }
            for (uint64_t i = a; i < a + n; ++i)
                if (cover[i]++) { fail(std::string(what) + " overlaps another range"); return; }
        };
        for (uint32_t s = 0; s < rows.size(); ++s)
        {
            const Row& r = rows[s];
            if (r.off == kNone) continue;
            mark(r.off, r.len, "a live range");
            for (uint32_t i = 0; i < r.len; ++i)
                if (mem[(size_t)r.off + i] != word(s, i)) { fail("slot " + std::to_string(s) + " lost its bytes"); break; }
            auto it = li.byOff.find(r.off);
            if (it == li.byOff.end() || it->second.slot != s || it->second.len != r.len) fail("live index misses a range (slot " + std::to_string(s) + " off " + std::to_string(r.off) + " len " + std::to_string(r.len) + (it == li.byOff.end() ? std::string(" none") : " has slot " + std::to_string(it->second.slot) + " len " + std::to_string(it->second.len)) + ")");
        }
        uint64_t free_n = 0;
        for (const auto& kv : fl.byOff) { mark(kv.first, kv.second, "a free range"); free_n += kv.second; }
        if (free_n != fl.total) fail("free list total drifted");
        if (fl.byOff.size() != fl.bySize.size()) fail("free list maps disagree");
        for (uint64_t i = 0; i < end; ++i) if (!cover[i]) { fail("an element is neither live nor free (lost)"); break; }
        if (fl.rejected) fail("free list rejected a release");
    }

    uint64_t liveElems() const
    {
        uint64_t n = 0;
        for (const Row& r : rows) if (r.off != kNone) n += r.len;
        return n;
    }
};

// Random slot lengths: mostly small, some large (a 65536-vertex range), some tiny slivers.
uint32_t randLen(std::mt19937& rng)
{
    const uint32_t k = rng() % 100u;
    if (k < 5) return 1u + rng() % 4u;
    if (k < 90) return 8u + rng() % 600u;
    if (k < 98) return 600u + rng() % 6000u;
    return 20000u + rng() % 45000u;
}

std::string runChurn(uint32_t seed, uint32_t frames, bool late_record)
{
    std::mt19937 rng(seed);
    Sim sim;
    sim.verifyEvery = 4;   // (the full partition check is O(arena))
    Params p;
    p.budget = 80000; p.maxOps = 512; p.maxScan = 64;
    std::vector<uint32_t> live;
    uint32_t next_slot = 0;
    for (uint32_t f = 0; f < frames && sim.err.empty(); ++f)
    {
        // churn: a burst of loads, a burst of frees (a region left), steady re-placements
        const uint32_t phase = (f / 50u) % 3u;
        const uint32_t adds = phase == 0 ? 30u : 5u, frees = phase == 1 ? 40u : 4u;
        for (uint32_t i = 0; i < adds; ++i)
        {
            const uint32_t s = next_slot++;
            sim.alloc(s, randLen(rng));
            live.push_back(s);
        }
        for (uint32_t i = 0; i < frees && !live.empty(); ++i)
        {
            const size_t k = rng() % live.size();
            sim.drop(live[k]);
            live[k] = live.back(); live.pop_back();
        }
        for (uint32_t i = 0; i < 3 && !live.empty(); ++i)
        {   // re-placement: the old range freed, a new one placed (a changed mirror)
            const uint32_t s = live[rng() % live.size()];
            sim.drop(s);
            sim.alloc(s, randLen(rng));
        }
        sim.planStep(p);
        // the frame records (or, sometimes, the record is skipped: the plan stays pending
        // while the next fill places / frees around it)
        if (!late_record || (rng() % 4u) != 0u) sim.record();
    }
    if (sim.err.empty()) sim.record();
    if (!sim.err.empty()) return "seed " + std::to_string(seed) + ": " + sim.err;

    // Compact to a fixed point with no churn: must terminate, never grow the end, and
    // leave little garbage.
    const uint32_t end0 = sim.end;
    uint32_t idle = 0, steps = 0;
    for (; steps < 20000 && idle < 4 && sim.err.empty(); ++steps)
    {
        sim.planStep(p);
        const bool any = !sim.pending.rows.empty();
        sim.record();
        idle = any ? 0 : idle + 1;
        if (sim.end > end0) sim.fail("compaction grew the arena end");
    }
    sim.verify();
    if (!sim.err.empty()) return "seed " + std::to_string(seed) + ": " + sim.err;
    if (idle < 4) return "seed " + std::to_string(seed) + ": compaction did not settle";
    const uint64_t live_n = sim.liveElems();
    std::ostringstream o;
    if (sim.fl.total * 8u > live_n + 4096u)
        o << "seed " << seed << ": garbage left " << sim.fl.total << " of live " << live_n << " (end " << end0
          << " -> " << sim.end << ")";
    return o.str();
}
}

namespace tut
{
    struct geomcompact_data {};
    typedef test_group<geomcompact_data> geomcompact_test;
    typedef geomcompact_test::object geomcompact_object;
    tut::geomcompact_test geomcompact("LLGeomCompact");

    // Free list: takeAt splits a range, release coalesces, trimTail returns the free tail.
    template<> template<>
    void geomcompact_object::test<1>()
    {
        FreeList fl;
        uint32_t end = 100;
        ensure("release", fl.release(10, 30));
        ensure("takeAt middle", fl.takeAt(20, 5));
        ensure_equals("two pieces", fl.byOff.size(), (size_t)2);
        ensure_equals("total", fl.total, (uint64_t)25);
        ensure("takeAt outside a free range is refused", !fl.takeAt(18, 5));
        ensure("release back", fl.release(20, 5));
        ensure_equals("coalesced", fl.byOff.size(), (size_t)1);
        ensure("overlapping release refused", !fl.release(35, 10));
        ensure("tail", fl.release(60, 40));
        fl.trimTail(end);
        ensure_equals("end trimmed", end, 60u);
        ensure_equals("tail gone", fl.total, (uint64_t)30);
    }

    // Tail fit: the top range moves into the hole below it; the source is held, then freed.
    template<> template<>
    void geomcompact_object::test<2>()
    {
        Sim sim;
        sim.alloc(0, 10); sim.alloc(1, 10); sim.alloc(2, 10);
        sim.record();
        sim.drop(0);
        Params p; p.budget = 1000;
        sim.planStep(p);
        ensure_equals("one tail move", sim.pending.tailMoves, 1u);
        ensure_equals("slot 2 moved to 0", sim.rows[2].off, 0u);
        ensure_equals("source held", sim.pending.holds.size(), (size_t)1);
        sim.record();
        ensure("no error", sim.err.empty());
        ensure_equals("end shrank", sim.end, 20u);
    }

    // Slides: a run after a hole moves down by the hole's length -- one copy when it fits the
    // hole, in hole-sized chunks (one copy round each) otherwise, never past maxChunks.
    template<> template<>
    void geomcompact_object::test<3>()
    {
        Params p; p.budget = 1000; p.maxChunks = 8;
        {   // a 6-element range over a 10-element hole (the 100-element top fits no hole): the
            // tail fit passes the top over and moves the 6 down in one copy round
            Sim sim;
            sim.alloc(0, 10); sim.alloc(1, 6); sim.alloc(2, 1); sim.alloc(3, 100);
            sim.record();
            sim.drop(0); sim.drop(2);
            sim.planStep(p);
            ensure_equals("one move", sim.pending.rows.size(), (size_t)1);
            ensure_equals("one round", sim.pending.rounds, 1u);
            ensure_equals("range moved down", sim.rows[1].off, 0u);
            sim.record();
            ensure("no error (one chunk)", sim.err.empty());
            sim.planStep(p);   // 11 free before a 100-element run: 10 chunks > 8, nothing moves
            ensure("chunk limit holds", sim.pending.rows.empty());
            sim.record();
        }
        {   // chunked: a 10-element run after a 4-element hole = 3 rounds; then the 100-element
            // top slides into the merged 5-element hole? (20 chunks: no) -- and with a hole of
            // 40 below it, it slides in 3 rounds and the end falls
            Sim sim;
            sim.alloc(0, 4); sim.alloc(1, 10); sim.alloc(2, 1); sim.alloc(3, 100);
            sim.record();
            sim.drop(0); sim.drop(2);
            sim.planStep(p);
            ensure_equals("chunked slide", sim.pending.slides.size(), (size_t)1);
            ensure_equals("three rounds", sim.pending.rounds, 3u);
            ensure_equals("run moved down (chunked)", sim.rows[1].off, 0u);
            sim.record();
            ensure("no error (chunked)", sim.err.empty());
            ensure_equals("holes merged", sim.fl.byOff.size(), (size_t)1);
            sim.alloc(4, 40);   // (fills the 5-element hole? no: appended at the end)
            sim.record();
            sim.drop(3);        // the 100-element range goes: 100 free below the new 40
            for (int i = 0; i < 8; ++i) { sim.planStep(p); sim.record(); }
            ensure("no error (settle)", sim.err.empty());
            ensure_equals("compacted to the live size", sim.end, 50u);
        }
        {   // a range larger than every hole: the ranges under it move away first, the free
            // space gathers right below it, then it slides down in chunks
            Sim sim;
            sim.alloc(0, 30); sim.alloc(1, 5); sim.alloc(2, 30); sim.alloc(3, 5); sim.alloc(4, 60);
            sim.record();
            sim.drop(0); sim.drop(2);   // holes of 30 below a 60-element top
            for (int i = 0; i < 8; ++i) { sim.planStep(p); sim.record(); }
            ensure("no error (gather)", sim.err.empty());
            ensure_equals("compacted (gather)", sim.end, 70u);
        }
    }

    // Random churn with every record on time, then with records skipped at random.
    template<> template<>
    void geomcompact_object::test<4>()
    {
        for (uint32_t seed = 1; seed <= 12; ++seed)
        {
            const std::string e = runChurn(seed, 600, false);
            ensure(e, e.empty());
        }
    }

    template<> template<>
    void geomcompact_object::test<5>()
    {
        for (uint32_t seed = 101; seed <= 112; ++seed)
        {
            const std::string e = runChurn(seed, 600, true);
            ensure(e, e.empty());
        }
    }
}
