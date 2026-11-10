/** Socket-owner native wait. SPDX-License-Identifier: LGPL-2.1-only */
#include "lltransportownerwait.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#if LL_LINUX
#include <sys/eventfd.h>
#endif
#endif

namespace
{
template<typename T> struct WaitAllocator
{
    using value_type = T;
    WaitAllocator() noexcept = default;
    template<typename U> WaitAllocator(const WaitAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        if (count > LLTransportOwnerWaitHeapBudgetBytes / sizeof(T)) throw std::bad_alloc();
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* data, std::size_t count) noexcept
    { std::allocator<T>{}.deallocate(data, count); }
    template<typename U> bool operator==(const WaitAllocator<U>&) const noexcept { return true; }
    template<typename U> bool operator!=(const WaitAllocator<U>&) const noexcept { return false; }
};
static_assert(std::atomic<bool>::is_always_lock_free);
class NativeOwnerWait final : public LLTransportOwnerWait
{
public:
    NativeOwnerWait() noexcept
    {
#if LL_WINDOWS
        mCommandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        mSocketEvent = WSACreateEvent();
        mAvailable.store(mCommandEvent && mSocketEvent != WSA_INVALID_EVENT);
#elif LL_LINUX
        mRead = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        mWrite = mRead;
        mAvailable.store(mRead >= 0);
#else
        int ends[2] = {-1, -1};
        if (pipe(ends) == 0)
        {
            mRead = ends[0]; mWrite = ends[1];
            bool ok = true;
            for (int fd : ends)
                ok = ok && fcntl(fd, F_SETFL, O_NONBLOCK) == 0
                    && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
            mAvailable.store(ok);
        }
#endif
    }
    ~NativeOwnerWait() override
    {
        // No producer/waiter remains; socket lifetime extends beyond this object.
        detachSocketOnOwner();
#if LL_WINDOWS
        if (mSocketEvent != WSA_INVALID_EVENT) WSACloseEvent(mSocketEvent);
        if (mCommandEvent) CloseHandle(mCommandEvent);
#else
        if (mWrite >= 0 && mWrite != mRead) close(mWrite);
        if (mRead >= 0) close(mRead);
#endif
    }
    bool available() const noexcept { return mAvailable.load(std::memory_order_acquire); }
    bool bindSocketOnOwner(int socket) noexcept override
    {
        if (!available() || socket < 0 || (mSocket >= 0 && mSocket != socket)) return false;
        if (mSocket == socket) return true;
#if LL_WINDOWS
        int socketType = 0;
        int typeSize = sizeof(socketType);
        if (getsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_TYPE,
                       reinterpret_cast<char*>(&socketType), &typeSize) != 0
            || socketType != SOCK_DGRAM) return false;
        if (WSAEventSelect(static_cast<SOCKET>(socket), mSocketEvent, FD_READ | FD_CLOSE) != 0)
            return false;
        mMask = FD_READ | FD_CLOSE;
#else
        const int flags = fcntl(socket, F_GETFL);
        int socketType = 0;
        socklen_t typeSize = sizeof(socketType);
        if (flags < 0 || !(flags & O_NONBLOCK)
            || getsockopt(socket, SOL_SOCKET, SO_TYPE, &socketType, &typeSize) != 0
            || socketType != SOCK_DGRAM) return false;
#endif
        mSocket = socket;
        return true;
    }
    void detachSocketOnOwner() noexcept override
    {
#if LL_WINDOWS
        if (mSocket >= 0)
            WSAEventSelect(static_cast<SOCKET>(mSocket), WSA_INVALID_EVENT, 0);
        mMask = 0;
#endif
        mSocket = -1;
    }
    void signal() noexcept override
    {
        if (!available()) return;
#if LL_WINDOWS
        if (!SetEvent(mCommandEvent)) mAvailable.store(false, std::memory_order_release);
#else
#if LL_LINUX
        const uint64_t value = 1;
#else
        const uint8_t value = 1;
#endif
        for (unsigned attempt = 0; attempt != 2; ++attempt)
        {
            const auto n = write(mWrite, &value, sizeof(value));
            if (n == static_cast<ssize_t>(sizeof(value)) || (n < 0 && errno == EAGAIN)) return;
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        mAvailable.store(false, std::memory_order_release);
#endif
    }
    LLTransportOwnerWaitResult waitUntil(int socket, LLTransportSteadyUsec deadline,
                                         bool wantWritable, bool wantReadable) noexcept override
    {
        LLTransportOwnerWaitResult result{};
        if (!available() || socket != mSocket || socket < 0)
        { result.error = true; return result; }
#if LL_WINDOWS
        const long mask = FD_CLOSE | (wantReadable ? FD_READ : 0) | (wantWritable ? FD_WRITE : 0);
        if (mask != mMask)
        {
            if (WSAEventSelect(static_cast<SOCKET>(socket), mSocketEvent, mask) != 0)
            { result.error = true; result.nativeError = WSAGetLastError(); return result; }
            mMask = mask;
        }
#endif
        for (;;)
        {
            if (!available()) { result.error = true; return result; }
            const uint64_t now = llTransportSteadyNow().value;
            // Even a deadline already reached performs a zero-time readiness
            // check, so commands/stop cannot be hidden by an expired timer.
            const uint64_t remaining = deadline.value > now ? deadline.value - now : 0;
            const uint64_t slice = remaining < 25000 ? remaining : 25000;
            const unsigned milliseconds = static_cast<unsigned>((slice + 999) / 1000);
#if LL_WINDOWS
            HANDLE handles[2] = {mCommandEvent, mSocketEvent};
            const DWORD status = WaitForMultipleObjects(2, handles, FALSE, milliseconds);
            if (status != WAIT_TIMEOUT && status != WAIT_OBJECT_0 && status != WAIT_OBJECT_0 + 1)
            { result.error = true; result.nativeError = GetLastError(); return result; }
            result.signalled = status == WAIT_OBJECT_0;
            // Collect both ready sources even if the command handle won first.
            if (WaitForSingleObject(mSocketEvent, 0) == WAIT_OBJECT_0)
            {
                WSANETWORKEVENTS events{};
                if (WSAEnumNetworkEvents(static_cast<SOCKET>(socket), mSocketEvent, &events) != 0)
                { result.error = true; result.nativeError = WSAGetLastError(); return result; }
                result.readable = wantReadable && (events.lNetworkEvents & FD_READ) != 0;
                result.writable = wantWritable && (events.lNetworkEvents & FD_WRITE) != 0;
                if (events.lNetworkEvents & FD_CLOSE)
                { result.error = true; result.nativeError = events.iErrorCode[FD_CLOSE_BIT]; }
                else
                {
                    const int eventError = events.iErrorCode[FD_READ_BIT]
                        ? events.iErrorCode[FD_READ_BIT] : events.iErrorCode[FD_WRITE_BIT];
                    if (eventError)
                    { result.socketErrorPending = true; result.nativeError = eventError; }
                }
            }
#else
            pollfd handles[2] = {{mRead, POLLIN, 0},
                                {(wantReadable || wantWritable) ? socket : -1,
                                 static_cast<short>((wantReadable ? POLLIN : 0) | (wantWritable ? POLLOUT : 0)), 0}};
            const int status = poll(handles, 2, static_cast<int>(milliseconds));
            if (status < 0)
            {
                if (errno == EINTR) continue;
                result.error = true; result.nativeError = static_cast<unsigned>(errno); return result;
            }
            if ((handles[0].revents & (POLLERR | POLLHUP | POLLNVAL))
                || (handles[1].revents & (POLLHUP | POLLNVAL)))
            { result.error = true; return result; }
            if (handles[1].revents & POLLERR)
            {
                int socketError = 0;
                socklen_t errorSize = sizeof(socketError);
                if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0)
                { result.error = true; result.nativeError = static_cast<unsigned>(errno); return result; }
                result.socketErrorPending = true;
                result.nativeError = static_cast<unsigned>(socketError);
            }
            result.signalled = (handles[0].revents & POLLIN) != 0;
            result.readable = wantReadable && (handles[1].revents & POLLIN) != 0;
            result.writable = wantWritable && (handles[1].revents & POLLOUT) != 0;
            if (result.signalled)
            {
#if LL_LINUX
                uint64_t consumed = 0;
#else
                uint8_t consumed[256]{};
#endif
                const auto n = read(mRead, &consumed, sizeof(consumed));
                if (n < 0 && errno != EAGAIN && errno != EINTR)
                { result.error = true; result.nativeError = static_cast<unsigned>(errno); return result; }
            }
#endif
            result.deadline = llTransportSteadyNow().value >= deadline.value;
            if (result.readable || result.writable || result.signalled || result.deadline
                || result.socketErrorPending) return result;
        }
    }
private:
    std::atomic<bool> mAvailable{false};
    int mSocket = -1;
#if LL_WINDOWS
    HANDLE mCommandEvent = nullptr;
    WSAEVENT mSocketEvent = WSA_INVALID_EVENT;
    long mMask = 0;
#else
    int mRead = -1, mWrite = -1;
#endif
};
}
std::shared_ptr<LLTransportOwnerWait> llCreateTransportOwnerWait() noexcept
{
    try
    {
        static_assert(sizeof(NativeOwnerWait) <= 128);
        auto wake = std::allocate_shared<NativeOwnerWait>(WaitAllocator<NativeOwnerWait>{});
        return wake->available() ? wake : std::shared_ptr<LLTransportOwnerWait>{};
    }
    catch (...) { return {}; }
}
