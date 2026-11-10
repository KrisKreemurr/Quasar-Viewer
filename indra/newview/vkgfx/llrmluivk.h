// RmlUi 6.3 rendering only. No native-window or inherited UI ownership.
#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <memory>

struct VkUISurfaceTarget;
class VkDeviceContext;
class VkUITextureHost;

class LLRmlUiVK final : public Rml::RenderInterface
{
public:
    // Construct lazily, only when a document becomes visible. One instance and
    // RmlUi context per target; callers own Rml::Initialise/Shutdown.
    // The texture host must outlive the renderer (the world renderer in the viewer).
    static std::unique_ptr<LLRmlUiVK> create(VkDeviceContext&, VkUITextureHost&) noexcept;
    ~LLRmlUiVK() override;
    // begin/end capture a retained display list. Call before Context::Update,
    // then Context::Render, then end. Unchanged documents replay without capture.
    bool begin() noexcept;
    bool end() noexcept;
    // Records segment target.layerSegment of the retained list: 0 does the frame's uploads and
    // draws up to the first segment marker, later segments (same frameSerial) draw theirs.
    bool record(const VkUISurfaceTarget&) noexcept;
    // While capturing: the actions after this call form the next segment (a z slot the viewer
    // interleaves its own UI before). At most 8 segments; extra markers are ignored.
    void     beginSegment() noexcept;
    uint32_t segmentCount() const noexcept;
    // Hide: drop the display list, then collect until all submitted uses retire.
    // Destruction requires caller-confirmed completion of lastSerial(). No wait,
    // submit or presentation is performed by this renderer.
    void     hide() noexcept;
    void     collect(uint64_t completedSerial) noexcept;
    uint64_t lastSerial() const noexcept;
    struct MemoryState
    {
        uint64_t buffers = 0, images = 0, imageBudget = 0, lastUse = 0, completed = 0;
        uint32_t softFailures = 0;
    };
    MemoryState memoryState() const noexcept;
    bool        allocationFailed() const noexcept;
    // Source line in llrmluivk.cpp of the operation that failed the last capture (0: none).
    int         failureLine() const noexcept;
    // Poisoned RmlUi can still reference this interface. Retire its GPU state
    // first, then retain the tiny interface shell without invoking its destructor.
    bool releaseGPUState(uint64_t completedSerial) noexcept;

    // "llui:<name>" texture sources are supplied by the viewer (the skin image atlas). The
    // source creates them with createAtlas and fills them with updateTexture (premultiplied
    // RGBA). Atlas textures keep a full-size host-visible mirror and upload only new regions.
    using NamedTextureSource = std::function<Rml::TextureHandle(LLRmlUiVK&, Rml::Vector2i& size, const Rml::String& source)>;
    void               setNamedTextureSource(NamedTextureSource source) noexcept;
    Rml::TextureHandle createAtlas(uint32_t width, uint32_t height) noexcept;
    // A viewer-owned sampled image (SHADER_READ_ONLY_OPTIMAL) as an RmlUi texture, never
    // destroyed here. The owner must keep the image alive until lastSerial() completes after
    // RmlUi released the handle (llrmluiclassic validates it every frame).
    Rml::TextureHandle externalTexture(VkImage image, VkImageView view, uint32_t width, uint32_t height) noexcept;
    bool updateTexture(Rml::TextureHandle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t strideBytes) noexcept;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override;
    void                        RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override;
    void                        ReleaseGeometry(Rml::CompiledGeometryHandle) override;
    Rml::TextureHandle          LoadTexture(Rml::Vector2i&, const Rml::String&) override;
    Rml::TextureHandle          GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override;
    void                        ReleaseTexture(Rml::TextureHandle) override;
    void                        EnableScissorRegion(bool) override;
    void                        SetScissorRegion(Rml::Rectanglei) override;
    void                        SetTransform(const Rml::Matrix4f*) override;
    void                        EnableClipMask(bool) override;
    void                        RenderToClipMask(Rml::ClipMaskOperation, Rml::CompiledGeometryHandle, Rml::Vector2f) override;
    Rml::LayerHandle            PushLayer() override;
    void                        PopLayer() override;
    void CompositeLayers(Rml::LayerHandle, Rml::LayerHandle, Rml::BlendMode, Rml::Span<const Rml::CompiledFilterHandle>) override;
    Rml::TextureHandle        SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
    Rml::CompiledFilterHandle CompileFilter(const Rml::String&, const Rml::Dictionary&) override;
    void                      ReleaseFilter(Rml::CompiledFilterHandle) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String&, const Rml::Dictionary&) override;
    void RenderShader(Rml::CompiledShaderHandle, Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override;
    void ReleaseShader(Rml::CompiledShaderHandle) override;

private:
    struct State;
    explicit LLRmlUiVK(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> m;
};
