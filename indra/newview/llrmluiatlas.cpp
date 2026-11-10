// #703 UI on RmlUi: the skin image atlas and the "llimage" decorator (see llrmluiatlas.h).
#include "llviewerprecompiledheaders.h"
#include "llrmluiatlas.h"

#include "llimage.h"
#include "llui.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "vkgfx/llrmluivk.h"
#include "llrmluiaccess.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/CompiledFilterShader.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderManager.h>
#include <algorithm>
#include <new>

namespace
{
constexpr uint32_t kPad = 1; // extruded edge around every image (linear filtering)

// LLUIImage::draw as geometry: scale-9 when the image has a scale region, else stretched.
class LLImageDecorator final : public Rml::Decorator
{
public:
    std::string name;
    bool        solid = false;

    bool init(Rml::Texture atlas, std::string image_name, bool solid_fill)
    {
        name  = std::move(image_name);
        solid = solid_fill;
        return AddTexture(atlas) >= 0;
    }
    Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override
    {
        const LLRmlUiAtlas::Entry* e = LLRmlUiAtlas::instance().find(name);
        if (!e)
            return Rml::Decorator::INVALID_DECORATORDATAHANDLE;
        const Rml::ComputedValues& computed = element->GetComputedValues();
        const Rml::RenderBox       box      = element->GetRenderBox(paint_area);
        const Rml::Vector2f        offset   = box.GetFillOffset();
        const Rml::Vector2f        size     = box.GetFillSize();
        const float                dp       = element->GetContext() ? element->GetContext()->GetDensityIndependentPixelRatio() : 1.f;
        const Rml::ColourbPremultiplied colour = computed.image_color().ToPremultiplied(computed.opacity());
        // Vector icons on: the raster at this exact pixel size, stretched over the box like
        // LLUIImage's vector draw (no scale-9).
        if (!LLUIImage::getClassicIcons())
            if (LLUIImagePtr image = LLUI::getUIImage(name); image.notNull() && image->hasVectorSource())
                if (const LLRmlUiAtlas::Entry* ve =
                        LLRmlUiAtlas::instance().findVector(image.get(), uint32_t(ll_round(size.x)), uint32_t(ll_round(size.y))))
                    e = ve;

        // Border widths in image pixels (= LL UI units), y up in the scale region.
        const float iw = e->w, ih = e->h;
        float bl = e->scale.mLeft * iw, br = (1.f - e->scale.mRight) * iw;
        float bt = (1.f - e->scale.mTop) * ih, bb = e->scale.mBottom * ih;
        bl *= dp; br *= dp; bt *= dp; bb *= dp;
        // gl_draw_scaled_image_with_border: borders shrink to fit a smaller box.
        float shrink = 1.f;
        if (bl + br > size.x && bl + br > 0.f) shrink = std::min(shrink, size.x / (bl + br));
        if (bt + bb > size.y && bt + bb > 0.f) shrink = std::min(shrink, size.y / (bt + bb));
        bl *= shrink; br *= shrink; bt *= shrink; bb *= shrink;

        const float inv = 1.f / float(LLRmlUiAtlas::kSize);
        const float u[4] = { e->x * inv, (e->x + e->scale.mLeft * iw) * inv, (e->x + e->scale.mRight * iw) * inv, (e->x + iw) * inv };
        const float v[4] = { e->y * inv, (e->y + (1.f - e->scale.mTop) * ih) * inv, (e->y + (1.f - e->scale.mBottom) * ih) * inv,
                             (e->y + ih) * inv };
        const float x[4] = { offset.x, offset.x + bl, offset.x + size.x - br, offset.x + size.x };
        const float y[4] = { offset.y, offset.y + bt, offset.y + size.y - bb, offset.y + size.y };

        Rml::Mesh mesh;
        mesh.vertices.reserve(16);
        mesh.indices.reserve(54);
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                Rml::Vertex vert;
                vert.position  = { x[i], y[j] };
                vert.colour    = colour;
                vert.tex_coord = { u[i], v[j] };
                mesh.vertices.push_back(vert);
            }
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
            {
                if (x[i + 1] <= x[i] || y[j + 1] <= y[j])
                    continue;
                const int a = j * 4 + i;
                for (int k : { a, a + 4, a + 5, a, a + 5, a + 1 })
                    mesh.indices.push_back(k);
            }
        if (mesh.indices.empty())
            return Rml::Decorator::INVALID_DECORATORDATAHANDLE;
        auto* data = new (std::nothrow) Data{ element->GetRenderManager()->MakeGeometry(std::move(mesh)), {} };
        if (!data)
            return Rml::Decorator::INVALID_DECORATORDATAHANDLE;
        if (solid) // LLUIImage::drawSolid: the colour through the image's alpha (one shader slot each)
            data->shader = element->GetRenderManager()->CompileShader("llimage-solid", {});
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);
    }
    void ReleaseElementData(Rml::DecoratorDataHandle data) const override { delete reinterpret_cast<Data*>(data); }
    void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle handle) const override
    {
        auto* data = reinterpret_cast<Data*>(handle);
        data->geometry.Render(element->GetAbsoluteOffset(Rml::BoxArea::Border), GetTexture(), data->shader);
    }
    struct Data
    {
        Rml::Geometry       geometry;
        Rml::CompiledShader shader;
    };
};

