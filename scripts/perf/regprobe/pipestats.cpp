// Offline register-count probe: creates a compute pipeline from a SPIR-V file on the local GPU with
// VK_KHR_pipeline_executable_properties and prints its statistics. Descriptor layouts are reflected
// from the module (set / binding decorations + variable types).
// GRAPHICS (#1019): an argument "vs.spv+fs.spv" builds a vertex + fragment pipeline instead (dynamic
// rendering; one RGBA16F colour attachment per fragment output location, D32 depth; the vertex inputs
// reflected from the VS as tightly packed attributes of one binding) and prints each stage's
// statistics -- the fragment shader's register count is the one a forward pass's cost turns on.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <fstream>
#include <string>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, (int)r_); exit(1); } } while (0)

struct Ty { uint32_t op = 0; std::vector<uint32_t> w; };

// One module's reflection: its descriptor bindings (merged into `sets` with `stage`), and its
// Input / Output variables by location (the SPIR-V type id of the pointee).
struct Reflect
{
    std::map<uint32_t, Ty> types;
    std::map<uint32_t, uint32_t> inputs, outputs;   // location -> pointee type id
};

static std::vector<uint32_t> loadSpv(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> code(bytes.size() / 4);
    memcpy(code.data(), bytes.data(), code.size() * 4);
    return code;
}

static void reflect(const std::vector<uint32_t>& code, VkShaderStageFlags stage,
                    std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>>& sets, Reflect& rf)
{
    std::map<uint32_t, Ty>& types = rf.types;
    std::map<uint32_t, uint32_t> consts, dset, dbind, loc;
    std::map<uint32_t, bool> block, bufblock, builtin;
    std::vector<std::pair<uint32_t, uint32_t>> vars;   // id, ptr type
    std::map<uint32_t, uint32_t> varSc;                // id -> storage class
    for (size_t i = 5; i < code.size();)
    {
        const uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
        const uint32_t* w = &code[i];
        if (op == 71)
        {
            if (w[2] == 34) dset[w[1]] = w[3];
            else if (w[2] == 33) dbind[w[1]] = w[3];
            else if (w[2] == 30) loc[w[1]] = w[3];
            else if (w[2] == 11) builtin[w[1]] = true;
            else if (w[2] == 2) block[w[1]] = true;
            else if (w[2] == 3) bufblock[w[1]] = true;
        }
        else if (op == 43) consts[w[2]] = w[3];
        else if (op == 59) { vars.push_back({w[2], w[1]}); varSc[w[2]] = w[3]; }
        else if ((op >= 19 && op <= 39) || op == 5341 || op == 4472)
        { Ty t; t.op = op; t.w.assign(w + 1, w + wc); types[w[1]] = t; }
        i += wc;
    }
    for (auto& v : vars)
    {
        const uint32_t sc = varSc[v.first];
        if ((sc == 1 || sc == 3) && loc.count(v.first) && !builtin.count(v.first))
        {
            // Input (1) / Output (3) with a location: the pointee type
            (sc == 1 ? rf.inputs : rf.outputs)[loc[v.first]] = types[v.second].w[2];
            continue;
        }
        if (!dset.count(v.first)) continue;
        const Ty& p = types[v.second];   // OpTypePointer: id, sc, type
        uint32_t t = p.w[2];
        uint32_t count = 1;
        while (types[t].op == 28 || types[t].op == 29)
        {
            if (types[t].op == 28) count *= consts[types[t].w[2]];
            else count = 4096;
            t = types[t].w[1];
        }
        const Ty& b = types[t];
        VkDescriptorType dt;
        if (sc == 2) dt = bufblock[t] ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (sc == 12) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (b.op == 25) { const uint32_t dim = b.w[2], sampled = b.w[6];
            if (dim == 5) dt = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else dt = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; }
        else if (b.op == 27) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (b.op == 26) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (b.op == 5341 || b.op == 4472) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        else { fprintf(stderr, "unknown descriptor type op %u\n", b.op); continue; }
        VkDescriptorSetLayoutBinding& lb = sets[dset[v.first]][dbind[v.first]];
        lb.binding = dbind[v.first];
        lb.descriptorType = dt;
        lb.descriptorCount = std::max(lb.descriptorCount, count);
        lb.stageFlags |= stage;
    }
}

