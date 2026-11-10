// dnbench (perf-4k60): times the viewer's a-trous denoiser kernel (clean_dn_atrous.comp, LLCleanDenoiserVK::
// recordSpatial) A against B on a seeded synthetic input (dn_init.comp), ABAB rounds in one process, and checks
// that B's output is BIT-IDENTICAL to A's from the same input (both ping-pong images, every layer).
//
//   dnbench --a A.spv --b B.spv --init dn_init.spv [--size 1920x1080] [--iters 3] [--rounds 12] [--frames 40]
//           [--b-lattice]   (B is the lattice-tiled kernel: dispatch ceil(W / 8s) s x ceil(H / 8s) s groups)
//           [--flags N]     (the push ctl.x; default: the GI instance's: history | info | young boost | hfix 4)
//           [--layers L]    (IN / OUT layers: 2 = GI, 3 = with the sun channel -- then ctl.x needs DN_F_SUN)
#include "../vqb_vk.h"
#include "../vqb_reflect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static std::vector<uint32_t> readWords(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "dnbench: cannot read %s\n", p.c_str()); std::exit(1); }
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> w(b.size() / 4);
    std::memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}

struct Push { float dims[4], prm0[4], prm1[4]; uint32_t ctl[4]; float sun0[4], sun1[4]; };

