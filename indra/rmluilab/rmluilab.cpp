// #703 slrmluilab: offscreen harness for the RmlUi Vulkan backend (vkgfx/llrmluivk.cpp).
//
// Renders RML documents through the real backend onto a synthetic "world" image (headless
// Vulkan device, no window, no viewer), writes PNGs, and times the steady state:
//   CPU  - capture (Context::Update + Render + backend end) when the document changes,
//          and record() alone when it does not (the retained replay);
//   GPU  - timestamps around record(): replay cost per frame.
// Usage: slrmluilab --fonts <dir> --out <dir> [--frames N] [--size WxH] [--validation] [doc.rml ...]
// With no documents the built-in set runs (glass proof, gradients/masks, a 400-control stress
// document). Keep a run short (< 2 min) and take the GPU lock (see feature rules) first.
#include "vk_device.h"
#include "vk_ui_surface.h"
#include "vk_ui_texture_host.h"
#include "llrmluivk.h"

#include <RmlUi/Core.h>
#include <png.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// Bindless slots of the harness (the viewer's come from the world renderer).
class HarnessTextureHost final : public VkUITextureHost
{
public:
    static constexpr uint32_t kSlots = 256;
    VkDevice              device = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl    = VK_NULL_HANDLE;
    struct Slot
    {
        Lease                 lease;
        VkDescriptorImageInfo info{};
        uint64_t              bytes = 0, lastUse = 0;
    };
    Slot     slots[kSlots]{};
    uint32_t generation = 0;
    uint64_t used       = 0;

    bool init(VkDevice dev)
    {
        device = dev;
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = kSlots;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorBindingFlags                    flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bfi.bindingCount  = 1;
        bfi.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.pNext        = &bfi;
        ci.bindingCount = 1;
        ci.pBindings    = &b;
        return vkCreateDescriptorSetLayout(device, &ci, nullptr, &dsl) == VK_SUCCESS;
    }
    void shutdown()
    {
        if (dsl)
            vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        dsl = VK_NULL_HANDLE;
    }
    Lease reserve(VkImageView view, VkSampler sampler, uint64_t bytes) noexcept override
    {
        if (!view || !sampler || used + bytes > budget())
            return {};
        // Slot 0 stays unused (the world's white texture lives there in the viewer).
        for (uint32_t i = 1; i < kSlots; ++i)
            if (!slots[i].lease.generation)
            {
                slots[i] = { { i, ++generation }, { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, bytes, 0 };
                used += bytes;
                return slots[i].lease;
            }
        return {};
    }
    Slot* find(Lease l) noexcept
    {
        return l.slot < kSlots && l.generation && slots[l.slot].lease.generation == l.generation ? &slots[l.slot] : nullptr;
    }
    bool pin(Lease l, uint64_t serial) noexcept override
    {
        Slot* s = find(l);
        if (s)
            s->lastUse = std::max(s->lastUse, serial);
        return s != nullptr;
    }
    bool release(Lease l, uint64_t completed) noexcept override
    {
        Slot* s = find(l);
        if (!s || s->lastUse > completed)
            return false;
        used -= s->bytes;
        *s = {};
        return true;
    }
    bool descriptor(Lease l, VkDescriptorImageInfo& info) const noexcept override
    {
        if (l.slot >= kSlots || !l.generation || slots[l.slot].lease.generation != l.generation)
            return false;
        info = slots[l.slot].info;
        return true;
    }
    VkDescriptorSetLayout layout() const noexcept override { return dsl; }
    uint32_t              capacity() const noexcept override { return kSlots; }
    uint64_t              bytes() const noexcept override { return used; }
    uint64_t              budget() const noexcept override { return 128ull << 20; }
};

class HarnessSystem final : public Rml::SystemInterface
{
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

public:
    double GetElapsedTime() override { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    bool   LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING)
            std::fprintf(stderr, "[RmlUi] %s\n", message.c_str());
        return true;
    }
};

