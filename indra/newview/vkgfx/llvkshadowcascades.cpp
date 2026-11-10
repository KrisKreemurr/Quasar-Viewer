/**
 * @file llvkshadowcascades.cpp
 * @brief Sun / moon shadow cascades: the per-frame fit and the cache schedule (see the header).
 */
#include "llvkshadowcascades.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// The vector arithmetic of LLVector3, operation for operation (the fit used LLVector3; the
// same float operations in the same order give the same bits).
struct V3
{
    float x, y, z;
};
inline V3 v3(const float* p) { return {p[0], p[1], p[2]}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b)   // LLVector3 operator%
{
    return {a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x, a.x * b.y - b.x * a.y};
}
inline void normalize(V3& a)   // LLVector3::normalize (FP_MAG_THRESHOLD 1e-7)
{
    const float mag = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (mag > 0.0000001f)
    {
        const float k = 1.f / mag;   // (operator/= multiplies by the reciprocal)
        a.x *= k; a.y *= k; a.z *= k;
    }
    else
    {
        a = {0.f, 0.f, 0.f};
    }
}
inline double dotd(V3 a, V3 b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }

// Frustum planes (ax+by+cz+d >= 0 inside) of a column-major world -> VK clip matrix
// (depth 0..1): L R B T near far.
void clipPlanes(const float* M, float out[6][4])
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
bool sphereInPlanes(const float planes[6][4], const float* c, float r)
{
    for (int i = 0; i < 6; ++i)
    {
        const float* p = planes[i];
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r) return false;
    }
    return true;
}
}   // namespace

void vkShadowRebaseOrigin(const float* M, const float* from, const float* to, float* out)
{
    const double d[3] = {(double)to[0] - (double)from[0], (double)to[1] - (double)from[1],
                         (double)to[2] - (double)from[2]};
    float tmp[16];
    std::memcpy(tmp, M, sizeof(tmp));
    for (int r = 0; r < 4; ++r)
        tmp[12 + r] = (float)((double)M[12 + r] + (double)M[r] * d[0] + (double)M[4 + r] * d[1]
                              + (double)M[8 + r] * d[2]);
    std::memcpy(out, tmp, sizeof(tmp));
}

