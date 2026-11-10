// CPU-only policy fixture for speculative display-target allocation. The renderer's
// central VkDeviceContext::runRelief uses reliefSuppressedForCurrentThread() directly.
#include "../vk_device.h"

#include <cassert>

namespace
{
bool mockRelief(uint32_t& calls)
{
    if (VkDeviceContext::reliefSuppressedForCurrentThread()) return false;
    ++calls;
    return true;
}

bool speculativeSoftAttempt(bool allocationFits, uint32_t& reliefCalls)
{
    VkDeviceContext::NoReliefScope noRelief;
    if (!allocationFits)
        (void)mockRelief(reliefCalls); // real preflight/refusal path must be suppressed
    return allocationFits;
}
}

int main()
{
    uint32_t reliefCalls = 0;
    assert(!VkDeviceContext::reliefSuppressedForCurrentThread());
    assert(!speculativeSoftAttempt(false, reliefCalls));
    assert(reliefCalls == 0);
    assert(!VkDeviceContext::reliefSuppressedForCurrentThread());

    {
        VkDeviceContext::NoReliefScope outer;
        assert(VkDeviceContext::reliefSuppressedForCurrentThread());
        {
            VkDeviceContext::NoReliefScope inner;
            assert(VkDeviceContext::reliefSuppressedForCurrentThread());
            assert(!mockRelief(reliefCalls));
        }
        assert(VkDeviceContext::reliefSuppressedForCurrentThread());
    }
    assert(!VkDeviceContext::reliefSuppressedForCurrentThread());

    assert(mockRelief(reliefCalls));
    assert(reliefCalls == 1);
}
