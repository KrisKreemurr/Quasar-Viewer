// #703 Nearby chat as a model (see llnearbychatlog.h).
#include "llviewerprecompiledheaders.h"
#include "llnearbychatlog.h"

LLNearbyChatLog& LLNearbyChatLog::instance()
{
    static LLNearbyChatLog log;
    return log;
}

void LLNearbyChatLog::add(const std::string& from, const LLUUID& fromID, const std::string& text, const std::string& time)
{
    const size_t slot = (mFirst + mCount) % kCapacity;
    if (mCount == kCapacity)
        mFirst = (mFirst + 1) % kCapacity; // the oldest line makes room (fixed size)
    else
        ++mCount;
    Line& line  = mLines[slot];
    line.from   = from;
    line.fromID = fromID;
    line.text   = text;
    line.time   = time;
    ++mRevision;
}
