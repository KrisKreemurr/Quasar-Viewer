/**
 * @file lltexturedata.cpp
 * @brief Texture record: CPU pixel copy + metadata the Vulkan backend uploads from
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


#include "linden_common.h"

#include "lltexturedata.h"

#include <atomic>
#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#if LL_WINDOWS
#include "llwin32headers.h"
#elif defined(__linux__)
#include <time.h>
#endif

#include "llerror.h"
#include "llimage.h"
#include "llmath.h"
#include "llrender.h"
#include "llframetimer.h"
#include "llthread.h"

//----------------------------------------------------------------------------
const F32 MIN_TEXTURE_LIFETIME = 10.f;

U32 LLTextureData::sFrameCount = 0;

//statics

S32 LLTextureData::sCount                   = 0;

F32 LLTextureData::sLastFrameTime           = 0.f;

//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
//-----------------------
//debug use
S32 LLTextureData::sCurTexSizeBar = -1 ;
S32 LLTextureData::sCurTexPickSize = -1 ;

//optimization for when we don't need to calculate mIsMask
bool LLTextureData::sSkipAnalyzeAlpha;

//------------------------
//****************************************************************************************************
//End for texture auditing use only
//****************************************************************************************************

//----------------------------------------------------------------------------
bool is_little_endian()
{
    S32 a = 0x12345678;
    U8 *c = (U8*)(&a);

    return (*c == 0x78) ;
}

//static
void LLTextureData::initClass(bool skip_analyze_alpha /* = false */)
{
    sSkipAnalyzeAlpha = skip_analyze_alpha;
}

//static
S32 LLTextureData::dataFormatBits(LLPixelFormat dataformat)
{
    switch (dataformat)
    {
    case LLPixelFormat::COMPRESSED_RED:                         return 8;
    case LLPixelFormat::COMPRESSED_RG:                          return 16;
    case LLPixelFormat::COMPRESSED_RGB:                         return 24;
    case LLPixelFormat::COMPRESSED_SRGB:                        return 32;
    case LLPixelFormat::COMPRESSED_RGBA:                        return 32;
    case LLPixelFormat::COMPRESSED_SRGB_ALPHA:                  return 32;
    case LLPixelFormat::COMPRESSED_LUMINANCE:                   return 8;
    case LLPixelFormat::COMPRESSED_LUMINANCE_ALPHA:             return 16;
    case LLPixelFormat::COMPRESSED_ALPHA:                       return 8;
    case LLPixelFormat::BC1_RGBA:          return 4;
    case LLPixelFormat::BC1_SRGB_ALPHA:    return 4;
    case LLPixelFormat::BC2_RGBA:          return 8;
    case LLPixelFormat::BC2_SRGB_ALPHA:    return 8;
    case LLPixelFormat::BC3_RGBA:          return 8;
    case LLPixelFormat::BC3_SRGB_ALPHA:    return 8;
    case LLPixelFormat::LUMINANCE:                              return 8;
    case LLPixelFormat::LUMINANCE8:                             return 8;
    case LLPixelFormat::ALPHA:                                  return 8;
    case LLPixelFormat::ALPHA8:                                 return 8;
    case LLPixelFormat::RED:                                    return 8;
    case LLPixelFormat::R8:                                     return 8;
    case LLPixelFormat::COLOR_INDEX:                            return 8;
    case LLPixelFormat::LUMINANCE_ALPHA:                        return 16;
    case LLPixelFormat::LUMINANCE8_ALPHA8:                      return 16;
    case LLPixelFormat::RG:                                     return 16;
    case LLPixelFormat::RG8:                                    return 16;
    case LLPixelFormat::RGB:                                    return 24;
    case LLPixelFormat::SRGB:                                   return 24;
    case LLPixelFormat::RGB8:                                   return 24;
    case LLPixelFormat::R11F_G11F_B10F:                         return 32;
    case LLPixelFormat::RGBA:                                   return 32;
    case LLPixelFormat::RGBA8:                                  return 32;
    case LLPixelFormat::RGB10_A2:                               return 32;
    case LLPixelFormat::SRGB_ALPHA:                             return 32;
    case LLPixelFormat::BGRA:                                   return 32;      // Used for QuickTime media textures on the Mac
    case LLPixelFormat::DEPTH_COMPONENT:                        return 24;
    case LLPixelFormat::DEPTH_COMPONENT24:                      return 24;
    case LLPixelFormat::RGBA16:                                 return 64;
    case LLPixelFormat::R16F:                                   return 16;
    case LLPixelFormat::RG16F:                                  return 32;
    case LLPixelFormat::RGB16F:                                 return 48;
    case LLPixelFormat::RGBA16F:                                return 64;
    case LLPixelFormat::R32F:                                   return 32;
    case LLPixelFormat::RG32F:                                  return 64;
    case LLPixelFormat::RGB32F:                                 return 96;
    case LLPixelFormat::RGBA32F:                                return 128;
    default:
        LL_ERRS() << "LLTextureData::Unknown format: " << std::hex << static_cast<U32>(dataformat) << std::dec << LL_ENDL;
        return 0;
    }
}

//static
S64 LLTextureData::dataFormatBytes(LLPixelFormat dataformat, S32 width, S32 height)
{
    switch (dataformat)
    {
    case LLPixelFormat::BC1_RGBA:
    case LLPixelFormat::BC1_SRGB_ALPHA:
    case LLPixelFormat::BC2_RGBA:
    case LLPixelFormat::BC2_SRGB_ALPHA:
    case LLPixelFormat::BC3_RGBA:
    case LLPixelFormat::BC3_SRGB_ALPHA:
        if (width < 4) width = 4;
        if (height < 4) height = 4;
        break;
    default:
        break;
    }
    S64 bytes (((S64)width * (S64)height * (S64)dataFormatBits(dataformat)+7)>>3);
    S64 aligned = (bytes+3)&~3;
    return aligned;
}

//static
S32 LLTextureData::dataFormatComponents(LLPixelFormat dataformat)
{
    switch (dataformat)
    {
      case LLPixelFormat::BC1_RGBA:    return 3;
      case LLPixelFormat::BC1_SRGB_ALPHA: return 3;
      case LLPixelFormat::BC2_RGBA:    return 4;
      case LLPixelFormat::BC2_SRGB_ALPHA: return 4;
      case LLPixelFormat::BC3_RGBA:    return 4;
      case LLPixelFormat::BC3_SRGB_ALPHA: return 4;
      case LLPixelFormat::LUMINANCE:                        return 1;
      case LLPixelFormat::ALPHA:                            return 1;
      case LLPixelFormat::RED:                              return 1;
      case LLPixelFormat::COLOR_INDEX:                      return 1;
      case LLPixelFormat::LUMINANCE_ALPHA:                  return 2;
      case LLPixelFormat::RG:                               return 2;
      case LLPixelFormat::RGB:                              return 3;
      case LLPixelFormat::SRGB:                             return 3;
      case LLPixelFormat::RGBA:                             return 4;
      case LLPixelFormat::SRGB_ALPHA:                       return 4;
      case LLPixelFormat::BGRA:                             return 4;       // Used for QuickTime media textures on the Mac
      default:
        LL_ERRS() << "LLTextureData::Unknown format: " << std::hex << static_cast<U32>(dataformat) << std::dec << LL_ENDL;
        return 0;
    }
}

//----------------------------------------------------------------------------

// static
void LLTextureData::updateStats(F32 current_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    sLastFrameTime = current_time;
}

// static
void LLTextureData::updateClass()
{
    sFrameCount++;
}

//----------------------------------------------------------------------------

