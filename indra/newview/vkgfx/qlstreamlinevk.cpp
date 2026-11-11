// Quasar: NVIDIA Streamline (Reflex, DLSS Frame Generation). See qlstreamlinevk.h.
#ifndef QL_SYNTAX_CHECK
#include "linden_common.h"
#include "llerror.h"
#define QL_SL_INFO(x) LL_INFOS("Vulkan") << x << LL_ENDL
#define QL_SL_WARN(x) LL_WARNS("Vulkan") << x << LL_ENDL
#else
#include <iostream>
#define QL_SL_INFO(x) (std::cerr << x << "\n")
#define QL_SL_WARN(x) (std::cerr << x << "\n")
#endif

#include "qlstreamlinevk.h"
#include "qlframegen_spv.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32) && defined(QL_HAS_STREAMLINE)
#define QL_SL 1
#include <windows.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_dlss_g.h>
#ifndef QL_SYNTAX_CHECK
#include <sl_security.h>
#endif
#else
#define QL_SL 0
#endif

namespace qlsl
{
namespace
{
bool sWanted = false;
std::string sNote = "off (QuasarStreamline)";
std::atomic<uint32_t> sGenerated{0};

// ---- the inputs' images and the pass that writes the motion + depth ----
struct Img
{
    GpuImage img;
    VkImageUsageFlags usage = 0;
};
struct InputsState
{
    VkDeviceContext* ctx = nullptr;
    uint32_t w = 0, h = 0;
    VkFormat hudFormat = VK_FORMAT_UNDEFINED;
    Img motion, depth, hud;
    VkSampler nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set[4] = {};
    bool firstUse = true;   // the images are UNDEFINED until their first write
} sIn;

constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;

void barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void destroyImages(VkDeviceContext& ctx)
{
    for (Img* i : {&sIn.motion, &sIn.depth, &sIn.hud})
        if (i->img.image) { ctx.destroyImage(i->img); i->img = GpuImage{}; }
    sIn.w = sIn.h = 0;
    sIn.firstUse = true;
}

bool ensurePipeline(VkDeviceContext& ctx)
{
    if (sIn.pipe) return true;
    VkDevice dev = ctx.device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!sIn.nearest && vkCreateSampler(dev, &si, nullptr, &sIn.nearest) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &sIn.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &sIn.dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &sIn.layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 4; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &sIn.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout ls[4] = {sIn.dsl, sIn.dsl, sIn.dsl, sIn.dsl};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = sIn.pool; ai.descriptorSetCount = 4; ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(dev, &ai, sIn.set) != VK_SUCCESS) return false;

