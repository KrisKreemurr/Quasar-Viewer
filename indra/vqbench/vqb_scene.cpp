// vqbench (#1039): the synthetic scene (vqb_scene.h).
#include "vqb_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// ---- tiny math (column-major mat4, as GLSL) ----
struct M4 { float m[16]; };
M4 mIdent() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r; }
M4 mMul(const M4& a, const M4& b)
{
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
        {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
bool mInv(const float* m, float* inv)
{
    float t[16];
    t[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    t[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    t[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    t[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    t[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    t[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    t[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    t[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    t[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    t[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    t[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    t[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    t[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    t[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    t[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    t[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0]*t[0]+m[1]*t[4]+m[2]*t[8]+m[3]*t[12];
    if (std::fabs(det) < 1e-30f) return false;
    det = 1.f / det;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] * det;
    return true;
}
VqbVec3 v3(float x, float y, float z) { VqbVec3 r; r.x = x; r.y = y; r.z = z; return r; }
VqbVec3 sub(VqbVec3 a, VqbVec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
VqbVec3 cross(VqbVec3 a, VqbVec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float dot(VqbVec3 a, VqbVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
VqbVec3 norm(VqbVec3 a) { const float l = std::sqrt(std::max(dot(a, a), 1e-30f)); return v3(a.x / l, a.y / l, a.z / l); }

// seeded PCG (reproducible: the report prints the seed)
struct Rng
{
    uint64_t s;
    explicit Rng(uint32_t seed) : s(0x853c49e6748fea9bull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull)) { next(); }
    uint32_t next()
    {
        const uint64_t o = s;
        s = o * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t x = (uint32_t)(((o >> 18u) ^ o) >> 27u), rot = (uint32_t)(o >> 59u);
        return (x >> rot) | (x << ((32u - rot) & 31u));
    }
    float u() { return (float)(next() >> 8) * (1.f / 16777216.f); }
    float r(float a, float b) { return a + (b - a) * u(); }
};

// ---- meshes (object space) ----
struct Vert { float p[3], n[3], uv[2]; };
struct Mesh { std::vector<Vert> v; std::vector<uint16_t> i; };

void addQuad(Mesh& m, VqbVec3 o, VqbVec3 a, VqbVec3 b, float u0, float v0, float u1, float v1)
{
    const VqbVec3 n = norm(cross(a, b));
    const uint16_t base = (uint16_t)m.v.size();
    const VqbVec3 c[4] = {o, v3(o.x + a.x, o.y + a.y, o.z + a.z), v3(o.x + a.x + b.x, o.y + a.y + b.y, o.z + a.z + b.z),
                          v3(o.x + b.x, o.y + b.y, o.z + b.z)};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (int k = 0; k < 4; ++k)
        m.v.push_back({{c[k].x, c[k].y, c[k].z}, {n.x, n.y, n.z}, {uv[k][0], uv[k][1]}});
    for (uint16_t k : {0, 1, 2, 0, 2, 3}) m.i.push_back((uint16_t)(base + k));
}
Mesh boxMesh()
{
    // unit box [-0.5, 0.5]^3, outward faces
    Mesh m;
    addQuad(m, v3(-.5f, -.5f, .5f), v3(1, 0, 0), v3(0, 1, 0), 0, 0, 1, 1);    // +z
    addQuad(m, v3(-.5f, .5f, -.5f), v3(1, 0, 0), v3(0, -1, 0), 0, 0, 1, 1);   // -z
    addQuad(m, v3(.5f, -.5f, -.5f), v3(0, 1, 0), v3(0, 0, 1), 0, 0, 1, 1);    // +x
    addQuad(m, v3(-.5f, .5f, -.5f), v3(0, -1, 0), v3(0, 0, 1), 0, 0, 1, 1);   // -x
    addQuad(m, v3(.5f, .5f, -.5f), v3(-1, 0, 0), v3(0, 0, 1), 0, 0, 1, 1);    // +y
    addQuad(m, v3(-.5f, -.5f, -.5f), v3(1, 0, 0), v3(0, 0, 1), 0, 0, 1, 1);   // -y
    return m;
}
Mesh groundMesh(Rng& rng, float size, int cells)
{
    Mesh m;
    std::vector<float> hgt((cells + 1) * (cells + 1));
    for (auto& h : hgt) h = rng.r(-0.15f, 0.15f);
    for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x)
        {
            const float px = -size * .5f + size * x / cells, py = -size * .5f + size * y / cells;
            m.v.push_back({{px, py, hgt[y * (cells + 1) + x]}, {0, 0, 1}, {x * 0.25f, y * 0.25f}});
        }
    for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x)
        {
            const uint16_t a = (uint16_t)(y * (cells + 1) + x), b = (uint16_t)(a + 1), c = (uint16_t)(a + cells + 1), d = (uint16_t)(c + 1);
            for (uint16_t k : {a, b, d, a, d, c}) m.i.push_back(k);
        }
    return m;
}
// a tree crown: CARDS alpha-masked cards around the trunk top (object space: the crown's centre at the origin)
Mesh crownMesh(Rng& rng, int cards)
{
    Mesh m;
    for (int c = 0; c < cards; ++c)
    {
        const float a = rng.r(0.f, 6.2831853f), e = rng.r(-0.6f, 0.9f), s = rng.r(1.2f, 2.2f);
        const VqbVec3 o = v3(rng.r(-1.6f, 1.6f), rng.r(-1.6f, 1.6f), rng.r(-1.2f, 1.4f));
        const VqbVec3 ax = v3(std::cos(a) * s, std::sin(a) * s, 0.f);
        const VqbVec3 ay = v3(-std::sin(a) * std::sin(e) * s, std::cos(a) * std::sin(e) * s, std::cos(e) * s);
        const int cell = (int)(rng.next() & 3);
        const float u0 = (cell & 1) * .5f, v0 = (cell >> 1) * .5f;
        addQuad(m, v3(o.x - ax.x * .5f - ay.x * .5f, o.y - ax.y * .5f - ay.y * .5f, o.z - ax.z * .5f - ay.z * .5f), ax, ay,
                u0, v0, u0 + .5f, v0 + .5f);
    }
    return m;
}
Mesh panelMesh(float u1, float v1)
{
    Mesh m;   // unit panel in the xz plane, [-0.5, 0.5] x [0, 1]
    addQuad(m, v3(-.5f, 0, 0), v3(1, 0, 0), v3(0, 0, 1), 0, 0, u1, v1);
    return m;
}

uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
int16_t q16(float x) { return (int16_t)std::max(-32767.f, std::min(32767.f, std::round(x))); }
uint32_t octSnorm(const float* n)
{
    float x = n[0], y = n[1], z = n[2];
    const float s = std::fabs(x) + std::fabs(y) + std::fabs(z);
    x /= s; y /= s; z /= s;
    if (z < 0.f)
    {
        const float ox = (1.f - std::fabs(y)) * (x >= 0.f ? 1.f : -1.f), oy = (1.f - std::fabs(x)) * (y >= 0.f ? 1.f : -1.f);
        x = ox; y = oy;
    }
    return (uint16_t)q16(x * 32767.f) | ((uint32_t)(uint16_t)q16(y * 32767.f) << 16);
}
float pow2Step(float halfExtent)
{
    // the lattice step: a power of two with halfExtent / step <= 32767 (clean_qvertex.glsl)
    float s = 1.f;
    const float want = std::max(halfExtent, 1e-6f) / 32767.f;
    while (s > want) s *= 0.5f;
    while (s < want) s *= 2.f;
    return s;
}

// one mesh slot of the static VBO: the decode header + the quantized vertices; returns the base vertex. The
// decoded positions (what the shaders reconstruct) are written back into the mesh (the BLAS is built from them).
uint32_t emitSlot(VqbScene& sc, Mesh& m, uint32_t& firstIndex)
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, ulo[2] = {1e30f, 1e30f}, uhi[2] = {-1e30f, -1e30f};
    for (const Vert& v : m.v)
    {
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.p[k]); hi[k] = std::max(hi[k], v.p[k]); }
        for (int k = 0; k < 2; ++k) { ulo[k] = std::min(ulo[k], v.uv[k]); uhi[k] = std::max(uhi[k], v.uv[k]); }
    }
    float pc[3], ps[3], uc[2], us[2];
    for (int k = 0; k < 3; ++k)
    {
        ps[k] = pow2Step((hi[k] - lo[k]) * .5f + 1e-4f);
        pc[k] = std::round((lo[k] + hi[k]) * .5f / ps[k]) * ps[k];
    }
    for (int k = 0; k < 2; ++k)
    {
        us[k] = pow2Step((uhi[k] - ulo[k]) * .5f + 1e-4f);
        uc[k] = std::round((ulo[k] + uhi[k]) * .5f / us[k]) * us[k];
    }
    // 2 header pseudo-vertices (10 words)
    const uint32_t hdr[10] = {f2u(pc[0]), f2u(pc[1]), f2u(pc[2]), f2u(ps[0]), f2u(ps[1]), f2u(ps[2]),
                              f2u(uc[0]), f2u(uc[1]), f2u(us[0]), f2u(us[1])};
    sc.vbo.insert(sc.vbo.end(), hdr, hdr + 10);
    sc.gbVerts.insert(sc.gbVerts.end(), 16, 0.f);
    const uint32_t base = (uint32_t)(sc.vbo.size() / 5);
    for (Vert& v : m.v)
    {
        int16_t q[3], qu[2];
        for (int k = 0; k < 3; ++k) { q[k] = q16((v.p[k] - pc[k]) / ps[k]); v.p[k] = pc[k] + q[k] * ps[k]; }
        for (int k = 0; k < 2; ++k) { qu[k] = q16((v.uv[k] - uc[k]) / us[k]); v.uv[k] = uc[k] + qu[k] * us[k]; }
        sc.vbo.push_back((uint16_t)q[0] | ((uint32_t)(uint16_t)q[1] << 16));
        sc.vbo.push_back((uint16_t)q[2]);   // (flags 0: no tangent)
        sc.vbo.push_back((uint16_t)qu[0] | ((uint32_t)(uint16_t)qu[1] << 16));
        sc.vbo.push_back(octSnorm(v.n));
        sc.vbo.push_back(0u);
        const float gv[8] = {v.p[0], v.p[1], v.p[2], v.n[0], v.n[1], v.n[2], v.uv[0], v.uv[1]};
        sc.gbVerts.insert(sc.gbVerts.end(), gv, gv + 8);
    }
    // 16-bit indices, 2 per word (every slot padded to a whole word: its first index is even)
    firstIndex = (uint32_t)sc.ibo.size() * 2u;
    std::vector<uint16_t> idx16(m.i.begin(), m.i.end());
    if ((idx16.size() & 1u) != 0u) idx16.push_back(0);
    for (size_t k = 0; k < idx16.size(); k += 2) sc.ibo.push_back(idx16[k] | ((uint32_t)idx16[k + 1] << 16));
    for (uint16_t i : m.i) sc.gbIdx.push_back(base + i);
    return base;
}

struct Slot { uint32_t base, firstIndex, indexCount, vertCount, gbFirst; Mesh mesh; };

M4 trs(VqbVec3 t, float rotZ, VqbVec3 s)
{
    M4 r = mIdent();
    const float c = std::cos(rotZ), sn = std::sin(rotZ);
    r.m[0] = c * s.x; r.m[1] = sn * s.x;
    r.m[4] = -sn * s.y; r.m[5] = c * s.y;
    r.m[10] = s.z;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

// texture texels (RGBA8, little-endian: r | g << 8 | b << 16 | a << 24)
uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
} // namespace

void VqbScene::generate(const VqbSceneCfg& c)
{
    cfg = c;
    Rng rng(c.seed);
    instances.clear(); materials.clear(); geom.clear(); geomVc.clear(); vbo.clear(); ibo.clear(); lights.clear();
    gbVerts.clear(); gbIdx.clear(); gbInst.clear();
    const VqbVec3 C = v3(128.f, 128.f, 22.f);   // the region's centre (agent space: the TLAS's absolute coordinates)

    // ---- materials (glTF PBR: flags 64; + 1 = base colour texture) ----
    auto mat = [&](float r, float g, float b, float a, float rough, float metal, float cutoff, uint32_t tex, uint32_t flags)
    {
        VqbMaterialRec m{};
        m.base_color_handle[0] = tex;
        m.base_color_factor[0] = r; m.base_color_factor[1] = g; m.base_color_factor[2] = b; m.base_color_factor[3] = a;
        m.spec_color_gloss[3] = 0.2f;
        m.metallic_factor = metal; m.roughness_factor = rough; m.alpha_cutoff = cutoff;
        m.flags = flags;
        materials.push_back(m);
        return (uint32_t)materials.size() - 1;
    };
    const uint32_t mGround = mat(0.32f, 0.36f, 0.24f, 1, 0.92f, 0, -1, 0, 64);
    uint32_t mWall[4];
    mWall[0] = mat(0.62f, 0.60f, 0.56f, 1, 0.8f, 0, -1, 0, 64);
    mWall[1] = mat(0.45f, 0.25f, 0.18f, 1, 0.7f, 0, -1, 0, 64);
    mWall[2] = mat(0.75f, 0.74f, 0.70f, 1, 0.4f, 0, -1, 0, 64);
    mWall[3] = mat(0.55f, 0.56f, 0.6f, 1, 0.3f, 0.8f, -1, 0, 64);
    const uint32_t mBark = mat(0.30f, 0.22f, 0.15f, 1, 0.9f, 0, -1, 0, 64);
    const uint32_t mLeaf = mat(0.55f, 0.75f, 0.40f, 1, 0.7f, 0, 0.5f, 1, 64 | 1);
    const uint32_t mFence = mat(0.40f, 0.38f, 0.35f, 1, 0.6f, 0.5f, 0.5f, 2, 64 | 1);
    const uint32_t mGlass = mat(0.85f, 0.95f, 1.0f, 0.55f, 0.1f, 0, -1, 3, 64 | 1);

    // ---- mesh slots ----
    std::vector<Slot> slots;
    auto slot = [&](Mesh m)
    {
        Slot s;
        s.gbFirst = (uint32_t)gbIdx.size();
        uint32_t fi = 0;
        s.base = emitSlot(*this, m, fi);
        s.firstIndex = fi;
        s.indexCount = (uint32_t)m.i.size();
        s.vertCount = (uint32_t)m.v.size();
        s.mesh = std::move(m);
        slots.push_back(std::move(s));
        return (uint32_t)slots.size() - 1;
    };
    const uint32_t sGround = slot(groundMesh(rng, 180.f, 48));
    const uint32_t sBox = slot(boxMesh());
    std::vector<uint32_t> sCrown;
    for (int k = 0; k < 6; ++k) sCrown.push_back(slot(crownMesh(rng, 56)));
    const uint32_t sFence = slot(panelMesh(4.f, 1.f));
    const uint32_t sGlass = slot(panelMesh(1.f, 1.f));

    // ---- instances (flags: 2 alpha blend, 4 alpha mask -- LLCleanInstanceStore::Instance) ----
    struct Pending { uint32_t slot, material, flags; M4 model; float tint[4]; };
    std::vector<Pending> pend;
    auto inst = [&](uint32_t s, uint32_t m, uint32_t fl, const M4& M, float a = 1.f)
    {
        Pending p{s, m, fl, M, {1, 1, 1, a}};
        pend.push_back(p);
    };
    inst(sGround, mGround, 0, trs(v3(C.x, C.y, C.z - 2.f), 0, v3(1, 1, 1)));
    for (uint32_t b = 0; b < c.buildings; ++b)
    {
        const float a = rng.r(0.f, 6.2831853f), d = rng.r(12.f, 70.f);
        const VqbVec3 s = v3(rng.r(3.f, 12.f), rng.r(3.f, 12.f), rng.r(3.f, 16.f));
        inst(sBox, mWall[b & 3], 0, trs(v3(C.x + std::cos(a) * d, C.y + std::sin(a) * d, C.z - 2.f + s.z * .5f), rng.r(0, 3.14f), s));
        // a roof slab overhanging on some (covered space: the GI / point lights under it)
        if ((b % 3) == 0)
            inst(sBox, mWall[(b + 1) & 3], 0, trs(v3(C.x + std::cos(a) * d, C.y + std::sin(a) * d, C.z - 2.f + s.z + 0.2f),
                                                  rng.r(0, 3.14f), v3(s.x * 1.6f, s.y * 1.6f, 0.4f)));
    }
    for (uint32_t t = 0; t < c.trees; ++t)
    {
        const VqbVec3 p = v3(C.x + rng.r(-60.f, 60.f), C.y + rng.r(-60.f, 60.f), C.z - 2.f);
        const float h = rng.r(3.5f, 7.f);
        inst(sBox, mBark, 0, trs(v3(p.x, p.y, p.z + h * .5f), 0, v3(0.35f, 0.35f, h)));
        inst(sCrown[t % sCrown.size()], mLeaf, 4, trs(v3(p.x, p.y, p.z + h + 0.8f), rng.r(0, 6.28f), v3(1, 1, 1)));
    }
    for (uint32_t f = 0; f < c.fences; ++f)
    {
        const VqbVec3 p = v3(C.x + rng.r(-45.f, 45.f), C.y + rng.r(-45.f, 45.f), C.z - 2.f);
        inst(sFence, mFence, 4, trs(p, rng.r(0, 3.14f), v3(rng.r(6.f, 14.f), 1, rng.r(1.2f, 2.5f))));
    }
    for (uint32_t g = 0; g < c.glass; ++g)
    {
        const VqbVec3 p = v3(C.x + rng.r(-40.f, 40.f), C.y + rng.r(-40.f, 40.f), C.z - 2.f + rng.r(0.f, 3.f));
        inst(sGlass, mGlass, 2, trs(p, rng.r(0, 3.14f), v3(rng.r(2.f, 6.f), 1, rng.r(2.f, 4.f))), 0.6f);
    }
    for (const Pending& p : pend)
    {
        const Slot& s = slots[p.slot];
        VqbInstanceRec r{};
        std::memcpy(r.model_matrix, p.model.m, 64);
        r.aabb_min[0] = r.aabb_min[1] = r.aabb_min[2] = -0.5f;
        r.aabb_max[0] = r.aabb_max[1] = r.aabb_max[2] = 0.5f;
        std::memcpy(r.tint, p.tint, 16);
        const M4 I = mIdent();
        std::memcpy(r.uv_matrix, I.m, 64);
        r.material_id = p.material;
        r.flags = p.flags;
        r.lod_group_id = p.slot;
        instances.push_back(r);
        const uint32_t g[4] = {s.firstIndex, s.base, 0u, s.indexCount};
        geom.insert(geom.end(), g, g + 4);
        const uint32_t vc[8] = {s.vertCount, 0, 0, 0, 0, 0, 0, 0};
        geomVc.insert(geomVc.end(), vc, vc + 8);
        const uint32_t texIdx = materials[p.material].base_color_handle[0];
        const uint32_t gi[8] = {s.gbFirst, s.base, p.material, p.flags, (materials[p.material].flags & 1u) ? texIdx : 0xFFFFFFFFu,
                                s.indexCount, 0, 0};
        gbInst.insert(gbInst.end(), gi, gi + 8);
        triCount += s.indexCount / 3;
        if (p.flags & 2u) ++blendInst; else if (p.flags & 4u) ++maskInst; else ++opaqueInst;
    }
    // (the BLASes: one per slot, from the decoded positions -- build())
    mSlotsForBuild.clear();
    for (const Slot& s : slots)
    {
        SlotBuild b;
        for (const Vert& v : s.mesh.v) b.pos.insert(b.pos.end(), v.p, v.p + 3);
        b.idx = s.mesh.i;
        mSlotsForBuild.push_back(std::move(b));
    }
    mInstSlot.clear();
    mInstModel.clear();
    for (const Pending& p : pend)
    {
        mInstSlot.push_back(p.slot);
        Mat4 m;
        std::memcpy(m.m, p.model.m, 64);
        mInstModel.push_back(m);
    }

    // ---- lights: point lights and projectors among the geometry, 0.5 .. 6 m up ----
    for (uint32_t i = 0; i < c.pointLights + c.projectors; ++i)
    {
        const bool proj = i >= c.pointLights;
        VqbLightRec L{};
        // (projectors: higher, wider and inside the camera's half of the scene -- stage-style lights whose
        //  footprints cover receivers the view sees, so pass 15 gets light samples)
        const VqbVec3 p = proj ? v3(C.x + rng.r(-30.f, 30.f), C.y + rng.r(-40.f, 15.f), C.z - 2.f + rng.r(3.f, 9.f))
                               : v3(C.x + rng.r(-50.f, 50.f), C.y + rng.r(-50.f, 50.f), C.z - 2.f + rng.r(0.6f, 6.f));
        const float radius = proj ? rng.r(12.f, 22.f) : rng.r(5.f, 14.f);
        L.pos_radius[0] = p.x; L.pos_radius[1] = p.y; L.pos_radius[2] = p.z; L.pos_radius[3] = radius;
        const float hue = rng.u();
        L.color_falloff[0] = 0.6f + 0.4f * std::cos(hue * 6.28f);
        L.color_falloff[1] = 0.6f + 0.4f * std::cos(hue * 6.28f - 2.1f);
        L.color_falloff[2] = 0.6f + 0.4f * std::cos(hue * 6.28f + 2.1f);
        L.color_falloff[3] = 1.0f;   // falloff
        L.proj_axis[0] = 0.12f;      // (point: the fixture's bounding radius -- rtrProjRaySetup's stop)
        if (proj)
        {
            const VqbVec3 aim = norm(v3(rng.r(-0.4f, 0.4f), rng.r(-0.4f, 0.4f), -1.f));
            L.proj_origin[0] = p.x; L.proj_origin[1] = p.y; L.proj_origin[2] = p.z; L.proj_origin[3] = 1.f;
            L.proj_axis[0] = aim.x; L.proj_axis[1] = aim.y; L.proj_axis[2] = aim.z; L.proj_axis[3] = 0.1f;
            L.proj_params[0] = 0.f; L.proj_params[1] = radius; L.proj_params[2] = 8.f; L.proj_params[3] = 0.f;
            // world -> projector texcoord: a 70-degree frustum along aim
            const VqbVec3 up = std::fabs(aim.z) > 0.9f ? v3(1, 0, 0) : v3(0, 0, 1);
            const VqbVec3 xa = norm(cross(aim, up)), ya = cross(xa, aim);
            M4 V = mIdent();
            V.m[0] = xa.x; V.m[4] = xa.y; V.m[8] = xa.z; V.m[12] = -dot(xa, p);
            V.m[1] = ya.x; V.m[5] = ya.y; V.m[9] = ya.z; V.m[13] = -dot(ya, p);
            V.m[2] = -aim.x; V.m[6] = -aim.y; V.m[10] = -aim.z; V.m[14] = dot(aim, p);
            const float f = 1.f / std::tan(0.96f);   // (a 110-degree cone)
            M4 P{};
            P.m[0] = f * .5f; P.m[5] = f * .5f; P.m[8] = -.5f; P.m[9] = -.5f; P.m[10] = -1.f; P.m[11] = -1.f; P.m[14] = -0.2f;
            const M4 T = mMul(P, V);
            std::memcpy(L.proj_mat, T.m, 64);
        }
        lights.push_back(L);
    }

    // ---- camera: a street-level view across the scene (z up), 60 degrees vertical ----
    eye = v3(C.x - 6.f, C.y - 52.f, C.z + 3.5f);
    const VqbVec3 at = v3(C.x + 4.f, C.y + 10.f, C.z);
    const VqbVec3 f = norm(sub(at, eye));
    const VqbVec3 r = norm(cross(f, v3(0, 0, 1)));
    const VqbVec3 u = cross(r, f);
    M4 V = mIdent();   // world -> view (camera-relative: no translation), -Z forward
    V.m[0] = r.x; V.m[4] = r.y; V.m[8] = r.z;
    V.m[1] = u.x; V.m[5] = u.y; V.m[9] = u.z;
    V.m[2] = -f.x; V.m[6] = -f.y; V.m[10] = -f.z;
    const float aspect = (float)c.width / (float)c.height, fy = 1.f / std::tan(fovY * .5f);
    // REVERSE-Z (the viewer's: near 1, far 0), Vulkan clip (y down)
    M4 P{};
    P.m[0] = fy / aspect;
    P.m[5] = -fy;
    P.m[10] = zNear / (zFar - zNear);
    P.m[11] = -1.f;
    P.m[14] = zNear * zFar / (zFar - zNear);
    const M4 VPr = mMul(P, V);
    std::memcpy(view, V.m, 64);
    std::memcpy(proj, P.m, 64);
    std::memcpy(vpRel, VPr.m, 64);
    mInv(vpRel, invVpRel);
    M4 Tr = mIdent();
    Tr.m[12] = -eye.x; Tr.m[13] = -eye.y; Tr.m[14] = -eye.z;
    const M4 VPa = mMul(VPr, Tr);
    std::memcpy(vpAbs, VPa.m, 64);
    mInv(vpAbs, invVpAbs);
    const float el = c.sunElevDeg * 0.0174533f, az = c.sunAzimDeg * 0.0174533f;
    sunDir = v3(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el));   // toward the sun
}

void VqbScene::build(VqbCtx& ctx)
{
    const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bInstances = ctx.bufferWith(instances.size() * sizeof(VqbInstanceRec), ssbo, instances.data(), instances.size() * sizeof(VqbInstanceRec));
    bMaterials = ctx.bufferWith(materials.size() * sizeof(VqbMaterialRec), ssbo, materials.data(), materials.size() * sizeof(VqbMaterialRec));
    bGeom = ctx.bufferWith(geom.size() * 4, ssbo, geom.data(), geom.size() * 4);
    bGeomVc = ctx.bufferWith(geomVc.size() * 4, ssbo, geomVc.data(), geomVc.size() * 4);
    bVbo = ctx.bufferWith(vbo.size() * 4, ssbo, vbo.data(), vbo.size() * 4);
    bIbo = ctx.bufferWith(ibo.size() * 4, ssbo, ibo.data(), ibo.size() * 4);
    bLights = ctx.bufferWith(std::max<size_t>(lights.size(), 1) * sizeof(VqbLightRec), ssbo, lights.data(), lights.size() * sizeof(VqbLightRec));
    bGbVerts = ctx.bufferWith(gbVerts.size() * 4, ssbo, gbVerts.data(), gbVerts.size() * 4);
    bGbIdx = ctx.bufferWith(gbIdx.size() * 4, ssbo, gbIdx.data(), gbIdx.size() * 4);
    bGbInst = ctx.bufferWith(gbInst.size() * 4, ssbo, gbInst.data(), gbInst.size() * 4);

    // ---- textures (the bindless table: 0 white, 1 leaf atlas, 2 fence lattice, 3 tinted glass) ----
    Rng rng(cfg.seed ^ 0x51ED);
    textures.push_back(ctx.texture2D(4, 4, std::vector<uint32_t>(16, rgba(255, 255, 255, 255)), true));
    {
        const uint32_t N = 512;
        std::vector<uint32_t> t(N * N, rgba(60, 90, 40, 0));
        for (int l = 0; l < 220; ++l)
        {
            const float cx = rng.r(0, (float)N), cy = rng.r(0, (float)N), ra = rng.r(6, 22), rb = ra * rng.r(0.3f, 0.6f), an = rng.r(0, 3.14f);
            const int g = (int)rng.r(90, 200);
            for (int y = (int)(cy - ra); y <= (int)(cy + ra); ++y)
                for (int x = (int)(cx - ra); x <= (int)(cx + ra); ++x)
                {
                    const float dx = x - cx, dy = y - cy, a = dx * std::cos(an) + dy * std::sin(an), b = -dx * std::sin(an) + dy * std::cos(an);
                    if ((a * a) / (ra * ra) + (b * b) / (rb * rb) <= 1.f)
                        t[((uint32_t)y % N) * N + ((uint32_t)x % N)] = rgba(g / 2, g, g / 3, 255);
                }
        }
        textures.push_back(ctx.texture2D(N, N, t, true));
    }
    {
        const uint32_t N = 128;
        std::vector<uint32_t> t(N * N);
        for (uint32_t y = 0; y < N; ++y)
            for (uint32_t x = 0; x < N; ++x)
            {
                const bool bar = (x % 32) < 5 || (y % 32) < 5 || ((x + y) % 45) < 3;
                t[y * N + x] = bar ? rgba(120, 115, 105, 255) : rgba(0, 0, 0, 0);
            }
        textures.push_back(ctx.texture2D(N, N, t, true));
    }
    {
        const uint32_t N = 64;
        std::vector<uint32_t> t(N * N);
        for (uint32_t k = 0; k < N * N; ++k) t[k] = rgba(170, 215, 235, (int)rng.r(70, 140));
        textures.push_back(ctx.texture2D(N, N, t, true));
    }

    // ---- BLASes (one per slot) ----
    std::vector<VqbBuffer> scratch;
    for (const SlotBuild& s : mSlotsForBuild)
    {
        VqbBuffer vb = ctx.bufferWith(s.pos.size() * 4, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, s.pos.data(), s.pos.size() * 4);
        VqbBuffer ib = ctx.bufferWith(s.idx.size() * 2 + 4, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, s.idx.data(), s.idx.size() * 2);
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = vb.addr;
        g.geometry.triangles.vertexStride = 12;
        g.geometry.triangles.maxVertex = (uint32_t)(s.pos.size() / 3) - 1;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT16;
        g.geometry.triangles.indexData.deviceAddress = ib.addr;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        const uint32_t prims = (uint32_t)(s.idx.size() / 3);
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        ctx.asSizes(ctx.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
        VqbAccel a;
        a.buf = ctx.buffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = a.buf.buf;
        ci.size = sz.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VQB_CK(ctx.createAS(ctx.dev, &ci, nullptr, &a.as));
        VqbBuffer sb = ctx.buffer(sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        bi.dstAccelerationStructure = a.as;
        bi.scratchData.deviceAddress = (sb.addr + 255) & ~(VkDeviceAddress)255;
        VkAccelerationStructureBuildRangeInfoKHR range{prims, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
        ctx.oneShot([&](VkCommandBuffer cmd) { ctx.cmdBuildAS(cmd, 1, &bi, &pr); });
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = a.as;
        a.addr = ctx.asAddr(ctx.dev, &ai);
        blas.push_back(a);
    }

    // ---- TLAS: custom index = the instance id, mask = its class, the opacity flags (clean_rt_instances.comp) ----
    std::vector<VkAccelerationStructureInstanceKHR> ti;
    for (size_t i = 0; i < instances.size(); ++i)
    {
        VkAccelerationStructureInstanceKHR t{};
        const float* m = mInstModel[i].m;
        for (int rr = 0; rr < 3; ++rr)
            for (int cc = 0; cc < 4; ++cc) t.transform.matrix[rr][cc] = m[cc * 4 + rr];
        const uint32_t f = instances[i].flags;
        const uint32_t cls = (f & 2u) ? 0x04u : (f & 4u) ? 0x02u : 0x01u;   // BLEND / ALPHAMASK / OPAQUE
        t.instanceCustomIndex = (uint32_t)i & 0xFFFFFFu;
        t.mask = cls;
        t.instanceShaderBindingTableRecordOffset = 0;
        const bool opaque = (f & 6u) == 0u;
        t.flags = (opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR)
                | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        t.accelerationStructureReference = blas[mInstSlot[i]].addr;
        ti.push_back(t);
    }
    VqbBuffer ib = ctx.bufferWith(ti.size() * sizeof(VkAccelerationStructureInstanceKHR),
                                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, ti.data(),
                                  ti.size() * sizeof(VkAccelerationStructureInstanceKHR));
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = ib.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &g;
    const uint32_t n = (uint32_t)ti.size();
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    ctx.asSizes(ctx.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sz);
    tlas.buf = ctx.buffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = tlas.buf.buf;
    ci.size = sz.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VQB_CK(ctx.createAS(ctx.dev, &ci, nullptr, &tlas.as));
    VqbBuffer sb = ctx.buffer(sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    bi.dstAccelerationStructure = tlas.as;
    bi.scratchData.deviceAddress = (sb.addr + 255) & ~(VkDeviceAddress)255;
    VkAccelerationStructureBuildRangeInfoKHR range{n, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
    ctx.oneShot([&](VkCommandBuffer cmd) { ctx.cmdBuildAS(cmd, 1, &bi, &pr); });
}
