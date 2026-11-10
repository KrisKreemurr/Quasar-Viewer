/**
 * @file vk_spvreflect.h
 * @brief #1411 bench helper: a minimal SPIR-V reflector. Builds a pipeline layout, vertex
 *        input and colour formats COMPATIBLE with a module set (not the renderer's exact ones),
 *        so the compile bench can create real pipelines from the viewer's embedded SPIR-V
 *        without the renderer. Dev tool only (slpipe-bench); never linked into the viewer.
 */
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct SpvBinding
{
    uint32_t set = 0, binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t count = 1;
    bool runtimeArray = false;
};

struct SpvAttr
{
    uint32_t location = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t bytes = 0;
};

struct SpvModuleInfo
{
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;   // ALL = unknown
    std::vector<SpvBinding> bindings;
    std::vector<SpvAttr> inputs;      // vertex stage only
    std::vector<SpvAttr> outputs;     // fragment stage: colour outputs by location
    bool pushConstants = false;
    std::vector<uint32_t> specIds;    // OpDecorate SpecId values present
    bool ok = false;
    std::string error;
};

SpvModuleInfo spvReflect(const uint32_t* words, size_t count);