// #703 z-slot segment marker: rendering it starts the backend's next display-list segment
// (llrmluiclassic places one just below the first view of each run after the first).
class LLSegmentDecorator final : public Rml::Decorator
{
public:
    Rml::DecoratorDataHandle GenerateElementData(Rml::Element*, Rml::BoxArea) const override { return 1; }
    void                     ReleaseElementData(Rml::DecoratorDataHandle) const override {}
    void                     RenderElement(Rml::Element*, Rml::DecoratorDataHandle) const override
    {
        if (LLRmlUiVK* backend = LLRmlUiAtlas::instance().backend())
            backend->beginSegment();
    }
};
class LLSegmentDecoratorInstancer final : public Rml::DecoratorInstancer
{
public:
    LLSegmentDecoratorInstancer()
    {
        RegisterProperty("segment", "0").AddParser("number");
        RegisterShorthand("decorator", "segment", Rml::ShorthandType::FallThrough);
    }
    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary&,
                                                     const Rml::DecoratorInstancerInterface&) override
    {
        return Rml::MakeShared<LLSegmentDecorator>();
    }
};

class LLImageDecoratorInstancer final : public Rml::DecoratorInstancer
{
    Rml::PropertyId mImage, mMode;

public:
    LLImageDecoratorInstancer()
    {
        mImage = RegisterProperty("image", "").AddParser("string").GetId();
        mMode  = RegisterProperty("mode", "scale").AddParser("keyword", "scale, solid").GetId();
        RegisterShorthand("decorator", "image, mode", Rml::ShorthandType::FallThrough);
    }
    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary& properties,
                                                     const Rml::DecoratorInstancerInterface& itf) override
    {
        const Rml::Property* image = properties.GetProperty(mImage);
        const Rml::Property* mode  = properties.GetProperty(mMode);
        if (!image)
            return nullptr;
        std::string name = image->Get<Rml::String>();
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"') && name.back() == name.front())
            name = name.substr(1, name.size() - 2); // quoted (names may contain '.')
        if (name.empty() || !LLRmlUiAtlas::instance().find(name))
            return nullptr;
        auto decorator = Rml::MakeShared<LLImageDecorator>();
        if (!decorator->init(itf.GetTexture("llui:atlas"), name, mode && mode->Get<int>() == 1))
            return nullptr;
        return decorator;
    }
};
} // namespace