int main(int argc, char** argv)
{
    std::string spvA, spvB, spvInit;
    uint32_t W = 1920, H = 1080, iters = 3, rounds = 12, frames = 40, layers = 2;
    bool bLattice = false, aLattice = false;
    uint32_t flags = 1u | 4u | 4096u | (4u << 27);   // DN_F_HIST | DN_F_YOUNG_BOOST | DN_F_INFO | hfix 4 (the GI instance)
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--a") spvA = next();
        else if (a == "--b") spvB = next();
        else if (a == "--init") spvInit = next();
        else if (a == "--size") { std::string s = next(); std::sscanf(s.c_str(), "%ux%u", &W, &H); }
        else if (a == "--iters") iters = std::stoul(next());
        else if (a == "--rounds") rounds = std::stoul(next());
        else if (a == "--frames") frames = std::stoul(next());
        else if (a == "--layers") layers = std::stoul(next());
        else if (a == "--flags") flags = (uint32_t)std::stoul(next(), nullptr, 0);
        else if (a == "--b-lattice") bLattice = true;
        else if (a == "--a-lattice") aLattice = true;
        else { std::fprintf(stderr, "dnbench: unknown option %s\n", a.c_str()); return 2; }
    }
    if (spvA.empty() || spvB.empty() || spvInit.empty()) { std::fprintf(stderr, "dnbench: --a --b --init required\n"); return 2; }
    VqbCtx ctx;
    ctx.init(-1);
    std::printf("dnbench on %s: %ux%u, %u iterations, IN/OUT %u layers, ctl.x 0x%08x\n", ctx.deviceName.c_str(), W, H, iters, layers, flags);

    // ---- layout: set 0 = binding 9 the frame info UBO (placeholder), set 1 = the six denoiser bindings ----
    VkDescriptorSetLayout dsl[2];
    {
        VkDescriptorSetLayoutBinding b0{9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1; ci.pBindings = &b0;
        VQB_CK(vkCreateDescriptorSetLayout(ctx.dev, &ci, nullptr, &dsl[0]));
        VkDescriptorSetLayoutBinding b1[6];
        for (uint32_t k = 0; k < 6; ++k)
            b1[k] = {k, k == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        ci.bindingCount = 6; ci.pBindings = b1;
        VQB_CK(vkCreateDescriptorSetLayout(ctx.dev, &ci, nullptr, &dsl[1]));
    }
    VkPipelineLayout pl;
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2; ci.pSetLayouts = dsl; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pr;
        VQB_CK(vkCreatePipelineLayout(ctx.dev, &ci, nullptr, &pl));
    }
    auto pipe = [&](const std::string& path)
    {
        const std::vector<uint32_t> code = readWords(path);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * 4; mi.pCode = code.data();
        VkShaderModule m; VQB_CK(vkCreateShaderModule(ctx.dev, &mi, nullptr, &m));
        // the viewer specializes kDebugViews (constant_id 1) off for the lighting kernels; the denoiser has none
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        pi.layout = pl;
        pi.flags = ctx.getStat ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0;
        VkPipeline p; VQB_CK(vkCreateComputePipelines(ctx.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p));
        vkDestroyShaderModule(ctx.dev, m, nullptr);
        if (ctx.getExe && ctx.getStat)
        {
            VkPipelineInfoKHR inf{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR}; inf.pipeline = p;
            uint32_t ne = 0; ctx.getExe(ctx.dev, &inf, &ne, nullptr);
            if (ne)
            {
                VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR}; ei.pipeline = p; ei.executableIndex = 0;
                uint32_t ns = 0; ctx.getStat(ctx.dev, &ei, &ns, nullptr);
                std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
                ctx.getStat(ctx.dev, &ei, &ns, st.data());
                std::printf("  %-40s", path.substr(path.find_last_of('/') + 1).c_str());
                for (auto& s : st)
                    if (std::strstr(s.name, "Register") || std::strstr(s.name, "Shared") || std::strstr(s.name, "Local"))
                        std::printf("  %s %llu", s.name, (unsigned long long)s.value.u64);
                std::printf("\n");
            }
        }
        return p;
    };
    VkPipeline pA = pipe(spvA), pB = pipe(spvB), pInit = pipe(spvInit);

    // ---- images ----
    const VkImageUsageFlags U = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VqbImage iIn = ctx.image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, layers, 1, U);
    VqbImage iMot = ctx.image(VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_UINT, W, H, 1, 1, U);
    VqbImage iGeom = ctx.image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R32G32B32A32_UINT, W, H, 2, 1, U);
    const uint32_t histLayers = layers >= 3 ? 6u : 4u;
    VqbImage iHist = ctx.image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R32G32B32A32_UINT, W, H, histLayers, 1, U);
    VqbImage iOut = ctx.image(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, layers, 1, U);
    VqbImage iMv = ctx.image(VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 4, 4, 1, 1, U);
    VqbBuffer ubo = ctx.buffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    VkSampler samp;
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        VQB_CK(vkCreateSampler(ctx.dev, &si, nullptr, &samp));
    }
    VkDescriptorPool dp;
    {
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 2; ci.poolSizeCount = 3; ci.pPoolSizes = ps;
        VQB_CK(vkCreateDescriptorPool(ctx.dev, &ci, nullptr, &dp));
    }
    VkDescriptorSet ds[2];
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dp; ai.descriptorSetCount = 2; ai.pSetLayouts = dsl;
        VQB_CK(vkAllocateDescriptorSets(ctx.dev, &ai, ds));
        VkDescriptorBufferInfo bi{ubo.buf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ii[6] = {{VK_NULL_HANDLE, iIn.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, iMot.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, iGeom.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, iHist.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, iOut.view, VK_IMAGE_LAYOUT_GENERAL}, {samp, iMv.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[7]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds[0], 9, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bi, nullptr};
        for (uint32_t k = 0; k < 6; ++k)
            w[1 + k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds[1], k, 0, 1,
                        k == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[k], nullptr, nullptr};
        vkUpdateDescriptorSets(ctx.dev, 7, w, 0, nullptr);
    }
    VkQueryPool qp;
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * 64;
        VQB_CK(vkCreateQueryPool(ctx.dev, &qi, nullptr, &qp));
    }
    auto barrier = [](VkCommandBuffer c)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(c, &d);
    };
    auto init = [&](VkCommandBuffer c)
    {
        VkClearColorValue z{};
        for (VqbImage* im : {&iIn, &iMot, &iGeom, &iHist, &iOut})
        {
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
            vkCmdClearColorImage(c, im->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
        }
        barrier(c);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pInit);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, ds, 0, nullptr);
        const uint32_t a[4] = {W, H, 1039u, histLayers - 2u};
        vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, a);
        vkCmdDispatch(c, (W + 7) / 8, (H + 7) / 8, 1);
        barrier(c);
    };
    // THE CHAIN (recordSpatial): iterations 0..N-1, a compute barrier before each
    auto chain = [&](VkCommandBuffer c, VkPipeline p, bool lattice, uint32_t q0)
    {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, ds, 0, nullptr);
        Push q{};
        q.dims[0] = (float)W; q.dims[1] = (float)H; q.dims[2] = (float)(2 * W); q.dims[3] = (float)(2 * H);
        q.prm0[0] = 32.f; q.prm0[1] = 32.f; q.prm0[2] = 2.f; q.prm0[3] = 4.f;
        q.prm1[0] = 1.f; q.prm1[1] = 64.f; q.prm1[2] = 0.f; q.prm1[3] = 1.f;
        q.ctl[0] = flags; q.ctl[2] = iters; q.ctl[3] = 7u;
        q.sun1[0] = 8.f;
        vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, q0);
        for (uint32_t k = 0; k < iters; ++k)
        {
            barrier(c);
            q.ctl[1] = k;
            vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
            const uint32_t s = 1u << std::min(k, 7u);
            if (lattice) vkCmdDispatch(c, (W + 8 * s - 1) / (8 * s) * s, (H + 8 * s - 1) / (8 * s) * s, 1);
            else vkCmdDispatch(c, (W + 7) / 8, (H + 7) / 8, 1);
        }
        vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, q0 + 1);
    };
    // ---- VERIFY: A and B from the same input, both ping-pong images compared byte for byte ----
    auto readImg = [&](const VqbImage& im)
    {
        const size_t texel = im.format == VK_FORMAT_R32G32B32A32_UINT ? 16 : 8;
        const size_t bytes = (size_t)im.w * im.h * im.layers * texel;
        VqbBuffer rb = ctx.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        ctx.oneShot([&](VkCommandBuffer c)
        {
            barrier(c);
            VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers}; r.imageExtent = {im.w, im.h, 1};
            vkCmdCopyImageToBuffer(c, im.img, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &r);
        });
        std::vector<uint8_t> v(bytes); std::memcpy(v.data(), rb.mapped, bytes);
        return v;
    };
    std::vector<uint8_t> refIn, refOut, refHist;
    for (int which = 0; which < 2; ++which)
    {
        ctx.oneShot([&](VkCommandBuffer c) { init(c); vkCmdResetQueryPool(c, qp, 0, 2); chain(c, which ? pB : pA, which ? bLattice : aLattice, 0); });
        std::vector<uint8_t> a = readImg(iIn), b = readImg(iOut), h = readImg(iHist);
        if (!which) { refIn = a; refOut = b; refHist = h; continue; }
        size_t dIn = 0, dOut = 0, dH = 0;
        for (size_t i = 0; i < a.size(); i += 2) dIn += std::memcmp(&a[i], &refIn[i], 2) != 0;
        for (size_t i = 0; i < b.size(); i += 2) dOut += std::memcmp(&b[i], &refOut[i], 2) != 0;
        for (size_t i = 0; i < h.size(); i += 4) dH += std::memcmp(&h[i], &refHist[i], 4) != 0;
        std::printf("VERIFY B vs A: IN %zu / %zu halfs differ, OUT %zu / %zu, HIST %zu / %zu words  -> %s\n", dIn, a.size() / 2, dOut,
                    b.size() / 2, dH, h.size() / 4, (dIn | dOut | dH) == 0 ? "BIT-IDENTICAL" : "DIFFERENT");
    }
    // ---- TIMING: ABAB rounds; each round `frames` chains per variant (a fresh input every 8: same work) ----
    std::vector<double> tA, tB;
    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = ctx.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VQB_CK(vkAllocateCommandBuffers(ctx.dev, &ai, &cmd));
    }
    VkFence fence; { VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VQB_CK(vkCreateFence(ctx.dev, &fi, nullptr, &fence)); }
    for (uint32_t r = 0; r < rounds; ++r)
        for (int which = 0; which < 2; ++which)
            for (uint32_t f0 = 0; f0 < frames; f0 += 8)
            {
                const uint32_t n = std::min(8u, frames - f0);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                VQB_CK(vkBeginCommandBuffer(cmd, &bi));
                vkCmdResetQueryPool(cmd, qp, 0, 2 * n);
                init(cmd);
                for (uint32_t f = 0; f < n; ++f) chain(cmd, which ? pB : pA, which ? bLattice : aLattice, 2 * f);
                VQB_CK(vkEndCommandBuffer(cmd));
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
                VQB_CK(vkQueueSubmit(ctx.queue, 1, &si, fence));
                VQB_CK(vkWaitForFences(ctx.dev, 1, &fence, VK_TRUE, ~0ull));
                VQB_CK(vkResetFences(ctx.dev, 1, &fence));
                std::vector<uint64_t> ts(2 * n);
                VQB_CK(vkGetQueryPoolResults(ctx.dev, qp, 0, 2 * n, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                if (r == 0) continue;   // (warm-up round)
                for (uint32_t f = 0; f < n; ++f) (which ? tB : tA).push_back((ts[2 * f + 1] - ts[2 * f]) * ctx.tsPeriodNs * 1e-6);
            }
    auto pct = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[(size_t)std::min<double>(v.size() - 1, p * v.size())]; };
    const double a50 = pct(tA, 0.5), b50 = pct(tB, 0.5);
    std::printf("TIMING (%zu chains each, %u iterations, ABAB x %u rounds)\n", tA.size(), iters, rounds - 1);
    std::printf("  A  median %.4f ms  p10 %.4f  p90 %.4f\n", a50, pct(tA, 0.1), pct(tA, 0.9));
    std::printf("  B  median %.4f ms  p10 %.4f  p90 %.4f\n", b50, pct(tB, 0.1), pct(tB, 0.9));
    std::printf("  B - A = %+.4f ms (%+.1f %%)\n", b50 - a50, 100.0 * (b50 - a50) / a50);
    // per-round medians (the drift check)
    return 0;
}
