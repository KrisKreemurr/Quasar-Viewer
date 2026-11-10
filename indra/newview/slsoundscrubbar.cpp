/**
 * @file slsoundscrubbar.cpp
 * @brief The sound preview player's scrub bar (see slsoundscrubbar.h)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slsoundscrubbar.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slsoundscrubbar.h"
#include "llsoundpreviewlimits.h"
#include <new>

#include "llfocusmgr.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "llwindow.h"

static LLDefaultChildRegistry::Register<SLSoundScrubBar> r("sound_scrub_bar");

SLSoundScrubBar::Params::Params()
:   background_color("background_color", LLUIColorTable::instance().getColor("TextBgReadOnlyColor")),
    border_color("border_color", LLUIColorTable::instance().getColor("DefaultShadowLight")),
    wave_color("wave_color", LLUIColorTable::instance().getColor("LabelTextColor")),
    played_color("played_color", LLUIColorTable::instance().getColor("EmphasisColor")),
    playhead_color("playhead_color", LLUIColorTable::instance().getColor("White"))
{
    changeDefault(mouse_opaque, true);
    changeDefault(tab_stop, false);
}

SLSoundScrubBar::SLSoundScrubBar(const Params& p)
:   LLUICtrl(p),
    mBackgroundColor(p.background_color),
    mBorderColor(p.border_color),
    mWaveColor(p.wave_color),
    mPlayedColor(p.played_color),
    mPlayheadColor(p.playhead_color)
{
}

void SLSoundScrubBar::setPCM(std::shared_ptr<const std::vector<S16>> pcm, U32 channels)
{
    mPCM = std::move(pcm);
    mChannels = llmax(1u, channels);
    buildPeaks();
    onRetainedDirty();
}

void SLSoundScrubBar::buildPeaks()
{
    const S32 width = plotRect().getWidth();
    if (mPCM && width > 0)
    {
        try
        {
            mPeaks = SLSoundScrub::waveform(mPCM->data(), (U32)(mPCM->size() / mChannels), mChannels,
                (std::min)((U32)width, LLSoundPreviewLimits::WAVE_COLUMNS));
        }
        catch (const std::bad_alloc&)
        {
            mPeaks.clear(); // playback remains available without a waveform
        }
    }
    else
    {
        mPeaks.clear();
    }
}

void SLSoundScrubBar::reshape(S32 width, S32 height, bool called_from_parent)
{
    const S32 old_width = getRect().getWidth();
    LLUICtrl::reshape(width, height, called_from_parent);
    if (mPCM && getRect().getWidth() != old_width)
    {
        buildPeaks();
        onRetainedDirty();
    }
}

void SLSoundScrubBar::setPlayhead(F32 fraction)
{
    if (!mDragging)
    {
        movePlayhead(fraction);
    }
}

void SLSoundScrubBar::movePlayhead(F32 fraction)
{
    fraction = llclamp(fraction, 0.f, 1.f);
    if (mPCM && playheadX(fraction) != playheadX(mPlayhead))
    {
        onRetainedDirty();
    }
    mPlayhead = fraction;
}

S32 SLSoundScrubBar::playheadX(F32 fraction) const
{
    const LLRect plot = plotRect();
    return llmin(plot.mLeft + ll_round(fraction * (F32)plot.getWidth()), plot.mRight - 1);
}

LLRect SLSoundScrubBar::plotRect() const
{
    LLRect rect = getLocalRect();
    rect.stretch(-1);
    return rect;
}

F32 SLSoundScrubBar::fractionAtX(S32 x) const
{
    const LLRect plot = plotRect();
    return plot.getWidth() > 0 ? llclamp((F32)(x - plot.mLeft) / (F32)plot.getWidth(), 0.f, 1.f) : 0.f;
}

bool SLSoundScrubBar::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled() || !mPCM)
    {
        return LLUICtrl::handleMouseDown(x, y, mask);
    }
    mDragStart = mPlayhead;
    mDragging = true;
    movePlayhead(fractionAtX(x));
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool SLSoundScrubBar::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mDragging)
    {
        return LLUICtrl::handleHover(x, y, mask);
    }
    movePlayhead(fractionAtX(x));
    getWindow()->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool SLSoundScrubBar::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!mDragging)
    {
        return LLUICtrl::handleMouseUp(x, y, mask);
    }
    movePlayhead(fractionAtX(x));
    mDragging = false;      // (first: the capture released below is not a cancelled drag)
    gFocusMgr.setMouseCapture(nullptr);
    onRetainedDirty();      // (drawn live while captured: the replay after it must re-record)
    onCommit();
    return true;
}

void SLSoundScrubBar::onMouseCaptureLost()
{
    if (mDragging)
    {
        mDragging = false;
        movePlayhead(mDragStart);
        onRetainedDirty();
    }
    LLUICtrl::onMouseCaptureLost();
}

void SLSoundScrubBar::draw()
{
    const LLRect bounds = getLocalRect();
    const LLRect plot = plotRect();
    const F32 alpha = getDrawContext().mAlpha * (getEnabled() ? 1.f : 0.5f);

    gl_rect_2d(bounds, mBackgroundColor.get() % alpha, true);

    if (!mPeaks.empty())
    {
        const S32 playhead_x = playheadX(mPlayhead);

        // One column per pixel, low to high, at least a pixel tall so silence still reads as a
        // line. TRIANGLES: LLRender flushes them in whole triangles however many columns there are.
        const F32 mid = 0.5f * (F32)(plot.mBottom + plot.mTop);
        const F32 half = 0.5f * (F32)plot.getHeight();
        const LLColor4 played = mPlayedColor.get() % alpha;
        const LLColor4 wave = mWaveColor.get() % alpha;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < (S32)mPeaks.size(); ++i)
        {
            const F32 x0 = (F32)plot.mLeft + (F32)i * (F32)plot.getWidth() / (F32)mPeaks.size();
            const F32 x1 = (F32)plot.mLeft + (F32)(i + 1) * (F32)plot.getWidth() / (F32)mPeaks.size();
            F32 y0 = mid + mPeaks[i].mLow * half;
            F32 y1 = mid + mPeaks[i].mHigh * half;
            if (y1 - y0 < 1.f)
            {
                const F32 centre = 0.5f * (y0 + y1);
                y0 = centre - 0.5f;
                y1 = centre + 0.5f;
            }
            gGL.color4fv((x0 < playhead_x ? played : wave).mV);
            gGL.vertex2f(x0, y0);
            gGL.vertex2f(x1, y0);
            gGL.vertex2f(x1, y1);
            gGL.vertex2f(x0, y0);
            gGL.vertex2f(x1, y1);
            gGL.vertex2f(x0, y1);
        }
        gGL.end();

        gl_rect_2d(playhead_x, plot.mTop, playhead_x + 1, plot.mBottom, mPlayheadColor.get() % alpha, true);
    }

    gl_rect_2d(bounds, mBorderColor.get() % alpha, false);

    LLUICtrl::draw();
}