LLTextureData::LLTextureData(bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
{
    init(usemipmaps);
    setSize(0, 0, 0);
    sCount++;
}

LLTextureData::LLTextureData(U32 width, U32 height, U8 components, bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
{
    llassert( components <= 4 );
    init(usemipmaps);
    setSize(width, height, components);
    sCount++;
}

LLTextureData::LLTextureData(const LLImageRaw* imageraw, bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
{
    init(usemipmaps);
    setSize(0, 0, 0);
    sCount++;

    setFromRaw(0, imageraw);
}


// static
void (*LLTextureData::sVKOnDestroy)(const LLTextureData* image, U64 serial) = nullptr;

std::atomic<S64> LLTextureData::sVKCpuBytes{0};
std::atomic<S32> LLTextureData::sVKCpuCount{0};

namespace
{
bool vkPrepThreadCpuNs(U64& value) noexcept
{
#if LL_WINDOWS
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER kernelTime{}, userTime{};
    kernelTime.LowPart = kernel.dwLowDateTime; kernelTime.HighPart = kernel.dwHighDateTime;
    userTime.LowPart = user.dwLowDateTime; userTime.HighPart = user.dwHighDateTime;
    const U64 limitValue = (std::numeric_limits<U64>::max)();
    if (kernelTime.QuadPart > limitValue - userTime.QuadPart) return false;
    const U64 ticks = kernelTime.QuadPart + userTime.QuadPart;
    if (ticks > limitValue / 100ull) return false;
    value = ticks * 100ull;
    return true;
#elif defined(__linux__) && defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0 || now.tv_sec < 0
        || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return false;
    const U64 seconds = U64(now.tv_sec);
    const U64 limitValue = (std::numeric_limits<U64>::max)();
    if (seconds > limitValue / 1000000000ull) return false;
    const U64 ns = seconds * 1000000000ull;
    if (U64(now.tv_nsec) > limitValue - ns) return false;
    value = ns + U64(now.tv_nsec);
    return true;
#else
    (void)value;
    return false;
#endif
}

    // (#707 diagnostics) every image owning a CPU copy: the stats walk only sees the
    // fetched texture list, so copies held elsewhere (dynamic / media / local images)
    // were counted in sVKCpuBytes but never attributed.
    std::mutex& vk_owned_mutex() { static std::mutex m; return m; }
    std::unordered_set<const LLTextureData*>& vk_owned_set() { static std::unordered_set<const LLTextureData*> s; return s; }
}

// static
void LLTextureData::vkForEachOwned(void (*fn)(const LLTextureData*, S64, void*), void* ctx)
{
    std::lock_guard<std::mutex> lk(vk_owned_mutex());
    for (const LLTextureData* img : vk_owned_set()) fn(img, img->mVKOwnedBytes, ctx);
}

void LLTextureData::releaseVKPlaceholder()
{
    setVKOwned(nullptr);
    mVKCpuData = nullptr;
    mVKCpuWritable = false;
    mVKCpuImmutable = false;
}

void LLTextureData::setVKOwned(LLImageRaw* raw)
{
    if (mVKOwnedBytes > 0) { sVKCpuBytes -= mVKOwnedBytes; --sVKCpuCount; }
    mVKCpuDataOwned = raw;
    mVKCpuImmutable = false;
    mVKOwnedBytes = (raw && raw->getData()) ? (S64)raw->getDataSize() : 0;
    if (mVKOwnedBytes > 0) { sVKCpuBytes += mVKOwnedBytes; ++sVKCpuCount; }
    {
        std::lock_guard<std::mutex> lk(vk_owned_mutex());
        if (mVKOwnedBytes > 0) vk_owned_set().insert(this);
        else vk_owned_set().erase(this);
    }
    if (raw) mVKCpuDropped = false;   // (#707) pixels again
}

void LLTextureData::setVKCpuData(LLImageRaw* raw, S32 generation)
{
    mVKCpuData = raw;
    mVKCpuWritable = false;
    mVKCpuImmutable = false;
    if (raw) mVKCpuDropped = false;
    mVKGeneration = generation;
    mVKDirtyGen = VK_DIRTY_UNKNOWN;
    mVKDirtyFromGen = VK_DIRTY_UNKNOWN;
    mVKDirtyX0 = mVKDirtyY0 = mVKDirtyX1 = mVKDirtyY1 = 0;
    mVKPrepDirtyGen = VK_DIRTY_UNKNOWN;
    mVKPrepDirtyFromGen = VK_DIRTY_UNKNOWN;
    mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
}

void LLTextureData::setVKCpuData(LLImageRaw* raw, S32 generation, S32 x, S32 y, S32 width, S32 height)
{
    const S32 previousGeneration = mVKGeneration;
    const bool validRect = raw && raw->getData() && x >= 0 && y >= 0 && width > 0 && height > 0 &&
        (U64)x + (U64)width <= raw->getWidth() && (U64)y + (U64)height <= raw->getHeight() &&
        previousGeneration >= 0 && generation > previousGeneration;
    const bool extend = validRect && mVKDirtyGen != VK_DIRTY_UNKNOWN && mVKDirtyGen == previousGeneration &&
        mVKDirtyFromGen != VK_DIRTY_UNKNOWN;
    const bool prepExtend = validRect && mVKPrepDirtyGen != VK_DIRTY_UNKNOWN &&
        mVKPrepDirtyGen == previousGeneration && mVKPrepDirtyFromGen != VK_DIRTY_UNKNOWN;
    const bool prepHasRect = mVKPrepDirtyX1 > mVKPrepDirtyX0 && mVKPrepDirtyY1 > mVKPrepDirtyY0;

    mVKCpuData = raw;
    mVKCpuWritable = false;
    mVKCpuImmutable = false;
    if (raw) mVKCpuDropped = false;
    mVKGeneration = generation;
    if (!validRect)
    {
        mVKDirtyGen = VK_DIRTY_UNKNOWN;
        mVKDirtyFromGen = VK_DIRTY_UNKNOWN;
        mVKDirtyX0 = mVKDirtyY0 = mVKDirtyX1 = mVKDirtyY1 = 0;
        mVKPrepDirtyGen = VK_DIRTY_UNKNOWN;
        mVKPrepDirtyFromGen = VK_DIRTY_UNKNOWN;
        mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
        return;
    }

    if (!extend)
    {
        mVKDirtyFromGen = previousGeneration;
        mVKDirtyX0 = x; mVKDirtyY0 = y;
        mVKDirtyX1 = x + width; mVKDirtyY1 = y + height;
    }
    else
    {
        mVKDirtyX0 = llmin(mVKDirtyX0, x);
        mVKDirtyY0 = llmin(mVKDirtyY0, y);
        mVKDirtyX1 = llmax(mVKDirtyX1, x + width);
        mVKDirtyY1 = llmax(mVKDirtyY1, y + height);
    }
    mVKDirtyGen = generation;

    if (!prepExtend)
    {
        mVKPrepDirtyFromGen = previousGeneration;
        mVKPrepDirtyX0 = x; mVKPrepDirtyY0 = y;
        mVKPrepDirtyX1 = x + width; mVKPrepDirtyY1 = y + height;
    }
    else if (!prepHasRect)
    {
        // A just-acknowledged source has an empty rectangle but keeps its accepted base.
        // The first later edit starts a fresh rect without spuriously including (0, 0).
        mVKPrepDirtyX0 = x; mVKPrepDirtyY0 = y;
        mVKPrepDirtyX1 = x + width; mVKPrepDirtyY1 = y + height;
    }
    else
    {
        mVKPrepDirtyX0 = llmin(mVKPrepDirtyX0, x);
        mVKPrepDirtyY0 = llmin(mVKPrepDirtyY0, y);
        mVKPrepDirtyX1 = llmax(mVKPrepDirtyX1, x + width);
        mVKPrepDirtyY1 = llmax(mVKPrepDirtyY1, y + height);
    }
    mVKPrepDirtyGen = generation;
}

bool LLTextureData::captureVKImmutablePixels(LLVKImmutablePixelSnapshot& snapshot) const
{
    // This is called by the owner thread while its caller holds a CPU-copy lease. Only
    // setFromRaw()/replaceVKCpuData() copies are worker-safe: setSubImageVK mutates its
    // writable buffer in place, and setVKCpuData may point at a producer-owned surface.
    if (!mVKCpuImmutable || mVKCpuWritable || mVKCpuDataOwned.isNull() ||
        mVKCpuData != mVKCpuDataOwned.get()) return false;
    const LLImageRaw* raw = mVKCpuDataOwned.get();
    if (!raw || raw->isBufferInvalid() || !raw->getData() || raw->getDataSize() <= 0 ||
        raw->getWidth() == 0 || raw->getHeight() == 0 || raw->getComponents() < 1 ||
        raw->getComponents() > 4) return false;
    const U64 expectedBytes = (U64)raw->getWidth() * raw->getHeight() * (U64)raw->getComponents();
    if (expectedBytes > (U64)(std::numeric_limits<S32>::max)() ||
        expectedBytes != (U64)raw->getDataSize()) return false;

    LLVKImmutablePixelSnapshot captured;
    captured.mRaw = mVKCpuDataOwned;
    captured.mWidth = raw->getWidth();
    captured.mHeight = raw->getHeight();
    captured.mComponents = raw->getComponents();
    captured.mFormatInternal = mFormatInternal;
    captured.mFormatPrimary = mFormatPrimary;
    captured.mFormatType = mFormatType;
    captured.mGeneration = mVKGeneration;
    captured.mSerial = mVKSerial;
    captured.mIsMask = mIsMask;
    captured.mAlphaGradientPct = mVKAlphaGradientPct;
    captured.mAlphaCommittedSide = mVKAlphaCommittedSide;
    snapshot = std::move(captured);  // failure leaves the caller's prior snapshot unchanged
    return true;
}

LLTextureData::EVKPrepCapture LLTextureData::captureVKPrepSource(
    S32 consumerGeneration, S32 discard, U8 coverageCut,
    const LLVKTexturePrep::CaptureBudget& budget, const LLVKCpuLease& lease,
    LLVKTexturePrep::SourceSnapshot& output, U64& mainCopyBytes) const noexcept
{
    U64 mainCopyCpuNs = 0;
    bool mainCopyCpuValid = true;
    return captureVKPrepSource(consumerGeneration, discard, coverageCut, budget, lease,
                               output, mainCopyBytes, mainCopyCpuNs, mainCopyCpuValid);
}

LLTextureData::EVKPrepCapture LLTextureData::captureVKPrepSource(
    S32 consumerGeneration, S32 discard, U8 coverageCut,
    const LLVKTexturePrep::CaptureBudget& budget, const LLVKCpuLease& lease,
    LLVKTexturePrep::SourceSnapshot& output, U64& mainCopyBytes,
    U64& mainCopyCpuNs, bool& mainCopyCpuValid) const noexcept
{
    mainCopyBytes = 0;
    mainCopyCpuNs = 0;
    mainCopyCpuValid = true; // immutable/zero-copy capture performs no main copy
    if (!assert_main_thread() || lease.get() != this)
        return EVKPrepCapture::Invalid;

    LLImageRaw* raw = mVKCpuData;
    if (!raw || !raw->getData() || raw->isBufferInvalid())
        return EVKPrepCapture::Pending;
    if (mVKGeneration < 0 || !mVKSerial || raw->getComponents() < 1 || raw->getComponents() > 4 ||
        !raw->getWidth() || !raw->getHeight())
        return EVKPrepCapture::Invalid;
    if (consumerGeneration == mVKGeneration)
        return EVKPrepCapture::Pending;

    const U32 fullWidth = raw->getWidth();
    const U32 fullHeight = raw->getHeight();
    const U32 components = raw->getComponents();
    if ((U64)fullWidth * fullHeight > (std::numeric_limits<U64>::max)() / components)
        return EVKPrepCapture::Invalid;
    const U64 expectedFullBytes = (U64)fullWidth * fullHeight * components;
    if (expectedFullBytes != (U64)raw->getDataSize())
        return EVKPrepCapture::Invalid;

    auto makeSnapshot = [&](const std::shared_ptr<const void>& owner, const U8* data,
                            U64 bytes, U64 retainedBytes, U32 x, U32 y, U32 width,
                            U32 height, U32 rowBytes, S32 baseGeneration,
                            U64 copiedBytes, U64 copiedCpuNs, bool copiedCpuValid) -> bool
    {
        if (!owner || !data || !bytes || !retainedBytes || retainedBytes > budget.maxRetainedBytes ||
            copiedBytes > budget.maxMainCopyBytes)
            return false;
        LLVKTexturePrep::SourceSnapshot candidate;
        candidate.owner = owner;
        candidate.byteCount = bytes;
        candidate.retainedBackingBytes = retainedBytes;
        candidate.mainCopyBytes = copiedBytes;
        candidate.mainCopyCpuNs = copiedCpuNs;
        candidate.mainCopyCpuValid = copiedCpuValid;
        candidate.serial = mVKSerial;
        candidate.generation = (U32)mVKGeneration;
        candidate.baseGeneration = baseGeneration;
        candidate.discard = discard;
        candidate.width = fullWidth;
        candidate.height = fullHeight;
        const S32 recordedWidth = getWidth();
        const S32 recordedHeight = getHeight();
        candidate.fullWidth = recordedWidth > 0
            ? std::max<U32>(fullWidth, static_cast<U32>(recordedWidth)) : fullWidth;
        candidate.fullHeight = recordedHeight > 0
            ? std::max<U32>(fullHeight, static_cast<U32>(recordedHeight)) : fullHeight;
        candidate.levels = 1;
        candidate.firstLevel = 0;
        candidate.components = components;
        // setSubImageVK stores a writable CPU backing in canonical byte-channel
        // order: BGRA/BGR source bytes have already been swapped to RGBA/RGB. The
        // legacy mFormat* fields intentionally retain the media wire format because
        // getVKCpuData() readers still use them. Describe the bytes retained by this
        // snapshot instead, so a worker neither swaps them twice nor treats already
        // unpacked byte channels as a packed scalar type (GStreamer UINT_8888_REV).
        LLPixelFormat snapshotPixelFormat = mFormatPrimary;
        LLPixelType snapshotPixelType = mFormatType;
        if (mVKCpuWritable)
        {
            if (mFormatPrimary == LLPixelFormat::BGRA)
                snapshotPixelFormat = LLPixelFormat::RGBA;
            else if (mFormatPrimary == LLPixelFormat::BGR)
                snapshotPixelFormat = LLPixelFormat::RGB;
            snapshotPixelType = LLPixelType::UBYTE;
        }
        candidate.pixelFormat = static_cast<U32>(snapshotPixelFormat);
        candidate.pixelType = static_cast<U32>(snapshotPixelType);
        candidate.storageFormat = static_cast<U32>(mFormatInternal);
        candidate.alphaCommittedSide = mVKAlphaCommittedSide;
        candidate.alphaGradientPct = mVKAlphaGradientPct;
        candidate.coverageCut = coverageCut;
        candidate.spanCount = 1;
        candidate.isMask = mIsMask;
        candidate.kind = LLVKTexturePrep::SourceKind::PixelBytes;
        auto& span = candidate.spans[0];
        span.data = data;
        span.bytes = bytes;
        span.rowBytes = rowBytes;
        span.x = x;
        span.y = y;
        span.width = width;
        span.height = height;
        span.level = 0;
        span.levelCount = 1;
        span.layout = LLVKTexturePrep::SpanLayout::Rows;
        if (!candidate.valid()) return false;
        output = std::move(candidate);
        mainCopyBytes = copiedBytes;
        return true;
    };

    LLVKImmutablePixelSnapshot immutable;
    if (captureVKImmutablePixels(immutable))
    {
        const U64 retainedBytes = expectedFullBytes + sizeof(LLVKImmutablePixelSnapshot) +
                                  sizeof(LLImageRaw) + 64u;
        if (retainedBytes < expectedFullBytes || retainedBytes > budget.maxRetainedBytes)
            return EVKPrepCapture::Deferred;
        try
        {
            auto holder = std::make_shared<const LLVKImmutablePixelSnapshot>(std::move(immutable));
            const LLImageRaw* pixels = holder->pixels();
            if (!pixels || !pixels->getData()) return EVKPrepCapture::Invalid;
            const std::shared_ptr<const void> owner(holder, holder.get());
            if (!makeSnapshot(owner, pixels->getData(), expectedFullBytes, retainedBytes,
                              0, 0, fullWidth, fullHeight, fullWidth * components, -1, 0, 0, true))
                return EVKPrepCapture::Deferred;
            return EVKPrepCapture::Ready;
        }
        catch (...)
        {
            return EVKPrepCapture::Deferred;
        }
    }

    S32 x = 0, y = 0, width = (S32)fullWidth, height = (S32)fullHeight;
    const bool patch = mVKPrepDirtyGen != VK_DIRTY_UNKNOWN &&
        mVKPrepDirtyGen == mVKGeneration && consumerGeneration == mVKPrepDirtyFromGen;
    if (patch)
    {
        x = mVKPrepDirtyX0;
        y = mVKPrepDirtyY0;
        width = mVKPrepDirtyX1 - mVKPrepDirtyX0;
        height = mVKPrepDirtyY1 - mVKPrepDirtyY0;
        if (width <= 0 || height <= 0) return EVKPrepCapture::Pending;
    }
    else
    {
        x = y = 0;
        width = (S32)fullWidth;
        height = (S32)fullHeight;
    }
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        (U64)x + (U64)width > fullWidth || (U64)y + (U64)height > fullHeight)
        return EVKPrepCapture::Invalid;

    const U64 rowBytes = (U64)width * components;
    if (rowBytes > (std::numeric_limits<U32>::max)() ||
        (U64)height > (std::numeric_limits<U64>::max)() / rowBytes)
        return EVKPrepCapture::Invalid;
    const U64 copyBytes = rowBytes * (U64)height;
    if (!copyBytes || copyBytes > budget.maxMainCopyBytes ||
        copyBytes > (std::numeric_limits<size_t>::max)())
        return EVKPrepCapture::Deferred;
    const U64 plannedRetainedBytes = copyBytes + sizeof(std::vector<U8>) + 64u;
    if (plannedRetainedBytes < copyBytes || plannedRetainedBytes > budget.maxRetainedBytes)
        return EVKPrepCapture::Deferred;

    U64 copyCpuStart = 0;
    mainCopyCpuValid = false;
    const bool copyCpuStartValid = vkPrepThreadCpuNs(copyCpuStart);
    bool copyCpuFinished = false;
    auto finishCopyTiming = [&]() noexcept
    {
        if (copyCpuFinished) return;
        copyCpuFinished = true;
        U64 end = 0;
        if (copyCpuStartValid && vkPrepThreadCpuNs(end) && end >= copyCpuStart)
        {
            mainCopyCpuNs = end - copyCpuStart;
            mainCopyCpuValid = true;
        }
        else
        {
            mainCopyCpuNs = 0;
            mainCopyCpuValid = false;
        }
    };
    try
    {
        auto bytes = std::make_shared<std::vector<U8>>((size_t)copyBytes);
        const U8* src = raw->getData();
        for (S32 row = 0; row < height; ++row)
        {
            const size_t srcOffset = ((size_t)(y + row) * fullWidth + (size_t)x) * components;
            memcpy(bytes->data() + (size_t)row * (size_t)rowBytes, src + srcOffset, (size_t)rowBytes);
        }
        mainCopyBytes = copyBytes;
        finishCopyTiming();
        const U64 retainedBytes = (U64)bytes->capacity() + sizeof(*bytes) + 64u;
        if (retainedBytes < bytes->capacity() || retainedBytes > budget.maxRetainedBytes)
            return EVKPrepCapture::Deferred;
        const std::shared_ptr<const void> owner(bytes, bytes.get());
        if (!makeSnapshot(owner, bytes->data(), copyBytes, retainedBytes,
                          (U32)x, (U32)y, (U32)width, (U32)height,
                          (U32)rowBytes, patch ? consumerGeneration : -1, copyBytes,
                          mainCopyCpuNs, mainCopyCpuValid))
            return EVKPrepCapture::Deferred;
    }
    catch (...)
    {
        finishCopyTiming();
        return EVKPrepCapture::Deferred;
    }
    return EVKPrepCapture::Ready;
}

bool LLTextureData::acknowledgeVKPrepSource(S32 acceptedGeneration)
{
    if (!assert_main_thread() || acceptedGeneration != mVKGeneration) return false;
    mVKPrepDirtyFromGen = acceptedGeneration;
    mVKPrepDirtyGen = acceptedGeneration;
    mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
    return true;
}

// static
const char* LLTextureData::vkLeaseName(U8 reason)
{
    static const char* const kNames[VKLEASE_COUNT] = {"media", "local", "stream", "ui", "alpha",
                                                       "callback", "preview", "appearance"};
    return reason < VKLEASE_COUNT ? kNames[reason] : "?";
}

void LLTextureData::acquireVKLease(U8 reason)
{
    if (reason < VKLEASE_COUNT && mVKLeases[reason] < 0xFFFFu) ++mVKLeases[reason];
}

void LLTextureData::releaseVKLease(U8 reason)
{
    if (reason < VKLEASE_COUNT && mVKLeases[reason] > 0) --mVKLeases[reason];
}

U32 LLTextureData::getVKLeaseMask() const
{
    U32 mask = 0;
    for (U32 r = 0; r < VKLEASE_COUNT; ++r)
        if (mVKLeases[r]) mask |= 1u << r;
    if (mVKCpuWritable) mask |= 1u << VKLEASE_MEDIA;
    if (mVKAlphaJobs) mask |= 1u << VKLEASE_ALPHA;
    return mask;
}

void LLTextureData::markVKUiUsed()
{
    mVKUiUsed = true;
    mVKUiDrawTime = (F32)LLFrameTimer::getElapsedSeconds();
}

bool LLTextureData::vkUiNeedsCpu() const
{
    // (5 s: a floater that draws every frame refreshes it; one that closed releases it)
    constexpr F32 UI_LEASE_SEC = 5.f;
    return mVKUiUsed && mVKUiUploadedGen != mVKGeneration
        && (F32)LLFrameTimer::getElapsedSeconds() - mVKUiDrawTime < UI_LEASE_SEC;
}

bool LLTextureData::dropVKCpuData()
{
    // Only the owned, immutable decode copy, and only lease-free: a writable (media)
    // buffer, a non-owned source (font atlas) or a running analysis keeps it.
    if (mVKCpuDataOwned.isNull() || mVKCpuData != mVKCpuDataOwned.get() || getVKLeaseMask()) return false;
    setVKOwned(nullptr);
    mVKCpuData = nullptr;
    mVKCpuImmutable = false;
    mVKCpuDropped = true;
    // The generation stays: consumers that uploaded it (UI cache, world image) keep
    // their pixels; the next decode bumps it.
    return true;
}

void LLTextureData::replaceVKCpuData(LLImageRaw* raw, S32 discard_level)
{
    if (!raw || !raw->getData()) return;
    mVKCpuWritable = false;
    setVKOwned(raw);
    mVKCpuImmutable = true; // replaceVKCpuData transfers immutable ownership from its caller
    mVKCpuData = mVKCpuDataOwned.get();
    mCurrentDiscardLevel = discard_level;
    ++mVKGeneration;
    mVKDirtyGen = VK_DIRTY_UNKNOWN;   // a new CPU copy
    mVKPrepDirtyGen = VK_DIRTY_UNKNOWN;
    mVKPrepDirtyFromGen = VK_DIRTY_UNKNOWN;
    mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
}

bool LLTextureData::getVKDirtyRect(S32 from_gen, S32& x, S32& y, S32& w, S32& h) const
{
    if (mVKDirtyGen == VK_DIRTY_UNKNOWN || mVKDirtyGen != mVKGeneration || from_gen != mVKDirtyFromGen)
    {
        return false;
    }
    x = mVKDirtyX0;
    y = mVKDirtyY0;
    w = mVKDirtyX1 > mVKDirtyX0 ? mVKDirtyX1 - mVKDirtyX0 : 0;
    h = mVKDirtyY1 > mVKDirtyY0 ? mVKDirtyY1 - mVKDirtyY0 : 0;
    return true;
}

void LLTextureData::resetVKDirty()
{
    mVKDirtyFromGen = mVKGeneration;
    mVKDirtyGen = mVKGeneration;
    mVKDirtyX0 = mVKDirtyY0 = mVKDirtyX1 = mVKDirtyY1 = 0;
}

// static
U64 LLTextureData::nextVKSerial()
{
    static std::atomic<U64> s_serial{0};
    return ++s_serial;
}

// VK async alpha analysis: images with a job in flight, by serial (a destroyed image
// unregisters, so a late result never touches it).
// Both are intentionally leaked function-local singletons: ~LLTextureData touches them, and
// LLImageGLs can be released from other translation units' static destructors (#275:
// LLViewerMediaTexture::sMediaMap died after these and crashed at exit). A leaked
// object is never destroyed, so it is valid regardless of static destruction order.
static std::mutex& vk_alpha_mutex()
{
    static std::mutex* m = new std::mutex();
    return *m;
}

static std::unordered_map<U64, LLTextureData*>& vk_alpha_images()
{
    static std::unordered_map<U64, LLTextureData*>* m = new std::unordered_map<U64, LLTextureData*>();
    return *m;
}

LLTextureData::~LLTextureData()
{
    {
        std::lock_guard<std::mutex> lk(vk_alpha_mutex());
        vk_alpha_images().erase(mVKSerial);
    }
    setVKOwned(nullptr);
    if (sVKOnDestroy)
    {
        sVKOnDestroy(this, mVKSerial);
    }
    sCount--;
}

void LLTextureData::init(bool usemipmaps)
{
    // keep these members in the same order as declared in lltexturedata.h
    // so that it is obvious by visual inspection if we forgot to
    // init a field.

    mLastBindTime = 0.f;

    mUseMipMaps = usemipmaps;
    mHasExplicitFormat = false;

    mIsMask = false;
    mNeedsAlphaAndPickMask = true ;
    mAlphaStride = 0 ;
    mAlphaOffset = 0 ;

    mContentReady = false ;
    mWidth = 0;
    mHeight = 0;
    mCurrentDiscardLevel = -1;

    mComponents = 0;
    mMaxDiscardLevel = MAX_DISCARD_LEVEL;

    mAddressMode = LLTexUnit::TAM_WRAP;

    mFormatInternal = LLPixelFormat::NONE;
    mFormatPrimary = LLPixelFormat::NONE;
    mFormatType = LLPixelType::UBYTE;

    mCategory = -1;
}

void LLTextureData::cleanup()
{
    // (The pick alpha map is gone: objects are picked by the GPU, #704.)
}

//----------------------------------------------------------------------------

//this function is used to check the size of a texture image.
//so dim should be a positive number
static bool check_power_of_two(S32 dim)
{
    if(dim < 0)
    {
        return false ;
    }
    if(!dim)//0 is a power-of-two number
    {
        return true ;
    }
    return !(dim & (dim - 1)) ;
}

//static
bool LLTextureData::checkSize(S32 width, S32 height)
{
    return check_power_of_two(width) && check_power_of_two(height);
}

bool LLTextureData::setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level)
{
    if (width != mWidth || height != mHeight || ncomponents != mComponents)
    {
        // Non-mipped UI rasters use their exact physical pixel size in Vulkan.
        // Record their components too, before setFromRaw derives the pixel format.
        // Keep the existing power-of-two policy for mipmapped/fetched sources.
        if (!checkSize(width, height) && (mUseMipMaps || width <= 0 || height <= 0))
        {
            LL_WARNS() << llformat("Texture has non power of two dimension: %dx%d",width,height) << LL_ENDL;
            return false;
        }

        mWidth = width;
        mHeight = height;
        mComponents = ncomponents;
        if (ncomponents > 0)
        {
            mMaxDiscardLevel = 0;
            while (width > 1 && height > 1 && mMaxDiscardLevel < MAX_DISCARD_LEVEL)
            {
                mMaxDiscardLevel++;
                width >>= 1;
                height >>= 1;
            }

            if(discard_level > 0)
            {
                mMaxDiscardLevel = llmax(mMaxDiscardLevel, (S8)discard_level);
            }
        }
        else
        {
            mMaxDiscardLevel = MAX_DISCARD_LEVEL;
        }
    }

    return true;
}

//----------------------------------------------------------------------------

// virtual
void LLTextureData::dump()
{
    LL_INFOS() << "mMaxDiscardLevel " << S32(mMaxDiscardLevel)
            << " mLastBindTime " << mLastBindTime
            << " mUseMipMaps " << S32(mUseMipMaps)
            << " mCurrentDiscardLevel " << S32(mCurrentDiscardLevel)
            << " mFormatInternal " << S32(mFormatInternal)
            << " mFormatPrimary " << S32(mFormatPrimary)
            << " mFormatType " << S32(mFormatType)
            << " mHasExplicitFormat " << S32(mHasExplicitFormat)
            << " mVKGeneration " << mVKGeneration
            << LL_ENDL;
}

//----------------------------------------------------------------------------
void LLTextureData::forceUpdateBindStats(void) const
{
    mLastBindTime = sLastFrameTime;
}

F32 LLTextureData::getTimePassedSinceLastBound()
{
    return sLastFrameTime - mLastBindTime ;
}

void LLTextureData::setExplicitFormat( LLPixelFormat internal_format, LLPixelFormat primary_format, LLPixelType type_format, bool swap_bytes )
{
    // Note: must be called before createTexture()
    // Note: it's up to the caller to ensure that the format matches the number of components.
    mHasExplicitFormat = true;
    mFormatInternal = internal_format;
    mFormatPrimary = primary_format;
    if (type_format == LLPixelType::NONE)
        mFormatType = LLPixelType::UBYTE;
    else
        mFormatType = type_format;

    calcAlphaChannelOffsetAndStride() ;
}

//----------------------------------------------------------------------------

bool LLTextureData::setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!width || !height)
    {
        return true;
    }
    // Write the sub-rect into this image's OWN CPU pixel buffer and bump the
    // generation, so the VK backend's resolveTexture() re-uploads it on the next
    // bind. This is the CEF/web media path (LLViewerMediaImpl::doMediaTexUpdate).
    return setSubImageVK(datap, data_width, data_height, x_pos, y_pos, width, height);
}

