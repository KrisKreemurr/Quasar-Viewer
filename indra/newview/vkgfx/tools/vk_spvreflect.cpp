/**
 * @file vk_spvreflect.cpp
 * @brief See vk_spvreflect.h.
 */
#include "vk_spvreflect.h"

#include <algorithm>
#include <unordered_map>

namespace
{
enum : uint32_t
{
    OpEntryPoint = 15, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpSpecConstant = 50,
    OpVariable = 59, OpDecorate = 71, OpTypeAccelerationStructureKHR = 5341
};
enum : uint32_t { DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecSet = 34, DecSpecId = 1 };
enum : uint32_t { ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScPushConstant = 9, ScStorageBuffer = 12 };

struct Type
{
    uint32_t op = 0;
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;   // operands (meaning per op)
};
}

SpvModuleInfo spvReflect(const uint32_t* w, size_t n)
{
    SpvModuleInfo out;
    if (!w || n < 5 || w[0] != 0x07230203u) { out.error = "not SPIR-V"; return out; }
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, uint32_t> constants;   // id -> first literal word
    struct Dec { int set = -1, binding = -1, location = -1; bool builtin = false, block = false, bufferBlock = false; };
    std::unordered_map<uint32_t, Dec> dec;
    struct Var { uint32_t type = 0, sc = 0; };
    std::unordered_map<uint32_t, Var> vars;
    size_t i = 5;
    while (i < n)
    {
        const uint32_t op = w[i] & 0xFFFFu, len = w[i] >> 16;
        if (!len || i + len > n) { out.error = "bad instruction length"; return out; }
        const uint32_t* o = w + i + 1;   // operands
        switch (op)
        {
        case OpEntryPoint:
            if (out.stage == VK_SHADER_STAGE_ALL)
            {
                switch (o[0])
                {
                case 0: out.stage = VK_SHADER_STAGE_VERTEX_BIT; break;
                case 4: out.stage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
                case 5: out.stage = VK_SHADER_STAGE_COMPUTE_BIT; break;
                case 5364: out.stage = VK_SHADER_STAGE_TASK_BIT_EXT; break;
                case 5365: out.stage = VK_SHADER_STAGE_MESH_BIT_EXT; break;
                default: break;
                }
            }
            break;
        case OpDecorate:
        {
            Dec& d = dec[o[0]];
            if (o[1] == DecSet) d.set = (int)o[2];
            else if (o[1] == DecBinding) d.binding = (int)o[2];
            else if (o[1] == DecLocation) d.location = (int)o[2];
            else if (o[1] == DecBuiltIn) d.builtin = true;
            else if (o[1] == DecBlock) d.block = true;
            else if (o[1] == DecBufferBlock) d.bufferBlock = true;
            else if (o[1] == DecSpecId) out.specIds.push_back(o[2]);
            break;
        }
        case OpTypeVoid: case OpTypeBool: case OpTypeSampler: case OpTypeAccelerationStructureKHR:
            types[o[0]] = Type{op};
            break;
        case OpTypeInt: types[o[0]] = Type{op, o[1], o[2]}; break;
        case OpTypeFloat: types[o[0]] = Type{op, o[1]}; break;
        case OpTypeVector: case OpTypeMatrix: types[o[0]] = Type{op, o[1], o[2]}; break;
        case OpTypeImage: types[o[0]] = Type{op, o[1], o[2], o[3], o[4], o[5], o[6]}; break;   // sampled type, dim, depth, arrayed, ms, sampled
        case OpTypeSampledImage: types[o[0]] = Type{op, o[1]}; break;
        case OpTypeArray: types[o[0]] = Type{op, o[1], o[2]}; break;   // element, length id
        case OpTypeRuntimeArray: types[o[0]] = Type{op, o[1]}; break;
        case OpTypeStruct: types[o[0]] = Type{op, len > 2 ? o[1] : 0}; break;   // first member (input matrices only)
        case OpTypePointer: types[o[0]] = Type{op, o[1], o[2]}; break;   // storage class, pointee
        case OpConstant: case OpSpecConstant: if (len >= 4) constants[o[1]] = o[2]; break;
        case OpVariable: vars[o[1]] = Var{o[0], o[2]}; break;
        default: break;
        }
        i += len;
    }
    auto typeOf = [&](uint32_t id) -> const Type* { auto it = types.find(id); return it == types.end() ? nullptr : &it->second; };
    // Scalar/vector -> format (+ bytes) for vertex inputs / colour outputs.
    auto fmtOf = [&](uint32_t id, bool output, uint32_t& bytes) -> VkFormat {
        const Type* t = typeOf(id);
        if (!t) return VK_FORMAT_UNDEFINED;
        uint32_t comps = 1, compType = id;
        if (t->op == OpTypeVector) { comps = t->b; compType = t->a; }
        const Type* c = typeOf(compType);
        if (!c) return VK_FORMAT_UNDEFINED;
        bytes = 4 * comps;
        if (c->op == OpTypeFloat)
        {
            if (output)
            {
                static const VkFormat f[4] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                              VK_FORMAT_R16G16B16A16_SFLOAT};
                return f[std::min(comps, 4u) - 1];
            }
            static const VkFormat f[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                          VK_FORMAT_R32G32B32A32_SFLOAT};
            return f[std::min(comps, 4u) - 1];
        }
        if (c->op == OpTypeInt)
        {
            const bool sgn = c->b != 0;
            static const VkFormat u[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
            static const VkFormat s[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
            if (output && comps == 3) comps = 4;
            return (sgn ? s : u)[std::min(comps, 4u) - 1];
        }
        return VK_FORMAT_UNDEFINED;
    };
    for (const auto& kv : vars)
    {
        const uint32_t id = kv.first;
        const Var& v = kv.second;
        const Type* ptr = typeOf(v.type);
        if (!ptr || ptr->op != OpTypePointer) continue;
        const Dec d = dec.count(id) ? dec[id] : Dec{};
        uint32_t pointee = ptr->b;
        if (v.sc == ScPushConstant) { out.pushConstants = true; continue; }
        if (v.sc == ScInput || v.sc == ScOutput)
        {
            if (d.builtin || d.location < 0) continue;
            const Type* t = typeOf(pointee);
            if (t && t->op == OpTypeStruct && dec.count(pointee) && dec[pointee].builtin) continue;
            const bool isOut = v.sc == ScOutput;
            if (isOut && out.stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            if (!isOut && out.stage != VK_SHADER_STAGE_VERTEX_BIT) continue;
            uint32_t locs = 1, elem = pointee;
            if (t && t->op == OpTypeMatrix) { locs = t->b; elem = t->a; }
            else if (t && t->op == OpTypeArray) { locs = constants.count(t->b) ? constants[t->b] : 1; elem = t->a; }
            for (uint32_t k = 0; k < locs; ++k)
            {
                SpvAttr a;
                a.location = (uint32_t)d.location + k;
                a.format = fmtOf(elem, isOut, a.bytes);
                if (a.format == VK_FORMAT_UNDEFINED) continue;
                (isOut ? out.outputs : out.inputs).push_back(a);
            }
            continue;
        }
        if (d.set < 0 || d.binding < 0) continue;
        SpvBinding b;
        b.set = (uint32_t)d.set;
        b.binding = (uint32_t)d.binding;
        const Type* t = typeOf(pointee);
        // unwrap arrays
        while (t && (t->op == OpTypeArray || t->op == OpTypeRuntimeArray))
        {
            if (t->op == OpTypeRuntimeArray) { b.runtimeArray = true; b.count = 1024; }
            else b.count *= constants.count(t->b) ? std::max(1u, constants[t->b]) : 1u;
            pointee = t->a;
            t = typeOf(pointee);
        }
        if (!t) continue;
        if (v.sc == ScStorageBuffer) b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (v.sc == ScUniform)
            b.type = (dec.count(pointee) && dec[pointee].bufferBlock) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                                      : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (v.sc == ScUniformConstant)
        {
            if (t->op == OpTypeSampledImage)
            {
                const Type* img = typeOf(t->a);
                b.type = (img && img->b == 5) ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            else if (t->op == OpTypeImage)
            {
                if (t->b == 5) b.type = t->f == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                else if (t->b == 6) b.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                else b.type = t->f == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            }
            else if (t->op == OpTypeSampler) b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if (t->op == OpTypeAccelerationStructureKHR) b.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        }
        if (b.type == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
        out.bindings.push_back(b);
    }
    auto byLoc = [](const SpvAttr& a, const SpvAttr& b) { return a.location < b.location; };
    std::sort(out.inputs.begin(), out.inputs.end(), byLoc);
    std::sort(out.outputs.begin(), out.outputs.end(), byLoc);
    out.ok = out.stage != VK_SHADER_STAGE_ALL;
    if (!out.ok) out.error = "no entry point";
    return out;
}
