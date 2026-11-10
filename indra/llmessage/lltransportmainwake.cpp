/** Native wake for main-thread transport work. SPDX-License-Identifier: LGPL-2.1-only */
#include "lltransportmainwake.h"
#include <atomic>
#include <cerrno>
#include <limits>
#include <memory>

#if LL_WINDOWS
#include "llwin32headers.h"
#elif LL_LINUX
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace
{
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "Transport wake must not wait on an atomic implementation mutex");
class NativeMainWake final : public LLTransportMainWake
{
public:
    explicit NativeMainWake(uint64_t initialSequence = 0) noexcept : mSequence(initialSequence)
    {
#if LL_WINDOWS
        mEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        mAvailable.store(mEvent != nullptr, std::memory_order_release);
#elif LL_LINUX
        mEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        mAvailable.store(mEvent >= 0, std::memory_order_release);
#endif
    }
    ~NativeMainWake() override
    {
        // Callers retain shared ownership until the main wait and every producer stop.
#if LL_WINDOWS
        if (mEvent) CloseHandle(mEvent);
#elif LL_LINUX
        if (mEvent >= 0) close(mEvent);
#endif
    }
    bool observe(uint64_t& out) const noexcept override
    {
        if (!mAvailable.load(std::memory_order_acquire)) return false;
        const uint64_t value = mSequence.load(std::memory_order_acquire);
        if (!mAvailable.load(std::memory_order_acquire)) return false;
        out = value;
        return true;
    }
    void signal() noexcept override
    {
        if (!mAvailable.load(std::memory_order_acquire)) return;
        uint64_t current = mSequence.load(std::memory_order_relaxed);
        bool published = false;
        for (unsigned attempt = 0; attempt < 16; ++attempt)
        {
            if (current == (std::numeric_limits<uint64_t>::max)()) break;
#ifdef LL_TRANSPORT_WAKE_TEST_BEFORE_CAS
            LL_TRANSPORT_WAKE_TEST_BEFORE_CAS();
#endif
            if (mSequence.compare_exchange_strong(current, current + 1,
                    std::memory_order_release, std::memory_order_relaxed))
            { published = true; break; }
        }
        // Saturate rather than briefly publishing a wrapped token. Contention is
        // bounded too: failure disables waiting, without blocking any producer.
        if (!published) mAvailable.store(false, std::memory_order_release);
#if LL_WINDOWS
        if (!SetEvent(mEvent)) mAvailable.store(false, std::memory_order_release);
#elif LL_LINUX
        const uint64_t increment = 1;
        for (unsigned attempt = 0; attempt < 2; ++attempt)
        {
            const ssize_t written = write(mEvent, &increment, sizeof(increment));
            if (written == static_cast<ssize_t>(sizeof(increment)) ||
                (written < 0 && errno == EAGAIN)) return; // already readable is enough
            if (written < 0 && errno == EINTR) continue;
            break;
        }
        mAvailable.store(false, std::memory_order_release);
#endif
    }
    LLTransportWakeResult waitUntil(uint64_t observed, LLTransportSteadyUsec deadline) noexcept override
    {
        for (;;)
        {
            if (!mAvailable.load(std::memory_order_acquire)) return LLTransportWakeResult::Unavailable;
            if (mSequence.load(std::memory_order_acquire) != observed) return LLTransportWakeResult::Signalled;
            const uint64_t now = llTransportSteadyNow().value;
            if (now >= deadline.value) return LLTransportWakeResult::Deadline;
            const uint64_t remaining = deadline.value - now;
            // Bounded error detection if an OS notification itself fails. Normal
            // notifications wake immediately; this does not poll or run viewer work.
            const uint64_t slice = remaining < 25000 ? remaining : 25000;
            const unsigned milliseconds = static_cast<unsigned>((slice + 999) / 1000);
#if LL_WINDOWS
            const DWORD result = WaitForSingleObject(mEvent, milliseconds);
            if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT)
            { mAvailable.store(false, std::memory_order_release); return LLTransportWakeResult::Unavailable; }
#elif LL_LINUX
            pollfd request{mEvent, POLLIN, 0};
            const int result = poll(&request, 1, static_cast<int>(milliseconds));
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || (request.revents & (POLLERR | POLLHUP | POLLNVAL)))
            { mAvailable.store(false, std::memory_order_release); return LLTransportWakeResult::Unavailable; }
            if (result > 0 && (request.revents & POLLIN))
            {
                uint64_t consumed = 0;
                const ssize_t received = read(mEvent, &consumed, sizeof(consumed));
                if (received != static_cast<ssize_t>(sizeof(consumed)) &&
                    !(received < 0 && (errno == EAGAIN || errno == EINTR)))
                { mAvailable.store(false, std::memory_order_release); return LLTransportWakeResult::Unavailable; }
            }
#else
            return LLTransportWakeResult::Unavailable;
#endif
        }
    }
private:
    std::atomic<uint64_t> mSequence;
    std::atomic<bool> mAvailable{false};
#if LL_WINDOWS
    HANDLE mEvent = nullptr;
#elif LL_LINUX
    int mEvent = -1;
#endif
};
}

std::shared_ptr<LLTransportMainWake> llCreateTransportMainWake() noexcept
{
    try
    {
        auto wake = std::make_shared<NativeMainWake>();
        uint64_t initial = 0;
        if (!wake->observe(initial)) return {};
        return wake;
    }
    catch (...)
    {
        return {};
    }
}