LLRmlUiAtlas& LLRmlUiAtlas::instance()
{
    static LLRmlUiAtlas atlas;
    return atlas;
}

void LLRmlUiAtlas::attach(LLRmlUiVK& backend)
{
    reset();
    mBackend = &backend;
    // Created now (not when RmlUi first samples it), so images can be packed while styles
    // are computed. Refused: every lookup fails and the LLView renderer stays in charge.
    mTexture = backend.createAtlas(kSize, kSize);
    if (!mTexture)
        LL_WARNS("RmlUi") << "Skin image atlas refused; classic RmlUi presentation disabled" << LL_ENDL;
    backend.setNamedTextureSource(
        [this](LLRmlUiVK& vk, Rml::Vector2i& size, const Rml::String& source) -> Rml::TextureHandle
        {
            if (source.compare(0, 6, "llext:") == 0)
                return mExternal ? mExternal(vk, size, source) : 0;
            if (source != "llui:atlas" || !mTexture)
                return 0;
            size = { int(kSize), int(kSize) };
            return mTexture;
        });
}

void LLRmlUiAtlas::registerDecorator()
{
    static LLImageDecoratorInstancer* instancer = new LLImageDecoratorInstancer(); // process lifetime (RmlUi keeps a raw pointer)
    Rml::Factory::RegisterDecoratorInstancer("llimage", instancer);
    static LLSegmentDecoratorInstancer* segments = new LLSegmentDecoratorInstancer(); // process lifetime
    Rml::Factory::RegisterDecoratorInstancer("llsegment", segments);
}

void LLRmlUiAtlas::reset()
{
    mBackend = nullptr;
    mTexture = 0;
    mShelves.clear();
    mEntries.clear();
    mVectorSizes.clear();
    mVectorUsed = 0;
    mNextY = mUsed = mRefused = 0;
}

bool LLRmlUiAtlas::pack(uint32_t w, uint32_t h, uint32_t& x, uint32_t& y)
{
    // Shelf packer: first shelf of a similar height with room, else a new shelf.
    for (Shelf& s : mShelves)
        if (h <= s.h && h * 4 >= s.h * 3 && s.x + w <= kSize)
        {
            x = s.x;
            y = s.y;
            s.x += w;
            return true;
        }
    if (mNextY + h > kSize || w > kSize)
        return false;
    mShelves.push_back({ mNextY, h, w });
    x = 0;
    y = mNextY;
    mNextY += h;
    return true;
}

bool LLRmlUiAtlas::place(const LLImageRaw& raw, Entry& entry)
{
    const uint32_t w = raw.getWidth(), h = raw.getHeight(), c = raw.getComponents();
    uint32_t       ax = 0, ay = 0;
    if (!w || !h || w > kMaxImage || h > kMaxImage || c < 1 || c > 4 || !pack(w + 2 * kPad, h + 2 * kPad, ax, ay))
        return false;
    // Premultiplied RGBA, top-down rows (LLImageRaw is bottom-up), with the edge texels
    // extruded into the padding.
    const uint32_t       pw = w + 2 * kPad, ph = h + 2 * kPad;
    std::vector<uint8_t> px(size_t(pw) * ph * 4);
    const U8*            src = raw.getData();
    for (uint32_t py = 0; py < ph; ++py)
        for (uint32_t pxi = 0; pxi < pw; ++pxi)
        {
            const uint32_t sx = uint32_t(std::clamp<int>(int(pxi) - int(kPad), 0, int(w) - 1));
            const uint32_t sy = uint32_t(std::clamp<int>(int(py) - int(kPad), 0, int(h) - 1));
            const U8*      s  = src + (size_t(h - 1 - sy) * w + sx) * c;
            U8             r, g, b, a;
            if (c >= 3) { r = s[0]; g = s[1]; b = s[2]; a = c == 4 ? s[3] : 255; }
            else        { r = g = b = s[0]; a = c == 2 ? s[1] : 255; }
            U8* d = &px[(size_t(py) * pw + pxi) * 4];
            d[0]  = U8((r * a + 127) / 255);
            d[1]  = U8((g * a + 127) / 255);
            d[2]  = U8((b * a + 127) / 255);
            d[3]  = a;
        }
    if (!mBackend->updateTexture(mTexture, ax, ay, pw, ph, px.data(), pw * 4))
        return false;
    entry.x  = uint16_t(ax + kPad);
    entry.y  = uint16_t(ay + kPad);
    entry.w  = uint16_t(w);
    entry.h  = uint16_t(h);
    entry.ok = true;
    mUsed += pw * ph;
    return true;
}