bool LLTextureData::setSubImageVK(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    const S32 comps = getComponents();
    const S32 w = getWidth(), h = getHeight();
    if (!datap || comps < 1 || comps > 4 || w <= 0 || h <= 0
        || x_pos < 0 || y_pos < 0
        || x_pos + width > w || y_pos + height > h
        || x_pos + width > data_width || y_pos + height > data_height)
    {
        return false;
    }
    // Writable buffer of the texture's full size. Never write into a raw we don't
    // own exclusively (setFromRaw's copies may be shared / immutable).
    bool newBacking = false;
    if (!mVKCpuWritable || mVKCpuDataOwned.isNull()
        || mVKCpuDataOwned->getWidth() != w || mVKCpuDataOwned->getHeight() != h
        || mVKCpuDataOwned->getComponents() != comps)
    {
        newBacking = true;
        setVKOwned(new LLImageRaw((U16)w, (U16)h, (S8)comps));
        if (!mVKCpuDataOwned->getData())
        {
            setVKOwned(nullptr);
            mVKCpuData = nullptr;
            mVKCpuWritable = false;
            return false;
        }
        memset(mVKCpuDataOwned->getData(), 0, (size_t)w * h * comps);
        mVKCpuWritable = true;
        mVKDirtyGen = VK_DIRTY_UNKNOWN;   // a new buffer: every texel changed
    }
    // The backend expects RGB(A) byte order; web media arrives as BGR(A).
    const bool swap_rb = comps >= 3 && (mFormatPrimary == LLPixelFormat::BGRA || mFormatPrimary == LLPixelFormat::BGR);
    U8* dst_base = mVKCpuDataOwned->getData();
    for (S32 r = 0; r < height; ++r)
    {
        const U8* src = datap + ((size_t)(y_pos + r) * data_width + x_pos) * comps;
        U8* dst = dst_base + ((size_t)(y_pos + r) * w + x_pos) * comps;
        if (!swap_rb)
        {
            memcpy(dst, src, (size_t)width * comps);
            continue;
        }
        for (S32 x = 0; x < width; ++x, src += comps, dst += comps)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            if (comps == 4) dst[3] = src[3];
        }
    }
    mVKCpuData = mVKCpuDataOwned.get();
    mVKCpuImmutable = false; // setSubImageVK is the sole writer of this backing
    // #458: grow the dirty rectangle (a tracked change keeps the chain valid).
    const bool chain = mVKDirtyGen != VK_DIRTY_UNKNOWN && mVKDirtyGen == mVKGeneration;
    const S32 previousGeneration = mVKGeneration;
    const bool prepChain = !newBacking && mVKPrepDirtyGen != VK_DIRTY_UNKNOWN &&
        mVKPrepDirtyGen == previousGeneration && mVKPrepDirtyFromGen != VK_DIRTY_UNKNOWN;
    const bool prepHasRect = mVKPrepDirtyX1 > mVKPrepDirtyX0 && mVKPrepDirtyY1 > mVKPrepDirtyY0;
    ++mVKGeneration;
    if (chain)
    {
        mVKDirtyGen = mVKGeneration;
        if (mVKDirtyX1 <= mVKDirtyX0 || mVKDirtyY1 <= mVKDirtyY0)
        {
            mVKDirtyX0 = x_pos; mVKDirtyY0 = y_pos;
            mVKDirtyX1 = x_pos + width; mVKDirtyY1 = y_pos + height;
        }
        else
        {
            mVKDirtyX0 = llmin(mVKDirtyX0, x_pos);
            mVKDirtyY0 = llmin(mVKDirtyY0, y_pos);
            mVKDirtyX1 = llmax(mVKDirtyX1, x_pos + width);
            mVKDirtyY1 = llmax(mVKDirtyY1, y_pos + height);
        }
    }
    if (newBacking)
    {
        // The zero-filled image plus this patch is a new source, not a delta over the
        // previous texture contents. Require a bounded full snapshot for its base.
        mVKPrepDirtyGen = VK_DIRTY_UNKNOWN;
        mVKPrepDirtyFromGen = VK_DIRTY_UNKNOWN;
        mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
    }
    else
    {
        if (!prepChain)
        {
            mVKPrepDirtyFromGen = previousGeneration;
            mVKPrepDirtyX0 = x_pos; mVKPrepDirtyY0 = y_pos;
            mVKPrepDirtyX1 = x_pos + width; mVKPrepDirtyY1 = y_pos + height;
        }
        else if (!prepHasRect)
        {
            mVKPrepDirtyX0 = x_pos; mVKPrepDirtyY0 = y_pos;
            mVKPrepDirtyX1 = x_pos + width; mVKPrepDirtyY1 = y_pos + height;
        }
        else
        {
            mVKPrepDirtyX0 = llmin(mVKPrepDirtyX0, x_pos);
            mVKPrepDirtyY0 = llmin(mVKPrepDirtyY0, y_pos);
            mVKPrepDirtyX1 = llmax(mVKPrepDirtyX1, x_pos + width);
            mVKPrepDirtyY1 = llmax(mVKPrepDirtyY1, y_pos + height);
        }
        mVKPrepDirtyGen = mVKGeneration;
    }
    mContentReady = true;
    return true;
}

