/**
 * @file slstraightalpha_test.cpp
 * @brief Straight-alpha RGBA (#1328): premultiply / unpremultiply for resampling a cut-out,
 *        the transparent floor, and the edge a resampled cut-out must not darken.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../slstraightalpha.h"

#include "../test/lltut.h"

#include <cstdlib>
#include <vector>

namespace
{
    std::vector<U8> px(U8 r, U8 g, U8 b, U8 a) { return { r, g, b, a }; }

    void ensurePixel(const char* what, const std::vector<U8>& p, U8 r, U8 g, U8 b, U8 a)
    {
        std::string msg = std::string(what) + llformat(": got %d %d %d %d, want %d %d %d %d",
                                                       p[0], p[1], p[2], p[3], r, g, b, a);
        tut::ensure(msg, p[0] == r && p[1] == g && p[2] == b && p[3] == a);
    }
}

namespace tut
{
    struct straightalpha_data {};
    typedef test_group<straightalpha_data> straightalpha_test;
    typedef straightalpha_test::object straightalpha_object;
    tut::straightalpha_test straightalpha("SLStraightAlpha");

    // Premultiply: opaque unchanged, transparent becomes black, half alpha halves the colour
    // (rounded to nearest).
    template<> template<>
    void straightalpha_object::test<1>()
    {
        std::vector<U8> p = px(200, 100, 3, 255);
        slPremultiplyRGBA(p.data(), 1);
        ensurePixel("opaque", p, 200, 100, 3, 255);

        p = px(200, 100, 3, 0);
        slPremultiplyRGBA(p.data(), 1);
        ensurePixel("transparent", p, 0, 0, 0, 0);

        p = px(200, 100, 3, 128);
        slPremultiplyRGBA(p.data(), 1);
        ensurePixel("half", p, 100, 50, 2, 128);   // 200 * 128 / 255 = 100.4, 3 * 128 / 255 = 1.5
    }

    // Unpremultiply: opaque unchanged; alpha 0 is the floor -- transparent black, whatever
    // colour a resampler left there; colour above its alpha (light with no coverage) clamps.
    template<> template<>
    void straightalpha_object::test<2>()
    {
        std::vector<U8> p = px(200, 100, 3, 255);
        slUnpremultiplyRGBA(p.data(), 1);
        ensurePixel("opaque", p, 200, 100, 3, 255);

        p = px(7, 9, 11, 0);
        slUnpremultiplyRGBA(p.data(), 1);
        ensurePixel("transparent floor", p, 0, 0, 0, 0);

        p = px(100, 50, 2, 128);
        slUnpremultiplyRGBA(p.data(), 1);
        ensurePixel("half", p, 199, 100, 4, 128);   // 100 * 255 / 128 = 199.2, 2 * 255 / 128 = 3.98

        p = px(90, 10, 0, 64);
        slUnpremultiplyRGBA(p.data(), 1);
        ensurePixel("over-bright clamps", p, 255, 40, 0, 64);
    }

    // A round trip at a usable alpha stays within the premultiplied quantum (255 / alpha / 2).
    template<> template<>
    void straightalpha_object::test<3>()
    {
        for (int a = 32; a <= 255; a += 37)
        {
            for (int c = 0; c <= 255; c += 17)
            {
                std::vector<U8> p = px((U8)c, (U8)(255 - c), (U8)(c / 2), (U8)a);
                slPremultiplyRGBA(p.data(), 1);
                slUnpremultiplyRGBA(p.data(), 1);
                const int tol = (255 + a - 1) / a / 2 + 1;
                ensure(llformat("round trip c %d a %d: %d", c, a, p[0]), std::abs((int)p[0] - c) <= tol);
                ensure(llformat("round trip c %d a %d: %d", 255 - c, a, p[1]), std::abs((int)p[1] - (255 - c)) <= tol);
                ensure_equals("alpha kept", (int)p[3], a);
            }
        }
    }

    // THE EDGE. A resampler averaging an opaque red pixel with a transparent one: in
    // premultiplied space the half-covered result is still red (straight averaging gives a
    // half-dark red -- the dark fringe of a resized cut-out).
    template<> template<>
    void straightalpha_object::test<4>()
    {
        std::vector<U8> two = { 255, 0, 0, 255,   0, 0, 0, 0 };
        slPremultiplyRGBA(two.data(), 2);
        std::vector<U8> mid(4);
        for (int k = 0; k < 4; ++k) mid[k] = (U8)((two[k] + two[4 + k] + 1) / 2);
        slUnpremultiplyRGBA(mid.data(), 1);
        ensurePixel("edge stays red", mid, 255, 0, 0, 128);
    }

    // Many pixels at once, and none (no crash on an empty buffer).
    template<> template<>
    void straightalpha_object::test<5>()
    {
        std::vector<U8> row = { 10, 20, 30, 255,   40, 50, 60, 0,   80, 80, 80, 51 };
        slPremultiplyRGBA(row.data(), 3);
        ensure_equals("pixel 0 kept", (int)row[0], 10);
        ensure_equals("pixel 1 cleared", (int)row[4], 0);
        ensure_equals("pixel 2 scaled", (int)row[8], 16);   // 80 * 51 / 255
        slPremultiplyRGBA(nullptr, 0);
        slUnpremultiplyRGBA(nullptr, 0);
    }
}
