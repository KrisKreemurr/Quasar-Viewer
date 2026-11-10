// vqbench (#1039): the Vulkan context (vqb_vk.h).
#include "vqb_vk.h"

#include <algorithm>
#include <cstring>

void VqbCtx::init(int deviceIndex)
{
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vqbench";
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VQB_CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    if (n == 0) { std::fprintf(stderr, "vqbench: no Vulkan device\n"); std::exit(1); }
    if (deviceIndex < 0)
    {
        // the first discrete GPU (the owner's 5090), else device 0
        deviceIndex = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pds[i], &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { deviceIndex = (int)i; break; }
        }
    }
    pd = pds[std::min<uint32_t>((uint32_t)deviceIndex, n - 1)];
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
    deviceName = props.deviceName;
    tsPeriodNs = props.limits.timestampPeriod;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    qfam = 0;
    for (uint32_t i = 0; i < qn; ++i)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }

    // every supported feature on (pipestats.cpp's chain + what the lighting kernels use)
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &pe};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, &as};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &af};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    as.accelerationStructureCaptureReplay = VK_FALSE;
    as.accelerationStructureHostCommands = VK_FALSE;
    // (robust access off, as the viewer's world pipelines: no pipeline robustness -- the kernels guard their reads)
    f2.features.robustBufferAccess = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
    if (!rq.rayQuery || !as.accelerationStructure || !f12.bufferDeviceAddress)
    {
        std::fprintf(stderr, "vqbench: %s lacks ray query / acceleration structures / buffer device address\n", deviceName.c_str());
        std::exit(1);
    }
    std::vector<const char*> exts = {"VK_KHR_ray_query", "VK_KHR_acceleration_structure", "VK_KHR_deferred_host_operations",
                                     "VK_KHR_pipeline_executable_properties"};
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ep.data());
        for (const char* want : {"VK_EXT_shader_atomic_float", "VK_KHR_shader_clock"})
            for (auto& e : ep)
                if (std::strcmp(e.extensionName, want) == 0) { exts.push_back(want); break; }
        bool haveAf = false;
        for (const char* e : exts) haveAf = haveAf || std::strcmp(e, "VK_EXT_shader_atomic_float") == 0;
        if (!haveAf) f13.pNext = &as;   // (drop the atomic-float struct from the chain)
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)exts.size();
    dci.ppEnabledExtensionNames = exts.data();
    VQB_CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qfam;
    VQB_CK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));

    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    subgroupSize = p11.subgroupSize;

    createAS = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(dev, "vkCreateAccelerationStructureKHR");
    destroyAS = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(dev, "vkDestroyAccelerationStructureKHR");
    asSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureBuildSizesKHR");
    cmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(dev, "vkCmdBuildAccelerationStructuresKHR");
    asAddr = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureDeviceAddressKHR");
    getExe = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
    getStat = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");
}

void VqbCtx::shutdown()
{
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    for (VqbImage& i : ownedImages)
    {
        if (i.view) vkDestroyImageView(dev, i.view, nullptr);
        if (i.img) vkDestroyImage(dev, i.img, nullptr);
        if (i.mem) vkFreeMemory(dev, i.mem, nullptr);
    }
    for (VqbBuffer& b : ownedBuffers)
    {
        if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
        if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    }
    ownedImages.clear();
    ownedBuffers.clear();
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    dev = VK_NULL_HANDLE;
}

uint32_t VqbCtx::memType(uint32_t bits, VkMemoryPropertyFlags want) const
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "vqbench: no memory type for bits 0x%x flags 0x%x\n", bits, want);
    std::exit(1);
}

VkDeviceAddress VqbCtx::bufAddr(VkBuffer b) const
{
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b;
    return vkGetBufferDeviceAddress(dev, &i);
}

VqbBuffer VqbCtx::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
    VqbBuffer b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = b.size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VQB_CK(vkCreateBuffer(dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, b.buf, &mr);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi};
    mi.allocationSize = mr.size;
    mi.memoryTypeIndex = memType(mr.memoryTypeBits, hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VQB_CK(vkAllocateMemory(dev, &mi, nullptr, &b.mem));
    VQB_CK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    if (hostVisible) VQB_CK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    b.addr = bufAddr(b.buf);
    ownedBuffers.push_back(b);
    return b;
}

void VqbCtx::upload(const VqbBuffer& dst, VkDeviceSize off, const void* data, size_t bytes)
{
    if (bytes == 0) return;
    VqbBuffer st = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(st.mapped, data, bytes);
    oneShot([&](VkCommandBuffer c)
    {
        VkBufferCopy r{0, off, bytes};
        vkCmdCopyBuffer(c, st.buf, dst.buf, 1, &r);
    });
    // (the staging buffer is freed now: drop it from the owned list)
    vkDestroyBuffer(dev, st.buf, nullptr);
    vkFreeMemory(dev, st.mem, nullptr);
    ownedBuffers.pop_back();
}

void VqbCtx::download(const VqbBuffer& src, VkDeviceSize off, void* data, size_t bytes)
{
    if (bytes == 0) return;
    VqbBuffer st = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    oneShot([&](VkCommandBuffer c)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(c, &d);
        VkBufferCopy r{off, 0, bytes};
        vkCmdCopyBuffer(c, src.buf, st.buf, 1, &r);
    });
    std::memcpy(data, st.mapped, bytes);
    vkDestroyBuffer(dev, st.buf, nullptr);
    vkFreeMemory(dev, st.mem, nullptr);
    ownedBuffers.pop_back();
}

