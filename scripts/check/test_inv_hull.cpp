// Inverted-hull outline fixture (scripts/check/checks/rt_inverted_hull.py). Three parts:
//   CLASSIFY  indra/newview/llcleaninvhull.h (included as-is: the rule the capture runs) on authored shapes --
//             a body (torso + arms + head, closed), the same body as an inverted hull (pushed 3 mm out, winding
//             flipped), an open sleeve tube both ways, a flat card both ways, a tiny shell: only the inward-wound
//             shells are marked.
//   SHADOW    a lamp 1 m from a body wrapped in its hull. The hardware facing (front <=> the winding normal
//             faces the ray: dot(cross(e1, e2), dir) < 0) with the rays' rules: OLD positional (no facing cull,
//             the hull traced) shadows the skin on the lamp's side entirely; the facing cull
//             (clean_rt_common.glsl rtFacingCullSet) or the hull left out (RenderVKRTOutlineHullExclude) lights
//             it; another body between skin and lamp still shadows under the cull.
//   GI        cosine rays from the skin: with the hull traced every ray hits it within millimetres (the dark
//             inside GI took for the world); left out, none does. The FIREFLY source: a hull hit beside a thin
//             part (an ear, 4 mm, hull 2 mm off each side) faces the part; a lamp beyond it lights the hit
//             (N . l > 0) and the hit's shadow ray as rtrProjRaySetup aims it (1 cm along N toward the lamp)
//             starts past the ear AND the far hull wall -- unshadowed, though the ear hides the lamp.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "llcleaninvhull.h"

struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(V3 a) { return std::sqrt(dot(a, a)); }
static V3 nrm(V3 a) { return a * (1.f / len(a)); }

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// a mesh face: positions + 16-bit indices (CCW = front)
struct Mesh
{
    std::vector<V3> p;
    std::vector<uint16_t> i;
    void tri(uint16_t a, uint16_t b, uint16_t c, bool flip) { i.push_back(a); i.push_back(flip ? c : b); i.push_back(flip ? b : c); }
    void add(const Mesh& m) { const uint16_t o = (uint16_t)p.size(); p.insert(p.end(), m.p.begin(), m.p.end()); for (uint16_t k : m.i) i.push_back((uint16_t)(k + o)); }
};
// a UV sphere wound outward (flip: inward)
static Mesh sphere(V3 c, float r, int nu, int nv, bool flip)
{
    Mesh m;
    for (int v = 0; v <= nv; ++v)
        for (int u = 0; u < nu; ++u)
        {
            const float th = 3.14159265f * v / nv, ph = 6.2831853f * u / nu;
            m.p.push_back(c + V3{std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)} * r);
        }
    for (int v = 0; v < nv; ++v)
        for (int u = 0; u < nu; ++u)
        {
            const uint16_t a = (uint16_t)(v * nu + u), b = (uint16_t)(v * nu + (u + 1) % nu);
            const uint16_t c2 = (uint16_t)((v + 1) * nu + u), d = (uint16_t)((v + 1) * nu + (u + 1) % nu);
            // outward: (a, c2, b) and (b, c2, d) -- theta grows toward c2, phi toward b
            m.tri(a, c2, b, flip);
            m.tri(b, c2, d, flip);
        }
    return m;
}
// a tube along +x from x0 to x1, radius r, centred on (y, z) = (cy, cz), wound outward (flip: inward); caps
// closed or open
static Mesh tube(float x0, float x1, float cy, float cz, float r, int nu, int nx, bool caps, bool flip)
{
    Mesh m;
    for (int k = 0; k <= nx; ++k)
        for (int u = 0; u < nu; ++u)
        {
            const float ph = 6.2831853f * u / nu;
            m.p.push_back({x0 + (x1 - x0) * k / nx, cy + r * std::cos(ph), cz + r * std::sin(ph)});
        }
    for (int k = 0; k < nx; ++k)
        for (int u = 0; u < nu; ++u)
        {
            const uint16_t a = (uint16_t)(k * nu + u), b = (uint16_t)(k * nu + (u + 1) % nu);
            const uint16_t c = (uint16_t)((k + 1) * nu + u), d = (uint16_t)((k + 1) * nu + (u + 1) % nu);
            m.tri(a, b, c, flip);   // (x, then phi: outward)
            m.tri(b, d, c, flip);
        }
    if (caps)
    {
        const uint16_t c0 = (uint16_t)m.p.size(); m.p.push_back({x0, cy, cz});
        const uint16_t c1 = (uint16_t)m.p.size(); m.p.push_back({x1, cy, cz});
        for (int u = 0; u < nu; ++u)
        {
            m.tri(c0, (uint16_t)((u + 1) % nu), (uint16_t)u, flip);                       // -x cap faces -x
            m.tri(c1, (uint16_t)(nx * nu + u), (uint16_t)(nx * nu + (u + 1) % nu), flip);  // +x cap faces +x
        }
    }
    return m;
}
// a flat card in z = 0
static Mesh card(int n, bool flip)
{
    Mesh m;
    for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) m.p.push_back({x * 0.05f, y * 0.05f, 0.f});
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
    {
        const uint16_t a = (uint16_t)(y * (n + 1) + x), b = (uint16_t)(a + 1), c = (uint16_t)(a + n + 1), d = (uint16_t)(c + 1);
        m.tri(a, b, c, flip); m.tri(b, d, c, flip);
    }
    return m;
}
// a body: torso sphere, two arms (tubes, closed), a head sphere -- off = pushed out by off (a hull)
static Mesh body(float off, bool flip)
{
    Mesh m;
    m.add(sphere({0.f, 0.f, 0.f}, 0.15f + off, 24, 16, flip));
    m.add(tube(0.10f, 0.75f, 0.f, 0.05f, 0.04f + off, 16, 12, true, flip));
    m.add(tube(-0.75f, -0.10f, 0.f, 0.05f, 0.04f + off, 16, 12, true, flip));
    m.add(sphere({0.f, 0.f, 0.27f}, 0.10f + off, 20, 12, flip));
    return m;
}
static float q(const Mesh& m)
{
    return cleanShellWindingQ([&m](uint32_t v) { return &m.p[v].x; }, m.i.data(), (uint32_t)m.i.size(), (uint32_t)m.p.size());
}
static bool shell(const Mesh& m)
{
    return cleanInsideOutShell([&m](uint32_t v) { return &m.p[v].x; }, m.i.data(), (uint32_t)m.i.size(), (uint32_t)m.p.size());
}

