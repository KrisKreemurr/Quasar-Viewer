// #703 retained RmlUi geometry and display lists, independent of window ownership.
#include "linden_common.h"
#include "llrmluivk.h"
#include "vk_device.h"
#include "vk_pipeline.h"
#include "vk_ui_surface.h"
#include "vk_ui_texture_host.h"
#include "vk_world_shaders_spv.h"

#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Variant.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <png.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace
{
constexpr uint32_t kGeometry = 8192, kDraws = 8192, kActions = 512;
constexpr uint32_t kTextures = 128, kFilters = 128, kGradients = 128, kLayers = 8;
constexpr uint64_t kPoolBytes = 16ull << 20;
struct alignas(16) DrawRecord
{
    float    transform[16]{};
    float    translation[4]{};
    float    clip[4]{};
    uint32_t textures[4]{};
    uint64_t vertices = 0, reserved = 0;
};
static_assert(sizeof(DrawRecord) == 128);
struct alignas(16) Gradient
{
    float    p[4]{};
    uint32_t flags[4]{};
    float    colors[16][4]{};
    float    stops[16]{};
};
static_assert(sizeof(Gradient) == 352);
struct Push
{
    uint64_t records = 0, gradients = 0;
    float    viewport[4]{};
    float    params[4] = { 0, 0, 0, 1 };
    float    tint[4]   = { 1, 1, 1, 1 };
    uint32_t images[4]{};
    float    display[4]{}; // x: HDR paper white nits of a linear target (0: SDR / an intermediate)
};
VK_PUSH_BUDGET_CHECK(Push);

// Generational handles never alias a retired slot. No pointer handles escape.
uint64_t handle(uint32_t slot, uint32_t generation)
{ return (uint64_t(generation) << 32) | (slot + 1u); }
uint32_t handleSlot(uint64_t value)
{ return static_cast<uint32_t>(value) - 1u; }
uint32_t handleGeneration(uint64_t value)
{ return static_cast<uint32_t>(value >> 32); }
void identity(float* matrix)
{
    std::fill_n(matrix, 16, 0.f);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.f;
}
// Stage and access scope of each layout the UI passes use (exact masks, no MEMORY_* catch-alls).
void layoutScope(VkImageLayout layout, VkPipelineStageFlags2& stage, VkAccessFlags2& access)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_UNDEFINED:
        stage  = VK_PIPELINE_STAGE_2_NONE;
        access = 0;
        break;
    default: // a provider layout we do not know (present, general): full scope
        stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
}
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout before, VkImageLayout after)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    layoutScope(before, b.srcStageMask, b.srcAccessMask);
    layoutScope(after, b.dstStageMask, b.dstAccessMask);
    b.oldLayout           = before;
    b.newLayout           = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image                                       = image;
    b.subresourceRange                            = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo info{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &info);
}
} // namespace

struct LLRmlUiVK::State
{
    VkDeviceContext&   ctx;
    VkUITextureHost&   world;
    struct Hole
    { uint32_t offset = 0, bytes = 0; };
    struct Geometry
    {
        uint32_t generation = 0, offset = 0, bytes = 0, indexOffset = 0, count = 0;
        uint64_t use  = 0;
        bool     live = false, retained = false;
    };
    struct Texture
    {
        GpuImage                          image{};
        GpuBuffer                         staging{};
        VkUITextureHost::Lease            lease{};
        VkImageLayout                     layout     = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t                          generation = 0;
        uint64_t                          use = 0, stagingUse = 0;
        bool                              live = false, retained = false, upload = false;
        // Atlas textures keep their staging buffer (it is the CPU copy) and upload only the
        // regions written since the last upload; regionCount > kRegions uploads everything.
        static constexpr uint32_t         kRegions = 32;
        bool                              persistent = false;
        bool                              external   = false; // image owned by the viewer: never destroyed here
        uint32_t                          regionCount = 0;
        VkRect2D                          regions[kRegions]{};
    };
    enum class Kind : uint8_t
    {
        Draw,
        Push,
        Pop,
        Composite,
        Mask,
        Save
    };
    struct Action
    {
        Kind                   kind  = Kind::Draw;
        uint32_t               first = 0, count = 0, source = 0, destination = 0;
        uint32_t               filterCount = 0;
        uint64_t               texture     = 0;
        uint64_t               filters[16]{};
        Rml::BlendMode         blend = Rml::BlendMode::Blend;
        Rml::ClipMaskOperation mask  = Rml::ClipMaskOperation::Set;
        float                  clip[4]{};
    };
    enum class FilterKind : uint8_t
    {
        None,
        Blur,
        Shadow,
        Opacity,
        Mask
    };
    struct Filter
    {
        FilterKind kind       = FilterKind::None;
        uint32_t   generation = 0;
        float      sigma = 0, opacity = 1, offset[2]{}, color[4]{};
        uint64_t   texture = 0;
        bool       live    = false;
    };
    struct Shader
    {
        uint32_t generation = 0;
        uint64_t use        = 0;
        bool     live = false, retained = false;
    };
    struct ListBuffer
    {
        GpuBuffer data{};
        uint64_t  use = 0;
    };
    struct Snapshot
    {
        VkDescriptorSet set      = VK_NULL_HANDLE;
        uint64_t        revision = 0, use = 0;
    };
    struct Timers
    {
        VkQueryPool pool  = VK_NULL_HANDLE;
        uint32_t    count = 0;
        uint64_t    use   = 0;
        const char* names[kActions * 2]{};
    };

    std::array<Geometry, kGeometry>                      geometry{};
    std::array<Hole, kGeometry>                          holes{};
    std::array<Texture, kTextures>                       textures{};
    std::array<Filter, kFilters>                         filters{};
    std::array<std::array<Filter, 16>, kActions>         listFilters{};
    std::array<Shader, kGradients>                       shaders{};
    std::array<DrawRecord, kDraws>                       records{};
    std::array<VkDrawIndexedIndirectCommand, kDraws>     draws{};
    std::array<Action, kActions>                         actions{};
    std::array<std::array<uint8_t, kTextures>, kActions> actionSamples{};
    std::array<uint8_t, kActions>                        actionMasks{};
    std::array<ListBuffer, 4>                            lists{};
    std::array<Snapshot, 4>                              snapshots{};
    std::array<Timers, 3>                                timers{};
    GpuBuffer                                            pool{}, gradientBuffer{};
    uint32_t         poolHead = 0, poolLive = 0, poolPeak = 0, generation = 0, drawCount = 0, actionCount = 0, listSlot = 0;
    uint32_t         layer = 0, layerStack[kLayers]{}, layerDepth = 0, layerCount = 0;
    uint32_t         maskSlot         = 0;
    uint64_t         white            = 0, layerTextures[kLayers]{}, maskTextures[2]{}, scratch[3]{};
    VkSampler        sampler          = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool   = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout   = VK_NULL_HANDLE;
    VkPipeline       geometryPipeline = VK_NULL_HANDLE, layerPipeline = VK_NULL_HANDLE;
    VkPipeline       maskPipeline = VK_NULL_HANDLE;
    VkPipeline       postPipeline = VK_NULL_HANDLE, replacePipeline = VK_NULL_HANDLE;
    VkPipeline       layerPostPipeline = VK_NULL_HANDLE, layerReplacePipeline = VK_NULL_HANDLE;
    VkFormat         format = VK_FORMAT_UNDEFINED;
    VkExtent2D       extent{};
    uint64_t         completed = 0, serial = 0, revision = 1;
    int              failLine = 0; // source line of the first capture failure (diagnostics)
    // #703 z-slot segments: the display list split where the viewer interleaves other UI.
    static constexpr uint32_t kSegments = 8;
    uint32_t         segmentStart[kSegments]{}, segmentCount = 1;
    bool             segmentBreak = false;
    VkDescriptorSet  frameSet     = VK_NULL_HANDLE; // segment 0's snapshot, reused by later segments
    Timers*          frameTimers  = nullptr;
    uint32_t         maxIndirect  = 0;              // limits.maxDrawIndirectCount (queried once)
    uint64_t         frameSegmentSerial = 0;
    void             fail(int line) noexcept { if (!failed) failLine = line; failed = true; }
    bool             recordFail(int line) noexcept { failLine = line; return false; }
    bool             capturing = false, failed = false, visible = false, scissor = false, maskEnabled = false, maskNeeded = false;
    bool             cpuAllocationFailure = false, rendering = false;
    VkImage          restoreImage  = VK_NULL_HANDLE;
    VkImageLayout    restoreLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    float            transform[16]{}, clip[4] = { 0, 0, 1e8f, 1e8f };
    uint32_t         activeGradient = 0;
    Timers*          activeTimers   = nullptr;
    uint64_t         timestampMask  = 0;
    double           timestampMs    = 0;
    struct Timing
    {
        const char* name = nullptr;
        double      sum = 0, peak = 0;
        uint32_t    count = 0;
    };
    Timing                                timings[16]{};
    LLRmlUiVK::NamedTextureSource         namedTexture;
    std::chrono::steady_clock::time_point reportAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point refuseAt{};