VqbBuffer VqbCtx::bufferWith(VkDeviceSize size, VkBufferUsageFlags usage, const void* data, size_t bytes)
{
    VqbBuffer b = buffer(std::max<VkDeviceSize>(size, bytes), usage, false);
    oneShot([&](VkCommandBuffer c) { vkCmdFillBuffer(c, b.buf, 0, VK_WHOLE_SIZE, 0u); });
    upload(b, 0, data, bytes);
    return b;
}

VqbImage VqbCtx::image(VkImageViewType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips,
                       VkImageUsageFlags usage, bool depth)
{
    VqbImage im;
    im.format = fmt; im.viewType = type; im.w = w; im.h = h; im.layers = layers; im.mips = mips; im.depth = depth;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    if (type == VK_IMAGE_VIEW_TYPE_3D) { ii.extent.depth = std::max(layers, 1u); im.d = ii.extent.depth; layers = 1; im.layers = 1; }
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VQB_CK(vkCreateImage(dev, &ii, nullptr, &im.img));
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, im.img, &mr);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = mr.size;
    mi.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VQB_CK(vkAllocateMemory(dev, &mi, nullptr, &im.mem));
    VQB_CK(vkBindImageMemory(dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = type;
    vi.format = fmt;
    vi.subresourceRange = {(VkImageAspectFlags)(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, mips, 0, layers};
    VQB_CK(vkCreateImageView(dev, &vi, nullptr, &im.view));
    // GENERAL, cleared to zero (storage + sampled reads both legal in GENERAL)
    oneShot([&](VkCommandBuffer c)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE; b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im.img;
        b.subresourceRange = vi.subresourceRange;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(c, &d);
        if (depth)
        {
            VkClearDepthStencilValue z{1.f, 0};
            vkCmdClearDepthStencilImage(c, im.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &vi.subresourceRange);
        }
        else
        {
            VkClearColorValue z{};
            vkCmdClearColorImage(c, im.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &vi.subresourceRange);
        }
    });
    ownedImages.push_back(im);
    return im;
}

VqbImage VqbCtx::texture2D(uint32_t w, uint32_t h, const std::vector<uint32_t>& rgba, bool srgb)
{
    uint32_t mips = 1;
    while ((std::max(w, h) >> mips) != 0) ++mips;
    VqbImage im = image(VK_IMAGE_VIEW_TYPE_2D, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, mips,
                        VK_IMAGE_USAGE_SAMPLED_BIT);
    // the mip chain on the CPU (2x2 box; alpha averaged like the colour -- the coverage the RT alpha test sees at a LOD)
    std::vector<std::vector<uint32_t>> lv{rgba};
    uint32_t lw = w, lh = h;
    for (uint32_t m = 1; m < mips; ++m)
    {
        const uint32_t nw = std::max(lw >> 1, 1u), nh = std::max(lh >> 1, 1u);
        std::vector<uint32_t> o(nw * nh);
        const std::vector<uint32_t>& s = lv.back();
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
            {
                uint32_t acc[4] = {};
                for (uint32_t k = 0; k < 4; ++k)
                {
                    const uint32_t sx = std::min(x * 2 + (k & 1), lw - 1), sy = std::min(y * 2 + (k >> 1), lh - 1);
                    const uint32_t t = s[sy * lw + sx];
                    for (uint32_t c = 0; c < 4; ++c) acc[c] += (t >> (8 * c)) & 0xFF;
                }
                uint32_t v = 0;
                for (uint32_t c = 0; c < 4; ++c) v |= ((acc[c] + 2) / 4) << (8 * c);
                o[y * nw + x] = v;
            }
        lv.push_back(std::move(o));
        lw = nw; lh = nh;
    }
    size_t total = 0;
    for (auto& l : lv) total += l.size() * 4;
    VqbBuffer st = buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::vector<VkBufferImageCopy> regions;
    size_t off = 0;
    lw = w; lh = h;
    for (uint32_t m = 0; m < mips; ++m)
    {
        std::memcpy((char*)st.mapped + off, lv[m].data(), lv[m].size() * 4);
        VkBufferImageCopy r{};
        r.bufferOffset = off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        r.imageExtent = {lw, lh, 1};
        regions.push_back(r);
        off += lv[m].size() * 4;
        lw = std::max(lw >> 1, 1u); lh = std::max(lh >> 1, 1u);
    }
    oneShot([&](VkCommandBuffer c)
    {
        vkCmdCopyBufferToImage(c, st.buf, im.img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
    });
    vkDestroyBuffer(dev, st.buf, nullptr);
    vkFreeMemory(dev, st.mem, nullptr);
    ownedBuffers.pop_back();
    return im;
}

void VqbCtx::oneShot(const std::function<void(VkCommandBuffer)>& rec)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c;
    VQB_CK(vkAllocateCommandBuffers(dev, &ai, &c));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VQB_CK(vkBeginCommandBuffer(c, &bi));
    rec(c);
    VQB_CK(vkEndCommandBuffer(c));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    VQB_CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VQB_CK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &c);
}
