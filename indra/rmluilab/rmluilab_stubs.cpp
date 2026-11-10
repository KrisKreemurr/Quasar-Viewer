// #703 slrmluilab: definitions the harness's subset of vkgfx refers to but never uses
// (viewer services outside the UI backend). Filled in from the link errors; each one is
// a no-op or a "not available" answer.
#include "llcleanreplayvk.h"

// Instant replay (video encode queue): not part of a UI harness device.
bool LLReplayEncoder::deviceSupport(VkPhysicalDevice, uint32_t&)
{
    return false;
}
const std::vector<const char*>& LLReplayEncoder::deviceExtensions()
{
    static const std::vector<const char*> none;
    return none;
}
