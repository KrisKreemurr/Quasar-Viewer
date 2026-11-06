/**
 * @file qlexternalstorage.h
 * @brief Quasar: textures whose storage another API allocates (the Vulkan presenter), where asked.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#ifndef QL_EXTERNAL_STORAGE_H
#define QL_EXTERNAL_STORAGE_H

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

    /// GL has deleted these names: the provider lets go of what it gave them. Any thread; the provider frees on its own.
    virtual void deleted(int count, const unsigned* textures) = 0;

    /// Whether @a texture's storage is the provider's.
    virtual bool owns(unsigned texture) const = 0;
};

extern QLExternalStorage* gQLExternalStorage;

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