    auto module = [&](const uint32_t* code, size_t bytes)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes; ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule vs = module(kQLFgFullscreenVert, sizeof(kQLFgFullscreenVert));
    VkShaderModule fs = module(kQLFgInputsFrag, sizeof(kQLFgInputsFrag));
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(dev, fs, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba[2]{};
    ba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    ba[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 2; cb.pAttachments = ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    const VkFormat fmts[2] = {kMotionFormat, kDepthFormat};
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 2; ri.pColorAttachmentFormats = fmts;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = sIn.layout;
    const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &sIn.pipe);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (r != VK_SUCCESS) { sIn.pipe = VK_NULL_HANDLE; return false; }
    return true;
}

bool ensureImages(VkDeviceContext& ctx, uint32_t w, uint32_t h, VkFormat hudFormat)
{
    sIn.ctx = &ctx;
    if (sIn.w == w && sIn.h == h && sIn.hudFormat == hudFormat && sIn.motion.img.image) return true;
    vkDeviceWaitIdle(ctx.device());
    destroyImages(ctx);
    const VkImageUsageFlags common = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sIn.motion.usage = common | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sIn.depth.usage = common | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sIn.hud.usage = common | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sIn.motion.img = ctx.createImage2D(w, h, kMotionFormat, sIn.motion.usage, VK_IMAGE_ASPECT_COLOR_BIT);
    sIn.depth.img = ctx.createImage2D(w, h, kDepthFormat, sIn.depth.usage, VK_IMAGE_ASPECT_COLOR_BIT);
    sIn.hud.img = ctx.createImage2D(w, h, hudFormat, sIn.hud.usage, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!sIn.motion.img.image || !sIn.depth.img.image || !sIn.hud.img.image)
    {
        destroyImages(ctx);
        return false;
    }
    sIn.w = w; sIn.h = h; sIn.hudFormat = hudFormat;
    return true;
}

// ---- Streamline ----
#if QL_SL
HMODULE sInterposer = nullptr;
bool sRunning = false;
bool sFeaturesStarted = false;
PFun_slInit* slInit = nullptr;
PFun_slShutdown* slShutdown = nullptr;
PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
PFun_slSetConstants* slSetConstants = nullptr;
PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
PFun_slReflexSleep* slReflexSleep = nullptr;
PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;
PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
sl::FrameToken* sToken = nullptr;
bool sFGLoaded = false;
bool sFGWanted = false;
bool sFGOn = false;
bool sFGFailed = false;
bool sFGArmed = false;   // frame generation may run for the frame being recorded
bool sRebuild = false;
int  sReflexMode = -1;
bool sInputsThisFrame = false;
uint32_t sSwapImages = 0, sSwapW = 0, sSwapH = 0;
VkFormat sSwapFormat = VK_FORMAT_UNDEFINED;
uint32_t sFramesSinceSwapchain = 0;
uint32_t sFrame = 0;
bool sLoggedTags = false, sLoggedConstants = false;
std::wstring sDir;

void slLog(sl::LogType type, const char* msg)
{
    std::string m(msg ? msg : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    if (type == sl::LogType::eError) QL_SL_WARN("Streamline: " << m);
    else if (type == sl::LogType::eWarn) QL_SL_INFO("Streamline (warning): " << m);
}

sl::ViewportHandle viewport() { return sl::ViewportHandle(0u); }

void setFrameGenMode(bool on)
{
    if (!slDLSSGSetOptions) return;
    sl::DLSSGOptions o{};
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = 1;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    o.numBackBuffers = sSwapImages;
    o.mvecDepthWidth = sSwapW;
    o.mvecDepthHeight = sSwapH;
    o.colorWidth = sSwapW;
    o.colorHeight = sSwapH;
    o.colorBufferFormat = (uint32_t)sSwapFormat;
    o.mvecBufferFormat = (uint32_t)kMotionFormat;
    o.depthBufferFormat = (uint32_t)kDepthFormat;
    o.hudLessBufferFormat = (uint32_t)sSwapFormat;
    const sl::Result r = slDLSSGSetOptions(viewport(), o);
    if (r != sl::Result::eOk)
    {
        QL_SL_WARN("DLSS Frame Generation " << (on ? "on" : "off") << ": slDLSSGSetOptions failed (sl::Result "
                   << (int)r << ")");
        if (on) { sFGFailed = true; sNote = "DLSS Frame Generation would not switch on"; return; }
    }
    sFGOn = on;
    QL_SL_INFO("DLSS Frame Generation " << (on ? "on" : "off") << ", " << sSwapW << "x" << sSwapH);
}

void mul4(const float* a, const float* b, float* out)   // column-major: out = a * b
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

bool invert4(const float* m, float* inv)
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
    const float det = m[0]*t[0] + m[1]*t[4] + m[2]*t[8] + m[3]*t[12];
    if (std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] / det;
    return true;
}

void setConstants(const Camera& cam)
{
    // Streamline's matrices are row-vector (v * M): a column-major column-vector matrix
    // read row by row is exactly that.
    auto matrix = [](sl::float4x4& dst, const float* m)
    {
        for (uint32_t row = 0; row < 4; ++row)
            dst.setRow(row, sl::float4(m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]));
    };
    float invProj[16], prevToCur[16], invView[16];
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!invert4(cam.proj, invProj)) std::memcpy(invProj, identity, sizeof(invProj));
    if (!invert4(cam.reproj, prevToCur)) std::memcpy(prevToCur, identity, sizeof(prevToCur));
    if (!invert4(cam.view, invView)) std::memcpy(invView, identity, sizeof(invView));
    sl::Constants c{};
    matrix(c.cameraViewToClip, cam.proj);
    matrix(c.clipToCameraView, invProj);
    matrix(c.clipToPrevClip, cam.reproj);
    matrix(c.prevClipToClip, prevToCur);
    matrix(c.clipToLensClip, identity);
    c.jitterOffset = sl::float2(0.f, 0.f);       // the inputs are the resolved (unjittered) frame
    c.mvecScale = sl::float2(1.f, 1.f);          // ql_fg_inputs.frag writes UV offsets already
    c.cameraPinholeOffset = sl::float2(0.f, 0.f);
    // Camera axes from the inverse view (columns: right, up, back, position).
    c.cameraPos = sl::float3(invView[12], invView[13], invView[14]);
    c.cameraRight = sl::float3(invView[0], invView[1], invView[2]);
    c.cameraUp = sl::float3(invView[4], invView[5], invView[6]);
    c.cameraFwd = sl::float3(-invView[8], -invView[9], -invView[10]);
    c.cameraNear = 0.1f;
    c.cameraFar = 1024.f;
    c.cameraFOV = cam.proj[5] != 0.f ? 2.f * std::atan(1.f / std::fabs(cam.proj[5])) : 1.f;
    c.cameraAspectRatio = (cam.proj[0] != 0.f) ? std::fabs(cam.proj[5] / cam.proj[0]) : 1.f;
    c.depthInverted = cam.reversedZ ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = cam.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    const sl::Result r = slSetConstants(c, *sToken, viewport());
    if (r != sl::Result::eOk && !sLoggedConstants)
    {
        QL_SL_WARN("DLSS Frame Generation: slSetConstants failed (sl::Result " << (int)r << ")");
        sLoggedConstants = true;
    }
}