bool LLTextureData::setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return setSubImage(imageraw->getData(), imageraw->getWidth(), imageraw->getHeight(), x_pos, y_pos, width, height);
}

bool LLTextureData::setFromRaw(S32 discard_level, const LLImageRaw* imageraw, bool defer_copy)
{
    return setFromRawInternal(discard_level, imageraw, defer_copy, nullptr);
}

bool LLTextureData::setFromImmutableRaw(S32 discard_level, const LLPointer<LLImageRaw>& imageraw)
{
    return imageraw.notNull() && setFromRawInternal(discard_level, imageraw.get(), false, imageraw.get());
}

bool LLTextureData::setFromRawInternal(S32 discard_level, const LLImageRaw* imageraw, bool defer_copy,
                                       LLImageRaw* immutable_raw)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // No GPU object is created here. Record the dimensions so callers that read
    // getWidth()/getComponents() still work, mark the texture "created", and retain
    // the pixels as the VK CPU data source; the VK backend uploads them to a VK image
    // on first bind. This also runs for fonts/UI images loaded before the backend is
    // installed.
    if (!imageraw || imageraw->isBufferInvalid())
    {
        mContentReady = false;
        return false;
    }
    if (immutable_raw && (immutable_raw != imageraw || !immutable_raw->getData() ||
                         immutable_raw->getDataSize() <= 0))
        return false;
    if (discard_level < 0)
    {
        discard_level = (mCurrentDiscardLevel >= 0) ? mCurrentDiscardLevel : 0;
    }
    S32 raw_w = imageraw->getWidth();
    S32 raw_h = imageraw->getHeight();
    setSize(raw_w << discard_level, raw_h << discard_level,
            imageraw->getComponents(), discard_level);
    // Stamp the current discard level. getDiscardLevel() returns mCurrentDiscardLevel, which
    // drives LLViewerFetchedTexture's fetch state machine
    // (getCurrentDiscardLevelForFetching / updateFetch). Without this it stays
    // -1 forever in VK mode (no GL object is ever created), so the fetcher
    // reads current_discard<0, treats the texture as "nothing landed", and --
    // for the mForceToSaveRawImage path (all sculpt maps) -- forces the level
    // to -1 even when a saved raw exists. The result is every texture/sculpt
    // map stalls at whatever level decoded first (observed savedlvl=2) and the
    // finer mips are never requested. Stamping the level the raw arrived at
    // lets updateFetch see "have N, want desired" and walk N -> ... -> 0.
    mCurrentDiscardLevel = discard_level;
    // Pixel format from the component count (enum bookkeeping only).
    // analyzeAlpha depends on it.
    if (!mHasExplicitFormat)
    {
        switch (mComponents)
        {
        case 1: mFormatInternal = LLPixelFormat::LUMINANCE8;        mFormatPrimary = LLPixelFormat::LUMINANCE;       break;
        case 2: mFormatInternal = LLPixelFormat::LUMINANCE8_ALPHA8; mFormatPrimary = LLPixelFormat::LUMINANCE_ALPHA; break;
        case 3: mFormatInternal = LLPixelFormat::RGB8;              mFormatPrimary = LLPixelFormat::RGB;             break;
        case 4: mFormatInternal = LLPixelFormat::RGBA8;             mFormatPrimary = LLPixelFormat::RGBA;            break;
        default: break;
        }
        mFormatType = LLPixelType::UBYTE;
        // (#717) An RGB decode latches the alpha analysis off for good (calcAlpha...
        // clears mNeedsAlphaAndPickMask). If this image now arrives WITH an alpha channel
        // (a re-decode that gained one), re-arm it and drop the facts committed for the
        // alpha-less content, or the face keeps the old classification forever.
        if (mVKAlphaLatchedOff && (mComponents == 2 || mComponents == 4))
        {
            mVKAlphaLatchedOff = false;
            mNeedsAlphaAndPickMask = true;
            mAlphaOffset = 0;
            mIsMask = false;
            mVKAlphaGradientPct = 255;
            mVKAlphaCommittedSide = 0;
        }
        calcAlphaChannelOffsetAndStride();
    }
    // defer_copy: the caller allocates the texture now and fills pixels later via
    // setSubImage (LLViewerMediaImpl::doMediaTexUpdate does this for CEF/web media).
    // In that mode `imageraw` is a NON-OWNING wrapper (LLImageRaw ctor no_copy=true)
    // whose reported getDataSize() spans the full power-of-two texture, but whose
    // backing buffer only holds the smaller media subimage. Deep-copying
    // getDataSize() bytes from it over-reads past the buffer and segfaults (seen in
    // doMediaTexUpdate on the login splash page). Record dimensions only; the pixels
    // arrive through setSubImage -> setSubImageVK, which fills an owned CPU buffer.
    if (defer_copy)
    {
        // No texel changes here: a valid dirty-rectangle chain stays valid (#458; the
        // media path calls this before every setSubImage).
        const bool chain = mVKDirtyGen != VK_DIRTY_UNKNOWN && mVKDirtyGen == mVKGeneration;
        const bool prepChain = mVKPrepDirtyGen != VK_DIRTY_UNKNOWN &&
            mVKPrepDirtyGen == mVKGeneration && mVKPrepDirtyFromGen != VK_DIRTY_UNKNOWN;
        ++mVKGeneration;
        if (chain) mVKDirtyGen = mVKGeneration;
        if (prepChain) mVKPrepDirtyGen = mVKGeneration;
        else
        {
            mVKPrepDirtyGen = VK_DIRTY_UNKNOWN;
            mVKPrepDirtyFromGen = VK_DIRTY_UNKNOWN;
            mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
        }
        // This path records dimensions only; the next setSubImage publishes new bytes.
        mVKCpuData = nullptr;
        mVKCpuImmutable = false;
        mContentReady = true;
        return true;
    }
    // Retain the CPU pixels (owning): the backend uploads on first bind, so the raw
    // must survive. This makes UI/LLUIImage textures (logo, button skins) resident
    // and correctly textured instead of white. mVKCpuData points at the owned
    // copy so getVKCpuData() stays valid for the backend's resolveTexture().
    // Generic callers deep-COPY the pixels into an immutable buffer: arbitrary
    // setFromRaw inputs may be borrowed or reused by their producer. Only the explicit
    // producer-qualified path above may retain a source without copying.
    if (immutable_raw)
    {
        mVKCpuWritable = false;
        setVKOwned(immutable_raw);
        mVKCpuImmutable = true;
    }
    else if (imageraw->getData() && imageraw->getDataSize() > 0)
    {
        mVKCpuWritable = false;
        setVKOwned(new LLImageRaw(imageraw->getData(),
                                  (U16)imageraw->getWidth(),
                                  (U16)imageraw->getHeight(),
                                  imageraw->getComponents()));
        mVKCpuImmutable = true;
    }
    else
    {
        mVKCpuWritable = false;
        setVKOwned(const_cast<LLImageRaw*>(imageraw));
        mVKCpuImmutable = false;
    }
    mVKCpuData = mVKCpuDataOwned.get();
    // Alpha classification of the base level (getIsAlphaMask). (Picks test the drawn
    // texels on the GPU, #704.)
    if (mVKCpuData && mVKCpuData->getData() && !vkPostAlphaAnalysis(raw_w, raw_h))
    {
        analyzeAlpha(mVKCpuData->getData(), raw_w, raw_h);
    }
    ++mVKGeneration;
    mVKDirtyGen = VK_DIRTY_UNKNOWN;   // a new CPU copy
    mVKPrepDirtyGen = VK_DIRTY_UNKNOWN;
    mVKPrepDirtyFromGen = VK_DIRTY_UNKNOWN;
    mVKPrepDirtyX0 = mVKPrepDirtyY0 = mVKPrepDirtyX1 = mVKPrepDirtyY1 = 0;
    mContentReady = true;
    return true;
}

