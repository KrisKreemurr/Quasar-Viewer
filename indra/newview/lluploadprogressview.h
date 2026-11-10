/**
 * @file lluploadprogressview.h
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

#ifndef LL_LLUPLOADPROGRESSVIEW_H
#define LL_LLUPLOADPROGRESSVIEW_H

#include "llview.h"

#include <vector>

// One bar per upload batch (LLUploadTracker), stacked down from the world view's top-right
// corner. Drawn over the world, a child of the root view: it never takes keyboard focus, and
// the mouse goes through it except on a bar (x cancels what is not yet sent; a click on a
// finished bar shows its item in inventory, without moving focus).
class LLUploadProgressView : public LLView
{
public:
    static LLUploadProgressView* create();   // LLViewerWindow::initBase

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

protected:
    LLUploadProgressView(const LLView::Params& p);
    friend class LLUICtrlFactory;

private:
    struct Hit
    {
        LLRect mBar;
        LLRect mCancel;     // empty: nothing to cancel
        U32    mBatch = 0;
        std::string mTip;
    };
    const Hit* hitAt(S32 x, S32 y) const;

    std::vector<Hit> mHits;     // the bars drawn last frame
    S32 mHoverX = -1, mHoverY = -1;
};

#endif // LL_LLUPLOADPROGRESSVIEW_H
