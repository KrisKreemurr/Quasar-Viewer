#include "../vk_display_calibration_job.h"
#include <cassert>
#include <functional>
#include <initializer_list>
#include <new>
#include <utility>

struct FakeQueue
{
    enum class Mode { Accept, Full, Closed, AllocationFailure } mode = Mode::Accept;
    std::function<void()> work;
    bool tryPost(std::function<void()> value)
    {
        if (mode == Mode::AllocationFailure) throw std::bad_alloc();
        if (mode != Mode::Accept) return false;
        work = std::move(value);
        return true;
    }
};

// Fake admission/reply failures exercise the actual lease's ownership rules;
// configured WorkQueue shutdown and threading still need integration tests.
int main()
{
    using namespace vkdisplay;
    auto rejected = std::make_shared<CalibrationJob>();
    {
        auto lease = std::make_shared<CalibrationJobLease>(rejected);
        std::function<void()> work = [lease] { (void)lease; };
        (void)work;
        // A full/closed queue rejects work and destroys its callable.
    }
    assert(rejected->released.load());

    auto dropped = std::make_shared<CalibrationJob>();
    std::function<void()> queued;
    {
        auto lease = std::make_shared<CalibrationJobLease>(dropped);
        queued = [lease] { (void)lease; };
    }
    assert(!dropped->released.load());
    queued = {}; // Queue closes and discards its accepted work.
    assert(dropped->released.load());

    auto failed = std::make_shared<CalibrationJob>();
    {
        auto lease = std::make_shared<CalibrationJobLease>(failed);
        std::function<void()> work = [lease] { (void)lease; throw std::bad_alloc(); };
        try { work(); } catch (const std::bad_alloc&) {}
        assert(!failed->released.load());
    }
    assert(failed->released.load());

    for (auto mode : {FakeQueue::Mode::Full, FakeQueue::Mode::Closed, FakeQueue::Mode::AllocationFailure})
    {
        auto job = std::make_shared<CalibrationJob>();
        {
            FakeQueue worker; worker.mode = mode;
            auto lease = std::make_shared<CalibrationJobLease>(job);
            try { (void)worker.tryPost([lease] { (void)lease; }); } catch (const std::bad_alloc&) {}
        }
        assert(job->released.load());
    }

    auto replyRejected = std::make_shared<CalibrationJob>();
    {
        FakeQueue worker, reply;
        reply.mode = FakeQueue::Mode::Closed;
        auto lease = std::make_shared<CalibrationJobLease>(replyRejected);
        const bool accepted = worker.tryPost([lease, &reply] { (void)lease; (void)reply.tryPost([] {}); });
        assert(accepted);
        lease.reset();
        worker.work();
        worker.work = {};
    }
    assert(replyRejected->released.load());

    // A is in flight, then B becomes pending. No reply is required: once the
    // lease publishes release, the wake's policy selects a retry for B.
    auto first = std::make_shared<CalibrationJob>();
    auto firstLease = std::make_shared<CalibrationJobLease>(first);
    first->ran = true;
    assert(decideCalibrationPoll(first->released.load(), first->ran, false, false, true) == CalibrationPollDecision::Wait);
    firstLease.reset();
    assert(decideCalibrationPoll(first->released.load(), first->ran, false, false, true) == CalibrationPollDecision::Retry);
    auto second = std::make_shared<CalibrationJob>();
    auto secondLease = std::make_shared<CalibrationJobLease>(second);
    second->ran = true;
    secondLease.reset();
    assert(decideCalibrationPoll(second->released.load(), second->ran, true, true, false) == CalibrationPollDecision::Apply);
    assert(decideCalibrationPoll(true, false, true, true, false) == CalibrationPollDecision::Retry);
}
