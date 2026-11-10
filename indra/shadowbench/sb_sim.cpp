// shadowbench sim -- the viewer's cascade fit + cache schedule (vkgfx/llvkshadowcascades.cpp)
// driven by the scenarios, CPU only: how often each cascade re-renders under each policy.
//   shadowbench_sim [fps]
#include "llvkshadowcascades.h"
#include "sb_scenario.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct SbPolicy
{
    const char* name;
    uint32_t mode;           // 0 realtime, 1 cached (event cache + overlay)
    float    scale0;         // cascade 0 fit scale
    float    scale_n;        // cascades 1-3 fit scale (cached)
    bool     roll;           // drift test counts the basis roll
    int      lever;          // 0 extent, 1 2R (the slice sphere's chord), 2 2R + 64 m
    float    light_tx;
};

static const SbPolicy kPolicies[] = {
    {"realtime",          0, 1.f,   1.f,   true,  0, 0.5f},
    {"cached (current)",  1, 1.f,   1.15f, true,  0, 0.5f},
    {"cached no-roll",    1, 1.f,   1.15f, false, 0, 0.5f},
    {"no-roll lever 2R",  1, 1.f,   1.15f, false, 1, 0.5f},
    {"no-roll lever 2R+64", 1, 1.f, 1.15f, false, 2, 0.5f},
    {"+ c0 slack 6%",     1, 1.06f, 1.15f, false, 2, 0.5f},
};

int main(int argc, char** argv)
{
    const float fps = argc > 1 ? (float)std::atof(argv[1]) : 60.f;
    std::printf("cascade re-renders per second (c0 c1 c2 c3 | deferred/s), %.0f fps\n", fps);
    for (int si = 0; si < kSbScenarioCount; ++si)
    {
        const SbScenario& sc = kSbScenarios[si];
        std::printf("\n== %s ==\n", sc.name);
        for (const SbPolicy& pol : kPolicies)
        {
            VKShadowCacheEntry cache[VKSC_CASCADES];
            VKShadowOvlEntry ovl[VKSC_CASCADES];
            uint32_t renders[VKSC_CASCADES] = {}, deferred = 0, light = 0, slice = 0;
            const int frames = (int)(sc.seconds * fps);
            for (int f = 0; f < frames; ++f)
            {
                const float t = (float)f / fps;
                SbPose p; sbCamera(sc, t, p);
                VKShadowFitIn fi;
                std::memcpy(fi.eye, p.eye, 12); std::memcpy(fi.at, p.at, 12);
                std::memcpy(fi.left, p.left, 12); std::memcpy(fi.up, p.up, 12);
                sbSun(sc, t, fi.light);
                fi.fov_y = 1.0472f; fi.aspect = 16.f / 9.f; fi.near_clip = 0.1f;
                fi.range = 256.f; fi.render_far = 256.f;   // default draw distance 256 m
                fi.res = 2048;
                if (pol.mode) { fi.scale[0] = pol.scale0; fi.scale[1] = fi.scale[2] = fi.scale[3] = pol.scale_n; }
                VKShadowFitOut fo; vkShadowFitCascades(fi, fo);
                VKShadowSchedIn in;
                in.frame = (uint32_t)f + 1u;
                in.mode = pol.mode; in.event_cache = pol.mode != 0; in.overlay = pol.mode != 0;
                in.light_tx = pol.light_tx; in.drift_roll = pol.roll;
                for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
                {
                    const float R = 1.f / std::sqrt(fo.mat[c][0] * fo.mat[c][0] + fo.mat[c][4] * fo.mat[c][4] + fo.mat[c][8] * fo.mat[c][8]);
                    in.lever[c] = pol.lever == 1 ? 2.f * R : (pol.lever == 2 ? 2.f * R + 64.f : 0.f);
                }
                in.mat = fo.mat; in.slice = fo.slice; in.texel = fo.texel;
                std::memcpy(in.eye, p.eye, 12);
                VKShadowSchedOut so; vkShadowSchedule(cache, ovl, in, so);
                if (f >= (int)fps)   // (after the first second: the cold start renders everything)
                {
                    for (uint32_t c = 0; c < VKSC_CASCADES; ++c) renders[c] += so.render[c] ? 1u : 0u;
                    deferred += so.n_deferred; light += so.ev_light; slice += so.ev_slice;
                }
                for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
                    if (in.overlay && cache[c].valid && so.ovl_refresh[c])
                    { ovl[c].valid = true; ovl[c].frame = in.frame; ovl[c].src = cache[c].frame; }
            }
            const float secs = sc.seconds - 1.f;
            std::printf("  %-22s %5.1f %5.1f %5.1f %5.1f | def %4.1f  (light %5.1f slice %5.1f)/s\n", pol.name,
                        renders[0] / secs, renders[1] / secs, renders[2] / secs, renders[3] / secs, deferred / secs,
                        light / secs, slice / secs);
        }
    }
    return 0;
}
