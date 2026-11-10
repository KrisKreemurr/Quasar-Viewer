// #703 UI on RmlUi: the skin image atlas and the "llimage" decorator.
//
// Skin images (textures.xml, LLUIImage) are packed on first use into one fixed-size atlas
// texture owned by the RmlUi backend, so the whole classic UI samples a single image and
// nothing is uploaded again unless a new image appears. RCSS draws them with
//     decorator: llimage(<LLUIImage name>);   image-color: <tint>;
// which reproduces LLUIImage::draw (scale-9 borders from the image's scale region,
// borders shrunk when the box is smaller than them). Fixed cost, bounded memory: one
// 2048x2048 RGBA page (16 MB GPU + its 16 MB host mirror); an image that does not fit is
// refused (the caller falls back to the LLView renderer), never an allocation crash.
#pragma once

#include "lluiimage.h"

class LLImageRaw;
#include <RmlUi/Core/Types.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class LLRmlUiVK;

class LLRmlUiAtlas
{
public:
    static constexpr uint32_t kSize = 2048;
    static constexpr uint32_t kMaxImage = 256; // larger images are not atlased
    struct Entry
    {
        uint16_t x = 0, y = 0, w = 0, h = 0; // atlas pixels, top-left origin, padding excluded
        LLRectf  scale{ 0.f, 1.f, 1.f, 0.f }; // LLUIImage scale region (normalized, y up)
        bool     ok = false;
    };

    static LLRmlUiAtlas& instance();

    // Install the "llui:atlas" texture source on a backend and register the decorator
    // (once per process, after Rml::Initialise). reset() forgets all packing (the backend
    // went away; its texture with it).
    void attach(LLRmlUiVK& backend);
    void registerDecorator();
    void reset();

    // The atlas entry for a UI image, packing it on first use. Null: not atlasable (no
    // local file, too large, undecodable, atlas full); the caller must fall back.
    const Entry* find(const LLUIImage* image);
    const Entry* find(const std::string& name);
    bool         canPresent(const LLUIImage* image) { return image && find(image); }
    // #1056 vector icons (classic icons off): the image's vector source rasterised at exactly
    // this pixel size, drawn as one quad. Null: no vector source, or the vector budget is spent
    // (then the bitmap entry is used, as LLUIImage does when its rasterizer fails).
    const Entry* findVector(const LLUIImage* image, uint32_t pixelW, uint32_t pixelH);

    uint32_t usedPixels() const { return mUsed; }
    LLRmlUiVK* backend() const { return mBackend; }
    // "llext:" texture sources (viewer textures, llrmluiclassic) are resolved by this.
    using ExternalSource = std::function<Rml::TextureHandle(LLRmlUiVK&, Rml::Vector2i&, const std::string&)>;
    void setExternalSource(ExternalSource source) { mExternal = std::move(source); }
    uint32_t refused() const { return mRefused; }

private:
    bool pack(uint32_t w, uint32_t h, uint32_t& x, uint32_t& y);
    // Pack LLImageRaw pixels (bottom-up rows, 1-4 components) premultiplied, edges extruded.
    bool place(const LLImageRaw& raw, Entry& entry);
    static constexpr uint32_t kVectorBudget = kSize * kSize / 4; // atlas pixels for rasters
    static constexpr uint32_t kVectorSizesPerImage = 16;
    uint32_t                  mVectorUsed = 0;
    std::unordered_map<std::string, uint32_t> mVectorSizes; // image -> sizes rasterised

    LLRmlUiVK*         mBackend = nullptr;
    ExternalSource     mExternal;
    Rml::TextureHandle mTexture = 0;
    struct Shelf
    {
        uint32_t y = 0, h = 0, x = 0;
    };
    std::vector<Shelf>                     mShelves;
    uint32_t                               mNextY = 0, mUsed = 0, mRefused = 0;
    std::unordered_map<std::string, Entry> mEntries; // by LLUIImage name; failures cached too
};
