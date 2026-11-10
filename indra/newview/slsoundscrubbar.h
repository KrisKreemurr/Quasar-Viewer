/**
 * @file slsoundscrubbar.h
 * @brief The sound preview player's scrub bar (#1311): a clip's waveform with the playhead
 *        over it. Registered as @c sound_scrub_bar.
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

#ifndef SL_SOUNDSCRUBBAR_H
#define SL_SOUNDSCRUBBAR_H

#include "lluictrl.h"
#include "lluicolor.h"
#include "slsoundscrub.h"

#include <memory>
#include <vector>

// The waveform is one min / max column per pixel (SLSoundScrub::waveform), worked out when the
// clip or the width changes (never while drawing); the part before the playhead is drawn in
// the played colour. A press puts the playhead under the pointer and a drag moves it; neither
// seeks. The commit callback fires on release, with the playhead (0..1) as the value: a click
// seeks at once, a drag seeks where it ends. A drag cut short (capture lost) puts the playhead
// back.
//
// Retained UI: a recorded floater replays without calling draw(), so every change to what the
// bar draws -- a new clip, its peaks, the playhead moving to another pixel -- reports
// onRetainedDirty(). A playhead that stays on the same pixel reports nothing.
class SLSoundScrubBar final : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<LLUIColor> background_color;
        Optional<LLUIColor> border_color;
        Optional<LLUIColor> wave_color;         // still to play
        Optional<LLUIColor> played_color;       // before the playhead
        Optional<LLUIColor> playhead_color;

        Params();
    };

    SLSoundScrubBar(const Params& p);

    // The clip to draw (interleaved 16-bit PCM, `channels` per frame); null draws an empty bar.
    void setPCM(std::shared_ptr<const std::vector<S16>> pcm, U32 channels);
    // The playhead, 0..1; ignored while the user drags it.
    void setPlayhead(F32 fraction);
    F32  getPlayhead() const { return mPlayhead; }
    bool isDragging() const { return mDragging; }

    LLSD getValue() const override { return LLSD((F64)mPlayhead); }
    void setValue(const LLSD& value) override { setPlayhead((F32)value.asReal()); }

    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    LLRect plotRect() const;            // inside the border
    F32    fractionAtX(S32 x) const;
    S32    playheadX(F32 fraction) const;   // the pixel column the playhead is drawn at
    void   buildPeaks();                    // for the clip at the current width
    void   movePlayhead(F32 fraction);      // (reports when it lands on another pixel)

    LLUIColor mBackgroundColor;
    LLUIColor mBorderColor;
    LLUIColor mWaveColor;
    LLUIColor mPlayedColor;
    LLUIColor mPlayheadColor;

    std::shared_ptr<const std::vector<S16>> mPCM;
    U32  mChannels = 1;
    std::vector<SLSoundScrub::Peak> mPeaks;     // one per plot column (empty: not worked out yet)
    F32  mPlayhead = 0.f;
    bool mDragging = false;
    F32  mDragStart = 0.f;                      // the playhead the drag started from
};

#endif // SL_SOUNDSCRUBBAR_H
