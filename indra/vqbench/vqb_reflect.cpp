// vqbench (#1039): SPIR-V reflection (vqb_reflect.h).
#include "vqb_reflect.h"

#include <algorithm>
#include <cstring>

namespace
{
enum : uint32_t
{
    OpName = 5, OpMemberName = 6, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
    OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29,
    OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpSpecConstant = 50, OpVariable = 59, OpDecorate = 71,
    OpMemberDecorate = 72, OpTypeAccelerationStructureKHR = 5341, OpTypeRayQueryKHR = 4472,
};
enum : uint32_t { DecBlock = 2, DecBufferBlock = 3, DecArrayStride = 6, DecMatrixStride = 7, DecBinding = 33, DecSet = 34, DecOffset = 35 };
enum : uint32_t { ScUniformConstant = 0, ScUniform = 2, ScPushConstant = 9, ScStorageBuffer = 12 };

std::string spvString(const uint32_t* w, uint32_t n)
{
    std::string s;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t b = 0; b < 4; ++b)
        {
            const char c = (char)((w[i] >> (8 * b)) & 0xFF);
            if (!c) return s;
            s.push_back(c);
        }
    return s;
}
} // namespace

uint32_t VqbModule::constU(uint32_t id) const
{
    auto it = mConsts.find(id);
    return it == mConsts.end() ? 1u : it->second;
}

uint32_t VqbModule::typeSize(uint32_t t) const
{
    auto it = mTypes.find(t);
    if (it == mTypes.end()) return 0;
    const Type& ty = it->second;
    switch (ty.op)
    {
    case OpTypeBool: return 4;
    case OpTypeInt:
    case OpTypeFloat: return ty.w[0] / 8;
    case OpTypeVector: return typeSize(ty.w[0]) * ty.w[1];
    case OpTypeMatrix: return 16u * ty.w[1];   // (column stride 16: every block here is std140 / std430 vec4-aligned)
    case OpTypeArray:
    {
        auto s = mArrayStride.find(t);
        const uint32_t stride = s != mArrayStride.end() ? s->second : typeSize(ty.w[0]);
        return stride * constU(ty.w[1]);
    }
    case OpTypeRuntimeArray: return 0;
    case OpTypeStruct:
    {
        uint32_t end = 0;
        auto offs = mMemberOffset.find(t);
        for (uint32_t m = 0; m < ty.w.size(); ++m)
        {
            const uint32_t off = (offs != mMemberOffset.end() && offs->second.count(m)) ? offs->second.at(m) : 0u;
            end = std::max(end, off + typeSize(ty.w[m]));
        }
        return end;
    }
    default: return 0;
    }
}

