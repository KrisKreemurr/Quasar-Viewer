/**
 * @file lluploadprogressview.cpp
 * @brief Background uploads (#1443): the compact progress bars in the top-right corner.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lluploadprogressview.h"

#include "llfontgl.h"
#include "llinventorypanel.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "lluploadqueue.h"
#include "llviewerwindow.h"
#include "llwindow.h"

namespace
{
    constexpr S32 BAR_WIDTH = 290;
    constexpr S32 BAR_HEIGHT = 40;
    constexpr S32 BAR_GAP = 4;
    constexpr S32 MARGIN = 10;
    constexpr S32 PAD = 6;
    constexpr S32 FILL_HEIGHT = 4;
    constexpr S32 CANCEL_SIZE = 14;
    constexpr size_t MAX_BARS = 6;

    const LLColor4 C_PANEL(0.07f, 0.08f, 0.10f, 0.84f);
    const LLColor4 C_EDGE(1.f, 1.f, 1.f, 0.10f);
    const LLColor4 C_TEXT(0.94f, 0.95f, 0.97f, 1.f);
    const LLColor4 C_DIM(0.70f, 0.73f, 0.78f, 1.f);
    const LLColor4 C_TRACK(1.f, 1.f, 1.f, 0.10f);
    const LLColor4 C_RUNNING(0.36f, 0.62f, 1.00f, 1.f);
    const LLColor4 C_DONE(0.36f, 0.80f, 0.46f, 1.f);
    const LLColor4 C_FAILED(0.92f, 0.36f, 0.32f, 1.f);
    const LLColor4 C_CANCELLED(0.55f, 0.57f, 0.62f, 1.f);
    const LLColor4 C_WARN(1.00f, 0.80f, 0.35f, 1.f);

    std::string tr(const std::string& key, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
    {
        return LLTrans::getString(key, args);
    }

    // The bar's second line.
    std::string status_line(const LLUploadTracker& tracker, const LLUploadTracker::Batch& b, LLColor4& color)
    {
        LLStringUtil::format_map_t args;
        color = C_DIM;
        if (b.mFinishedAt > 0.0)
        {
            if (b.mFailed)
            {
                color = C_FAILED;
                if (b.mSingle && !b.mFailures.empty())
                {
                    args["[REASON]"] = b.mFailures[0].mReason;
                    return tr("UploadBarFailedReason", args);
                }
                args["[DONE]"] = llformat("%d", b.mDone);
                args["[FAILED]"] = llformat("%d", b.mFailed);
                return tr("UploadBarDoneFailed", args);
            }
            if (b.mDone)
            {
                color = C_DONE;
                return tr("UploadBarDone");
            }
            return tr("UploadBarCancelled");
        }
        if (tracker.cancelArmed(b))
        {
            color = C_WARN;
            args["[COUNT]"] = llformat("%d", llmax(b.mTotal - b.finished() - b.mInFlight, 0));
            return tr("UploadBarCancelArmed", args);
        }
        if (b.mCancelRequested)
        {
            color = C_WARN;
            return tr("UploadBarCancelling");
        }
        args["[NAME]"] = b.mCurrent;
        const bool named = !b.mSingle && !b.mCurrent.empty();
        switch (b.mPhase)
        {
        case LLUploadTracker::PHASE_PREPARING:
            return tr(named ? "UploadBarPreparingItem" : "UploadBarPreparing", args);
        case LLUploadTracker::PHASE_UPLOADING:
            return tr(named ? "UploadBarUploadingItem" : "UploadBarUploading", args);
        default:
            return tr("UploadBarQueued");
        }
    }

    F32 fraction(const LLUploadTracker::Batch& b)
    {
        if (b.mFinishedAt > 0.0)
        {
            return 1.f;
        }
        if (b.mSingle)
        {
            switch (b.mPhase)
            {
            case LLUploadTracker::PHASE_PREPARING: return 0.3f;
            case LLUploadTracker::PHASE_UPLOADING: return 0.7f;
            default:                               return 0.05f;
            }
        }
        const F32 total = (F32)llmax(b.mTotal, 1);
        return llclamp(((F32)b.finished() + 0.5f * (F32)b.mInFlight) / total, 0.02f, 1.f);
    }
}

// static
LLUploadProgressView* LLUploadProgressView::create()
{
    LLView::Params p;
    p.name("upload_progress");
    p.mouse_opaque(false);
    p.follows.flags(FOLLOWS_ALL);
    return LLUICtrlFactory::create<LLUploadProgressView>(p);
}

LLUploadProgressView::LLUploadProgressView(const LLView::Params& p)
:   LLView(p)
{
}

void LLUploadProgressView::draw()
{
    mHits.clear();
    if (!LLUploadTracker::instanceExists())
    {
        return;
    }
    LLUploadTracker& tracker = LLUploadTracker::instance();
    tracker.expire();
    const std::deque<LLUploadTracker::Batch>& batches = tracker.batches();
    if (batches.empty() || !gViewerWindow)
    {
        return;
    }

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    if (!font || !bold)
    {
        return;
    }

    // The world view's top-right corner (this view covers the root view: the same coordinates).
    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    const S32 width = llmin(BAR_WIDTH, world.getWidth() - 2 * MARGIN);
    if (width < 120)
    {
        return;
    }
    const S32 right = world.mRight - MARGIN;
    const S32 left = right - width;
    S32 top = world.mTop - MARGIN;

    size_t shown = 0;
    for (const LLUploadTracker::Batch& b : batches)
    {
        if (shown == MAX_BARS)
        {
            break;
        }
        ++shown;

        Hit hit;
        hit.mBatch = b.mId;
        hit.mBar.setLeftTopAndSize(left, top, width, BAR_HEIGHT);
        top -= BAR_HEIGHT + BAR_GAP;

        const LLRect& r = hit.mBar;
        const bool hover = r.pointInRect(mHoverX, mHoverY);
        gl_rect_2d(r, C_PANEL);
        gl_rect_2d(r, hover ? C_DIM : C_EDGE, false);

        // x: what is not yet sent can be cancelled
        const bool running = b.mFinishedAt <= 0.0;
        const bool cancellable = running && !b.mCancelRequested && b.mTotal - b.finished() - b.mInFlight > 0;
        S32 text_right = r.mRight - PAD;
        if (cancellable)
        {
            hit.mCancel.setLeftTopAndSize(r.mRight - PAD - CANCEL_SIZE, r.mTop - PAD + 1, CANCEL_SIZE, CANCEL_SIZE);
            const bool over = hit.mCancel.pointInRect(mHoverX, mHoverY);
            if (over || tracker.cancelArmed(b))
            {
                gl_rect_2d(hit.mCancel, over ? C_TRACK : LLColor4(C_WARN.mV[VRED], C_WARN.mV[VGREEN], C_WARN.mV[VBLUE], 0.25f));
            }
            bold->renderUTF8("x", 0, (F32)hit.mCancel.getCenterX(), (F32)hit.mCancel.mBottom + 1, over ? C_TEXT : C_DIM,
                             LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            text_right = hit.mCancel.mLeft - 4;
        }

        // first line: the name, then "n of m" and the L$
        std::string right_text;
        if (!b.mSingle)
        {
            LLStringUtil::format_map_t args;
            args["[DONE]"] = llformat("%d", b.finished());
            args["[TOTAL]"] = llformat("%d", b.mTotal);
            right_text = tr("UploadBarCount", args);
        }
        const S32 money = running ? b.mQuoted : b.mCharged;
        if (money > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COST]"] = llformat("%d", money);
            if (!right_text.empty())
            {
                right_text += "  ";
            }
            right_text += tr("UploadBarCost", args);
        }
        const S32 line1_y = r.mTop - PAD - bold->getLineHeight() + 2;
        S32 name_right = text_right;
        if (!right_text.empty())
        {
            font->renderUTF8(right_text, 0, (F32)text_right, (F32)line1_y, C_DIM,
                             LLFontGL::RIGHT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            name_right = text_right - font->getWidth(right_text) - 8;
        }
        bold->renderUTF8(b.mName, 0, (F32)(r.mLeft + PAD), (F32)line1_y, C_TEXT,
                         LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                         S32_MAX, llmax(name_right - r.mLeft - PAD, 10), nullptr, true);

        // second line: what is happening
        LLColor4 status_color;
        const std::string status = status_line(tracker, b, status_color);
        const S32 line2_y = line1_y - font->getLineHeight();
        font->renderUTF8(status, 0, (F32)(r.mLeft + PAD), (F32)line2_y, status_color,
                         LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                         S32_MAX, r.getWidth() - 2 * PAD, nullptr, true);
        hit.mTip = b.mName + "\n" + status;

        // the fill
        const LLRect track(r.mLeft + PAD, r.mBottom + PAD + FILL_HEIGHT, r.mRight - PAD, r.mBottom + PAD);
        gl_rect_2d(track, C_TRACK);
        const LLColor4& fill_color = !running ? (b.mFailed ? C_FAILED : (b.mDone ? C_DONE : C_CANCELLED))
                                              : (b.mCancelRequested ? C_WARN : C_RUNNING);
        const S32 fill_w = llmax(1, (S32)llround(fraction(b) * (F32)track.getWidth()));
        gl_rect_2d(track.mLeft, track.mTop, track.mLeft + fill_w, track.mBottom, fill_color);

        mHits.push_back(hit);
    }

    if (batches.size() > shown)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)(batches.size() - shown));
        font->renderUTF8(tr("UploadBarMore", args), 0, (F32)right, (F32)(top + BAR_HEIGHT - font->getLineHeight()), C_DIM,
                         LLFontGL::RIGHT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
    }

    LLView::draw();
}

const LLUploadProgressView::Hit* LLUploadProgressView::hitAt(S32 x, S32 y) const
{
    for (const Hit& hit : mHits)
    {
        if (hit.mBar.pointInRect(x, y))
        {
            return &hit;
        }
    }
    return nullptr;
}

bool LLUploadProgressView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        return false;   // click-through to the world
    }
    // Handled here without taking keyboard or mouse focus: typing and movement keys go on
    // to wherever they went before.
    if (hit->mCancel.notEmpty() && hit->mCancel.pointInRect(x, y))
    {
        LLUploadTracker::instance().onCancelClicked(hit->mBatch);
        return true;
    }
    for (const LLUploadTracker::Batch& b : LLUploadTracker::instance().batches())
    {
        if (b.mId == hit->mBatch && b.mLastItem.notNull())
        {
            LLInventoryPanel::openInventoryPanelAndSetSelection(true, b.mLastItem, false, false, true);
            break;
        }
    }
    return true;
}

bool LLUploadProgressView::handleHover(S32 x, S32 y, MASK mask)
{
    mHoverX = x;
    mHoverY = y;
    if (!hitAt(x, y))
    {
        mHoverX = mHoverY = -1;
        return false;
    }
    gViewerWindow->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool LLUploadProgressView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        return false;
    }
    LLRect screen;
    localRectToScreen(hit->mBar, &screen);
    LLToolTipMgr::instance().show(LLToolTip::Params().message(hit->mTip).sticky_rect(screen));
    return true;
}
