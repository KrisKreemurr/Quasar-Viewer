// CPU fixture for the rigged refit POSE SKIP threshold (indra/newview/llcleanrigposeskip.h; check rig-pose-skip,
// scripts/check/checks/rig_pose_skip.py).
//
// A skin patch (a sphere, the belly of an avatar the camera is close to) is drawn at its current pose; its rigged
// BLAS holds the pose of its last refit, grown outward by the pose drift the skip allowed (the worst case: the
// traced skin lies OUTSIDE the drawn one by that much). Every visible receiver's GI ray leaves from the raster
// point lifted along its eye-facing normal exactly as clean_deferred_light.comp giFreshRay lifts it:
//     lift = 0.5 x camd x pa / max(|Ng . V|, 0.25)       (pa: the receiver pixel's angle, GI scale 1)
// A receiver whose origin lies inside the traced sphere starts inside the body: its ray meets the body's back face
// first and the GI back-face stop returns no light (the dark skin; the receivers left outside are the specks).
//
// Asserts: the OLD threshold (2 mm within 10 m) starts most close-up receivers inside the traced body (the bug);
// the NEW one starts none, at every distance, resolution, field of view and screen position below; the cap leaves
// the #1007 savings where a pixel is larger than the drift (far avatars at 1080p keep the metric threshold); an
// orthographic view, a missing projection or a disabled skip never skips.
#include "llcleanrigposeskip.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
struct V3 { double x, y, z; };
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
V3 nrm(V3 a) { return mul(a, 1.0 / len(a)); }

int g_fail = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

struct View { int w, h; double vfov_deg; const char* name; };

// The old rule (llcleanrtvk.cpp before this fix): pose_thr x max(1, d / 10 m).
float oldThreshold(float pose_thr, float d) { return pose_thr * std::max(1.f, d / kPoseRefMeters); }

// The view's px_scale (0.5 x height x |P11|) and corner cos^2 (from |P00|, |P11|), as llcleanrtvk.cpp passes them.
double pxScale(const View& v) { return 0.5 * v.h / std::tan(0.5 * v.vfov_deg * 3.14159265358979 / 180.0); }
float cornerCos2(const View& v)
{
    const double p11 = 1.0 / std::tan(0.5 * v.vfov_deg * 3.14159265358979 / 180.0), p00 = p11 * v.h / v.w;
    return cleanRigCornerCos2((float)p00, (float)p11);
}
float newThreshold(const View& v, float pose_thr, float d)
{
    return cleanRigPoseSkipThreshold(pose_thr, d, (float)pxScale(v), cornerCos2(v), false);
}

// Receivers of a sphere (radius rb) whose centre lies at distance D along the screen direction (sx, sy) in [-1, 1]
// (fractions of the half width / height); the camera at the origin looking down +z. Returns how many visible,
// on-screen receivers start their GI ray inside the traced sphere grown by drift, and how many were tested.
void countInside(const View& v, double D, double sx, double sy, double rb, double drift, int& inside, int& total)
{
    inside = total = 0;
    const double f = 0.5 * v.h / std::tan(0.5 * v.vfov_deg * 3.14159265358979 / 180.0);   // px_scale
    const V3 C = mul(nrm({sx * 0.5 * v.w / f, sy * 0.5 * v.h / f, 1.0}), D);
    const int N = 160;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
        {
            const double u = (i + 0.5) / N, w = (j + 0.5) / N;
            const double zc = 1.0 - 2.0 * u, r = std::sqrt(std::max(0.0, 1.0 - zc * zc)), ph = 6.28318530718 * w;
            const V3 n = {r * std::cos(ph), r * std::sin(ph), zc};
            const V3 P = add(C, mul(n, rb));
            const V3 V = mul(P, -1.0 / len(P));   // toward the eye
            if (dot(n, V) <= 0.0) continue;          // not visible from the camera
            if (!(P.z > 0.0)) continue;
            const double px = P.x / P.z * f, py = P.y / P.z * f;   // pixels from the centre
            if (std::fabs(px) > 0.5 * v.w || std::fabs(py) > 0.5 * v.h) continue;
            // the receiver pixel's angle (rtrPixelAngle: one pixel step along x of the view direction)
            const double pa = len(sub(nrm({px + 1.0, py, f}), nrm({px, py, f})));
            const double camd = len(P);
            const double lift = 0.5 * camd * pa / std::max(std::fabs(dot(n, V)), 0.25);
            const V3 o = add(P, mul(n, lift));
            ++total;
            if (len(sub(o, C)) < rb + drift) ++inside;
        }
}
} // namespace

