// Build (from indra/): g++ -std=c++20 -O2 -march=x86-64-v2 -I newview/vkgfx shadowbench/tests/sched_golden.cpp newview/vkgfx/llvkshadowcascades.cpp
// GOLDEN: setShadows' schedule as it was (9cc7f58542, llcleanrenderervk.cpp 22733-22948,
// verbatim; members as globals) vs vkShadowSchedule, over random frame sequences.
#include "llvkshadowcascades.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <cstdint>
typedef float F32; typedef uint32_t U32;
static const uint32_t NUM_CASCADES = 4;
struct ShadowCache { bool valid = false; uint32_t frame = 0; float mat[16] = {}; float eye[3] = {}; float texel = 0.f; uint32_t deferred = 0; };
struct ShadowOvlState { bool valid = false; uint32_t frame = 0; uint32_t src = 0; };
struct WFs { float shadow_mat[4][16]; float shadow_slice[4][4]; float shadow_texel[4]; };
namespace LLVKHitch { inline void count(const char*, uint32_t) {} }
std::vector<float> gVKChangedSpheres; std::vector<uint8_t> gVKChangedSphereTags; bool gVKChangedOverflow = false;
static void clipPlanes(const float* M, float out[6][4])
{
    float rw[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) rw[r][c] = M[c * 4 + r];
    const int sgn[6][2] = {{0,1},{0,-1},{1,1},{1,-1},{2,0},{2,-1}};
    for (int i = 0; i < 6; ++i)
    {
        float pl[4];
        for (int c = 0; c < 4; ++c)
            pl[c] = (i == 4) ? rw[2][c] : rw[3][c] + (float)sgn[i][1] * rw[sgn[i][0]][c];
        const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        for (int c = 0; c < 4; ++c) out[i][c] = len > 0.f ? pl[c] / len : pl[c];
    }
}
static bool sphereInPlanes(const float planes[6][4], const float* c, float r)
{
    for (int i = 0; i < 6; ++i) { const float* p = planes[i]; if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r) return false; }
    return true;
}
static void vkRebaseOrigin(const float* M, const float* from, const float* to, float* out) { vkShadowRebaseOrigin(M, from, to, out); }
struct Orig {
    ShadowCache mShadowCache[4]; ShadowOvlState mShadowOvl[4]; bool mShadowRender[4] = {}; bool mShadowOvlRefresh[4] = {};
    float mShadowMat[4][16] = {}; float mShadowTexel[4] = {}; float mShadowEye[3] = {}; uint32_t mShadowFrame = 0;
    bool mShadowOverlay = true; bool mPrbActive = false; uint32_t mPrbFace = 0; bool mShadowDeferredNow = false;
    void run(const WFs& wf, uint32_t mode, bool event_cache, float light_tx, uint32_t safety)
    {
        static const uint32_t kInterval[3][NUM_CASCADES] = { {1, 1, 1, 1}, {1, 2, 4, 8}, {1, 4, 16, 32} };
        std::memcpy(mShadowMat, wf.shadow_mat, sizeof(mShadowMat));
        std::memcpy(mShadowTexel, wf.shadow_texel, sizeof(mShadowTexel));
    uint32_t n_render = 0, c_ev_slice = 0, c_ev_light = 0, c_ev_change = 0, c_ev_safety = 0;
    // shadowEvChange by sphere producer (gVKChangedSphereTags, as the projector tiles'
    // projDirtBy*): 0 changed queue, 1 moved, 2 seat, 3 vegetation, 4 glTF, 5 mover class
    // start / settle, 6 instance store registration (new / released / re-shaped -- LOD
    // rebuilds), 7 untagged, 8 overflow.
    uint32_t c_ev_by[9] = {};
    const size_t n_sph = gVKChangedSpheres.size() / 4;
    // #825 EVEN-OUT: what each cascade's re-render was for. FORCED (the cache is invalid, the
    // light jumped, the view slice outgrew the map, a non-event mode's interval) renders this
    // frame; DEFERRABLE (a static change in the map, the light's slow drift, the safety net, an
    // earlier deferral coming due) may wait a frame -- see the budget below.
    bool want[NUM_CASCADES] = {}, forced[NUM_CASCADES] = {};
    float cached_mat[NUM_CASCADES][16] = {};
    for (uint32_t c = 0; c < NUM_CASCADES; ++c)
    {
        ShadowCache& sc = mShadowCache[c];
        bool render = event_cache
            ? (!sc.valid || (mShadowFrame - sc.frame) >= safety + c * 37u)   // (staggered)
            : (mode == 0 || c == 0 || !sc.valid || (mShadowFrame - sc.frame) >= kInterval[mode][c]);
        bool force = !sc.valid;
        if (!sc.valid) sc.deferred = 0;
        if (event_cache && sc.valid && render) ++c_ev_safety;
        // The cached map's matrix re-expressed relative to THIS frame's eye (#555): the
        // map's world placement is unchanged, only the origin the GPU subtracts moved.
        float* cached = cached_mat[c];
        if (sc.valid) vkRebaseOrigin(sc.mat, sc.eye, mShadowEye, cached);
        // #825: the light-jump and coverage tests run even when the safety net / interval
        // already asked for a render -- they make it FORCED (never deferred below).
        if (sc.valid)
        {
            // Light jump: compare the maps' depth axes (row 2 of both matrices).
            const float* A = sc.mat; const float* B = wf.shadow_mat[c];
            const float la = std::sqrt(A[2]*A[2] + A[6]*A[6] + A[10]*A[10]);
            const float lb = std::sqrt(B[2]*B[2] + B[6]*B[6] + B[10]*B[10]);
            const float cosang = (la > 0.f && lb > 0.f) ? (A[2]*B[2] + A[6]*B[6] + A[10]*B[10]) / (la * lb) : -1.f;
            if (cosang < 0.99939f) { render = true; force = true; }   // ~2 degrees
            if (!render && event_cache && sc.texel > 0.f)
            {
                // #567: the slow day-cycle drift, measured where it shows: a point at the
                // map's far reach moves angle x extent (extent = the larger of the depth
                // range 1/|row2| and the width 2/|row0|); re-render past light_tx texels.
                // Roll of the map's basis (row 0) counts the same way.
                // (atan2 of |a x b| and a.b in double: acos of a float dot near 1 has a
                // ~3e-4 rad noise floor, a texel at the far cascade's reach)
                auto rowAngle = [](const float* P, const float* Q, int r) {
                    const double ax = P[r], ay = P[4 + r], az = P[8 + r], bx = Q[r], by = Q[4 + r], bz = Q[8 + r];
                    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
                };
                const float ra = std::sqrt(A[0]*A[0] + A[4]*A[4] + A[8]*A[8]);
                const double ang = std::max(rowAngle(A, B, 2), rowAngle(A, B, 0));
                const double extent = std::max(la > 0.f ? 1.0 / la : 0.0, ra > 0.f ? 2.0 / ra : 0.0);
                if (ang * extent > (double)(light_tx * sc.texel)) { render = true; ++c_ev_light; }
            }
        }
        if (sc.valid && !force)
        {
            // Coverage: this frame's slice sphere, projected with the CACHED matrix,
            // must lie inside the cached map (xy extent +-1, depth 0..1). The sphere
            // centre is agent space; the matrix maps (P - eye) (#555).
            const float* M = cached;
            const float S[4] = {(float)((double)wf.shadow_slice[c][0] - mShadowEye[0]),
                                (float)((double)wf.shadow_slice[c][1] - mShadowEye[1]),
                                (float)((double)wf.shadow_slice[c][2] - mShadowEye[2]),
                                wf.shadow_slice[c][3]};
            const float x = M[0]*S[0] + M[4]*S[1] + M[8]*S[2]  + M[12];
            const float y = M[1]*S[0] + M[5]*S[1] + M[9]*S[2]  + M[13];
            const float z = M[2]*S[0] + M[6]*S[1] + M[10]*S[2] + M[14];
            const float sxy = std::sqrt(M[0]*M[0] + M[4]*M[4] + M[8]*M[8]);   // 1 / R
            const float sz  = std::sqrt(M[2]*M[2] + M[6]*M[6] + M[10]*M[10]); // 1 / depth range
            const float rxy = S[3] * sxy, rz = S[3] * sz;
            if (std::fabs(x) + rxy > 1.f || std::fabs(y) + rxy > 1.f || z - rz < 0.f || z + rz > 1.f)
            {
                if (event_cache && !render) ++c_ev_slice;
                render = true;
                force = true;   // (the map no longer covers the view: wrong shadows now)
            }
        }
        if (!render && event_cache && (gVKChangedOverflow || n_sph))
        {
            // #567: a static change inside the cached map's whole volume (every depth: a
            // caster anywhere along the light inside it), spheres agent space -> this eye.
            bool hit = gVKChangedOverflow;
            uint32_t by = 8u;   // the producer of the first sphere hit (8 = overflow)
            if (!hit)
            {
                float planes[6][4];
                clipPlanes(cached, planes);
                for (size_t i = 0; i < n_sph && !hit; ++i)
                {
                    const float* s = &gVKChangedSpheres[i * 4];
                    const float rel[3] = { (float)((double)s[0] - mShadowEye[0]), (float)((double)s[1] - mShadowEye[1]),
                                           (float)((double)s[2] - mShadowEye[2]) };
                    hit = sphereInPlanes(planes, rel, s[3]);
                    if (hit) by = i < gVKChangedSphereTags.size() ? std::min<uint32_t>(gVKChangedSphereTags[i], 7u) : 7u;
                }
            }
            if (hit) { render = true; ++c_ev_change; ++c_ev_by[by]; }
        }
        // An earlier deferral (its change spheres are gone: the latch carries it) comes due.
        if (event_cache && sc.deferred) render = true;
        want[c] = render;
        forced[c] = render && (force || !event_cache);
    }
    // #825 FAR-CASCADE BUDGET (event-driven cache, RenderVKShadowDeferFrames N > 0): a far
    // cascade (2 / 3: the expensive maps; their nested volumes take the same distant change
    // together) re-renders for a DEFERRABLE reason at most once per frame -- none when a
    // forced far re-render or the scheduled probe's face 5 + cube filter (mPrbFace 5, one
    // frame in six) already loads the frame. A deferred cascade keeps its cached map (a
    // consistent matrix + content, at most N frames behind the change) and LATCHES the
    // re-render (ShadowCache::deferred): it renders once its wait reaches N whatever else is
    // due, longest-waiting first otherwise. Cascades 0 / 1 (near, cheap, the most visible)
    // and forced reasons are never deferred.
    uint32_t n_deferred = 0;
    {
        uint32_t defer_max = 2;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_defer(gSavedSettings, "RenderVKShadowDeferFrames", 2);
        defer_max = std::min<uint32_t>((U32)s_defer, 8u);
#endif
        if (!event_cache || defer_max == 0u)
        {
            for (ShadowCache& sc : mShadowCache) sc.deferred = 0;
        }
        else
        {
            bool far_forced = false;
            for (uint32_t c = 2; c < NUM_CASCADES; ++c) far_forced = far_forced || forced[c];
            uint32_t budget = (far_forced || (mPrbActive && mPrbFace == 5u)) ? 0u : 1u;
            uint32_t order[NUM_CASCADES];
            uint32_t n_cand = 0;
            for (uint32_t c = 2; c < NUM_CASCADES; ++c)
                if (want[c] && !forced[c]) order[n_cand++] = c;
            std::stable_sort(order, order + n_cand, [&](uint32_t x, uint32_t y)
                             { return mShadowCache[x].deferred > mShadowCache[y].deferred; });
            for (uint32_t i = 0; i < n_cand; ++i)
            {
                ShadowCache& sc = mShadowCache[order[i]];
                if (sc.deferred >= defer_max) continue;      // waited long enough: renders
                if (budget) { --budget; continue; }
                want[order[i]] = false;                      // waits a frame
                ++sc.deferred;
                ++n_deferred;
            }
        }
    }
    for (uint32_t c = 0; c < NUM_CASCADES; ++c)
    {
        ShadowCache& sc = mShadowCache[c];
        const bool render = want[c];
        mShadowRender[c] = render;
        if (render)
        {
            sc.valid = true; sc.frame = mShadowFrame;
            sc.deferred = 0;
            std::memcpy(sc.mat, wf.shadow_mat[c], sizeof(sc.mat));
            std::memcpy(sc.eye, mShadowEye, sizeof(sc.eye));
            sc.texel = wf.shadow_texel[c];
            ++n_render;
        }
        else
        {
            std::memcpy(mShadowMat[c], cached_mat[c], sizeof(cached_mat[c]));
            mShadowTexel[c] = sc.texel;
        }
    }
    LLVKHitch::count("shadowEvDeferred", n_deferred);   // #825 far cascades held a frame
    mShadowDeferredNow = n_deferred > 0u;   // (FRAMEJOB bit 16, set once recordWorld opened the frame's slot)
    LLVKHitch::count("shadowCascadesPlanned", n_render);
    // STAGGERED FAR-CASCADE OVERLAY (RenderVKShadowFarCascadeInterval N, overlay modes): the
    // dynamic overlay of cascades 2 and 3 (static copy + dynamic / rigged / particle casters)
    // runs every N frames, the two alternating so the per-frame cost stays even (#825: by
    // age, below); a skipped cascade's sampled layer keeps the last overlay, built on the
    // SAME cached map (its matrix is the cached one rebased onto this eye: the same world
    // placement), so only the dynamic casters lag, by at most N - 1 frames (N after a far
    // re-render shifted the alternation), in the far cascades. Forced
    // whenever the cascade re-renders (new matrix / static content), its last overlay is
    // gone (invalidateShadowCache: origin shift, toggles, map re-allocation, a ray-traced
    // sun frame) or was built on another render of the cache. Cascades 0 / 1 every frame.
    {
        uint32_t far_n = 2;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_far(gSavedSettings, "RenderVKShadowFarCascadeInterval", 2);
        far_n = std::clamp<uint32_t>((U32)s_far, 1u, 16u);
#endif
        // #825: the periodic refresh is by AGE, at most one far cascade per frame (the oldest
        // once it is N frames old) and none in a frame that already redraws a far overlay for
        // a forced reason (a re-render: new static content) -- a cascade re-render no longer
        // stacks with the other far cascade's scheduled overlay; the alternation shifts by a
        // frame instead (lag bound N, hard limit N + 1). Was a fixed (frame + c) parity.
        uint32_t n_skip = 0;
        bool force_ovl[NUM_CASCADES] = {};
        bool far_forced = false;
        for (uint32_t c = 0; c < NUM_CASCADES; ++c)
        {
            const ShadowOvlState& o = mShadowOvl[c];
            force_ovl[c] = c < 2u || far_n <= 1u || mShadowRender[c] || !o.valid
                        || o.src != mShadowCache[c].frame || (mShadowFrame - o.frame) > far_n;
            if (c >= 2u && force_ovl[c] && mShadowCache[c].valid) far_forced = true;
        }
        int periodic = -1;
        if (!far_forced)
            for (uint32_t c = 2; c < NUM_CASCADES; ++c)
            {
                const uint32_t age = mShadowFrame - mShadowOvl[c].frame;
                if (!force_ovl[c] && age >= far_n
                    && (periodic < 0 || age > mShadowFrame - mShadowOvl[periodic].frame)) periodic = (int)c;
            }
        for (uint32_t c = 0; c < NUM_CASCADES; ++c)
        {
            const bool refresh = force_ovl[c] || (int)c == periodic;
            mShadowOvlRefresh[c] = refresh;
            if (!refresh && mShadowOverlay && mShadowCache[c].valid) ++n_skip;
        }
        LLVKHitch::count("shadowOvlSkipped", n_skip);   // far cascades reusing their last overlay
    }

    }
};
int main()
{
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    long bad = 0, frames = 0;
    for (int seq = 0; seq < 400; ++seq)
    {
        Orig o; o.mShadowOverlay = (seq % 5) != 0;
        VKShadowCacheEntry cache[4]; VKShadowOvlEntry ovl[4];
        const uint32_t mode = seq % 3; const bool ev = (seq % 4) != 3;
        const float ltx = (seq % 2) ? 0.5f : 2.f; const uint32_t safety = (seq % 7 == 0) ? 5u : 600u;
        float lx = 0.3f * u(rng), ly = 0.3f * u(rng);
        float ex = 100.f * u(rng), ey = 100.f * u(rng);
        for (int f = 0; f < 300; ++f)
        {
            ++o.mShadowFrame;
            o.mPrbActive = (f % 6) == 5; o.mPrbFace = 5;
            lx += 2e-4f * (seq % 3); ly += (f % 97 == 0) ? 0.05f : 0.f;
            ex += 0.3f * u(rng); ey += 0.3f * u(rng);
            const float eye[3] = {ex, ey, 30.f};
            o.mShadowEye[0] = eye[0]; o.mShadowEye[1] = eye[1]; o.mShadowEye[2] = eye[2];
            VKShadowFitIn fi; float L[3] = {lx, ly, 1.f}; float n = std::sqrt(lx*lx+ly*ly+1.f);
            for (int i = 0; i < 3; ++i) { fi.light[i] = L[i] / n; fi.eye[i] = eye[i]; }
            const float yaw = 0.01f * f * (seq % 2 ? 1.f : 0.f);
            fi.at[0] = std::cos(yaw); fi.at[1] = std::sin(yaw); fi.at[2] = 0.f;
            fi.left[0] = -std::sin(yaw); fi.left[1] = std::cos(yaw); fi.left[2] = 0.f;
            if (mode) fi.scale[1] = fi.scale[2] = fi.scale[3] = 1.15f;
            VKShadowFitOut fo; vkShadowFitCascades(fi, fo);
            WFs wf; std::memcpy(wf.shadow_mat, fo.mat, sizeof(fo.mat)); std::memcpy(wf.shadow_slice, fo.slice, sizeof(fo.slice));
            std::memcpy(wf.shadow_texel, fo.texel, sizeof(fo.texel));
            gVKChangedSpheres.clear(); gVKChangedSphereTags.clear(); gVKChangedOverflow = (f % 151) == 150;
            if (f % 13 == 0) { gVKChangedSpheres = {ex + 200.f * u(rng), ey + 200.f * u(rng), 25.f, 5.f}; gVKChangedSphereTags = {(uint8_t)(f % 8)}; }
            o.run(wf, mode, ev, ltx, safety);
            VKShadowSchedIn si; si.frame = o.mShadowFrame; si.mode = mode; si.event_cache = ev; si.overlay = o.mShadowOverlay;
            si.light_tx = ltx; si.drift_roll = true; si.safety = safety; si.defer_max = 2; si.far_n = 2; si.probe_face5 = o.mPrbActive && o.mPrbFace == 5u;
            si.mat = fo.mat; si.slice = fo.slice; si.texel = fo.texel; std::memcpy(si.eye, eye, 12);
            si.spheres = gVKChangedSpheres.data(); si.n_spheres = gVKChangedSpheres.size() / 4;
            si.sphere_tags = gVKChangedSphereTags.data(); si.n_tags = gVKChangedSphereTags.size(); si.change_overflow = gVKChangedOverflow;
            VKShadowSchedOut so; vkShadowSchedule(cache, ovl, si, so);
            bool same = true;
            for (int c = 0; c < 4; ++c)
            {
                same = same && so.render[c] == o.mShadowRender[c] && so.ovl_refresh[c] == o.mShadowOvlRefresh[c]
                    && !std::memcmp(so.mat[c], o.mShadowMat[c], 64) && so.texel[c] == o.mShadowTexel[c]
                    && cache[c].valid == o.mShadowCache[c].valid && cache[c].frame == o.mShadowCache[c].frame
                    && cache[c].deferred == o.mShadowCache[c].deferred;
            }
            ++frames; if (!same) { if (bad < 5) std::printf("MISMATCH seq %d frame %d\n", seq, f); ++bad; }
            // the recorder's overlay bookkeeping (both sides the same)
            for (int c = 0; c < 4; ++c)
                if (o.mShadowOverlay && o.mShadowCache[c].valid && o.mShadowOvlRefresh[c])
                {
                    o.mShadowOvl[c].valid = true; o.mShadowOvl[c].frame = o.mShadowFrame; o.mShadowOvl[c].src = o.mShadowCache[c].frame;
                    ovl[c].valid = true; ovl[c].frame = si.frame; ovl[c].src = cache[c].frame;
                }
        }
    }
    std::printf("schedule golden: %ld / %ld frames identical\n", frames - bad, frames);
    return bad ? 1 : 0;
}
