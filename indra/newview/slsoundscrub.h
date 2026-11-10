/**
 * @file slsoundscrub.h
 * @brief The sound preview player (#1311): the arithmetic behind its scrub bar -- the times it
 *        shows, the playhead's place on the bar and the waveform's per-column peaks. No viewer
 *        state, so it is unit-tested (tests/slsoundscrub_test.cpp).
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

#ifndef SL_SOUNDSCRUB_H
#define SL_SOUNDSCRUB_H

#include "stdtypes.h"

#include <string>
#include <vector>

// Positions and lengths are frames: one sample per channel, which is what OpenAL's
// AL_SAMPLE_OFFSET counts. A clip's length follows from its PCM as it plays (the decoded
// .dsf's data bytes / 2 / its header's channel count), so a stereo asset under the decoder's
// mono header is as long here as it is in-world.
namespace SLSoundScrub
{
    // m:ss.cc (hundredths of a second, rounded down so a playhead never shows a time it has not
    // reached; minutes are neither padded nor capped). Rate 0 shows 0:00.00.
    std::string formatTime(U32 frames, U32 rate);

    // The playhead's place on the bar (0..1) for `frame` of a clip `frames` long.
    F32 fractionAt(U32 frame, U32 frames);
    // The frame a place on the bar seeks to (the last frame at most, so a seek to the very end
    // still plays it). 0 for an empty clip.
    U32 frameAt(F32 fraction, U32 frames);

    // One waveform column: the lowest and highest sample in its stretch of the clip, -1..1, over
    // every channel.
    struct Peak
    {
        F32 mLow = 0.f;
        F32 mHigh = 0.f;
    };
    // `columns` columns over the whole clip (interleaved 16-bit PCM, `channels` per frame), each
    // over an equal stretch of frames; a clip shorter than the bar is stretched (neighbouring
    // columns share a frame). Empty when there is nothing to draw.
    std::vector<Peak> waveform(const S16* pcm, U32 frames, U32 channels, U32 columns);
}

#endif // SL_SOUNDSCRUB_H
