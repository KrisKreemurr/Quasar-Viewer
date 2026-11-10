/**
 * @file llcleanphysshapemesh_test.cpp
 * @brief Show Physics Shape When Editing (#1176): the unit-space shape mesh -- welding,
 *        normals, the slot split at the vertex limit, dropped triangles -- and the cost colours.
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

#include "linden_common.h"

#include "../llcleanphysshapemesh.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "../test/lltut.h"

namespace
{
    // Every chunk: indices inside its vertices, at most max_verts vertices, unit normals.
    bool chunksValid(const std::vector<LLPhysShapeMesh::Chunk>& chunks, U32 max_verts)
    {
        for (const LLPhysShapeMesh::Chunk& c : chunks)
        {
            if (c.pos.size() > max_verts || c.normal.size() != c.pos.size() || c.idx.size() % 3) return false;
            for (U32 i : c.idx)
                if (i >= c.pos.size()) return false;
            for (const LLVector3& n : c.normal)
                if (std::fabs(n.length() - 1.f) > 1e-4f) return false;
        }
        return true;
    }

    size_t triangles(const std::vector<LLPhysShapeMesh::Chunk>& chunks)
    {
        size_t n = 0;
        for (const LLPhysShapeMesh::Chunk& c : chunks) n += c.idx.size() / 3;
        return n;
    }
}

namespace tut
{
    struct physshapemesh_data {};
    typedef test_group<physshapemesh_data> physshapemesh_test;
    typedef physshapemesh_test::object physshapemesh_object;
    tut::physshapemesh_test physshapemesh("LLPhysShapeMesh");

    // A box: 12 triangles over its 8 welded corners, one slot.
    template<> template<>
    void physshapemesh_object::test<1>()
    {
        LLPhysShapeMesh m;
        m.addBox(LLVector3(0.f, 0.f, 0.f), LLVector3(0.25f, 0.25f, 0.25f));
        ensure_equals("triangles in", m.triangleCount(), 12u);
        const std::vector<LLPhysShapeMesh::Chunk> c = m.build();
        ensure_equals("one slot", c.size(), (size_t)1);
        ensure_equals("welded corners", c[0].pos.size(), (size_t)8);
        ensure_equals("indices", c[0].idx.size(), (size_t)36);
        ensure("valid", chunksValid(c, LLPhysShapeMesh::kMaxSlotVerts));
        for (const LLVector3& p : c[0].pos)
            ensure("corner at +-0.25", std::fabs(std::fabs(p.mV[0]) - 0.25f) < 1e-6f
                                       && std::fabs(std::fabs(p.mV[1]) - 0.25f) < 1e-6f
                                       && std::fabs(std::fabs(p.mV[2]) - 0.25f) < 1e-6f);
    }

    // Two triangles on one edge share its two vertices; the normal of a flat pair is the face's.
    template<> template<>
    void physshapemesh_object::test<2>()
    {
        LLPhysShapeMesh m;
        const std::vector<LLVector3> soup = {
            LLVector3(0.f, 0.f, 0.f), LLVector3(1.f, 0.f, 0.f), LLVector3(1.f, 1.f, 0.f),
            LLVector3(0.f, 0.f, 0.f), LLVector3(1.f, 1.f, 0.f), LLVector3(0.f, 1.f, 0.f),
            LLVector3(5.f, 5.f, 5.f) };   // a trailing partial triangle is ignored
        m.addTriangles(soup);
        ensure_equals("triangles in", m.triangleCount(), 2u);
        const std::vector<LLPhysShapeMesh::Chunk> c = m.build();
        ensure_equals("one slot", c.size(), (size_t)1);
        ensure_equals("shared edge welded", c[0].pos.size(), (size_t)4);
        ensure("valid", chunksValid(c, LLPhysShapeMesh::kMaxSlotVerts));
        for (const LLVector3& n : c[0].normal)
            ensure("normal +Z", std::fabs(n.mV[2] - 1.f) < 1e-5f);
    }

    // Degenerate and non-finite triangles are dropped; -0 welds with +0.
    template<> template<>
    void physshapemesh_object::test<3>()
    {
        LLPhysShapeMesh m;
        m.addTriangle(LLVector3(0.f, 0.f, 0.f), LLVector3(0.f, 0.f, 0.f), LLVector3(1.f, 0.f, 0.f));
        const F32 inf = std::numeric_limits<F32>::infinity();
        m.addTriangle(LLVector3(inf, 0.f, 0.f), LLVector3(1.f, 0.f, 0.f), LLVector3(0.f, 1.f, 0.f));
        m.addTriangle(LLVector3(-0.f, 0.f, 0.f), LLVector3(1.f, 0.f, 0.f), LLVector3(0.f, 1.f, 0.f));
        m.addTriangle(LLVector3(0.f, 0.f, 0.f), LLVector3(0.f, 1.f, 0.f), LLVector3(-1.f, 0.f, 0.f));
        const std::vector<LLPhysShapeMesh::Chunk> c = m.build();
        ensure_equals("kept", triangles(c), (size_t)2);
        ensure_equals("-0 and +0 welded", c[0].pos.size(), (size_t)4);
        ensure("valid", chunksValid(c, LLPhysShapeMesh::kMaxSlotVerts));
    }

    // Past the vertex limit the mesh splits into slots of whole triangles, none lost.
    template<> template<>
    void physshapemesh_object::test<4>()
    {
        LLPhysShapeMesh m;
        for (U32 i = 0; i < 50; ++i)   // a strip: each triangle shares one corner with the one before
        {
            const F32 x = (F32)i;
            m.addTriangle(LLVector3(x, 0.f, 0.f), LLVector3(x + 1.f, 0.f, 0.f), LLVector3(x, 1.f, 0.f));
        }
        const U32 limit = 7;
        const std::vector<LLPhysShapeMesh::Chunk> c = m.build(limit);
        ensure("split", c.size() > 1);
        ensure_equals("no triangle lost", triangles(c), (size_t)50);
        ensure("valid", chunksValid(c, limit));
        ensure("below three vertices nothing fits", m.build(2).empty());
    }

    // Indexed input: out-of-range indices skip their triangle.
    template<> template<>
    void physshapemesh_object::test<5>()
    {
        const LLVector3 pts[4] = { LLVector3(0.f, 0.f, 0.f), LLVector3(1.f, 0.f, 0.f),
                                   LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, 0.f, 1.f) };
        const U16 idx[9] = { 0, 1, 2,   0, 2, 3,   0, 1, 9 };
        LLPhysShapeMesh m;
        m.addIndexed(pts, 4, idx, 9);
        ensure_equals("bad triangle skipped", m.triangleCount(), 2u);
        m.addIndexed(nullptr, 4, idx, 9);
        ensure_equals("no points, nothing added", m.triangleCount(), 2u);
        const std::vector<LLPhysShapeMesh::Chunk> c = m.build();
        ensure_equals("vertices", c[0].pos.size(), (size_t)4);
        ensure("valid", chunksValid(c, LLPhysShapeMesh::kMaxSlotVerts));
    }

    // Cost steps rise with the cost; colours stay on LL's ramp; edges are half the fill;
    // the no-hull colour is LL's magenta.
    template<> template<>
    void physshapemesh_object::test<6>()
    {
        using namespace LLPhysShapeColor;
        ensure_equals("unknown cost", costStep(0.f), 0u);
        ensure_equals("NaN cost", costStep(std::numeric_limits<F32>::quiet_NaN()), 0u);
        ensure_equals("huge cost", costStep(1.0e6f), kCostSteps - 1);
        U32 prev = 0;
        for (F32 cost = 0.5f; cost < 400.f; cost *= 1.5f)
        {
            const U32 s = costStep(cost);
            ensure("monotonic", s >= prev && s < kCostSteps);
            prev = s;
        }
        ensure("steps fine enough to read as a continuous ramp", kCostSteps >= 64);
        ensure("a heavy object reaches the ramp's top step", costStep(400.f) == kCostSteps - 1);
        F32 prev_f[4] = {};
        for (U32 col = 0; col < kCostSteps; ++col)
        {
            F32 f[4], e[4];
            fill(col, f);
            edge(col, e);
            ensure("ramp alpha", f[3] >= 0.5f && f[3] <= 0.75f);
            for (int i = 0; i < 4; ++i) ensure("edge = fill x 0.5", std::fabs(e[i] - f[i] * 0.5f) < 1e-6f);
            // Neighbouring steps: at most one step of a half ramp apart (2 / kCostSteps a channel).
            for (int i = 0; col > 0 && i < 4; ++i)
                ensure("no visible jump between steps", std::fabs(f[i] - prev_f[i]) <= 2.f / (F32)kCostSteps + 1e-6f);
            std::memcpy(prev_f, f, sizeof(prev_f));
        }
        F32 f[4], e[4];
        fill(kNoHull, f);
        edge(kNoHull, e);
        ensure("magenta fill", f[0] == 1.f && f[1] == 0.f && f[2] == 1.f && f[3] == 1.f);
        ensure("magenta edges", e[0] == 1.f && e[1] == 0.f && e[2] == 1.f && e[3] == 0.5f);
    }
}
