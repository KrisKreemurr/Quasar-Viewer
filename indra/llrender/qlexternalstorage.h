/**
 * @file qlexternalstorage.h
 * @brief Quasar: textures whose storage another API allocates (the Vulkan presenter), where asked.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#ifndef QL_EXTERNAL_STORAGE_H
#define QL_EXTERNAL_STORAGE_H

#include <atomic>

// GL can use memory Vulkan allocates (GL_EXT_memory_object); Vulkan cannot use GL's. A texture that both are to draw
// into or read is therefore given storage from Vulkan's memory: LLImageGL::allocateTexture2D asks the provider below,
// while a QLExternalStorageScope stands on the allocating thread, before it makes storage of its own. The provider
// (llwindow's QLVulkanPresenter, which sets gQLExternalStorage once its formats have passed their self-test) answers
// for the formats it can give; for any other, and with no scope standing, GL allocates as it always has.
class QLExternalStorage
{
public:
    virtual ~QLExternalStorage() = default;

    /// Gives @a texture (a name with no storage yet, bound at @a target on unit 0) storage of @a levels levels of
    /// @a internal_format at @a width x @a height, from memory the provider allocates. False: it gave none, and the
    /// caller allocates as before. On the thread whose GL context is current, which is the main one.
    virtual bool allocate(unsigned texture, unsigned target, int internal_format, int width, int height, int levels) = 0;

    /// A fetched texture's storage (@a levels levels, uploaded into later), placed among others in memory the provider
    /// shares out (QuasarVulkanTextures). @a counted_bytes is what the texture budget counts it as (GL's size); the
    /// provider declines a texture whose storage would cost it much more than that. False: GL allocates as before. On
    /// the main thread or the LLImageGL thread, with that thread's context current.
    virtual bool allocateTexture(unsigned texture, int internal_format, int width, int height, int levels,
                                 unsigned long long counted_bytes) = 0;

    /// GL has deleted these names: the provider lets go of what it gave them. Any thread; the provider frees on its own.
    virtual void deleted(int count, const unsigned* textures) = 0;

    /// Whether @a texture's storage is the provider's.
    virtual bool owns(unsigned texture) const = 0;
};

extern QLExternalStorage* gQLExternalStorage;

// Whether fetched textures are offered to the provider too (LLImageGL::allocateTextureStorage, on the main thread).
extern bool gQLExternalTexturesWanted;

// Fetched textures given storage while gQLExternalTexturesWanted, on the main thread and off it (the LLImageGL
// thread's uploads). For the status line.
extern std::atomic<unsigned> gQLExternalTexturesOffered;
extern std::atomic<unsigned> gQLExternalTexturesOffThread;

// What the provider's texture memory holds past what the texture budget counts for the textures in it (alignment,
// blocks' free space, RGB stored as RGBA), in bytes: the budget adds it (ALVRAMUsage), so that it sees the video memory
// textures really take. Set by the provider.
extern std::atomic<unsigned long long> gQLExternalTextureOverheadBytes;

// While one stands on a thread, the render targets and textures that thread allocates are offered to the provider.
class QLExternalStorageScope
{
public:
    explicit QLExternalStorageScope(bool wanted);
    ~QLExternalStorageScope();

    static bool active();

private:
    bool mPrevious;
};

#endif // QL_EXTERNAL_STORAGE_H
