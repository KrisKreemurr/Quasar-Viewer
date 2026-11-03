/**
 * @file qljobpool.h
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

#pragma once

#include "stdtypes.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/// A fork-join pool for the main thread (QuasarJobThreads).
///
/// The viewer's thread pools (LL::ThreadPool, LL::WorkQueue) queue fire-and-forget work, and a thread's main coroutine
/// may not wait on their results (WorkQueue::waitForResult throws there), so the main thread had no way to split a job
/// of its own across cores and carry on once all of it was done. parallelFor() does that: it hands out [0, count) in
/// chunks to its workers and takes chunks itself, and returns once every index has been run.
///
/// Its workers sleep on a condition variable between jobs, and the pool's threads take the process's default CPU
/// sets, which QLCpuSchedule makes the performance cores on a hybrid CPU.
///
/// Rules for what a job may do, since nothing here can check them:
///   - fn(begin, end) may run on any thread, its calls at the same time as each other: it may read what nothing writes
///     while the job runs, and write only what is its own index's (a slot of an output array);
///   - no GL call, no LLSingleton made for the first time, no LL_PROFILE GPU zone, no setting read through
///     gSavedSettings (an LLCachedControl's value read is fine);
///   - only the main thread calls parallelFor(), and never from inside a job.
class QLJobPool
{
public:
    static QLJobPool& instance();

    /// Starts `threads` workers; 0 runs every job on the calling thread alone. Call once, on the main thread.
    void start(U32 threads);

    /// Wakes and joins the workers. Call on the main thread, with no job running, before the app shuts down.
    void stop();

    /// The workers started; jobs also run on the calling thread.
    U32 workers() const { return (U32)mThreads.size(); }

    /// Runs fn(begin, end) over every index of [0, count), in chunks of at least `min_chunk`, across the workers and the
    /// calling thread, and returns once all have been run. A job too small to share, or a pool with no workers, runs
    /// on the calling thread as fn(0, count).
    void parallelFor(U32 count, U32 min_chunk, const std::function<void(U32, U32)>& fn);

private:
    QLJobPool() = default;
    ~QLJobPool();

    void workerLoop(U32 index);
    // Takes and runs chunks of the current job until none is left.
    void runChunks();

    std::vector<std::thread> mThreads;

    std::mutex              mMutex;
    std::condition_variable mWake;
    U64                     mGeneration = 0;  // bumped for each job, under mMutex
    bool                    mQuit = false;

    // The current job, written under mMutex before it is opened. A worker joins a job only while it is open, under
    // mMutex, and the caller closes it once every chunk has been handed out, then waits for the workers that joined:
    // a worker the OS wakes late finds the job closed and goes back to sleep, never touching a job (or its caller's
    // fn) that has ended, and the caller never waits on a worker that did not join.
    const std::function<void(U32, U32)>* mFn = nullptr;
    U32                     mCount = 0;
    U32                     mChunk = 1;
    bool                    mOpen = false;
    std::atomic<U32>        mNext{ 0 };      // the next index to hand out
    std::atomic<U32>        mActive{ 0 };    // workers that joined the current job and have not yet left it
};
