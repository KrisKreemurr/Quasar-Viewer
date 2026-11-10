/**
 * @file llsoundpreviewlimits.h
 * @brief Fixed budgets for local sound previews.
 * Copyright (C) 2026 Slipstream Viewer contributors.
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#pragma once
#include <cstdint>
#include <limits>
#include <cstdio>
#include <new>
#include <string>
namespace LLSoundPreviewLimits
{
constexpr std::uint32_t REQUESTS = 128;
constexpr std::uint32_t CLIPS = 64;
constexpr std::uint64_t PCM_BYTES = 64ull * 1024 * 1024;
constexpr std::uint32_t WAVE_COLUMNS = 4096;
inline bool fits(std::uint64_t current, std::uint64_t old_bytes, std::uint64_t new_bytes) noexcept
{
    return old_bytes <= current && new_bytes <= PCM_BYTES && current - old_bytes <= PCM_BYTES - new_bytes;
}
inline bool sourceBytes(std::uint32_t frames, std::uint32_t channels, std::uint32_t rate,
                        std::uint64_t& bytes) noexcept
{
    bytes = 0;
    if (!frames || (channels != 1 && channels != 2) || !rate ||
        rate > std::uint32_t((std::numeric_limits<std::int32_t>::max)())) return false;
    const std::uint64_t size = std::uint64_t(frames) * channels * sizeof(std::int16_t);
    if (size > std::uint64_t((std::numeric_limits<std::int32_t>::max)())) return false;
    bytes = size;
    return true;
}
constexpr std::uint64_t FILE_OVERHEAD_BYTES = 1024ull * 1024;
enum class FileReadResult { Ready, Invalid, Refused };
// Read a bounded snapshot from the already-open cache file: growing files cannot
// grow our allocation, and a concurrent size change fails without a partial read.
inline FileReadResult readFile(std::FILE* input, std::uint64_t pcm_budget, std::string& data)
{
    data.clear();
    if (!input || std::fseek(input, 0, SEEK_END) != 0) return FileReadResult::Invalid;
    const long length = std::ftell(input);
    if (length < 0 || std::fseek(input, 0, SEEK_SET) != 0) return FileReadResult::Invalid;
    const std::uint64_t budget = pcm_budget < PCM_BYTES ? pcm_budget : PCM_BYTES;
    if (std::uint64_t(length) > budget + FILE_OVERHEAD_BYTES) return FileReadResult::Refused;
    try { data.resize(static_cast<std::size_t>(length)); }
    catch (const std::bad_alloc&) { return FileReadResult::Refused; }
    if (std::fread(data.data(), 1, data.size(), input) != data.size() ||
        std::fgetc(input) != EOF || std::ferror(input))
    {
        data.clear(); return FileReadResult::Invalid;
    }
    return FileReadResult::Ready;
}
inline bool retainedBytes(std::uint64_t samples, std::uint64_t capacity,
                          std::uint32_t channels, std::uint32_t rate, std::uint64_t& bytes) noexcept
{
    bytes = 0;
    if (!samples || (channels != 1 && channels != 2) || samples % channels ||
        capacity < samples || capacity > PCM_BYTES / sizeof(std::int16_t)) return false;
    std::uint64_t source_bytes = 0;
    if (!sourceBytes(static_cast<std::uint32_t>(samples / channels), channels, rate, source_bytes)) return false;
    bytes = capacity * sizeof(std::int16_t); // retained ownership, not just logical length
    return true;
}

}
