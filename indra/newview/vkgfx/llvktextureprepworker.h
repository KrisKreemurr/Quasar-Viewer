#pragma once

#include "llvktextureprep.h"

#include <cstdint>

namespace LLVKTexturePrep
{
struct WorkspaceSize
{
    uint64_t outputBytes = 0;
    uint64_t scratchBytes = 0;
};

// Exact upper bounds used by the service before it admits a job. The worker writes only
// into these caller-owned buffers and performs no dynamic allocation.
bool workspaceSize(const SourceSnapshot& source, const Recipe& recipe,
                   WorkspaceSize& size) noexcept;

// Copies only the compressed mips present in SourceSnapshot. A resident GPU tail is
// described by Recipe but remains the renderer's responsibility to copy at record time.
bool prepareCompressedPayload(const SourceSnapshot& source, const Recipe& recipe,
                              uint8_t* destination, uint64_t destinationBytes,
                              OutputLayout& output) noexcept;

bool prepareTextureInput(const SourceSnapshot& source, const Recipe& recipe,
                         uint8_t* destination, uint64_t destinationBytes,
                         uint8_t* scratch, uint64_t scratchBytes,
                         OutputLayout& output) noexcept;

bool preparedTextureOutputMatches(const Recipe& recipe, const OutputLayout& output,
                                  uint32_t width, uint32_t height,
                                  uint64_t capacity) noexcept;
}