void tag(VkCommandBuffer cmd)
{
    auto res = [](const Img& i, VkFormat f)
    {
        sl::Resource r(sl::ResourceType::eTex2d, (void*)i.img.image, (void*)i.img.memory, (void*)i.img.view,
                       (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
        r.width = i.img.extent.width;
        r.height = i.img.extent.height;
        r.nativeFormat = (uint32_t)f;
        r.mipLevels = 1;
        r.arrayLayers = 1;
        r.usage = (uint32_t)i.usage;
        return r;
    };
    sl::Resource depth = res(sIn.depth, kDepthFormat);
    sl::Resource motion = res(sIn.motion, kMotionFormat);
    sl::Resource hud = res(sIn.hud, sIn.hudFormat);
    const sl::Extent ext{0, 0, sIn.w, sIn.h};
    const sl::ResourceTag tags[3] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &ext),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &ext),
        sl::ResourceTag(&hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &ext),
    };
    const sl::Result r = slSetTagForFrame(*sToken, viewport(), tags, 3, reinterpret_cast<sl::CommandBuffer*>(cmd));
    if (r != sl::Result::eOk && !sLoggedTags)
    {
        QL_SL_WARN("DLSS Frame Generation: slSetTagForFrame failed (sl::Result " << (int)r << ")");
        sLoggedTags = true;
    }
}
#endif // QL_SL
} // namespace

void setWanted(bool streamline) { sWanted = streamline; }
bool wanted() { return sWanted; }
const char* note() { return sNote.c_str(); }

void* bootInterposer()
{
#if QL_SL
    if (sInterposer) return sInterposer;
    if (!sWanted) { sNote = "off (QuasarStreamline)"; return nullptr; }
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    sDir.assign(exe, n);
    const size_t slash = sDir.find_last_of(L"\\/");
    sDir = slash == std::wstring::npos ? std::wstring(L".") : sDir.substr(0, slash);
    const std::wstring path = sDir + L"\\sl.interposer.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        sNote = "sl.interposer.dll is not beside the viewer";
        return nullptr;
    }
