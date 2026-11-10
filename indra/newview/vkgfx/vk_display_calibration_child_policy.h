#pragma once
#include <cerrno>

namespace vkdisplay
{
enum class ChildWaitDecision { Running, Reaped, OwnershipLost, Retry };

// P_PIDFD waits may fail without proving that the child has gone away.
inline ChildWaitDecision classifyPidfdWait(long result, int error, bool hasStatus) noexcept
{
    if (result == 0) return hasStatus ? ChildWaitDecision::Reaped : ChildWaitDecision::Running;
    return error == ECHILD ? ChildWaitDecision::OwnershipLost : ChildWaitDecision::Retry;
}
inline bool releasePidfdGate(ChildWaitDecision decision) noexcept
{
    return decision == ChildWaitDecision::Reaped || decision == ChildWaitDecision::OwnershipLost;
}
}
