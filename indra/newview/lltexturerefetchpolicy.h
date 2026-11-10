/**
 * @file lltexturerefetchpolicy.h
 * @brief #1435: what a failed re-fetch of a texture that loaded earlier does.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLTEXTUREREFETCHPOLICY_H
#define LL_LLTEXTUREREFETCHPOLICY_H

// A texture that delivered data earlier in the session exists: its asset id is immutable. When
// every copy of it was dropped (an emergency downrez under low memory) and the re-fetch then fails
// (the asset CDN answered 403 minutes after serving it), marking it missing would draw it white for
// the rest of the session. It is retried after a growing delay instead. Textures that never
// delivered data, and the kinds whose server copy can legitimately go away (server bakes) or never
// exist (map tiles), keep the old rule. Header-only and free of viewer types so the policy is tested
// standalone (indra/newview/tests/texture_refetch_policy_checks.cpp).
namespace LLTextureRefetchPolicy
{
    // regularAsset: fetched by asset id (FTT_DEFAULT). fetchedBefore: data arrived this session.
    inline bool retryInsteadOfMissing(bool regularAsset, bool fetchedBefore)
    {
        return regularAsset && fetchedBefore;
    }

    // Seconds to wait after the (failures + 1)-th failed re-fetch in a row: 5, 15, 60, then 300.
    inline float retryDelaySeconds(unsigned failures)
    {
        static const float kDelays[] = { 5.f, 15.f, 60.f, 300.f };
        const unsigned last = sizeof(kDelays) / sizeof(kDelays[0]) - 1;
        return kDelays[failures < last ? failures : last];
    }

    // retryAt: the earliest time of the next request, 0 when no retry is pending.
    inline bool mayRequest(double retryAt, double now)
    {
        return retryAt <= 0.0 || now >= retryAt;
    }
}

#endif // LL_LLTEXTUREREFETCHPOLICY_H