#ifndef QL_SYNTAX_CHECK
    if (!sl::security::verifyEmbeddedSignature(path.c_str()))
    {
        sNote = "sl.interposer.dll does not carry NVIDIA's signature";
        return nullptr;
    }
#endif
    HMODULE h = LoadLibraryW(path.c_str());
    if (!h) { sNote = "sl.interposer.dll did not load"; return nullptr; }
    slInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(h, "slInit"));
    slShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(h, "slShutdown"));
    slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(h, "slGetFeatureFunction"));
    slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(h, "slGetNewFrameToken"));
    slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(h, "slSetTagForFrame"));
    slSetConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(h, "slSetConstants"));
    if (!slInit || !slShutdown || !slGetFeatureFunction || !slGetNewFrameToken || !GetProcAddress(h, "vkCreateInstance"))
    {
        sNote = "sl.interposer.dll lacks the functions it should have";
        FreeLibrary(h);
        return nullptr;
    }
    static const sl::Feature features[] = {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G};
    static const wchar_t* dirs[1];
    dirs[0] = sDir.c_str();
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = dirs;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = slLog;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "Quasar Viewer (Slipstream)";
    pref.projectId = "3f1c6e9a-5b2d-4c7e-9a41-8d0b6f2e7c53";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk)
    {
        sNote = "slInit failed (sl::Result " + std::to_string((int)r) + ")";
        FreeLibrary(h);
        return nullptr;
    }
    sInterposer = h;
    sRunning = true;
    sNote = "running";
    return h;
#else
    sNote = "not in this build (Windows with the Streamline SDK only)";
    return nullptr;
#endif
}

bool running()
{
#if QL_SL
    return sRunning;
#else
    return false;
#endif
}

void startFeatures()
{
#if QL_SL
    if (!sRunning || sFeaturesStarted) return;
    sFeaturesStarted = true;
    auto get = [](sl::Feature f, const char* name) -> void*
    {
        void* fn = nullptr;
        return slGetFeatureFunction(f, name, fn) == sl::Result::eOk ? fn : nullptr;
    };
    slReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(get(sl::kFeatureReflex, "slReflexSetOptions"));
    slReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(get(sl::kFeatureReflex, "slReflexSleep"));
    slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(get(sl::kFeaturePCL, "slPCLSetMarker"));
    slDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(get(sl::kFeatureDLSS_G, "slDLSSGSetOptions"));
    slDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState*>(get(sl::kFeatureDLSS_G, "slDLSSGGetState"));
    sFGLoaded = slDLSSGSetOptions && slDLSSGGetState && slSetTagForFrame && slSetConstants && slPCLSetMarker &&
                slReflexSetOptions;
    QL_SL_INFO("Streamline features: Reflex " << (slReflexSetOptions && slReflexSleep ? "loaded" : "not loaded")
               << ", PC Latency markers " << (slPCLSetMarker ? "loaded" : "not loaded")
               << ", DLSS Frame Generation " << (sFGLoaded ? "loaded" : "not loaded"));
#endif
}