// ---- rays ----
struct Hit { bool hit; float t; bool front; V3 ng; };   // ng: the winding normal (unit)
// closest hit along o + d t, t in (tmin, tmax); cullFront: the shadow rays' facing cull (front faces skipped)
static Hit trace(const Mesh& m, V3 o, V3 d, float tmin, float tmax, bool cullFront)
{
    Hit h{false, tmax, false, {0, 0, 0}};
    for (size_t k = 0; k + 2 < m.i.size(); k += 3)
    {
        const V3 a = m.p[m.i[k]], b = m.p[m.i[k + 1]], c = m.p[m.i[k + 2]];
        const V3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
        const float det = dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        const float inv = 1.f / det;
        const V3 tv = o - a;
        const float u = dot(tv, pv) * inv;
        if (u < 0.f || u > 1.f) continue;
        const V3 qv = cross(tv, e1);
        const float v = dot(d, qv) * inv;
        if (v < 0.f || u + v > 1.f) continue;
        const float t = dot(e2, qv) * inv;
        if (!(t > tmin && t < h.t)) continue;
        const V3 n = cross(e1, e2);
        const bool front = dot(n, d) < 0.f;   // (the spec's facing: the raster's front, CCW from the origin)
        if (cullFront && front) continue;
        h = {true, t, front, nrm(n)};
    }
    return h;
}
static bool blocked(const Mesh& m, V3 o, V3 lamp, bool cullFront)
{
    const V3 lv = lamp - o;
    const float l = len(lv);
    return trace(m, o, lv * (1.f / l), 1e-4f, l, cullFront).hit;
}