bool VqbModule::parse(const std::vector<uint32_t>& code, std::string& err)
{
    if (code.size() < 5 || code[0] != 0x07230203u) { err = "not SPIR-V"; return false; }
    std::map<uint32_t, uint32_t> dset, dbind;
    std::map<uint32_t, bool> block, bufblock;
    std::vector<std::pair<uint32_t, uint32_t>> vars;   // id, pointer type
    for (size_t i = 5; i < code.size();)
    {
        const uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFFu;
        if (wc == 0) { err = "bad word count"; return false; }
        const uint32_t* w = &code[i];
        switch (op)
        {
        case OpName: mNames[w[1]] = spvString(w + 2, wc - 2); break;
        case OpMemberName: mMemberNames[w[1]][w[2]] = spvString(w + 3, wc - 3); break;
        case OpDecorate:
            if (w[2] == DecSet) dset[w[1]] = w[3];
            else if (w[2] == DecBinding) dbind[w[1]] = w[3];
            else if (w[2] == DecBlock) block[w[1]] = true;
            else if (w[2] == DecBufferBlock) bufblock[w[1]] = true;
            else if (w[2] == DecArrayStride) mArrayStride[w[1]] = w[3];
            break;
        case OpMemberDecorate:
            if (w[3] == DecOffset) mMemberOffset[w[1]][w[2]] = w[4];
            else if (w[3] == DecMatrixStride) mMatrixStride[w[1]][w[2]] = w[4];
            break;
        case OpConstant:
        case OpSpecConstant: mConsts[w[2]] = w[3]; break;
        case OpVariable: vars.push_back({w[2], w[1]}); if (w[3] == ScPushConstant) mPushType = w[1]; break;
        default:
            if ((op >= OpTypeBool && op <= OpTypePointer) || op == OpTypeAccelerationStructureKHR || op == OpTypeRayQueryKHR)
            {
                Type t;
                t.op = op;
                t.w.assign(w + 2, w + wc);
                mTypes[w[1]] = t;
            }
            break;
        }
        i += wc;
    }
    if (mPushType)
    {
        const Type& p = mTypes[mPushType];
        mPushSize = typeSize(p.w[1]);
    }
    for (auto& v : vars)
    {
        if (!dset.count(v.first)) continue;
        auto pit = mTypes.find(v.second);
        if (pit == mTypes.end() || pit->second.op != OpTypePointer) continue;
        const uint32_t sc = pit->second.w[0];
        uint32_t t = pit->second.w[1];
        VqbBindingInfo b;
        b.set = dset[v.first];
        b.binding = dbind[v.first];
        b.name = mNames.count(v.first) ? mNames[v.first] : std::string();
        uint32_t count = 1;
        while (mTypes[t].op == OpTypeArray || mTypes[t].op == OpTypeRuntimeArray)
        {
            if (mTypes[t].op == OpTypeArray) count *= constU(mTypes[t].w[1]);
            else { b.runtimeArray = true; count = 0; }
            t = mTypes[t].w[0];
        }
        b.count = count;
        const Type& ty = mTypes[t];
        if (sc == ScUniform || sc == ScStorageBuffer)
        {
            b.type = (sc == ScStorageBuffer || bufblock[t]) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.blockType = t;
            b.blockSize = typeSize(t);
            if (b.name.empty() && mNames.count(t)) b.name = mNames[t];
            // a trailing runtime array: its stride
            if (ty.op == OpTypeStruct && !ty.w.empty())
            {
                const uint32_t last = ty.w.back();
                if (mTypes[last].op == OpTypeRuntimeArray)
                {
                    auto s = mArrayStride.find(last);
                    b.runtimeStride = s != mArrayStride.end() ? s->second : typeSize(mTypes[last].w[0]);
                    const uint32_t off = mMemberOffset[t].count((uint32_t)ty.w.size() - 1) ? mMemberOffset[t][(uint32_t)ty.w.size() - 1] : 0u;
                    b.blockSize = std::max(b.blockSize, off);
                }
            }
        }
        else if (ty.op == OpTypeImage || ty.op == OpTypeSampledImage)
        {
            const Type& im = ty.op == OpTypeSampledImage ? mTypes[ty.w[0]] : ty;
            b.dim = im.w[1];
            b.depth = im.w[2] == 1;
            b.arrayed = im.w[3] != 0;
            b.spvFormat = im.w[6];
            const Type& st = mTypes[im.w[0]];
            b.sampledKind = st.op == OpTypeFloat ? 0u : (st.w.size() > 1 && st.w[1] ? 1u : 2u);
            if (ty.op == OpTypeSampledImage) b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (b.dim == 5) b.type = im.w[5] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else b.type = im.w[5] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        else if (ty.op == OpTypeSampler) b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (ty.op == OpTypeAccelerationStructureKHR) b.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        else continue;
        mBindings.push_back(b);
    }
    std::sort(mBindings.begin(), mBindings.end(), [](const VqbBindingInfo& a, const VqbBindingInfo& b)
              { return a.set != b.set ? a.set < b.set : a.binding < b.binding; });
    return true;
}

const VqbBindingInfo* VqbModule::find(uint32_t set, uint32_t binding) const
{
    for (const VqbBindingInfo& b : mBindings)
        if (b.set == set && b.binding == binding) return &b;
    return nullptr;
}

VqbField VqbModule::field(uint32_t set, uint32_t binding, const std::string& path) const
{
    VqbField f;
    const VqbBindingInfo* b = find(set, binding);
    if (!b || !b->blockType) return f;
    uint32_t t = b->blockType, off = 0;
    size_t p = 0;
    while (p < path.size())
    {
        size_t e = path.find('.', p);
        if (e == std::string::npos) e = path.size();
        std::string comp = path.substr(p, e - p);
        p = e + 1;
        int idx = -1;
        const size_t br = comp.find('[');
        if (br != std::string::npos) { idx = std::atoi(comp.c_str() + br + 1); comp = comp.substr(0, br); }
        auto ty = mTypes.find(t);
        if (ty == mTypes.end() || ty->second.op != OpTypeStruct) return f;
        auto names = mMemberNames.find(t);
        if (names == mMemberNames.end()) return f;
        int mem = -1;
        for (auto& kv : names->second)
            if (kv.second == comp) { mem = (int)kv.first; break; }
        if (mem < 0) return f;
        auto mo = mMemberOffset.find(t);
        off += (mo != mMemberOffset.end() && mo->second.count((uint32_t)mem)) ? mo->second.at((uint32_t)mem) : 0u;
        t = ty->second.w[(size_t)mem];
        if (idx >= 0)
        {
            auto at = mTypes.find(t);
            if (at == mTypes.end() || (at->second.op != OpTypeArray && at->second.op != OpTypeRuntimeArray)) return f;
            auto s = mArrayStride.find(t);
            const uint32_t stride = s != mArrayStride.end() ? s->second : typeSize(at->second.w[0]);
            off += stride * (uint32_t)idx;
            t = at->second.w[0];
        }
    }
    f.offset = off;
    f.size = typeSize(t);
    f.ok = true;
    return f;
}

bool vqbUnionBindings(const std::vector<const VqbModule*>& mods, std::map<uint32_t, std::map<uint32_t, VqbBindingInfo>>& out,
                      std::string& err)
{
    for (const VqbModule* m : mods)
        for (const VqbBindingInfo& b : m->bindings())
        {
            auto& slot = out[b.set];
            auto it = slot.find(b.binding);
            if (it == slot.end()) { slot[b.binding] = b; continue; }
            VqbBindingInfo& a = it->second;
            if (a.type != b.type || a.dim != b.dim || a.arrayed != b.arrayed)
            {
                err = "set " + std::to_string(b.set) + " binding " + std::to_string(b.binding) + ": '" + a.name + "' vs '" + b.name
                    + "' disagree on the descriptor type / image shape";
                return false;
            }
            a.count = std::max(a.count, b.count);
            a.runtimeArray = a.runtimeArray || b.runtimeArray;
            a.blockSize = std::max(a.blockSize, b.blockSize);
            if (!a.spvFormat) a.spvFormat = b.spvFormat;
            if (a.name.empty()) a.name = b.name;
        }
    return true;
}
