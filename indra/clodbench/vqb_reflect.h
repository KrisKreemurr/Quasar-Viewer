// vqbench (#1039): SPIR-V reflection for the bench --
//   * the descriptor bindings of a module (set, binding, type, count, the image's dim / format / arrayed / depth)
//     -- the bench unions every kernel's into ONE five-set layout, as the viewer's mRtReflLayout;
//   * the byte layout of a uniform / storage block by NAME ("linfo.cam_pos", "gi.cfg", "views[0].cur_view"):
//     the bench writes the frame info, the light info, the GI header ... through the viewer's own declarations
//     (the unoptimised twin of a kernel keeps the member names), so no C++ mirror of those structs can drift.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct VqbBindingInfo
{
    uint32_t set = 0, binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t count = 1;         // array size (runtime arrays: 0)
    bool runtimeArray = false;
    // images
    uint32_t dim = 1;           // SPIR-V Dim (0 1D, 1 2D, 2 3D, 3 Cube, 5 Buffer)
    bool arrayed = false, depth = false;
    uint32_t spvFormat = 0;     // SPIR-V ImageFormat (storage images)
    uint32_t sampledKind = 0;   // 0 float, 1 int, 2 uint (the image's sampled type)
    // blocks
    uint32_t blockType = 0;     // the block struct's id (0: not a block)
    uint32_t blockSize = 0;     // bytes up to the end of the last member (a trailing runtime array: its stride once)
    uint32_t runtimeStride = 0; // a trailing runtime array's element stride (0: none)
    std::string name;           // the variable's / block's name (names twin)
};

struct VqbField
{
    uint32_t offset = 0;
    uint32_t size = 0;
    bool ok = false;
};

class VqbModule
{
public:
    bool parse(const std::vector<uint32_t>& words, std::string& err);
    const std::vector<VqbBindingInfo>& bindings() const { return mBindings; }
    bool hasPush() const { return mPushType != 0; }
    uint32_t pushSize() const { return mPushSize; }
    // the field at PATH inside the block at (set, binding); path components separated by '.', array indices as
    // [n]. The first component names a member of the block itself (e.g. "linfo" of LInfo { LightInfo linfo; }).
    VqbField field(uint32_t set, uint32_t binding, const std::string& path) const;
    const VqbBindingInfo* find(uint32_t set, uint32_t binding) const;

private:
    struct Type
    {
        uint32_t op = 0;
        std::vector<uint32_t> w;   // operands after the result id
    };
    uint32_t typeSize(uint32_t t) const;
    uint32_t constU(uint32_t id) const;
    std::map<uint32_t, Type> mTypes;
    std::map<uint32_t, uint32_t> mConsts;
    std::map<uint32_t, std::string> mNames;
    std::map<uint32_t, std::map<uint32_t, std::string>> mMemberNames;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> mMemberOffset;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> mMatrixStride;
    std::map<uint32_t, uint32_t> mArrayStride;
    std::vector<VqbBindingInfo> mBindings;
    uint32_t mPushType = 0, mPushSize = 0;
};

// the union of several modules' bindings into one layout per set (a conflicting type is an error)
bool vqbUnionBindings(const std::vector<const VqbModule*>& mods, std::map<uint32_t, std::map<uint32_t, VqbBindingInfo>>& out,
                      std::string& err);
