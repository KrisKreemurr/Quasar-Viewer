/**
 * @file llcleaninvhull.h
 * @brief INSIDE-OUT SHELL classifier for one mesh face (the "inverted hull" cel-shading outline).
 *
 * An inverted-hull outline is a rigged copy of the body pushed a few mm out along its normals with its
 * winding (and normals) flipped: drawn back-face culled, the camera sees only the shell's FAR inside past
 * the body's silhouette -- the outline. It is a raster trick, not a surface: every ray-traced ray that
 * leaves the skin starts INSIDE the shell and meets its near face from its front side (GI rays take that
 * dark inside, millimetres away, for the whole sky; RT reflections show it; positional shadow rays met it
 * as an occluder). LLCleanMeshAsset::prepareCapture classifies each captured face once; registerCleanFace
 * marks single-sided rigged rows (LLCleanInstanceStore::kRigInsideOut) and LLCleanRTVK leaves them out of
 * the rigged acceleration structures -- the raster keeps drawing them.
 *
 * THE TEST: the sign of the face's signed volume by its WINDING (cleanShellWindingQ below): an inward-wound
 * shell's front side faces its own inside, so it is seen only from inside -- or, back-face culled, through
 * its far wall from outside, which is the outline trick. Content meant to be seen from outside (a body, a
 * sleeve, a skirt, a head, a cape) is wound outward. Shared, header-only and LL-free, with the CPU fixture
 * scripts/check/test_inv_hull.cpp (scripts/check/checks/rt_inverted_hull.py).
 *
 * What else it marks (all single-sided rigged faces): inner LININGS (a skirt's / hood's inside layer, wound
 * inward to be seen through the opening) and inward cavities authored as their own face (a mouth interior).
 * Out of RT they cost nothing visible: a ray from inside meets the garment's / head's outer layer from
 * behind (an occluder, GI's back-face stop), and from outside the outer layer hides them. Flipped-NORMAL
 * sheets (normals against the winding, 8ee88d7506) are not this case: the test reads the winding only.
 */
#ifndef LL_LLCLEANINVHULL_H
#define LL_LLCLEANINVHULL_H

#include <cmath>
#include <cstdint>

// The face's SIGNED VOLUME seen from its area-weighted centroid C, against its unsigned counterpart:
//   q = sum_i A_i s_i / sum_i A_i |s_i|,   s_i = n_i (unit winding normal) . (c_i - C)
// (sum A s / 3 is the divergence-theorem volume of a closed mesh from ANY point: its sign is the winding's,
// whatever the shape -- arms, legs, a head on a neck; for an open shell it is the volume of the cones from C).
// +1 a convex shell wound outward, -1 one wound inward; a body with limbs dilutes it toward 0 (a limb's two
// sides cancel in the numerator, not the denominator) but keeps its sign. A flat card has s ~ 0 everywhere
// (the flatness gate: sum A |s| against the face's mean radius) and is never an inside-out shell.
constexpr float kInvHullMaxQ = -0.25f;    // q at or below this: wound inward
constexpr float kInvHullMinDepth = 0.02f; // sum A |s| / (sum A x mean |c - C|) below this: a flat card
// fewer triangles than this is a card / a small detail, never a shell
constexpr uint32_t kInvHullMinTris = 24u;

// q (above) of a face, or 0 when it is too small / flat / degenerate to be a shell.
// pos(i) -> const float* to vertex i's xyz (any stride); idx: n_idx 16-bit face-local indices.
template <class PosFn>
inline float cleanShellWindingQ(PosFn pos, const uint16_t* idx, uint32_t n_idx, uint32_t n_verts)
{
    if (!idx || n_idx < 3u * kInvHullMinTris) return 0.f;
    const uint32_t n_tri = n_idx / 3u;
    double cx = 0.0, cy = 0.0, cz = 0.0, wsum = 0.0;
    uint32_t used = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        double num = 0.0, den = 0.0, spread = 0.0;
        for (uint32_t t = 0; t < n_tri; ++t)
        {
            const uint32_t i0 = idx[3u * t], i1 = idx[3u * t + 1u], i2 = idx[3u * t + 2u];
            if (i0 >= n_verts || i1 >= n_verts || i2 >= n_verts) return 0.f;
            const float* a = pos(i0); const float* b = pos(i1); const float* c = pos(i2);
            const double ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
            const double fx = c[0] - a[0], fy = c[1] - a[1], fz = c[2] - a[2];
            const double nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
            const double ar = std::sqrt(nx * nx + ny * ny + nz * nz);   // (2 x the area: weights only)
            if (!(ar > 0.0) || !std::isfinite(ar)) continue;
            const double mx = (a[0] + b[0] + c[0]) / 3.0, my = (a[1] + b[1] + c[1]) / 3.0, mz = (a[2] + b[2] + c[2]) / 3.0;
            if (pass == 0) { cx += ar * mx; cy += ar * my; cz += ar * mz; wsum += ar; ++used; continue; }
            const double dx = mx - cx, dy = my - cy, dz = mz - cz;
            const double s = (nx * dx + ny * dy + nz * dz) / ar;
            num += ar * s;
            den += ar * std::fabs(s);
            spread += ar * std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        if (pass == 0)
        {
            if (used < kInvHullMinTris || !(wsum > 0.0)) return 0.f;
            cx /= wsum; cy /= wsum; cz /= wsum;
            continue;
        }
        if (!(spread > 0.0) || den < kInvHullMinDepth * spread) return 0.f;
        return (float)(num / den);
    }
    return 0.f;
}
template <class PosFn>
inline bool cleanInsideOutShell(PosFn pos, const uint16_t* idx, uint32_t n_idx, uint32_t n_verts)
{
    return cleanShellWindingQ(pos, idx, n_idx, n_verts) <= kInvHullMaxQ;
}

#endif // LL_LLCLEANINVHULL_H