    State(VkDeviceContext& c, VkUITextureHost& w) : ctx(c), world(w) { identity(transform); }
    ~State();
    bool      init() noexcept;
    bool      pipelines(VkFormat targetFormat) noexcept;
    uint32_t  allocate(uint32_t bytes) noexcept;
    void      freeRange(uint32_t offset, uint32_t bytes) noexcept;
    Action*   action(Kind kind) noexcept;
    Geometry* geom(uint64_t value) noexcept;
    Texture*  tex(uint64_t value) noexcept;
    Filter*   filter(uint64_t value) noexcept;
    uint64_t  image(uint32_t w, uint32_t h, const Rml::byte* data = nullptr) noexcept;
    bool      ensureTargets(VkExtent2D target) noexcept;
    bool      snapshot(VkDescriptorSet& set) noexcept;
    bool      upload(VkCommandBuffer cmd) noexcept;
    void      target(VkCommandBuffer cmd, Texture* t, const VkUISurfaceTarget& surface, bool clear) noexcept;
    void      endTarget(VkCommandBuffer cmd) noexcept;
    void      sample(VkCommandBuffer cmd, Texture& t) noexcept;
    void      post(VkCommandBuffer cmd, Texture& src, Texture* dst, const VkUISurfaceTarget&, VkDescriptorSet, uint32_t mode, bool blend,
                   float opacity = 1, const float* tint = nullptr, float dx = 0, float dy = 0, float sigma = 0, Texture* mask = nullptr,
                   bool clipped = true, int caller = __builtin_LINE()) noexcept;
    void      timer(VkCommandBuffer cmd, const char* name = nullptr) noexcept;
    void      refusal(const char* operation, uint64_t bytes) noexcept;
    void      dropTexture(uint64_t id) noexcept
    {
        if (auto* t = tex(id))
            if (t->live)
            {
                t->live = false;
                ++revision;
            }
    }
    uint32_t slot(uint64_t id) noexcept
    {
        auto* t = tex(id);
        return t ? t->lease.slot : 0;
    }
};

LLRmlUiVK::State::~State()
{
    // The caller must retire the final submission before destroying this object.
    // No implicit queue/device wait is hidden in a UI destructor.
    for (auto& t : textures)
        if (t.image.image)
        {
            if (world.release(t.lease, completed) && !t.external)
                ctx.destroyImage(t.image);
            if (t.staging.buffer && t.stagingUse <= completed)
                ctx.destroyBuffer(t.staging);
        }
    for (auto& b : lists)
        if (b.data.buffer && b.use <= completed)
            ctx.destroyBuffer(b.data);
    if (pool.buffer && serial <= completed)
        ctx.destroyBuffer(pool);
    if (gradientBuffer.buffer && serial <= completed)
        ctx.destroyBuffer(gradientBuffer);
    // Destruction is legal only after completion, including the pools and sampler.
    if (serial > completed)
        return;
    for (auto& t : timers)
        if (t.pool)
            vkDestroyQueryPool(ctx.device(), t.pool, nullptr);
    for (VkPipeline p :
         { geometryPipeline, layerPipeline, maskPipeline, postPipeline, replacePipeline, layerPostPipeline, layerReplacePipeline })
        if (p)
        {
            vkForgetPipeline(p);
            vkDestroyPipeline(ctx.device(), p, nullptr);
        }
    if (pipelineLayout)
        vkDestroyPipelineLayout(ctx.device(), pipelineLayout, nullptr);
    if (descriptorPool)
        vkDestroyDescriptorPool(ctx.device(), descriptorPool, nullptr);
    if (sampler)
        vkDestroySampler(ctx.device(), sampler, nullptr);
}

bool LLRmlUiVK::State::init() noexcept
try
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
    timestampMs    = double(props.limits.timestampPeriod) * 1e-6;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &count, nullptr);
    if (ctx.queueFamily() < count)
    {
        // Queue family count is device-bounded; allocation failure stays soft.
        try
        {
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &count, families.data());
            const uint32_t bits = families[ctx.queueFamily()].timestampValidBits;
            timestampMask       = bits == 64 ? UINT64_MAX : bits ? (uint64_t(1) << bits) - 1 : 0;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }
    if (!timestampMask)
        return false;
    VkMemScope                      category(VKMEM_UI);
    VkDeviceContext::SoftAllocScope soft(&ctx);
    pool = ctx.createBuffer(
        kPoolBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    gradientBuffer =
        ctx.createBuffer(sizeof(Gradient) * kGradients, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    if (!pool.buffer || !pool.mapped || !gradientBuffer.buffer || !gradientBuffer.mapped)
        return false;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device(), &sci, nullptr, &sampler) != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayout layout = world.layout();
    if (!layout)
        return false;
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange        push{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    pci.setLayoutCount         = 1;
    pci.pSetLayouts            = &layout;
    pci.pushConstantRangeCount = 1;
    pci.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(ctx.device(), &pci, nullptr, &pipelineLayout) != VK_SUCCESS)
        return false;
    VkDescriptorPoolSize       size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, world.capacity() * 4u };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets       = 4;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes    = &size;
    if (vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &descriptorPool) != VK_SUCCESS)
        return false;
    const Rml::byte whitePixel[4] = { 255, 255, 255, 255 };
    white                         = image(1, 1, whitePixel);
    return white != 0;
}
catch (const std::bad_alloc&)
{
    cpuAllocationFailure = true;
    return false;
}

uint32_t LLRmlUiVK::State::allocate(uint32_t bytes) noexcept
{
    bytes = (bytes + 15u) & ~15u;
    for (auto& h : holes)
        if (h.bytes >= bytes)
        {
            const uint32_t offset = h.offset;
            h.offset += bytes;
            h.bytes -= bytes;
            poolLive += bytes;
            poolPeak = (std::max)(poolPeak, poolLive);
            return offset;
        }
    if (bytes > kPoolBytes - poolHead)
    {
        refusal("geometry_pool", bytes);
        return UINT32_MAX;
    }
    const uint32_t offset = poolHead;
    poolHead += bytes;
    poolLive += bytes;
    poolPeak = (std::max)(poolPeak, poolLive);
    return offset;
}
void LLRmlUiVK::State::freeRange(uint32_t offset, uint32_t bytes) noexcept
{
    bytes = (bytes + 15u) & ~15u;
    poolLive -= bytes;
    for (auto& h : holes)
        if (h.bytes && h.offset + h.bytes == offset)
        {
            h.bytes += bytes;
            return;
        }
    for (auto& h : holes)
        if (h.bytes && offset + bytes == h.offset)
        {
            h.offset = offset;
            h.bytes += bytes;
            return;
        }
    for (auto& h : holes)
        if (!h.bytes)
        {
            h = { offset, bytes };
            return;
        }
}
LLRmlUiVK::State::Geometry* LLRmlUiVK::State::geom(uint64_t value) noexcept
{
    const auto s = handleSlot(value);
    return s < geometry.size() && geometry[s].generation == handleGeneration(value) ? &geometry[s] : nullptr;
}
LLRmlUiVK::State::Texture* LLRmlUiVK::State::tex(uint64_t value) noexcept
{
    const auto s = handleSlot(value);
    return s < textures.size() && textures[s].generation == handleGeneration(value) ? &textures[s] : nullptr;
}
LLRmlUiVK::State::Filter* LLRmlUiVK::State::filter(uint64_t value) noexcept
{
    const auto s = handleSlot(value);
    return s < filters.size() && filters[s].generation == handleGeneration(value) ? &filters[s] : nullptr;
}
LLRmlUiVK::State::Action* LLRmlUiVK::State::action(Kind kind) noexcept
{
    if (!capturing || failed || actionCount == actions.size())
    {
        fail(__LINE__);
        return nullptr;
    }
    auto& a = actions[actionCount++];
    a       = {};
    a.kind  = kind;
    if (scissor)
        std::copy_n(clip, 4, a.clip);
    else
    {
        a.clip[2] = a.clip[3] = 1e8f;
    }
    return &a;
}

uint64_t LLRmlUiVK::State::image(uint32_t w, uint32_t h, const Rml::byte* data) noexcept
{
    if (!w || !h || w > 8192 || h > 8192 || uint64_t(w) * h * 4 > (64ull << 20) || generation == UINT32_MAX)
    {
        refusal("image_extent", uint64_t(w) * h * 4);
        return 0;
    }
    uint32_t index = 0;
    while (index < textures.size() && textures[index].generation)
        ++index;
    if (index == textures.size())
    {
        refusal("image_slots", uint64_t(w) * h * 4);
        return 0;
    }
    const uint64_t bytes = uint64_t(w) * h * 4;
    if (bytes > world.budget() || world.bytes() > world.budget() - bytes)
    {
        refusal("image_budget", bytes);
        return 0;
    }
    Texture t;
    try
    {
        VkMemScope                      category(VKMEM_UI);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        t.image = ctx.createImage2D(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.image.image)
            return 0;
        t.lease = world.reserve(t.image.view, sampler, ctx.allocBytes(t.image));
        if (!t.lease.generation)
        {
            ctx.destroyImage(t.image);
            return 0;
        }
        if (data)
        {
            t.staging = ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!t.staging.buffer || !t.staging.mapped)
            {
                world.release(t.lease, completed);
                ctx.destroyImage(t.image);
                if (t.staging.buffer)
                    ctx.destroyBuffer(t.staging);
                return 0;
            }
            std::memcpy(t.staging.mapped, data, bytes);
            t.upload = true;
        }
        t.generation    = ++generation;
        t.live          = true;
        textures[index] = t;
        ++revision;
        return handle(index, t.generation);
    }
    catch (const std::bad_alloc&)
    {
        if (t.lease.generation)
            world.release(t.lease, completed);
        if (t.staging.buffer)
            ctx.destroyBuffer(t.staging);
        if (t.image.image)
            ctx.destroyImage(t.image);
        cpuAllocationFailure = true;
        return 0;
    }
}

