/**
 * @file slsoundscrub_test.cpp
 * @brief The sound preview player (#1311): its time display, the playhead's place on the scrub
 *        bar, and the waveform's per-column peaks.
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

#include "../slsoundscrub.h"

#include "../test/lltut.h"

namespace
{
    const U32 RATE = 44100;
    const F32 FULL = 32768.f;   // a sample of FULL is 1.0 on the waveform

    void ensurePeak(const std::string& what, const SLSoundScrub::Peak& peak, F32 low, F32 high)
    {
        tut::ensure_approximately_equals_range((what + " low").c_str(), peak.mLow, low, 1e-6f);
        tut::ensure_approximately_equals_range((what + " high").c_str(), peak.mHigh, high, 1e-6f);
    }
}

namespace tut
{
    struct soundscrub_data {};
    typedef test_group<soundscrub_data> soundscrub_test;
    typedef soundscrub_test::object soundscrub_object;
    tut::soundscrub_test soundscrub("SLSoundScrub");

    // m:ss.cc from frames, rounded down to the hundredth.
    template<> template<>
    void soundscrub_object::test<1>()
    {
        using SLSoundScrub::formatTime;
        ensure_equals("start", formatTime(0, RATE), std::string("0:00.00"));
        ensure_equals("3.21 s", formatTime(3 * RATE + 9261, RATE), std::string("0:03.21"));
        ensure_equals("12.5 s", formatTime(12 * RATE + RATE / 2, RATE), std::string("0:12.50"));
        ensure_equals("one frame short of a second stays under it", formatTime(RATE - 1, RATE), std::string("0:00.99"));
        ensure_equals("a single frame", formatTime(1, RATE), std::string("0:00.00"));
        ensure_equals("past a minute", formatTime(61 * RATE + 2205, RATE), std::string("1:01.05"));
        ensure_equals("ten minutes (the longest upload preview)", formatTime(600 * RATE, RATE), std::string("10:00.00"));
        ensure_equals("another rate", formatTime(22050, 22050), std::string("0:01.00"));
        ensure_equals("no rate", formatTime(RATE, 0), std::string("0:00.00"));
        ensure_equals("the largest clip does not overflow", formatTime(0xFFFFFFFFu, RATE), std::string("1623:11.54"));
        // A 10 s stereo asset decodes to 1764000 data bytes under the decoder's mono header:
        // read the way it plays, that is 882000 mono frames, 20 s.
        ensure_equals("stereo data under a mono header", formatTime(1764000 / 2 / 1, RATE), std::string("0:20.00"));
    }

    // The playhead's place on the bar, and the frame a place on the bar seeks to.
    template<> template<>
    void soundscrub_object::test<2>()
    {
        using SLSoundScrub::fractionAt;
        using SLSoundScrub::frameAt;
        ensure_equals("start", fractionAt(0, 100), 0.f);
        ensure_equals("middle", fractionAt(50, 100), 0.5f);
        ensure_equals("end", fractionAt(100, 100), 1.f);
        ensure_equals("past the end is the end", fractionAt(150, 100), 1.f);
        ensure_equals("an empty clip", fractionAt(5, 0), 0.f);

        ensure_equals("seek to the start", frameAt(0.f, 100), 0u);
        ensure_equals("seek to the middle", frameAt(0.5f, 100), 50u);
        ensure_equals("a seek to the very end plays the last frame", frameAt(1.f, 100), 99u);
        ensure_equals("left of the bar", frameAt(-0.2f, 100), 0u);
        ensure_equals("right of the bar", frameAt(1.5f, 100), 99u);
        ensure_equals("an empty clip seeks nowhere", frameAt(0.5f, 0), 0u);

        const U32 frames = 10 * RATE, frame = 123456;
        const S64 back = (S64)frameAt(fractionAt(frame, frames), frames);
        ensure("bar and frame round-trip within a frame", std::abs(back - (S64)frame) <= 1);
    }

    // Waveform columns: each the min / max of its own stretch of frames, over every channel.
    template<> template<>
    void soundscrub_object::test<3>()
    {
        using SLSoundScrub::waveform;
        const S16 mono[8] = { 0, 16384, -16384, 8192, 32767, -32768, 0, 0 };
        ensure("no PCM", waveform(nullptr, 8, 1, 4).empty());
        ensure("no frames", waveform(mono, 0, 1, 4).empty());
        ensure("no channels", waveform(mono, 8, 0, 4).empty());
        ensure("no columns", waveform(mono, 8, 1, 0).empty());

        const std::vector<SLSoundScrub::Peak> m = waveform(mono, 8, 1, 4);
        ensure_equals("one peak per column", m.size(), (size_t)4);
        ensurePeak("mono 0", m[0], 0.f, 0.5f);
        ensurePeak("mono 1", m[1], -0.5f, 0.25f);
        ensurePeak("mono 2 (full scale)", m[2], -1.f, 32767.f / FULL);
        ensurePeak("mono 3 (silence)", m[3], 0.f, 0.f);

        // Interleaved L R: both channels count.
        const S16 stereo[8] = { 100, -200, 300, 0, -32768, 0, 0, 16384 };
        const std::vector<SLSoundScrub::Peak> s = waveform(stereo, 4, 2, 2);
        ensure_equals("stereo columns", s.size(), (size_t)2);
        ensurePeak("stereo 0", s[0], -200.f / FULL, 300.f / FULL);
        ensurePeak("stereo 1", s[1], -1.f, 0.5f);

        // Fewer frames than columns: every column still shows a frame.
        const S16 two[2] = { -16384, 16384 };
        const std::vector<SLSoundScrub::Peak> t = waveform(two, 2, 1, 4);
        ensure_equals("stretched columns", t.size(), (size_t)4);
        ensurePeak("stretched 0", t[0], -0.5f, -0.5f);
        ensurePeak("stretched 1", t[1], -0.5f, -0.5f);
        ensurePeak("stretched 2", t[2], 0.5f, 0.5f);
        ensurePeak("stretched 3", t[3], 0.5f, 0.5f);

        // Frames that do not divide evenly: each frame lands in exactly one column, in order
        // (frames 0-2, 3-5, 6-9 of a ramp).
        S16 ramp[10];
        for (S16 i = 0; i < 10; ++i)
        {
            ramp[i] = (S16)(i * 1000);
        }
        const std::vector<SLSoundScrub::Peak> r = waveform(ramp, 10, 1, 3);
        ensure_equals("uneven columns", r.size(), (size_t)3);
        ensurePeak("uneven 0", r[0], 0.f, 2000.f / FULL);
        ensurePeak("uneven 1", r[1], 3000.f / FULL, 5000.f / FULL);
        ensurePeak("uneven 2", r[2], 6000.f / FULL, 9000.f / FULL);
    }
}
