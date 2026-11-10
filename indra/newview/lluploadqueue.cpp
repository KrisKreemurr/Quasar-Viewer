/**
 * @file lluploadqueue.cpp
 * @brief Background uploads (#1443): batches, toasts and the bounded worker pipeline.
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

#include "lluploadqueue.h"

#include "llapp.h"
#include "llframetimer.h"
#include "llinventorypanel.h"
#include "llnotificationsutil.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llviewerassetupload.h"
#include "threadpool.h"
#include "workqueue.h"

#include <algorithm>

namespace
{
    constexpr F64 SHOW_FINISHED_SECONDS = 6.0;   // a finished bar stays this long
    constexpr F64 CANCEL_ARM_SECONDS = 5.0;      // the second x press must follow within this
    constexpr size_t MAX_BATCHES = 256;          // finished ones go first when it is reached
    constexpr size_t MAX_LISTED_FAILURES = 5;    // in the batch's toast
    const char* const POOL_NAME = "UploadPrep";

    std::string tr(const std::string& key, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
    {
        return LLTrans::getString(key, args);
    }

    void show_in_inventory(const LLUUID& item_id)
    {
        if (item_id.notNull())
        {
            // The user pressed the toast's button: open inventory there, but leave keyboard focus.
            LLInventoryPanel::openInventoryPanelAndSetSelection(true, item_id, false, false, true);
        }
    }
}

//=============================================================================
// LLUploadTracker

LLUploadTracker::batch_t LLUploadTracker::begin(const std::string& name, S32 total, S32 quoted_cost, bool single, bool quiet)
{
    if (mBatches.size() >= MAX_BATCHES)
    {
        for (auto it = mBatches.begin(); it != mBatches.end(); ++it)
        {
            if (it->mFinishedAt > 0.0)
            {
                mBatches.erase(it);
                break;
            }
        }
    }

    Batch b;
    b.mId = mNextId++;
    if (mNextId == 0)
    {
        mNextId = 1;
    }
    b.mName = name;
    b.mSingle = single;
    b.mTotal = llmax(total, 1);
    b.mQuoted = llmax(quoted_cost, 0);
    b.mQuiet = quiet;
    mBatches.push_back(b);
    return b.mId;
}

void LLUploadTracker::addItems(batch_t id, S32 count, S32 quoted_cost)
{
    if (Batch* b = find(id))
    {
        b->mTotal += llmax(count, 0);
        b->mQuoted += llmax(quoted_cost, 0);
    }
}

void LLUploadTracker::setPhase(batch_t id, const std::string& item, EPhase phase)
{
    if (Batch* b = find(id))
    {
        if (!item.empty())
        {
            b->mCurrent = item;
        }
        if (b->mInFlight == 0 || phase == PHASE_UPLOADING)
        {
            b->mPhase = phase;
        }
    }
}

void LLUploadTracker::sent(batch_t id, const std::string& item)
{
    if (Batch* b = find(id))
    {
        ++b->mInFlight;
        if (!item.empty())
        {
            b->mCurrent = item;
        }
        b->mPhase = PHASE_UPLOADING;
    }
}

void LLUploadTracker::answered(batch_t id)
{
    if (Batch* b = find(id))
    {
        b->mInFlight = llmax(b->mInFlight - 1, 0);
    }
}

void LLUploadTracker::succeeded(batch_t id, const LLUUID& item_id)
{
    if (Batch* b = find(id))
    {
        ++b->mDone;
        if (item_id.notNull())
        {
            b->mLastItem = item_id;
        }
        checkFinished(*b);
    }
}

void LLUploadTracker::charged(batch_t id, S32 amount)
{
    if (Batch* b = find(id))
    {
        b->mCharged += llmax(amount, 0);
    }
}

void LLUploadTracker::failed(batch_t id, const Failure& failure)
{
    if (Batch* b = find(id))
    {
        ++b->mFailed;
        if (b->mFailures.size() < MAX_FAILURES)
        {
            // one line for the bar and the toast's list
            Failure f = failure;
            LLStringUtil::replaceChar(f.mReason, '\n', ' ');
            LLStringUtil::trim(f.mReason);
            b->mFailures.push_back(f);
        }
        LL_WARNS("UploadQueue") << "Upload failed: " << failure.mName << ": " << failure.mReason
                                << (failure.mChargeUnknown ? " (charge unknown)" : "") << LL_ENDL;
        checkFinished(*b);
    }
}

void LLUploadTracker::cancelled(batch_t id, S32 count)
{
    if (Batch* b = find(id))
    {
        b->mCancelled += llmax(count, 0);
        checkFinished(*b);
    }
}

bool LLUploadTracker::isCancelled(batch_t id) const
{
    const Batch* b = find(id);
    return b && b->mCancelRequested;
}

bool LLUploadTracker::cancelArmed(const Batch& b) const
{
    return b.mCancelArmedAt > 0.0 && LLFrameTimer::getTotalSeconds() - b.mCancelArmedAt < CANCEL_ARM_SECONDS;
}

void LLUploadTracker::onCancelClicked(batch_t id)
{
    Batch* b = find(id);
    if (!b || b->mFinishedAt > 0.0 || b->mCancelRequested)
    {
        return;
    }
    if (b->mInFlight > 0 && !cancelArmed(*b))
    {
        // Say first that the upload already sent finishes (and is charged).
        b->mCancelArmedAt = LLFrameTimer::getTotalSeconds();
        return;
    }
    b->mCancelRequested = true;
    b->mCancelArmedAt = 0.0;
    LL_INFOS("UploadQueue") << "Upload batch '" << b->mName << "' cancelled by the user: "
                            << (b->mTotal - b->finished()) << " not finished, " << b->mInFlight << " in flight" << LL_ENDL;
    // What is still waiting goes now; prepared ones stop before they are sent.
    LLUploadQueue::instance().purgeCancelled();
}

S32 LLUploadTracker::pendingCount() const
{
    S32 count = 0;
    for (const Batch& b : mBatches)
    {
        if (b.mFinishedAt <= 0.0)
        {
            count += llmax(b.mTotal - b.finished(), 0);
        }
    }
    return count;
}

void LLUploadTracker::expire()
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    while (!mBatches.empty())
    {
        auto it = std::find_if(mBatches.begin(), mBatches.end(), [now](const Batch& b)
            { return b.mFinishedAt > 0.0 && now - b.mFinishedAt > SHOW_FINISHED_SECONDS; });
        if (it == mBatches.end())
        {
            break;
        }
        mBatches.erase(it);
    }
}

LLUploadTracker::Batch* LLUploadTracker::find(batch_t id)
{
    if (!id)
    {
        return nullptr;
    }
    for (Batch& b : mBatches)
    {
        if (b.mId == id)
        {
            return &b;
        }
    }
    return nullptr;
}

const LLUploadTracker::Batch* LLUploadTracker::find(batch_t id) const
{
    return const_cast<LLUploadTracker*>(this)->find(id);
}

void LLUploadTracker::checkFinished(Batch& b)
{
    if (b.mFinishedAt > 0.0 || !b.complete())
    {
        return;
    }
    b.mFinishedAt = LLFrameTimer::getTotalSeconds();
    b.mInFlight = 0;
    LL_INFOS("UploadQueue") << "Upload batch '" << b.mName << "' finished: " << b.mDone << " done, " << b.mFailed
                            << " failed, " << b.mCancelled << " cancelled of " << b.mTotal << "; L$" << b.mCharged
                            << " charged (L$" << b.mQuoted << " quoted)" << LL_ENDL;
    report(b);
}

void LLUploadTracker::report(const Batch& b)
{
    // A non-modal toast: what was made (with a link to it), what failed and why.
    if (b.mQuiet || (!b.mDone && !b.mFailed))
    {
        return; // its owner reports it, or all were cancelled (the bar said so)
    }

    std::string message;
    LLStringUtil::format_map_t args;
    args["[NAME]"] = b.mName;
    if (b.mSingle)
    {
        if (b.mDone)
        {
            message = tr("UploadToastDone", args);
        }
        else if (!b.mFailures.empty())
        {
            args["[REASON]"] = b.mFailures[0].mReason;
            message = tr("UploadToastFailed", args);
        }
    }
    else
    {
        args["[DONE]"] = llformat("%d", b.mDone);
        args["[TOTAL]"] = llformat("%d", b.mTotal);
        message = tr("UploadToastBatch", args);
        for (size_t i = 0; i < b.mFailures.size() && i < MAX_LISTED_FAILURES; ++i)
        {
            LLStringUtil::format_map_t fargs;
            fargs["[NAME]"] = b.mFailures[i].mName;
            fargs["[REASON]"] = b.mFailures[i].mReason;
            message += "\n" + tr("UploadToastFailedLine", fargs);
        }
        if (b.mFailed > (S32)MAX_LISTED_FAILURES)
        {
            LLStringUtil::format_map_t fargs;
            fargs["[COUNT]"] = llformat("%d", b.mFailed - (S32)MAX_LISTED_FAILURES);
            message += "\n" + tr("UploadToastMoreFailed", fargs);
        }
        if (b.mCancelled)
        {
            LLStringUtil::format_map_t fargs;
            fargs["[COUNT]"] = llformat("%d", b.mCancelled);
            message += "\n" + tr("UploadToastCancelled", fargs);
        }
    }
    if (b.mCharged > 0)
    {
        LLStringUtil::format_map_t cargs;
        cargs["[AMOUNT]"] = llformat("%d", b.mCharged);
        message += "\n" + tr("UploadToastCharged", cargs);
    }

    bool charge_unknown = false;
    std::vector<std::function<void()>> retries;
    for (const Failure& f : b.mFailures)
    {
        charge_unknown |= f.mChargeUnknown;
        if (f.mRetry && !f.mChargeUnknown)
        {
            retries.push_back(f.mRetry);
        }
    }
    if (charge_unknown)
    {
        message += "\n" + tr("UploadToastChargeUnknown");
    }

    LLSD subs;
    subs["MESSAGE"] = message;
    const LLUUID item = b.mLastItem;
    const bool can_show = item.notNull();
    const bool can_retry = !retries.empty();
    const char* name = can_show ? (can_retry ? "SLUploadDoneRetry" : "SLUploadDone")
                                : (can_retry ? "SLUploadResultRetry" : "SLUploadResult");
    LLNotificationsUtil::add(name, subs, LLSD(),
        [item, retries, can_show](const LLSD& notification, const LLSD& response)
        {
            const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
            const S32 retry_option = can_show ? 1 : 0;
            if (can_show && option == 0)
            {
                show_in_inventory(item);
            }
            else if (option == retry_option && !retries.empty())
            {
                // An explicit request: each was refused before anything was charged.
                for (const std::function<void()>& retry : retries)
                {
                    retry();
                }
            }
        });
}

//=============================================================================
// LLUploadQueue

LLUploadQueue::LLUploadQueue()
{
}

LLUploadQueue::~LLUploadQueue()
{
    if (mPool)
    {
        mPool->close();
        mPool.reset();
    }
}

LL::ThreadPool* LLUploadQueue::pool()
{
    if (!mPool && !LLApp::isExiting())
    {
        // Created on the first upload; two threads, so a bulk upload never takes every core.
        mPool.reset(new LL::ThreadPool(POOL_NAME, WORKER_THREADS, 4096));
        mPool->start();
    }
    return mPool.get();
}

void LLUploadQueue::submit(Job job)
{
    mWaiting.push_back(std::move(job));
    pump();
}

void LLUploadQueue::submit(const std::string& url, const std::shared_ptr<LLResourceUploadInfo>& info)
{
    if (!info)
    {
        return;
    }
    if (!info->needsWorkerPrepare())
    {
        LLViewerAssetUpload::EnqueueUploadCoproc(url, info);
        return;
    }

    Job job;
    job.mName = info->getDisplayName();
    job.mBatch = info->getBatchId();
    job.mBefore = [info]() { info->beginWorkerPrepare(); };
    job.mWork = [info]() { info->prepareOnWorker(); };
    job.mAfter = [url, info](std::function<void()> release)
    {
        info->setPipelineRelease(release);
        LLViewerAssetUpload::EnqueueUploadCoproc(url, info);
    };
    job.mCancel = [info](bool prepared)
    {
        if (prepared)
        {
            info->discardPrepared();
        }
        if (info->countsAsBatchItem())
        {
            LLUploadTracker::instance().cancelled(info->getBatchId());
        }
        info->onCancelled();
    };
    submit(std::move(job));
}

bool LLUploadQueue::runOnWorker(std::function<void()> work, std::function<void()> done)
{
    LL::ThreadPool* p = pool();
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t worker = LL::WorkQueue::getInstance(POOL_NAME);
    if (!p || !main_queue || !worker)
    {
        return false;
    }
    return main_queue->postTo(worker,
        [work]()
        {
            try
            {
                work();
            }
            catch (const std::bad_alloc&)
            {
                LL_WARNS("UploadQueue") << "Out of memory in an upload worker task" << LL_ENDL;
            }
            catch (...)
            {
                LOG_UNHANDLED_EXCEPTION("upload worker task");
            }
        },
        [done]()
        {
            if (!LLApp::isExiting())
            {
                done();
            }
        });
}

void LLUploadQueue::purgeCancelled()
{
    LLUploadTracker& tracker = LLUploadTracker::instance();
    std::deque<Job> keep;
    std::vector<Job> dropped;
    for (Job& job : mWaiting)
    {
        if (tracker.isCancelled(job.mBatch))
        {
            dropped.push_back(std::move(job));
        }
        else
        {
            keep.push_back(std::move(job));
        }
    }
    mWaiting.swap(keep);
    for (Job& job : dropped)
    {
        if (job.mCancel)
        {
            job.mCancel(false);
        }
    }
}

void LLUploadQueue::release()
{
    mInPipeline = llmax(mInPipeline - 1, 0);
    pump();
}

void LLUploadQueue::pump()
{
    LLUploadTracker& tracker = LLUploadTracker::instance();
    while (!mWaiting.empty() && mPreparing < WORKER_THREADS && mInPipeline < PIPELINE_SLOTS)
    {
        std::shared_ptr<Job> job = std::make_shared<Job>(std::move(mWaiting.front()));
        mWaiting.pop_front();

        if (tracker.isCancelled(job->mBatch))
        {
            if (job->mCancel)
            {
                job->mCancel(false);
            }
            continue;
        }

        ++mPreparing;
        ++mInPipeline;
        tracker.setPhase(job->mBatch, job->mName, LLUploadTracker::PHASE_PREPARING);

        LLTimer main_timer;
        if (job->mBefore)
        {
            job->mBefore();
        }
        const F64 before_ms = main_timer.getElapsedTimeF64() * 1000.0;

        auto worker_ms = std::make_shared<F64>(0.0);
        const bool posted = runOnWorker(
            [job, worker_ms]()
            {
                LLTimer t;
                if (job->mWork)
                {
                    job->mWork();
                }
                *worker_ms = t.getElapsedTimeF64() * 1000.0;
            },
            [job, worker_ms, before_ms]()
            {
                if (!LLUploadQueue::instanceExists())
                {
                    return;
                }
                LLUploadQueue& self = LLUploadQueue::instance();
                self.mPreparing = llmax(self.mPreparing - 1, 0);

                // The slot is held until the job's uploads finish; release() frees it once.
                auto released = std::make_shared<bool>(false);
                std::function<void()> release = [released]()
                {
                    if (!*released && LLUploadQueue::instanceExists())
                    {
                        *released = true;
                        LLUploadQueue::instance().release();
                    }
                };

                LLTimer t;
                if (LLUploadTracker::instance().isCancelled(job->mBatch))
                {
                    if (job->mCancel)
                    {
                        job->mCancel(true);
                    }
                    release();
                }
                else if (job->mAfter)
                {
                    job->mAfter(release);
                }
                else
                {
                    release();
                }
                LL_INFOS("UploadQueue") << "Upload prepared: '" << job->mName << "' worker " << llformat("%.1f", *worker_ms)
                                        << " ms; main thread " << llformat("%.2f", before_ms + t.getElapsedTimeF64() * 1000.0)
                                        << " ms" << LL_ENDL;
                self.pump();
            });

        if (!posted)
        {
            // No worker (quitting): nothing is sent.
            --mPreparing;
            --mInPipeline;
            if (job->mCancel)
            {
                job->mCancel(false);
            }
        }
    }
}
