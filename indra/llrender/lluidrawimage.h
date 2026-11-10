/** Shared UI-image draw history. Pending textures never bind the white placeholder. */
#ifndef LL_LLUIDRAWIMAGE_H
#define LL_LLUIDRAWIMAGE_H
#include "lltexture.h"
#include "llrect.h"
#include <vector>

struct LLUIDrawImage
{
    LLPointer<LLTexture> texture;
    LLRectf uv;
    LLRectf center;
    F32 degrees = 0.f;
    bool solid = false;
    bool scaleInner = false;
};

// One bounded, lazy history per live view; no global pointer-keyed lifetime cache.
class LLUIImageDrawState
{
public:
    struct Site
    {
        F32 key[8];
        LLUIDrawImage image;
        U64 visit = 0;
    };
    std::vector<Site> sites;
    U64 visit = 0;
};

class LLUIImageDrawScope
{
public:
    explicit LLUIImageDrawScope(LLUIImageDrawState& state);
    ~LLUIImageDrawScope();
    LLUIImageDrawScope(const LLUIImageDrawScope&) = delete;
    LLUIImageDrawScope& operator=(const LLUIImageDrawScope&) = delete;
private:
    LLUIImageDrawState* mPrevious;
};
#endif