const char* kGlass = R"rml(<rml><head><style>
body { width: 400dp; margin: 72dp; padding: 24dp; font-family: DejaVu Sans; font-size: 18dp; color: #ffffff;
 background-color: #17304c99; border: 1dp #a1cee388; border-radius: 18dp; backdrop-filter: blur(8px);
 box-shadow: #00000088 0dp 10dp 22dp; }
h1 { display: block; font-size: 26dp; margin: 0dp 0dp 16dp 0dp; }
p { display: block; }
img { width: 64dp; height: 64dp; margin: 12dp 0dp; }
</style></head><body><h1>Vulkan UI</h1><p>Retained RmlUi geometry over the world.</p>
<img src="rmlui:checker"/><p>Live scene blur, text and image.</p></body></rml>)rml";

const char* kEffects = R"rml(<rml><head><style>
body { font-family: DejaVu Sans; font-size: 15dp; color: #fff; }
div { display: block; position: absolute; width: 180dp; height: 110dp; border-radius: 12dp; }
#lin { left: 20dp; top: 20dp; decorator: linear-gradient(120deg, #ff4060 0%, #40a0ff 100%); }
#rad { left: 220dp; top: 20dp; decorator: radial-gradient(circle, #ffe060, #ff6000 60%, #00000000); }
#con { left: 420dp; top: 20dp; decorator: conic-gradient(#f00, #ff0, #0f0, #0ff, #00f, #f0f, #f00); }
#mask { left: 20dp; top: 160dp; background-color: #30c080; mask-image: radial-gradient(circle, #fff 40%, #fff0 70%); }
#shadow { left: 220dp; top: 160dp; background-color: #303848; box-shadow: #000a 4dp 8dp 16dp; }
#filter { left: 420dp; top: 160dp; background-color: #6040ff; filter: drop-shadow(#000 3dp 3dp 4dp) opacity(0.7); }
#clip { left: 20dp; top: 300dp; width: 560dp; height: 60dp; overflow: hidden; background-color: #222a; transform: rotate(-3deg); }
</style></head><body><div id="lin">linear</div><div id="rad">radial</div><div id="con">conic</div>
<div id="mask">mask</div><div id="shadow">box-shadow</div><div id="filter">filter</div>
<div id="clip">clipped and rotated: the quick brown fox jumps over the lazy dog, twice over the edge of its box</div>
</body></rml>)rml";

const char* kExternal = R"rml(<rml><head><style>
body { font-family: DejaVu Sans; font-size: 14dp; color: #fff; }
img { display: block; position: absolute; }
</style></head><body>
<img src="llext:lab" style="left: 20dp; top: 20dp; width: 256dp; height: 256dp;"/>
<img src="llext:lab" rect="0 0 128 128" style="left: 300dp; top: 20dp; width: 256dp; height: 256dp; transform: scaleY(-1);"/>
<img src="llext:lab" style="left: 580dp; top: 20dp; width: 128dp; height: 128dp; image-color: #ffffff80;"/>
</body></rml>)rml";

