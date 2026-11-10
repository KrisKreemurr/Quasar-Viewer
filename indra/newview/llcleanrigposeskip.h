/**
 * @file llcleanrigposeskip.h
 * @brief The #1007 rigged-refit POSE SKIP threshold: how far an avatar-local rigged BLAS may lag its raster pose.
 *
 * LLCleanRTVK skips an avatar's rig copy + BLAS refit while the summed pelvis-relative pose drift since its last
 * refit (joint translation + rotation x kPoseLever, measured on the GPU) stays under a threshold. While it is
 * skipped the traced skin sits up to that drift off the skin the raster draws this frame.
 *
 * Every ray that leaves a rigged receiver starts from the RASTER surface (rebuilt from depth), lifted off it by a
 * fraction of the pixel footprint: the GI fresh ray by 0.5 x camd x pixel angle / max(|Ng . V|, 0.25)
 * (clean_deferred_light.comp giFreshRay, "THE ORIGIN'S LIFT"). Where the stale BLAS lies OUTSIDE the drawn skin by
 * more than that lift, the ray starts inside the traced body: GI's back-face stop (RenderVKGIBackFaceStop) returns
 * no light there and the reuse's visibility rays from inside are blocked, so the GI on that skin collapses, and the
 * few texels whose origin lies outside the stale surface keep their (correct) GI and show as pale single-pixel
 * specks that change every frame. Worst up close: there the footprint (and the lift) is a fraction of a
 * millimetre, while the old threshold stayed at 2 mm everywhere within 10 m.
 *
 * THE RULE: the metric threshold (RenderVKRigRefitPoseThreshold m up to kPoseRefMeters, linear past it) is CAPPED
 * by the smallest GI origin lift the avatar can have on screen: kPoseLiftShare x 0.5 x its nearest point's
 * distance x the smallest pixel angle of the view. A pixel theta off axis subtends at least cos^2 theta of the
 * centre pixel's angle (1 / px_scale), so the smallest one is the corner's: cos^2 of the corner angle, from the
 * projection (tan^2 = 1 / P00^2 + 1 / P11^2). Near avatars then refit as their pose moves (every frame up close);
 * far ones keep the #1007 savings wherever a pixel is larger than the drift. An orthographic view (no distance
 * footprint for the GI lift to follow) never skips.
 *
 * Header-only and LL-free: shared by llcleanrtvk.cpp and the CPU fixture scripts/check/test_rig_pose_skip.cpp
 * (scripts/check/checks/rig_pose_skip.py).
 */
#ifndef LL_LLCLEANRIGPOSESKIP_H
#define LL_LLCLEANRIGPOSESKIP_H

#include <algorithm>
#include <cmath>

// The metric threshold holds up to this distance from the camera and grows linearly beyond (#1007).
constexpr float kPoseRefMeters = 10.f;
// The share of the smallest GI origin lift (0.5 x distance x the corner pixel's angle) the pose drift may reach.
constexpr float kPoseLiftShare = 0.9f;

// cos^2 of the view's corner angle from its projection's |P[0][0]| and |P[1][1]| (a symmetric perspective frustum);
// 0 when they are unusable.
inline float cleanRigCornerCos2(float p00, float p11)
{
    const float a = std::fabs(p00), b = std::fabs(p11);
    if (!(a > 0.f) || !(b > 0.f) || !std::isfinite(a) || !std::isfinite(b)) return 0.f;
    return 1.f / (1.f + 1.f / (a * a) + 1.f / (b * b));
}

// The pose drift (m) under which an avatar-local rigged group may skip its refit this frame; <= 0 = never skip.
//   pose_thr     RenderVKRigRefitPoseThreshold (m at <= kPoseRefMeters; <= 0: the skip is off)
//   av_dist      the avatar's nearest point from the main camera (m; 0 = the camera inside / touching it)
//   px_scale     the main view's pixels per metre at 1 m (0.5 x height x |P[1][1]|): 1 / the centre pixel's angle
//   corner_cos2  cleanRigCornerCos2 of the main view's projection
//   ortho        the main view is orthographic
inline float cleanRigPoseSkipThreshold(float pose_thr, float av_dist, float px_scale, float corner_cos2, bool ortho)
{
    if (!(pose_thr > 0.f) || ortho || !std::isfinite(av_dist) || !(av_dist > 0.f)) return 0.f;
    // (no projection known: never skip rather than lag by an unknown share of a pixel)
    if (!(px_scale > 0.f) || !std::isfinite(px_scale) || !(corner_cos2 > 0.f) || !(corner_cos2 <= 1.f)) return 0.f;
    const float metric = pose_thr * std::max(1.f, av_dist / kPoseRefMeters);
    const float lift = 0.5f * av_dist * corner_cos2 / px_scale;   // the smallest GI origin lift at the avatar
    return std::min(metric, kPoseLiftShare * lift);
}

#endif // LL_LLCLEANRIGPOSESKIP_H
