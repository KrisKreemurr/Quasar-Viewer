/**
 * @brief Bounded CPU animation exports awaiting renderer consumption (#1343).
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

template<uint32_t MaxJoints, uint32_t MaxBlends>
struct LLAnimPending
{
    struct Blend { uint32_t flags; float weight; const void* list; uint32_t index; float time; };
    std::vector<uint32_t> hdr; // joint, first blend, count
    std::vector<Blend> blends;
    std::vector<float> values; // twelve floats per procedural state
    static constexpr size_t appendHeaderLimit = size_t(MaxJoints) * 2u * 3u;

    void clear() { hdr.clear(); blends.clear(); values.clear(); }
    void beginExport(uint32_t consumerEpoch)
    {
        mRepeated = consumerEpoch == mEpoch && !hdr.empty();
        if (consumerEpoch != mEpoch) clear();
        mEpoch = consumerEpoch;
    }
    void endExport()
    {
        // The usual consumed-per-frame path pays no compaction. When presentation
        // is skipped, each complete CPU update replaces the last update per joint.
        if (mRepeated) compact();
        mRepeated = false;
    }

    bool compact()
    {
        constexpr size_t missing = std::numeric_limits<size_t>::max();
        std::array<size_t, MaxJoints> last;
        last.fill(missing);
        if (hdr.size() % 3u) { clear(); return false; }
        size_t previousBlendEnd = 0, previousValueEnd = 0;
        // Validate before modifying anything. Producers append disjoint payload
        // in header order; this permits allocation-free, leftward compaction.
        for (size_t h = 0; h < hdr.size(); h += 3u)
        {
            const size_t first = hdr[h + 1u], count = hdr[h + 2u];
            if (hdr[h] >= MaxJoints || count > MaxBlends || first < previousBlendEnd ||
                first > blends.size() || count > blends.size() - first)
            { clear(); return false; }
            previousBlendEnd = first + count;
            for (size_t b = first; b < first + count; ++b)
            {
                if (blends[b].flags & 1u) continue;
                const size_t off = size_t(blends[b].index) * 12u;
                if (off < previousValueEnd || off > values.size() || values.size() - off < 12u)
                { clear(); return false; }
                previousValueEnd = off + 12u;
            }
            last[hdr[h]] = h;
        }
        size_t nh = 0, nb = 0, nv = 0;
        for (size_t h = 0; h < hdr.size(); h += 3u)
        {
            if (last[hdr[h]] != h) continue;
            const uint32_t joint = hdr[h], first = hdr[h + 1u], count = hdr[h + 2u];
            hdr[nh++] = joint;
            hdr[nh++] = uint32_t(nb);
            hdr[nh++] = count;
            for (size_t i = first; i < size_t(first) + count; ++i)
            {
                Blend b = blends[i];
                if (!(b.flags & 1u))
                {
                    std::memmove(values.data() + nv, values.data() + size_t(b.index) * 12u, 12u * sizeof(float));
                    b.index = uint32_t(nv / 12u);
                    nv += 12u;
                }
                blends[nb++] = b;
            }
        }
        hdr.resize(nh); blends.resize(nb); values.resize(nv);
        return true;
    }
private:
    uint32_t mEpoch = std::numeric_limits<uint32_t>::max();
    bool mRepeated = false;
};
