/**
 * @file qlvkglsl.cpp
 * @brief Quasar: GL's own GLSL programs compiled for Vulkan, with what Vulkan needs to feed them.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "qlvkglsl.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace ql_glsl
{
namespace
{
    std::mutex sProcessMutex;
    int        sProcessCount = 0;

    EShLanguage language(Stage stage)
    {
        switch (stage)
        {
            case STAGE_VERTEX: return EShLangVertex;
            case STAGE_TESS_CONTROL: return EShLangTessControl;
            case STAGE_TESS_EVALUATION: return EShLangTessEvaluation;
            case STAGE_FRAGMENT: return EShLangFragment;
            case STAGE_COMPUTE: return EShLangCompute;
            default: return EShLangVertex;
        }
    }

    uint32_t stageBits(EShLanguageMask mask)
    {
        uint32_t bits = 0;
        if (mask & EShLangVertexMask) bits |= 1u << STAGE_VERTEX;
        if (mask & EShLangTessControlMask) bits |= 1u << STAGE_TESS_CONTROL;
        if (mask & EShLangTessEvaluationMask) bits |= 1u << STAGE_TESS_EVALUATION;
        if (mask & EShLangFragmentMask) bits |= 1u << STAGE_FRAGMENT;
        if (mask & EShLangComputeMask) bits |= 1u << STAGE_COMPUTE;
        return bits;
    }

    std::string stripIndex(const std::string& name)
    {
        // "foo[0]" names the array foo in reflection; "s[2].f" stays as it is (an element of an array of structs is
        // a member of its own).
        if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0)
        {
            return name.substr(0, name.size() - 3);
        }
        return name;
    }

    // Only glslang's public headers are installed (no TType): GL's type enum tells the kinds apart.
    bool isSamplerType(uint32_t t)
    {
        return (t >= 0x8B5D && t <= 0x8B64) || (t >= 0x8DC0 && t <= 0x8DC5) || (t >= 0x8DC9 && t <= 0x8DD8) ||
               (t >= 0x900C && t <= 0x900F) || (t >= 0x9108 && t <= 0x910D);
    }
    bool isImageType(uint32_t t) { return t >= 0x904C && t <= 0x906C; }
    bool isTexelBufferType(uint32_t t) { return t == 0x8DC2 || t == 0x8DD0 || t == 0x8DD8; }
    bool isMatrixType(uint32_t t) { return (t >= 0x8B5A && t <= 0x8B5C) || (t >= 0x8B65 && t <= 0x8B6A); }
}

void initialize()
{
    std::lock_guard<std::mutex> lock(sProcessMutex);
    if (sProcessCount++ == 0)
    {
        glslang::InitializeProcess();
    }
}

void finalize()
{
    std::lock_guard<std::mutex> lock(sProcessMutex);
    if (sProcessCount > 0 && --sProcessCount == 0)
    {
        glslang::FinalizeProcess();
    }
}

bool compile(const std::vector<Source>& sources, Program& out)
{
    out = Program();
    if (sources.empty())
    {
        out.log = "no sources";
        return false;
    }

    uint32_t present = 0;
    for (const Source& s : sources)
    {
        present |= 1u << s.stage;
    }

    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgRelaxedErrors);
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    glslang::TProgram program;
    bool ok = true;
    for (const Source& s : sources)
    {
        const EShLanguage lang = language(s.stage);
        auto shader = std::make_unique<glslang::TShader>(lang);
        const char* text = s.text.c_str();
        const int length = (int)s.text.size();
        const char* name = s.name.c_str();
        shader->setStringsWithLengthsAndNames(&text, &length, &name, 1);
        shader->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
        shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        shader->setEnvInputVulkanRulesRelaxed();
        shader->setAutoMapBindings(true);
        shader->setAutoMapLocations(true);
        if (!shader->parse(GetDefaultResources(), 100, false, messages))
        {
            out.log += std::string(s.name) + ":\n" + shader->getInfoLog() + shader->getInfoDebugLog();
            ok = false;
        }
        program.addShader(shader.get());
        shaders.push_back(std::move(shader));
    }
    if (!ok)
    {
        return false;
    }
    if (!program.link(messages) || !program.mapIO())
    {
        out.log += std::string("link:\n") + program.getInfoLog() + program.getInfoDebugLog();
        return false;
    }
    if (!program.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers))
    {
        out.log += "reflection failed";
        return false;
    }

    for (uint32_t st = 0; st < STAGE_COUNT; ++st)
    {
        glslang::TIntermediate* intermediate = program.getIntermediate(language((Stage)st));
        if (!intermediate || !(present & (1u << st)))
        {
            continue;
        }
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.generateDebugInfo = false;
        options.disableOptimizer  = true;  // glslang's optimizer needs SPIRV-Tools; the driver optimizes anyway
        options.validate          = false;
        glslang::GlslangToSpv(*intermediate, out.spirv[st], &logger, &options);
        const std::string said = logger.getAllMessages();
        if (!said.empty())
        {
            out.log += said;
        }
        if (out.spirv[st].empty())
        {
            out.log += "no SPIR-V for a stage";
            return false;
        }
        out.stage_mask |= 1u << st;
        if (st == STAGE_COMPUTE)
        {
            for (int i = 0; i < 3; ++i)
            {
                out.local_size[i] = program.getLocalSize(i);
            }
        }
    }

    // Blocks: the default one, the engine's, and buffer blocks.
    int default_block_index = -1;
    for (int i = 0; i < program.getNumUniformBlocks(); ++i)
    {
        const glslang::TObjectReflection& b = program.getUniformBlock(i);
        Resource r;
        r.name    = b.name;
        r.kind    = RES_UNIFORM_BLOCK;
        r.binding = b.getBinding() < 0 ? 0 : (uint32_t)b.getBinding();
        r.size    = b.size < 0 ? 0 : (uint32_t)b.size;
        r.stages  = stageBits(b.stages);
        r.default_block = b.name == "gl_DefaultUniformBlock";
        if (r.default_block)
        {
            default_block_index  = i;
            out.default_size     = r.size;
            out.default_resource = (int)out.resources.size();
        }
        out.resources.push_back(r);
    }
    for (int i = 0; i < program.getNumBufferBlocks(); ++i)
    {
        const glslang::TObjectReflection& b = program.getBufferBlock(i);
        Resource r;
        r.name    = b.name;
        r.kind    = RES_STORAGE_BLOCK;
        r.binding = b.getBinding() < 0 ? 0 : (uint32_t)b.getBinding();
        r.size    = b.size < 0 ? 0 : (uint32_t)b.size;
        r.stages  = stageBits(b.stages);
        out.resources.push_back(r);
    }

    // Uniforms: samplers and images are resources; the default block's members are what the viewer sets by name.
    for (int i = 0; i < program.getNumUniformVariables(); ++i)
    {
        const glslang::TObjectReflection& u = program.getUniform(i);
        const uint32_t gl_type = (uint32_t)u.glDefineType;
        if (isSamplerType(gl_type) || isImageType(gl_type))
        {
            Resource r;
            r.name    = stripIndex(u.name);
            r.kind    = isImageType(gl_type) ? RES_STORAGE_IMAGE : isTexelBufferType(gl_type) ? RES_TEXEL_BUFFER
                                                                                          : RES_SAMPLER;
            r.binding = u.getBinding() < 0 ? 0 : (uint32_t)u.getBinding();
            r.count   = u.size > 0 ? (uint32_t)u.size : 1;
            r.gl_type = (uint32_t)u.glDefineType;
            r.stages  = stageBits(u.stages);
            out.resources.push_back(r);
            continue;
        }
        if (u.index != default_block_index || default_block_index < 0)
        {
            continue;  // a member of an engine block: the block is filled whole
        }
        Member m;
        m.name          = stripIndex(u.name);
        m.offset        = u.offset < 0 ? 0 : (uint32_t)u.offset;
        m.gl_type       = (uint32_t)u.glDefineType;
        m.count         = u.size > 0 ? (uint32_t)u.size : 1;
        m.array_stride  = (uint32_t)(u.arrayStride > 0 ? u.arrayStride : 0);
        m.matrix_stride = isMatrixType(gl_type) ? 16 : 0;  // std140: a column is a vec4's worth
        out.members.push_back(m);
    }

    for (int i = 0; i < program.getNumPipeInputs(); ++i)
    {
        const glslang::TObjectReflection& a = program.getPipeInput(i);
        if (a.name.compare(0, 3, "gl_") == 0)
        {
            continue;
        }
        out.inputs.push_back({ a.name, a.layoutLocation(), (uint32_t)a.glDefineType, a.size > 0 ? (uint32_t)a.size : 1u });
    }
    for (int i = 0; i < program.getNumPipeOutputs(); ++i)
    {
        const glslang::TObjectReflection& a = program.getPipeOutput(i);
        if (a.name.compare(0, 3, "gl_") == 0)
        {
            continue;
        }
        out.outputs.push_back({ stripIndex(a.name), a.layoutLocation(), (uint32_t)a.glDefineType,
                                a.size > 0 ? (uint32_t)a.size : 1u });
    }
    return true;
}

const Member* findMember(const Program& program, const std::string& gl_name, uint32_t& element)
{
    element = 0;
    std::string name = gl_name;
    for (const Member& m : program.members)
    {
        if (m.name == name)
        {
            return &m;
        }
    }
    // "foo[3]": element 3 of foo.
    if (!name.empty() && name.back() == ']')
    {
        const size_t open = name.rfind('[');
        if (open != std::string::npos)
        {
            const std::string base = name.substr(0, open);
            const uint32_t index = (uint32_t)std::strtoul(name.c_str() + open + 1, nullptr, 10);
            for (const Member& m : program.members)
            {
                if (m.name == base && index < m.count)
                {
                    element = index;
                    return &m;
                }
            }
        }
    }
    return nullptr;
}
}
