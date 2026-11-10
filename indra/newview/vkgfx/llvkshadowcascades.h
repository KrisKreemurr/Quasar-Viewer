/**
 * @file llvkshadowcascades.h
 * @brief Sun / moon shadow cascades: the per-frame fit and the cache schedule, as pure
 *        functions (no viewer state), shared by the viewer and indra/shadowbench.
 */
#pragma once

// The viewer's sun cascades in two steps, both free of viewer state so the standalone
// shadow harness (indra/shadowbench) runs the very code the viewer runs:
//  * vkShadowFitCascades -- the per-frame fit (LLAppViewer's LLVKWorldFrame build): splits,
//    bounding-sphere ortho boxes snapped to whole texels, cull planes, caster planes.
//  * vkShadowSchedule -- which cascades re-render this frame and which are sampled from
//    their cache (LLCleanRendererVK::setShadows: the event-driven cache, the far-cascade
//    deferral budget, the staggered far overlay).
// No LL headers: plain floats in, plain floats out.

#include <cstddef>
#include <cstdint>

constexpr uint32_t VKSC_CASCADES = 4;

// ---- the fit ---------------------------------------------------------------------------

struct VKShadowFitIn
{
    float eye[3]  = {};          // camera position (agent space)
    float at[3]   = {1, 0, 0};   // camera forward / left / up (unit)
    float left[3] = {0, 1, 0};
    float up[3]   = {0, 0, 1};
    float light[3] = {0, 0, 1};  // toward the light (unit)
    float fov_y = 1.f;           // vertical field of view (radians)
    float aspect = 1.f;
    float near_clip = 0.1f;      // camera near plane (m)
    float range = 512.f;         // the cascades' far end: min(draw distance, 512) x the VRAM range scale
    float render_far = 512.f;    // draw distance (the pull-back toward the light)
    float split_exp[3] = {3.f, 3.f, 2.f};   // RenderShadowSplitExponent
    uint32_t res = 2048;         // the shadow map resolution (without the border)
    bool  ortho = false;         // orthographic camera (its half width / height below)
    float ortho_hw = 0.f, ortho_hh = 0.f;
    // Per-cascade fit scale (the cache's slack: cached cascades are fitted larger so the map
    // stays valid while the camera moves until the coverage test says re-render).
    float scale[VKSC_CASCADES] = {1.f, 1.f, 1.f, 1.f};
    // BORDER (cached cascades): the map extends this many texels beyond the fitted box on
    // every side, at the fitted box's texel size and snap grid -- the slack a cached map needs
    // to stay valid while the camera moves, without the coarser texels a larger box (scale)
    // costs: the map is (res + 2 x border)^2 and samples exactly as an unbordered one.
    uint32_t border = 0;
};

struct VKShadowFitOut
{
    float mat[VKSC_CASCADES][16];              // (world - eye) -> shadow NDC, column-major (#555)
    float planes[VKSC_CASCADES][6][4];         // world-space cull planes of each box (L R B T near far)
    float clip[VKSC_CASCADES];                 // far split (view depth)
    float texel[VKSC_CASCADES];                // world texel size
    float slice[VKSC_CASCADES][4];             // the view slice's bounding sphere (agent xyz, r)
    float caster_planes[VKSC_CASCADES][6][4];  // planes a caster must be inside ((0,0,0,1) = unused)
    // SWEPT CASTER VOLUME (exact): the view slice swept toward the light to infinity is
    // bounded by the caster planes above (the slice faces a shadow cannot cross) and, through
    // every SILHOUETTE edge of the slice (a kept face meeting a dropped one), the plane holding
    // the edge and the light direction -- at most 6 (the outline of a projected hexahedron).
    // A caster outside any of them cannot shadow any point of the slice. Unused = (0,0,0,1).
    float sil_planes[VKSC_CASCADES][6][4];
    uint32_t n_sil[VKSC_CASCADES];
};

void vkShadowFitCascades(const VKShadowFitIn& in, VKShadowFitOut& out);

// ---- the cache schedule ----------------------------------------------------------------