//force to invalidate the texture, most likely a sculpty texture
void LLTextureData::invalidateDiscard()
{
    mCurrentDiscardLevel = -1 ; //invalidate mCurrentDiscardLevel.
}

//----------------------------------------------------------------------------

void LLTextureData::setAddressMode(LLTexUnit::eTextureAddressMode mode)
{
    // Kept on this image; LLTexUnit applies it when the image is bound.
    mAddressMode = mode;
    mVKAddressMode = mode;
}

S32 LLTextureData::getHeight(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 height = mHeight >> discard_level;
    if (height < 1) height = 1;
    return height;
}

S32 LLTextureData::getWidth(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 width = mWidth >> discard_level;
    if (width < 1) width = 1;
    return width;
}

S64 LLTextureData::getBytes(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 w = mWidth>>discard_level;
    S32 h = mHeight>>discard_level;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    return dataFormatBytes(mFormatPrimary, w, h);
}

bool LLTextureData::isJustBound() const
{
    return sLastFrameTime - mLastBindTime < 0.5f;
}

bool LLTextureData::getBoundRecently() const
{
    return (bool)(sLastFrameTime - mLastBindTime < MIN_TEXTURE_LIFETIME);
}

bool LLTextureData::getIsAlphaMask() const
{
    llassert_always(!sSkipAnalyzeAlpha);
    return mIsMask;
}