// ---- the fit (was LLAppViewer's LLVKWorldFrame build, "Sun/moon shadow cascades") -------
// Splits: LL's exponent formula over [near, range] (clip[0] padded 1.25x). Each cascade is
// fitted as the BOUNDING SPHERE of its overlapping frustum slice (LL's 0.75/1.25 bands), so
// its ortho extent is constant and the centre can be snapped to whole shadow texels in
// light space -- stable under camera motion, where LL's per-frame AABB/LiSPSM refit swims.
// The ortho near is pulled far toward the light (cull planes included) so casters behind
// the camera still land in the map; D32 keeps the long depth range precise.
void vkShadowFitCascades(const VKShadowFitIn& in, VKShadowFitOut& out)
{
    const V3 L = v3(in.light), eye = v3(in.eye), at = v3(in.at), left = v3(in.left), up = v3(in.up);
    const float fovY = in.fov_y, aspect = in.aspect;
    const float n = std::clamp(in.near_clip, 0.1f, 4.f);
    const float f = std::max(n + 16.f, in.range);
    const float* sx = in.split_exp;
    const float da = powf(1.f - std::max(fabsf(dot(L, up)), fabsf(dot(L, left))), sx[2]);
    const float sxp = sx[1] + (sx[0] - sx[1]) * da;
    float clip[4];
    for (int i = 0; i < 4; ++i) clip[i] = n + (f - n) * powf((float)(i + 1) / 4.f, sxp);
    clip[0] *= 1.25f;

    // Light basis: fw = direction light travels (ortho forward), rt/uu span the map.
    const V3 fw = -L;
    V3 rt = cross(fabsf(fw.z) < 0.99f ? V3{0.f, 0.f, 1.f} : V3{1.f, 0.f, 0.f}, fw);
    normalize(rt);
    const V3 uu = cross(fw, rt);
    const float k = tanf(fovY * 0.5f) * sqrtf(1.f + aspect * aspect);   // half-diagonal slope
    const uint32_t res = in.res;
    // Capped at 2048 m (unchanged for draw distances <= 1024): at a 4 km draw distance a
    // 8 km pull-back would drag skyboxes kilometres overhead into the ground cascades (and
    // the caster cull) while the shadow range stays 512 m.
    const float back = std::clamp(2.f * in.render_far, 512.f, 2048.f);
    for (int c = 0; c < 4; ++c)
    {
        const float a = (c == 0) ? n : clip[c - 1] * 0.75f;
        const float b = (c == 3) ? clip[3] : clip[c] * 1.25f;
        // Minimal sphere around the slice's 8 corners (centre on the view axis).
        float t = std::min(b, (a + b) * (1.f + k * k) * 0.5f);
        float R = sqrtf(std::max((b - t) * (b - t) + b * b * k * k, (t - a) * (t - a) + a * a * k * k));
        if (in.ortho)   // the box slice: the same half diagonal at both ends
        {
            t = 0.5f * (a + b);
            R = sqrtf(0.25f * (b - a) * (b - a) + in.ortho_hw * in.ortho_hw + in.ortho_hh * in.ortho_hh);
        }
        const V3 ctr = eye + at * t;
        out.slice[c][0] = ctr.x; out.slice[c][1] = ctr.y; out.slice[c][2] = ctr.z; out.slice[c][3] = R;
        // Cached cascades are fitted larger (VKShadowFitIn::scale), so the map stays valid
        // while the camera moves until the renderer's coverage test says re-render.
        if (in.scale[c] != 1.f) R *= in.scale[c];
        R = ceilf(R * 4.f) * 0.25f;
        const float texel = 2.f * R / (float)res;
        // The border: the map's half extent grows by whole texels (the texel stays), and the
        // depth range by as much on its far side (away from the light; the near side has the
        // pull-back), so the slice may move that far either way before the map stops covering it.
        const float Rz = R;
        const float zpad = in.border ? (float)in.border * texel : 0.f;
        if (in.border) R += zpad;
        // TEXEL SNAP in ABSOLUTE light space (the grid is fixed to the world, so the map is
        // stable under camera motion), in double: every input is a float, so the products
        // are exact and floor() sees the true quotient -- at Z 3000 the float quotient
        // (~1e6 texels) was itself off by ~0.03 texel.
        const double texd = (double)texel;
        const double cxd = std::floor(dotd(ctr, rt) / texd) * texd;
        const double cyd = std::floor(dotd(ctr, uu) / texd) * texd;
        const float cx = (float)cxd;
        const float cy = (float)cyd;
        const float cz = dot(ctr, fw);
        const float znear = cz - Rz - back, zfar = cz + Rz + zpad;
        const float iz = 1.f / (zfar - znear);
        // World -> shadow NDC (x,y in [-1,1] map to uv like the VK viewport; z in [0,1]),
        // column-major. Rendered and sampled with the same matrix, so no Y flip is
        // involved. Mw (world space) only feeds the cull planes.
        float Mw[16];
        Mw[0] = rt.x / R; Mw[4] = rt.y / R; Mw[8]  = rt.z / R; Mw[12] = -cx / R;
        Mw[1] = uu.x / R; Mw[5] = uu.y / R; Mw[9]  = uu.z / R; Mw[13] = -cy / R;
        Mw[2] = fw.x * iz; Mw[6] = fw.y * iz; Mw[10] = fw.z * iz; Mw[14] = -znear * iz;
        Mw[3] = 0.f; Mw[7] = 0.f; Mw[11] = 0.f; Mw[15] = 1.f;
        // CAMERA-RELATIVE (#555): what the GPU gets maps (world - eye), the translation
        // built in double (the snapped centre minus the eye, both ~km along the light axes,
        // cancel exactly here instead of per vertex / per pixel in float). World-space, at
        // Z 3000 the receiver depth and the caster depth each carried ~2e-4 m of float
        // noise -- 3x the D32 constant bias -- which re-rounded every frame: flickering acne
        // on sun-facing floors (normal offset ~0 there). Camera-relative: ~2e-5 m.
        {
            const double zneard = dotd(ctr - eye, fw) - (double)Rz - (double)back;   // znear - eye.fw
            float* M = out.mat[c];
            std::memcpy(M, Mw, sizeof(Mw));
            M[12] = (float)(-(cxd - dotd(eye, rt)) / (double)R);
            M[13] = (float)(-(cyd - dotd(eye, uu)) / (double)R);
            M[14] = (float)(-zneard * (double)iz);
        }
        // Cull planes (Gribb-Hartmann, VK depth 0..1): L R B T near far.
        clipPlanes(Mw, out.planes[c]);
        out.clip[c]  = clip[c];
        out.texel[c] = texel;

        // SHADOW-CASTER PLANES for this cascade's view slice [a, b]: a shadow travels along
        // fw, so for every slice plane whose inward normal does not point along fw
        // (n.fw <= 0) the caster itself must already be inside it. Necessary, not
        // sufficient -- conservative by construction.
        {
            const float hy = fovY * 0.5f;
            const float hx = atanf(tanf(hy) * aspect);
            V3 nrm[6];
            float dd[6];
            auto side = [&](const V3& edge_dir, const V3& axis, int i) {
                V3 nn = cross(axis, edge_dir);
                if (dot(nn, at) < 0.f) nn = -nn;
                normalize(nn);
                nrm[i] = nn; dd[i] = -dot(nn, eye);
            };
            side(at * cosf(hx) + left * sinf(hx), up,   0);   // left
            side(at * cosf(hx) - left * sinf(hx), up,   1);   // right
            side(at * cosf(hy) + up * sinf(hy),   left, 2);   // top
            side(at * cosf(hy) - up * sinf(hy),   left, 3);   // bottom
            if (in.ortho)   // the box's sides: parallel to the view axis
            {
                nrm[0] = -left; dd[0] = in.ortho_hw + dot(left, eye);   // left
                nrm[1] =  left; dd[1] = in.ortho_hw - dot(left, eye);   // right
                nrm[2] = -up;   dd[2] = in.ortho_hh + dot(up, eye);     // top
                nrm[3] =  up;   dd[3] = in.ortho_hh - dot(up, eye);     // bottom
            }
            nrm[4] = at;  dd[4] = -dot(at, eye) - a;            // slice near
            nrm[5] = -at; dd[5] =  dot(at, eye) + b;            // slice far
            bool kept[6];
            for (int i = 0; i < 6; ++i)
            {
                float* P = out.caster_planes[c][i];
                kept[i] = dot(nrm[i], fw) <= 0.f;
                if (kept[i])
                { P[0] = nrm[i].x; P[1] = nrm[i].y; P[2] = nrm[i].z; P[3] = dd[i]; }
                else
                { P[0] = 0.f; P[1] = 0.f; P[2] = 0.f; P[3] = 1.f; }
            }
            // The swept volume's other planes: through each silhouette edge (faces i, j: one
            // kept, one dropped) parallel to the light, oriented to hold the slice. Corners:
            // (depth d, side sx = +1 left, sy = +1 top).
            {
                const float tx = in.ortho ? 0.f : tanf(hx), ty = in.ortho ? 0.f : tanf(hy);
                auto corner = [&](float dist, float hs, float vs) {
                    const float ox = in.ortho ? in.ortho_hw : dist * tx, oy = in.ortho ? in.ortho_hh : dist * ty;
                    return eye + at * dist + left * (hs * ox) + up * (vs * oy);
                };
                const V3 centroid = eye + at * (0.5f * (a + b));
                // the 12 edges: faces (0 left, 1 right, 2 top, 3 bottom, 4 near, 5 far) and ends
                struct Edge { int fi, fj; float d0, sx0, sy0, d1, sx1, sy1; };
                const Edge edges[12] = {
                    {0, 2, a,  1,  1, b,  1,  1}, {0, 3, a,  1, -1, b,  1, -1},
                    {1, 2, a, -1,  1, b, -1,  1}, {1, 3, a, -1, -1, b, -1, -1},
                    {4, 0, a,  1,  1, a,  1, -1}, {4, 1, a, -1,  1, a, -1, -1},
                    {4, 2, a,  1,  1, a, -1,  1}, {4, 3, a,  1, -1, a, -1, -1},
                    {5, 0, b,  1,  1, b,  1, -1}, {5, 1, b, -1,  1, b, -1, -1},
                    {5, 2, b,  1,  1, b, -1,  1}, {5, 3, b,  1, -1, b, -1, -1}};
                uint32_t ns = 0;
                for (const Edge& e : edges)
                {
                    if (kept[e.fi] == kept[e.fj] || ns >= 6u) continue;
                    const V3 p0 = corner(e.d0, e.sx0, e.sy0), p1 = corner(e.d1, e.sx1, e.sy1);
                    V3 nn = cross(p1 - p0, fw);
                    const float len = std::sqrt(dot(nn, nn));
                    if (!(len > 1e-6f * std::sqrt(dot(p1 - p0, p1 - p0)))) continue;   // (edge along the light)
                    nn = nn * (1.f / len);
                    float d = -dot(nn, p0);
                    if (dot(nn, centroid) + d < 0.f) { nn = -nn; d = -d; }
                    float* P = out.sil_planes[c][ns++];
                    P[0] = nn.x; P[1] = nn.y; P[2] = nn.z; P[3] = d;
                }
                out.n_sil[c] = ns;
                for (uint32_t u = ns; u < 6u; ++u)
                {
                    float* P = out.sil_planes[c][u];
                    P[0] = 0.f; P[1] = 0.f; P[2] = 0.f; P[3] = 1.f;
                }
            }
        }
    }
}

