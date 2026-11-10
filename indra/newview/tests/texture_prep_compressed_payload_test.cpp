#include "llvktextureprepworker.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
using namespace LLVKTexturePrep;

bool expect(bool ok, const char* message)
{
    if (!ok) std::fprintf(stderr, "texture prep compressed payload: %s\n", message);
    return ok;
}

uint64_t mipBytes(uint32_t width, uint32_t height)
{
    const uint64_t blocksX = std::max<uint32_t>((width + 3u) / 4u, 1u);
    const uint64_t blocksY = std::max<uint32_t>((height + 3u) / 4u, 1u);
    return blocksX * blocksY * 8u; // BC1 block payload
}

SourceSnapshot makePartialBlob(const std::shared_ptr<std::vector<uint8_t>>& bytes,
                               bool splitPieces)
{
    SourceSnapshot source;
    source.owner = std::shared_ptr<const void>(bytes, bytes.get());
    source.width = source.fullWidth = 1024;
    source.height = source.fullHeight = 1024;
    source.levels = 11;
    source.firstLevel = 2;
    source.components = 4;
    source.storageFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    source.kind = SourceKind::CompressedLevels;
    source.serial = 91;
    source.generation = 7;
    source.spanCount = splitPieces ? 3 : 1;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < source.spanCount; ++i)
    {
        const uint32_t first = splitPieces ? 2u + i : 2u;
        const uint32_t count = splitPieces ? 1u : 3u;
        uint64_t countBytes = 0;
        for (uint32_t j = 0; j < count; ++j)
            countBytes += mipBytes(std::max(1024u >> (first + j), 1u),
                                   std::max(1024u >> (first + j), 1u));
        InputSpan& span = source.spans[i];
        span.data = bytes->data() + offset;
        span.bytes = countBytes;
        span.level = first;
        span.levelCount = count;
        span.width = std::max(1024u >> first, 1u);
        span.height = std::max(1024u >> first, 1u);
        span.layout = SpanLayout::MipChain;
        offset += static_cast<uint32_t>(countBytes);
    }
    source.byteCount = offset;
    source.retainedBackingBytes = bytes->capacity();
    return source;
}

Recipe partialRecipe()
{
    Recipe recipe;
    recipe.kind = RecipeKind::CopyCompressed;
    recipe.targetWidth = recipe.targetHeight = 256;
    recipe.targetLevels = 9;
    recipe.payloadLevels = 3;
    recipe.residentTailLevels = 6;
    recipe.targetStorageFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    recipe.preparedStorageFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    return recipe;
}

bool checkPartialThreeMipPayload(bool splitPieces)
{
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    const uint64_t expectedBytes = mipBytes(256, 256) + mipBytes(128, 128) + mipBytes(64, 64);
    bytes->resize(static_cast<size_t>(expectedBytes));
    for (size_t i = 0; i < bytes->size(); ++i) (*bytes)[i] = static_cast<uint8_t>(i * 13u + 7u);
    SourceSnapshot source = makePartialBlob(bytes, splitPieces);
    Recipe recipe = partialRecipe();
    WorkspaceSize workspace{};
    if (!expect(source.valid(), "source snapshot must validate")
        || !expect(workspaceSize(source, recipe, workspace), "partial payload must admit")
        || !expect(workspace.outputBytes == expectedBytes, "stage size must include only levels 2..4"))
        return false;
    std::vector<uint8_t> staged(static_cast<size_t>(workspace.outputBytes));
    OutputLayout output{};
    if (!expect(prepareCompressedPayload(source, recipe, staged.data(), staged.size(), output),
                "partial payload worker must prepare")) return false;
    return expect(output.mipCount == 3 && output.byteCount == expectedBytes,
                  "worker reports only the three staged levels")
        && expect(output.mips[0].width == 256 && output.mips[1].width == 128
                  && output.mips[2].width == 64,
                  "absolute source levels remap to destination mips 0..2")
        && expect(staged == *bytes, "compressed payload bytes are copied without conversion")
        && expect(preparedTextureOutputMatches(recipe, output, 256, 256, expectedBytes),
                  "prepared output validates against the staged range");
}

bool checkRejectsGapAndTailMismatch()
{
    const uint64_t payloadBytes = mipBytes(256, 256) + mipBytes(128, 128) + mipBytes(64, 64);
    auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(payloadBytes));
    SourceSnapshot source = makePartialBlob(bytes, true);
    source.spanCount = 1;
    source.spans[0].levelCount = 1;
    source.byteCount = source.spans[0].bytes;
    Recipe recipe = partialRecipe();
    std::vector<uint8_t> out(static_cast<size_t>(payloadBytes));
    OutputLayout unchanged{};
    unchanged.byteCount = 123;
    const bool gapRejected = !prepareCompressedPayload(source, recipe, out.data(), out.size(), unchanged)
        && unchanged.byteCount == 123;

    source = makePartialBlob(bytes, true);
    recipe.residentTailLevels = 5;
    const bool tailRejected = !prepareCompressedPayload(source, recipe, out.data(), out.size(), unchanged)
        && unchanged.byteCount == 123;
    return expect(gapRejected, "missing absolute mip levels are rejected without publishing metadata")
        && expect(tailRejected, "payload plus resident tail must cover the final mip chain");
}
}

int main()
{
    return checkPartialThreeMipPayload(false)
        && checkPartialThreeMipPayload(true)
        && checkRejectsGapAndTailMismatch() ? 0 : 1;
}