std::string stressDocument()
{
    // 400 controls: a busy floater's worth of buttons, labels and fields.
    std::string doc = R"rml(<rml><head><style>
body { font-family: DejaVu Sans; font-size: 11dp; color: #e0e0e0; }
div { display: block; position: absolute; width: 140dp; height: 20dp; border-radius: 3dp; background-color: #2a2f38e0;
 border: 1dp #50586a; text-align: center; line-height: 18dp; }
</style></head><body>)rml";
    for (int i = 0; i < 400; ++i)
        doc += "<div style=\"left:" + std::to_string(10 + (i % 8) * 150) + "dp;top:" + std::to_string(10 + (i / 8) * 24) +
               "dp\">Control " + std::to_string(i) + "</div>";
    doc += "</body></rml>";
    return doc;
}

bool writePNG(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width   = w;
    img.height  = h;
    img.format  = PNG_FORMAT_RGBA;
    return png_image_write_to_file(&img, path.c_str(), 0, rgba, 0, nullptr) != 0;
}

struct Percentiles
{
    double p10 = 0, p50 = 0, p90 = 0;
};
Percentiles percentiles(std::vector<double> v)
{
    if (v.empty())
        return {};
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]; };
    return { at(0.1), at(0.5), at(0.9) };
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
    auto scope = [](VkImageLayout l, VkPipelineStageFlags2& st, VkAccessFlags2& ac)
    {
        st = l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
             : l == VK_IMAGE_LAYOUT_UNDEFINED              ? VK_PIPELINE_STAGE_2_NONE
             : l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                           : VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        ac = l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
             : l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL   ? VK_ACCESS_2_TRANSFER_READ_BIT
             : l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL   ? VK_ACCESS_2_TRANSFER_WRITE_BIT
             : l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                                           : VK_ACCESS_2_NONE;
    };
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    scope(from, b.srcStageMask, b.srcAccessMask);
    scope(to, b.dstStageMask, b.dstAccessMask);
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo info{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &info);
}
} // namespace

