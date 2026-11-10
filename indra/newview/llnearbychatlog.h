// #703 Nearby chat as a model: the last lines said nearby this session, UI-independent.
// Fed where nearby chat arrives (LLFloaterIMNearbyChat::addMessage); read by the new interface's
// chat view. Fixed size ring (kCapacity lines); a revision counter tells readers what changed.
#pragma once

#include "lluuid.h"
#include <array>
#include <cstdint>
#include <string>

class LLNearbyChatLog
{
public:
    struct Line
    {
        std::string from, text, time;
        LLUUID      fromID;
    };
    static constexpr size_t kCapacity = 200;

    static LLNearbyChatLog& instance();
    void     add(const std::string& from, const LLUUID& fromID, const std::string& text, const std::string& time);
    size_t   size() const { return mCount; }
    // i = 0 is the oldest kept line.
    const Line& at(size_t i) const { return mLines[(mFirst + i) % kCapacity]; }
    uint64_t revision() const { return mRevision; }

private:
    std::array<Line, kCapacity> mLines{};
    size_t                      mFirst = 0, mCount = 0;
    uint64_t                    mRevision = 0;
};
