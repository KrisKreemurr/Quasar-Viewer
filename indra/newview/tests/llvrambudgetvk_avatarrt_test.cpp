/**
 * @file llvrambudgetvk_avatarrt_test.cpp
 * @brief #1424 avatar RT bands on the VRAM ladder: rung order (farthest first, before every
 *        content rung; the all-avatars rung last), the caps they produce (rtLevel untouched),
 *        the per-avatar distance hysteresis, and the ladder's settle wait after a band.
 *
 * Standalone (no viewer libraries; the ladder is pure CPU code):
 *   g++ -std=c++20 -O1 -I indra/newview/vkgfx indra/newview/tests/llvrambudgetvk_avatarrt_test.cpp \
 *       indra/newview/vkgfx/llvrambudgetvk.cpp -o /tmp/avrt_test && /tmp/avrt_test
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

#include <cmath>
#include <cstdio>
#include <cstring>

// (the status text helpers llvrambudgetvk.cpp links against; unused here)
const char* vkMemCatName(uint32_t) { return "?"; }
const char* vkDrvBucketName(uint32_t) { return "?"; }

namespace
{
int g_fail = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

using B = LLVramBudgetVK;

int firstOf(uint8_t step, int sub = -1)
{
    for (uint32_t i = 0; i < B::rungCount(); ++i)
        if (B::rung(i).step == step && (sub < 0 || B::rung(i).sub == (uint8_t)sub)) return (int)i;
    return -1;
}
} // namespace

int main()
{
    // ---- table ----
    check(B::rungCount() <= B::kMaxRungs, "rung table fits kMaxRungs");
    check(std::strcmp(B::stepName(B::STEP_AVATAR_RT), "?") != 0, "STEP_AVATAR_RT has a name");
    int band[B::kAvatarRtLevels + 1] = {};
    for (uint32_t l = 1; l <= B::kAvatarRtLevels; ++l)
    {
        band[l] = firstOf(B::STEP_AVATAR_RT, (int)l);
        check(band[l] >= 0, "every avatar RT band level has a rung");
        if (l > 1) check(band[l] > band[l - 1], "bands farthest first (level L after L-1)");
    }
    const int content[] = {firstOf(B::STEP_TEX_FAR), firstOf(B::STEP_GEOM_LOD), firstOf(B::STEP_FAR_DRAW),
                           firstOf(B::STEP_TEX_NEAR), firstOf(B::STEP_FAR_SHADOWS), firstOf(B::STEP_MIRRORS),
                           firstOf(B::STEP_PROBES, 3)};
    bool before_content = true;
    for (int c : content) before_content = before_content && c >= 0 && band[B::kAvatarRtLevels] < c;
    check(before_content, "every band before far / near textures, mesh detail, draw distance, far shadows, probe count");
    check(band[1] < firstOf(B::STEP_TEX_OFFSCREEN), "the farthest band is the first rung");
    check(firstOf(B::STEP_RT, B::RT_RIGGED_OUT) > band[B::kAvatarRtLevels], "all-avatars rung after every band");
    check(firstOf(B::STEP_RT, B::RT_RIGGED_OUT) > firstOf(B::STEP_FAR_DRAW), "all-avatars rung stays after draw distance");

    // ---- caps ----
    bool mono = true, rt_untouched = true;
    uint32_t prev = 0;
    for (uint32_t n = 0; n <= B::rungCount(); ++n)
    {
        const B::Caps c = B::capsAt(n);
        mono = mono && c.avatarRtLevel >= prev;
        prev = c.avatarRtLevel;
        uint32_t rt = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (B::rung(i).step == B::STEP_RT) rt = std::max<uint32_t>(rt, B::rung(i).sub);
        rt_untouched = rt_untouched && c.rtLevel == rt;
    }
    check(mono, "capsAt avatarRtLevel is monotone");
    check(B::capsAt((uint32_t)band[2] + 1).avatarRtLevel == 2, "caps after the 40 m rung = level 2");
    check(rt_untouched, "band rungs never move rtLevel (RT_* comparisons unchanged)");
    check(B::capsAt(B::rungCount()).avatarRtLevel == B::kAvatarRtLevels, "whole ladder = nearest band");

    // ---- cutoffs + hysteresis ----
    check(B::avatarRtCutoffM(0) == 0.f && B::avatarRtCutoffM(1) == 64.f && B::avatarRtCutoffM(4) == 12.f, "cutoffs");
    check(!B::avatarRtBandOut(false, 1000.f, 0, false), "level 0 keeps everyone");
    check(!B::avatarRtBandOut(false, 63.f, 1, false), "63 m in at 64 m");
    check(B::avatarRtBandOut(false, 65.f, 1, false), "65 m out at 64 m");
    check(B::avatarRtBandOut(true, 61.f, 1, false), "out stays out inside the 4 m hysteresis");
    check(!B::avatarRtBandOut(true, 59.f, 1, false), "back in past the hysteresis");
    check(!B::avatarRtBandOut(false, 500.f, 4, true), "own / protected never band-dropped");
    check(!B::avatarRtBandOut(false, NAN, 4, false), "unknown distance stays in");
    check(B::avatarRtBandOut(false, 13.f, 4, false) && !B::avatarRtBandOut(false, 11.f, 4, false), "12 m band");

    // ---- ladder: the first step under pressure is the far band, the next waits for it to settle ----
    {
        B lad;
        B::Input in;
        in.osBudget = 10ull << 30;
        in.usage = 8600ull << 20;   // between the soft line (8491 MB) and B (8704 MB): soft mode
        auto useful = [](uint32_t) { return true; };
        in.now = 100.0;
        lad.update(in, useful);
        check(lad.engaged() == 1u && lad.caps().avatarRtLevel == 1u, "soft pressure: first rung = avatars beyond 64 m");
        in.now = 103.0;   // past the 2 s pacing, inside the 5 s band settle
        lad.update(in, useful);
        check(lad.engaged() == 1u, "next rung waits for the band to settle (5 s)");
        in.now = 106.0;
        lad.update(in, useful);
        check(lad.engaged() == 2u, "then the ladder moves on");
        // restore: usage well under the budget -> the bands come back in reverse order
        in.usage = 1ull << 30;
        for (double t = 107.0; t < 200.0 && lad.engaged(); t += 0.5) { in.now = t; lad.update(in, useful); }
        check(lad.engaged() == 0u && lad.caps().avatarRtLevel == 0u, "calm: every band restored");
    }
    // ---- mode 2 (inside the reserve): the fast burst stops at a band and waits for it ----
    {
        B lad;
        B::Input in;
        in.osBudget = 10ull << 30;
        in.usage = (uint64_t)(9.5 * 1073741824.0);   // over the 8.5 GB budget (10 GB - 1.5 GB reserve)
        auto useful = [](uint32_t) { return true; };
        in.now = 100.0;
        lad.update(in, useful);
        check(lad.engaged() == 1u, "mode 2: the burst ends after the first band");
        in.now = 100.5;
        lad.update(in, useful);
        check(lad.engaged() == 1u, "mode 2: the band settles before the next burst");
        in.now = 105.5;
        lad.update(in, useful);
        check(lad.engaged() == 4u && lad.caps().avatarRtLevel == 2u,
              "mode 2 after the settle: tex modes + the 40 m band, then stop");
    }
    // ---- owner test 2026-11-04 (debug cap 7000 MB, usage 7702 MB, OS 22 GB; 8 near avatars):
    //      bands that would free nothing are skipped AND do not cap RT ----
    auto notBand = [](uint32_t r) { return B::rung(r).step != B::STEP_AVATAR_RT; };
    {
        B lad;
        B::Input in;
        in.osBudget = 22668ull << 20;
        in.debugCap = 7000ull << 20;
        in.usage = 7702ull << 20;
        for (double t = 100.0; t < 101.1; t += 0.25) { in.now = t; lad.update(in, notBand); }
        check(lad.engaged() > (uint32_t)band[B::kAvatarRtLevels], "fast mode passed every (useless) band");
        check(lad.caps().avatarRtLevel == 0u, "skipped bands take no avatar out of RT");
        // far avatars arrive: the engaged 64 m band becomes useful and applies now
        auto only64 = [&](uint32_t r) { return (int)r == band[1]; };
        in.now = 101.5;
        lad.update(in, only64);
        check(lad.caps().avatarRtLevel == 1u, "an engaged band applies once it becomes useful");
    }
    // ---- fast mode over the DEBUG cap (no crash risk): an applied band settles 5 s ----
    {
        B lad;
        B::Input in;
        in.osBudget = 22668ull << 20;
        in.debugCap = 7000ull << 20;
        in.usage = 7702ull << 20;
        auto useful = [](uint32_t) { return true; };
        in.now = 100.0;
        lad.update(in, useful);
        check(lad.engaged() == 1u && lad.caps().avatarRtLevel == 1u, "debug cap: burst stops at the first band");
        bool waited = true;
        for (double t = 100.25; t < 104.9; t += 0.25) { in.now = t; lad.update(in, useful); waited = waited && lad.engaged() == 1u; }
        check(waited && !lad.settleBypassed(), "debug cap: no content rung during the band's 5 s settle");
        in.now = 105.1;
        lad.update(in, useful);
        check(lad.engaged() > 1u, "debug cap: the ladder moves on after the settle");
    }
    // ---- failure imminent (usage > 97% of the OS budget): the settle is bypassed ----
    {
        B lad;
        B::Input in;
        in.osBudget = 10ull << 30;
        in.usage = (uint64_t)(9.8 * 1073741824.0);
        auto useful = [](uint32_t) { return true; };
        in.now = 100.0;
        lad.update(in, useful);
        check(lad.engaged() == 3u, "imminent: the burst does not stop at a band");
        in.now = 100.25;
        lad.update(in, useful);
        check(lad.engaged() > 3u, "imminent: the next burst does not wait for a settling band");
        // (bypass flag: set on a check where a settle was skipped)
        B lad2;
        in.now = 100.0;
        in.usage = (uint64_t)(9.5 * 1073741824.0);   // reserve, not imminent: the band settles
        lad2.update(in, useful);
        in.now = 100.25;
        in.usage = (uint64_t)(9.8 * 1073741824.0);   // now imminent
        lad2.update(in, useful);
        check(lad2.settleBypassed() && lad2.engaged() > 1u, "imminent: settle bypass reported to the caller");
    }
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
