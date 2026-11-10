/**
 * @file llcleanphysshapevk.cpp
 * @brief Show Physics Shape When Editing (#1176): the selection's physics shapes, drawn by
 *        the GPU from rows that change only when the selection or a shape does.
 *
 * The CPU half is newview/llcleanphysshape.cpp (LLCleanPhysShapeStore): each shape's geometry
 * as raw slots of the clean mesh store, packed into mVbo / mIbo like any other slot, and one
 * row per selected object and shape slot (the object's instance id, the slot). This file
 * defines the LLCleanRendererVK members of its "PHYSICS SHAPES" block:
 *
 *  - setPhysShapes: the rows into this frame slot's host buffer, only when their generation
 *    differs from the one that buffer holds;
 *  - recordPhysShapeCmds (after the main cull, outside rendering): clean_phys_shape.comp
 *    writes each row's DrawCmd from the instance and slot tables as they are this frame, or
 *    an empty one, in place -- the CPU's colour-bucket ranges stay valid;
 *  - recordPhysShapes (in the selection pass, against the display depth, before the selection
 *    outline and the manipulators): per colour bucket one indirect draw over its range,
 *    through the world VS -- the object's own pose, so edit drags, spin and worn attachments
 *    on their joint come with it -- and the selection FS (the push's flat colour).
 *
 * Four pipelines: see-through fill and edges (GREATER, alpha x kSeeThroughAlpha), then the
 * visible fill and edges (LEQUAL; edges kEdgeWidth px where the device has wideLines). All four
 * share ONE depth bias toward the camera: the visible / see-through boundary moves for both,
 * so a shape lying on its own prim draws each fragment exactly once. They are written in
 * conventional depth terms (GraphicsPipelineDesc::reverseZ mirrors the compare and the bias
 * sign). Edges are polygonMode LINE over the same triangles.
 *
 * The buffers (2 x 64 KB rows, 2 x 80 KB commands at LLVKPhysShapes::kRowCap) and the
 * pipelines are made on first use -- the pipelines as a background batch, nothing drawn until
 * they land -- and kept (high water). Off in stereo, like the selection outline.
 */

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "llcleanphysshapemesh.h"   // LLPhysShapeColor (the bucket colours)
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
constexpr VkDeviceSize kDrawCmdBytes = sizeof(VkDrawIndexedIndirectCommand);   // DrawCmd (20 B)
constexpr VkDeviceSize kRowBytes     = sizeof(LLVKPhysShapeRow);              // 16 B
constexpr VkDeviceSize kInstanceBytes = 224;   // Instance (llcleanrenderervk.cpp)
constexpr VkDeviceSize kSlotInfoBytes = 32;    // SlotInfo (llcleanrenderervk.cpp)
constexpr float kSeeThroughAlpha = 0.4f;       // the shape's hidden parts, dimmed
constexpr float kEdgeWidth = 3.f;              // the visible edges (px; 1 without wideLines)
constexpr float kDepthBias = -1.f;             // toward the camera, constant and slope (as mSelVisPipe)
constexpr double kBufferRetrySec = 2.0;

enum PhysPass { PASS_HIDDEN_FILL, PASS_HIDDEN_EDGES, PASS_FILL, PASS_EDGES, PASS_COUNT };

// clean_phys_shape.comp PhysPush.
struct PhysPush
{
    uint32_t rows[2];
    uint32_t inst[2];
    uint32_t slots[2];
    uint32_t cmds[2];
    uint32_t n_rows, n_inst, n_slots, pad;
};
static_assert(sizeof(PhysPush) == 48, "PhysPush must match clean_phys_shape.comp");

// The bucket colours: fixed, so built once (LL's cost ramp in LLPhysShapeColor's steps).
struct PhysShapeColours
{
    float fill[LLVKPhysShapes::kColors][4];
    float edge[LLVKPhysShapes::kColors][4];
};

const PhysShapeColours& physShapeColours()
{
    static const PhysShapeColours colours = []
    {
        PhysShapeColours c{};
        for (uint32_t i = 0; i < LLVKPhysShapes::kColors; ++i)
        {
            LLPhysShapeColor::fill(i, c.fill[i]);
            LLPhysShapeColor::edge(i, c.edge[i]);
        }
        return c;
    }();
    return colours;
}

double steadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