void frameBegin(const FrameSettings& s)
{
#if QL_SL
    if (!sRunning) return;
    if (!sFeaturesStarted) startFeatures();
    ++sFrame;
    ++sFramesSinceSwapchain;
    // A frame recorded without the inputs (the login screen, a snapshot): off until they return.
    if (sFGOn && !sInputsThisFrame) setFrameGenMode(false);
    sInputsThisFrame = false;
    if (s.frameGen != sFGWanted)
    {
        sFGWanted = s.frameGen;
        sFGFailed = false;
        if (sFGLoaded) sRebuild = true;   // Streamline: a new swapchain whenever frame generation is switched
        QL_SL_INFO("DLSS Frame Generation " << (s.frameGen ? "wanted" : "not wanted"));
    }
    sToken = nullptr;
    if (slGetNewFrameToken(sToken, nullptr) != sl::Result::eOk) { sToken = nullptr; return; }
    int mode = s.reflexMode < 0 ? 0 : (s.reflexMode > 2 ? 2 : s.reflexMode);
    if (sFGWanted && sFGLoaded && mode == 0) mode = 1;   // frame generation paces itself through Reflex
    if (mode != sReflexMode && slReflexSetOptions)
    {
        sl::ReflexOptions o{};
        o.mode = mode == 2 ? sl::ReflexMode::eLowLatencyWithBoost
                           : (mode == 1 ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff);
        const sl::Result r = slReflexSetOptions(o);
        QL_SL_INFO("Reflex " << (mode == 2 ? "on + boost" : (mode == 1 ? "on" : "off"))
                   << (r == sl::Result::eOk ? "" : " (failed)"));
        sReflexMode = mode;
    }
    if (sReflexMode > 0 && slReflexSleep) slReflexSleep(*sToken);
    if (slPCLSetMarker) slPCLSetMarker(sl::PCLMarker::eSimulationStart, *sToken);

    // Frame generation's state for this frame, decided before the frame is recorded.
    bool want = sFGWanted && sFGLoaded && !sFGFailed;
    if (want && s.vsync)
    {
        static bool logged = false;
        if (!logged) { QL_SL_INFO("DLSS Frame Generation waits: VSync is on, and under Vulkan it needs it off"); logged = true; }
        want = false;
    }
    if (want && (sFramesSinceSwapchain < 3 || !sSwapImages)) want = false;   // a new swapchain settles first
    if (!want && sFGOn) setFrameGenMode(false);
    sFGArmed = want;
    // Generated frames Streamline actually showed (FPS*), now and then.
    if (sFGOn && slDLSSGGetState)
    {
        // Each frame: how many frames the last present actually showed (2 = one generated).
        sl::DLSSGState st{};
        if (slDLSSGGetState(viewport(), st, nullptr) == sl::Result::eOk)
        {
            if (st.numFramesActuallyPresented > 1)
                sGenerated.fetch_add(st.numFramesActuallyPresented - 1, std::memory_order_relaxed);
            if ((sFrame % 600) == 0)
                QL_SL_INFO("DLSS Frame Generation state: " << st.numFramesActuallyPresented
                           << " frames shown per real frame, status " << (int)st.status
                           << ", minimum size " << st.minWidthOrHeight);
        }
    }
#else
    (void)s;
#endif
}

void marker(Marker m)
{
#if QL_SL
    if (!sRunning || !sToken || !slPCLSetMarker) return;
    sl::PCLMarker k = sl::PCLMarker::eSimulationEnd;
    switch (m)
    {
    case M_SIM_END: k = sl::PCLMarker::eSimulationEnd; break;
    case M_RENDER_SUBMIT_START: k = sl::PCLMarker::eRenderSubmitStart; break;
    case M_RENDER_SUBMIT_END: k = sl::PCLMarker::eRenderSubmitEnd; break;
    case M_PRESENT_START: k = sl::PCLMarker::ePresentStart; break;
    case M_PRESENT_END: k = sl::PCLMarker::ePresentEnd; break;
    }
    slPCLSetMarker(k, *sToken);
#else
    (void)m;
#endif
}

bool takeSwapchainRebuild()
{
#if QL_SL
    const bool r = sRebuild;
    sRebuild = false;
    return r;
#else
    return false;
#endif
}

void swapchainChanged(uint32_t imageCount, uint32_t w, uint32_t h, VkFormat format)
{
#if QL_SL
    sSwapImages = imageCount; sSwapW = w; sSwapH = h; sSwapFormat = format;
    sFramesSinceSwapchain = 0;
    sFGOn = false;   // switched on again for the new chain once it settles
#else
    (void)imageCount; (void)w; (void)h; (void)format;
#endif
}

