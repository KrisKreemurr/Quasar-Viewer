/**
 * @file slbenchpath.h
 * @brief Render benchmark camera path: keyframes, JSON, smooth playback (#1304).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SL_BENCHPATH_H
#define SL_BENCHPATH_H

#include "llquaternion.h"
#include "stdtypes.h"
#include "v3math.h"

#include <string>
#include <vector>

// CAMERA PATH (doc/bench.md). One JSON object, agent coordinates (the capture's frame in a
// replay, the agent region's in a live session):
//
//   { "format": "slipstream-bench-path", "version": 1,
//     "keys": [ { "t": 0.0, "pos": [x, y, z], "rot": [x, y, z, w], "fov": 1.047,
//                 "segment": "slow pan", "still": 32 }, ... ] }
//
// rot is the camera frame's quaternion (LLCoordFrame: at = +X, left = +Y, up = +Z); a key
// may give "focus": [x, y, z] instead (look-at point, world up +Z) for hand-written paths.
// fov = vertical field of view in radians. "segment" names the stretch from this key up to
// the next key that names another one; keys without one continue the previous segment (the
// first defaults to "default"). Reports break their timings down per segment.
// "still": K = on reaching this key the camera holds for K frames for the noise measurement
// (path time stands; those frames are not timed).
//
// Playback: position = cubic Hermite with Catmull-Rom tangents over the (non-uniform) key
// times, rotation = squad (Shoemake), fov = linear. C1 positions, smooth rotations.
class SLBenchPath
{
public:
    static constexpr F32 kDefaultFov = 1.0471976f;   // 60 degrees

    struct Key
    {
        F64 t = 0.0;
        LLVector3 pos;
        LLQuaternion rot;
        F32 fov = kDefaultFov;
        std::string segment;   // filled in on append / parse (inherited)
        U32 still = 0;         // hold frames at this key (0 = none)
    };
    struct Pose
    {
        LLVector3 pos;
        LLQuaternion rot;
        F32 fov = kDefaultFov;
    };

    bool load(const std::string& file, std::string& err);
    bool save(const std::string& file, std::string& err) const;
    bool parse(const std::string& json, std::string& err);
    std::string serialize() const;

    bool empty() const { return mKeys.empty(); }
    size_t size() const { return mKeys.size(); }
    F64 duration() const { return mKeys.empty() ? 0.0 : mKeys.back().t; }
    const std::vector<Key>& keys() const { return mKeys; }
    void clear() { mKeys.clear(); }
    // Append a key; t must not go backwards (a key at an earlier time is refused: false).
    // An empty segment continues the previous key's.
    bool append(Key k);

    // Pose at time t, clamped to [0, duration()].
    Pose sample(F64 t) const;
    // Index of the last key with key.t <= t (0 before the first key).
    size_t keyAt(F64 t) const;
    // The segment the stretch at time t belongs to ("" when empty).
    const std::string& segmentAt(F64 t) const;
    // Segment names in order of first appearance.
    std::vector<std::string> segmentNames() const;

    // Camera frame quaternion looking from pos at focus (world up +Z).
    static LLQuaternion lookRotation(const LLVector3& pos, const LLVector3& focus);

private:
    void fixHemispheres();   // neighbouring keys on the same quaternion hemisphere (shortest arcs)
    std::vector<Key> mKeys;  // sorted by t
};

#endif // SL_BENCHPATH_H