int main(int argc, char** argv)
{
    std::string              fonts = "indra/newview/fonts", out = ".";
    uint32_t                 W = 1280, H = 720, frames = 240;
    bool                     validation = false, hdr = false; // --hdr: a linear-nits R16F target (paper white 350)
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--fonts" && i + 1 < argc)
            fonts = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--frames" && i + 1 < argc)
            frames = uint32_t(std::max(2, std::atoi(argv[++i])));
        else if (a == "--size" && i + 1 < argc)
            std::sscanf(argv[++i], "%ux%u", &W, &H);
        else if (a == "--hdr")
            hdr = true;
        else if (a == "--validation")
            validation = true;
        else
            files.push_back(a);
    }

    VkDeviceContext ctx;
    if (!ctx.init(validation))
    {
        std::fprintf(stderr, "no Vulkan device\n");
        return 2;
    }
    HarnessTextureHost host;
    if (!host.init(ctx.device()))
        return 3;
    {
        auto renderer = LLRmlUiVK::create(ctx, host);
        if (!renderer)
        {
            std::fprintf(stderr, "backend create failed\n");
            return 4;
        }
        HarnessSystem system;
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(renderer.get());
        Rml::Initialise();
        for (const char* face : { "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf" })
            Rml::LoadFontFace(fonts + "/" + face);
        Rml::Context* context = Rml::CreateContext("lab", { int(W), int(H) }, renderer.get());

        // Target (the "swapchain image") and the synthetic world under it.
        const VkFormat targetFormat = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const uint32_t texel        = hdr ? 8u : 4u;
        GpuImage  target = ctx.createImage2D(W, H, targetFormat,
                                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT);
        GpuBuffer world  = ctx.createBuffer(VkDeviceSize(W) * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        GpuBuffer readback = ctx.createBuffer(VkDeviceSize(W) * H * texel, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!target.image || !world.mapped || !readback.mapped)
            return 5;
        auto* px = static_cast<uint8_t*>(world.mapped);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                uint8_t* p = px + (size_t(y) * W + x) * 4;
                const bool check = ((x / 40) + (y / 40)) & 1;
                p[0] = uint8_t(40 + 180 * x / W);
                p[1] = uint8_t(check ? 150 : 60);
                p[2] = uint8_t(40 + 180 * y / H);
                p[3] = 255;
            }

        // A viewer-owned texture (external slot test): 256x256 pattern, SHADER_READ_ONLY.
        GpuImage  extImage = ctx.createImage2D(256, 256, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        GpuBuffer extPixels = ctx.createBuffer(256 * 256 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!extImage.image || !extPixels.mapped)
            return 6;
        for (uint32_t y = 0; y < 256; ++y)
            for (uint32_t x = 0; x < 256; ++x)
            {
                uint8_t* p = static_cast<uint8_t*>(extPixels.mapped) + (size_t(y) * 256 + x) * 4;
                p[0] = uint8_t(x); p[1] = uint8_t(y); p[2] = uint8_t(((x / 32) + (y / 32)) & 1 ? 255 : 0); p[3] = 255;
            }
        bool extReady = false;
        renderer->setNamedTextureSource([&](LLRmlUiVK& vk, Rml::Vector2i& size, const Rml::String& source) -> Rml::TextureHandle
        {
            if (source != "llext:lab")
                return 0;
            size = { 256, 256 };
            return vk.externalTexture(extImage.image, extImage.view, 256, 256);
        });

        VkQueryPool             queries = VK_NULL_HANDLE;
        VkQueryPoolCreateInfo   qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        vkCreateQueryPool(ctx.device(), &qci, nullptr, &queries);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
        VkCommandBuffer             cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool        = ctx.commandPool();
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx.device(), &ai, &cmd);
        VkFence           fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(ctx.device(), &fci, nullptr, &fence);

        struct Doc
        {
            std::string name, rml;
        };
        std::vector<Doc> docs;
        if (files.empty())
            docs = { { "glass", kGlass }, { "effects", kEffects }, { "stress400", stressDocument() }, { "external", kExternal } };
        for (const auto& f : files)
            if (FILE* fp = std::fopen(f.c_str(), "rb"))
            {
                std::string s;
                char        buf[4096];
                size_t      n;
                while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
                    s.append(buf, n);
                std::fclose(fp);
                std::string base = f.substr(f.find_last_of('/') + 1);
                docs.push_back({ base.substr(0, base.find('.')), s });
            }

        uint64_t serial = 0;
        int      failures = 0;
        std::printf("%-10s %9s %9s %9s | %9s %9s %9s | %9s %9s %9s | %s\n", "doc", "cap p50", "p10", "p90", "rec p50", "p10", "p90", "gpu p50",
                    "p10", "p90", "draws");
        for (const Doc& d : docs)
        {
            Rml::ElementDocument* doc = context->LoadDocumentFromMemory(d.rml, d.name + ".rml");
            if (!doc)
            {
                ++failures;
                continue;
            }
            doc->Show();
            std::vector<double> capture, record, gpu;
            for (uint32_t f = 0; f < frames; ++f)
            {
                // Every 60th frame the document changes (one text node), like a ticking clock.
                const bool change = f == 0 || f % 60 == 0;
                if (change)
                {
                    if (Rml::Element* first = doc->GetFirstChild())
                        first->SetAttribute("data-frame", std::to_string(f));
                    const auto t0 = std::chrono::steady_clock::now();
                    renderer->begin();
                    context->Update();
                    context->Render();
                    if (!renderer->end())
                    {
                        if (!failures)
                            std::fprintf(stderr, "%s: capture failed (llrmluivk.cpp:%d)\n", d.name.c_str(), renderer->failureLine());
                        ++failures;
                    }
                    capture.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                }
                VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkResetCommandBuffer(cmd, 0);
                vkBeginCommandBuffer(cmd, &bi);
                vkCmdResetQueryPool(cmd, queries, 0, 2);
                if (!extReady)
                {
                    imageBarrier(cmd, extImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkBufferImageCopy ec{};
                    ec.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    ec.imageExtent      = { 256, 256, 1 };
                    vkCmdCopyBufferToImage(cmd, extPixels.buffer, extImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ec);
                    imageBarrier(cmd, extImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    extReady = true;
                }
                imageBarrier(cmd, target.image, f == 0 && &d == &docs[0] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.imageExtent      = { W, H, 1 };
                if (hdr)
                {
                    // the "world": 60 nits grey (the HDR UI target is linear light in nits)
                    VkClearColorValue grey{};
                    grey.float32[0] = grey.float32[1] = grey.float32[2] = 60.f;
                    grey.float32[3] = 1.f;
                    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1, &range);
                }
                else
                    vkCmdCopyBufferToImage(cmd, world.buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                imageBarrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

                VkUISurfaceTarget t;
                t.cmd             = cmd;
                t.colorImage      = target.image;
                t.colorView       = target.view;
                t.colorFormat     = targetFormat;
                t.displayPaperWhiteNits = hdr ? 350.f : 0.f;
                t.extent          = { W, H };
                t.frameSerial     = ++serial;
                t.completedSerial = serial - 1;
                t.sceneImage      = target.image;
                t.sceneView       = target.view;
                t.sceneFormat     = t.colorFormat;
                t.sceneExtent     = t.extent;
                t.sceneLayout     = t.colorLayout;
                t.sceneUsage      = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0);
                const auto r0 = std::chrono::steady_clock::now();
                if (!renderer->record(t))
                {
                    if (!failures)
                        std::fprintf(stderr, "%s: record failed (llrmluivk.cpp:%d)\n", d.name.c_str(), renderer->failureLine());
                    ++failures;
                }
                record.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count());
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queries, 1);
                imageBarrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                const bool last = f + 1 == frames;
                if (last)
                    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
                vkEndCommandBuffer(cmd);
                VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1;
                si.pCommandBuffers    = &cmd;
                vkResetFences(ctx.device(), 1, &fence);
                vkQueueSubmit(ctx.queue(), 1, &si, fence);
                vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
                uint64_t ts[2]{};
                if (vkGetQueryPoolResults(ctx.device(), queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    gpu.push_back(double(ts[1] - ts[0]) * props.limits.timestampPeriod * 1e-6);
                renderer->collect(serial);
                std::vector<uint8_t> converted;
                if (last && hdr)
                {
                    // nits -> SDR preview: / paper white, sRGB encode
                    converted.resize(size_t(W) * H * 4);
                    const uint16_t* src = static_cast<const uint16_t*>(readback.mapped);
                    auto half = [](uint16_t h)
                    {
                        const uint32_t e = (h >> 10) & 31, m = h & 1023;
                        float v = e == 0 ? m / 1024.f / 16384.f : e == 31 ? 65504.f : (1.f + m / 1024.f) * std::ldexp(1.f, int(e) - 15);
                        return (h & 0x8000) ? -v : v;
                    };
                    for (size_t i = 0; i < size_t(W) * H; ++i)
                        for (int c = 0; c < 4; ++c)
                        {
                            float v = half(src[i * 4 + c]);
                            if (c < 3)
                            {
                                v = std::clamp(v / 350.f, 0.f, 1.f);
                                v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
                            }
                            converted[i * 4 + c] = uint8_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
                        }
                }
                if (last && !writePNG(out + "/" + d.name + ".png", hdr ? converted.data() : static_cast<const uint8_t*>(readback.mapped), W, H))
                    ++failures;
            }
            const auto c = percentiles(capture), r = percentiles(record), g = percentiles(gpu);
            const auto m = renderer->memoryState();
            std::printf("%-10s %9.3f %9.3f %9.3f | %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f | soft_fail=%u buffers=%llu images=%llu\n",
                        d.name.c_str(), c.p50, c.p10, c.p90, r.p50, r.p10, r.p90, g.p50, g.p10, g.p90, m.softFailures,
                        (unsigned long long)m.buffers, (unsigned long long)m.images);
            doc->Close();
            context->Update();
        }
        renderer->hide();
        Rml::RemoveContext("lab");
        Rml::Shutdown();
        vkDeviceWaitIdle(ctx.device());
        renderer->collect(serial);
        renderer.reset();
        vkDestroyFence(ctx.device(), fence, nullptr);
        vkDestroyQueryPool(ctx.device(), queries, nullptr);
        ctx.destroyBuffer(world);
        ctx.destroyBuffer(extPixels);
        ctx.destroyImage(extImage); // after the backend released its external slot (renderer reset above)
        ctx.destroyBuffer(readback);
        ctx.destroyImage(target);
        if (failures)
            std::printf("FAILURES: %d\n", failures);
        host.shutdown();
        ctx.shutdown();
        return failures ? 1 : 0;
    }
}