bool recordInputs(VkDeviceContext& ctx, VkCommandBuffer cmd, const Inputs& in, const Camera& cam)
{
#if QL_SL
    if (!sRunning || !sFGArmed || !sToken || in.slot >= 4) return false;
    if (in.w != sSwapW || in.h != sSwapH) return false;
    if (!ensurePipeline(ctx) || !ensureImages(ctx, in.w, in.h, in.swapFormat)) return false;

    const VkImageLayout from = sIn.firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    // HUD-less colour: the swap image as the world left it.
    barrier(cmd, in.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, sIn.hud.img.image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cr{};
    cr.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cr.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cr.extent = {in.w, in.h, 1};
    vkCmdCopyImage(cmd, in.swapImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sIn.hud.img.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cr);
    barrier(cmd, in.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(cmd, sIn.hud.img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);

    // Motion + depth at the display size.
    const bool stencil = in.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || in.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT;
    const VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    const VkPipelineStageFlags2 depthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier(cmd, in.depthImage, da, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, depthStages,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (Img* i : {&sIn.motion, &sIn.depth})
        barrier(cmd, i->img.image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    const bool motion = in.motionView != VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[2]{};
    ii[0] = {sIn.nearest, motion ? in.motionView : in.depthView,
             motion ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    ii[1] = {sIn.nearest, in.depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sIn.set[in.slot];
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(ctx.device(), 2, wr, 0, nullptr);
    VkRenderingAttachmentInfo ca[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = i == 0 ? sIn.motion.img.view : sIn.depth.img.view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {in.w, in.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0.f, 0.f, (float)in.w, (float)in.h, 0.f, 1.f};
    VkRect2D sc{{0, 0}, {in.w, in.h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sIn.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sIn.layout, 0, 1, &sIn.set[in.slot], 0, nullptr);
    const float push[4] = {1.f / (float)in.w, 1.f / (float)in.h, motion ? 1.f : 0.f, in.flipMotionY ? 1.f : 0.f};
    vkCmdPushConstants(cmd, sIn.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    barrier(cmd, in.depthImage, da, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    for (Img* i : {&sIn.motion, &sIn.depth})
        barrier(cmd, i->img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    sIn.firstUse = false;

    // Streamline: the frame's camera, its inputs (it copies them in this command buffer),
    // and frame generation on for it.
    setConstants(cam);
    tag(cmd);
    if (!sFGOn) setFrameGenMode(true);
    sInputsThisFrame = true;
    return true;
#else
    (void)ctx; (void)cmd; (void)in; (void)cam;
    return false;
#endif
}

bool frameGenOn()
{
#if QL_SL
    return sRunning && sFGOn;
#else
    return false;
#endif
}

uint32_t takeGeneratedFrames() { return sGenerated.exchange(0, std::memory_order_relaxed); }

void destroy(VkDeviceContext& ctx)
{
    VkDevice dev = ctx.device();
    if (!dev) return;
    destroyImages(ctx);
    if (sIn.pipe) vkDestroyPipeline(dev, sIn.pipe, nullptr);
    if (sIn.layout) vkDestroyPipelineLayout(dev, sIn.layout, nullptr);
    if (sIn.dsl) vkDestroyDescriptorSetLayout(dev, sIn.dsl, nullptr);
    if (sIn.pool) vkDestroyDescriptorPool(dev, sIn.pool, nullptr);
    if (sIn.nearest) vkDestroySampler(dev, sIn.nearest, nullptr);
    sIn = InputsState{};
}

void shutdown()
{
#if QL_SL
    if (sRunning && slShutdown) slShutdown();
    sRunning = false;
#endif
}
} // namespace qlsl

#include "qlframegen_settings.h"
void qlSetStreamlineWanted(bool on) { qlsl::setWanted(on); }