// ---- the cache schedule (was LLCleanRendererVK::setShadows' body) -----------------------
// CASCADE CACHING (#98). Realtime (0): every cascade every frame. Balanced (1) /
// Performance (2) without the event cache: the nearest cascade every frame; the others at
// most every 2/4/8 (balanced) or 4/16/32 (performance) frames -- sooner when this frame's
// view slice no longer fits the cached map or the light jumped. EVENT-DRIVEN CACHE (#567,
// with the static / dynamic split): no periodic re-render and no always-rendered nearest
// cascade -- a cascade re-renders only when the view slice outgrows it, the light turned by
// light_tx texels at the map's extent, a static change touched its volume, or after the
// safety period. A cascade that isn't re-rendered is SAMPLED with the matrix + texel it was
// rendered with.
void vkShadowSchedule(VKShadowCacheEntry cache[VKSC_CASCADES], const VKShadowOvlEntry ovl[VKSC_CASCADES],
                      const VKShadowSchedIn& in, VKShadowSchedOut& out)
{
    out = VKShadowSchedOut{};
    static const uint32_t kInterval[3][VKSC_CASCADES] = { {1, 1, 1, 1}, {1, 2, 4, 8}, {1, 4, 16, 32} };
    const uint32_t mode = std::min<uint32_t>(in.mode, 2u);
    const bool event_cache = in.event_cache;
    const uint32_t frame = in.frame;
    const size_t n_sph = in.spheres ? in.n_spheres : 0;
    // #825 EVEN-OUT: what each cascade's re-render was for. FORCED (the cache is invalid, the
    // light jumped, the view slice outgrew the map, a non-event mode's interval) renders this
    // frame; DEFERRABLE (a static change in the map, the light's slow drift, the safety net,
    // an earlier deferral coming due) may wait a frame -- see the budget below.
    bool want[VKSC_CASCADES] = {}, forced[VKSC_CASCADES] = {};
    float cached_mat[VKSC_CASCADES][16] = {};
    for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
    {
        VKShadowCacheEntry& sc = cache[c];
        bool render = event_cache
            ? (!sc.valid || (frame - sc.frame) >= in.safety + c * 37u)   // (staggered)
            : (mode == 0 || c == 0 || !sc.valid || (frame - sc.frame) >= kInterval[mode][c]);
        bool force = !sc.valid;
        if (!sc.valid) sc.deferred = 0;
        if (event_cache && sc.valid && render) ++out.ev_safety;
        // The cached map's matrix re-expressed relative to THIS frame's eye (#555): the
        // map's world placement is unchanged, only the origin the GPU subtracts moved.
        float* cached = cached_mat[c];
        if (sc.valid) vkShadowRebaseOrigin(sc.mat, sc.eye, in.eye, cached);
        // #825: the light-jump and coverage tests run even when the safety net / interval
        // already asked for a render -- they make it FORCED (never deferred below).
        if (sc.valid)
        {
            // Light jump: compare the maps' depth axes (row 2 of both matrices).
            const float* A = sc.mat; const float* B = in.mat[c];
            const float la = std::sqrt(A[2]*A[2] + A[6]*A[6] + A[10]*A[10]);
            const float lb = std::sqrt(B[2]*B[2] + B[6]*B[6] + B[10]*B[10]);
            const float cosang = (la > 0.f && lb > 0.f) ? (A[2]*B[2] + A[6]*B[6] + A[10]*B[10]) / (la * lb) : -1.f;
            if (cosang < 0.99939f) { render = true; force = true; }   // ~2 degrees
            if (!render && event_cache && sc.texel > 0.f)
            {
                // #567: the slow day-cycle drift, measured where it shows: a shadow moves by
                // the light's turn (row 2, the depth axis) x its caster-to-receiver distance,
                // at most the map's depth range (extent); re-render past light_tx texels.
                // (The basis' roll, row 0, moves no shadow: drift_roll, the old rule only.)
                // (atan2 of |a x b| and a.b in double: acos of a float dot near 1 has a
                // ~3e-4 rad noise floor, a texel at the far cascade's reach)
                auto rowAngle = [](const float* P, const float* Q, int r) {
                    const double ax = P[r], ay = P[4 + r], az = P[8 + r], bx = Q[r], by = Q[4 + r], bz = Q[8 + r];
                    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
                };
                const float ra = std::sqrt(A[0]*A[0] + A[4]*A[4] + A[8]*A[8]);
                const double ang = in.drift_roll ? std::max(rowAngle(A, B, 2), rowAngle(A, B, 0)) : rowAngle(A, B, 2);
                double extent = std::max(la > 0.f ? 1.0 / la : 0.0, ra > 0.f ? 2.0 / ra : 0.0);
                if (in.lever[c] > 0.f) extent = std::min(extent, (double)in.lever[c]);
                if (ang * extent > (double)(in.light_tx * sc.texel)) { render = true; ++out.ev_light; }
            }
        }
        if (sc.valid && !force)
        {
            // Coverage: this frame's slice sphere, projected with the CACHED matrix, must
            // lie inside the cached map (xy extent +-1, depth 0..1). The sphere centre is
            // agent space; the matrix maps (P - eye) (#555).
            const float* M = cached;
            const float S[4] = {(float)((double)in.slice[c][0] - in.eye[0]),
                                (float)((double)in.slice[c][1] - in.eye[1]),
                                (float)((double)in.slice[c][2] - in.eye[2]),
                                in.slice[c][3]};
            const float x = M[0]*S[0] + M[4]*S[1] + M[8]*S[2]  + M[12];
            const float y = M[1]*S[0] + M[5]*S[1] + M[9]*S[2]  + M[13];
            const float z = M[2]*S[0] + M[6]*S[1] + M[10]*S[2] + M[14];
            const float sxy = std::sqrt(M[0]*M[0] + M[4]*M[4] + M[8]*M[8]);   // 1 / R
            const float sz  = std::sqrt(M[2]*M[2] + M[6]*M[6] + M[10]*M[10]); // 1 / depth range
            const float rxy = S[3] * sxy, rz = S[3] * sz;
            if (std::fabs(x) + rxy > 1.f || std::fabs(y) + rxy > 1.f || z - rz < 0.f || z + rz > 1.f)
            {
                if (event_cache && !render) ++out.ev_slice;
                render = true;
                force = true;   // (the map no longer covers the view: wrong shadows now)
            }
        }
        if (!render && event_cache && (in.change_overflow || n_sph))
        {
            // #567: a static change inside the cached map's whole volume (every depth: a
            // caster anywhere along the light inside it), spheres agent space -> this eye.
            bool hit = in.change_overflow;
            uint32_t by = 8u;   // the producer of the first sphere hit (8 = overflow)
            if (!hit)
            {
                float planes[6][4];
                clipPlanes(cached, planes);
                for (size_t i = 0; i < n_sph && !hit; ++i)
                {
                    const float* s = &in.spheres[i * 4];
                    const float rel[3] = { (float)((double)s[0] - in.eye[0]), (float)((double)s[1] - in.eye[1]),
                                           (float)((double)s[2] - in.eye[2]) };
                    hit = sphereInPlanes(planes, rel, s[3]);
                    if (hit) by = (in.sphere_tags && i < in.n_tags) ? std::min<uint32_t>(in.sphere_tags[i], 7u) : 7u;
                }
            }
            if (hit) { render = true; ++out.ev_change; ++out.ev_by[by]; }
        }
        // An earlier deferral (its change spheres are gone: the latch carries it) comes due.
        if (event_cache && sc.deferred) render = true;
        want[c] = render;
        forced[c] = render && (force || !event_cache);
    }
    // #825 FAR-CASCADE BUDGET (event-driven cache, defer_max N > 0): a far cascade (2 / 3:
    // the expensive maps; their nested volumes take the same distant change together)
    // re-renders for a DEFERRABLE reason at most once per frame -- none when a forced far
    // re-render or the scheduled probe's face 5 + cube filter already loads the frame. A
    // deferred cascade keeps its cached map (a consistent matrix + content, at most N frames
    // behind the change) and LATCHES the re-render (deferred): it renders once its wait
    // reaches N whatever else is due, longest-waiting first otherwise. Cascades 0 / 1 (near,
    // cheap, the most visible) and forced reasons are never deferred.
    {
        const uint32_t defer_max = std::min<uint32_t>(in.defer_max, 8u);
        if (!event_cache || defer_max == 0u)
        {
            for (uint32_t c = 0; c < VKSC_CASCADES; ++c) cache[c].deferred = 0;
        }
        else
        {
            bool far_forced = false;
            for (uint32_t c = 2; c < VKSC_CASCADES; ++c) far_forced = far_forced || forced[c];
            uint32_t budget = (far_forced || in.probe_face5) ? 0u : 1u;
            uint32_t order[VKSC_CASCADES];
            uint32_t n_cand = 0;
            for (uint32_t c = 2; c < VKSC_CASCADES; ++c)
                if (want[c] && !forced[c]) order[n_cand++] = c;
            std::stable_sort(order, order + n_cand, [&](uint32_t x, uint32_t y)
                             { return cache[x].deferred > cache[y].deferred; });
            for (uint32_t i = 0; i < n_cand; ++i)
            {
                VKShadowCacheEntry& sc = cache[order[i]];
                if (sc.deferred >= defer_max) continue;      // waited long enough: renders
                if (budget) { --budget; continue; }
                want[order[i]] = false;                      // waits a frame
                ++sc.deferred;
                ++out.n_deferred;
            }
        }
    }
    for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
    {
        VKShadowCacheEntry& sc = cache[c];
        const bool render = want[c];
        out.render[c] = render;
        if (render)
        {
            sc.valid = true; sc.frame = frame;
            sc.deferred = 0;
            std::memcpy(sc.mat, in.mat[c], sizeof(sc.mat));
            std::memcpy(sc.eye, in.eye, sizeof(sc.eye));
            sc.texel = in.texel[c];
            ++out.n_render;
            std::memcpy(out.mat[c], in.mat[c], sizeof(out.mat[c]));
            out.texel[c] = in.texel[c];
        }
        else
        {
            std::memcpy(out.mat[c], cached_mat[c], sizeof(cached_mat[c]));
            out.texel[c] = sc.texel;
        }
    }
    // STAGGERED FAR-CASCADE OVERLAY (far_n N, overlay modes): the dynamic overlay of
    // cascades 2 and 3 (static copy + dynamic / rigged / particle casters) runs every N
    // frames, the two alternating so the per-frame cost stays even (#825: by age, below); a
    // skipped cascade's sampled layer keeps the last overlay, built on the SAME cached map
    // (its matrix is the cached one rebased onto this eye: the same world placement), so
    // only the dynamic casters lag, by at most N - 1 frames (N after a far re-render shifted
    // the alternation), in the far cascades. Forced whenever the cascade re-renders (new
    // matrix / static content), its last overlay is gone (the cache was invalidated) or was
    // built on another render of the cache. Cascades 0 / 1 every frame.
    {
        const uint32_t far_n = std::clamp<uint32_t>(in.far_n, 1u, 16u);
        // #825: the periodic refresh is by AGE, at most one far cascade per frame (the
        // oldest once it is N frames old) and none in a frame that already redraws a far
        // overlay for a forced reason (a re-render: new static content).
        bool force_ovl[VKSC_CASCADES] = {};
        bool far_forced = false;
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            const VKShadowOvlEntry& o = ovl[c];
            force_ovl[c] = c < 2u || far_n <= 1u || out.render[c] || !o.valid
                        || o.src != cache[c].frame || (frame - o.frame) > far_n;
            if (c >= 2u && force_ovl[c] && cache[c].valid) far_forced = true;
        }
        int periodic = -1;
        if (!far_forced)
            for (uint32_t c = 2; c < VKSC_CASCADES; ++c)
            {
                const uint32_t age = frame - ovl[c].frame;
                if (!force_ovl[c] && age >= far_n
                    && (periodic < 0 || age > frame - ovl[periodic].frame)) periodic = (int)c;
            }
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            const bool refresh = force_ovl[c] || (int)c == periodic;
            out.ovl_refresh[c] = refresh;
            if (!refresh && in.overlay && cache[c].valid) ++out.n_ovl_skip;
        }
    }
}