const S8 INVALID_OFFSET = -99 ;
void LLTextureData::setNeedsAlphaAndPickMask(bool need_mask)
{
    if(mNeedsAlphaAndPickMask != need_mask)
    {
        mNeedsAlphaAndPickMask = need_mask;

        if(mNeedsAlphaAndPickMask)
        {
            mAlphaOffset = 0 ;
        }
        else //do not need alpha mask
        {
            mAlphaOffset = INVALID_OFFSET ;
            mIsMask = false;
        }
    }
}

void LLTextureData::calcAlphaChannelOffsetAndStride()
{
    if(mAlphaOffset == INVALID_OFFSET)//do not need alpha mask
    {
        return ;
    }

    mAlphaStride = -1 ;
    switch (mFormatPrimary)
    {
    case LLPixelFormat::LUMINANCE:
    case LLPixelFormat::ALPHA:
        mAlphaStride = 1;
        break;
    case LLPixelFormat::LUMINANCE_ALPHA:
        mAlphaStride = 2;
        break;
    case LLPixelFormat::RED:
    case LLPixelFormat::RGB:
    case LLPixelFormat::SRGB:
        mNeedsAlphaAndPickMask = false;
        mIsMask = false;
        mVKAlphaLatchedOff = true;
        return; //no alpha channel.
    case LLPixelFormat::RGBA:
    case LLPixelFormat::SRGB_ALPHA:
        mAlphaStride = 4;
        break;
    case LLPixelFormat::BGRA:
        mAlphaStride = 4;
        break;
    default:
        break;
    }

    mAlphaOffset = -1 ;
    if (mFormatType == LLPixelType::UBYTE)
    {
        mAlphaOffset = mAlphaStride - 1 ;
    }
    else if(is_little_endian())
    {
        if (mFormatType == LLPixelType::UINT_8888)
        {
            mAlphaOffset = 0 ;
        }
        else if (mFormatType == LLPixelType::UINT_8888_REV)
        {
            mAlphaOffset = 3 ;
        }
    }
    else //big endian
    {
        if (mFormatType == LLPixelType::UINT_8888)
        {
            mAlphaOffset = 3 ;
        }
        else if (mFormatType == LLPixelType::UINT_8888_REV)
        {
            mAlphaOffset = 0 ;
        }
    }

    if( mAlphaStride < 1 || //unsupported format
        mAlphaOffset < 0 || //unsupported type
        (mFormatPrimary == LLPixelFormat::BGRA && mFormatType != LLPixelType::UBYTE)) //unknown situation
    {
        LL_WARNS() << "Cannot analyze alpha for image with format type " << std::hex << static_cast<U32>(mFormatType) << std::dec << LL_ENDL;

        mNeedsAlphaAndPickMask = false ;
        mIsMask = false;
    }
}

