/**
 * @file llcleanphysshapemesh.h
 * @brief Show Physics Shape When Editing (#1176): one physics shape as an indexed unit-space
 *        mesh for the clean mesh store, and the cost colours shapes are drawn in.
 *
 * LLCleanPhysShapeStore (llcleanphysshape.h) decides WHICH shape an object shows (LL's
 * renderPhysicsShape rules) and feeds its triangles here in the object's unit (volume)
 * space, the space its clean instance row's model matrix places. build() welds equal
 * positions, gives every vertex an area-weighted normal and splits the result into store
 * slots of at most kMaxSlotVerts vertices (slot-local indices are 16-bit). Pure geometry:
 * no viewer state, so its unit test links it alone (tests/llcleanphysshapemesh_test.cpp).
 */

#pragma once

#include "stdtypes.h"
#include "v3math.h"

#include <vector>

class LLPhysShapeMesh
{
public:
    // = LLCleanQV::kMaxSlotVerts: a store slot's indices are 16-bit.
    static constexpr U32 kMaxSlotVerts = 65536;

    void addTriangle(const LLVector3& a, const LLVector3& b, const LLVector3& c);
    // Consecutive triples (a decomposition's hull and physics meshes).
    void addTriangles(const std::vector<LLVector3>& corners);
    // pts[idx[i]] triples (a volume face, a prim's hull); a triangle naming a point past npts
    // is skipped.
    void addIndexed(const LLVector3* pts, U32 npts, const U16* idx, U32 nidx);
    // The axis-aligned box [c - r, c + r] (LL drawBox): 12 triangles.
    void addBox(const LLVector3& c, const LLVector3& r);

    U32  triangleCount() const { return (U32)(mCorners.size() / 3); }
    bool empty() const { return mCorners.empty(); }

    struct Chunk
    {
        std::vector<LLVector3> pos;      // welded
        std::vector<LLVector3> normal;   // unit, area-weighted (+Z where a vertex has no area)
        std::vector<U32>       idx;      // into pos, 3 per triangle
    };
    // Equal positions welded; split into chunks of at most max_verts vertices, whole
    // triangles each. Triangles with two equal corners or a non-finite one are dropped.
    std::vector<Chunk> build(U32 max_verts = kMaxSlotVerts) const;

private:
    std::vector<LLVector3> mCorners;   // 3 per triangle
};

// The colour a shape is drawn in: LL's object-cost ramp (ObjectCostLowColor -> Mid -> High at
// n = 1 - e^(-cost / 50); those settings went with the GL pipeline) in kCostSteps steps, and
// LL's magenta for a prim whose convex hull could not be made.
namespace LLPhysShapeColor
{
    constexpr U32 kCostSteps = 64;   // fine enough to read as LL's continuous ramp
    constexpr U32 kNoHull    = kCostSteps;
    constexpr U32 kColors    = kCostSteps + 1;

    U32  costStep(F32 cost);              // [0, kCostSteps); an unknown cost is step 0
    void fill(U32 color, F32 out[4]);     // the filled passes
    void edge(U32 color, F32 out[4]);     // the edges: LL's colour x 0.5 (magenta: alpha 0.5)
}
