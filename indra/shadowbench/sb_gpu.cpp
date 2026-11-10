// shadowbench -- GPU side (see sb_gpu.h).
#include "sb_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

namespace
{
constexpr uint32_t kBuckets = 5;          // opaque, mask, blend, ds opaque, ds mask
constexpr uint32_t kCap = 8192;           // commands per bucket
constexpr uint32_t kViews = 64;           // draw-view ring entries
constexpr uint32_t kQueries = 128;
constexpr uint32_t kCullViews = 16;

// ---- byte layouts (the viewer's) ----
struct Instance   // 224 B, LLCleanInstanceStore::Instance / clean_draw.vert
{
    float model[16]; float aabb_min[4]; float aabb_max[4]; float tint[4]; float uv[16];
    uint32_t lod, material, flags, skin; float spin_axis[4]; float spin_pivot[4];
};
static_assert(sizeof(Instance) == 224, "Instance");
struct VsInst     // 112 B, clean_vs_inst.glsl
{
    float m0[4], m1[4], m2[4]; float tint[4]; float uvlin[4]; float uvoff[2]; float bound_r; uint32_t aux;
    uint32_t material, flags, skin, pad;
};
static_assert(sizeof(VsInst) == 112, "VsInst");
struct Material   // 96 B
{
    uint32_t base[2], nrm[2], mr[2], emi[2]; float base_factor[4]; float emi_factor[4]; float spec[4];
    float metallic, roughness, alpha_cutoff; uint32_t flags;
};
static_assert(sizeof(Material) == 96, "Material");
struct DrawView   // 256 B, clean_draw_view.glsl
{
    float view_proj[16]; float sun_dir[4]; float sun_color[4]; float ambient[4]; float cam_time[4];
    float sky_h[4]; float sky_z[4]; float inv_vp[16]; float water_fog[4]; float origin[4];
};
static_assert(sizeof(DrawView) == 256, "DrawView");
struct SbObj
{
    float bmin[4]; float bmax[4]; uint32_t first_index, index_count; int32_t vertex_offset; uint32_t bucket_dyn;
};
static_assert(sizeof(SbObj) == 48, "SbObj");
struct SbView
{
    float planes[6][4]; float caster[12][4]; uint32_t n_caster, split; float min_size; uint32_t cmd_base;
};
static_assert(sizeof(SbView) == 304, "SbView");
// frame info UBO (clean_frame_info.glsl SSRBuf) offsets, from glslangValidator -q reflection
constexpr uint32_t kSsrSize = 8352;
constexpr uint32_t oAtmos = 32, oShadowMat = 384, oShadowClip = 640, oShadowParams = 656, oShadowTexel = 672;
constexpr uint32_t oSunRt = 4192, oRenderOpts = 4208, oCamOrigin = 4272, oShadowFit = 5008;
constexpr uint32_t kLinfoSize = 368;

// ---- mesh building ----
struct MeshData { std::vector<float> pos, uv, nrm; std::vector<uint16_t> idx; };
struct MeshSlot { uint32_t first_index, index_count; int32_t base_vertex; float bmin[3], bmax[3]; };

void addQuad(MeshData& m, const float* a, const float* b, const float* c, const float* d, const float* n)
{
    const uint16_t base = (uint16_t)(m.pos.size() / 3);
    const float* v[4] = {a, b, c, d};
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i)
    {
        m.pos.insert(m.pos.end(), v[i], v[i] + 3);
        m.uv.insert(m.uv.end(), uvs[i], uvs[i] + 2);
        m.nrm.insert(m.nrm.end(), n, n + 3);
    }
    const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
    for (uint16_t k : q) m.idx.push_back((uint16_t)(base + k));
}

// A (n+1) x (n+1) shared-vertex grid patch: P(u, v) for u, v in [0, 1], normal N(u, v).
template <class FP, class FN>
void addGrid(MeshData& m, int nu, int nv, FP P, FN N)
{
    const uint16_t base = (uint16_t)(m.pos.size() / 3);
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i)
        {
            float p[3], n[3];
            P((float)i / nu, (float)j / nv, p); N((float)i / nu, (float)j / nv, n);
            m.pos.insert(m.pos.end(), p, p + 3); m.nrm.insert(m.nrm.end(), n, n + 3);
            m.uv.push_back((float)i / nu); m.uv.push_back((float)j / nv);
        }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
        {
            const uint16_t a = (uint16_t)(base + j * (nu + 1) + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + nu + 2), d = (uint16_t)(a + nu + 1);
            const uint16_t q[6] = {a, b, c, a, c, d};
            for (uint16_t k : q) m.idx.push_back(k);
        }
}

MeshData meshBoxSub(int n)   // unit cube, every face an n x n grid (detailed mesh content)
{
    MeshData m;
    for (int ax = 0; ax < 3; ++ax)
        for (int sg = -1; sg <= 1; sg += 2)
        {
            const int u = (ax + 1) % 3, w = (ax + 2) % 3;
            addGrid(m, n, n,
                    [&](float a, float b, float* p) { p[ax] = 0.5f * sg; p[u] = a - 0.5f; p[w] = (b - 0.5f) * (float)sg; },
                    [&](float, float, float* nn) { nn[0] = nn[1] = nn[2] = 0.f; nn[ax] = (float)sg; });
        }
    return m;
}

MeshData meshBox()   // unit cube [-0.5, 0.5]^3, CCW outward
{
    MeshData m;
    for (int ax = 0; ax < 3; ++ax)
        for (int s = -1; s <= 1; s += 2)
        {
            float n[3] = {0, 0, 0}; n[ax] = (float)s;
            const int u = (ax + 1) % 3, w = (ax + 2) % 3;
            float c[4][3];
            const float su[4] = {-1, 1, 1, -1}, sw[4] = {-1, -1, 1, 1};
            for (int i = 0; i < 4; ++i)
            {
                c[i][ax] = 0.5f * s; c[i][u] = 0.5f * su[i]; c[i][w] = 0.5f * sw[i] * (float)s;
            }
            addQuad(m, c[0], c[1], c[2], c[3], n);
        }
    return m;
}

MeshData meshRoof()   // prism over [-0.5,0.5]^2, height 1 (ridge along x)
{
    MeshData m;
    const float A[3] = {-0.5f, -0.5f, 0.f}, B[3] = {0.5f, -0.5f, 0.f}, C[3] = {0.5f, 0.f, 1.f}, D[3] = {-0.5f, 0.f, 1.f};
    const float E[3] = {-0.5f, 0.5f, 0.f}, F[3] = {0.5f, 0.5f, 0.f};
    const float n1[3] = {0.f, -0.894f, 0.447f}, n2[3] = {0.f, 0.894f, 0.447f};
    addQuad(m, A, B, C, D, n1);
    addQuad(m, F, E, D, C, n2);
    const float nx[3] = {1, 0, 0}, nxm[3] = {-1, 0, 0};
    addQuad(m, B, F, C, C, nx);
    addQuad(m, E, A, D, D, nxm);
    return m;
}

MeshData meshCylinder(int seg)   // radius 0.5, z 0..1
{
    MeshData m;
    for (int i = 0; i < seg; ++i)
    {
        const float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg, am = 0.5f * (a0 + a1);
        const float p0[3] = {0.5f * std::cos(a0), 0.5f * std::sin(a0), 0.f}, p1[3] = {0.5f * std::cos(a1), 0.5f * std::sin(a1), 0.f};
        const float p2[3] = {p1[0], p1[1], 1.f}, p3[3] = {p0[0], p0[1], 1.f};
        const float n[3] = {std::cos(am), std::sin(am), 0.f};
        addQuad(m, p0, p1, p2, p3, n);
    }
    return m;
}

MeshData meshLeaves(int quads, uint32_t seed)   // cards inside the unit sphere (double-sided mask)
{
    MeshData m;
    std::mt19937 r(seed);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (int q = 0; q < quads; ++q)
    {
        float c[3] = {u(r) * 0.6f, u(r) * 0.6f, u(r) * 0.6f};
        float a[3] = {u(r), u(r), u(r)}; sbNorm(a);
        float b[3] = {u(r), u(r), u(r)};
        float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; sbNorm(n);
        float t[3] = {n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0]};
        const float s = 0.35f;
        float p[4][3];
        for (int k = 0; k < 3; ++k)
        {
            p[0][k] = c[k] - s * a[k] - s * t[k]; p[1][k] = c[k] + s * a[k] - s * t[k];
            p[2][k] = c[k] + s * a[k] + s * t[k]; p[3][k] = c[k] - s * a[k] + s * t[k];
        }
        addQuad(m, p[0], p[1], p[2], p[3], n);
    }
    return m;
}

MeshData meshEllipsoid(int rings, int segs)   // unit sphere (scaled per instance): avatar body
{
    MeshData m;
    auto P = [](float u, float v, float* o) {
        const float th = 3.14159265f * v, ph = 6.2831853f * u;
        o[0] = std::sin(th) * std::cos(ph); o[1] = std::sin(th) * std::sin(ph); o[2] = -std::cos(th);
    };
    addGrid(m, segs, rings, P, P);
    return m;
}

