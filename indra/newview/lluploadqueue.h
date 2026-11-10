/**
 * @file lluploadqueue.h
 * @brief Background uploads (#1443): the batches behind the top-right progress bars, and the
 *        bounded worker pipeline that prepares uploads (decode / encode) off the main thread.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLUPLOADQUEUE_H
#define LL_LLUPLOADQUEUE_H

#include "llsingleton.h"
#include "lluuid.h"
#include "threadpool_fwd.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LLResourceUploadInfo;

// Every upload belongs to a batch: a single upload is a batch of one. The progress view
// (lluploadprogressview.h) draws one bar per batch. Main thread only.
//
// A batch counts items: files and materials the user picked, not the requests behind them
// (a material's textures are steps of the material). An item ends done, failed or cancelled;
// the batch ends when all of its items have, and then reports once, in a toast.
class LLUploadTracker : public LLSingleton<LLUploadTracker>
{
    LLSINGLETON_EMPTY_CTOR(LLUploadTracker);

public:
    typedef U32 batch_t;   // 0: not tracked

    enum EPhase
    {
        PHASE_QUEUED,       // waiting for a pipeline slot
        PHASE_PREPARING,    // decoding / encoding on the upload pool
        PHASE_UPLOADING     // a request is with the server
    };

    struct Failure
    {
        std::string mName;
        std::string mReason;
        // Failed after the file went to the server: it may or may not have been created and
        // charged. Never offered for retry.
        bool        mChargeUnknown = false;
        // Empty: no safe retry (it may have been charged, or the source is gone).
        std::function<void()> mRetry;
    };

    struct Batch
    {
        batch_t     mId = 0;
        std::string mName;
        bool        mSingle = true;     // one upload: no "n of m"
        S32         mTotal = 0;
        S32         mDone = 0;
        S32         mFailed = 0;
        S32         mCancelled = 0;
        S32         mInFlight = 0;      // sent to the server, not answered yet
        S32         mQuoted = 0;        // L$ the user confirmed
        S32         mCharged = 0;       // L$ the server reported
        std::string mCurrent;           // the item being worked on
        EPhase      mPhase = PHASE_QUEUED;
        bool        mQuiet = false;     // its owner reports the result (no toast from here)
        LLUUID      mLastItem;          // the newest inventory item it made
        std::vector<Failure> mFailures; // the first few (MAX_FAILURES); the count is mFailed
        bool        mCancelRequested = false;
        F64         mCancelArmedAt = 0.0;   // x pressed once while an upload is in flight
        F64         mFinishedAt = 0.0;      // 0: running

        S32  finished() const { return mDone + mFailed + mCancelled; }
        bool complete() const { return finished() >= mTotal; }
    };

    static constexpr size_t MAX_FAILURES = 32;

    // quiet: the caller reports the outcome itself (the bar still shows); no toast from here.
    batch_t begin(const std::string& name, S32 total, S32 quoted_cost, bool single, bool quiet = false);
    void    addItems(batch_t id, S32 count, S32 quoted_cost);
    void    setPhase(batch_t id, const std::string& item, EPhase phase);
    void    sent(batch_t id, const std::string& item);      // a request went to the server
    void    answered(batch_t id);                           // ... and came back (any outcome)
    void    succeeded(batch_t id, const LLUUID& item_id);
    void    charged(batch_t id, S32 amount);
    void    failed(batch_t id, const Failure& failure);
    void    cancelled(batch_t id, S32 count = 1);
    bool    isCancelled(batch_t id) const;

    // The bar's x: cancels what is not yet sent. With an upload in flight the first press only
    // arms it (the bar says that upload finishes and is charged), a second press cancels.
    void    onCancelClicked(batch_t id);
    bool    cancelArmed(const Batch& b) const;

    // Items not finished, in every batch (the quit prompt).
    S32     pendingCount() const;

    const std::deque<Batch>& batches() const { return mBatches; }
    // Finished batches leave a few seconds after they end (the view calls it as it draws).
    void    expire();

private:
    Batch*       find(batch_t id);
    const Batch* find(batch_t id) const;
    void         checkFinished(Batch& b);
    void         report(const Batch& b);

    std::deque<Batch> mBatches;
    batch_t           mNextId = 1;
};

// The worker side of uploads. Jobs wait here (light: names and paths, no pixels) until a
// pipeline slot is free; then the worker step runs on the "UploadPrep" thread pool and the
// main-thread step sends the result. A job keeps its slot until its uploads have finished,
// so at most PIPELINE_SLOTS jobs are prepared or uploading at a time (the next ones are
// encoded while the current one uploads) and at most WORKER_THREADS encode at once.
class LLUploadQueue : public LLSingleton<LLUploadQueue>
{
    LLSINGLETON(LLUploadQueue);
    ~LLUploadQueue();

public:
    static constexpr S32 WORKER_THREADS = 2;
    static constexpr S32 PIPELINE_SLOTS = 4;

    struct Job
    {
        std::string                 mName;
        LLUploadTracker::batch_t    mBatch = 0;
        std::function<void()>       mBefore;    // main thread, just before the worker step
        std::function<void()>       mWork;      // worker thread: decode / encode / parse only
        // Main thread, after the worker step: sends the uploads and calls release() once they
        // have all finished (any outcome). release() may be called more than once.
        std::function<void(std::function<void()> release)> mAfter;
        // Main thread: the batch was cancelled before mAfter ran (mWork may have run).
        std::function<void(bool prepared)> mCancel;
    };

    void submit(Job job);
    // The asset uploads (LLViewerAssetUpload::EnqueueInventoryUpload): through the pipeline when
    // the info has a worker step, else straight to the upload coroutine.
    void submit(const std::string& url, const std::shared_ptr<LLResourceUploadInfo>& info);

    // Worker work with a main-thread answer that holds no pipeline slot (reading files for the
    // bulk list, decoding sounds for their preview). The caller bounds how many it has posted.
    // false: no worker (quitting); done is not called.
    bool runOnWorker(std::function<void()> work, std::function<void()> done);

    S32 waitingCount() const { return (S32)mWaiting.size(); }

    // Drops the waiting jobs of cancelled batches now (they hold no slot and nothing was read).
    void purgeCancelled();

private:
    void pump();
    void release();
    LL::ThreadPool* pool();

    std::deque<Job> mWaiting;
    S32 mPreparing = 0;     // in the worker step
    S32 mInPipeline = 0;    // prepared or preparing, not finished uploading
    std::unique_ptr<LL::ThreadPool> mPool;
};

#endif // LL_LLUPLOADQUEUE_H
