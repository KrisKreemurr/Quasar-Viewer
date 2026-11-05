/**
 * @file qljobpool.cpp
 * @brief Quasar Viewer: a small fork-join pool, for the main thread to spread one job across cores and wait for it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code (based on Alchemy Viewer)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "qljobpool.h"

#include "llthread.h"

#include <algorithm>

QLJobPool& QLJobPool::instance()
{
    static QLJobPool pool;
    return pool;
}

QLJobPool::~QLJobPool()
{
    // A backstop: stop() is meant to have run during the app's own shutdown, while joining is still safe.
    stop();
}

void QLJobPool::start(U32 threads)
{
    if (!mThreads.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = false;
    }
    mThreads.reserve(threads);
    for (U32 i = 0; i < threads; ++i)
    {
        mThreads.emplace_back([this, i]() { workerLoop(i); });
    }
    LL_INFOS("Quasar") << "Job pool started with " << threads << " workers" << LL_ENDL;
}

void QLJobPool::stop()
{
    if (mThreads.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = true;
    }
    mWake.notify_all();
    for (std::thread& thread : mThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    mThreads.clear();
}

void QLJobPool::workerLoop(U32 index)
{
    const std::string name = "QuasarJob:" + std::to_string(index + 1);
    LL_PROFILER_SET_THREAD_NAME(name.c_str());
    set_thread_name(name.c_str());
    set_thread_fp_mode();

    U64 seen = 0;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mWake.wait(lock, [&]() { return mQuit || (mOpen && mGeneration != seen) || !mTasks.empty(); });
            if (mQuit)
            {
                return;
            }
            if (!(mOpen && mGeneration != seen))
            {
                // A task alongside the main thread: taken under the lock, run outside it, marked done under it.
                TaskHandle task = std::move(mTasks.front());
                mTasks.pop_front();
                lock.unlock();
                task->mFn();
                lock.lock();
                task->mDone = true;
                lock.unlock();
                mTaskDone.notify_all();
                continue;
            }
            seen = mGeneration;
            // Joined while the job is open, under the lock its caller closes it under.
            mActive.fetch_add(1, std::memory_order_relaxed);
        }
        runChunks();
        // What this worker wrote for the job is seen by the caller, which waits for this.
        mActive.fetch_sub(1, std::memory_order_release);
    }
}

void QLJobPool::runChunks()
{
    // mFn, mCount and mChunk were written under mMutex before the job opened, and stay until the caller has seen every
    // worker that joined leave.
    const U32 count = mCount;
    const U32 chunk = mChunk;
    for (;;)
    {
        const U32 begin = mNext.fetch_add(chunk, std::memory_order_relaxed);
        if (begin >= count)
        {
            return;
        }
        (*mFn)(begin, std::min(begin + chunk, count));
    }
}

void QLJobPool::parallelFor(U32 count, U32 min_chunk, const std::function<void(U32, U32)>& fn)
{
    if (count == 0)
    {
        return;
    }
    min_chunk = std::max(min_chunk, 1u);
    const U32 workers = (U32)mThreads.size();
    if (workers == 0 || count < min_chunk * 2)
    {
        fn(0, count);
        return;
    }

    // About four chunks a thread, so a slow chunk does not hold the others up, and none smaller than asked.
    const U32 chunk = std::max(min_chunk, count / ((workers + 1) * 4));

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFn = &fn;
        mCount = count;
        mChunk = chunk;
        mNext.store(0, std::memory_order_relaxed);
        mActive.store(0, std::memory_order_relaxed);
        ++mGeneration;
        mOpen = true;
    }
    mWake.notify_all();

    // The caller works too, so the job finishes even if no worker wakes in time.
    runChunks();

    // Every chunk is handed out: no worker joins from here on, and the ones that joined finish what they took.
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mOpen = false;
    }
    while (mActive.load(std::memory_order_acquire) != 0)
    {
        std::this_thread::yield();
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFn = nullptr;
    }
}

QLJobPool::TaskHandle QLJobPool::runAsync(std::function<void()> fn)
{
    TaskHandle task = std::make_shared<Task>();
    task->mFn = std::move(fn);
    if (mThreads.empty())
    {
        return task;  // run by wait(), on the waiting thread
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mTasks.push_back(task);
    }
    mWake.notify_one();
    return task;
}

void QLJobPool::wait(const TaskHandle& task)
{
    if (!task)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(mMutex);
    if (task->mDone)
    {
        return;
    }
    // Not taken yet (or never queued, with no workers): taken here, and run on this thread.
    auto queued = std::find(mTasks.begin(), mTasks.end(), task);
    if (queued != mTasks.end() || mThreads.empty())
    {
        if (queued != mTasks.end())
        {
            mTasks.erase(queued);
        }
        lock.unlock();
        task->mFn();
        lock.lock();
        task->mDone = true;
        return;
    }
    // A worker has it: wait for it to finish.
    mTaskDone.wait(lock, [&]() { return task->mDone; });
}