// A vertex attribute format for a reflected input type (scalar / vector of 32-bit float / int).
static VkFormat attribFormat(const std::map<uint32_t, Ty>& types, uint32_t t, uint32_t& bytes)
{
    uint32_t comps = 1, ct = t;
    auto it = types.find(t);
    if (it != types.end() && it->second.op == 23) { ct = it->second.w[1]; comps = it->second.w[2]; }
    const auto c = types.find(ct);
    const bool isFloat = c != types.end() && c->second.op == 22;
    const bool isSigned = c != types.end() && c->second.op == 21 && c->second.w.size() > 2 && c->second.w[2] == 1;
    bytes = comps * 4;
    static const VkFormat ff[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat uf[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    static const VkFormat sf[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
    comps = std::min(std::max(comps, 1u), 4u);
    return isFloat ? ff[comps - 1] : isSigned ? sf[comps - 1] : uf[comps - 1];
}

static VkShaderModule makeModule(VkDevice dev, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    VkShaderModule sm;
    CK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
    return sm;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: pipestats file.spv... | vs.spv+fs.spv...\n"); return 1; }
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);

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
    const char* exts[] = {"VK_KHR_ray_query", "VK_KHR_acceleration_structure", "VK_KHR_deferred_host_operations",
                          "VK_KHR_pipeline_executable_properties", "VK_EXT_shader_atomic_float"};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 5;
    dci.ppEnabledExtensionNames = exts;
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    auto getExe = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
    auto getStat = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");

    for (int a = 1; a < argc; ++a)
    {
        const std::string arg = argv[a];
        const size_t plus = arg.find('+');
        const bool graphics = plus != std::string::npos;
        const std::vector<uint32_t> code = loadSpv(graphics ? arg.substr(0, plus).c_str() : argv[a]);
        std::vector<uint32_t> fcode;
        std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> sets;
        Reflect rv, rfr;
        reflect(code, graphics ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_COMPUTE_BIT, sets, rv);
        if (graphics)
        {
            fcode = loadSpv(arg.substr(plus + 1).c_str());
            reflect(fcode, VK_SHADER_STAGE_FRAGMENT_BIT, sets, rfr);
        }
        uint32_t maxSet = 0;
        for (auto& s : sets) maxSet = std::max(maxSet, s.first + 1);
        std::vector<VkDescriptorSetLayout> layouts(maxSet);
        for (uint32_t s = 0; s < maxSet; ++s)
        {
            std::vector<VkDescriptorSetLayoutBinding> bs;
            std::vector<VkDescriptorBindingFlags> fl;
            for (auto& kv : sets[s]) { bs.push_back(kv.second); fl.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT); }
            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            bf.bindingCount = (uint32_t)fl.size();
            bf.pBindingFlags = fl.data();
            VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
            lci.bindingCount = (uint32_t)bs.size();
            lci.pBindings = bs.data();
            CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layouts[s]));
        }
        VkPushConstantRange pcr{graphics ? (VkShaderStageFlags)(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                         : (VkShaderStageFlags)VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = maxSet;
        plci.pSetLayouts = layouts.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VkPipelineLayout pl;
        CK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
        VkShaderModule sm = makeModule(dev, code);
        VkShaderModule fsm = graphics ? makeModule(dev, fcode) : VK_NULL_HANDLE;
        // PS_SPEC="id=value,id=value": 32-bit specialization constants, as the viewer sets them (e.g. "1=0": the
        // lighting kernels' kDebugViews off -- llcleanprojstochvk.cpp pdnLitSpec / llcleanvisqueuevk.cpp vqLitSpec)
        std::vector<VkSpecializationMapEntry> specMap;
        std::vector<uint32_t> specData;
        if (const char* sp = getenv("PS_SPEC"))
        {
            std::string s(sp);
            size_t p = 0;
            while (p < s.size())
            {
                size_t c = s.find(',', p);
                if (c == std::string::npos) c = s.size();
                const std::string kv = s.substr(p, c - p);
                const size_t eq = kv.find('=');
                if (eq != std::string::npos)
                {
                    const uint32_t id = (uint32_t)strtoul(kv.substr(0, eq).c_str(), nullptr, 0);
                    const uint32_t val = (uint32_t)strtoul(kv.substr(eq + 1).c_str(), nullptr, 0);
                    specMap.push_back({id, (uint32_t)(specData.size() * 4u), 4u});
                    specData.push_back(val);
                }
                p = c + 1;
            }
        }
        VkSpecializationInfo si{(uint32_t)specMap.size(), specMap.data(), specData.size() * 4u, specData.data()};
        const VkPipelineCreateFlags pflags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR
                                           | (getenv("PS_IR") ? VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0);
        VkPipeline pipe;
        if (!graphics)
        {
            VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.flags = pflags;
            cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main",
                          specMap.empty() ? nullptr : &si};
            cpci.layout = pl;
            CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
        }
        else
        {
            const VkPipelineShaderStageCreateInfo st[2] = {
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sm, "main",
                 specMap.empty() ? nullptr : &si},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main",
                 specMap.empty() ? nullptr : &si}};
            std::vector<VkVertexInputAttributeDescription> attrs;
            uint32_t off = 0;
            for (auto& kv : rv.inputs)
            {
                uint32_t bytes = 4;
                const VkFormat fmt = attribFormat(rv.types, kv.second, bytes);
                attrs.push_back({kv.first, 0, fmt, off});
                off += bytes;
            }
            const VkVertexInputBindingDescription vb{0, std::max(off, 4u), VK_VERTEX_INPUT_RATE_VERTEX};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = attrs.empty() ? 0 : 1; vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size(); vi.pVertexAttributeDescriptions = attrs.data();
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vps.viewportCount = 1; vps.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
            uint32_t ncol = 0;
            for (auto& kv : rfr.outputs) ncol = std::max(ncol, kv.first + 1);
            std::vector<VkFormat> cf(ncol, VK_FORMAT_R16G16B16A16_SFLOAT);
            std::vector<VkPipelineColorBlendAttachmentState> cb(ncol);
            for (auto& b : cb) { b = VkPipelineColorBlendAttachmentState{}; b.colorWriteMask = 0xF; }
            VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cbs.attachmentCount = ncol; cbs.pAttachments = cb.data();
            const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            pr.colorAttachmentCount = ncol; pr.pColorAttachmentFormats = cf.data();
            pr.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr};
            gp.flags = pflags;
            gp.stageCount = 2; gp.pStages = st;
            gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
            gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
            gp.pColorBlendState = &cbs; gp.pDynamicState = &dy;
            gp.layout = pl;
            CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe));
        }
        VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        pi.pipeline = pipe;
        uint32_t ne = 0;
        getExe(dev, &pi, &ne, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> ex(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        getExe(dev, &pi, &ne, ex.data());
        for (uint32_t e = 0; e < ne; ++e)
        {
            VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            ei.pipeline = pipe;
            ei.executableIndex = e;
            uint32_t ns = 0;
            getStat(dev, &ei, &ns, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> sts(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            getStat(dev, &ei, &ns, sts.data());
            // (graphics: one line per stage, the stage named after the file -- "...spv[fragment]:")
            if (graphics)
            {
                const char* stage = (ex[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT) ? "fragment" : "vertex";
                printf("%s[%s].spv:", argv[a], stage);
            }
            else printf("%s:", argv[a]);
            for (auto& s : sts)
            {
                if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) printf(" %s=%llu;", s.name, (unsigned long long)s.value.u64);
                else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) printf(" %s=%lld;", s.name, (long long)s.value.i64);
                else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) printf(" %s=%g;", s.name, s.value.f64);
            }
            printf("\n");
            if (getenv("PS_IR"))
            {
                auto getIR = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
                uint32_t nr = 0;
                getIR(dev, &ei, &nr, nullptr);
                std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
                getIR(dev, &ei, &nr, ir.data());
                std::vector<std::vector<char>> bufs(nr);
                for (uint32_t k = 0; k < nr; ++k) { bufs[k].resize(ir[k].dataSize + 1); ir[k].pData = bufs[k].data(); }
                getIR(dev, &ei, &nr, ir.data());
                for (uint32_t k = 0; k < nr; ++k)
                {
                    printf("IR %u: %s (%s) text=%d size=%zu\n", k, ir[k].name, ir[k].description, (int)ir[k].isText, ir[k].dataSize);
                    std::string fn = std::string(argv[a]) + ".ir" + std::to_string(e) + "_" + std::to_string(k) + ".txt";
                    FILE* o = fopen(fn.c_str(), "wb");
                    if (o) { fwrite(bufs[k].data(), 1, ir[k].dataSize, o); fclose(o); }
                }
            }
        }
        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyShaderModule(dev, sm, nullptr);
        if (fsm) vkDestroyShaderModule(dev, fsm, nullptr);
        vkDestroyPipelineLayout(dev, pl, nullptr);
        for (auto l : layouts) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
