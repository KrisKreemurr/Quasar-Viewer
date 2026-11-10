// shadowbench -- camera / sun scenarios (shared by the CPU schedule simulation and the GPU run).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

struct SbPose
{
    float eye[3];
    float at[3], left[3], up[3];
};

struct SbScenario
{
    const char* name;
    float seconds;        // run length
    float day_period;     // sun day-cycle period (s); 0 = fixed sun
    int   camera;         // 0 still, 1 walk, 2 turn, 3 fly
    bool  crowd_moves;    // avatars walk (GPU run)
    float sun0 = 0.62f;   // the sun's start on its circle (rad): 0.62 ~35 deg up, 1.40 ~ near noon
};

// Scenarios of the brief: still camera (fixed sun / 4 h day / 1 h day), walking, turning,
// flying (4 h day), and the crowd (still camera, avatars moving).
static const SbScenario kSbScenarios[] = {
    {"still",        20.f, 0.f,      0, false},
    {"still_sun4h",  20.f, 14400.f,  0, false},
    {"still_sun1h",  20.f, 3600.f,   0, false},
    {"walk",         20.f, 14400.f,  1, true},
    {"turn",         20.f, 14400.f,  2, true},
    {"fly",          20.f, 14400.f,  3, true},
    {"crowd",        20.f, 0.f,      0, true},
    {"noon_sun4h",   20.f, 14400.f,  0, false, 1.40f},   // the sun near the zenith, 4 h day
    {"walk_fixed",   20.f, 0.f,      1, true},           // (the camera's share of the diffs: fixed sun)
    {"turn_fixed",   20.f, 0.f,      2, true},
};
static const int kSbScenarioCount = (int)(sizeof(kSbScenarios) / sizeof(kSbScenarios[0]));

inline void sbNorm(float* v)
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// The terrain height of the synthetic region (metres): gentle hills around z = 20.
inline float sbGround(float x, float y)
{
    return 20.f + 3.f * std::sin(x * 0.021f) * std::cos(y * 0.017f) + 1.5f * std::sin((x + y) * 0.043f);
}

// Toward the light at time t (sun or moon). The sun runs on a circle tilted toward +y; at
// t = 0 it stands ~35 degrees up in the south-east.
inline void sbSun(const SbScenario& s, float t, float* L)
{
    const float phi = s.sun0 + (s.day_period > 0.f ? 6.2831853f * t / s.day_period : 0.f);
    L[0] = std::cos(phi); L[1] = 0.42f; L[2] = std::sin(phi);
    sbNorm(L);
}

inline void sbCamera(const SbScenario& s, float t, SbPose& p)
{
    float x = 96.f, y = 112.f, yaw = 0.35f, pitch = -0.12f, h = 1.8f;
    switch (s.camera)
    {
    case 1:   // walk: 1.4 m/s, the view swinging +-20 degrees
        yaw = 0.35f + 0.35f * std::sin(t * 0.785f);
        x += 1.4f * t * std::cos(0.35f); y += 1.4f * t * std::sin(0.35f);
        break;
    case 2:   // turn in place, 30 degrees / s
        yaw = 0.35f + 0.5236f * t;
        break;
    case 3:   // fly: 12 m/s, climbing 1 m/s, slow yaw
        yaw = 0.35f + 0.05f * t;
        x += 12.f * t * std::cos(0.35f); y += 12.f * t * std::sin(0.35f); h = 6.f + 1.f * t;
        pitch = -0.2f;
        break;
    default: break;
    }
    p.eye[0] = x; p.eye[1] = y; p.eye[2] = sbGround(x, y) + h;
    p.at[0] = std::cos(yaw) * std::cos(pitch); p.at[1] = std::sin(yaw) * std::cos(pitch); p.at[2] = std::sin(pitch);
    sbNorm(p.at);
    // left = up_world x at; up = at x left (LLCoordFrame convention: at x left = up)
    p.left[0] = -p.at[1]; p.left[1] = p.at[0]; p.left[2] = 0.f;
    sbNorm(p.left);
    p.up[0] = p.at[1] * p.left[2] - p.at[2] * p.left[1];
    p.up[1] = p.at[2] * p.left[0] - p.at[0] * p.left[2];
    p.up[2] = p.at[0] * p.left[1] - p.at[1] * p.left[0];
    sbNorm(p.up);
}
