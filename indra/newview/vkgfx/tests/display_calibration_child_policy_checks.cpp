#include "../vk_display_calibration_child_policy.h"
#include <cassert>
#include <initializer_list>

int main()
{
    using namespace vkdisplay;
    assert(classifyPidfdWait(0, 0, false) == ChildWaitDecision::Running);
    assert(classifyPidfdWait(0, 0, true) == ChildWaitDecision::Reaped);
    assert(classifyPidfdWait(-1, ECHILD, false) == ChildWaitDecision::OwnershipLost);
    for (int error : {EINTR, EINVAL, ENOSYS, EPERM})
    {
        const auto decision = classifyPidfdWait(-1, error, false);
        assert(decision == ChildWaitDecision::Retry && !releasePidfdGate(decision));
    }
    assert(!releasePidfdGate(ChildWaitDecision::Running));
    assert(releasePidfdGate(ChildWaitDecision::Reaped));
    assert(releasePidfdGate(ChildWaitDecision::OwnershipLost));
}