// Pure version of analyzeAlpha for the worker (same math).
// Also counts the texels with partial alpha (16 < a < 240, the LLCleanTexture
// uploadSlot metric that RenderCleanAutoMaskMaxGradient is defined against) in the
// same pass: `partial` out of w*h.
static bool vk_alpha_is_mask(const U8* data_in, U32 w, U32 h, U32 alpha_offset, U32 alpha_stride,
                             U32& partial)
{
    partial = 0;
    auto is_partial = [](U32 a) -> U32 { return (a > 16 && a < 240) ? 1u : 0u; };
    U32 length = w * h;
    U32 alphatotal = 0;
    U32 sample[16];
    memset(sample, 0, sizeof(U32) * 16);
    if (w >= 2 && h >= 2 && (w % 2) == 0 && (h % 2) == 0)
    {
        const U8* rowstart = data_in + alpha_offset;
        for (U32 y = 0; y < h; y += 2)
        {
            const U8* current = rowstart;
            for (U32 x = 0; x < w; x += 2)
            {
                const U32 s1 = current[0];             alphatotal += s1;
                const U32 s2 = current[w * alpha_stride]; alphatotal += s2;
                current += alpha_stride;
                const U32 s3 = current[0];             alphatotal += s3;
                const U32 s4 = current[w * alpha_stride]; alphatotal += s4;
                current += alpha_stride;
                ++sample[s1 / 16]; ++sample[s2 / 16]; ++sample[s3 / 16]; ++sample[s4 / 16];
                partial += is_partial(s1) + is_partial(s2) + is_partial(s3) + is_partial(s4);
                const U32 asum = (s1 + s2 + s3 + s4);
                alphatotal += asum;
                sample[asum / (16 * 4)] += 4;
            }
            rowstart += 2 * w * alpha_stride;
        }
        length *= 2;
    }
    else
    {
        const U8* current = data_in + alpha_offset;
        for (U32 i = 0; i < length; i++)
        {
            const U32 s1 = *current;
            alphatotal += s1;
            ++sample[s1 / 16];
            partial += is_partial(s1);
            current += alpha_stride;
        }
    }
    U32 midrangetotal = 0, lowerhalftotal = 0, upperhalftotal = 0;
    for (U32 i = 2; i < 13; i++) midrangetotal += sample[i];
    for (U32 i = 0; i < 8; i++)  lowerhalftotal += sample[i];
    for (U32 i = 8; i < 16; i++) upperhalftotal += sample[i];
    return !(midrangetotal > length / 48 ||
             (lowerhalftotal == length && alphatotal != 0) ||
             (upperhalftotal == length && alphatotal != 255 * length));
}