int main()
{
    const float pose_thr = 0.002f;   // RenderVKRigRefitPoseThreshold's default
    const double rb = 0.15;          // the patch's radius (a belly)
    const View views[] = {{1920, 1080, 60.0, "1080p 60deg"}, {3840, 2160, 60.0, "4K 60deg"},
                          {3840, 2088, 60.0, "4K window 60deg"}, {3840, 2160, 20.0, "4K zoomed 20deg"},
                          {3840, 2160, 90.0, "4K wide 90deg"}, {7680, 4320, 60.0, "8K 60deg"}};
    const double dists[] = {0.25, 0.3, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 40.0};
    const double spots[][2] = {{0.0, 0.0}, {0.9, 0.0}, {0.0, 0.9}, {0.9, 0.9}, {-0.9, -0.9}};

    // 1) the bug: the old threshold, up close, starts most receivers inside the stale traced body
    {
        const View& v = views[2];
        const double D = 0.3 + rb;   // nearest point 0.3 m away
        int in = 0, tot = 0;
        countInside(v, D, 0.0, 0.0, rb, 0.999 * oldThreshold(pose_thr, (float)(D - rb)), in, tot);
        std::printf("old rule, %s, 0.3 m: %d of %d receivers start inside the traced body\n", v.name, in, tot);
        expect(tot > 1000 && in > tot * 9 / 10, "the old 2 mm threshold should start >90% of close-up receivers inside "
                                                "the stale BLAS (fixture sanity: the reported bug)");
        const float t = newThreshold(v, pose_thr, (float)(D - rb));
        expect(t < 0.1f * oldThreshold(pose_thr, (float)(D - rb)),
               "the new threshold up close should be far below the old 2 mm");
    }
    // 2) the fix: no receiver starts inside, anywhere
    int worst = 0;
    for (const View& v : views)
    {
        for (double nearest : dists)
            for (const auto& s : spots)
            {
                const double D = nearest + rb;
                const float t = newThreshold(v, pose_thr, (float)nearest);
                expect(t > 0.f, "the skip should stay available for a moving avatar in view");
                int in = 0, tot = 0;
                countInside(v, D, s[0], s[1], rb, 0.999 * t, in, tot);
                if (in > 0)
                {
                    std::printf("FAIL: %s, %.2f m, screen (%.1f, %.1f): %d of %d receivers inside (thr %.3g m)\n",
                                v.name, nearest, s[0], s[1], in, tot, t);
                    ++g_fail;
                }
                worst = std::max(worst, in);
            }
    }
    std::printf("new rule: worst receivers inside over %zu views x %zu distances x %zu screen spots: %d\n",
                sizeof(views) / sizeof(views[0]), sizeof(dists) / sizeof(dists[0]), sizeof(spots) / sizeof(spots[0]),
                worst);
    // 3) far avatars at 1080p keep the #1007 metric threshold (the savings where a pixel is larger than the drift)
    for (float d : {40.f, 80.f})
        expect(newThreshold(views[0], pose_thr, d) == oldThreshold(pose_thr, d),
               "a far avatar at 1080p 60deg should keep the metric threshold");
    std::printf("thresholds (mm): 0.3 m 4K %.4f (old %.1f), 10 m 4K %.3f, 40 m 1080p %.2f\n",
                1e3 * newThreshold(views[2], pose_thr, 0.3f), 1e3 * oldThreshold(pose_thr, 0.3f),
                1e3 * newThreshold(views[2], pose_thr, 10.f), 1e3 * newThreshold(views[0], pose_thr, 40.f));
    // 4) orthographic (no distance footprint for the lift to follow), off, unknown: never skip
    const float c2 = cornerCos2(views[1]);
    expect(cleanRigPoseSkipThreshold(pose_thr, 1.f, 2000.f, c2, true) == 0.f, "an orthographic view should never skip");
    expect(cleanRigPoseSkipThreshold(0.f, 1.f, 1000.f, c2, false) == 0.f, "threshold 0 should turn the skip off");
    expect(cleanRigPoseSkipThreshold(pose_thr, 1.f, 0.f, c2, false) == 0.f, "no projection should never skip");
    expect(cleanRigPoseSkipThreshold(pose_thr, 1.f, 1000.f, 0.f, false) == 0.f, "no corner angle should never skip");
    expect(cleanRigPoseSkipThreshold(pose_thr, 0.f, 1000.f, c2, false) == 0.f, "a camera inside the avatar should never skip");
    expect(cleanRigPoseSkipThreshold(pose_thr, NAN, 1000.f, c2, false) == 0.f, "a non-finite distance should never skip");
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
