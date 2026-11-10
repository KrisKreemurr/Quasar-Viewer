// RR semantic contract. No scene contents, light IDs or animation clocks belong here.
#ifndef LL_RR_HISTORY_POLICY_H
#define LL_RR_HISTORY_POLICY_H
#include <array>
#include <cstdint>
namespace LLRRHistory
{
// Log bit positions: raw routing, shadow width/height, shadow estimator, overlay,
// hit march distance/steps, responsivity, jitter sign, sun path, shadow controls,
// exposure mode/value, raw configuration, guide debug, full-res signal policy.
using Policy = std::array<uint32_t, 16>;
inline uint32_t changedMask(const Policy& before, const Policy& after)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < before.size(); ++i)
        if (before[i] != after[i]) mask |= 1u << i;
    return mask;
}
// Rate limiting coalesces reason bits, never silently loses the trigger.
struct ResetBatch
{
    uint32_t reasons = 0, policyBits = 0, requests = 0;
    void add(uint32_t why, uint32_t changes)
    {
        reasons |= why; policyBits |= changes;
        if (requests != UINT32_MAX) ++requests;
    }
};
} // namespace LLRRHistory
#endif
