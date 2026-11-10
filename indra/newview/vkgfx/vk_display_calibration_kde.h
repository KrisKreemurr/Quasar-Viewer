// KDE output selection and CTA-861 parsing; neither input is logged or retained.
#pragma once
#include "vk_display_calibration.h"
#include "vk_display_limits.h"
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkdisplay
{
struct KscreenMatch { std::string connector; float whiteNits = 0.f; };

inline bool jsonNumber(const boost::json::value* value, double& result)
{
    if (!value) return false;
    if (value->is_double()) result = value->as_double();
    else if (value->is_int64()) result = static_cast<double>(value->as_int64());
    else if (value->is_uint64()) result = static_cast<double>(value->as_uint64());
    else return false;
    return std::isfinite(result);
}
inline bool safeConnector(std::string_view name)
{
    if (name.empty() || name.size() > 64) return false;
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}
inline bool matchKscreenOutput(std::string_view json, const LogicalBounds& bounds, KscreenMatch& match)
{
    if (json.empty() || json.size() > 256 * 1024 || bounds.width <= 0 || bounds.height <= 0) return false;
    boost::system::error_code ec;
    const auto root = boost::json::parse(boost::json::string_view(json.data(), json.size()), ec);
    if (ec || !root.is_object()) return false;
    const auto* outputs = root.as_object().if_contains("outputs");
    if (!outputs || !outputs->is_array() || outputs->as_array().size() > 64) return false;
    unsigned matches = 0, overlaps = 0;
    KscreenMatch candidate;
    for (const auto& item : outputs->as_array())
    {
        if (!item.is_object()) return false;
        const auto& obj = item.as_object();
        const auto* connected = obj.if_contains("connected"); const auto* enabled = obj.if_contains("enabled");
        if (!connected || !connected->is_bool() || !enabled || !enabled->is_bool()) return false;
        if (!connected->as_bool() || !enabled->as_bool()) continue;
        const auto* pos = obj.if_contains("pos"); const auto* size = obj.if_contains("size"); const auto* name = obj.if_contains("name");
        if (!pos || !pos->is_object() || !size || !size->is_object()) return false;
        double x, y, width, height, scale, white;
        if (!jsonNumber(pos->as_object().if_contains("x"), x) || !jsonNumber(pos->as_object().if_contains("y"), y) ||
            !jsonNumber(size->as_object().if_contains("width"), width) || !jsonNumber(size->as_object().if_contains("height"), height) ||
            !jsonNumber(obj.if_contains("scale"), scale) || scale < 0.5 || scale > 8.0 ||
            width <= 0 || height <= 0 || width > 65536 || height > 65536 ||
            std::abs(x) > 100000 || std::abs(y) > 100000) return false;
        // KScreen's size is already rotated; do not swap it a second time.
        const double logicalWidth = width / scale, logicalHeight = height / scale;
        if (std::min(x + logicalWidth, double(bounds.x) + bounds.width) - std::max(x, double(bounds.x)) > 1.0 &&
            std::min(y + logicalHeight, double(bounds.y) + bounds.height) - std::max(y, double(bounds.y)) > 1.0)
            ++overlaps;
        if (std::abs(x - bounds.x) > 1.0 || std::abs(y - bounds.y) > 1.0 ||
            std::abs(logicalWidth - bounds.width) > 1.0 || std::abs(logicalHeight - bounds.height) > 1.0) continue;
        ++matches;
        const auto* hdr = obj.if_contains("hdr");
        if (!hdr || !hdr->is_bool() || !hdr->as_bool() || !name || !name->is_string() ||
            !jsonNumber(obj.if_contains("sdr-brightness"), white) || white < 80.0 || white > kDisplayReferenceWhiteMaxNits) continue;
        const auto nameView = name->as_string();
        std::string connector(nameView.data(), nameView.size());
        if (!safeConnector(connector)) continue;
        candidate = {connector, static_cast<float>(white)};
    }
    if (matches != 1 || overlaps != 1 || candidate.connector.empty()) return false;
    match = std::move(candidate);
    return true;
}

inline bool ctaAdvertisedMaximum(const std::vector<uint8_t>& bytes, float& nits)
{
    static constexpr std::array<uint8_t, 8> signature{0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00};
    if (bytes.size() < 128 || !std::equal(signature.begin(), signature.end(), bytes.begin())) return false;
    const size_t blocks = size_t(bytes[126]) + 1u;
    if (blocks > 8 || bytes.size() != blocks * 128u) return false;
    float found = 0.f;
    for (size_t block = 0; block < blocks; ++block)
    {
        unsigned sum = 0; for (size_t i = 0; i < 128; ++i) sum += bytes[block * 128 + i];
        if ((sum & 255u) != 0) return false;
        if (!block || bytes[block * 128] != 0x02) continue;
        const size_t base = block * 128;
        if (bytes[base + 1] < 3) continue;
        const uint8_t end = bytes[base + 2];
        if (end == 0) continue;
        if (end < 4 || end > 127) return false;
        for (size_t at = 4; at < end;)
        {
            const uint8_t header = bytes[base + at++];
            const size_t len = header & 31u;
            if (at + len > end) return false;
            if ((header >> 5) == 7 && len >= 4 && bytes[base + at] == 6 &&
                (bytes[base + at + 1] & 0x0c) != 0 && (bytes[base + at + 2] & 0x01) != 0 &&
                bytes[base + at + 3] != 0)
            {
                const float candidate = 50.f * std::exp2(float(bytes[base + at + 3]) / 32.f);
                if (found && std::abs(found - candidate) > 0.01f) return false;
                found = candidate;
            }
            at += len;
        }
    }
    if (!std::isfinite(found) || found < 80.f || found > 10000.f) return false;
    nits = found;
    return true;
}
}