bool LLTextureData::vkPostAlphaAnalysis(S32 w, S32 h)
{
    if (sSkipAnalyzeAlpha || !mNeedsAlphaAndPickMask || !mVKCpuDataOwned.notNull()
        || mVKCpuData != mVKCpuDataOwned.get() || w <= 0 || h <= 0)
        return false;
    // Same preconditions as analyzeAlpha's format gate.
    if (mFormatType != LLPixelType::UBYTE || mAlphaStride <= 0 || mAlphaOffset < 0)
        return false;
    LL::WorkQueue::ptr_t main_q = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t gen_q  = LL::WorkQueue::getInstance("General");
    if (!main_q || !gen_q) return false;
    struct Result { bool is_mask = false; U8 grad_pct = 255; };
    LLPointer<LLImageRaw> raw = mVKCpuDataOwned;   // thread-safe refcount; immutable copy
    const U32 off = mAlphaOffset, stride = mAlphaStride;
    const U64 serial = mVKSerial;
    // Commit gate (#82): decent resolution, or this image's full-resolution decode.
    const U16 side = (U16)llmin(w, h);
    const bool decisive = side >= VK_ALPHA_COMMIT_MIN_SIDE || mCurrentDiscardLevel == 0;
    {
        std::lock_guard<std::mutex> lk(vk_alpha_mutex());
        vk_alpha_images()[serial] = this;
    }
    // #707 ALPHA lease: the copy stays until every job posted for this image returned
    // (a drop before the commit would lose the classification the tier-2 capture needs).
    if (mVKAlphaJobs < 0xFF) ++mVKAlphaJobs;
    const bool posted = main_q->postTo(
        gen_q,
        [raw, w, h, off, stride]()
        {
            Result r;
            const U8* d = raw->getData();
            if (!d) return r;
            U32 partial = 0;
            r.is_mask = vk_alpha_is_mask(d, (U32)w, (U32)h, off, stride, partial);
            const U64 n = (U64)w * (U64)h;
            r.grad_pct = (U8)llmin((U64)100, ((U64)partial * 100) / llmax((U64)1, n));
            return r;
        },
        [serial, raw, side, decisive](Result r)   // main loop
        {
            LLTextureData* img = nullptr;
            {
                std::lock_guard<std::mutex> lk(vk_alpha_mutex());
                auto& alpha_images = vk_alpha_images();
                auto it = alpha_images.find(serial);
                if (it == alpha_images.end()) return;   // destroyed meanwhile
                img = it->second;
                if (!img) return;
                // #707: the registration lives until the image's last job returned (the
                // ALPHA lease count); only then is it erased.
                if (img->mVKAlphaJobs) --img->mVKAlphaJobs;
                if (!img->mVKAlphaJobs) alpha_images.erase(it);
                // A newer decode replaced the pixels: its own job applies -- erasing on
                // the first result dropped the NEWER job's whenever two decodes of one
                // image were in flight together (the final, full-resolution analysis was
                // the one lost).
                // (#717) ...but only when that newer decode can supply a verdict. A newer
                // decode that is smaller than this one's, or below the commit resolution
                // (a residency down-res / re-fetch at a coarse level), commits nothing, so
                // returning here threw away the only decisive analysis and the texture sat
                // on "pending" (alpha-blend) for good. Same asset => same alpha content, and
                // the committed-side rule below still keeps a larger decision sticky.
                if (img->mVKCpuData != raw.get() && (img->mVKCpuWritable || !decisive)) return;
            }
            // Alpha classification: only a decisive decode commits, and never one
            // smaller than what is already committed (a decision made at a higher
            // resolution sticks through residency down-res / late low mips).
            if (decisive && side >= img->mVKAlphaCommittedSide)
            {
                img->mIsMask = r.is_mask;
                img->mVKAlphaGradientPct = r.grad_pct;
                img->mVKAlphaCommittedSide = side;
            }
        });
    if (!posted)
    {
        std::lock_guard<std::mutex> lk(vk_alpha_mutex());
        if (mVKAlphaJobs) --mVKAlphaJobs;
        if (!mVKAlphaJobs) vk_alpha_images().erase(serial);
        return false;
    }
    return true;
}

void LLTextureData::commitVKAlphaFacts(bool is_mask, U8 grad_pct, U16 side)
{
    if (!side || side < mVKAlphaCommittedSide) return;
    mIsMask = is_mask;
    mVKAlphaGradientPct = grad_pct;
    mVKAlphaCommittedSide = side;
}

void LLTextureData::analyzeAlpha(const void* data_in, U32 w, U32 h)
{
    if(sSkipAnalyzeAlpha || !mNeedsAlphaAndPickMask)
    {
        return ;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 length = w * h;
    U32 alphatotal = 0;

    U32 sample[16];
    memset(sample, 0, sizeof(U32)*16);

    // generate histogram of quantized alpha.
    // also add-in the histogram of a 2x2 box-sampled version.  The idea is
    // this will mid-skew the data (and thus increase the chances of not
    // being used as a mask) from high-frequency alpha maps which
    // suffer the worst from aliasing when used as alpha masks.
    if (w >= 2 && h >= 2)
    {
        llassert(w % 2 == 0);
        llassert(h % 2 == 0);
        const U8* rowstart = ((const U8*) data_in) + mAlphaOffset;
        for (U32 y = 0; y < h; y += 2)
        {
            const U8* current = rowstart;
            for (U32 x = 0; x < w; x += 2)
            {
                const U32 s1 = current[0];
                alphatotal += s1;
                const U32 s2 = current[w * mAlphaStride];
                alphatotal += s2;
                current += mAlphaStride;
                const U32 s3 = current[0];
                alphatotal += s3;
                const U32 s4 = current[w * mAlphaStride];
                alphatotal += s4;
                current += mAlphaStride;

                ++sample[s1/16];
                ++sample[s2/16];
                ++sample[s3/16];
                ++sample[s4/16];

                const U32 asum = (s1+s2+s3+s4);
                alphatotal += asum;
                sample[asum/(16*4)] += 4;
            }

            rowstart += 2 * w * mAlphaStride;
        }
        length *= 2; // we sampled everything twice, essentially
    }
    else
    {
        const U8* current = ((const U8*) data_in) + mAlphaOffset;
        for (U32 i = 0; i < length; i++)
        {
            const U32 s1 = *current;
            alphatotal += s1;
            ++sample[s1/16];
            current += mAlphaStride;
        }
    }

    // if more than 1/16th of alpha samples are mid-range, this
    // shouldn't be treated as a 1-bit mask

    // also, if all of the alpha samples are clumped on one half
    // of the range (but not at an absolute extreme), then consider
    // this to be an intentional effect and don't treat as a mask.

    U32 midrangetotal = 0;
    for (U32 i = 2; i < 13; i++)
    {
        midrangetotal += sample[i];
    }
    U32 lowerhalftotal = 0;
    for (U32 i = 0; i < 8; i++)
    {
        lowerhalftotal += sample[i];
    }
    U32 upperhalftotal = 0;
    for (U32 i = 8; i < 16; i++)
    {
        upperhalftotal += sample[i];
    }

    if (midrangetotal > length/48 || // lots of midrange, or
        (lowerhalftotal == length && alphatotal != 0) || // all close to transparent but not all totally transparent, or
        (upperhalftotal == length && alphatotal != 255*length)) // all close to opaque but not all totally opaque
    {
        mIsMask = false; // not suitable for masking
    }
    else
    {
        mIsMask = true;
    }
}

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
void LLTextureData::setCurTexSizebar(S32 index, bool set_pick_size)
{
    sCurTexSizeBar = index ;

    if(set_pick_size)
    {
        sCurTexPickSize = (1 << index) ;
    }
    else
    {
        sCurTexPickSize = -1 ;
    }
}
void LLTextureData::resetCurTexSizebar()
{
    sCurTexSizeBar = -1 ;
    sCurTexPickSize = -1 ;
}
