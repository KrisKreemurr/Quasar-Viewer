/**
 * @file llvrambudgetvk_lines_test.cpp
 * @brief The VRAM LINES (llvrambudgetvk.h, owner decision 2026-11-07): fixed reserves by card
 *        tier, the ordering invariant between every VRAM controller's line, small cards at
 *        least as conservative as the old percentages, RT's headroom under the soft line, and
 *        the old-vs-new table for 4 / 6 / 8 / 12 / 16 / 24 / 32 GB cards.
 *
 * Standalone (no viewer libraries; the lines are pure CPU code):
 *   g++ -std=c++20 -O1 -I indra/newview/vkgfx indra/newview/tests/llvrambudgetvk_lines_test.cpp \
 *       indra/newview/vkgfx/llvrambudgetvk.cpp -o /tmp/vram_lines_test && /tmp/vram_lines_test
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

#include "llvrambudgetvk.h"

#include <cstdio>

// (the status text helpers llvrambudgetvk.cpp links against; unused here)
const char* vkMemCatName(uint32_t) { return "?"; }
const char* vkDrvBucketName(uint32_t) { return "?"; }

namespace
{
int g_fail = 0;
void check(bool ok, const char* what)
{
    if (!ok) std::printf("FAIL %s\n", what);
    if (!ok) ++g_fail;
}
void checkOnce(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

using V = LLVramBudgetVK;
constexpr uint64_t MB = 1ull << 20;
constexpr uint64_t mb(uint64_t b) { return b / MB; }

// The old percentage rules (before 2026-11-07), for the table and the small-card check.
struct Old
{
    double budget, soft, borrow, texCalm, readdFit, recover, rtReserve;
};
Old oldLines(uint64_t c)
{
    const double C = (double)c, B = C - (double)(c / 10u);
    return {B, 0.95 * B, 0.93 * B, 0.88 * B, 0.85 * B, 0.80 * B,
            (double)std::max<uint64_t>(2ull << 30, c / 100 * 30)};
}

// RT's headroom target at a usage that is all non-RT (llcleanrtvk.cpp updateHeadroomBudget).
uint64_t rtRoom(uint64_t dev, uint64_t other, uint64_t reserve)
{
    return dev > other + reserve ? dev - other - reserve : 0;
}
} // namespace

int main()
{
    // ---- tiers (and the ramp past each edge) ----
    checkOnce(V::reserveFor(3072 * MB) == 512 * MB, "3 GB: 512 MB");
    checkOnce(V::reserveFor(4096 * MB - 1) == 512 * MB, "just under 4 GB: 512 MB");
    checkOnce(V::reserveFor(4096 * MB + 512 * MB) == 1024 * MB, "4.5 GB: 1 GB (ramp done)");
    checkOnce(V::reserveFor(7680 * MB) == 1024 * MB, "7.5 GB (a reported 8 GB card): 1 GB");
    checkOnce(V::reserveFor(8192 * MB - 1) == 1024 * MB, "just under 8 GB: 1 GB");
    checkOnce(V::reserveFor(8192 * MB + 512 * MB) == 1536 * MB, "8.5 GB: 1.5 GB");
    checkOnce(V::reserveFor(16384 * MB - 1) == 1536 * MB, "just under 16 GB: 1.5 GB");
    checkOnce(V::reserveFor(16384 * MB + 512 * MB) == 2048 * MB, "16.5 GB: 2 GB");
    checkOnce(V::reserveFor(19149 * MB) == 2048 * MB, "18.7 GB (the 5090 incident): 2 GB");
    checkOnce(V::reserveFor(65536 * MB) == 2048 * MB, "64 GB: 2 GB");
    checkOnce(V::reserveFor(98304 * MB) == 3072 * MB, "96 GB: C/32 (stays under the 97% line)");
    checkOnce(V::reserveFor(1024 * MB) == 256 * MB, "1 GB debug cap: C/4");
    checkOnce(V::hysteresisFor(512 * MB) == 128 * MB && V::hysteresisFor(2048 * MB) == 256 * MB,
              "hysteresis 128 MB .. 256 MB");

    // ---- sweep 512 MB .. 192 GB, 1 MB steps: ordering invariant, monotone budget ----
    {
        bool order = true, mono = true, rt_under_soft = true, hard_ok = true, rig_ok = true;
        uint64_t prevB = 0, prevR = 0;
        for (uint64_t c = 512 * MB; c <= 196608 * MB; c += MB)
        {
            const V::Lines l = V::linesFor(c);
            const bool ok = l.recover < l.readdFit && l.readdFit < l.texCalm && l.texCalm < l.grow && l.grow < l.soft
                         && l.soft < l.budget && l.budget < c && l.recover > 0;
            if (!ok && order) std::printf("     order broken at %llu MB\n", (unsigned long long)mb(c));
            order = order && ok;
            mono = mono && l.budget >= prevB && l.reserve >= prevR;
            prevB = l.budget;
            prevR = l.reserve;
            // RT's usage ceiling (dev - rtReserveFor) = the grow line, under the soft line
            rt_under_soft = rt_under_soft && c - V::rtReserveFor(c) == l.grow && l.grow < l.soft;
            // the crash-risk line (97% of the OS budget; OS >= C) stays above B and the rigged floor
            hard_ok = hard_ok && (double)l.budget < 0.97 * (double)c && (double)V::rigFloorLine(c) < 0.97 * (double)c;
            // rigged floor of an OS budget >= C stays >= this C's budget
            rig_ok = rig_ok && V::rigFloorLine(c) >= l.budget && V::rigFloorLine(c + 4096 * MB) >= l.budget;
        }
        checkOnce(order, "recover < readdFit < texCalm < grow < soft < B < C for every capacity");
        checkOnce(mono, "budget and reserve never fall as the capacity rises (no flapping at tier edges)");
        checkOnce(rt_under_soft, "RT headroom ceiling = grow line, under the soft line");
        checkOnce(hard_ok, "B and the rigged floor stay under the 97%-of-OS crash line");
        checkOnce(rig_ok, "rigged floor (OS - R(OS)) >= B");
    }

    // ---- small cards (2-8 GB): at least as conservative as the old percentages on every line ----
    {
        bool safe = true;
        for (uint64_t c = 2048 * MB; c < 8192 * MB + 512 * MB; c += MB)
        {
            const V::Lines l = V::linesFor(c);
            const Old o = oldLines(c);
            const bool ok = (double)l.budget <= o.budget && (double)l.soft <= o.soft && (double)l.grow <= o.borrow
                         && (double)l.texCalm <= o.texCalm && (double)l.readdFit <= o.readdFit
                         && (double)l.recover <= o.recover;
            if (!ok && safe) std::printf("     less conservative at %llu MB\n", (unsigned long long)mb(c));
            safe = safe && ok;
        }
        checkOnce(safe, "2-8.5 GB: B, soft, grow, texCalm, readdFit, recover <= the old lines");
    }

    // ---- the incident: 5090, VK budget 18.7 GB, usage 16.2 GB (RT holds 1 GB) ----
    {
        const uint64_t dev = 19149 * MB, usage = 16589 * MB, held = 1024 * MB;
        const uint64_t other = usage - held;
        const uint64_t old_res = std::max<uint64_t>(2ull << 30, dev / 100 * 30);
        const uint64_t new_target = rtRoom(dev, other, V::rtReserveFor(dev));
        std::printf("     incident: old reserve %llu MB -> RT %llu MB (floor 384); new reserve %llu MB -> RT %llu MB\n",
                    (unsigned long long)mb(old_res), (unsigned long long)mb(rtRoom(dev, other, old_res)),
                    (unsigned long long)mb(V::rtReserveFor(dev)), (unsigned long long)mb(new_target));
        checkOnce(rtRoom(dev, other, old_res) < held, "incident: the old reserve shrank RT");
        checkOnce(new_target >= held, "incident: the new reserve lets RT keep what it holds");
        checkOnce(usage < V::linesFor(dev).soft, "incident: the ladder stays calm at 16.2 GB");
    }

    // ---- the table (copied into llvrambudgetvk.h and the commit message) ----
    std::printf("\n  card  | old soft  old recov  old RT res. | new R    H  soft   recov  RT res.\n");
    for (uint64_t g : {4u, 6u, 8u, 12u, 16u, 24u, 32u})
    {
        const uint64_t c = g * 1024u * MB;
        const V::Lines l = V::linesFor(c);
        const Old o = oldLines(c);
        std::printf("  %2llu GB | %5llu     %5llu      %5llu       | %4llu  %3llu  %5llu  %5llu  %4llu\n",
                    (unsigned long long)g, (unsigned long long)mb((uint64_t)o.soft), (unsigned long long)mb((uint64_t)o.recover),
                    (unsigned long long)mb((uint64_t)o.rtReserve), (unsigned long long)mb(l.reserve),
                    (unsigned long long)mb(l.hyst), (unsigned long long)mb(l.soft), (unsigned long long)mb(l.recover),
                    (unsigned long long)mb(V::rtReserveFor(c)));
        check(l.recover < l.soft && l.soft < l.budget, "table row ordered");
    }
    // Pinned rows (a change to the tiers must update the header table and this test together).
    checkOnce(mb(V::linesFor(8192 * MB).soft) == 6997 && mb(V::rtReserveFor(8192 * MB)) == 1365, "8 GB: soft 6997, RT 1365");
    checkOnce(mb(V::linesFor(32768 * MB).soft) == 30464 && mb(V::rtReserveFor(32768 * MB)) == 2560, "32 GB: soft 30464, RT 2560");
    checkOnce(mb(V::linesFor(4096 * MB).soft) == 3456 && mb(V::rtReserveFor(4096 * MB)) == 768, "4 GB: soft 3456, RT 768");

    // ---- the ladder itself uses the lines ----
    {
        V lad;
        V::Input in;
        in.osBudget = 10240 * MB;   // R 1.5 GB, H 213 MB: B 8704, soft 8491
        auto useful = [](uint32_t) { return true; };
        in.now = 100.0;
        in.usage = 8400 * MB;
        lad.update(in, useful);
        checkOnce(lad.mode() == 0u && lad.engaged() == 0u && lad.budget() == 8704 * MB, "10 GB: calm under the soft line");
        in.now = 101.0;
        in.usage = 8600 * MB;
        lad.update(in, useful);
        checkOnce(lad.mode() == 1u && lad.engaged() == 1u, "10 GB: soft mode between soft and B");
        in.now = 110.0;
        in.usage = 8800 * MB;
        lad.update(in, useful);
        checkOnce(lad.mode() == 2u, "10 GB: fast mode inside the reserve");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