VkShaderModule physModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void putAddress(VkDevice dev, VkBuffer b, uint32_t out[2])
{
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b;
    const VkDeviceAddress a = vkGetBufferDeviceAddress(dev, &ai);
    out[0] = (uint32_t)(a & 0xFFFFFFFFull);
    out[1] = (uint32_t)(a >> 32);
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace

struct LLCleanPhysShapeVK
{
    GpuBuffer rows[2]{};        // host-visible: the CPU writes a frame slot's when the rows change
    GpuBuffer cmds[2]{};        // device-local: clean_phys_shape.comp writes them every frame
    uint64_t  rowsGen[2] = {~0ull, ~0ull};
    double    bufferRetryAt = 0.0;

    // This frame (setPhysShapes).
    uint32_t count = 0;
    bool     recorded = false;   // this recordWorld's commands were written (recordPhysShapeCmds)
    std::shared_ptr<const LLVKPhysShapes::Set> set;   // its bucket ranges (rows as uploaded)

    VkPipeline       draw[PASS_COUNT] = {};
    VkPipeline       fixup = VK_NULL_HANDLE;
    VkPipelineLayout fixupLayout = VK_NULL_HANDLE;
    VkPipeline       stage[PASS_COUNT + 1] = {};   // the background batch's outputs (fixup last)
    PipelineJobSlot  job;
    VkFormat         format = VK_FORMAT_UNDEFINED;
    bool             pipesFailed = false;

    void destroyPipelines(VkDevice dev)
    {
        job.wait();
        for (VkPipeline& p : stage) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        for (VkPipeline& p : draw) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        if (fixup) vkDestroyPipeline(dev, fixup, nullptr);
        fixup = VK_NULL_HANDLE;
    }
};

void LLCleanRendererVK::setPhysShapes(const LLVKWorldFrame& wf)
{
    const LLVKPhysShapes& in = wf.phys;
    const uint32_t count = in.set ? (uint32_t)std::min<size_t>(in.set->rows.size(), LLVKPhysShapes::kRowCap) : 0u;
    if (!mPhysShape)
    {
        if (!count || !mCtx) return;   // never used: nothing made
        mPhysShape = new LLCleanPhysShapeVK();
    }
    LLCleanPhysShapeVK& s = *mPhysShape;
    s.count = 0;
    s.set.reset();
    if (!count || !physShapeEnsure()) return;
    const uint32_t slot = mFrameSlot & 1u;
    if (s.rowsGen[slot] != in.gen)
    {
        std::memcpy(s.rows[slot].mapped, in.set->rows.data(), count * kRowBytes);
        s.rowsGen[slot] = in.gen;
    }
    s.count = count;
    s.set = in.set;
}

bool LLCleanRendererVK::physShapeEnsure()
{
    LLCleanPhysShapeVK& s = *mPhysShape;
    VkDevice dev = mCtx->device();

    if (!s.cmds[1].buffer)
    {
        const double now = steadySeconds();
        if (now < s.bufferRetryAt) return false;
        {
            VkDeviceContext::SoftAllocScope soft(mCtx);   // a refused allocation is retried, never fatal
            VkMemScope mem(VKMEM_CULL);                    // (indirect command buffers)
            for (int r = 0; r < 2; ++r)
            {
                s.rows[r] = mCtx->createBuffer(LLVKPhysShapes::kRowCap * kRowBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                s.cmds[r] = mCtx->createBuffer(LLVKPhysShapes::kRowCap * kDrawCmdBytes,
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            }
        }
        bool ok = true;
        for (int r = 0; r < 2; ++r) ok = ok && s.rows[r].buffer && s.rows[r].mapped && s.cmds[r].buffer;
        if (!ok)
        {
            for (int r = 0; r < 2; ++r)
            {
                if (s.rows[r].buffer) mCtx->destroyBuffer(s.rows[r]);
                if (s.cmds[r].buffer) mCtx->destroyBuffer(s.cmds[r]);
                s.rows[r] = GpuBuffer{};
                s.cmds[r] = GpuBuffer{};
            }
            s.bufferRetryAt = now + kBufferRetrySec;
            LL_WARNS_ONCE("VKPhysShape") << "physics-shape buffers ("
                << 2 * LLVKPhysShapes::kRowCap * (kRowBytes + kDrawCmdBytes) / 1024
                << " KB) refused; retrying every " << kBufferRetrySec << " s" << LL_ENDL;
            return false;
        }
        s.rowsGen[0] = s.rowsGen[1] = ~0ull;
    }

    if (s.job.pending())
    {
        if (!s.job.poll()) return false;   // still compiling
        for (int p = 0; p < PASS_COUNT; ++p) { s.draw[p] = s.stage[p]; s.stage[p] = VK_NULL_HANDLE; }
        s.fixup = s.stage[PASS_COUNT];
        s.stage[PASS_COUNT] = VK_NULL_HANDLE;
        bool ok = s.fixup != VK_NULL_HANDLE;
        for (VkPipeline p : s.draw) ok = ok && p;
        if (!ok)
        {
            s.destroyPipelines(dev);
            s.pipesFailed = true;
            LL_WARNS("VKPhysShape") << "physics-shape pipelines failed to build; the shapes are not drawn" << LL_ENDL;
            return false;
        }
        LL_INFOS("VKPhysShape") << "physics-shape pipelines built (background)" << LL_ENDL;
    }
    if (s.fixup && s.format == mColorFormat) return true;
    if (s.pipesFailed && s.format == mColorFormat) return false;
    if (s.fixup || s.draw[0])
    {
        vkDeviceWaitIdle(dev);   // the colour format changed (rare): rebuild
        s.destroyPipelines(dev);
    }
    s.format = mColorFormat;
    s.pipesFailed = false;

    PipelineBatchScope batch(dev);   // (queued here, compiled in the background)
    if (!s.fixupLayout)
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PhysPush)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pr;
        if (vkdCreatePipelineLayout(dev, &li, &s.fixupLayout, "mPhysShapeFixupLayout") != VK_SUCCESS)
        {
            s.fixupLayout = VK_NULL_HANDLE;
            s.pipesFailed = true;
            return false;
        }
    }
    VkShaderModule cs = physModule(dev, kWorldPhysShapeSpv, kWorldPhysShapeSpv_bytes, "kWorldPhysShapeSpv");
    VkShaderModule vs = physModule(dev, kWorldVertSpv, kWorldVertSpv_bytes, "kWorldVertSpv");
    VkShaderModule fs = physModule(dev, kWorldSelectFragSpv, kWorldSelectFragSpv_bytes, "kWorldSelectFragSpv");
    if (cs && vs && fs)
    {
        createComputePipelineInto(dev, cs, s.fixupLayout, 0, nullptr, &s.stage[PASS_COUNT], "physics shape draws");
        // The lit pass's vertex input, depth format and reversed-Z with every attachment
        // and blend extra stripped, as the selection pipelines: world VS, selection FS.
        GraphicsPipelineDesc d = mLitDesc;
        d.views = 1;   // (#1141: one view -- VR draws it per eye; the desktop's mLitDesc is 1 already)
        d.task = d.mesh = VK_NULL_HANDLE;
        d.glowFormat = VK_FORMAT_UNDEFINED; d.glowWrite = false;
        d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
        d.mrtCount = 0;
        d.colorWrite = true; d.noColor = false;
        d.additive = false; d.customBlend = false; d.partAccum = false; d.glowAdditive = false;
        d.fragSpec = nullptr; d.fsrAttachment = false; d.dynamicShadingRate = false;
        d.vert = vs; vkWorldStaticVI(d);
        d.frag = fs;
        d.layout = mDrawLayout;
        d.colorFormat = mColorFormat;
        d.blend = true;
        d.depthTest = true; d.depthWrite = false; d.depthClamp = false;
        d.cullMode = VK_CULL_MODE_NONE;
        d.depthBiasConstant = kDepthBias; d.depthBiasSlope = kDepthBias;
        static const char* const kNames[PASS_COUNT] = {"physics shape see-through fill", "physics shape see-through edges",
                                                       "physics shape fill", "physics shape edges"};
        for (int p = 0; p < PASS_COUNT; ++p)
        {
            const bool edges = p == PASS_HIDDEN_EDGES || p == PASS_EDGES;
            d.depthCompare = p < PASS_FILL ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS_OR_EQUAL;
            d.polygonMode = edges ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
            d.lineWidth = p == PASS_EDGES && mCtx->wideLinesEnabled()
                        ? std::max(1.f, std::min(kEdgeWidth, mCtx->lineWidthMax())) : 1.f;
            createGraphicsPipelineInto(dev, d, &s.stage[p], kNames[p]);
        }
    }
    else
    {
        s.pipesFailed = true;
    }
    for (VkShaderModule m : {cs, vs, fs}) if (m) destroyShaderModuleDeferred(dev, m, nullptr);
    if (s.pipesFailed) return false;
    PipelineJobOpts o;
    o.prio = VKPIPE_PRIO_USER;   // the user turned it on and is looking
    o.gate = false;
    o.what = "physics shapes";
    s.job.start(pipelineBatchFlushAsync(dev, o));
    return false;
}

bool LLCleanRendererVK::physShapesWanted() const
{
    return mPhysShape && mPhysShape->recorded;
}

void LLCleanRendererVK::recordPhysShapeCmds(VkCommandBuffer cmd)
{
    if (!mPhysShape) return;
    LLCleanPhysShapeVK& s = *mPhysShape;
    s.recorded = false;
    // (#1075: not multiview, like the selection outline: off in stereo)
    if (!s.count || !s.fixup || !mInstanceSSBO.buffer || !mSlotSSBO.buffer) return;
    const uint32_t slot = mFrameSlot & 1u;
    VkDevice dev = mCtx->device();
    PhysPush pp{};
    putAddress(dev, s.rows[slot].buffer, pp.rows);
    putAddress(dev, mInstanceSSBO.buffer, pp.inst);
    putAddress(dev, mSlotSSBO.buffer, pp.slots);
    putAddress(dev, s.cmds[slot].buffer, pp.cmds);
    pp.n_rows = s.count;
    pp.n_inst = (uint32_t)(mInstanceSSBO.size / kInstanceBytes);
    pp.n_slots = (uint32_t)(mSlotSSBO.size / kSlotInfoBytes);
    // (the instance / slot uploads are visible to compute since the main cull; this only
    // orders the rewrite after any earlier frame's indirect reads of these commands)
    memBarrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, 0,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fixup);
    VK_PUSH_BUDGET_CHECK(pp);
    vkCmdPushConstants(cmd, s.fixupLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
    vkCmdDispatch(cmd, (s.count + 63u) / 64u, 1, 1);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    s.recorded = true;
}

void LLCleanRendererVK::recordPhysShapes(VkCommandBuffer cmd, const VKDrawPush& push)
{
    if (!physShapesWanted()) return;
    LLCleanPhysShapeVK& s = *mPhysShape;
    s.recorded = false;   // (consumed: a later recordWorld writes its own)
    const VkBuffer cmds = s.cmds[mFrameSlot & 1u].buffer;
    const PhysShapeColours& colours = physShapeColours();
    const LLVKPhysShapes::Set& set = *s.set;
    for (int pass = 0; pass < PASS_COUNT; ++pass)
    {
        const bool edges = pass == PASS_HIDDEN_EDGES || pass == PASS_EDGES;
        bool bound = false;
        for (uint32_t b = 0; b < LLVKPhysShapes::kBuckets; ++b)
        {
            // Only a bucket with rows costs a draw (within the rows uploaded: s.count).
            const uint32_t first = std::min(set.first[b], s.count);
            const uint32_t n = std::min(set.n[b], s.count - first);
            if (!n || (edges && b >= LLVKPhysShapes::kColors)) continue;   // (filled-only buckets)
            if (!bound)
            {
                bindGraphicsPipeline(cmd, s.draw[pass]);
                bound = true;
            }
            const uint32_t color = b % LLVKPhysShapes::kColors;
            VKDrawPush p = push;
            // Camera-relative like the main view's draws (#555), so the shapes land on the same depths.
            std::memcpy(p.view_proj, selViewProjRel(), sizeof(p.view_proj));
            p.origin[0] = push.cam_time[0]; p.origin[1] = push.cam_time[1];
            p.origin[2] = push.cam_time[2]; p.origin[3] = 0.f;
            std::memcpy(p.water_fog, edges ? colours.edge[color] : colours.fill[color], sizeof(p.water_fog));   // (the colour)
            if (pass < PASS_FILL) p.water_fog[3] *= kSeeThroughAlpha;
            pushDrawView(cmd, mDrawLayout, p);
            vkCmdDrawIndexedIndirect(cmd, cmds, first * kDrawCmdBytes, n, (uint32_t)kDrawCmdBytes);
        }
    }
}

void LLCleanRendererVK::destroyPhysShapes()
{
    if (!mPhysShape) return;
    LLCleanPhysShapeVK& s = *mPhysShape;
    if (mCtx)
    {
        VkDevice dev = mCtx->device();
        vkDeviceWaitIdle(dev);
        s.destroyPipelines(dev);
        if (s.fixupLayout) vkDestroyPipelineLayout(dev, s.fixupLayout, nullptr);
        for (int r = 0; r < 2; ++r)
        {
            if (s.rows[r].buffer) mCtx->destroyBuffer(s.rows[r]);
            if (s.cmds[r].buffer) mCtx->destroyBuffer(s.cmds[r]);
        }
    }
    delete mPhysShape;
    mPhysShape = nullptr;
}