MeshData meshTerrain(float ox, float oy, float size, int grid)
{
    MeshData m;
    const float step = size / grid;
    for (int y = 0; y <= grid; ++y)
        for (int x = 0; x <= grid; ++x)
        {
            const float wx = ox + x * step, wy = oy + y * step, h = sbGround(wx, wy);
            const float hx = sbGround(wx + 0.5f, wy) - sbGround(wx - 0.5f, wy), hy = sbGround(wx, wy + 0.5f) - sbGround(wx, wy - 0.5f);
            float n[3] = {-hx, -hy, 1.f}; sbNorm(n);
            m.pos.push_back(x * step); m.pos.push_back(y * step); m.pos.push_back(h);
            m.uv.push_back((float)x / grid); m.uv.push_back((float)y / grid);
            m.nrm.insert(m.nrm.end(), n, n + 3);
        }
    for (int y = 0; y < grid; ++y)
        for (int x = 0; x < grid; ++x)
        {
            const uint16_t a = (uint16_t)(y * (grid + 1) + x), b = (uint16_t)(a + 1), c = (uint16_t)(a + grid + 2), d = (uint16_t)(a + grid + 1);
            const uint16_t q[6] = {a, b, c, a, c, d};
            for (uint16_t k : q) m.idx.push_back(k);
        }
    return m;
}

int16_t snorm16(float v) { return (int16_t)std::lround(std::clamp(v, -1.f, 1.f) * 32767.f); }
void octEncode(const float* n, int16_t& ox, int16_t& oy)
{
    const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.f)
    {
        const float tx = (1.f - std::fabs(y)) * (x >= 0.f ? 1.f : -1.f);
        const float ty = (1.f - std::fabs(x)) * (y >= 0.f ? 1.f : -1.f);
        x = tx; y = ty;
    }
    ox = snorm16(x); oy = snorm16(y);
}
float pow2Step(float extent)   // the lattice step: a power of two with |q| <= 32767
{
    float s = 1.f;
    while (extent / s > 32000.f) s *= 2.f;
    while (extent / s < 16000.f && s > 1e-8f) s *= 0.5f;
    return s;
}

// The quantized static stream (clean_qvertex.glsl): per slot 2 header pseudo-vertices, then
// 5 words per vertex. Returns the slot (base_vertex = its first real vertex).
MeshSlot appendMesh(std::vector<uint32_t>& words, std::vector<uint16_t>& indices, const MeshData& m)
{
    MeshSlot s{};
    const size_t nv = m.pos.size() / 3;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < nv; ++i)
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], m.pos[i * 3 + k]); mx[k] = std::max(mx[k], m.pos[i * 3 + k]); }
    float pc[3], ps[3];
    for (int k = 0; k < 3; ++k)
    {
        ps[k] = pow2Step(std::max(0.5f * (mx[k] - mn[k]), 1e-3f));
        pc[k] = std::round(0.5f * (mx[k] + mn[k]) / ps[k]) * ps[k];
        s.bmin[k] = mn[k]; s.bmax[k] = mx[k];
    }
    const float uc[2] = {0.5f, 0.5f}, us[2] = {pow2Step(0.5f), pow2Step(0.5f)};
    const uint32_t header_vert = (uint32_t)(words.size() / 5);
    float hdr[10] = {pc[0], pc[1], pc[2], ps[0], ps[1], ps[2], uc[0], uc[1], us[0], us[1]};
    for (float f : hdr) { uint32_t w; std::memcpy(&w, &f, 4); words.push_back(w); }
    s.base_vertex = (int32_t)(header_vert + 2);
    auto lo16 = [](int v) { return (uint32_t)(uint16_t)(int16_t)v; };
    for (size_t i = 0; i < nv; ++i)
    {
        int q[3];
        for (int k = 0; k < 3; ++k) q[k] = (int)std::lround((m.pos[i * 3 + k] - pc[k]) / ps[k]);
        const int qu = (int)std::lround((m.uv[i * 2] - uc[0]) / us[0]), qv = (int)std::lround((m.uv[i * 2 + 1] - uc[1]) / us[1]);
        int16_t nx, ny; octEncode(&m.nrm[i * 3], nx, ny);
        words.push_back(lo16(q[0]) | (lo16(q[1]) << 16));
        words.push_back(lo16(q[2]));                         // flags 0: no tangent
        words.push_back(lo16(qu) | (lo16(qv) << 16));
        words.push_back((uint32_t)(uint16_t)nx | ((uint32_t)(uint16_t)ny << 16));
        words.push_back(0u);
    }
    s.first_index = (uint32_t)indices.size();
    s.index_count = (uint32_t)m.idx.size();
    indices.insert(indices.end(), m.idx.begin(), m.idx.end());
    if (indices.size() & 1) indices.push_back(0);   // (keep the next slot 4-byte aligned)
    return s;
}

// ---- small matrix helpers (column-major) ----
void matMul(const float* a, const float* b, float* o)
{
    float t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    std::memcpy(o, t, sizeof(t));
}

enum ObjKind { OK_TERRAIN, OK_HOUSE, OK_ROOF, OK_TRUNK, OK_LEAVES, OK_PROP, OK_GLASS, OK_BODY, OK_HAIR, OK_MOVER };

struct Obj
{
    ObjKind kind;
    uint32_t mesh;
    uint32_t bucket;       // 0..4
    bool dyn;
    uint32_t material;
    uint32_t flags;        // instance flags (2 blend, 4 mask, 256 double-sided)
    float scale[3];
    float pos[3];
    float yaw;
    // motion (dynamic)
    float centre[3]; float radius; float speed; float phase;
};
}   // namespace

struct SbBench::Impl
{
    VkDeviceContext* ctx = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memprops{};
    float ts_period = 1.f;
    SbOptions opt;

    std::vector<MeshSlot> meshes;
    std::vector<Obj> objs;
    uint32_t n_obj = 0;

    GpuBuffer vbo, ibo, objBuf, instBuf, vsBuf, matBuf, jointBuf, rezBuf, viewBuf, ssrBuf, linfoBuf, cmdBuf, ctrBuf, cullViewBuf, ctrRb, recvRb;

    struct Img { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageView layer[4] = {}; };
    Img shadow, shadowStatic, transDepth, transColor, transDepthStatic, transColorStatic, refImg;
    Img gPos, gNrm, gDepth, gOut, leafTex, dummyArr, dummy2d;
    uint32_t transRes = 1024;
    VkSampler sCompare = VK_NULL_HANDLE, sRaw = VK_NULL_HANDLE, sLinear = VK_NULL_HANDLE, sTex = VK_NULL_HANDLE;

