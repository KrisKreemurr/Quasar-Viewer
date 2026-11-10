// vqbench (#1039): a minimal Vulkan context for the standalone visibility-queue bench -- device with every
// supported feature on (as scripts/perf/regprobe/pipestats.cpp: the viewer's lighting kernels need ray query,
// buffer device address, descriptor indexing, subgroup ballot, 16-bit types ...), buffers with device addresses,
// images, acceleration structures, one-shot submits. Not the viewer's VkDeviceContext (no allocator, no
// pressure policy): one VkDeviceMemory per resource, everything freed at exit.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#define VQB_CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "vqbench: %s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); std::exit(1); } } while (0)

struct VqbBuffer
{
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress addr = 0;
    void* mapped = nullptr;   // host-visible buffers only
};

struct VqbImage
{
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t w = 1, h = 1, d = 1, layers = 1, mips = 1;
    bool depth = false;
};

struct VqbAccel
{
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VqbBuffer buf;
    VkDeviceAddress addr = 0;
};

struct VqbCtx
{
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    float tsPeriodNs = 1.f;
    uint32_t subgroupSize = 32;
    std::string deviceName;
    std::vector<VqbBuffer> ownedBuffers;
    std::vector<VqbImage> ownedImages;

    PFN_vkCreateAccelerationStructureKHR createAS = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAS = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR asSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAS = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR asAddr = nullptr;
    PFN_vkGetPipelineExecutablePropertiesKHR getExe = nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR getStat = nullptr;

    void init(int deviceIndex);
    void shutdown();
    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const;
    VqbBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    // device-local storage buffer filled with data (bytes; zero-filled past it up to size)
    VqbBuffer bufferWith(VkDeviceSize size, VkBufferUsageFlags usage, const void* data, size_t bytes);
    void upload(const VqbBuffer& dst, VkDeviceSize off, const void* data, size_t bytes);
    void download(const VqbBuffer& src, VkDeviceSize off, void* data, size_t bytes);
    VqbImage image(VkImageViewType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips,
                   VkImageUsageFlags usage, bool depth = false);
    // RGBA8 2D image with a full mip chain from level 0's texels (box filter) -> SHADER_READ_ONLY
    VqbImage texture2D(uint32_t w, uint32_t h, const std::vector<uint32_t>& rgba, bool srgb);
    void oneShot(const std::function<void(VkCommandBuffer)>& rec);
    VkDeviceAddress bufAddr(VkBuffer b) const;
};
