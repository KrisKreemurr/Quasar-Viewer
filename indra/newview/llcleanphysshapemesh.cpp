/**
 * @file llcleanphysshapemesh.cpp
 * @brief Show Physics Shape When Editing (#1176): unit-space shape meshes and cost colours.
 *        See llcleanphysshapemesh.h.
 */
#include "linden_common.h"

#include "llcleanphysshapemesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace
{
// A position's bit pattern (-0 folded into +0), for welding exactly equal corners.
struct PosKey
{
    U32 bits[3];
    bool operator==(const PosKey& o) const
    {
        return bits[0] == o.bits[0] && bits[1] == o.bits[1] && bits[2] == o.bits[2];
    }
};

struct PosKeyHash
{
    size_t operator()(const PosKey& k) const
    {
        U64 h = 1469598103934665603ull;   // FNV-1a over the three words
        for (U32 b : k.bits)
        {
            h ^= b;
            h *= 1099511628211ull;
        }
        return (size_t)h;
    }
};

PosKey posKey(const LLVector3& p)
{
    PosKey k;
    for (int i = 0; i < 3; ++i)
    {
        const F32 f = p.mV[i] == 0.f ? 0.f : p.mV[i];
        std::memcpy(&k.bits[i], &f, sizeof(F32));
    }
    return k;
}

// LL's ObjectCost{Low,Mid,High}Color defaults.
constexpr F32 kLow[4]  = {0.f, 0.5f, 1.f, 0.5f};
constexpr F32 kMid[4]  = {1.f, 0.75f, 0.f, 0.65f};
constexpr F32 kHigh[4] = {1.f, 0.f, 0.f, 0.75f};
} // namespace

void LLPhysShapeMesh::addTriangle(const LLVector3& a, const LLVector3& b, const LLVector3& c)
{
    mCorners.push_back(a);
    mCorners.push_back(b);
    mCorners.push_back(c);
}

void LLPhysShapeMesh::addTriangles(const std::vector<LLVector3>& corners)
{
    for (size_t i = 0; i + 2 < corners.size(); i += 3)
        addTriangle(corners[i], corners[i + 1], corners[i + 2]);
}

void LLPhysShapeMesh::addIndexed(const LLVector3* pts, U32 npts, const U16* idx, U32 nidx)
{
    if (!pts || !idx) return;
    for (U32 i = 0; i + 2 < nidx; i += 3)
    {
        if (idx[i] >= npts || idx[i + 1] >= npts || idx[i + 2] >= npts) continue;
        addTriangle(pts[idx[i]], pts[idx[i + 1]], pts[idx[i + 2]]);
    }
}

void LLPhysShapeMesh::addBox(const LLVector3& c, const LLVector3& r)
{
    LLVector3 v[8];
    for (U32 i = 0; i < 8; ++i)
        v[i].set(c.mV[0] + ((i & 1) ? r.mV[0] : -r.mV[0]),
                 c.mV[1] + ((i & 2) ? r.mV[1] : -r.mV[1]),
                 c.mV[2] + ((i & 4) ? r.mV[2] : -r.mV[2]));
    static const U8 q[6][4] = { {0,1,3,2}, {4,6,7,5}, {0,4,5,1}, {2,3,7,6}, {0,2,6,4}, {1,5,7,3} };
    for (const auto& f : q)
    {
        addTriangle(v[f[0]], v[f[1]], v[f[2]]);
        addTriangle(v[f[0]], v[f[2]], v[f[3]]);
    }
}

std::vector<LLPhysShapeMesh::Chunk> LLPhysShapeMesh::build(U32 max_verts) const
{
    std::vector<Chunk> out;
    if (max_verts < 3) return out;
    std::unordered_map<PosKey, U32, PosKeyHash> welded;   // position -> vertex of the open chunk
    Chunk cur;
    auto close = [&]()
    {
        if (cur.idx.empty()) return;
        for (LLVector3& n : cur.normal)
        {
            const F32 len = n.length();
            n = len > 0.f ? n * (1.f / len) : LLVector3(0.f, 0.f, 1.f);
        }
        out.push_back(std::move(cur));
        cur = Chunk();
        welded.clear();
    };
    for (size_t t = 0; t + 2 < mCorners.size(); t += 3)
    {
        const LLVector3* c = &mCorners[t];
        if (!c[0].isFinite() || !c[1].isFinite() || !c[2].isFinite()) continue;
        const PosKey k[3] = {posKey(c[0]), posKey(c[1]), posKey(c[2])};
        if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) continue;
        size_t fresh = 0;
        for (const PosKey& key : k) fresh += welded.count(key) ? 0 : 1;
        if (cur.pos.size() + fresh > max_verts) close();
        const LLVector3 area = (c[1] - c[0]) % (c[2] - c[0]);   // |area| = twice the triangle's
        for (int i = 0; i < 3; ++i)
        {
            auto it = welded.find(k[i]);
            U32 v;
            if (it == welded.end())
            {
                v = (U32)cur.pos.size();
                welded.emplace(k[i], v);
                cur.pos.push_back(c[i]);
                cur.normal.push_back(LLVector3(0.f, 0.f, 0.f));
            }
            else
            {
                v = it->second;
            }
            cur.normal[v] += area;
            cur.idx.push_back(v);
        }
    }
    close();
    return out;
}

U32 LLPhysShapeColor::costStep(F32 cost)
{
    if (!(cost > 0.f)) return 0;   // (unknown, or NaN)
    const F32 n = 1.f - std::exp(-cost / 50.f);
    return std::min((U32)(n * (F32)kCostSteps), kCostSteps - 1);
}

void LLPhysShapeColor::fill(U32 color, F32 out[4])
{
    if (color >= kCostSteps)
    {
        out[0] = 1.f; out[1] = 0.f; out[2] = 1.f; out[3] = 1.f;   // LL: magenta box, no hull
        return;
    }
    const F32 n = ((F32)color + 0.5f) / (F32)kCostSteps;   // the step's middle
    const F32* a = n <= 0.5f ? kLow : kMid;
    const F32* b = n <= 0.5f ? kMid : kHigh;
    const F32 t = n <= 0.5f ? 2.f * n : 2.f * (n - 0.5f);
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

void LLPhysShapeColor::edge(U32 color, F32 out[4])
{
    if (color >= kCostSteps)
    {
        out[0] = 1.f; out[1] = 0.f; out[2] = 1.f; out[3] = 0.5f;
        return;
    }
    fill(color, out);
    for (int i = 0; i < 4; ++i) out[i] *= 0.5f;
}