const LLRmlUiAtlas::Entry* LLRmlUiAtlas::findVector(const LLUIImage* image, uint32_t pixelW, uint32_t pixelH)
{
    const LLUIImage::VectorSpec* spec = image ? image->getVectorSpec() : nullptr;
    if (!spec || !mBackend || !mTexture || !pixelW || !pixelH || pixelW > kMaxImage || pixelH > kMaxImage)
        return nullptr;
    try
    {
        const std::string key = image->getName() + "@" + std::to_string(pixelW) + "x" + std::to_string(pixelH);
        if (auto it = mEntries.find(key); it != mEntries.end())
            return it->second.ok ? &it->second : nullptr;
        // Bounded: a few sizes per icon and a quarter of the atlas for all rasters.
        uint32_t& sizes = mVectorSizes[image->getName()];
        Entry     entry;
        if (sizes < kVectorSizesPerImage && mVectorUsed + (pixelW + 2) * (pixelH + 2) <= kVectorBudget)
        {
            LLPointer<LLImageRaw> raw = LLUIImageList::getInstance()->rasterizeVectorRaw(*spec, S32(pixelW), S32(pixelH));
            if (raw.notNull() && place(*raw, entry))
            {
                ++sizes;
                mVectorUsed += (pixelW + 2) * (pixelH + 2);
            }
        }
        auto res = mEntries.emplace(key, entry);
        return res.first->second.ok ? &res.first->second : nullptr;
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

const LLRmlUiAtlas::Entry* LLRmlUiAtlas::find(const std::string& name)
{
    auto it = mEntries.find(name);
    if (it != mEntries.end())
        return it->second.ok ? &it->second : nullptr;
    LLUIImagePtr image = LLUI::getUIImage(name);
    return image.notNull() && image->getName() == name ? find(image.get()) : nullptr;
}

const LLRmlUiAtlas::Entry* LLRmlUiAtlas::find(const LLUIImage* image)
{
    if (!image)
        return nullptr;
    const std::string& name = image->getName();
    auto               it   = mEntries.find(name);
    if (it != mEntries.end())
        return it->second.ok ? &it->second : nullptr;
    if (!mBackend || !mTexture)
        return nullptr; // the atlas texture is created when the first document loads
    Entry entry;
    try
    {
        auto* fetched = dynamic_cast<const LLViewerFetchedTexture*>(image->getImage().get());
        std::string path = fetched ? fetched->getUrl() : std::string();
        if (path.compare(0, 7, "file://") == 0)
            path.erase(0, 7);
        LLPointer<LLImageFormatted> file = path.empty() ? nullptr : LLImageFormatted::createFromExtension(path);
        LLPointer<LLImageRaw>       raw  = new LLImageRaw();
        if (file.notNull() && file->load(path) && file->decode(raw, 0.f) && place(*raw, entry))
            entry.scale = LLRmlUiClassicAccess::scaleRegion(image);
    }
    catch (const std::bad_alloc&)
    {
        entry.ok = false; // refused, cached below; the caller falls back
    }
    if (!entry.ok)
        ++mRefused;
    try
    {
        auto res = mEntries.emplace(name, entry);
        return res.first->second.ok ? &res.first->second : nullptr;
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}
