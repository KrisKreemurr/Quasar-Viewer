// clodbench (CLOD perf, 2026-11-05): a standalone timing bench for the cluster LOD RT build kernels
// (clean_cluster_rt.comp, the viewer's source and defines) on a synthetic avatar-heavy steady state:
// N rigged RT arena slots whose vertices are 2a's skinned cluster arena (SKIN_OUT, the shared skinned
// cut), the party's mix (most slots refreshed + compared, every pose moving).
//
// It times the kernels the viewer dispatches per frame for that state -- mode 14 (place), mode 9 (the
// RT vertex copy + reuse compare) and, optionally, the rigged CLAS build -- for several SPIR-V builds of
// the same source (e.g. the one-pipeline uber kernel against per-mode variants), interleaved in short
// rounds. Correctness: every variant's outputs (rig arena vertices, CLAS build arguments as a set,
// counters) are hashed after a fixed sequence from the same state and must match.
//
// Not part of the viewer build. Vulkan context + reflection: vqb_vk.* / vqb_reflect.* (copied from
// indra/vqbench, branch vq-hardcode).
#include "vqb_reflect.h"
#include "vqb_vk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
// = clean_cluster_rt.comp / clean_cluster_common.glsl / clean_cluster_skin_common.glsl constants
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kPageWords = 20480, kPageHdrBytes = 32, kRecBytes = 64;
constexpr uint32_t kMaxVerts = 64, kMaxTris = 128;
constexpr uint32_t kRigHdrBase = 16;
constexpr uint32_t kHdrShared = 0x80000000u, kStampRefresh = 0x80000000u;
constexpr uint32_t CNT_RIG = 16, CNT_RIG_ARGS = 30, RIG_SKIN_DISPATCH = 76, RIG_LANE_DISPATCH = 80, RIG_OWN_DISPATCH = 84, kCntWords = 88;
constexpr uint32_t kRigAddrWords = 8;
constexpr uint32_t CLI_RIGGED = 1, CLI1B_AVATAR = 2048, CLI1B_DRAW_RT = 1024, CLINST_RIGGED = 0x80000000u;
constexpr uint32_t kAvSlotCap = 1024, kAvRows = kAvSlotCap + 1, kAvStride = 8;
constexpr uint32_t CLK_CAP = 1, CLK_OVERFLOW = 2, CLK_MAP_CAP = 20, CLK_HDR = 48;
constexpr uint32_t kSkinCharWords = 36;
constexpr uint32_t CLC_VIEW_WORDS = 96;
constexpr uint32_t kInstCutWords = 8;

uint32_t hashWords(uint32_t cap) { if (cap <= 1) return 2; uint32_t b = 0; while ((1u << b) < cap) ++b; return 2u << b; }
uint32_t skinCharBase(uint32_t cap)
{
    const uint32_t box = cap * kMaxVerts * 10u;
    const uint32_t avbox = box + cap * 8u;
    const uint32_t meta = avbox + kAvRows * 8u;
    const uint32_t fr = meta + cap * 8u;
    const uint32_t hb = fr + cap;
    return hb + hashWords(cap);
}
uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

struct ClRtPush
{
    float cam[4];
    float px_scale, tol_px, hyst;
    uint32_t rt_flags, mode, instance_count, ri_cap, frame, cut_cap, max_cut, pool_pages, clas_pages_max, clas_cap,
             blas_max, tier, job_count;
    uint32_t pool_addr[2], decode_addr[2], clas_base[2], clas_refs_addr[2];
    uint32_t clas_stride, idle_frames, part_size, ptlas_full;
};
static_assert(sizeof(ClRtPush) == 128, "ClRtPush = the shader's push block");

struct Opts
{
    uint32_t slots = 90000, avatars = 65, instPerAv = 20, pages = 2048;
    float refreshFrac = 0.8f;
    bool still = false;
    uint32_t frames = 100, warmup = 20, rounds = 6;
    std::vector<std::string> variants;   // name=path.spv
    std::string csv;
    int device = -1;
    bool serialize = true;
    bool clas = false;
    bool own = false;   // the shared skinned cut does not match: every slot is skinned by mode 9 itself
};

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

std::vector<uint32_t> readSpv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "clodbench: cannot read %s\n", path.c_str()); std::exit(1); }
    const size_t n = (size_t)f.tellg();
    std::vector<uint32_t> w(n / 4);
    f.seekg(0);
    f.read((char*)w.data(), n);
    return w;
}

double pct(std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double x = p * (v.size() - 1);
    const size_t i = (size_t)x;
    const double f = x - i;
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}
}   // namespace