bool LLRmlUiVK::State::snapshot(VkDescriptorSet& set) noexcept
try
{
    // An immutable snapshot owns *all* descriptors it contains through its last
    // binding, even when a particular draw does not sample every entry. Dead,
    // unretained images are omitted from new snapshots and can then retire.
    auto pinSnapshot = [&]()
    {
        for (auto& t : textures)
            if (t.generation && (t.live || t.retained))
            {
                t.use = (std::max)(t.use, serial);
                world.pin(t.lease, serial);
            }
    };
    for (auto& s : snapshots)
        if (s.set && s.revision == revision)
        {
            s.use = serial;
            set   = s.set;
            pinSnapshot();
            return true;
        }
    Snapshot* entry = nullptr;
    for (auto& s : snapshots)
        if (!s.set || s.use <= completed)
        {
            entry = &s;
            break;
        }
    if (!entry)
        return false;
    if (entry->set)
    {
        vkFreeDescriptorSets(ctx.device(), descriptorPool, 1, &entry->set);
        *entry = {};
    }
    const VkDescriptorSetLayout layout = world.layout();
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &layout;
    if (vkAllocateDescriptorSets(ctx.device(), &ai, &entry->set) != VK_SUCCESS)
        return false;
    std::array<VkDescriptorImageInfo, kTextures> infos{};
    std::array<VkWriteDescriptorSet, kTextures>  writes{};
    uint32_t                                     n = 0;
    for (const auto& t : textures)
        if (t.generation && (t.live || t.retained))
        {
            if (!world.descriptor(t.lease, infos[n]))
                return false;
            auto& w           = writes[n];
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = entry->set;
            w.dstBinding      = 0;
            w.dstArrayElement = t.lease.slot;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo      = &infos[n];
            ++n;
        }
    vkUpdateDescriptorSets(ctx.device(), n, writes.data(), 0, nullptr);
    entry->revision = revision;
    entry->use      = serial;
    set             = entry->set;
    pinSnapshot();
    return true;
}
catch (const std::bad_alloc&)
{
    cpuAllocationFailure = true;
    return false;
}

bool LLRmlUiVK::State::pipelines(VkFormat targetFormat) noexcept
{
    if (format != VK_FORMAT_UNDEFINED)
        return format == targetFormat && geometryPipeline && layerPipeline && postPipeline && replacePipeline && layerPostPipeline &&
               layerReplacePipeline && maskPipeline;
    VkShaderModule  modules[4]{};
    const uint32_t* code[]  = { kWorldRmlUiVertSpv, kWorldRmlUiFragSpv, kWorldRmlUiPostVertSpv, kWorldRmlUiPostFragSpv };
    const size_t    sizes[] = { sizeof(kWorldRmlUiVertSpv), sizeof(kWorldRmlUiFragSpv), sizeof(kWorldRmlUiPostVertSpv),
                                sizeof(kWorldRmlUiPostFragSpv) };
    const char*     names[] = { "rmlui_vert", "rmlui_frag", "rmlui_post_vert", "rmlui_post_frag" };
    bool            success = true;
    for (uint32_t i = 0; i < 4; ++i)
    {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = sizes[i];
        ci.pCode    = code[i];
        if (vkCreateShaderModule(ctx.device(), &ci, nullptr, &modules[i]) != VK_SUCCESS)
        {
            success = false;
            break;
        }
        try
        {
            vkNoteShaderModule(modules[i], code[i], sizes[i], names[i]);
        }
        catch (const std::bad_alloc&)
        {
            cpuAllocationFailure = true;
            success              = false;
            break;
        }
    }
    if (success)
    {
        try
        {
            GraphicsPipelineDesc d;
            d.layout      = pipelineLayout;
            d.vert        = modules[0];
            d.frag        = modules[1];
            d.blend       = true;
            d.colorFormat = targetFormat;
            createGraphicsPipelineInto(ctx.device(), d, &geometryPipeline, "rmlui_geometry");
            d.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
            createGraphicsPipelineInto(ctx.device(), d, &layerPipeline, "rmlui_layer");
            d.blend = false;
            createGraphicsPipelineInto(ctx.device(), d, &maskPipeline, "rmlui_mask");
            d.blend       = true;
            d.vert        = modules[2];
            d.frag        = modules[3];
            d.colorFormat = targetFormat;
            createGraphicsPipelineInto(ctx.device(), d, &postPipeline, "rmlui_composite");
            d.blend = false;
            createGraphicsPipelineInto(ctx.device(), d, &replacePipeline, "rmlui_replace");
            d.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
            createGraphicsPipelineInto(ctx.device(), d, &layerReplacePipeline, "rmlui_layer_replace");
            d.blend = true;
            createGraphicsPipelineInto(ctx.device(), d, &layerPostPipeline, "rmlui_layer_composite");
            pipelineBatchFlush(ctx.device());
        }
        catch (const std::bad_alloc&)
        {
            cpuAllocationFailure = true;
            success              = false;
        }
    }
    for (auto module : modules)
        if (module)
        {
            try
            {
                destroyShaderModuleDeferred(ctx.device(), module);
            }
            catch (const std::bad_alloc&)
            {
                vkDestroyShaderModule(ctx.device(), module, nullptr);
                cpuAllocationFailure = true;
                success              = false;
            }
        }
    // A first-use caller retries after a queued batch has completed; never read
    // queued handles as proof that pipeline compilation succeeded.
    if (success)
        format = targetFormat;
    return success && geometryPipeline && layerPipeline && maskPipeline && postPipeline && replacePipeline && layerPostPipeline &&
           layerReplacePipeline;
}

bool LLRmlUiVK::State::ensureTargets(VkExtent2D targetExtent) noexcept
{
    if (extent.width && (extent.width != targetExtent.width || extent.height != targetExtent.height))
    {
        if (serial > completed)
            return false;
        for (auto& id : layerTextures)
        {
            dropTexture(id);
            id = 0;
        }
        for (auto& id : maskTextures)
        {
            dropTexture(id);
            id = 0;
        }
        for (auto& id : scratch)
        {
            dropTexture(id);
            id = 0;
        }
        extent = {};
        return false; // caller collects the old targets before retrying
    }
    extent = targetExtent;
    for (uint32_t i = 0; i < layerCount; ++i)
        if (!layerTextures[i])
            layerTextures[i] = image(extent.width, extent.height);
    if (maskNeeded)
        for (auto& id : maskTextures)
            if (!id)
                id = image(extent.width, extent.height);
    bool needPost = false;
    for (uint32_t i = 0; i < actionCount; ++i)
        if (actions[i].kind == Kind::Composite)
            needPost = true;
    if (needPost)
    {
        // The first scratch is full resolution, two blur ping-pong targets half.
        if (!scratch[0])
            scratch[0] = image(extent.width, extent.height);
        for (uint32_t i = 1; i < 3; ++i)
            if (!scratch[i])
                scratch[i] = image((extent.width + 1) / 2, (extent.height + 1) / 2);
    }
    for (uint32_t i = 0; i < layerCount; ++i)
        if (!tex(layerTextures[i]))
            return false;
    if (maskNeeded && (!tex(maskTextures[0]) || !tex(maskTextures[1])))
        return false;
    if (needPost && (!tex(scratch[0]) || !tex(scratch[1]) || !tex(scratch[2])))
        return false;
    return true;
}

