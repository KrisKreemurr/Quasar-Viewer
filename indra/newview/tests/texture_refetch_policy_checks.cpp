// #1435: a texture that delivered data earlier in the session is never marked missing by a later
// failed re-fetch (a CDN 403 after an emergency downrez dropped every copy); it is retried with a
// growing delay instead. Standalone: g++ -std=c++20 -Wall -Wextra -Werror -I indra/newview
//   indra/newview/tests/texture_refetch_policy_checks.cpp -o /tmp/t && /tmp/t
#include "lltexturerefetchpolicy.h"
#include <cstdio>

static int sFailures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++sFailures; } } while (0)

int main()
{
    using namespace LLTextureRefetchPolicy;

    // Only a regular texture (fetched by asset id) that delivered data before is kept.
    CHECK(retryInsteadOfMissing(true, true));
    CHECK(!retryInsteadOfMissing(true, false));   // never fetched: a real missing asset
    CHECK(!retryInsteadOfMissing(false, true));   // server bakes, map tiles, local files: as before
    CHECK(!retryInsteadOfMissing(false, false));

    // The delay grows 5 s, 15 s, 60 s, then stays at 5 minutes.
    CHECK(retryDelaySeconds(0) == 5.f);
    CHECK(retryDelaySeconds(1) == 15.f);
    CHECK(retryDelaySeconds(2) == 60.f);
    CHECK(retryDelaySeconds(3) == 300.f);
    CHECK(retryDelaySeconds(4) == 300.f);
    CHECK(retryDelaySeconds(1000000u) == 300.f);
    for (unsigned n = 0; n < 8; ++n) CHECK(retryDelaySeconds(n) <= retryDelaySeconds(n + 1));

    // The gate: no new request before the retry time, one at or after it; 0 = no retry pending.
    CHECK(mayRequest(0.0, 10.0));
    CHECK(!mayRequest(20.0, 19.9));
    CHECK(mayRequest(20.0, 20.0));
    CHECK(mayRequest(20.0, 25.0));

    if (sFailures) { std::printf("%d failure(s)\n", sFailures); return 1; }
    std::printf("texture_refetch_policy_checks: OK\n");
    return 0;
}