int main(int argc, char** argv)
{
    Opts o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--slots") o.slots = (uint32_t)std::stoul(next());
        else if (a == "--avatars") o.avatars = (uint32_t)std::stoul(next());
        else if (a == "--inst") o.instPerAv = (uint32_t)std::stoul(next());
        else if (a == "--refresh") o.refreshFrac = std::stof(next());
        else if (a == "--still") o.still = true;
        else if (a == "--frames") o.frames = (uint32_t)std::stoul(next());
        else if (a == "--warmup") o.warmup = (uint32_t)std::stoul(next());
        else if (a == "--rounds") o.rounds = (uint32_t)std::stoul(next());
        else if (a == "--variant") o.variants.push_back(next());
        else if (a == "--csv") o.csv = next();
        else if (a == "--device") o.device = std::stoi(next());
        else if (a == "--overlap") o.serialize = false;
        else if (a == "--clas") o.clas = true;
        else if (a == "--own") o.own = true;
        else { std::fprintf(stderr, "clodbench: unknown option %s\n", a.c_str()); return 2; }
    }
    if (o.variants.empty()) { std::fprintf(stderr, "clodbench: give --variant name=file.spv (two or more)\n"); return 2; }
    const uint32_t N = o.slots, A = std::min(o.avatars, kAvSlotCap), I = A * o.instPerAv;
    const uint32_t perInst = std::max(1u, N / I);
    if (perInst > 2048) { std::fprintf(stderr, "clodbench: %u slots per instance > 2048\n", perInst); return 2; }

    VqbCtx C;
    C.init(o.device);
    std::printf("clodbench on %s: %u slots, %u avatars x %u instances (%u slots each), refresh %.2f, %s poses\n",
                C.deviceName.c_str(), N, A, o.instPerAv, perInst, o.refreshFrac, o.still ? "held" : "moving");

    // ---- modules + one layout (union of the variants' bindings) ----
    struct Variant { std::string name; std::vector<uint32_t> spv; VqbModule mod; VkPipeline pipe = VK_NULL_HANDLE; };
    std::vector<Variant> vars;
    for (const std::string& v : o.variants)
    {
        const size_t eq = v.find('=');
        Variant x;
        x.name = eq == std::string::npos ? v : v.substr(0, eq);
        x.spv = readSpv(eq == std::string::npos ? v : v.substr(eq + 1));
        std::string err;
        if (!x.mod.parse(x.spv, err)) { std::fprintf(stderr, "clodbench: %s: %s\n", x.name.c_str(), err.c_str()); return 1; }
        vars.push_back(std::move(x));
    }
    std::map<uint32_t, std::map<uint32_t, VqbBindingInfo>> sets;
    {
        std::vector<const VqbModule*> mods;
        for (auto& v : vars) mods.push_back(&v.mod);
        std::string err;
        if (!vqbUnionBindings(mods, sets, err)) { std::fprintf(stderr, "clodbench: %s\n", err.c_str()); return 1; }
    }

    // ---- the synthetic steady state ----
    const uint32_t cap = N, skinCap = N;
    const uint32_t vtxBase = kRigHdrBase + cap * 4;
    std::mt19937 rng(642);
    std::uniform_real_distribution<float> U(-1.f, 1.f);

    // pool: o.pages pages of 64 full clusters (64 verts, 124 tris); slot s takes cluster (s / pages) % 64 of page s % pages
    std::vector<uint32_t> pool((size_t)o.pages * kPageWords, 0u);
    for (uint32_t p = 0; p < o.pages; ++p)
    {
        uint32_t* pg = &pool[(size_t)p * kPageWords];
        pg[1] = 64;
        for (uint32_t c = 0; c < 64; ++c)
        {
            uint32_t* r = pg + (kPageHdrBytes + c * kRecBytes) / 4;
            r[3] = fbits(0.2f);
            r[8] = 2 + c * 64;                                  // vert_off20 (after a 2-vertex header)
            r[9] = 64u | (124u << 8) | (0u << 16);              // counts
            r[10] = (4096 * 20 / 4 + 1024) / 4 + c * 96;        // index_off4 (past the vertices)
            r[11] = kNone;                                      // weight_off10
            uint8_t* ib = (uint8_t*)pg + (size_t)r[10] * 4;
            for (uint32_t t = 0; t < 124; ++t) { ib[t * 3] = 0; ib[t * 3 + 1] = (uint8_t)(1 + t % 62); ib[t * 3 + 2] = (uint8_t)(2 + t % 62); }
        }
    }
    // instances: avatar a at a ring of positions, instPerAv instances each
    std::vector<float> avPos(A * 3);
    for (uint32_t a = 0; a < A; ++a)
    {
        const float ang = a * 2.39996f, rad = 2.f + 0.6f * std::sqrt((float)a);
        avPos[a * 3] = rad * std::cos(ang); avPos[a * 3 + 1] = rad * std::sin(ang); avPos[a * 3 + 2] = 0.f;
    }
    std::vector<uint32_t> inst((size_t)I * 20, 0u);         // ClInstance, 80 B
    for (uint32_t ii = 0; ii < I; ++ii)
    {
        const uint32_t a = ii / o.instPerAv;
        float* f = (float*)&inst[(size_t)ii * 20];
        uint32_t* u = &inst[(size_t)ii * 20];
        f[0] = 1; f[3] = avPos[a * 3];
        f[5] = 1; f[7] = avPos[a * 3 + 1];
        f[10] = 1; f[11] = avPos[a * 3 + 2];
        u[12] = ii;                                          // render_instance
        u[13] = 0; u[14] = 0;                                // mesh, skin_id
        u[15] = CLI_RIGGED | CLI1B_AVATAR | CLI1B_DRAW_RT | (a << 16);
        f[16] = 1; f[17] = 1; f[18] = 0.05f; u[19] = 1;
    }
    // ClView 0 (the main view): px_scale, tol, frame, sel_cap
    std::vector<uint32_t> views(64 * 16 + 512, 0u);   // + the 1b tail (ClSelectAux: aux_base = word 1024 + 25 = 0)
    {
        float* f = (float*)views.data();
        f[16 + 24 + 8] = 1000.f;   // px_scale (after mat4 64 B + 6 planes 96 B + cam 16 + origin 16 = 192 B -> word 48)
        // word index: view_proj 0..15, planes 16..39, cam_pos 40..43, origin 44..47, px_scale 48, tol 49, flags 50,
        // sel_base 51, sel_cap 52, draw_base 53, draw_cap 54, hiz 55, frame 56, view_index 57, viewport 58..59
        f[48] = 1000.f; f[49] = 1.f; views[50] = 0; views[51] = 0; views[52] = 1u << 22; views[56] = 7; views[57] = 0;
        f[16 + 0] = 0;   // (planes unused here)
    }
    // the per-frame rig_of region (per instance 8 words, then the per-avatar table) + persistent words after it
    const uint32_t avBase = I * 8;
    const uint32_t rigOfWords = avBase + kAvRows * kAvStride + kAvRows * (1 + 4 + 4);
    std::vector<uint32_t> rigOf(rigOfWords, 0u);
    // clinst (2 uvec4 per instance), st (6 uvec4 per ri), cut arena entries
    std::vector<uint32_t> clinst((size_t)I * 8, 0u), st((size_t)I * 6 * 4, 0u), cutArena((size_t)N * 4, 0u),
                          instCut((size_t)I * kInstCutWords, 0u);
    // rig arena: header + vertex region; rig_addr: dst (2 cap), sizes (cap), stamps (cap), built (cap)
    std::vector<uint32_t> rig((size_t)vtxBase + (size_t)cap * kMaxVerts * 4, 0u);
    std::vector<uint32_t> rigAddr((size_t)cap * kRigAddrWords, 0u);
    const uint32_t stamp = 5;
    uint32_t s = 0;
    for (uint32_t ii = 0; ii < I; ++ii)
    {
        const uint32_t a = ii / o.instPerAv;
        const uint32_t s0 = s;
        const uint32_t n = (ii + 1 == I) ? (N - s0) : perInst;
        const bool refreshAv = (float)a / A < o.refreshFrac;   // whole avatars refreshed (reuse compare)
        for (uint32_t k = 0; k < n; ++k, ++s)
        {
            const uint32_t page = s % o.pages, idx = (s / o.pages) % 64;
            const uint32_t ref = (page << 6) | idx;
            rig[kRigHdrBase + s * 4 + 0] = ref;
            rig[kRigHdrBase + s * 4 + 1] = s;                       // SKIN_OUT slot
            rig[kRigHdrBase + s * 4 + 2] = k | (124u << 16);
            rig[kRigHdrBase + s * 4 + 3] = ii | kHdrShared;
            rigAddr[(size_t)cap * 3 + s] = stamp | (refreshAv ? kStampRefresh : 0u);
            rigAddr[(size_t)cap * 4 + s] = refreshAv ? 3u : 0u;     // built frame (a CLAS exists)
            cutArena[(size_t)s * 4 + 0] = ref; cutArena[(size_t)s * 4 + 1] = s; cutArena[(size_t)s * 4 + 2] = 124;
        }
        clinst[(size_t)ii * 8 + 0] = ii; clinst[(size_t)ii * 8 + 1] = s0; clinst[(size_t)ii * 8 + 2] = n;
        clinst[(size_t)ii * 8 + 3] = CLINST_RIGGED | (n * 124);
        clinst[(size_t)ii * 8 + 4] = 0x1234567u ^ ii;               // hash a
        clinst[(size_t)ii * 8 + 5] = 0x89abcdu ^ (ii * 3);
        instCut[(size_t)ii * kInstCutWords + 0] = n;
        instCut[(size_t)ii * kInstCutWords + 1] = (0x1234567u ^ ii) + (o.own ? 1u : 0u);
        instCut[(size_t)ii * kInstCutWords + 2] = 0x89abcdu ^ (ii * 3);
        instCut[(size_t)ii * kInstCutWords + 3] = n * 124;
        instCut[(size_t)ii * kInstCutWords + 4] = 7 + 1;            // view frame + 1
        if (refreshAv) st[((size_t)ii * 6 + 2) * 4 + 0] = s0, st[((size_t)ii * 6 + 2) * 4 + 1] = n,
                       st[((size_t)ii * 6 + 2) * 4 + 2] = kNone;   // RIG_ST_TAG
        if (a != (ii + 1 < I ? (ii + 1) / o.instPerAv : A)) { }
    }
    // per-avatar table: base, want; KEEP 2 (refreshed) for the refresh avatars
    for (uint32_t a = 0; a < A; ++a)
    {
        const uint32_t ab = avBase + a * kAvStride;
        const uint32_t i0 = a * o.instPerAv, i1 = i0 + o.instPerAv;
        rigOf[ab + 1] = clinst[(size_t)i0 * 8 + 1];
        uint32_t want = 0;
        for (uint32_t ii = i0; ii < i1; ++ii) want += clinst[(size_t)ii * 8 + 2];
        rigOf[ab + 0] = want;
        rigOf[ab + 3] = ((float)a / A < o.refreshFrac) ? 2u : 0u;
    }
    // SKIN_OUT: 64 SkinVertex per skinned slot + the boxes / meta / hash, then the character rows
    const uint32_t charBase = skinCharBase(skinCap);
    std::vector<uint32_t> skinOut((size_t)charBase + kAvRows * kSkinCharWords + 4096, 0u);
    for (uint32_t v = 0; v < skinCap * kMaxVerts; ++v)
    {
        float* f = (float*)&skinOut[(size_t)v * 5];
        f[0] = 0.3f * U(rng); f[1] = 0.3f * U(rng); f[2] = 0.9f + 0.9f * U(rng);
        skinOut[(size_t)v * 5 + 3] = (uint32_t)(int32_t)(U(rng) * 20000) & 0xFFFFu;
        skinOut[(size_t)v * 5 + 4] = 0x8000u;
    }
    // 2a's slot meta [7] (frame last skinned): moving poses = re-skinned every frame (the prototype's reuse test)
    {
        const uint32_t metaBase = skinCharBase(skinCap) - hashWords(skinCap) - skinCap - skinCap * 8u;
        for (uint32_t k = 0; k < skinCap; ++k) skinOut[(size_t)metaBase + k * 8u + 7u] = o.still ? 0u : 0xFFFFFFF0u;
    }
    for (uint32_t a = 0; a < kAvRows; ++a)
    {
        float* m = (float*)&skinOut[(size_t)charBase + a * kSkinCharWords];
        m[0] = 1; m[4] = 1; m[8] = 1;
        if (a < A) { m[9] = avPos[a * 3]; m[10] = avPos[a * 3 + 1]; m[11] = avPos[a * 3 + 2]; }
        m[33] = 1;
    }
    // 2a's map (selected entry -> skinned slot = the entry's own slot here), after the margins
    const uint32_t mapBase = CLK_HDR + I + 64;
    std::vector<uint32_t> clRig((size_t)mapBase + N, 0u);
    clRig[CLK_CAP] = skinCap; clRig[CLK_OVERFLOW] = 0; clRig[CLK_MAP_CAP] = N; clRig[9] = mapBase;
    for (uint32_t e = 0; e < N; ++e) clRig[(size_t)mapBase + e] = e;
    // the main view's selected slice: entry e = slot e's cluster of its instance, ranked k within it
    std::vector<uint32_t> clSel((size_t)N * 2), clDraws(N);
    for (uint32_t ii = 0; ii < I; ++ii)
        for (uint32_t k = 0; k < clinst[(size_t)ii * 8 + 2]; ++k)
        {
            const uint32_t e = clinst[(size_t)ii * 8 + 1] + k;
            clSel[(size_t)e * 2] = rig[kRigHdrBase + e * 4]; clSel[(size_t)e * 2 + 1] = ii;
            clDraws[e] = 1024u | (k << 12);                 // CLA_RANKED | rank
        }
    std::vector<uint32_t> clCnt((size_t)16 * CLC_VIEW_WORDS + 16, 0u);
    clCnt[0] = N;   // view 0 selected
    clCnt[16] = N;  // CLC_SEL_CLAMPED
    std::vector<uint32_t> cnt(kCntWords, 0u);
    cnt[CNT_RIG] = N;
    cnt[RIG_SKIN_DISPATCH] = std::min(N, 32768u); cnt[RIG_SKIN_DISPATCH + 1] = (N + 32767) / 32768; cnt[RIG_SKIN_DISPATCH + 2] = 1;
    {
        const uint32_t g = (N + 63) / 64;
        cnt[RIG_LANE_DISPATCH] = std::min(g, 32768u); cnt[RIG_LANE_DISPATCH + 1] = (g + 32767) / 32768; cnt[RIG_LANE_DISPATCH + 2] = 1;
    }

    // ---- buffers per binding ----
    const VkBufferUsageFlags SU = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    std::map<std::pair<uint32_t, uint32_t>, VqbBuffer> bufs;
    auto bind = [&](uint32_t set, uint32_t b, const std::vector<uint32_t>& data, size_t minBytes = 0)
    {
        bufs[{set, b}] = C.bufferWith(std::max(minBytes, data.size() * 4), SU, data.data(), data.size() * 4);
    };
    bind(0, 0, pool);
    bind(0, 6, inst);
    bind(0, 7, views);
    bind(0, 8, clSel);
    bind(0, 9, clCnt);
    bind(0, 10, clDraws);
    bind(0, 15, instCut);
    bind(0, 16, skinOut);
    bind(0, 17, clRig);
    bind(1, 0, st);
    bind(1, 1, clinst);
    bind(1, 2, cutArena);
    bind(1, 4, cnt);
    bind(1, 26, rig);
    bind(1, 27, rigOf);
    bind(1, 29, std::vector<uint32_t>((size_t)cap * 16, 0u));
    bind(1, 30, rigAddr);
    // the rigged CLAS build (--clas): one explicit destination per slot (= planRigClas), fast build
    VkClusterAccelerationStructureTriangleClusterInputNV ctri{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV};
    VkClusterAccelerationStructureInputInfoNV cin{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
    VqbBuffer clasArena, clasScratch;
    uint32_t clasStride = 0;
    if (o.clas && !C.hasClas) { std::fprintf(stderr, "clodbench: --clas needs VK_NV_cluster_acceleration_structure\n"); return 1; }
    if (o.clas)
    {
        ctri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        ctri.maxGeometryIndexValue = 2047; ctri.maxClusterUniqueGeometryCount = 1;
        ctri.maxClusterTriangleCount = kMaxTris; ctri.maxClusterVertexCount = kMaxVerts;
        ctri.maxTotalTriangleCount = kMaxTris; ctri.maxTotalVertexCount = kMaxVerts;
        cin.maxAccelerationStructureCount = 1;
        cin.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        cin.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
        cin.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
        cin.opInput.pTriangleClusters = &ctri;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        C.clasSizes(C.dev, &cin, &sz);
        clasStride = (uint32_t)((sz.accelerationStructureSize + 255) & ~255ull);
        ctri.maxTotalTriangleCount = cap * kMaxTris; ctri.maxTotalVertexCount = cap * kMaxVerts;
        cin.maxAccelerationStructureCount = cap;
        C.clasSizes(C.dev, &cin, &sz);
        clasArena = C.buffer((VkDeviceSize)cap * clasStride + 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        clasScratch = C.buffer(sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        std::printf("  CLAS: %u B per slot, arena %.0f MB, scratch %.0f MB\n", clasStride, cap * (double)clasStride / 1048576.0,
                    sz.buildScratchSize / 1048576.0);
    }
    // rcpu (parameters), after the buffers it addresses exist
    std::vector<uint32_t> rcpu(256, 0u);
    {
        const VqbBuffer& rb = bufs[{1, 26}];
        const VkDeviceAddress va = rb.addr + (VkDeviceAddress)vtxBase * 4;
        const VkDeviceAddress sa = bufs[{0, 16}].addr;
        rcpu[0] = 1; rcpu[1] = 0; rcpu[2] = 1; rcpu[3] = cap; rcpu[4] = vtxBase; rcpu[5] = 0;
        rcpu[6] = (uint32_t)va; rcpu[7] = (uint32_t)(va >> 32);
        rcpu[8] = 1; rcpu[11] = 1;
        rcpu[18] = (uint32_t)sa; rcpu[19] = (uint32_t)(sa >> 32);
        rcpu[20] = avBase;
        rcpu[23] = 4;                       // RP_KEEP bit 2: reuse continuity
        rcpu[27] = stamp;                   // RP_STAMP
        const VkDeviceAddress cbase = o.clas ? ((clasArena.addr + 255) & ~255ull) : 0x110000000ull;
        rcpu[28] = (uint32_t)cbase; rcpu[29] = (uint32_t)(cbase >> 32); rcpu[30] = o.clas ? clasStride : 1536;
        rcpu[33] = rcpu[34] = rcpu[35] = 1;
        rcpu[36] = rcpu[37] = rcpu[38] = fbits(1.f);
        rcpu[39] = fbits(8.f); rcpu[40] = fbits(24.f);
        rcpu[41] = 8; rcpu[42] = fbits(0.f);
        rcpu[43] = 1; rcpu[44] = 1;         // RP_RANK, RP_REUSE (the defaults)
        rcpu[45] = fbits(0.00002f); rcpu[46] = fbits(0.5f);
        for (uint32_t w = 0; w < 32; ++w) rcpu[96 + w] = 0xFFFFFFFFu;   // RP_RT_AV: every avatar traced
        bind(1, 31, rcpu);
    }
    // everything else: a small zeroed buffer
    for (auto& [set, m] : sets)
        for (auto& [b, info] : m)
            if (!bufs.count({set, b}))
                bufs[{set, b}] = C.bufferWith(1 << 16, SU, nullptr, 0);
    // pristine copies of the per-frame state the kernels change (restored before every timed frame)
    const VqbBuffer cntInit = C.bufferWith(cnt.size() * 4, SU, cnt.data(), cnt.size() * 4);
    const VqbBuffer rigOfInit = C.bufferWith(rigOf.size() * 4, SU, rigOf.data(), rigOf.size() * 4);
    const VqbBuffer rigInit = C.bufferWith(rig.size() * 4, SU, rig.data(), rig.size() * 4);
    const VqbBuffer rigAddrInit = C.bufferWith(rigAddr.size() * 4, SU, rigAddr.data(), rigAddr.size() * 4);

    // ---- descriptor sets ----
    std::vector<VkDescriptorSetLayout> dsl(2, VK_NULL_HANDLE);
    std::vector<VkDescriptorSet> ds(2, VK_NULL_HANDLE);
    VkDescriptorPool dpool;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 4; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VQB_CK(vkCreateDescriptorPool(C.dev, &pi, nullptr, &dpool));
    }
    for (uint32_t set = 0; set < 2; ++set)
    {
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (auto& [b, info] : sets[set])
        {
            if (info.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            { std::fprintf(stderr, "clodbench: set %u binding %u: type %d unsupported\n", set, b, (int)info.type); return 1; }
            lb.push_back({b, info.type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)lb.size(); li.pBindings = lb.data();
        VQB_CK(vkCreateDescriptorSetLayout(C.dev, &li, nullptr, &dsl[set]));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsl[set];
        VQB_CK(vkAllocateDescriptorSets(C.dev, &ai, &ds[set]));
        std::vector<VkDescriptorBufferInfo> bi;
        bi.reserve(lb.size());
        std::vector<VkWriteDescriptorSet> wr;
        for (auto& l : lb)
        {
            const VqbBuffer& b = bufs[{set, l.binding}];
            bi.push_back({b.buf, 0, VK_WHOLE_SIZE});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = ds[set]; w.dstBinding = l.binding; w.descriptorCount = 1; w.descriptorType = l.descriptorType;
            w.pBufferInfo = &bi.back();
            wr.push_back(w);
        }
        vkUpdateDescriptorSets(C.dev, (uint32_t)wr.size(), wr.data(), 0, nullptr);
    }
    VkPipelineLayout layout;
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClRtPush)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 2; li.pSetLayouts = dsl.data(); li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        VQB_CK(vkCreatePipelineLayout(C.dev, &li, nullptr, &layout));
    }
    for (Variant& v : vars)
    {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = v.spv.size() * 4; mi.pCode = v.spv.data();
        VkShaderModule m;
        VQB_CK(vkCreateShaderModule(C.dev, &mi, nullptr, &m));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = layout;
        VQB_CK(vkCreateComputePipelines(C.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &v.pipe));
        vkDestroyShaderModule(C.dev, m, nullptr);
        // registers / shared memory (VK_KHR_pipeline_executable_properties)
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = v.pipe; ei.executableIndex = 0;
        uint32_t sn = 0;
        C.getStat(C.dev, &ei, &sn, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> sv(sn, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        C.getStat(C.dev, &ei, &sn, sv.data());
        std::printf("  %-10s", v.name.c_str());
        for (auto& x : sv)
            if (!std::strcmp(x.name, "Register Count") || !std::strcmp(x.name, "Shared Memory Size"))
                std::printf("  %s=%llu", x.name, (unsigned long long)x.value.u64);
        std::printf("\n");
    }

    // ---- recording ----
    auto barrier = [&](VkCommandBuffer c)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(c, &d);
    };
    VkQueryPool qp;
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 64 * 8;
        VQB_CK(vkCreateQueryPool(C.dev, &qi, nullptr, &qp));
    }
    auto push = [&](VkCommandBuffer c, uint32_t mode, uint32_t frame)
    {
        ClRtPush p{};
        p.cam[0] = 0; p.cam[1] = 0; p.cam[2] = 1.7f; p.cam[3] = 0.1f;
        p.px_scale = 1000.f; p.tol_px = 2.f; p.hyst = 0.25f; p.rt_flags = 0;
        p.mode = mode; p.instance_count = I; p.ri_cap = I; p.frame = frame;
        p.cut_cap = 1u << 17; p.max_cut = 2048; p.pool_pages = o.pages; p.clas_pages_max = 256; p.clas_cap = 0;
        p.blas_max = I; p.tier = 1;
        const VkDeviceAddress pa = bufs[{0, 0}].addr;
        p.pool_addr[0] = (uint32_t)pa; p.pool_addr[1] = (uint32_t)(pa >> 32);
        vkCmdPushConstants(c, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    };
    auto restore = [&](VkCommandBuffer c, uint32_t frame)
    {
        VkBufferCopy r{0, 0, cnt.size() * 4};
        vkCmdCopyBuffer(c, cntInit.buf, bufs[{1, 4}].buf, 1, &r);
        r.size = rigOf.size() * 4;
        vkCmdCopyBuffer(c, rigOfInit.buf, bufs[{1, 27}].buf, 1, &r);
        // (the per-frame words only: dst, sizes, stamps, build frames -- not the persistent RA_SRC words)
        r.size = (VkDeviceSize)cap * 5 * 4;
        vkCmdCopyBuffer(c, rigAddrInit.buf, bufs[{1, 30}].buf, 1, &r);
        // the poses move: every character's translation shifts a little each frame (no reuse skip), and
        // 2a re-skins every slot (meta [7] = this frame; the meta region filled whole: only [7] is read)
        if (!o.still)
        {
            const VkDeviceSize mb = ((VkDeviceSize)skinCharBase(skinCap) - hashWords(skinCap) - skinCap - (VkDeviceSize)skinCap * 8u) * 4;
            vkCmdFillBuffer(c, bufs[{0, 16}].buf, mb, (VkDeviceSize)skinCap * 32, frame);
            std::vector<float> t(A * 12);
            for (uint32_t a = 0; a < A; ++a)
            {
                float* m = &t[a * 12];
                std::memset(m, 0, 48);
                m[0] = 1; m[4] = 1; m[8] = 1;
                m[9] = avPos[a * 3] + 0.001f * (frame % 7); m[10] = avPos[a * 3 + 1]; m[11] = avPos[a * 3 + 2];
            }
            for (uint32_t a = 0; a < A; ++a)
                vkCmdUpdateBuffer(c, bufs[{0, 16}].buf, ((VkDeviceSize)charBase + a * kSkinCharWords) * 4, 48, &t[a * 12]);
        }
    };
    // the stages, in the viewer's order (record(): 11 gather, [0 cut, 13 admit], 14 place, 12 scatter,
    // 9 skin / copy, the rigged CLAS build); each mark = the time since the previous one
    const std::vector<std::string> stage = {"m11 gather", "m14 place", "m12 scatter", "m9 / m23+24+9", "CLAS build"};
    const uint32_t kMarks = (uint32_t)stage.size() + 1;
    auto record = [&](VkCommandBuffer c, const Variant& v, uint32_t frame, int qbase)
    {
        restore(c, frame);
        barrier(c);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, v.pipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, ds.data(), 0, nullptr);
        uint32_t m = 0;
        auto mark = [&]() { if (qbase >= 0) vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, qbase + m); ++m; };
        mark();
        push(c, 11, frame);
        vkCmdDispatch(c, (N + 63) / 64, 1, 1);                              // mode 11: one lane per selected entry
        barrier(c); mark();
        push(c, 14, frame);
        vkCmdDispatch(c, std::min(I, 32768u), (I + 32767) / 32768, 1);   // mode 14: one workgroup per instance
        barrier(c); mark();
        push(c, 12, frame);
        vkCmdDispatch(c, (N + 63) / 64, 1, 1);                              // mode 12: one lane per selected entry
        barrier(c); mark();
        // a "char*" variant = 2026-11-05's CHARACTER-SPACE chain: 23 (one lane per slot), 24, 9 over the own list
        if (v.name.rfind("char", 0) == 0)
        {
            push(c, 23, frame);
            vkCmdDispatchIndirect(c, bufs[{1, 4}].buf, (VkDeviceSize)RIG_LANE_DISPATCH * 4);
            barrier(c);
            push(c, 24, frame);
            vkCmdDispatch(c, 1, 1, 1);
            barrier(c);
            push(c, 9, frame);
            vkCmdDispatchIndirect(c, bufs[{1, 4}].buf, (VkDeviceSize)RIG_OWN_DISPATCH * 4);
        }
        else
        {
            push(c, 9, frame);
            vkCmdDispatchIndirect(c, bufs[{1, 4}].buf, (VkDeviceSize)RIG_SKIN_DISPATCH * 4);
        }
        barrier(c); mark();
        if (o.clas)
        {
            VkClusterAccelerationStructureCommandsInfoNV ci{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
            ci.input = cin;
            ci.scratchData = (clasScratch.addr + 255) & ~255ull;
            const VkDeviceAddress ra = bufs[{1, 30}].addr;
            ci.dstAddressesArray = {ra, 8, (VkDeviceSize)cap * 8};
            ci.dstSizesArray = {ra + (VkDeviceSize)cap * 8, 4, (VkDeviceSize)cap * 4};
            ci.srcInfosArray = {bufs[{1, 29}].addr, 64, (VkDeviceSize)cap * 64};
            ci.srcInfosCount = bufs[{1, 4}].addr + CNT_RIG_ARGS * 4;
            C.cmdBuildClas(c, &ci);
            barrier(c);
        }
        mark();
    };

    // ---- VERIFY: from the same state, 3 frames, hash the outputs ----
    std::printf("VERIFY (3 frames from the same state)\n");
    std::vector<uint64_t> hashes;
    for (const Variant& v : vars)
    {
        C.oneShot([&](VkCommandBuffer c)
        {
            VkBufferCopy r{0, 0, rig.size() * 4};
            vkCmdCopyBuffer(c, rigInit.buf, bufs[{1, 26}].buf, 1, &r);
            VkBufferCopy ra{0, 0, rigAddr.size() * 4};
            vkCmdCopyBuffer(c, rigAddrInit.buf, bufs[{1, 30}].buf, 1, &ra);
            vkCmdFillBuffer(c, bufs[{1, 29}].buf, 0, VK_WHOLE_SIZE, 0u);
            barrier(c);
            for (uint32_t f = 0; f < 3; ++f) { record(c, v, 100 + f, -1); barrier(c); }
        });
        std::vector<uint32_t> outRig(rig.size()), outArgs((size_t)cap * 16), outAddr(rigAddr.size()), outCnt(kCntWords), outOf(rigOf.size());
        C.download(bufs[{1, 26}], 0, outRig.data(), outRig.size() * 4);
        C.download(bufs[{1, 29}], 0, outArgs.data(), outArgs.size() * 4);
        C.download(bufs[{1, 30}], 0, outAddr.data(), outAddr.size() * 4);
        C.download(bufs[{1, 4}], 0, outCnt.data(), outCnt.size() * 4);
        C.download(bufs[{1, 27}], 0, outOf.data(), outOf.size() * 4);
        // the CLAS args come in atomic order: compare them as a set (each with its destination)
        const uint32_t na = std::min(outCnt[CNT_RIG_ARGS], cap);
        std::vector<std::vector<uint32_t>> recs(na);
        for (uint32_t a = 0; a < na; ++a)
        {
            recs[a].assign(outArgs.begin() + (size_t)a * 16, outArgs.begin() + (size_t)a * 16 + 16);
            recs[a].push_back(outAddr[a * 2]); recs[a].push_back(outAddr[a * 2 + 1]);
        }
        std::sort(recs.begin(), recs.end());
        uint64_t hArgs = 1469598103934665603ull;
        for (auto& r : recs) hArgs = fnv(r.data(), r.size() * 4, hArgs);
        // the same without the vertex source (stride word 4, address words 8 / 9), and how many args name
        // their slot's SKIN_OUT cluster at stride 20 (the character-space chain) / the RT arena at 16
        uint32_t srcSkin = 0, srcArena = 0;
        const VkDeviceAddress soA = bufs[{0, 16}].addr, arA = bufs[{1, 26}].addr + (VkDeviceAddress)vtxBase * 4;
        std::vector<std::vector<uint32_t>> rn = recs;
        for (auto& r : rn)
        {
            const uint32_t sl = r[0];
            const VkDeviceAddress va = (VkDeviceAddress)r[8] | ((VkDeviceAddress)r[9] << 32);
            const uint32_t vpage = outRig[kRigHdrBase + sl * 4 + 1];
            if (r[4] == (20u << 16) && va == soA + (VkDeviceAddress)vpage * 64 * 20) ++srcSkin;
            if (r[4] == (16u << 16) && va == arA + (VkDeviceAddress)sl * 64 * 16) ++srcArena;
            r[4] = r[8] = r[9] = 0;
        }
        std::sort(rn.begin(), rn.end());
        uint64_t hArgsNoSrc = 1469598103934665603ull;
        for (auto& r : rn) hArgsNoSrc = fnv(r.data(), r.size() * 4, hArgsNoSrc);
        std::printf("  %-10s CLAS args without the vertex source %016llx; source = SKIN_OUT slot (stride 20) %u, RT arena (16) %u\n",
                    v.name.c_str(), (unsigned long long)hArgsNoSrc, srcSkin, srcArena);
        const uint64_t hRig = fnv(outRig.data(), outRig.size() * 4);
        const uint64_t hStamp = fnv(outAddr.data() + (size_t)cap * 3, (size_t)cap * 2 * 4);
        const uint64_t hCnt = fnv(outCnt.data(), 76 * 4);
        const uint64_t hOf = fnv(outOf.data(), outOf.size() * 4);
        const uint64_t h = hArgs ^ (hRig * 3) ^ (hStamp * 5) ^ (hCnt * 7) ^ (hOf * 11);
        hashes.push_back(h);
        std::printf("  %-10s args %u (copied %u, own %u, cmp %u, skip %u)  rig %016llx args %016llx stamps %016llx cnt %016llx rig_of %016llx %s\n",
                    v.name.c_str(), na, outCnt[75], outCnt[74], outCnt[72], outCnt[73], (unsigned long long)hRig,
                    (unsigned long long)hArgs, (unsigned long long)hStamp, (unsigned long long)hCnt, (unsigned long long)hOf,
                    h == hashes[0] ? "" : "*** DIFFERS ***");
    }

    // ---- TIMING: interleaved rounds ----
    std::map<std::string, std::vector<std::vector<double>>> tst;   // [variant][stage + total]
    VkCommandBuffer cb;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = C.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VQB_CK(vkAllocateCommandBuffers(C.dev, &ai, &cb));
    }
    VkFence fence;
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VQB_CK(vkCreateFence(C.dev, &fi, nullptr, &fence));
    }
    const uint32_t kBatch = 16;
    uint32_t frame = 1000;
    for (uint32_t round = 0; round < o.rounds; ++round)
        for (size_t vi = 0; vi < vars.size(); ++vi)
        {
            // alternate the order per round (drift falls on every variant equally)
            const Variant& v = vars[(round & 1) ? vars.size() - 1 - vi : vi];
            uint32_t done = 0;
            const uint32_t total = o.warmup + o.frames;
            while (done < total)
            {
                const uint32_t n = std::min(kBatch, total - done);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VQB_CK(vkBeginCommandBuffer(cb, &bi));
                vkCmdResetQueryPool(cb, qp, 0, kBatch * kMarks);
                for (uint32_t f = 0; f < n; ++f) record(cb, v, frame++, (int)(f * kMarks));
                VQB_CK(vkEndCommandBuffer(cb));
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                VQB_CK(vkQueueSubmit(C.queue, 1, &si, fence));
                VQB_CK(vkWaitForFences(C.dev, 1, &fence, VK_TRUE, ~0ull));
                VQB_CK(vkResetFences(C.dev, 1, &fence));
                std::vector<uint64_t> ts(n * kMarks);
                VQB_CK(vkGetQueryPoolResults(C.dev, qp, 0, n * kMarks, ts.size() * 8, ts.data(), 8,
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                for (uint32_t f = 0; f < n; ++f)
                {
                    if (done + f < o.warmup) continue;
                    const double k = C.tsPeriodNs * 1e-6;
                    auto& T = tst[v.name];
                    T.resize(stage.size() + 1);
                    for (size_t q = 0; q < stage.size(); ++q) T[q].push_back((ts[f * kMarks + q + 1] - ts[f * kMarks + q]) * k);
                    T[stage.size()].push_back((ts[f * kMarks + stage.size()] - ts[f * kMarks]) * k);
                }
                done += n;
            }
        }
    std::printf("TIMING (ms; median [p10 p90]; %u rounds x %u frames per variant, serialized stages)\n", o.rounds, o.frames);
    for (size_t q = 0; q <= stage.size(); ++q)
    {
        std::printf("  %-12s", q < stage.size() ? stage[q].c_str() : "TOTAL");
        for (const Variant& v : vars)
        {
            const auto& x = tst[v.name][q];
            std::printf("  %s %7.4f [%7.4f %7.4f]", v.name.c_str(), pct(x, .5), pct(x, .1), pct(x, .9));
        }
        std::printf("\n");
    }
    if (!o.csv.empty())
    {
        std::ofstream f(o.csv);
        f << "variant,stage,sample,ms\n";
        for (const Variant& v : vars)
            for (size_t q = 0; q <= stage.size(); ++q)
                for (size_t i = 0; i < tst[v.name][q].size(); ++i)
                    f << v.name << "," << (q < stage.size() ? stage[q] : std::string("TOTAL")) << "," << i << "," << tst[v.name][q][i] << "\n";
    }
    vkDeviceWaitIdle(C.dev);
    C.shutdown();
    return 0;
}