void LLRmlUiVK::State::timer(VkCommandBuffer cmd, const char* name) noexcept
{
    if (activeTimers && activeTimers->count < kActions * 4u)
    {
        if (!(activeTimers->count & 1u))
            activeTimers->names[activeTimers->count / 2] = name;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, activeTimers->pool, activeTimers->count++);
    }
}
void LLRmlUiVK::State::refusal(const char* operation, uint64_t bytes) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < refuseAt)
        return;
    refuseAt = now + std::chrono::seconds(5);
    VK_LOG("rmlui_refused operation=%s operation_bytes=%llu pool_live=%u pool_capacity=%llu image_live=%llu image_budget=%llu "
           "vk_soft_failures=%u last_use=%llu completed=%llu",
           operation, static_cast<unsigned long long>(bytes), poolLive, static_cast<unsigned long long>(kPoolBytes),
           static_cast<unsigned long long>(world.bytes()), static_cast<unsigned long long>(world.budget()),
           ctx.allocFailures(), static_cast<unsigned long long>(serial), static_cast<unsigned long long>(completed));
}
bool LLRmlUiVK::State::upload(VkCommandBuffer cmd) noexcept
{
    bool any = false;
    for (auto& t : textures)
        if (t.upload)
        {
            if (!any)
            {
                timer(cmd, "rmlui_upload");
                vkBreadcrumb(cmd, "rmlui_upload");
                any = true;
            }
            barrier(cmd, t.image.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copies[Texture::kRegions]{};
            uint32_t          copyCount = 1;
            copies[0].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copies[0].imageExtent      = { t.image.extent.width, t.image.extent.height, 1 };
            if (t.persistent && t.regionCount && t.regionCount <= Texture::kRegions)
            {
                // Region uploads out of the full-size staging mirror (row length = image width).
                copyCount = t.regionCount;
                for (uint32_t i = 0; i < copyCount; ++i)
                {
                    const VkRect2D& r            = t.regions[i];
                    copies[i]                    = {};
                    copies[i].bufferOffset       = (VkDeviceSize(r.offset.y) * t.image.extent.width + VkDeviceSize(r.offset.x)) * 4;
                    copies[i].bufferRowLength    = t.image.extent.width;
                    copies[i].bufferImageHeight  = t.image.extent.height;
                    copies[i].imageSubresource   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    copies[i].imageOffset        = { r.offset.x, r.offset.y, 0 };
                    copies[i].imageExtent        = { r.extent.width, r.extent.height, 1 };
                }
            }
            t.regionCount = 0;
            vkCmdCopyBufferToImage(cmd, t.staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copyCount, copies);
            barrier(cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            t.layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            t.upload     = false;
            t.stagingUse = serial;
            t.use        = serial;
            world.pin(t.lease, serial);
        }
    if (any)
        timer(cmd);
    return true;
}
void LLRmlUiVK::State::sample(VkCommandBuffer cmd, Texture& t) noexcept
{
    barrier(cmd, t.image.image, t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t.use    = serial;
    world.pin(t.lease, serial);
}
void LLRmlUiVK::State::target(VkCommandBuffer cmd, Texture* t, const VkUISurfaceTarget& surface, bool clear) noexcept
{
    const VkExtent2D size = t ? t->image.extent : surface.extent;
    if (t)
    {
        barrier(cmd, t->image.image, t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        t->use    = serial;
        world.pin(t->lease, serial);
    }
    else
        barrier(cmd, surface.colorImage, surface.colorLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo attachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    attachment.imageView   = t ? t->image.view : surface.colorView;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp      = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent    = size;
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &attachment;
    vkCmdBeginRendering(cmd, &ri);
    rendering     = true;
    restoreImage  = t ? VK_NULL_HANDLE : surface.colorImage;
    restoreLayout = surface.colorLayout;
    VkViewport vp{ 0, 0, float(size.width), float(size.height), 0, 1 };
    VkRect2D   sc{ { 0, 0 }, size };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}
void LLRmlUiVK::State::endTarget(VkCommandBuffer cmd) noexcept
{
    if (!rendering)
        return;
    vkCmdEndRendering(cmd);
    rendering = false;
    if (restoreImage)
        barrier(cmd, restoreImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, restoreLayout);
    restoreImage = VK_NULL_HANDLE;
}
void LLRmlUiVK::State::post(VkCommandBuffer cmd, Texture& src, Texture* dst, const VkUISurfaceTarget& surface, VkDescriptorSet set,
                            uint32_t mode, bool blend, float opacity, const float* tint, float dx, float dy, float sigma, Texture* mask,
                            bool clipped, int caller) noexcept
{
    if (&src == dst || (mask && mask == dst))
    {
        fail(caller);
        return;
    }
    timer(cmd, mode == 1 ? (dx != 0 ? "rmlui_blur_h" : "rmlui_blur_v") : mode == 2 ? "rmlui_shadow" : "rmlui_composite");
    vkBreadcrumb(cmd, mode == 1 ? (dx != 0 ? "rmlui_blur_h" : "rmlui_blur_v") : mode == 2 ? "rmlui_shadow" : "rmlui_composite");
    sample(cmd, src);
    if (mask)
        sample(cmd, *mask);
    target(cmd, dst, surface, false);
    if (clipped && mode != 1)
    {
        const VkExtent2D size   = dst ? dst->image.extent : surface.extent;
        const int32_t    x      = static_cast<int32_t>((std::clamp)(clip[0], 0.f, float(size.width)));
        const int32_t    y      = static_cast<int32_t>((std::clamp)(clip[1], 0.f, float(size.height)));
        const uint32_t   right  = static_cast<uint32_t>((std::clamp)(clip[2], float(x), float(size.width)));
        const uint32_t   bottom = static_cast<uint32_t>((std::clamp)(clip[3], float(y), float(size.height)));
        VkRect2D         sc{ { x, y }, { right - uint32_t(x), bottom - uint32_t(y) } };
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    bindGraphicsPipeline(cmd, dst ? (blend ? layerPostPipeline : layerReplacePipeline) : (blend ? postPipeline : replacePipeline));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
    Push push;
    push.params[0] = dx;
    push.params[1] = dy;
    push.params[2] = sigma;
    push.params[3] = opacity;
    push.images[0] = src.lease.slot;
    push.images[1] = mask ? mask->lease.slot : 0;
    push.images[2] = mode;
    push.images[3] = blend ? 0u : 1u;
    push.display[0] = dst ? 0.f : surface.displayPaperWhiteNits;
    if (mask)
    {
        const bool cropped = mask->image.extent.width != surface.extent.width || mask->image.extent.height != surface.extent.height;
        push.viewport[0]   = cropped ? clip[0] : 0;
        push.viewport[1]   = cropped ? clip[1] : 0;
        push.viewport[2]   = 1.f / float(mask->image.extent.width);
        push.viewport[3]   = 1.f / float(mask->image.extent.height);
    }
    if (tint)
        std::copy_n(tint, 4, push.tint);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    endTarget(cmd);
    timer(cmd);
}

LLRmlUiVK::LLRmlUiVK(std::unique_ptr<State> state) noexcept : m(std::move(state))
{
}
LLRmlUiVK::~LLRmlUiVK() = default;
std::unique_ptr<LLRmlUiVK> LLRmlUiVK::create(VkDeviceContext& ctx, VkUITextureHost& world) noexcept
{
    try
    {
        auto state = std::unique_ptr<State>(new (std::nothrow) State(ctx, world));
        if (!state || !state->init())
            return {};
        return std::unique_ptr<LLRmlUiVK>(new (std::nothrow) LLRmlUiVK(std::move(state)));
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
}
uint64_t LLRmlUiVK::lastSerial() const noexcept
{ return m ? m->serial : 0; }
int LLRmlUiVK::failureLine() const noexcept { return m ? m->failLine : 0; }
bool LLRmlUiVK::allocationFailed() const noexcept
{ return m && m->cpuAllocationFailure; }
LLRmlUiVK::MemoryState LLRmlUiVK::memoryState() const noexcept
{
    MemoryState result;
    if (!m)
        return result;
    result.buffers = m->pool.size + m->gradientBuffer.size;
    for (const auto& list : m->lists)
        result.buffers += list.data.size;
    result.images       = m->world.bytes();
    result.imageBudget  = m->world.budget();
    result.lastUse      = m->serial;
    result.completed    = m->completed;
    result.softFailures = m->ctx.allocFailures();
    return result;
}
bool LLRmlUiVK::releaseGPUState(uint64_t completedSerial) noexcept
{
    if (!m)
        return true;
    collect(completedSerial);
    if (m->serial > m->completed)
        return false;
    m.reset();
    return true;
}
void LLRmlUiVK::collect(uint64_t completedSerial) noexcept
{
    if (!m)
        return;
    m->completed = (std::max)(m->completed, completedSerial);
    for (auto& t : m->timers)
        if (t.pool && t.count && t.use <= m->completed)
        {
            std::array<uint64_t, kActions * 4> values{};
            if (vkGetQueryPoolResults(m->ctx.device(), t.pool, 0, t.count, t.count * sizeof(uint64_t), values.data(), sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                for (uint32_t i = 0; i + 1 < t.count; i += 2)
                    if (t.names[i / 2])
                        for (auto& timing : m->timings)
                            if (!timing.name || std::strcmp(timing.name, t.names[i / 2]) == 0)
                            {
                                const double ms = double((values[i + 1] - values[i]) & m->timestampMask) * m->timestampMs;
                                timing.name     = t.names[i / 2];
                                timing.sum += ms;
                                timing.peak = (std::max)(timing.peak, ms);
                                ++timing.count;
                                break;
                            }
            }
            t.count = 0;
        }
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - m->reportAt).count() >= 5)
    {
        for (auto& timing : m->timings)
            if (timing.count)
            {
                VK_LOG("%s avg=%.3f worst=%.3f ms samples=%u", timing.name, timing.sum / timing.count, timing.peak, timing.count);
                timing.sum = timing.peak = 0;
                timing.count             = 0;
            }
        VK_LOG("rmlui_memory pool_live=%u pool_peak=%u pool_high_water=%u pool_capacity=%llu images=%llu/%llu draws=%u actions=%u",
               m->poolLive, m->poolPeak, m->poolHead, static_cast<unsigned long long>(kPoolBytes),
               static_cast<unsigned long long>(m->world.bytes()), static_cast<unsigned long long>(m->world.budget()),
               m->drawCount, m->actionCount);
        m->reportAt = now;
    }
    for (auto& g : m->geometry)
        if (g.generation && !g.live && !g.retained && g.use <= m->completed)
        {
            m->freeRange(g.offset, g.bytes);
            g = {};
        }
    for (auto& s : m->shaders)
        if (!s.live && !s.retained && s.use <= m->completed)
            s = {};
    for (auto& t : m->textures)
        if (t.generation)
        {
            if (t.staging.buffer && !t.persistent && !t.upload && t.stagingUse <= m->completed)
                m->ctx.destroyBuffer(t.staging);
            if (!t.live && !t.retained && t.use <= m->completed && m->world.release(t.lease, m->completed))
            {
                if (t.staging.buffer)
                    m->ctx.destroyBuffer(t.staging);
                if (!t.external)
                    m->ctx.destroyImage(t.image);
                t = {};
                ++m->revision;
            }
        }
}
void LLRmlUiVK::hide() noexcept
{
    if (!m)
        return;
    m->visible     = false;
    m->capturing   = false;
    m->actionCount = m->drawCount = 0;
    for (auto& g : m->geometry)
    {
        g.live     = false;
        g.retained = false;
        g.use      = m->serial;
    }
    for (auto& t : m->textures)
    {
        t.live     = false;
        t.retained = false;
        t.use      = m->serial;
    }
    for (auto& s : m->shaders)
    {
        s.live     = false;
        s.retained = false;
        s.use      = m->serial;
    }
}
bool LLRmlUiVK::begin() noexcept
try
{
    m->capturing  = false;
    uint32_t slot = 0;
    while (slot < m->lists.size() && m->lists[slot].use > m->completed)
        ++slot;
    if (slot == m->lists.size())
        return false;
    auto& list = m->lists[slot];
    if (!list.data.buffer)
    {
        VkMemScope                      category(VKMEM_UI);
        VkDeviceContext::SoftAllocScope soft(&m->ctx);
        list.data = m->ctx.createBuffer(kDraws * (sizeof(DrawRecord) + sizeof(VkDrawIndexedIndirectCommand)),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        if (!list.data.buffer || !list.data.mapped)
            return false;
    }
    m->listSlot  = slot;
    m->drawCount = m->actionCount = 0;
    list.use                      = 0;
    // The current CPU display list owns its resources independently of RmlUi's
    // handles. A temporary released immediately after RenderGeometry must still
    // survive its first submission and every unchanged replay.
    for (auto& g : m->geometry)
        if (g.retained)
        {
            g.retained = false;
            g.use      = (std::max)(g.use, m->serial);
        }
    for (auto& t : m->textures)
        if (t.retained)
        {
            t.retained = false;
            t.use      = (std::max)(t.use, m->serial);
            ++m->revision;
        }
    for (auto& s : m->shaders)
        if (s.retained)
        {
            s.retained = false;
            s.use      = (std::max)(s.use, m->serial);
        }
    m->layer = m->layerCount = m->layerDepth = 0;
    m->maskEnabled                           = false;
    m->maskNeeded                            = false;
    m->maskSlot                              = 0;
    m->scissor                               = false;
    m->clip[0] = m->clip[1] = 0;
    m->clip[2] = m->clip[3] = 1e8f;
    identity(m->transform);
    m->activeGradient = 0;
    m->failed         = false;
    m->failLine       = 0;
    m->segmentCount   = 1;
    m->segmentStart[0] = 0;
    m->segmentBreak   = false;
    m->capturing      = true;
    m->visible        = false;
    for (auto& f : m->filters)
        if (!f.live)
            f = {};
    return true;
}
catch (const std::bad_alloc&)
{
    m->cpuAllocationFailure = true;
    return false;
}
bool LLRmlUiVK::end() noexcept
{
    if (!m->capturing)
        return false;
    m->capturing = false;
    if (m->failed || m->layerDepth)
    {
        m->drawCount = m->actionCount = 0;
        return false;
    }
    uint32_t passBudget = 1; // texture upload batch
    for (uint32_t i = 0; i < m->actionCount; ++i)
    {
        m->actionSamples[i].fill(0);
        m->actionMasks[i]  = 0;
        const auto& action = m->actions[i];
        if (action.kind == State::Kind::Draw || action.kind == State::Kind::Mask)
            for (uint32_t j = action.first; j < action.first + action.count; ++j)
            {
                const auto& record = m->records[j];
                if (auto* geometry = m->geom(record.reserved))
                    geometry->retained = true;
                if (record.textures[2])
                    m->shaders[record.textures[2] - 1].retained = true;
                for (uint32_t t = 0; t < kTextures; ++t)
                    if (m->textures[t].generation && m->textures[t].lease.slot == record.textures[0])
                    {
                        m->actionSamples[i][t]  = 1;
                        m->textures[t].retained = true;
                    }
                const uint32_t mask = record.textures[3] & 3u;
                if (mask)
                    m->actionMasks[i] |= uint8_t(1u << (mask - 1));
            }
        passBudget += m->actions[i].kind == State::Kind::Composite ? 2u + 6u * m->actions[i].filterCount : 1u;
        if (m->actions[i].kind == State::Kind::Composite)
            for (uint32_t j = 0; j < m->actions[i].filterCount; ++j)
            {
                const auto* f = m->filter(m->actions[i].filters[j]);
                if (!f)
                {
                    m->fail(__LINE__);
                    return false;
                }
                m->listFilters[i][j] = *f;
                if (auto* texture = m->tex(f->texture))
                    texture->retained = true;
            }
        if (action.kind == State::Kind::Save)
            if (auto* texture = m->tex(action.texture))
                texture->retained = true;
    }
    if (passBudget > kActions * 2u)
    {
        m->fail(__LINE__);
        return false;
    }
    auto* data = static_cast<Rml::byte*>(m->lists[m->listSlot].data.mapped);
    std::memcpy(data, m->records.data(), m->drawCount * sizeof(DrawRecord));
    std::memcpy(data + kDraws * sizeof(DrawRecord), m->draws.data(), m->drawCount * sizeof(VkDrawIndexedIndirectCommand));
    m->visible = m->drawCount != 0;
    return true;
}

Rml::CompiledGeometryHandle LLRmlUiVK::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    static_assert(sizeof(Rml::Vertex) == 20 && sizeof(int) == 4);
    if (vertices.empty() || indices.empty() || m->generation == UINT32_MAX || vertices.size() > kPoolBytes / 20 ||
        indices.size() > kPoolBytes / 4)
        return 0;
    for (const int index : indices)
        if (index < 0 || size_t(index) >= vertices.size())
            return 0;
    uint32_t slot = 0;
    while (slot < m->geometry.size() && m->geometry[slot].generation)
        ++slot;
    if (slot == m->geometry.size())
        return 0;
    const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * 20);
    const uint32_t indexStart  = (vertexBytes + 3u) & ~3u;
    const uint32_t bytes       = (indexStart + static_cast<uint32_t>(indices.size() * 4) + 15u) & ~15u;
    const uint32_t offset      = m->allocate(bytes);
    if (offset == UINT32_MAX)
        return 0;
    auto* data = static_cast<Rml::byte*>(m->pool.mapped) + offset;
    std::memcpy(data, vertices.data(), vertexBytes);
    std::memcpy(data + indexStart, indices.data(), indices.size() * 4);
    auto& g       = m->geometry[slot];
    g.generation  = ++m->generation;
    g.offset      = offset;
    g.bytes       = bytes;
    g.indexOffset = offset + indexStart;
    g.count       = static_cast<uint32_t>(indices.size());
    g.live        = true;
    return handle(slot, g.generation);
}
void LLRmlUiVK::ReleaseGeometry(Rml::CompiledGeometryHandle value)
{
    if (auto* g = m->geom(value))
    {
        g->live = false;
        g->use  = m->serial;
    }
}
void LLRmlUiVK::RenderGeometry(Rml::CompiledGeometryHandle value, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto* g = m->geom(value);
    if (!m->capturing || !g || !g->live || m->drawCount == kDraws)
    {
        m->fail(__LINE__);
        return;
    }
    if (!texture)
        texture = m->white;
    auto* t = m->tex(texture);
    if (!t || !t->live)
    {
        m->fail(__LINE__);
        return;
    }
    State::Action* a = nullptr;
    if (m->actionCount && m->actions[m->actionCount - 1].kind == State::Kind::Draw && !m->segmentBreak)
        a = &m->actions[m->actionCount - 1];
    else
        a = m->action(State::Kind::Draw);
    if (!a)
        return;
    m->segmentBreak = false;
    if (!a->count)
        a->first = m->drawCount;
    ++a->count;
    const uint32_t index = m->drawCount++;
    auto&          r     = m->records[index];
    r                    = {};
    std::copy_n(m->transform, 16, r.transform);
    r.translation[0] = translation.x;
    r.translation[1] = translation.y;
    if (m->scissor)
        std::copy_n(m->clip, 4, r.clip);
    else
    {
        r.clip[2] = r.clip[3] = 1e8f;
    }
    r.textures[0]   = t->lease.slot;
    r.textures[2]   = m->activeGradient;
    r.textures[3]   = m->maskEnabled ? m->maskSlot + 1u : 0u; // resolved to the current mask image at replay
    r.vertices      = m->pool.address + g->offset;
    r.reserved      = value; // CPU replay pins the geometry using this generational id
    m->draws[index] = { g->count, 1, g->indexOffset / 4, 0, index };
}
Rml::TextureHandle LLRmlUiVK::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i size)
{
    if (size.x <= 0 || size.y <= 0 || uint64_t(size.x) * uint64_t(size.y) * 4 != source.size())
        return 0;
    return m->image(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), source.data());
}
Rml::TextureHandle LLRmlUiVK::externalTexture(VkImage image, VkImageView view, uint32_t width, uint32_t height) noexcept
{
    // A viewer-owned image in SHADER_READ_ONLY_OPTIMAL, sampled in place. Its last use is
    // lastSerial(); the owner keeps it alive past that (the caller validates it every frame
    // and releases this handle before the image can go).
    if (!image || !view || !width || !height || m->generation == UINT32_MAX)
        return 0;
    uint32_t index = 0;
    while (index < m->textures.size() && m->textures[index].generation)
        ++index;
    if (index == m->textures.size())
    {
        m->refusal("image_slots", 0);
        return 0;
    }
    State::Texture t;
    t.lease = m->world.reserve(view, m->sampler, uint64_t(width) * height * 4);
    if (!t.lease.generation)
        return 0;
    t.image.image  = image;
    t.image.view   = view;
    t.image.extent = { width, height };
    t.image.format = VK_FORMAT_R8G8B8A8_UNORM;
    t.layout       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t.external     = true;
    t.generation   = ++m->generation;
    t.live         = true;
    m->textures[index] = t;
    ++m->revision;
    return handle(index, t.generation);
}
void LLRmlUiVK::setNamedTextureSource(NamedTextureSource source) noexcept
{
    m->namedTexture = std::move(source);
}
Rml::TextureHandle LLRmlUiVK::createAtlas(uint32_t width, uint32_t height) noexcept
{
    const uint64_t id = m->image(width, height);
    auto*          t  = m->tex(id);
    if (!t)
        return 0;
    {
        VkMemScope                      category(VKMEM_UI);
        VkDeviceContext::SoftAllocScope soft(&m->ctx);
        t->staging = m->ctx.createBuffer(uint64_t(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!t->staging.buffer || !t->staging.mapped)
    {
        // Not usable without its CPU mirror: let collect() retire the image.
        if (t->staging.buffer)
            m->ctx.destroyBuffer(t->staging);
        t->live = false;
        return 0;
    }
    std::memset(t->staging.mapped, 0, uint64_t(width) * height * 4);
    t->persistent  = true;
    t->upload      = true;
    t->regionCount = 0; // first upload: everything (clears the image)
    return id;
}
bool LLRmlUiVK::updateTexture(Rml::TextureHandle id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba,
                              uint32_t strideBytes) noexcept
{
    auto* t = m->tex(id);
    if (!t || !t->live || !t->persistent || !t->staging.mapped || !w || !h || x + w > t->image.extent.width ||
        y + h > t->image.extent.height)
        return false;
    // Append-only regions: a pending copy never reads bytes written here, so no wait.
    auto* base = static_cast<uint8_t*>(t->staging.mapped);
    for (uint32_t row = 0; row < h; ++row)
        std::memcpy(base + (size_t(y + row) * t->image.extent.width + x) * 4, rgba + size_t(row) * strideBytes, size_t(w) * 4);
    const VkRect2D region{ { int32_t(x), int32_t(y) }, { w, h } };
    if (!t->upload)
    {
        t->upload      = true;
        t->regionCount = 1;
        t->regions[0]  = region;
    }
    else if (t->regionCount == 0 || t->regionCount > State::Texture::kRegions)
    {
        // a full upload is already pending
    }
    else if (t->regionCount < State::Texture::kRegions)
        t->regions[t->regionCount++] = region;
    else
        t->regionCount = State::Texture::kRegions + 1; // too many regions: one full upload
    return true;
}
Rml::TextureHandle LLRmlUiVK::LoadTexture(Rml::Vector2i& size, const Rml::String& source)
{
    if (source.compare(0, 5, "llui:") == 0 || source.compare(0, 6, "llext:") == 0)
    {
        // Named textures supplied by the viewer (the skin image atlas): created blank, filled
        // through updateTexture by their owner.
        if (!m->namedTexture)
            return 0;
        try
        {
            return m->namedTexture(*this, size, source);
        }
        catch (const std::bad_alloc&)
        {
            return 0;
        }
    }
    if (source != "rmlui:checker")
    {
        auto* files = Rml::GetFileInterface();
        if (!files)
            return 0;
        const auto file = files->Open(source);
        if (!file)
            return 0;
        struct CloseFile
        {
            Rml::FileInterface* files;
            Rml::FileHandle     file;
            ~CloseFile() { files->Close(file); }
        } close{ files, file };
        try
        {
            const size_t length = files->Length(file);
            if (!length || length > (16ull << 20))
                return 0;
            std::vector<Rml::byte> encoded(length);
            if (files->Read(encoded.data(), length, file) != length)
                return 0;
            png_image decoded{};
            decoded.version = PNG_IMAGE_VERSION;
            if (!png_image_begin_read_from_memory(&decoded, encoded.data(), encoded.size()))
                return 0;
            struct FreePNG
            {
                png_image* image;
                ~FreePNG() { png_image_free(image); }
            } freePNG{ &decoded };
            if (!decoded.width || !decoded.height || decoded.width > 8192 || decoded.height > 8192 ||
                uint64_t(decoded.width) * decoded.height * 4 > (64ull << 20))
                return 0;
            decoded.format = PNG_FORMAT_RGBA;
            std::vector<Rml::byte> pixels(size_t(decoded.width) * decoded.height * 4);
            if (!png_image_finish_read(&decoded, nullptr, pixels.data(), 0, nullptr))
                return 0;
            for (size_t i = 0; i < pixels.size(); i += 4)
                for (uint32_t j = 0; j < 3; ++j)
                    pixels[i + j] = Rml::byte((uint32_t(pixels[i + j]) * pixels[i + 3] + 127) / 255);
            const auto texture = m->image(decoded.width, decoded.height, pixels.data());
            if (texture)
                size = { int(decoded.width), int(decoded.height) };
            return texture;
        }
        catch (const std::bad_alloc&)
        {
            return 0;
        }
    }
    // Deterministic proof asset, using the same upload path as PNGs and glyphs.
    std::array<Rml::byte, 16 * 16 * 4> pixels{};
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x)
        {
            const size_t i = (y * 16 + x) * 4;
            pixels[i]      = ((x / 4 + y / 4) & 1u) ? 50 : 210;
            pixels[i + 1]  = 150;
            pixels[i + 2]  = 220;
            pixels[i + 3]  = 255;
        }
    size = { 16, 16 };
    return m->image(16, 16, pixels.data());
}
void LLRmlUiVK::ReleaseTexture(Rml::TextureHandle value)
{ m->dropTexture(value); }
void LLRmlUiVK::EnableScissorRegion(bool enabled)
{ m->scissor = enabled; }
void LLRmlUiVK::SetScissorRegion(Rml::Rectanglei region)
{
    m->clip[0] = float(region.Left());
    m->clip[1] = float(region.Top());
    m->clip[2] = float(region.Right());
    m->clip[3] = float(region.Bottom());
}
void LLRmlUiVK::SetTransform(const Rml::Matrix4f* matrix)
{
    if (matrix)
        std::memcpy(m->transform, matrix->data(), 64);
    else
        identity(m->transform);
}

void LLRmlUiVK::EnableClipMask(bool enabled)
{
    m->maskEnabled = enabled;
    m->maskNeeded |= enabled;
}
void LLRmlUiVK::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle value, Rml::Vector2f translation)
{
    auto* a = m->action(State::Kind::Mask);
    if (!a)
        return;
    const uint32_t actionIndex = m->actionCount - 1, first = m->drawCount;
    const bool     enabled = m->maskEnabled;
    m->maskEnabled         = operation == Rml::ClipMaskOperation::Intersect;
    RenderGeometry(value, translation, 0);
    m->maskEnabled = enabled;
    if (m->failed || m->drawCount != first + 1)
        return;
    --m->actionCount; // replace the generated Draw action by our Mask action
    a              = &m->actions[actionIndex];
    a->first       = first;
    a->count       = 1;
    a->mask        = operation;
    m->maskSlot    = 1u - m->maskSlot;
    a->destination = m->maskSlot;
    m->records[first].textures[3] |= 0x100u;
    if (operation == Rml::ClipMaskOperation::SetInverse)
        m->records[first].textures[3] |= 0x200u;
    m->maskNeeded = true;
}
Rml::LayerHandle LLRmlUiVK::PushLayer()
{
    if (m->layerDepth == kLayers)
    {
        m->fail(__LINE__);
        return 0;
    }
    auto* a = m->action(State::Kind::Push);
    if (!a)
        return 0;
    m->layerStack[m->layerDepth] = m->layer;
    m->layer                     = ++m->layerDepth;
    m->layerCount                = (std::max)(m->layerCount, m->layerDepth);
    a->destination               = m->layer;
    return m->layer;
}
void LLRmlUiVK::PopLayer()
{
    if (!m->layerDepth)
    {
        m->fail(__LINE__);
        return;
    }
    auto* a = m->action(State::Kind::Pop);
    if (!a)
        return;
    m->layer       = m->layerStack[--m->layerDepth];
    a->destination = m->layer;
}
void LLRmlUiVK::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend,
                                Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    if (source > kLayers || destination > kLayers || filters.size() > 16)
    {
        m->fail(__LINE__);
        return;
    }
    auto* a = m->action(State::Kind::Composite);
    if (!a)
        return;
    a->source      = static_cast<uint32_t>(source);
    a->destination = static_cast<uint32_t>(destination);
    a->blend       = blend;
    a->filterCount = static_cast<uint32_t>(filters.size());
    std::copy(filters.begin(), filters.end(), a->filters);
    a->texture = m->maskEnabled ? m->maskSlot + 1u : 0u;
}
Rml::TextureHandle LLRmlUiVK::SaveLayerAsTexture()
{
    const float w = m->clip[2] - m->clip[0], h = m->clip[3] - m->clip[1];
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return 0;
    const auto id = m->image(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (!id)
    {
        m->fail(__LINE__);
        return 0;
    }
    auto* a = m->action(State::Kind::Save);
    if (!a)
    {
        m->dropTexture(id);
        return 0;
    }
    a->source  = m->layer;
    a->texture = id;
    return id;
}
Rml::CompiledFilterHandle LLRmlUiVK::SaveLayerAsMaskImage()
{
    const auto texture = SaveLayerAsTexture();
    if (!texture)
        return 0;
    Rml::Dictionary params;
    const auto      id = CompileFilter("mask", params);
    if (auto* f = m->filter(id))
        f->texture = texture;
    else
        m->dropTexture(texture);
    return id;
}
Rml::CompiledFilterHandle LLRmlUiVK::CompileFilter(const Rml::String& name, const Rml::Dictionary& params)
{
    State::Filter f;
    if (name == "blur")
    {
        f.kind  = State::FilterKind::Blur;
        f.sigma = Rml::Get(params, "sigma", 1.f);
    }
    else if (name == "drop-shadow")
    {
        f.kind            = State::FilterKind::Shadow;
        f.sigma           = Rml::Get(params, "sigma", 0.f);
        const auto offset = Rml::Get(params, "offset", Rml::Vector2f{});
        f.offset[0]       = offset.x;
        f.offset[1]       = offset.y;
        const auto color  = Rml::Get(params, "color", Rml::Colourb{}).ToPremultiplied();
        for (uint32_t i = 0; i < 4; ++i)
            f.color[i] = float(color[i]) / 255.f;
    }
    else if (name == "opacity")
    {
        f.kind    = State::FilterKind::Opacity;
        f.opacity = Rml::Get(params, "value", 1.f);
    }
    else if (name == "mask")
        f.kind = State::FilterKind::Mask;
    else
        return 0;
    if (!std::isfinite(f.sigma) || f.sigma < 0 || f.sigma > 32 || !std::isfinite(f.opacity) || !std::isfinite(f.offset[0]) ||
        !std::isfinite(f.offset[1]) || m->generation == UINT32_MAX)
        return 0;
    for (uint32_t i = 0; i < kFilters; ++i)
        if (!m->filters[i].generation)
        {
            f.generation  = ++m->generation;
            f.live        = true;
            m->filters[i] = f;
            return handle(i, f.generation);
        }
    return 0;
}
void LLRmlUiVK::ReleaseFilter(Rml::CompiledFilterHandle value)
{
    if (auto* f = m->filter(value))
    {
        // Display lists copy filter values in end(), before these can recycle.
        if (f->texture)
            m->dropTexture(f->texture);
        f->live = false;
    }
}
Rml::CompiledShaderHandle LLRmlUiVK::CompileShader(const Rml::String& name, const Rml::Dictionary& params)
{
    Gradient data;
    if (name == "linear-gradient")
    {
        data.flags[0] = 1;
        const auto p  = Rml::Get(params, "p0", Rml::Vector2f{});
        const auto v  = Rml::Get(params, "p1", Rml::Vector2f{}) - p;
        data.p[0]     = p.x;
        data.p[1]     = p.y;
        data.p[2]     = v.x;
        data.p[3]     = v.y;
    }
    else if (name == "radial-gradient")
    {
        data.flags[0] = 2;
        const auto p  = Rml::Get(params, "center", Rml::Vector2f{});
        const auto r  = Rml::Get(params, "radius", Rml::Vector2f(1.f));
        if (r.x <= 0 || r.y <= 0)
            return 0;
        data.p[0] = p.x;
        data.p[1] = p.y;
        data.p[2] = 1.f / r.x;
        data.p[3] = 1.f / r.y;
    }
    else if (name == "conic-gradient")
    {
        data.flags[0] = 3;
        const auto  p = Rml::Get(params, "center", Rml::Vector2f{});
        const float a = Rml::Get(params, "angle", 0.f);
        data.p[0]     = p.x;
        data.p[1]     = p.y;
        data.p[2]     = std::cos(a);
        data.p[3]     = std::sin(a);
    }
    else if (name == "llimage-solid")
        data.flags[0] = 4; // no stops: the fragment shader uses colour x image alpha
    else
        return 0;
    if (m->generation == UINT32_MAX)
        return 0;
    if (data.flags[0] != 4)
    {
        const auto it = params.find("color_stop_list");
        if (it == params.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST)
            return 0;
        const auto& stops = it->second.GetReference<Rml::ColorStopList>();
        if (stops.empty() || stops.size() > 16)
            return 0;
        data.flags[1] = Rml::Get(params, "repeating", false) ? 1u : 0u;
        data.flags[2] = static_cast<uint32_t>(stops.size());
        for (uint32_t i = 0; i < data.flags[2]; ++i)
        {
            if (!std::isfinite(stops[i].position.number))
                return 0;
            data.stops[i] = stops[i].position.number;
            for (uint32_t j = 0; j < 4; ++j)
                data.colors[i][j] = float(stops[i].color[j]) / 255.f;
        }
    }
    for (uint32_t i = 0; i < kGradients; ++i)
        if (!m->shaders[i].generation)
        {
            auto& shader      = m->shaders[i];
            shader.generation = ++m->generation;
            shader.live       = true;
            std::memcpy(static_cast<Rml::byte*>(m->gradientBuffer.mapped) + sizeof(Gradient) * i, &data, sizeof(data));
            return handle(i, shader.generation);
        }
    return 0;
}
void LLRmlUiVK::ReleaseShader(Rml::CompiledShaderHandle value)
{
    const uint32_t slot = handleSlot(value);
    if (slot < kGradients && m->shaders[slot].generation == handleGeneration(value))
    {
        m->shaders[slot].live = false;
        m->shaders[slot].use  = m->serial;
    }
}
void LLRmlUiVK::RenderShader(Rml::CompiledShaderHandle value, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                             Rml::TextureHandle texture)
{
    const uint32_t slot = handleSlot(value);
    if (slot >= kGradients || m->shaders[slot].generation != handleGeneration(value))
    {
        m->fail(__LINE__);
        return;
    }
    m->activeGradient = slot + 1;
    RenderGeometry(geometry, translation, texture);
    m->activeGradient = 0;
}

void LLRmlUiVK::beginSegment() noexcept
{
    // Called while capturing (a marker element renders): later actions form the next segment.
    if (!m->capturing || m->segmentCount >= State::kSegments)
        return;
    m->segmentStart[m->segmentCount++] = m->actionCount;
    m->segmentBreak                    = true;
}
uint32_t LLRmlUiVK::segmentCount() const noexcept
{
    return m ? m->segmentCount : 0;
}
bool LLRmlUiVK::record(const VkUISurfaceTarget& surface) noexcept
try
{
    const uint32_t segment = surface.layerSegment;
    if (segment > 0)
    {
        // A later z slot of the same frame: only its actions, with segment 0's frame state.
        if (segment >= m->segmentCount)
            return true; // nothing captured there
        if (m->frameSegmentSerial != surface.frameSerial || !m->frameSet || !m->frameTimers || m->failed)
            return m->recordFail(__LINE__);
    }
    else
    {
    collect(surface.completedSerial);
    if (!m->visible || m->failed || !surface.cmd || !surface.colorImage || !surface.colorView || !surface.frameSerial ||
        surface.frameSerial <= m->completed || !surface.extent.width || !surface.extent.height || !m->pipelines(surface.colorFormat))
        return m->recordFail(__LINE__);
    // A target's extent is part of its retained list. The host must recapture
    // after changing the context dimensions/dp-ratio, before calling record.
    if (!m->ensureTargets(surface.extent))
        return m->recordFail(__LINE__);
    auto& list = m->lists[m->listSlot]; // (prep scope)
    // Resolve lazily allocated mask indices once, never mutate a submitted list.
    if (!list.use)
    {
        for (uint32_t i = 0; i < m->drawCount; ++i)
        {
            const uint32_t mask = (m->records[i].textures[3] & 3u);
            if (mask)
                m->records[i].textures[1] = m->slot(m->maskTextures[mask - 1u]);
        }
        std::memcpy(list.data.mapped, m->records.data(), m->drawCount * sizeof(DrawRecord));
    }
    m->serial           = surface.frameSerial;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!m->snapshot(set))
        return m->recordFail(__LINE__);
    m->frameSet           = set;
    m->frameSegmentSerial = surface.frameSerial;
    list.use        = m->serial;
    m->activeTimers = nullptr;
    for (auto& t : m->timers)
        if (t.use <= m->completed)
        {
            if (!t.pool)
            {
                VkQueryPoolCreateInfo ci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
                ci.queryCount = kActions * 4u;
                if (vkCreateQueryPool(m->ctx.device(), &ci, nullptr, &t.pool) != VK_SUCCESS)
                    break;
            }
            t.count         = 0;
            t.use           = m->serial;
            m->activeTimers = &t;
            vkCmdResetQueryPool(surface.cmd, t.pool, 0, kActions * 4u);
            break;
        }
    if (!m->activeTimers)
        return m->recordFail(__LINE__);
    m->frameTimers = m->activeTimers;
    m->upload(surface.cmd);
    } // segment 0 prep
    auto&           list = m->lists[m->listSlot];
    VkDescriptorSet set  = m->frameSet;
    m->activeTimers      = m->frameTimers;
    const uint32_t  actionBegin = m->segmentStart[segment];
    const uint32_t  actionEnd   = segment + 1 < m->segmentCount ? m->segmentStart[segment + 1] : m->actionCount;
    uint32_t currentLayer = 0;
    auto     layerTexture = [&](uint32_t layer) -> State::Texture*
    {
        return layer > 0 && layer <= kLayers ? m->tex(m->layerTextures[layer - 1]) : nullptr;
    };
    auto copy = [&](State::Texture* source, State::Texture& destination, const float* crop) -> bool
    {
        VkImage        image  = source ? source->image.image : surface.sceneImage;
        VkImageLayout  layout = source ? source->layout : surface.sceneLayout;
        VkExtent2D     size   = source ? source->image.extent : surface.sceneExtent;
        const VkFormat format = source ? source->image.format : surface.sceneFormat;
        if (!image || image == destination.image.image || !size.width || !size.height || format == VK_FORMAT_UNDEFINED ||
            (!source && !(surface.sceneUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)))
            return m->recordFail(__LINE__);
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m->ctx.physicalDevice(), format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            return m->recordFail(__LINE__);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1]                        = { int32_t(size.width), int32_t(size.height), 1 };
        if (crop)
        {
            blit.srcOffsets[0] = { int32_t((std::clamp)(crop[0], 0.f, float(size.width))),
                                   int32_t((std::clamp)(crop[1], 0.f, float(size.height))), 0 };
            blit.srcOffsets[1] = { int32_t((std::clamp)(crop[2], float(blit.srcOffsets[0].x), float(size.width))),
                                   int32_t((std::clamp)(crop[3], float(blit.srcOffsets[0].y), float(size.height))), 1 };
            if (blit.srcOffsets[0].x == blit.srcOffsets[1].x || blit.srcOffsets[0].y == blit.srcOffsets[1].y)
                return m->recordFail(__LINE__);
        }
        blit.dstOffsets[1] = { int32_t(destination.image.extent.width), int32_t(destination.image.extent.height), 1 };
        m->timer(surface.cmd, source ? "rmlui_layer_copy" : "rmlui_backdrop_copy");
        vkBreadcrumb(surface.cmd, source ? "rmlui_layer_copy" : "rmlui_backdrop_copy");
        barrier(surface.cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(surface.cmd, destination.image.image, destination.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(surface.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        barrier(surface.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
        destination.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destination.use    = m->serial;
        if (source)
        {
            source->use = m->serial;
            m->world.pin(source->lease, m->serial);
        }
        m->world.pin(destination.lease, m->serial);
        m->sample(surface.cmd, destination);
        m->timer(surface.cmd);
        return true;
    };
    for (uint32_t i = actionBegin; i < actionEnd; ++i)
    {
        const auto& a = m->actions[i];
        std::copy_n(a.clip, 4, m->clip);
        switch (a.kind)
        {
            case State::Kind::Push:
                currentLayer = a.destination;
                m->timer(surface.cmd, "rmlui_layer_clear");
                vkBreadcrumb(surface.cmd, "rmlui_layer_clear");
                m->target(surface.cmd, layerTexture(currentLayer), surface, true);
                m->endTarget(surface.cmd);
                m->timer(surface.cmd);
                break;
            case State::Kind::Pop:
                currentLayer = a.destination;
                break;
            case State::Kind::Draw:
            case State::Kind::Mask:
            {
                const bool      mask        = a.kind == State::Kind::Mask;
                State::Texture* destination = mask ? m->tex(m->maskTextures[a.destination]) : layerTexture(currentLayer);
                // All resources sampled in the batch are made readable before
                // entering dynamic rendering; an attachment is never self-sampled.
                for (uint32_t t = 0; t < kTextures; ++t)
                    if (m->actionSamples[i][t])
                    {
                        auto& texture = m->textures[t];
                        if (&texture == destination)
                            return m->recordFail(__LINE__);
                        m->sample(surface.cmd, texture);
                    }
                for (uint32_t maskIndex = 0; maskIndex < 2; ++maskIndex)
                    if (m->actionMasks[i] & (1u << maskIndex))
                    {
                        auto* texture = m->tex(m->maskTextures[maskIndex]);
                        if (!texture || texture == destination)
                            return m->recordFail(__LINE__);
                        m->sample(surface.cmd, *texture);
                    }
                m->timer(surface.cmd, mask ? "rmlui_mask" : "rmlui_geometry");
                vkBreadcrumb(surface.cmd, mask ? "rmlui_mask" : "rmlui_geometry");
                m->target(surface.cmd, destination, surface, mask);
                if (mask && a.mask == Rml::ClipMaskOperation::SetInverse)
                {
                    VkClearAttachment clear{};
                    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    for (float& value : clear.clearValue.color.float32)
                        value = 1;
                    VkClearRect rect{ { { 0, 0 }, surface.extent }, 0, 1 };
                    vkCmdClearAttachments(surface.cmd, 1, &clear, 1, &rect);
                }
                bindGraphicsPipeline(surface.cmd, mask ? m->maskPipeline : destination ? m->layerPipeline : m->geometryPipeline);
                vkCmdBindDescriptorSets(surface.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m->pipelineLayout, 0, 1, &set, 0, nullptr);
                vkCmdBindIndexBuffer(surface.cmd, m->pool.buffer, 0, VK_INDEX_TYPE_UINT32);
                Push push;
                push.records     = list.data.address;
                push.gradients   = m->gradientBuffer.address;
                push.viewport[0] = float(surface.extent.width);
                push.viewport[1] = float(surface.extent.height);
                push.viewport[2] = 1.f / push.viewport[0];
                push.viewport[3] = 1.f / push.viewport[1];
                push.display[0]  = destination ? 0.f : surface.displayPaperWhiteNits;
                vkCmdPushConstants(surface.cmd, m->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                if (!m->maxIndirect)
                {
                    VkPhysicalDeviceProperties properties{};
                    vkGetPhysicalDeviceProperties(m->ctx.physicalDevice(), &properties);
                    m->maxIndirect = (std::max)(properties.limits.maxDrawIndirectCount, 1u);
                }
                const uint32_t batchSize = m->maxIndirect;
                for (uint32_t first = 0; first < a.count; first += batchSize)
                {
                    const uint32_t count = (std::min)(a.count - first, batchSize);
                    vkCmdDrawIndexedIndirect(surface.cmd, list.data.buffer,
                                             kDraws * sizeof(DrawRecord) + (a.first + first) * sizeof(VkDrawIndexedIndirectCommand), count,
                                             sizeof(VkDrawIndexedIndirectCommand));
                }
                m->endTarget(surface.cmd);
                m->timer(surface.cmd);
                break;
            }
            case State::Kind::Save:
            {
                auto* dst = m->tex(a.texture);
                if (!dst || !copy(layerTexture(a.source), *dst, a.clip))
                    return m->recordFail(__LINE__);
                break;
            }
            case State::Kind::Composite:
            {
                auto* full        = m->tex(m->scratch[0]);
                auto* ping        = m->tex(m->scratch[1]);
                auto* pong        = m->tex(m->scratch[2]);
                auto* destination = layerTexture(a.destination);
                if (!full || !ping || !pong || !copy(layerTexture(a.source), *full, nullptr))
                    return m->recordFail(__LINE__);
                State::Texture* src     = full;
                State::Texture* mask    = a.texture ? m->tex(m->maskTextures[a.texture - 1]) : nullptr;
                float           opacity = 1;
                for (uint32_t j = 0; j < a.filterCount; ++j)
                {
                    const auto& f = m->listFilters[i][j];
                    if (f.kind == State::FilterKind::Opacity)
                    {
                        opacity *= f.opacity;
                        continue;
                    }
                    if (f.kind == State::FilterKind::Mask)
                    {
                        mask = m->tex(f.texture);
                        continue;
                    }
                    if (f.kind == State::FilterKind::Blur || f.kind == State::FilterKind::Shadow)
                    {
                        if (f.kind == State::FilterKind::Shadow && src != full && !copy(src, *full, nullptr))
                            return m->recordFail(__LINE__);
                        if (src != ping && !copy(src, *ping, nullptr))
                            return m->recordFail(__LINE__);
                        if (f.sigma > 0)
                        {
                            m->post(surface.cmd, *ping, pong, surface, set, 1, false, 1, nullptr, 1.f / float(ping->image.extent.width), 0,
                                    f.sigma * 0.5f);
                            m->post(surface.cmd, *pong, ping, surface, set, 1, false, 1, nullptr, 0, 1.f / float(pong->image.extent.height),
                                    f.sigma * 0.5f);
                        }
                        if (f.kind == State::FilterKind::Shadow)
                        {
                            m->post(surface.cmd, *ping, pong, surface, set, 2, false, 1, f.color, f.offset[0] / float(surface.extent.width),
                                    f.offset[1] / float(surface.extent.height), 0, nullptr, false);
                            // The unfiltered content is placed over its shadow.
                            m->post(surface.cmd, *full, pong, surface, set, 0, true, 1, nullptr, 0, 0, 0, nullptr, false);
                            src = pong;
                        }
                        else
                            src = ping;
                    }
                }
                m->post(surface.cmd, *src, destination, surface, set, 0, a.blend == Rml::BlendMode::Blend, opacity, nullptr, 0, 0, 0, mask);
                break;
            }
        }
    }
    m->activeTimers = nullptr;
    return !m->failed;
}
catch (const std::bad_alloc&)
{
    m->endTarget(surface.cmd);
    m->cpuAllocationFailure = true;
    return false;
}
