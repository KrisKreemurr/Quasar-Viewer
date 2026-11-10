/** Source-only clock-domain checks. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportclock.h"
#include "../lltransportcircuittypes.h"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
static void check(bool value, int line) { if (!value) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
static_assert(!std::is_convertible_v<LLTransportLegacyUsec, LLTransportSteadyUsec>);
static_assert(!std::is_convertible_v<LLTransportSteadyUsec, LLTransportLegacyUsec>);
int main()
{
    const LLTransportLegacyUsec epoch(1700000000000000ULL);
    const LLTransportSteadyUsec anchor(10000000000ULL), now(10005000000ULL);
    const auto legacy = llTransportLegacyAt(epoch,anchor,now);
    CHECK(legacy.value == 1700000005000000ULL);
    LLTransportCircuitSnapshot snap;
    snap.state = LLTransportSnapshotState::Present;
    snap.ownerMonotonicUsec = anchor.value;
    snap.birthMonotonicUsec = anchor.value - 2000000ULL;
    uint64_t age = 0;
    CHECK(llTransportCircuitAgeUsec(snap,now.value,age) && age == 7000000ULL);
    CHECK(llTransportLegacyAt(epoch,now,anchor).value == epoch.value);
    const auto max = (std::numeric_limits<uint64_t>::max)();
    CHECK(llTransportLegacyAt(LLTransportLegacyUsec(max-1),anchor,now).value == max);
}