    VkDescriptorSetLayout dslDraw = VK_NULL_HANDLE, dslTex = VK_NULL_HANDLE, dslCull = VK_NULL_HANDLE, dslRecv = VK_NULL_HANDLE, dslRecv3 = VK_NULL_HANDLE, dslEmpty = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet dsDraw = VK_NULL_HANDLE, dsTex = VK_NULL_HANDLE, dsCull = VK_NULL_HANDLE, dsRecv = VK_NULL_HANDLE, dsRecv3 = VK_NULL_HANDLE, dsEmpty = VK_NULL_HANDLE;
    VkPipelineLayout plDraw = VK_NULL_HANDLE, plCull = VK_NULL_HANDLE, plRecv = VK_NULL_HANDLE;
    VkPipeline pDepth = VK_NULL_HANDLE, pMask = VK_NULL_HANDLE, pDs = VK_NULL_HANDLE, pMaskDs = VK_NULL_HANDLE;
    VkPipeline pTransDepth = VK_NULL_HANDLE, pTransColor = VK_NULL_HANDLE, pGbuf = VK_NULL_HANDLE, pCull = VK_NULL_HANDLE, pRecv = VK_NULL_HANDLE;
    VkQueryPool qpool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags props)
    {
        for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & props) == props) return i;
        std::fprintf(stderr, "no memory type\n"); std::abort();
    }
    Img makeImage(uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect)
    {
        Img im;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(dev, &ci, nullptr, &im.image));
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, im.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &im.mem));
        VK_CHECK(vkBindImageMemory(dev, im.image, im.mem, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.image; vi.format = fmt;
        vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (layers == 1 && (usage & VK_IMAGE_USAGE_SAMPLED_BIT) && aspect == VK_IMAGE_ASPECT_COLOR_BIT && w == 1) vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.subresourceRange = {aspect, 0, 1, 0, layers};
        VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &im.view));
        if (layers > 1)
            for (uint32_t l = 0; l < layers && l < 4; ++l)
            {
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.subresourceRange = {aspect, 0, 1, l, 1};
                VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &im.layer[l]));
            }
        return im;
    }
    GpuBuffer hostBuf(VkDeviceSize size, VkBufferUsageFlags usage)
    {
        return ctx->createBuffer(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkShaderModule loadSpv(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) { std::fprintf(stderr, "missing shader %s\n", path.c_str()); std::abort(); }
        std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = d.size(); ci.pCode = reinterpret_cast<const uint32_t*>(d.data());
        VkShaderModule m; VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
        return m;
    }
    void barrierImg(VkImage img, VkImageAspectFlags aspect, uint32_t layer0, uint32_t layers, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        b.oldLayout = from; b.newLayout = to; b.image = img; b.subresourceRange = {aspect, 0, 1, layer0, layers};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    void barrierMem(VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }

    void buildScene();
    void createResources();
    void createPipelines(const std::string& dir);
    void ensureShadowImages(uint32_t r);
    uint32_t shadowRes = 0;
    VkPipeline graphicsPipe(VkShaderModule vs, VkShaderModule fs, VkCullModeFlags cull, bool bias, bool depthTest, bool depthWrite,
                            uint32_t nColor, VkFormat colorFmt, bool colorWrite, bool mulBlend, VkCompareOp cmp, bool clamp);
    void updateDynamic(const SbScenario& sc, float t, const SbPose& cam);
};

// ---------------------------------------------------------------------------------------
void SbBench::Impl::buildScene()
{
    std::vector<uint32_t> words;
    std::vector<uint16_t> indices;
    // mesh 0 box, 1 roof, 2 trunk, 3..10 leaves variants, 11 body, 12 hair, 13.. terrain patches
    // (SL content is mesh-heavy: a house ~7k triangles, an avatar ~55k (mesh body + clothing))
    meshes.push_back(appendMesh(words, indices, meshBoxSub(3)));      // 0 props / movers / glass: 108 tris
    meshes.push_back(appendMesh(words, indices, meshRoof()));         // 1
    meshes.push_back(appendMesh(words, indices, meshCylinder(24)));   // 2 trunk
    for (uint32_t v = 0; v < 8; ++v) meshes.push_back(appendMesh(words, indices, meshLeaves(600, 100 + v)));   // 3..10: 1200 tris
    meshes.push_back(appendMesh(words, indices, meshEllipsoid(120, 200)));   // 11 avatar body: 48000 tris
    meshes.push_back(appendMesh(words, indices, meshLeaves(3000, 7)));       // 12 hair / clothing mask layer: 6000 tris
    meshes.push_back(appendMesh(words, indices, meshBoxSub(24)));            // 13 house walls: 6912 tris
    const uint32_t kHouse = 13;
    const uint32_t kTerrain0 = (uint32_t)meshes.size();
    const float tsize = 64.f;
    const int tgrid = 32;
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            meshes.push_back(appendMesh(words, indices, meshTerrain(-160.f + tx * tsize, -144.f + ty * tsize, tsize, tgrid)));

    std::mt19937 r(4242);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    auto add = [&](ObjKind k, uint32_t mesh, uint32_t bucket, bool dyn, uint32_t mat, uint32_t flags, float sx, float sy, float sz,
                   float x, float y, float z, float yaw) -> Obj& {
        Obj o{}; o.kind = k; o.mesh = mesh; o.bucket = bucket; o.dyn = dyn; o.material = mat; o.flags = flags;
        o.scale[0] = sx; o.scale[1] = sy; o.scale[2] = sz; o.pos[0] = x; o.pos[1] = y; o.pos[2] = z; o.yaw = yaw;
        objs.push_back(o);
        return objs.back();
    };
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            add(OK_TERRAIN, kTerrain0 + ty * 8 + tx, 0, false, 0, 0, 1, 1, 1, -160.f + tx * tsize, -144.f + ty * tsize, 0.f, 0.f);
    const float cx = 96.f, cy = 112.f;   // camera start (sb_scenario.h)
    auto place = [&](float spread, float avoid, float& x, float& y) {
        do { x = cx + (u(r) * 2.f - 1.f) * spread; y = cy + (u(r) * 2.f - 1.f) * spread; }
        while (std::hypot(x - cx, y - cy) < avoid);
    };
    // houses: walls + roof
    for (int i = 0; i < 140; ++i)
    {
        float x, y; place(220.f, 25.f, x, y);
        const float w = 6.f + 9.f * u(r), d = 6.f + 9.f * u(r), h = 3.f + 7.f * u(r), yaw = 6.2831853f * u(r);
        const float g = sbGround(x, y);
        add(OK_HOUSE, kHouse, 0, false, 0, 0, w, d, h, x, y, g + 0.5f * h - 0.3f, yaw);
        add(OK_ROOF, 1, 0, false, 0, 0, w * 1.05f, d * 1.1f, 2.f + 2.f * u(r), x, y, g + h - 0.3f, yaw);
    }
    // trees: trunk + leaf cards (double-sided alpha mask)
    for (int i = 0; i < 180; ++i)
    {
        float x, y; place(200.f, 8.f, x, y);
        const float h = 6.f + 8.f * u(r), g = sbGround(x, y);
        add(OK_TRUNK, 2, 0, false, 0, 0, 0.5f, 0.5f, h * 0.7f, x, y, g - 0.2f, 0.f);
        const float cr = 2.f + 2.5f * u(r);
        add(OK_LEAVES, 3 + (i % 8), 4, false, 1, 4u | 256u, cr, cr, cr * 1.2f, x, y, g + h * 0.75f, 6.2831853f * u(r));
    }
    // small props (the bulk of a region's instances): crates / benches / clutter
    for (int i = 0; i < 3000; ++i)
    {
        float x, y; place(230.f, 2.f, x, y);
        const float s = 0.3f + 1.2f * u(r);
        add(OK_PROP, 0, 0, false, 0, 0, s, s * (0.6f + u(r)), s * (0.5f + u(r)), x, y, sbGround(x, y) + 0.4f * s, 6.2831853f * u(r));
    }
    // static glass (alpha-blend: translucent shadows)
    for (int i = 0; i < 30; ++i)
    {
        float x, y; place(80.f, 10.f, x, y);
        add(OK_GLASS, 0, 2, false, 2, 2u, 4.f, 0.1f, 3.f, x, y, sbGround(x, y) + 2.f, 6.2831853f * u(r));
    }
    // moving glass panes (dynamic translucent casters: a bus window, a glass elevator)
    for (int i = 0; i < 4; ++i)
    {
        Obj& o = add(OK_GLASS, 0, 2, true, 2, 2u, 3.f, 0.1f, 2.5f, 0, 0, 0, 0);
        o.centre[0] = cx + 18.f + 4.f * i; o.centre[1] = cy + 6.f - 5.f * i; o.radius = 6.f; o.speed = 0.4f; o.phase = 1.3f * i;
    }
    // avatars: body (opaque) + hair / clothing (double-sided mask); the crowd mills around a
    // spot ~25 m ahead of the camera start
    for (int i = 0; i < 40; ++i)
    {
        const float acx = cx + 20.f + 10.f * (u(r) - 0.5f), acy = cy + 9.f + 10.f * (u(r) - 0.5f);
        const float rad = 1.f + 5.f * u(r), sp = 0.2f + 0.3f * u(r), ph = 6.2831853f * u(r);
        Obj& b = add(OK_BODY, 11, 0, true, 0, 0, 0.28f, 0.2f, 0.9f, 0, 0, 0, 0);
        b.centre[0] = acx; b.centre[1] = acy; b.radius = rad; b.speed = sp; b.phase = ph;
        Obj& hh = add(OK_HAIR, 12, 4, true, 3, 4u | 256u, 0.3f, 0.3f, 0.35f, 0, 0, 0, 0);
        hh.centre[0] = acx; hh.centre[1] = acy; hh.radius = rad; hh.speed = sp; hh.phase = ph;
    }
    // movers (vehicles)
    for (int i = 0; i < 10; ++i)
    {
        Obj& o = add(OK_MOVER, 0, 0, true, 0, 0, 4.f, 2.f, 1.5f, 0, 0, 0, 0);
        o.centre[0] = cx + 40.f * (u(r) - 0.5f) * 2.f; o.centre[1] = cy + 40.f * (u(r) - 0.5f) * 2.f;
        o.radius = 30.f; o.speed = 0.15f; o.phase = 6.2831853f * u(r);
    }
    n_obj = (uint32_t)objs.size();
    uint64_t tris = 0;
    for (const Obj& o : objs) tris += meshes[o.mesh].index_count / 3;
    std::printf("[sb] scene: %u objects, %.2f M triangles, %zu vertices\n", n_obj, tris / 1e6, words.size() / 5);

    vbo = ctx->createDeviceLocalWithData(words.data(), words.size() * 4,
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ibo = ctx->createDeviceLocalWithData(indices.data(), indices.size() * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void SbBench::Impl::updateDynamic(const SbScenario& sc, float t, const SbPose& cam)
{
    (void)cam;
    SbObj* ob = static_cast<SbObj*>(objBuf.mapped);
    Instance* in = static_cast<Instance*>(instBuf.mapped);
    VsInst* vs = static_cast<VsInst*>(vsBuf.mapped);
    const bool first = t <= 0.f;
    for (uint32_t i = 0; i < n_obj; ++i)
    {
        Obj& o = objs[i];
        if (!o.dyn && !first) continue;
        if (o.dyn)
        {
            const float tm = sc.crowd_moves ? t : 0.f;
            const float a = o.phase + o.speed * tm;
            float x = o.centre[0] + o.radius * std::cos(a), y = o.centre[1] + o.radius * std::sin(a);
            if (o.kind == OK_MOVER) { x = o.centre[0] + o.radius * std::sin(a); y = o.centre[1]; }
            const float g = sbGround(x, y);
            o.pos[0] = x; o.pos[1] = y;
            o.yaw = a + 1.5708f;
            if (o.kind == OK_BODY) o.pos[2] = g + 0.9f;
            else if (o.kind == OK_HAIR) o.pos[2] = g + 1.75f;
            else if (o.kind == OK_GLASS) o.pos[2] = g + 1.8f;
            else o.pos[2] = g + 0.75f;
        }
        const MeshSlot& ms = meshes[o.mesh];
        const float c = std::cos(o.yaw), s = std::sin(o.yaw);
        // model = T * Rz * S
        const float M[16] = {c * o.scale[0], s * o.scale[0], 0, 0, -s * o.scale[1], c * o.scale[1], 0, 0,
                             0, 0, o.scale[2], 0, o.pos[0], o.pos[1], o.pos[2], 1};
        // world AABB of the object box
        float wmn[3] = {1e30f, 1e30f, 1e30f}, wmx[3] = {-1e30f, -1e30f, -1e30f};
        for (int k = 0; k < 8; ++k)
        {
            const float p[3] = {(k & 1) ? ms.bmax[0] : ms.bmin[0], (k & 2) ? ms.bmax[1] : ms.bmin[1], (k & 4) ? ms.bmax[2] : ms.bmin[2]};
            for (int a2 = 0; a2 < 3; ++a2)
            {
                const float w = M[a2] * p[0] + M[4 + a2] * p[1] + M[8 + a2] * p[2] + M[12 + a2];
                wmn[a2] = std::min(wmn[a2], w); wmx[a2] = std::max(wmx[a2], w);
            }
        }
        SbObj& so = ob[i];
        for (int k = 0; k < 3; ++k) { so.bmin[k] = wmn[k]; so.bmax[k] = wmx[k]; }
        so.first_index = ms.first_index; so.index_count = ms.index_count; so.vertex_offset = ms.base_vertex;
        so.bucket_dyn = o.bucket | (o.dyn ? 256u : 0u);
        Instance& I = in[i];
        std::memset(&I, 0, sizeof(I));
        std::memcpy(I.model, M, sizeof(M));
        for (int k = 0; k < 3; ++k) { I.aabb_min[k] = ms.bmin[k]; I.aabb_max[k] = ms.bmax[k]; }
        I.tint[0] = I.tint[1] = I.tint[2] = I.tint[3] = 1.f;
        I.uv[0] = I.uv[5] = I.uv[10] = I.uv[15] = 1.f;
        I.material = o.material; I.flags = o.flags; I.skin = 0xFFFFFFFFu;
        VsInst& V = vs[i];
        std::memset(&V, 0, sizeof(V));
        for (int k = 0; k < 4; ++k) { V.m0[k] = M[k * 4 + 0]; V.m1[k] = M[k * 4 + 1]; V.m2[k] = M[k * 4 + 2]; }
        V.tint[0] = V.tint[1] = V.tint[2] = V.tint[3] = 1.f;
        V.uvlin[0] = 1.f; V.uvlin[3] = 1.f;
        float br = 0.f;
        for (int k = 0; k < 3; ++k) br += std::max(ms.bmin[k] * ms.bmin[k], ms.bmax[k] * ms.bmax[k]);
        V.bound_r = std::sqrt(br);
        V.material = o.material; V.flags = o.flags; V.skin = 0xFFFFFFFFu;
    }
}

void SbBench::Impl::createResources()
{
    objBuf  = hostBuf(sizeof(SbObj) * n_obj, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    instBuf = hostBuf(sizeof(Instance) * n_obj, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    vsBuf   = hostBuf(sizeof(VsInst) * n_obj, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    {
        Material mats[4]{};
        for (Material& m : mats) { m.base_factor[0] = m.base_factor[1] = m.base_factor[2] = 0.7f; m.base_factor[3] = 1.f; m.alpha_cutoff = 0.5f; m.roughness = 1.f; }
        mats[1].flags = 1u; mats[1].base[0] = 0;                                          // leaves: textured mask
        mats[2].base_factor[0] = 0.3f; mats[2].base_factor[1] = 0.6f; mats[2].base_factor[2] = 0.9f; mats[2].base_factor[3] = 0.4f;
        mats[2].alpha_cutoff = -1.f;                                                      // glass: alpha blend
        mats[3].flags = 1u; mats[3].base[0] = 0;                                          // hair: textured mask
        matBuf = ctx->createDeviceLocalWithData(mats, sizeof(mats), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    {
        std::vector<uint8_t> z(65536, 0);
        jointBuf = ctx->createDeviceLocalWithData(z.data(), z.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        rezBuf = ctx->createDeviceLocalWithData(z.data(), 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    viewBuf = hostBuf(sizeof(DrawView) * kViews, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ssrBuf = hostBuf(kSsrSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    linfoBuf = hostBuf(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::memset(ssrBuf.mapped, 0, kSsrSize);
    std::memset(linfoBuf.mapped, 0, 512);
    cmdBuf = ctx->createBuffer((VkDeviceSize)kBuckets * kCap * 20u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ctrBuf = ctx->createBuffer(64 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                               | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ctrRb = ctx->createBuffer(16 * 8 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    cullViewBuf = hostBuf(sizeof(SbView) * kCullViews, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    recvRb = ctx->createBuffer((VkDeviceSize)opt.recv_w * opt.recv_h * 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    const VkImageUsageFlags dUse = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags cUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    (void)dUse; (void)cUse;
    refImg = makeImage(opt.res, opt.res, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkImageUsageFlags gUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    gPos = makeImage(opt.recv_w, opt.recv_h, 1, VK_FORMAT_R32G32B32A32_SFLOAT, gUse, VK_IMAGE_ASPECT_COLOR_BIT);
    gNrm = makeImage(opt.recv_w, opt.recv_h, 1, VK_FORMAT_R32G32B32A32_SFLOAT, gUse, VK_IMAGE_ASPECT_COLOR_BIT);
    gDepth = makeImage(opt.recv_w, opt.recv_h, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    gOut = makeImage(opt.recv_w, opt.recv_h, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    leafTex = makeImage(256, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    dummyArr = makeImage(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy2d = makeImage(2, 2, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    // leaf alpha texture: blobs, ~50 % coverage
    std::vector<uint8_t> tex(256 * 256 * 4);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x)
        {
            const float fx = x / 256.f, fy = y / 256.f;
            const float v = std::sin(fx * 31.f) * std::cos(fy * 27.f) + 0.6f * std::sin((fx + fy) * 53.f);
            uint8_t* p = &tex[(y * 256 + x) * 4];
            p[0] = 60; p[1] = 140; p[2] = 50; p[3] = v > 0.f ? 255 : 0;
        }
    GpuBuffer stage = hostBuf(tex.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(stage.mapped, tex.data(), tex.size());
    ctx->submitImmediate([&](VkCommandBuffer c) {
        cmd = c;
        auto toDst = [&](VkImage im, VkImageAspectFlags a, uint32_t layers) {
            barrierImg(im, a, 0, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        };
        toDst(leafTex.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {256, 256, 1};
        vkCmdCopyBufferToImage(c, stage.buffer, leafTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        VkClearColorValue white{{1.f, 1.f, 1.f, 1.f}};
        VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toDst(dummyArr.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        vkCmdClearColorImage(c, dummyArr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &cr);
        toDst(dummy2d.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        vkCmdClearColorImage(c, dummy2d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &cr);
        for (VkImage im : {leafTex.image, dummyArr.image, dummy2d.image})
            barrierImg(im, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        for (VkImage im : {gPos.image, gNrm.image, gOut.image})
            barrierImg(im, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        for (VkImage im : {gDepth.image, refImg.image})
            barrierImg(im, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    ctx->destroyBuffer(stage);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE; si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &sCompare));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.compareEnable = VK_FALSE; si.compareOp = VK_COMPARE_OP_ALWAYS;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &sRaw));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &sLinear));
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &sTex));

    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kQueries;
    VK_CHECK(vkCreateQueryPool(dev, &qi, nullptr, &qpool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = ctx->commandPool(); ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(dev, &ca, &cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(dev, &fi, nullptr, &fence));
}

// The cascade arrays at map size r (the policy's border included): (re)created when a run
// needs another size; every layer starts cleared to 1 (nothing), sampled layers in the sampled
// layouts, static layers in TRANSFER_SRC (as the viewer's).
void SbBench::Impl::ensureShadowImages(uint32_t r)
{
    if (shadowRes == r) return;
    if (shadowRes)
    {
        vkDeviceWaitIdle(dev);
        for (Img* im : {&shadow, &shadowStatic, &transDepth, &transColor, &transDepthStatic, &transColorStatic})
        {
            for (VkImageView v : im->layer) if (v) vkDestroyImageView(dev, v, nullptr);
            vkDestroyImageView(dev, im->view, nullptr);
            vkDestroyImage(dev, im->image, nullptr);
            vkFreeMemory(dev, im->mem, nullptr);
            *im = Img{};
        }
    }
    shadowRes = r;
    const VkImageUsageFlags dUse = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags cUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    shadow = makeImage(r, r, 4, VK_FORMAT_D32_SFLOAT, dUse, VK_IMAGE_ASPECT_DEPTH_BIT);
    shadowStatic = makeImage(r, r, 4, VK_FORMAT_D32_SFLOAT, dUse, VK_IMAGE_ASPECT_DEPTH_BIT);
    transRes = std::max(r / 2u, 128u);
    transDepth = makeImage(transRes, transRes, 4, VK_FORMAT_D32_SFLOAT, dUse, VK_IMAGE_ASPECT_DEPTH_BIT);
    transColor = makeImage(transRes, transRes, 4, VK_FORMAT_R8G8B8A8_UNORM, cUse, VK_IMAGE_ASPECT_COLOR_BIT);
    transDepthStatic = makeImage(transRes, transRes, 4, VK_FORMAT_D32_SFLOAT, dUse, VK_IMAGE_ASPECT_DEPTH_BIT);
    transColorStatic = makeImage(transRes, transRes, 4, VK_FORMAT_R8G8B8A8_UNORM, cUse, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkCommandBuffer frameCmd = cmd;   // (barrierImg records into `cmd`)
    ctx->submitImmediate([&](VkCommandBuffer c) {
        cmd = c;
        auto toDst = [&](VkImage im, VkImageAspectFlags a) {
            barrierImg(im, a, 0, 4, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        };
        VkClearDepthStencilValue one{1.f, 0};
        VkClearColorValue white{{1.f, 1.f, 1.f, 1.f}};
        VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 4};
        for (VkImage im : {shadow.image, shadowStatic.image, transDepth.image, transDepthStatic.image})
        {
            toDst(im, VK_IMAGE_ASPECT_DEPTH_BIT);
            vkCmdClearDepthStencilImage(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &dr);
        }
        VkImageSubresourceRange cr4{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4};
        for (VkImage im : {transColor.image, transColorStatic.image})
        {
            toDst(im, VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdClearColorImage(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &cr4);
        }
        auto fin = [&](VkImage im, VkImageAspectFlags a, VkImageLayout to) {
            barrierImg(im, a, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        };
        fin(shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        fin(shadowStatic.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        fin(transDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        fin(transDepthStatic.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        fin(transColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        fin(transColorStatic.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });
    cmd = frameCmd;
    if (dsRecv)
    {
        VkDescriptorImageInfo ii[4] = {{sCompare, shadow.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {sRaw, shadow.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {sCompare, transDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {sLinear, transColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const uint32_t bind[4] = {15, 26, 27, 28};
        VkWriteDescriptorSet w[4]{};
        for (int k = 0; k < 4; ++k)
        {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = dsRecv; w[k].dstBinding = bind[k];
            w[k].descriptorCount = 1; w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &ii[k];
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
    }
}

VkPipeline SbBench::Impl::graphicsPipe(VkShaderModule vs, VkShaderModule fs, VkCullModeFlags cull, bool bias, bool depthTest,
                                       bool depthWrite, uint32_t nColor, VkFormat colorFmt, bool colorWrite, bool mulBlend,
                                       VkCompareOp cmp, bool clamp)
{
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    // vkWorldStaticVI: one stride-20 stream (clean_qvertex.glsl)
    VkVertexInputBindingDescription vb{0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0}, {1, 0, VK_FORMAT_R16G16_SINT, 8},
                                               {2, 0, VK_FORMAT_R16G16_SNORM, 12}, {3, 0, VK_FORMAT_R16G16_SNORM, 16}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = clamp ? VK_TRUE : VK_FALSE; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    rs.depthBiasEnable = bias ? VK_TRUE : VK_FALSE; rs.depthBiasConstantFactor = 2.f; rs.depthBiasSlopeFactor = 2.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depthTest; ds.depthWriteEnable = depthWrite; ds.depthCompareOp = cmp;
    VkPipelineColorBlendAttachmentState cb[2]{};
    for (uint32_t i = 0; i < nColor; ++i)
    {
        cb[i].colorWriteMask = colorWrite ? 0xF : 0;
        if (mulBlend)
        {
            cb[i].blendEnable = VK_TRUE;
            cb[i].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; cb[i].dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR; cb[i].colorBlendOp = VK_BLEND_OP_ADD;
            cb[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cb[i].alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = nColor; cbs.pAttachments = cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkFormat cf[2] = {colorFmt, colorFmt};
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = nColor; ri.pColorAttachmentFormats = cf; ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cbs; gi.pDynamicState = &dy; gi.layout = plDraw;
    VkPipeline p; VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &p));
    return p;
}

void SbBench::Impl::createPipelines(const std::string& dir)
{
    // ---- descriptor layouts ----
    auto layout = [&](std::vector<VkDescriptorSetLayoutBinding> b, bool bindless = false) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = (uint32_t)b.size(); ci.pBindings = b.data();
        VkDescriptorBindingFlags fl = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        if (bindless) { bf.bindingCount = 1; bf.pBindingFlags = &fl; ci.pNext = &bf; }
        VkDescriptorSetLayout l; VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &l));
        return l;
    };
    const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dslDraw = layout({{0, SB, 1, VF}, {1, SB, 1, VF}, {2, SB, 1, VF}, {9, UB, 1, VF}, {36, SB, 1, VF}, {39, SB, 1, VF},
                      {54, SB, 1, VF}, {58, SB, 1, VF}});
    dslTex = layout({{0, CIS, 16, VK_SHADER_STAGE_FRAGMENT_BIT}}, true);
    const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
    dslCull = layout({{0, SB, 1, C}, {1, SB, 1, C}, {2, SB, 1, C}, {3, SB, 1, C}});
    dslRecv = layout({{6, UB, 1, C}, {9, UB, 1, C}, {15, CIS, 1, C}, {26, CIS, 1, C}, {27, CIS, 1, C}, {28, CIS, 1, C},
                      {31, CIS, 1, C}, {34, CIS, 4, C}, {36, SB, 1, C}, {46, CIS, 1, C}});
    dslRecv3 = layout({{0, SI, 1, C}, {1, SI, 1, C}, {2, SI, 1, C}});
    dslEmpty = layout({});

    VkDescriptorPoolSize ps[4] = {{SB, 32}, {UB, 8}, {CIS, 40}, {SI, 8}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 8; pci.poolSizeCount = 4; pci.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &pool));
    auto alloc = [&](VkDescriptorSetLayout l, uint32_t var = 0) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
        VkDescriptorSetVariableDescriptorCountAllocateInfo vc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        if (var) { vc.descriptorSetCount = 1; vc.pDescriptorCounts = &var; ai.pNext = &vc; }
        VkDescriptorSet s; VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &s));
        return s;
    };
    dsDraw = alloc(dslDraw); dsTex = alloc(dslTex, 1); dsCull = alloc(dslCull); dsRecv = alloc(dslRecv); dsRecv3 = alloc(dslRecv3);
    dsEmpty = alloc(dslEmpty);

    ensureShadowImages(opt.res);
    std::vector<VkWriteDescriptorSet> w;
    std::vector<VkDescriptorBufferInfo> bis; bis.reserve(32);
    std::vector<VkDescriptorImageInfo> iis; iis.reserve(32);
    auto wb = [&](VkDescriptorSet s, uint32_t b, VkDescriptorType t, const GpuBuffer& buf, VkDeviceSize range = VK_WHOLE_SIZE) {
        bis.push_back({buf.buffer, 0, range});
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = s; x.dstBinding = b; x.descriptorCount = 1;
        x.descriptorType = t; x.pBufferInfo = &bis.back(); w.push_back(x);
    };
    auto wi = [&](VkDescriptorSet s, uint32_t b, uint32_t el, VkDescriptorType t, VkImageView v, VkSampler sm, VkImageLayout l) {
        iis.push_back({sm, v, l});
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = s; x.dstBinding = b; x.dstArrayElement = el;
        x.descriptorCount = 1; x.descriptorType = t; x.pImageInfo = &iis.back(); w.push_back(x);
    };
    wb(dsDraw, 0, SB, instBuf); wb(dsDraw, 1, SB, matBuf); wb(dsDraw, 2, SB, jointBuf); wb(dsDraw, 9, UB, ssrBuf, kSsrSize);
    wb(dsDraw, 36, SB, viewBuf); wb(dsDraw, 39, SB, vbo); wb(dsDraw, 54, SB, vsBuf); wb(dsDraw, 58, SB, rezBuf);
    wi(dsTex, 0, 0, CIS, leafTex.view, sTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wb(dsCull, 0, SB, objBuf); wb(dsCull, 1, SB, cmdBuf); wb(dsCull, 2, SB, ctrBuf); wb(dsCull, 3, SB, cullViewBuf);
    wb(dsRecv, 6, UB, linfoBuf, kLinfoSize); wb(dsRecv, 9, UB, ssrBuf, kSsrSize); wb(dsRecv, 36, SB, viewBuf);
    wi(dsRecv, 15, 0, CIS, shadow.view, sCompare, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    wi(dsRecv, 26, 0, CIS, shadow.view, sRaw, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    wi(dsRecv, 27, 0, CIS, transDepth.view, sCompare, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    wi(dsRecv, 28, 0, CIS, transColor.view, sLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wi(dsRecv, 31, 0, CIS, dummyArr.view, sLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (uint32_t k = 0; k < 4; ++k) wi(dsRecv, 34, k, CIS, dummyArr.view, sLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wi(dsRecv, 46, 0, CIS, dummy2d.view, sLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wi(dsRecv3, 0, 0, SI, gPos.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    wi(dsRecv3, 1, 0, SI, gNrm.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    wi(dsRecv3, 2, 0, SI, gOut.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    vkUpdateDescriptorSets(dev, (uint32_t)w.size(), w.data(), 0, nullptr);

    // ---- pipeline layouts ----
    {
        VkDescriptorSetLayout sl[2] = {dslDraw, dslTex};
        VkPushConstantRange pr{VF, 0, 16};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2; ci.pSetLayouts = sl; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pr;
        VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &plDraw));
    }
    {
        VkPushConstantRange pr{C, 0, 16};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1; ci.pSetLayouts = &dslCull; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pr;
        VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &plCull));
    }
    {
        VkDescriptorSetLayout sl[4] = {dslRecv, dslEmpty, dslEmpty, dslRecv3};
        VkPushConstantRange pr{C, 0, 16};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 4; ci.pSetLayouts = sl; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pr;
        VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &plRecv));
    }
    // ---- shader modules: the viewer's (vkclean/shaders, compiled as regen_world_spv.py does) ----
    VkShaderModule vsDepth = loadSpv(dir + "/kWorldVertDepthSpv.spv");
    VkShaderModule vsDepthMask = loadSpv(dir + "/kWorldVertDepthMaskSpv.spv");
    VkShaderModule fsMask = loadSpv(dir + "/kWorldShadowMaskFragSpv.spv");
    VkShaderModule fsTransDepth = loadSpv(dir + "/kWorldShadowTransDepthFragSpv.spv");
    VkShaderModule fsTransColor = loadSpv(dir + "/kWorldShadowTransColorFragSpv.spv");
    VkShaderModule vsG = loadSpv(dir + "/sb_gbuf.vert.spv"), fsG = loadSpv(dir + "/sb_gbuf.frag.spv");
    VkShaderModule csCull = loadSpv(dir + "/sb_cull.comp.spv"), csRecv = loadSpv(dir + "/sb_receiver.comp.spv");
    // The viewer's sun caster pipelines (LLCleanRendererVK: mShadowPipe / MaskPipe / DsPipe / MaskDsPipe):
    // D32, LEQUAL, slope bias 2 / 2, depth clamp, front = CCW, back-face cull (none for double-sided / alpha).
    const VkCompareOp LE = VK_COMPARE_OP_LESS_OR_EQUAL;
    pDepth  = graphicsPipe(vsDepth, VK_NULL_HANDLE, VK_CULL_MODE_BACK_BIT, true, true, true, 0, VK_FORMAT_UNDEFINED, false, false, LE, true);
    pMask   = graphicsPipe(vsDepthMask, fsMask, VK_CULL_MODE_BACK_BIT, true, true, true, 0, VK_FORMAT_UNDEFINED, false, false, LE, true);
    pDs     = graphicsPipe(vsDepth, VK_NULL_HANDLE, VK_CULL_MODE_NONE, true, true, true, 0, VK_FORMAT_UNDEFINED, false, false, LE, true);
    pMaskDs = graphicsPipe(vsDepthMask, fsMask, VK_CULL_MODE_NONE, true, true, true, 0, VK_FORMAT_UNDEFINED, false, false, LE, true);
    // translucency layer (#212): nearest translucent depth (no colour write), then transmittance multiply
    pTransDepth = graphicsPipe(vsDepthMask, fsTransDepth, VK_CULL_MODE_NONE, true, true, true, 1, VK_FORMAT_R8G8B8A8_UNORM, false, false, LE, true);
    pTransColor = graphicsPipe(vsDepthMask, fsTransColor, VK_CULL_MODE_NONE, false, false, false, 1, VK_FORMAT_R8G8B8A8_UNORM, true, true, LE, true);
    pGbuf = graphicsPipe(vsG, fsG, VK_CULL_MODE_NONE, false, true, true, 2, VK_FORMAT_R32G32B32A32_SFLOAT, true, false, VK_COMPARE_OP_LESS, false);
    auto comp = [&](VkShaderModule m, VkPipelineLayout l) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = l;
        VkPipeline p; VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
        return p;
    };
    pCull = comp(csCull, plCull);
    pRecv = comp(csRecv, plRecv);
    for (VkShaderModule m : {vsDepth, vsDepthMask, fsMask, fsTransDepth, fsTransColor, vsG, fsG, csCull, csRecv})
        vkDestroyShaderModule(dev, m, nullptr);
}

// ---------------------------------------------------------------------------------------
bool SbBench::init(VkDeviceContext& ctx, const std::string& shader_dir, const SbOptions& opt)
{
    m = new Impl();
    m->ctx = &ctx; m->dev = ctx.device(); m->opt = opt;
    vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice(), &m->memprops);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &pp);
    m->ts_period = pp.limits.timestampPeriod;
    mDeviceName = pp.deviceName;
    m->buildScene();
    m->createResources();
    m->createPipelines(shader_dir);
    return true;
}

void SbBench::shutdown()
{
    if (!m) return;
    vkDeviceWaitIdle(m->dev);
    delete m;   // (process exit releases the device objects)
    m = nullptr;
}

namespace
{
// main camera: VK clip (y down, depth 0..1), camera-relative view
void cameraViewProj(const SbPose& p, float aspect, float fovy, float zn, float zf, float* out)
{
    const float f = 1.f / std::tan(fovy * 0.5f);
    const float P[16] = {f / aspect, 0, 0, 0, 0, -f, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0};
    // view: x = right (-left), y = up, z = -at
    const float V[16] = {-p.left[0], p.up[0], -p.at[0], 0, -p.left[1], p.up[1], -p.at[1], 0, -p.left[2], p.up[2], -p.at[2], 0, 0, 0, 0, 1};
    matMul(P, V, out);
}

}   // namespace

SbRunResult SbBench::run(const SbScenario& sc, const SbPolicy& pol)
{
    Impl& I = *m;
    SbRunResult rr;
    const float fps = I.opt.fps;
    const float secs = I.opt.seconds > 0.f ? I.opt.seconds : sc.seconds;
    const int frames = (int)(secs * fps);
    VKShadowCacheEntry cache[VKSC_CASCADES];
    VKShadowOvlEntry ovl[VKSC_CASCADES];
    const bool cached = pol.mode != 0;
    // the border (cached cascades): an even number of texels per side, so the half-resolution
    // translucency layer keeps the texel grid too
    const uint32_t border = cached ? 2u * (uint32_t)std::lround(pol.border_frac * I.opt.res * 0.25f) : 0u;
    const uint32_t R = I.opt.res + 2u * border;
    I.ensureShadowImages(R);
    const VkDeviceSize zero = 0;
    std::mt19937 chg_rng(99);

    for (int f = 0; f < frames; ++f)
    {
        const float t = (float)f / fps;
        SbPose cam; sbCamera(sc, t, cam);
        float L[3]; sbSun(sc, t, L);
        I.updateDynamic(sc, t, cam);

        // ---- the viewer's fit + schedule ----
        VKShadowFitIn fi;
        std::memcpy(fi.eye, cam.eye, 12); std::memcpy(fi.at, cam.at, 12); std::memcpy(fi.left, cam.left, 12);
        std::memcpy(fi.up, cam.up, 12); std::memcpy(fi.light, L, 12);
        fi.fov_y = 1.0472f; fi.aspect = (float)I.opt.recv_w / (float)I.opt.recv_h; fi.near_clip = 0.1f;
        fi.range = 256.f; fi.render_far = 256.f; fi.res = I.opt.res; fi.border = border;
        if (cached) { fi.scale[0] = pol.scale0; fi.scale[1] = fi.scale[2] = fi.scale[3] = pol.scale_n; }
        VKShadowFitOut fo; vkShadowFitCascades(fi, fo);
        VKShadowSchedIn si;
        si.frame = (uint32_t)f + 1u; si.mode = pol.mode; si.event_cache = cached; si.overlay = cached;
        si.light_tx = pol.light_tx; si.drift_roll = pol.roll; si.far_n = pol.far_n; si.defer_max = pol.defer;
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            const float* M = fo.mat[c];
            const float Rr = 1.f / std::sqrt(M[0] * M[0] + M[4] * M[4] + M[8] * M[8]);
            si.lever[c] = pol.lever == 1 ? 2.f * Rr : (pol.lever == 2 ? 2.f * Rr + 64.f : 0.f);
        }
        si.mat = fo.mat; si.slice = fo.slice; si.texel = fo.texel; std::memcpy(si.eye, cam.eye, 12);
        // a static change somewhere every 3 s (a door, a prim edit): the cache re-renders what it touches
        float sph[4];
        uint8_t tag = 0;
        if (f > 0 && f % (int)(3.f * fps) == 0)
        {
            std::uniform_real_distribution<float> u(-120.f, 120.f);
            sph[0] = 96.f + u(chg_rng); sph[1] = 112.f + u(chg_rng); sph[2] = 25.f; sph[3] = 4.f;
            si.spheres = sph; si.n_spheres = 1; si.sphere_tags = &tag; si.n_tags = 1;
        }
        if (pol.force_all)   // (structure only: every cascade re-rendered and overlaid every frame)
            for (uint32_t c = 0; c < VKSC_CASCADES; ++c) cache[c].valid = false;
        VKShadowSchedOut so; vkShadowSchedule(cache, ovl, si, so);
        bool ovl_draw[VKSC_CASCADES] = {};
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            if (so.render[c]) ++rr.renders[c];
            ovl_draw[c] = cached && cache[c].valid && so.ovl_refresh[c];
            if (ovl_draw[c]) { ++rr.overlays[c]; ovl[c].valid = true; ovl[c].frame = si.frame; ovl[c].src = cache[c].frame; }
        }

        // ---- per-frame data ----
        DrawView* views = static_cast<DrawView*>(I.viewBuf.mapped);
        std::memset(views, 0, sizeof(DrawView) * 8);
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            std::memcpy(views[c].view_proj, so.mat[c], 64);   // camera-relative (#555)
            std::memcpy(views[c].origin, cam.eye, 12);
            std::memcpy(views[c].cam_time, cam.eye, 12); views[c].cam_time[3] = t;
        }
        float vp[16];
        cameraViewProj(cam, fi.aspect, fi.fov_y, 0.1f, 512.f, vp);
        std::memcpy(views[4].view_proj, vp, 64); std::memcpy(views[4].origin, cam.eye, 12);
        std::memcpy(views[4].cam_time, cam.eye, 12); views[4].cam_time[3] = t;
        SbView* cv = static_cast<SbView*>(I.cullViewBuf.mapped);
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            SbView& v = cv[c];
            std::memset(&v, 0, sizeof(v));
            std::memcpy(v.planes, fo.planes[c], sizeof(v.planes));   // this frame's box (mShadowPlanes)
            std::memcpy(v.caster, fo.caster_planes[c], sizeof(float) * 24);
            v.n_caster = 6;
            if (pol.swept)   // + the swept volume's silhouette planes (the viewer's av_vis silhouette tail)
            {
                std::memcpy(v.caster[6], fo.sil_planes[c], sizeof(float) * 24);
                v.n_caster = 12;
            }
            v.min_size = fo.texel[c] * 1.f;   // RenderVKShadowMinCasterTexels 1 (this frame's texel, fillShadowCasterTail)
            v.split = cached ? 1u : 0u;
            cv[4 + c] = v; cv[4 + c].split = 2u;   // the overlay's dynamic cull of cascade c
            // A CACHED static layer serves every later slice that still fits the map (the
            // coverage test), not just this frame's: its casters are culled by the map's box
            // alone (+ the sub-texel test) -- the slice's caster planes would drop casters a
            // later slice needs. (pol.static_slice_planes: the old cull, for the A/B.)
            if (cached && !pol.static_slice_planes) v.n_caster = 0;
        }
        // frame info (the lit kernels' view of the cascades)
        uint8_t* ssr = static_cast<uint8_t*>(I.ssrBuf.mapped);
        std::memcpy(ssr + oAtmos + 7 * 16, L, 12);
        std::memcpy(ssr + oShadowMat, so.mat, sizeof(so.mat));        // relative to this eye = cam_origin
        std::memcpy(ssr + oShadowClip, fo.clip, 16);
        const float params[4] = {0.f, 0.01f, (float)R, 1.f};
        std::memcpy(ssr + oShadowParams, params, 16);
        std::memcpy(ssr + oShadowTexel, so.texel, 16);
        const float opts[4] = {(float)I.opt.filter, 0.f, 1.f, 0.f};   // render_opts: filter, -, translucent shadows on
        std::memcpy(ssr + oRenderOpts, opts, 16);
        const float org[4] = {cam.eye[0], cam.eye[1], cam.eye[2], 0.f};
        std::memcpy(ssr + oCamOrigin, org, 16);
        const float fit[4] = {1.f, 0.5f, 2.f, 0.f};
        std::memcpy(ssr + oShadowFit, fit, 16);
        const float sunrt[4] = {0.00465f, 0, 0, 0};
        std::memcpy(ssr + oSunRt, sunrt, 16);
        uint8_t* li = static_cast<uint8_t*>(I.linfoBuf.mapped);
        std::memcpy(li, org, 16);
        const float fwd[4] = {cam.at[0], cam.at[1], cam.at[2], 0.f};
        std::memcpy(li + 16, fwd, 16);

        const bool sample = (f % I.opt.sample_every) == I.opt.sample_every - 1;

        // ---- record ----
        VkCommandBuffer cmd = I.cmd;
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        vkCmdResetQueryPool(cmd, I.qpool, 0, kQueries);
        uint32_t q = 0;
        std::vector<int> qcat;   // category of the interval ending at query q
        auto ts = [&](int cat) { vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, I.qpool, q++); qcat.push_back(cat); };
        enum { C_NONE, C_REF, C_CULL, C_DRAW, C_TRANS, C_COPY, C_DYNCULL, C_OVL, C_MISC, C_RECV };
        ts(C_NONE);
        // reference pass: a fixed 2048^2 depth clear (contention gauge)
        {
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = I.refImg.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = {1.f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {I.opt.res, I.opt.res}}; ri.layerCount = 1; ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri); vkCmdEndRendering(cmd);
            ts(C_REF);
        }
        auto setVp = [&](uint32_t w, uint32_t h) {
            VkViewport v{0, 0, (float)w, (float)h, 0, 1}; VkRect2D s{{0, 0}, {w, h}};
            vkCmdSetViewport(cmd, 0, 1, &v); vkCmdSetScissor(cmd, 0, 1, &s);
        };
        auto bindDraw = [&](uint32_t view) {
            VkDescriptorSet sets[2] = {I.dsDraw, I.dsTex};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.plDraw, 0, 2, sets, 0, nullptr);
            const uint32_t pc[4] = {view, 0, 0, 0};
            vkCmdPushConstants(cmd, I.plDraw, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
            vkCmdBindVertexBuffers(cmd, 0, 1, &I.vbo.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, I.ibo.buffer, 0, VK_INDEX_TYPE_UINT16);
        };
        auto bucket = [&](VkPipeline p, uint32_t b) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            vkCmdDrawIndexedIndirectCount(cmd, I.cmdBuf.buffer, (VkDeviceSize)b * kCap * 20u, I.ctrBuf.buffer, b * 4u, kCap, 20);
        };
        // the cull of one view into the shared buckets / counters (as the viewer: prior indirect
        // reads -> clear + cull; cull writes -> indirect reads)
        auto cull = [&](uint32_t view, uint32_t rbslot) {
            I.barrierMem(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vkCmdFillBuffer(cmd, I.ctrBuf.buffer, 0, 8 * 4, 0);
            I.barrierMem(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.pCull);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.plCull, 0, 1, &I.dsCull, 0, nullptr);
            const uint32_t pc[4] = {view, I.n_obj, kCap, 0};
            vkCmdPushConstants(cmd, I.plCull, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
            vkCmdDispatch(cmd, (I.n_obj + 63) / 64, 1, 1);
            I.barrierMem(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
            if (sample && rbslot < 16)
            {
                VkBufferCopy bc{0, (VkDeviceSize)rbslot * 32u, 32u};
                vkCmdCopyBuffer(cmd, I.ctrBuf.buffer, I.ctrRb.buffer, 1, &bc);
            }
        };
        bool rb_used[16] = {};

        // ===== recordShadows replica =====
        I.barrierImg(I.shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        I.barrierImg(I.transDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        I.barrierImg(I.transColor.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        bool any_render = false;
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c) any_render |= so.render[c];
        const bool trans_static_layer = cached && pol.trans_ovl;   // static translucency in its own layers
        if (cached && any_render)
        {
            I.barrierImg(I.shadowStatic.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            if (trans_static_layer)
            {
                I.barrierImg(I.transDepthStatic.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
                I.barrierImg(I.transColorStatic.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
            }
        }
        auto transPass = [&](uint32_t c, VkImageView depthView, VkImageView colorView, bool load, uint32_t view) {
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = colorView; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ca.clearValue.color = {{1.f, 1.f, 1.f, 1.f}};
            VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ta.imageView = depthView; ta.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            ta.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ta.clearValue.depthStencil = {1.f, 0};
            VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            tri.renderArea = {{0, 0}, {I.transRes, I.transRes}}; tri.layerCount = 1;
            tri.colorAttachmentCount = 1; tri.pColorAttachments = &ca; tri.pDepthAttachment = &ta;
            vkCmdBeginRendering(cmd, &tri);
            setVp(I.transRes, I.transRes);
            bindDraw(view);
            bucket(I.pTransDepth, 2);
            bucket(I.pTransColor, 2);
            vkCmdEndRendering(cmd);
            (void)c;
        };
        uint32_t n_blend_static = 0, n_blend_dyn = 0;
        for (const Obj& o : I.objs) { if (o.bucket == 2) (o.dyn ? n_blend_dyn : n_blend_static)++; }
        for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
        {
            if (!so.render[c]) continue;
            cull(c, c);
            rb_used[c] = sample;
            ts(C_CULL);
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = cached ? I.shadowStatic.layer[c] : I.shadow.layer[c];
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = {1.f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {R, R}}; ri.layerCount = 1; ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
            setVp(R, R);
            bindDraw(c);
            bucket(I.pDepth, 0);      // opaque
            bucket(I.pMask, 1);       // alpha mask
            bucket(I.pDs, 3);         // double-sided opaque
            bucket(I.pMaskDs, 4);     // double-sided mask
            vkCmdEndRendering(cmd);
            ts(C_DRAW);
            // translucency layer: the blend bucket of this cull (static only when cached)
            const bool skip = pol.trans_skip && (cached ? n_blend_static : n_blend_static + n_blend_dyn) == 0;
            if (!skip)
            {
                if (trans_static_layer) transPass(c, I.transDepthStatic.layer[c], I.transColorStatic.layer[c], false, c);
                else transPass(c, I.transDepth.layer[c], I.transColor.layer[c], false, c);
                ts(C_TRANS);
            }
        }
        if (cached)
        {
            if (any_render)
                I.barrierImg(I.shadowStatic.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            I.barrierImg(I.shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy regions[VKSC_CASCADES]{};
            uint32_t nr = 0;
            for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
            {
                if (!ovl_draw[c]) continue;
                VkImageCopy& r = regions[nr++];
                r.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, c, 1}; r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, c, 1};
                r.extent = {R, R, 1};
            }
            if (nr) vkCmdCopyImage(cmd, I.shadowStatic.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, I.shadow.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nr, regions);
            I.barrierImg(I.shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            if (trans_static_layer)
            {
                // the translucency layers the same way: static -> sampled, then this frame's
                // dynamic translucent casters on top
                if (any_render)
                {
                    I.barrierImg(I.transDepthStatic.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                    I.barrierImg(I.transColorStatic.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                }
                I.barrierImg(I.transDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                I.barrierImg(I.transColor.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkImageCopy tr[VKSC_CASCADES]{};
                uint32_t nt = 0;
                for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
                {
                    if (!ovl_draw[c]) continue;
                    VkImageCopy& r = tr[nt++];
                    r.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, c, 1}; r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, c, 1};
                    r.extent = {I.transRes, I.transRes, 1};
                }
                if (nt)
                {
                    vkCmdCopyImage(cmd, I.transDepthStatic.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, I.transDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nt, tr);
                    for (uint32_t k = 0; k < nt; ++k) { tr[k].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; tr[k].dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; }
                    vkCmdCopyImage(cmd, I.transColorStatic.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, I.transColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nt, tr);
                }
                I.barrierImg(I.transDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
                I.barrierImg(I.transColor.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
            }
            ts(C_COPY);
            for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
            {
                if (!ovl_draw[c]) continue;
                cull(4 + c, 4 + c);
                rb_used[4 + c] = sample;
                ts(C_DYNCULL);
                VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                da.imageView = I.shadow.layer[c]; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {R, R}}; ri.layerCount = 1; ri.pDepthAttachment = &da;
                vkCmdBeginRendering(cmd, &ri);
                setVp(R, R);
                bindDraw(c);
                bucket(I.pDepth, 0);
                bucket(I.pMask, 1);
                if (!pol.trans_ovl) bucket(I.pMaskDs, 2);   // current: dynamic alpha-blend casters alpha-tested
                bucket(I.pDs, 3);
                bucket(I.pMaskDs, 4);
                vkCmdEndRendering(cmd);
                if (pol.trans_ovl && (!pol.trans_skip || n_blend_dyn))
                    transPass(c, I.transDepth.layer[c], I.transColor.layer[c], true, c);
                ts(C_OVL);
            }
        }
        // depth writes -> sampling
        I.barrierImg(I.shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        I.barrierImg(I.transDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        I.barrierImg(I.transColor.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        ts(C_MISC);

        // ===== receivers: G-buffer + the viewer's sunShadowT =====
        {
            VkRenderingAttachmentInfo ca[2]{};
            const Impl::Img* gi[2] = {&I.gPos, &I.gNrm};
            for (int k = 0; k < 2; ++k)
            {
                ca[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[k].imageView = gi[k]->view; ca[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                ca[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE; ca[k].clearValue.color = {{0, 0, 0, 0}};
            }
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = I.gDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; da.clearValue.depthStencil = {1.f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {I.opt.recv_w, I.opt.recv_h}}; ri.layerCount = 1;
            ri.colorAttachmentCount = 2; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
            setVp(I.opt.recv_w, I.opt.recv_h);
            bindDraw(4);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.pGbuf);
            for (uint32_t i = 0; i < I.n_obj; ++i)
            {
                if (I.objs[i].bucket == 2) continue;   // (alpha-blend surfaces are not G-buffer receivers)
                const MeshSlot& ms = I.meshes[I.objs[i].mesh];
                vkCmdDrawIndexed(cmd, ms.index_count, 1, ms.first_index, ms.base_vertex, i);
            }
            vkCmdEndRendering(cmd);
            I.barrierMem(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            ts(C_NONE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.pRecv);
            VkDescriptorSet rs[4] = {I.dsRecv, I.dsEmpty, I.dsEmpty, I.dsRecv3};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.plRecv, 0, 4, rs, 0, nullptr);
            const uint32_t pc[4] = {4u, 0u, I.opt.recv_w, I.opt.recv_h};
            vkCmdPushConstants(cmd, I.plRecv, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
            vkCmdDispatch(cmd, (I.opt.recv_w + 7) / 8, (I.opt.recv_h + 7) / 8, 1);
            ts(C_RECV);
            if (sample)
            {
                I.barrierMem(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                I.barrierImg(I.gOut.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {I.opt.recv_w, I.opt.recv_h, 1};
                vkCmdCopyImageToBuffer(cmd, I.gOut.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, I.recvRb.buffer, 1, &bc);
                I.barrierImg(I.gOut.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            }
            // the G-buffer images go back to attachments next frame (GENERAL throughout)
            I.barrierMem(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        I.barrierMem(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub.commandBufferCount = 1; sub.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(I.ctx->queue(), 1, &sub, I.fence));
        VK_CHECK(vkWaitForFences(I.dev, 1, &I.fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(I.dev, 1, &I.fence));

        // ---- results ----
        std::vector<uint64_t> tv(q);
        VK_CHECK(vkGetQueryPoolResults(I.dev, I.qpool, 0, q, q * 8, tv.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        SbPassTimes pt;
        for (uint32_t k = 1; k < q; ++k)
        {
            const double ms = (double)(tv[k] - tv[k - 1]) * I.ts_period * 1e-6;
            switch (qcat[k])
            {
            case C_REF: pt.ref += ms; break;
            case C_CULL: pt.cull += ms; break;
            case C_DRAW: pt.draw += ms; break;
            case C_TRANS: pt.trans += ms; break;
            case C_COPY: pt.copy += ms; break;
            case C_DYNCULL: pt.dyncull += ms; break;
            case C_OVL: pt.ovl += ms; break;
            case C_RECV: pt.receiver += ms; break;
            default: break;
            }
            if (qcat[k] >= C_CULL && qcat[k] <= C_MISC) pt.shadow_total += ms;
        }
        rr.frames.push_back(pt);
        if (sample)
        {
            const uint32_t* ctr = static_cast<const uint32_t*>(I.ctrRb.mapped);
            for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
            {
                // the draws that ended in cascade c this frame: its static cull (if re-rendered) + its overlay cull
                uint64_t n = 0; bool any = false;
                for (uint32_t s : {c, 4 + c})
                    if (rb_used[s]) { any = true; for (uint32_t b = 0; b < kBuckets; ++b) n += std::min(ctr[s * 8 + b], kCap); }
                if (any && (!cached || ovl_draw[c] || so.render[c])) { rr.draws[c] += n; ++rr.draw_samples[c]; }
            }
            if (I.opt.keep_samples)
            {
                const float* px = static_cast<const float*>(I.recvRb.mapped);
                const size_t n = (size_t)I.opt.recv_w * I.opt.recv_h;
                std::vector<uint16_t> img(n * 4);
                for (size_t i = 0; i < n * 4; ++i) img[i] = (uint16_t)std::lround(std::clamp(px[i], 0.f, 1.f) * 65535.f);
                rr.samples.push_back(std::move(img));
            }
        }
    }
    return rr;
}