struct VKShadowCacheEntry
{
    bool     valid = false;
    uint32_t frame = 0;          // schedule frame when rendered
    float    mat[16] = {};       // relative to eye[] (#555)
    float    eye[3] = {};        // the eye when rendered
    float    texel = 0.f;
    uint32_t deferred = 0;       // #825: frames a due re-render has waited (0 = none latched)
};

struct VKShadowOvlEntry
{
    bool     valid = false;
    uint32_t frame = 0;          // schedule frame of the last overlay
    uint32_t src = 0;            // VKShadowCacheEntry::frame it was overlaid on
};

struct VKShadowSchedIn
{
    uint32_t frame = 0;          // this frame's schedule counter (incremented by the caller)
    uint32_t mode = 0;           // 0 realtime, 1 balanced, 2 performance (RenderVKShadowUpdate)
    bool     event_cache = true; // event-driven re-render (needs the static / dynamic split)
    bool     overlay = false;    // the dynamic-caster overlay is on (static layers exist)
    float    light_tx = 0.5f;    // light drift threshold in texels
    // LIGHT-DRIFT LEVER (shadowbench experiments): the distance a light turn is multiplied by
    // to get the shadow shift it causes. 0 (the viewer) = the map's extent: the whole
    // pulled-back depth range, the strict bound (a caster anywhere in the box, a receiver at
    // its far end); > 0 = a shorter assumed caster-to-receiver distance (capped at the extent).
    float    lever[VKSC_CASCADES] = {};
    // Count the map basis' ROLL (row 0) as drift too (the old rule; shadowbench's replica of
    // it). A cached map is sampled with the matrix it was rendered with, so a roll of its
    // basis moves no shadow -- only the light direction (row 2) does. The roll ran up to ~10x
    // the light's own turn with the sun near the zenith (rt = z x fw), re-rendering for nothing.
    bool     drift_roll = false;
    uint32_t safety = 600;       // safety-net re-render period (frames)
    uint32_t defer_max = 2;      // #825 far-cascade deferral (frames; 0 = off)
    uint32_t far_n = 2;          // staggered far overlay period (frames)
    bool     probe_face5 = false;   // the scheduled probe's heavy face this frame (budget 0)
    const float (*mat)[16] = nullptr;      // this frame's fit (VKShadowFitOut::mat)
    const float (*slice)[4] = nullptr;     // this frame's slice spheres
    const float* texel = nullptr;          // this frame's texels
    float    eye[3] = {};                  // this frame's eye
    // Static changes this frame (agent-space spheres xyzr, producer tags 0..7), or overflow.
    const float*   spheres = nullptr;
    size_t         n_spheres = 0;
    const uint8_t* sphere_tags = nullptr;
    size_t         n_tags = 0;
    bool           change_overflow = false;
};

struct VKShadowSchedOut
{
    bool     render[VKSC_CASCADES] = {};       // re-render cascade c this frame
    bool     ovl_refresh[VKSC_CASCADES] = {};  // overlay cascade c this frame (cache valid + overlay on)
    float    mat[VKSC_CASCADES][16] = {};      // the matrix cascade c is sampled with this frame
    float    texel[VKSC_CASCADES] = {};
    // why (hitch counters): see setShadows
    uint32_t n_render = 0, ev_slice = 0, ev_light = 0, ev_change = 0, ev_safety = 0;
    uint32_t ev_by[9] = {};
    uint32_t n_deferred = 0, n_ovl_skip = 0;
};

// Updates cache[] (rendered cascades take this frame's fit) and fills out. ovl[] is read
// only (the recorder updates it when it actually overlays a cascade).
void vkShadowSchedule(VKShadowCacheEntry cache[VKSC_CASCADES], const VKShadowOvlEntry ovl[VKSC_CASCADES],
                      const VKShadowSchedIn& in, VKShadowSchedOut& out);

// M maps (P - from); writes the same transform for (P - to) (#555; in double).
void vkShadowRebaseOrigin(const float* M, const float* from, const float* to, float* out);