int main()
{
    // ---- CLASSIFY ----
    const Mesh bodyOut = body(0.f, false), hull = body(0.003f, true);
    std::printf("q: body %+.3f, hull %+.3f", q(bodyOut), q(hull));
    CHECK(!shell(bodyOut), "the body (wound outward) is marked inside-out (q %g)", q(bodyOut));
    CHECK(shell(hull), "the inverted hull is not marked (q %g)", q(hull));
    const Mesh sleeve = tube(0.f, 0.6f, 0.f, 0.f, 0.04f, 16, 12, false, false), sleeveIn = tube(0.f, 0.6f, 0.f, 0.f, 0.045f, 16, 12, false, true);
    std::printf(", sleeve %+.3f / inverted %+.3f", q(sleeve), q(sleeveIn));
    CHECK(!shell(sleeve), "an open sleeve wound outward is marked (q %g)", q(sleeve));
    CHECK(shell(sleeveIn), "an open inverted sleeve shell is not marked (q %g)", q(sleeveIn));
    CHECK(!shell(card(8, false)) && !shell(card(8, true)), "a flat card is marked inside-out");
    const Mesh head = sphere({0.f, 0.f, 0.f}, 0.1f, 24, 16, false);
    CHECK(!shell(head) && shell(sphere({0.f, 0.f, 0.f}, 0.103f, 24, 16, true)), "a head / its hull misclassified");
    CHECK(!shell(sphere({0.f, 0.f, 0.f}, 0.1f, 4, 3, true)), "a 24-triangle-minimum detail is marked");
    std::printf("\n");

    // ---- SHADOW: a lamp 1 m off the body's torso side; the receivers are the torso's lamp-facing skin ----
    Mesh scene = bodyOut; scene.add(hull);
    Mesh sceneEx = bodyOut;                         // (the hull left out of the RT scene)
    const V3 lamp{0.f, -1.f, 0.f};
    int nRecv = 0, oldBlk = 0, cullBlk = 0, exBlk = 0;
    for (int a = 0; a < 64; ++a)
    {
        const float th = 0.3f + 1.0f * float(a % 8) / 8.f, ph = -2.2f + 1.4f * float(a / 8) / 8.f;   // (y < 0 side)
        const V3 n{std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)};
        if (!(dot(n, nrm(lamp - n * 0.15f)) > 0.3f)) continue;
        const V3 P = n * 0.15f + n * 1e-4f;   // (rtReceiverOrigin: a hair off the skin)
        ++nRecv;
        oldBlk += blocked(scene, P, lamp, false);
        cullBlk += blocked(scene, P, lamp, true);
        exBlk += blocked(sceneEx, P, lamp, false);
    }
    std::printf("shadow: %d lamp-facing receivers -- old positional rule %d shadowed, facing cull %d, hull out %d\n",
                nRecv, oldBlk, cullBlk, exBlk);
    CHECK(nRecv > 10, "fixture sanity: too few receivers (%d)", nRecv);
    CHECK(oldBlk == nRecv, "fixture sanity: the traced hull no longer shadows the skin under the old rule (%d / %d)", oldBlk, nRecv);
    CHECK(cullBlk == 0, "facing cull: %d receivers still shadowed by their own outline", cullBlk);
    CHECK(exBlk == 0, "hull out: %d receivers still shadowed", exBlk);
    {   // another body between skin and lamp still shadows under the cull (its far side's back faces)
        Mesh s2 = scene; s2.add(sphere({0.f, -0.5f, 0.f}, 0.1f, 24, 16, false));
        const V3 P = V3{0.f, -0.15f, 0.f} + V3{0.f, -1e-4f, 0.f};
        CHECK(blocked(s2, P, lamp, true), "facing cull: a body between the skin and the lamp no longer shadows it");
    }

    // ---- GI: cosine rays from the torso's skin ----
    uint32_t rng = 0x9e3779b9u;
    auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) * (1.f / 16777216.f); };
    int rays = 0, hullHit = 0, exHit = 0;
    float maxD = 0.f;
    for (int k = 0; k < 400; ++k)
    {
        const V3 n = nrm(V3{rnd() - 0.5f, -0.5f - rnd(), rnd() - 0.5f});   // (the torso's front, away from the arms)
        const V3 P = n * 0.15f + n * 1e-4f;
        // cosine sample about n
        const V3 t = nrm(std::fabs(n.z) < 0.9f ? cross(n, V3{0, 0, 1}) : cross(n, V3{1, 0, 0})), b = cross(n, t);
        const float r1 = rnd(), r2 = 6.2831853f * rnd(), sr = std::sqrt(r1);
        const V3 d = nrm(t * (sr * std::cos(r2)) + b * (sr * std::sin(r2)) + n * std::sqrt(1.f - r1));
        ++rays;
        const Hit h = trace(scene, P, d, 1e-4f, 10.f, false);
        // (the GI walk skips single-sided BACK faces; a hit on the hull from inside is a FRONT face)
        if (h.hit && h.front && h.t < 0.02f) { ++hullHit; maxD = std::fmax(maxD, h.t); }
        const Hit he = trace(sceneEx, P, d, 1e-4f, 10.f, false);
        if (he.hit && he.t < 0.02f) ++exHit;
    }
    std::printf("GI: %d skin rays -- hull traced: %d hit its FRONT within %.1f mm; hull out: %d near hits\n",
                rays, hullHit, maxD * 1000.f, exHit);
    CHECK(hullHit > rays * 9 / 10, "fixture sanity: the traced hull catches only %d of %d GI rays", hullHit, rays);
    CHECK(exHit == 0, "hull out: %d GI rays still stop within 2 cm of the skin", exHit);
    {   // the firefly source: an ear (a 4 mm slab, x in [0, 0.004]) inside its hull (2 mm off each side)
        Mesh ear, earHull;
        auto slab = [](Mesh& m, float x0, float x1, bool flip) {
            // a closed box x in [x0, x1], y, z in [-0.03, 0.03], wound outward (flip: inward)
            const float s = 0.03f;
            const uint16_t o = (uint16_t)m.p.size();
            for (int k = 0; k < 8; ++k) m.p.push_back({(k & 1) ? x1 : x0, (k & 2) ? s : -s, (k & 4) ? s : -s});
            const int f[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
            for (const auto& q4 : f)
            {
                m.tri((uint16_t)(o + q4[0]), (uint16_t)(o + q4[1]), (uint16_t)(o + q4[2]), flip);
                m.tri((uint16_t)(o + q4[0]), (uint16_t)(o + q4[2]), (uint16_t)(o + q4[3]), flip);
            }
        };
        slab(ear, 0.f, 0.004f, false);
        slab(earHull, -0.002f, 0.006f, true);
        Mesh s = ear; s.add(earHull);
        const V3 skin{-1e-4f, 0.005f, 0.002f};     // the ear's -x skin
        const V3 d{-1.f, 0.f, 0.f};                // its GI ray, straight out
        const Hit h = trace(s, skin, d, 1e-4f, 1.f, false);
        CHECK(h.hit && h.front && h.t < 0.0025f, "fixture sanity: the ear's GI ray does not meet its hull's front (t %g)", h.t);
        const V3 H = skin + d * h.t;
        const V3 N = h.front ? h.ng : h.ng * -1.f;  // (the hull's normals agree with its winding: toward the ear)
        const V3 earLamp{0.2f, 0.005f, 0.002f};     // a lamp 20 cm beyond the ear
        const V3 l = nrm(earLamp - H);
        const float nl = dot(N, l);
        const V3 o = H + (nl < 0.f ? N * -1.f : N) * 0.01f;   // (rtrProjRaySetup)
        const bool shadowedOld = blocked(s, o, earLamp, false);
        const bool truth = blocked(s, H + N * 1e-4f, earLamp, false);
        std::printf("firefly: hull hit %.1f mm off the ear, N.l %.2f toward a lamp the ear hides (true visibility %s); "
                    "its shadow ray from 1 cm along N: %s\n", h.t * 1000.f, nl, truth ? "shadowed" : "lit",
                    shadowedOld ? "shadowed" : "LIT (the leak)");
        CHECK(nl > 0.9f && truth && !shadowedOld, "fixture sanity: the ear hull leak no longer reproduces (N.l %g)", nl);
        // with the hull out of the scene the ear's GI ray leaves it
        const Hit he = trace(ear, skin, d, 1e-4f, 1.f, false);
        CHECK(!he.hit, "hull out: the ear's GI ray still hits something (t %g)", he.t);
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
