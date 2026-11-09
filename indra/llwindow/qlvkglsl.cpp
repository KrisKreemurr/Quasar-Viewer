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

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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

    // ---- Varyings lined up by name, as GL links them -------------------------------------------------------------
    // glslang's default IO mapping gives each stage's ins and outs locations in the order each stage declares them,
    // not by name ("does not do proper cross-stage lining up"): a fragment stage that declares its inputs in another
    // order than its vertex stage wrote them reads the wrong ones. Each stage's inputs are moved, in its SPIR-V, to the
    // locations the stage before it gave the outputs of the same name.
    struct InterfaceVar
    {
        uint32_t    id = 0;
        bool        output = false;
        std::string name;         // the variable's
        std::string block;        // its block type's, for a block whose instance names differ between stages
        size_t      location_word = 0;  // the Location decoration's literal in the words (0: none)
        uint32_t    location = 0;
        uint32_t    width = 1;    // the locations it takes (an array's elements, a matrix's columns): an estimate
    };

    std::string spvString(const std::vector<uint32_t>& spv, size_t at, size_t end)
    {
        std::string out;
        for (size_t w = at; w < end; ++w)
        {
            for (int b = 0; b < 4; ++b)
            {
                const char c = (char)((spv[w] >> (8 * b)) & 0xFF);
                if (!c)
                {
                    return out;
                }
                out += c;
            }
        }
        return out;
    }

    std::vector<InterfaceVar> interfaceVars(const std::vector<uint32_t>& spv)
    {
        std::unordered_map<uint32_t, std::string> names;
        std::unordered_map<uint32_t, uint32_t>    pointee, array_length_of, constants, columns;
        std::unordered_map<uint32_t, uint32_t>    array_element;
        std::unordered_map<uint32_t, std::pair<size_t, uint32_t>> locations;  // id -> (word, value)
        std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> variables;  // id, (pointer type, storage)
        for (size_t w = 5; w < spv.size();)
        {
            const uint32_t op = spv[w] & 0xFFFF;
            const uint32_t n  = spv[w] >> 16;
            if (n == 0 || w + n > spv.size())
            {
                break;
            }
            switch (op)
            {
                case 5:  // OpName
                    names[spv[w + 1]] = spvString(spv, w + 2, w + n);
                    break;
                case 71:  // OpDecorate: target, decoration, literal
                    if (n >= 4 && spv[w + 2] == 30)  // Location
                    {
                        locations[spv[w + 1]] = { w + 3, spv[w + 3] };
                    }
                    break;
                case 43:  // OpConstant: type, id, value
                    if (n >= 4)
                    {
                        constants[spv[w + 2]] = spv[w + 3];
                    }
                    break;
                case 28:  // OpTypeArray: id, element, length (a constant)
                    if (n >= 4)
                    {
                        array_element[spv[w + 1]]   = spv[w + 2];
                        array_length_of[spv[w + 1]] = spv[w + 3];
                    }
                    break;
                case 24:  // OpTypeMatrix: id, column type, count
                    if (n >= 4)
                    {
                        columns[spv[w + 1]] = spv[w + 3];
                    }
                    break;
                case 32:  // OpTypePointer: id, storage, type
                    if (n >= 4)
                    {
                        pointee[spv[w + 1]] = spv[w + 3];
                    }
                    break;
                case 59:  // OpVariable: type, id, storage
                    if (n >= 4 && (spv[w + 3] == 1 || spv[w + 3] == 3))  // Input, Output
                    {
                        variables.push_back({ spv[w + 2], { spv[w + 1], spv[w + 3] } });
                    }
                    break;
                default:
                    break;
            }
            w += n;
        }
        std::vector<InterfaceVar> out;
        for (const auto& [id, info] : variables)
        {
            const auto at = locations.find(id);
            if (at == locations.end())
            {
                continue;  // a built-in
            }
            InterfaceVar v;
            v.id            = id;
            v.output        = info.second == 3;
            v.name          = names.count(id) ? names[id] : std::string();
            v.location_word = at->second.first;
            v.location      = at->second.second;
            uint32_t type   = pointee.count(info.first) ? pointee[info.first] : 0;
            if (names.count(type))
            {
                v.block = names[type];
            }
            uint32_t width = 1;
            while (array_element.count(type))
            {
                const uint32_t length = constants.count(array_length_of[type]) ? constants[array_length_of[type]] : 1;
                width *= std::max(length, 1u);
                type = array_element[type];
            }
            if (columns.count(type))
            {
                width *= columns[type];
            }
            v.width = width;
            out.push_back(v);
        }
        return out;
    }

    // The consumer's inputs at the producer's outputs' locations, by name (or block type); the inputs the producer has
    // no output for after every location it uses, where they meet nothing.
    void lineUp(const std::vector<uint32_t>& producer, std::vector<uint32_t>& consumer, bool per_vertex_consumer)
    {
        (void)per_vertex_consumer;
        std::unordered_map<std::string, uint32_t> by_name, by_block;
        uint32_t next = 0;
        for (const InterfaceVar& v : interfaceVars(producer))
        {
            if (!v.output)
            {
                continue;
            }
            if (!v.name.empty())
            {
                by_name[v.name] = v.location;
            }
            if (!v.block.empty())
            {
                by_block[v.block] = v.location;
            }
            next = std::max(next, v.location + v.width);
        }
        for (const InterfaceVar& v : interfaceVars(consumer))
        {
            if (v.output)
            {
                continue;
            }
            uint32_t location = 0;
            if (!v.name.empty() && by_name.count(v.name))
            {
                location = by_name[v.name];
            }
            else if (!v.block.empty() && by_block.count(v.block))
            {
                location = by_block[v.block];
            }
            else
            {
                location = next;
                next += v.width;
            }
            consumer[v.location_word] = location;
        }
    }
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

    // Each stage's inputs where the stage before it wrote them, by name, as GL links a program.
    {
        int previous = -1;
        for (uint32_t st = 0; st < STAGE_COUNT; ++st)
        {
            if (st == STAGE_COMPUTE || out.spirv[st].empty())
            {
                continue;
            }
            if (previous >= 0)
            {
                lineUp(out.spirv[previous], out.spirv[st], st == STAGE_TESS_CONTROL || st == STAGE_TESS_EVALUATION);
            }
            previous = (int)st;
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
