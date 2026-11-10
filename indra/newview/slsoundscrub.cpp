/**
 * @file slsoundscrub.cpp
 * @brief The sound preview player's scrub bar arithmetic (see slsoundscrub.h)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slsoundscrub.h)
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "slsoundscrub.h"

#include "llformat.h"

#include <algorithm>

std::string SLSoundScrub::formatTime(U32 frames, U32 rate)
{
    const U64 centis = rate ? (U64)frames * 100 / rate : 0;
    return llformat("%u:%02u.%02u", (U32)(centis / 6000), (U32)(centis / 100 % 60), (U32)(centis % 100));
}

F32 SLSoundScrub::fractionAt(U32 frame, U32 frames)
{
    return frames ? llclamp((F32)((F64)frame / (F64)frames), 0.f, 1.f) : 0.f;
}

U32 SLSoundScrub::frameAt(F32 fraction, U32 frames)
{
    if (!frames)
    {
        return 0;
    }
    const U32 frame = (U32)((F64)llclamp(fraction, 0.f, 1.f) * (F64)frames);
    return llmin(frame, frames - 1);
}

std::vector<SLSoundScrub::Peak> SLSoundScrub::waveform(const S16* pcm, U32 frames, U32 channels, U32 columns)
{
    std::vector<Peak> peaks;
    if (!pcm || !frames || !channels || !columns)
    {
        return peaks;
    }
    peaks.resize(columns);
    for (U32 c = 0; c < columns; ++c)
    {
        const U32 first = (U32)((U64)c * frames / columns);
        const U32 end = std::max(first + 1, (U32)((U64)(c + 1) * frames / columns));
        const S16* begin_sample = pcm + (size_t)first * channels;
        const S16* end_sample = pcm + (size_t)end * channels;
        const auto range = std::minmax_element(begin_sample, end_sample);
        peaks[c].mLow = (F32)*range.first / 32768.f;
        peaks[c].mHigh = (F32)*range.second / 32768.f;
    }
    return peaks;
}
