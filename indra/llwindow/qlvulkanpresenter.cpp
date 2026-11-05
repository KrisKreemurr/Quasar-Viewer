/**
 * @file qlvulkanpresenter.cpp
 * @brief Quasar Viewer: presents the frames GL draws through a Vulkan swapchain (QuasarVulkanPresent).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code (based on Alchemy Viewer)
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
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "qlvulkanpresenter.h"

#include "llwindowwin32.h"

#include "llglheaders.h"
#include "llstring.h"

#define VK_NO_PROTOTYPES
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <vector>

// GL_EXT_memory_object, GL_EXT_memory_object_win32, GL_EXT_semaphore, GL_EXT_semaphore_win32: the values from the
// Khronos registry, where the GL headers in use do not carry them.
#ifndef GL_DEDICATED_MEMORY_OBJECT_EXT
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#endif
#ifndef GL_TEXTURE_TILING_EXT
#define GL_TEXTURE_TILING_EXT 0x9580
#endif
#ifndef GL_OPTIMAL_TILING_EXT
#define GL_OPTIMAL_TILING_EXT 0x9584
#endif
#ifndef GL_NUM_DEVICE_UUIDS_EXT
#define GL_NUM_DEVICE_UUIDS_EXT 0x9596
#endif
#ifndef GL_DEVICE_UUID_EXT
#define GL_DEVICE_UUID_EXT 0x9597
#endif
#ifndef GL_UUID_SIZE_EXT
#define GL_UUID_SIZE_EXT 16
#endif
#ifndef GL_HANDLE_TYPE_OPAQUE_WIN32_EXT
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#endif
#ifndef GL_LAYOUT_TRANSFER_SRC_EXT
#define GL_LAYOUT_TRANSFER_SRC_EXT 0x9592
#endif
#ifndef GL_LAYOUT_GENERAL_EXT
#define GL_LAYOUT_GENERAL_EXT 0x958D
#endif

namespace
{
    // The GL entry points this file calls, loaded here so it depends on nothing llgl loads or leaves out.
    typedef void (APIENTRY* QL_PFN_GetIntegerv)(GLenum, GLint*);
    typedef GLboolean (APIENTRY* QL_PFN_IsEnabled)(GLenum);
    typedef void (APIENTRY* QL_PFN_EnableDisable)(GLenum);
    typedef void (APIENTRY* QL_PFN_Void)(void);
    typedef GLenum (APIENTRY* QL_PFN_GetError)(void);
    typedef const GLubyte* (APIENTRY* QL_PFN_GetStringi)(GLenum, GLuint);
    typedef void (APIENTRY* QL_PFN_CreateObjects)(GLsizei, GLuint*);
    typedef void (APIENTRY* QL_PFN_DeleteObjects)(GLsizei, const GLuint*);
    typedef void (APIENTRY* QL_PFN_CreateTextures)(GLenum, GLsizei, GLuint*);
    typedef void (APIENTRY* QL_PFN_TextureParameteri)(GLuint, GLenum, GLint);
    typedef void (APIENTRY* QL_PFN_NamedFramebufferTexture)(GLuint, GLenum, GLuint, GLint);
    typedef GLenum (APIENTRY* QL_PFN_CheckNamedFramebufferStatus)(GLuint, GLenum);
    typedef void (APIENTRY* QL_PFN_NamedFramebufferReadBuffer)(GLuint, GLenum);
    typedef void (APIENTRY* QL_PFN_ClearNamedFramebufferfv)(GLuint, GLenum, GLint, const GLfloat*);
    typedef void (APIENTRY* QL_PFN_GetTextureSubImage)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                                       GLenum, GLenum, GLsizei, void*);
    typedef void (APIENTRY* QL_PFN_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    typedef void (APIENTRY* QL_PFN_GetBooleanv)(GLenum, GLboolean*);
    typedef void (APIENTRY* QL_PFN_BlitNamedFramebuffer)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                                         GLint, GLbitfield, GLenum);
    typedef void (APIENTRY* QL_PFN_MemoryObjectParameteriv)(GLuint, GLenum, const GLint*);
    typedef void (APIENTRY* QL_PFN_ImportMemoryWin32Handle)(GLuint, GLuint64, GLenum, void*);
    typedef void (APIENTRY* QL_PFN_TextureStorageMem2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
    typedef void (APIENTRY* QL_PFN_ImportSemaphoreWin32Handle)(GLuint, GLenum, void*);
    typedef void (APIENTRY* QL_PFN_SemaphoreOp)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    typedef void (APIENTRY* QL_PFN_GetUnsignedBytei_v)(GLenum, GLuint, GLubyte*);

    struct QLGLInterop
    {
        QL_PFN_GetIntegerv                  GetIntegerv = nullptr;
        QL_PFN_IsEnabled                    IsEnabled = nullptr;
        QL_PFN_EnableDisable                Enable = nullptr;
        QL_PFN_EnableDisable                Disable = nullptr;
        QL_PFN_Void                         Flush = nullptr;
        QL_PFN_Void                         Finish = nullptr;
        QL_PFN_GetError                     GetError = nullptr;
        QL_PFN_GetStringi                   GetStringi = nullptr;
        QL_PFN_CreateTextures               CreateTextures = nullptr;
        QL_PFN_DeleteObjects                DeleteTextures = nullptr;
        QL_PFN_TextureParameteri            TextureParameteri = nullptr;
        QL_PFN_CreateObjects                CreateFramebuffers = nullptr;
        QL_PFN_DeleteObjects                DeleteFramebuffers = nullptr;
        QL_PFN_NamedFramebufferTexture      NamedFramebufferTexture = nullptr;
        QL_PFN_CheckNamedFramebufferStatus  CheckNamedFramebufferStatus = nullptr;
        QL_PFN_NamedFramebufferReadBuffer   NamedFramebufferReadBuffer = nullptr;
        QL_PFN_ClearNamedFramebufferfv      ClearNamedFramebufferfv = nullptr;
        QL_PFN_GetTextureSubImage           GetTextureSubImage = nullptr;
        QL_PFN_ColorMask                    ColorMask = nullptr;
        QL_PFN_GetBooleanv                  GetBooleanv = nullptr;
        QL_PFN_BlitNamedFramebuffer         BlitNamedFramebuffer = nullptr;
        QL_PFN_CreateObjects                CreateMemoryObjectsEXT = nullptr;
        QL_PFN_DeleteObjects                DeleteMemoryObjectsEXT = nullptr;
        QL_PFN_MemoryObjectParameteriv      MemoryObjectParameterivEXT = nullptr;
        QL_PFN_ImportMemoryWin32Handle      ImportMemoryWin32HandleEXT = nullptr;
        QL_PFN_TextureStorageMem2D          TextureStorageMem2DEXT = nullptr;
        QL_PFN_CreateObjects                GenSemaphoresEXT = nullptr;
        QL_PFN_DeleteObjects                DeleteSemaphoresEXT = nullptr;
        QL_PFN_ImportSemaphoreWin32Handle   ImportSemaphoreWin32HandleEXT = nullptr;
        QL_PFN_SemaphoreOp                  SignalSemaphoreEXT = nullptr;
        QL_PFN_SemaphoreOp                  WaitSemaphoreEXT = nullptr;
        QL_PFN_GetUnsignedBytei_v           GetUnsignedBytei_vEXT = nullptr;

        bool load()
        {
#define QL_GL_LOAD(member, name) member = reinterpret_cast<decltype(member)>(LLWindowWin32::getProcAddress(name)); \
            if (!member) { mMissing = name; return false; }
            QL_GL_LOAD(GetIntegerv, "glGetIntegerv");
            QL_GL_LOAD(IsEnabled, "glIsEnabled");
            QL_GL_LOAD(Enable, "glEnable");
            QL_GL_LOAD(Disable, "glDisable");
            QL_GL_LOAD(Flush, "glFlush");
            QL_GL_LOAD(Finish, "glFinish");
            QL_GL_LOAD(GetError, "glGetError");
            QL_GL_LOAD(GetStringi, "glGetStringi");
            QL_GL_LOAD(CreateTextures, "glCreateTextures");
            QL_GL_LOAD(DeleteTextures, "glDeleteTextures");
            QL_GL_LOAD(TextureParameteri, "glTextureParameteri");
            QL_GL_LOAD(CreateFramebuffers, "glCreateFramebuffers");
            QL_GL_LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
            QL_GL_LOAD(NamedFramebufferTexture, "glNamedFramebufferTexture");
            QL_GL_LOAD(CheckNamedFramebufferStatus, "glCheckNamedFramebufferStatus");
            QL_GL_LOAD(NamedFramebufferReadBuffer, "glNamedFramebufferReadBuffer");
            QL_GL_LOAD(ClearNamedFramebufferfv, "glClearNamedFramebufferfv");
            QL_GL_LOAD(GetTextureSubImage, "glGetTextureSubImage");
            QL_GL_LOAD(ColorMask, "glColorMask");
            QL_GL_LOAD(GetBooleanv, "glGetBooleanv");
            QL_GL_LOAD(BlitNamedFramebuffer, "glBlitNamedFramebuffer");
            QL_GL_LOAD(CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT");
            QL_GL_LOAD(DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT");
            QL_GL_LOAD(MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT");
            QL_GL_LOAD(ImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT");
            QL_GL_LOAD(TextureStorageMem2DEXT, "glTextureStorageMem2DEXT");
            QL_GL_LOAD(GenSemaphoresEXT, "glGenSemaphoresEXT");
            QL_GL_LOAD(DeleteSemaphoresEXT, "glDeleteSemaphoresEXT");
            QL_GL_LOAD(ImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT");
            QL_GL_LOAD(SignalSemaphoreEXT, "glSignalSemaphoreEXT");
            QL_GL_LOAD(WaitSemaphoreEXT, "glWaitSemaphoreEXT");
            QL_GL_LOAD(GetUnsignedBytei_vEXT, "glGetUnsignedBytei_vEXT");
#undef QL_GL_LOAD
            return true;
        }

        bool hasExtension(const char* name) const
        {
            GLint count = 0;
            GetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; ++i)
            {
                const char* ext = reinterpret_cast<const char*>(GetStringi(GL_EXTENSIONS, (GLuint)i));
                if (ext && strcmp(ext, name) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        const char* mMissing = "";
    };

    constexpr U32 FRAMES_IN_FLIGHT = 2;

    const char* present_mode_name(VkPresentModeKHR mode)
    {
        switch (mode)
        {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR:   return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:      return "fifo";
        default:                            return "other";
        }
    }
}

namespace
{
    // The validation layer's findings, into the log.
    VKAPI_ATTR VkBool32 VKAPI_CALL ql_vk_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
        const char* text = data && data->pMessage ? data->pMessage : "";
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            LL_WARNS("Vulkan") << "Validation error: " << text << LL_ENDL;
        }
        else
        {
            LL_WARNS("Vulkan") << "Validation warning: " << text << LL_ENDL;
        }
        return VK_FALSE;
    }
}

struct QLVulkanPresenter::Impl
{
    // Vulkan, loaded from vulkan-1.dll.
    HMODULE mLibrary = nullptr;
#define QL_VK_FN(name) PFN_##name name = nullptr
    QL_VK_FN(vkGetInstanceProcAddr);
    QL_VK_FN(vkCreateInstance);
    QL_VK_FN(vkEnumerateInstanceLayerProperties);
    QL_VK_FN(vkDestroyInstance);
    QL_VK_FN(vkEnumeratePhysicalDevices);
    QL_VK_FN(vkGetPhysicalDeviceProperties);
    QL_VK_FN(vkGetPhysicalDeviceProperties2);
    QL_VK_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    QL_VK_FN(vkGetPhysicalDeviceMemoryProperties);
    QL_VK_FN(vkGetPhysicalDeviceFormatProperties);
    QL_VK_FN(vkEnumerateDeviceExtensionProperties);
    QL_VK_FN(vkCreateDevice);
    QL_VK_FN(vkGetDeviceProcAddr);
    QL_VK_FN(vkCreateWin32SurfaceKHR);
    QL_VK_FN(vkDestroySurfaceKHR);
    QL_VK_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    QL_VK_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    QL_VK_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    QL_VK_FN(vkGetPhysicalDeviceSurfacePresentModesKHR);
    QL_VK_FN(vkDestroyDevice);
    QL_VK_FN(vkGetDeviceQueue);
    QL_VK_FN(vkDeviceWaitIdle);
    QL_VK_FN(vkCreateSwapchainKHR);
    QL_VK_FN(vkDestroySwapchainKHR);
    QL_VK_FN(vkGetSwapchainImagesKHR);
    QL_VK_FN(vkAcquireNextImageKHR);
    QL_VK_FN(vkQueuePresentKHR);
    QL_VK_FN(vkCreateImage);
    QL_VK_FN(vkDestroyImage);
    QL_VK_FN(vkGetImageMemoryRequirements);
    QL_VK_FN(vkAllocateMemory);
    QL_VK_FN(vkFreeMemory);
    QL_VK_FN(vkBindImageMemory);
    QL_VK_FN(vkGetMemoryWin32HandleKHR);
    QL_VK_FN(vkCreateSemaphore);
    QL_VK_FN(vkDestroySemaphore);
    QL_VK_FN(vkGetSemaphoreWin32HandleKHR);
    QL_VK_FN(vkCreateFence);
    QL_VK_FN(vkDestroyFence);
    QL_VK_FN(vkWaitForFences);
    QL_VK_FN(vkResetFences);
    QL_VK_FN(vkCreateCommandPool);
    QL_VK_FN(vkDestroyCommandPool);
    QL_VK_FN(vkAllocateCommandBuffers);
    QL_VK_FN(vkBeginCommandBuffer);
    QL_VK_FN(vkEndCommandBuffer);
    QL_VK_FN(vkResetCommandBuffer);
    QL_VK_FN(vkCmdPipelineBarrier);
    QL_VK_FN(vkCmdBlitImage);
    QL_VK_FN(vkCmdClearColorImage);
    QL_VK_FN(vkCmdCopyImageToBuffer);
    QL_VK_FN(vkCreateBuffer);
    QL_VK_FN(vkDestroyBuffer);
    QL_VK_FN(vkGetBufferMemoryRequirements);
    QL_VK_FN(vkBindBufferMemory);
    QL_VK_FN(vkMapMemory);
    QL_VK_FN(vkUnmapMemory);
    QL_VK_FN(vkCreateDebugUtilsMessengerEXT);
    QL_VK_FN(vkDestroyDebugUtilsMessengerEXT);
    QL_VK_FN(vkQueueSubmit);
#undef QL_VK_FN

    QLGLInterop gl;

    HWND             mWindow = nullptr;
    VkInstance       mInstance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR     mSurface = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkDevice         mDevice = VK_NULL_HANDLE;
    U32              mQueueFamily = 0;
    VkQueue          mQueue = VK_NULL_HANDLE;
    VkCommandPool    mCommandPool = VK_NULL_HANDLE;

    VkSwapchainKHR           mSwapchain = VK_NULL_HANDLE;
    VkFormat                 mSwapFormat = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR         mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     mSwapImages;
    std::vector<VkSemaphore> mRenderDone;  // one a swapchain image: its present waits on it
    U32                      mSwapWidth = 0;
    U32                      mSwapHeight = 0;
    bool                     mSwapchainDirty = false;
    bool                     mVSync = false;

    // The image both APIs share, sized to the window's back buffer.
    VkImage        mShared = VK_NULL_HANDLE;
    VkDeviceMemory mSharedMemory = VK_NULL_HANDLE;
    U32            mSharedWidth = 0;
    U32            mSharedHeight = 0;
    GLuint         mGLMemory = 0;
    GLuint         mGLTexture = 0;
    GLuint         mGLFramebuffer = 0;

    // GL signals mGLDone once the frame is in the shared image; Vulkan signals mVKDone once it has read it.
    VkSemaphore mGLDoneVK = VK_NULL_HANDLE;
    VkSemaphore mVKDoneVK = VK_NULL_HANDLE;
    GLuint      mGLDoneGL = 0;
    GLuint      mVKDoneGL = 0;
    bool        mGLMustWait = false;  // a read Vulkan signalled the end of that GL has not yet waited for

    VkCommandBuffer mCommands[FRAMES_IN_FLIGHT] = {};
    VkFence         mFences[FRAMES_IN_FLIGHT] = {};
    VkSemaphore     mAcquired[FRAMES_IN_FLIGHT] = {};
    U64             mFrame = 0;

    // Diagnosis of a black screen. For its first DIAGNOSE_FRAMES presents Vulkan fills the window with magenta rather
    // than the frame: seen, Vulkan reaches the screen. For the next DIAGNOSE_FRAMES GL fills the shared image with green
    // rather than copying its back buffer into it, and Vulkan presents that: seen, the sharing works and what comes back
    // empty is GL's back buffer. GL's errors around the sharing go to the log too.
    static constexpr U64 DIAGNOSE_FRAMES = 240;
    bool mLoggedFirstCopy = false;

    // GL errors since the last call, logged with what was just done; GL's error queue is left empty.
    void logGLErrors(const char* after)
    {
        if (!gl.GetError)
        {
            return;
        }
        for (S32 i = 0; i < 16; ++i)
        {
            const GLenum err = gl.GetError();
            if (err == GL_NO_ERROR)
            {
                break;
            }
            LL_WARNS("Vulkan") << "GL error 0x" << std::hex << err << std::dec << " after " << after << LL_ENDL;
        }
    }

    bool        mActive = false;
    std::string mNote = "not started";

    bool fail(const std::string& why)
    {
        mNote = why;
        LL_WARNS("Vulkan") << "Vulkan presentation: " << why << LL_ENDL;
        return false;
    }

    bool failVK(const char* what, VkResult result)
    {
        return fail(llformat("%s failed (VkResult %d)", what, (S32)result));
    }

    bool loadInstanceFunctions()
    {
#define QL_VK_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(mInstance, #name)); \
        if (!name) { return fail(std::string("missing ") + #name); }
        QL_VK_INSTANCE(vkDestroyInstance);
        QL_VK_INSTANCE(vkEnumeratePhysicalDevices);
        QL_VK_INSTANCE(vkGetPhysicalDeviceProperties);
        QL_VK_INSTANCE(vkGetPhysicalDeviceProperties2);
        QL_VK_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
        QL_VK_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
        QL_VK_INSTANCE(vkGetPhysicalDeviceFormatProperties);
        QL_VK_INSTANCE(vkEnumerateDeviceExtensionProperties);
        QL_VK_INSTANCE(vkCreateDevice);
        QL_VK_INSTANCE(vkGetDeviceProcAddr);
        QL_VK_INSTANCE(vkCreateWin32SurfaceKHR);
        QL_VK_INSTANCE(vkDestroySurfaceKHR);
        QL_VK_INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);
        QL_VK_INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        QL_VK_INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);
        QL_VK_INSTANCE(vkGetPhysicalDeviceSurfacePresentModesKHR);
#undef QL_VK_INSTANCE
        return true;
    }

    bool loadDeviceFunctions()
    {
#define QL_VK_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(mDevice, #name)); \
        if (!name) { return fail(std::string("missing ") + #name); }
        QL_VK_DEVICE(vkDestroyDevice);
        QL_VK_DEVICE(vkGetDeviceQueue);
        QL_VK_DEVICE(vkDeviceWaitIdle);
        QL_VK_DEVICE(vkCreateSwapchainKHR);
        QL_VK_DEVICE(vkDestroySwapchainKHR);
        QL_VK_DEVICE(vkGetSwapchainImagesKHR);
        QL_VK_DEVICE(vkAcquireNextImageKHR);
        QL_VK_DEVICE(vkQueuePresentKHR);
        QL_VK_DEVICE(vkCreateImage);
        QL_VK_DEVICE(vkDestroyImage);
        QL_VK_DEVICE(vkGetImageMemoryRequirements);
        QL_VK_DEVICE(vkAllocateMemory);
        QL_VK_DEVICE(vkFreeMemory);
        QL_VK_DEVICE(vkBindImageMemory);
        QL_VK_DEVICE(vkGetMemoryWin32HandleKHR);
        QL_VK_DEVICE(vkCreateSemaphore);
        QL_VK_DEVICE(vkDestroySemaphore);
        QL_VK_DEVICE(vkGetSemaphoreWin32HandleKHR);
        QL_VK_DEVICE(vkCreateFence);
        QL_VK_DEVICE(vkDestroyFence);
        QL_VK_DEVICE(vkWaitForFences);
        QL_VK_DEVICE(vkResetFences);
        QL_VK_DEVICE(vkCreateCommandPool);
        QL_VK_DEVICE(vkDestroyCommandPool);
        QL_VK_DEVICE(vkAllocateCommandBuffers);
        QL_VK_DEVICE(vkBeginCommandBuffer);
        QL_VK_DEVICE(vkEndCommandBuffer);
        QL_VK_DEVICE(vkResetCommandBuffer);
        QL_VK_DEVICE(vkCmdPipelineBarrier);
        QL_VK_DEVICE(vkCmdBlitImage);
        QL_VK_DEVICE(vkCmdClearColorImage);
        QL_VK_DEVICE(vkCmdCopyImageToBuffer);
        QL_VK_DEVICE(vkCreateBuffer);
        QL_VK_DEVICE(vkDestroyBuffer);
        QL_VK_DEVICE(vkGetBufferMemoryRequirements);
        QL_VK_DEVICE(vkBindBufferMemory);
        QL_VK_DEVICE(vkMapMemory);
        QL_VK_DEVICE(vkUnmapMemory);
        QL_VK_DEVICE(vkQueueSubmit);
#undef QL_VK_DEVICE
        return true;
    }

    // ---- setting up ---------------------------------------------------------------------------------------------

    bool checkGL()
    {
        if (!gl.load())
        {
            return fail(std::string("GL has no ") + gl.mMissing);
        }
        for (const char* ext : { "GL_EXT_memory_object", "GL_EXT_memory_object_win32", "GL_EXT_semaphore",
                                 "GL_EXT_semaphore_win32" })
        {
            if (!gl.hasExtension(ext))
            {
                return fail(std::string("GL has no ") + ext);
            }
        }
        return true;
    }

    bool createInstance()
    {
        mLibrary = LoadLibraryA("vulkan-1.dll");
        if (!mLibrary)
        {
            return fail("no Vulkan runtime (vulkan-1.dll)");
        }
        vkGetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mLibrary, "vkGetInstanceProcAddr"));
        if (!vkGetInstanceProcAddr)
        {
            return fail("vulkan-1.dll has no vkGetInstanceProcAddr");
        }
        vkCreateInstance =
            reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
        if (!vkCreateInstance)
        {
            return fail("missing vkCreateInstance");
        }

        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName   = "Quasar Viewer";
        app.applicationVersion = 1;
        app.pEngineName        = "Quasar";
        app.engineVersion      = 1;
        app.apiVersion         = VK_API_VERSION_1_1;

        std::vector<const char*> extensions = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };

        // The validation layer, where it is installed and asked for (QUASAR_VK_VALIDATION=1 in the environment).
        std::vector<const char*> layers;
        char env[8] = {};
        if (GetEnvironmentVariableA("QUASAR_VK_VALIDATION", env, sizeof(env)) > 0 && env[0] == '1' &&
            vkEnumerateInstanceLayerProperties)
        {
            U32 count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> props(count);
            vkEnumerateInstanceLayerProperties(&count, props.data());
            for (const VkLayerProperties& p : props)
            {
                if (strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    // The layer brings the messenger its findings come through.
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    LL_INFOS("Vulkan") << "Vulkan validation layer on" << LL_ENDL;
                }
            }
        }

        VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pApplicationInfo        = &app;
        info.enabledExtensionCount   = (U32)extensions.size();
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount       = (U32)layers.size();
        info.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();

        VkResult result = vkCreateInstance(&info, nullptr, &mInstance);
        if (result != VK_SUCCESS)
        {
            mInstance = VK_NULL_HANDLE;
            return failVK("vkCreateInstance", result);
        }
        if (!layers.empty())
        {
            vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT"));
            vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT"));
            if (vkCreateDebugUtilsMessengerEXT)
            {
                VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
                messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                messenger.pfnUserCallback = ql_vk_debug_message;
                vkCreateDebugUtilsMessengerEXT(mInstance, &messenger, nullptr, &mMessenger);
            }
        }
        return loadInstanceFunctions();
    }

    bool createSurface()
    {
        VkWin32SurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        info.hinstance = GetModuleHandle(nullptr);
        info.hwnd      = mWindow;
        VkResult result = vkCreateWin32SurfaceKHR(mInstance, &info, nullptr, &mSurface);
        if (result != VK_SUCCESS)
        {
            mSurface = VK_NULL_HANDLE;
            return failVK("vkCreateWin32SurfaceKHR", result);
        }
        return true;
    }

    // The GPU GL is drawing on: memory and semaphores are shared only within one device.
    bool pickDevice()
    {
        GLint gl_uuids = 0;
        gl.GetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &gl_uuids);
        std::vector<std::vector<GLubyte>> gl_ids;
        for (GLint i = 0; i < gl_uuids; ++i)
        {
            std::vector<GLubyte> id(GL_UUID_SIZE_EXT, 0);
            gl.GetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, (GLuint)i, id.data());
            gl_ids.push_back(id);
        }
        if (gl_ids.empty())
        {
            return fail("GL reports no device UUID");
        }

        U32 count = 0;
        vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(mInstance, &count, devices.data());

        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props.pNext = &ids;
            vkGetPhysicalDeviceProperties2(device, &props);
            if (props.properties.apiVersion < VK_API_VERSION_1_1)
            {
                continue;
            }
            bool same = false;
            for (const auto& id : gl_ids)
            {
                same = same || memcmp(id.data(), ids.deviceUUID, VK_UUID_SIZE) == 0;
            }
            if (!same)
            {
                continue;
            }

            U32 families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
            std::vector<VkQueueFamilyProperties> family_props(families);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &families, family_props.data());
            for (U32 f = 0; f < families; ++f)
            {
                VkBool32 presents = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, f, mSurface, &presents);
                if ((family_props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents)
                {
                    mPhysical    = device;
                    mQueueFamily = f;
                    LL_INFOS("Vulkan") << "Vulkan presentation on " << props.properties.deviceName << LL_ENDL;
                    return true;
                }
            }
        }
        return fail("no Vulkan device that is GL's and can present to the window");
    }

    bool createDevice()
    {
        const char* wanted[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
        U32 count = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &count, props.data());
        for (const char* name : wanted)
        {
            bool found = false;
            for (const VkExtensionProperties& p : props)
            {
                found = found || strcmp(p.extensionName, name) == 0;
            }
            if (!found)
            {
                return fail(std::string("device has no ") + name);
            }
        }

        const F32 priority = 1.f;
        VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue.queueFamilyIndex = mQueueFamily;
        queue.queueCount       = 1;
        queue.pQueuePriorities = &priority;

        VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.queueCreateInfoCount    = 1;
        info.pQueueCreateInfos       = &queue;
        info.enabledExtensionCount   = (U32)LL_ARRAY_SIZE(wanted);
        info.ppEnabledExtensionNames = wanted;

        VkResult result = vkCreateDevice(mPhysical, &info, nullptr, &mDevice);
        if (result != VK_SUCCESS)
        {
            mDevice = VK_NULL_HANDLE;
            return failVK("vkCreateDevice", result);
        }
        if (!loadDeviceFunctions())
        {
            return false;
        }
        vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);

        VkFormatProperties format = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, VK_FORMAT_R8G8B8A8_UNORM, &format);
        if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        {
            return fail("the shared image's format cannot be blitted from");
        }

        VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = mQueueFamily;
        result = vkCreateCommandPool(mDevice, &pool, nullptr, &mCommandPool);
        if (result != VK_SUCCESS)
        {
            mCommandPool = VK_NULL_HANDLE;
            return failVK("vkCreateCommandPool", result);
        }

        VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool        = mCommandPool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = FRAMES_IN_FLIGHT;
        result = vkAllocateCommandBuffers(mDevice, &alloc, mCommands);
        if (result != VK_SUCCESS)
        {
            return failVK("vkAllocateCommandBuffers", result);
        }

        for (U32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(mDevice, &fence, nullptr, &mFences[i]);
            if (result != VK_SUCCESS)
            {
                mFences[i] = VK_NULL_HANDLE;
                return failVK("vkCreateFence", result);
            }
            VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            result = vkCreateSemaphore(mDevice, &sem, nullptr, &mAcquired[i]);
            if (result != VK_SUCCESS)
            {
                mAcquired[i] = VK_NULL_HANDLE;
                return failVK("vkCreateSemaphore", result);
            }
        }
        return true;
    }

    // A semaphore Vulkan makes and GL takes a share of.
    bool createSharedSemaphore(VkSemaphore& vk, GLuint& gl_sem)
    {
        VkExportSemaphoreCreateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        exported.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        info.pNext = &exported;
        VkResult result = vkCreateSemaphore(mDevice, &info, nullptr, &vk);
        if (result != VK_SUCCESS)
        {
            vk = VK_NULL_HANDLE;
            return failVK("vkCreateSemaphore (shared)", result);
        }

        VkSemaphoreGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
        get.semaphore  = vk;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE handle  = nullptr;
        result = vkGetSemaphoreWin32HandleKHR(mDevice, &get, &handle);
        if (result != VK_SUCCESS || !handle)
        {
            return failVK("vkGetSemaphoreWin32HandleKHR", result);
        }

        gl.GenSemaphoresEXT(1, &gl_sem);
        gl.ImportSemaphoreWin32HandleEXT(gl_sem, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        // Importing takes no ownership of the handle: the import holds its own reference.
        CloseHandle(handle);
        return gl_sem != 0;
    }

    // The image GL copies the frame into and Vulkan copies it out of, at the back buffer's size.
    bool createSharedImage(U32 width, U32 height)
    {
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        VkImageCreateInfo image = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.pNext         = &external;
        image.imageType     = VK_IMAGE_TYPE_2D;
        image.format        = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent        = { width, height, 1 };
        image.mipLevels     = 1;
        image.arrayLayers   = 1;
        image.samples       = VK_SAMPLE_COUNT_1_BIT;
        image.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // Declared for every use GL may make of a texture, so the driver lays the memory out as GL's texture expects:
        // an image declared for less can be laid out (compressed) in a way GL's view of the same memory does not read.
        VkFormatProperties shared_props = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, VK_FORMAT_R8G8B8A8_UNORM, &shared_props);
        image.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (shared_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
        {
            image.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        image.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(mDevice, &image, nullptr, &mShared);
        if (result != VK_SUCCESS)
        {
            mShared = VK_NULL_HANDLE;
            return failVK("vkCreateImage (shared)", result);
        }

        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, mShared, &req);
        VkPhysicalDeviceMemoryProperties mem = {};
        vkGetPhysicalDeviceMemoryProperties(mPhysical, &mem);
        U32 type = U32_MAX;
        for (U32 i = 0; i < mem.memoryTypeCount; ++i)
        {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                type = i;
                break;
            }
        }
        if (type == U32_MAX)
        {
            return fail("no device-local memory for the shared image");
        }

        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = mShared;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = type;
        result = vkAllocateMemory(mDevice, &alloc, nullptr, &mSharedMemory);
        if (result != VK_SUCCESS)
        {
            mSharedMemory = VK_NULL_HANDLE;
            return failVK("vkAllocateMemory (shared)", result);
        }
        result = vkBindImageMemory(mDevice, mShared, mSharedMemory, 0);
        if (result != VK_SUCCESS)
        {
            return failVK("vkBindImageMemory", result);
        }

        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = mSharedMemory;
        get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE handle  = nullptr;
        result = vkGetMemoryWin32HandleKHR(mDevice, &get, &handle);
        if (result != VK_SUCCESS || !handle)
        {
            return failVK("vkGetMemoryWin32HandleKHR", result);
        }

        // GL's side: a memory object over Vulkan's allocation, a texture stored in it, and a framebuffer to copy into.
        gl.CreateMemoryObjectsEXT(1, &mGLMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(mGLMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(mGLMemory, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        CloseHandle(handle);

        gl.CreateTextures(GL_TEXTURE_2D, 1, &mGLTexture);
        gl.TextureParameteri(mGLTexture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(mGLTexture, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height, mGLMemory, 0);

        logGLErrors("importing the shared image's memory into a texture");
        gl.CreateFramebuffers(1, &mGLFramebuffer);
        gl.NamedFramebufferTexture(mGLFramebuffer, GL_COLOR_ATTACHMENT0, mGLTexture, 0);
        logGLErrors("making the shared image's framebuffer");
        if (gl.CheckNamedFramebufferStatus(mGLFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            return fail("the shared image is not a complete GL framebuffer");
        }

        mSharedWidth  = width;
        mSharedHeight = height;
        LL_INFOS("Vulkan") << "Shared image " << width << "x" << height << ", " << req.size << " bytes, GL texture "
                           << mGLTexture << ", framebuffer " << mGLFramebuffer << LL_ENDL;
        return true;
    }

    void destroySharedImage()
    {
        if (mGLFramebuffer)
        {
            gl.DeleteFramebuffers(1, &mGLFramebuffer);
            mGLFramebuffer = 0;
        }
        if (mGLTexture)
        {
            gl.DeleteTextures(1, &mGLTexture);
            mGLTexture = 0;
        }
        if (mGLMemory)
        {
            gl.DeleteMemoryObjectsEXT(1, &mGLMemory);
            mGLMemory = 0;
        }
        if (mShared)
        {
            vkDestroyImage(mDevice, mShared, nullptr);
            mShared = VK_NULL_HANDLE;
        }
        if (mSharedMemory)
        {
            vkFreeMemory(mDevice, mSharedMemory, nullptr);
            mSharedMemory = VK_NULL_HANDLE;
        }
        mSharedWidth = mSharedHeight = 0;
    }

    VkPresentModeKHR choosePresentMode()
    {
        U32 count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysical, mSurface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysical, mSurface, &count, modes.data());
        auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
        if (!mVSync)
        {
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            {
                return VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            {
                return VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;  // every device has it
    }

    // The swapchain at the window's size, made again in place of the one before. A window of no size makes none.
    bool createSwapchain(U32 width, U32 height)
    {
        VkSurfaceCapabilitiesKHR caps = {};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysical, mSurface, &caps);
        if (result != VK_SUCCESS)
        {
            return failVK("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
        }
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        {
            return fail("the window's swapchain images cannot be copied into");
        }

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == U32_MAX)
        {
            extent.width  = llclamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = llclamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        U32 count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysical, mSurface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysical, mSurface, &count, formats.data());
        if (formats.empty())
        {
            return fail("the window offers no swapchain formats");
        }
        // The back buffer holds display values already: an 8-bit UNORM format takes them as they are.
        VkSurfaceFormatKHR chosen = formats[0];
        for (const VkSurfaceFormatKHR& f : formats)
        {
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen = f;
                break;
            }
        }
        VkFormatProperties format_props = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, chosen.format, &format_props);
        if (!(format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        {
            return fail("the swapchain's format cannot be blitted into");
        }

        U32 images = llmax(caps.minImageCount + 1, 2u);
        if (caps.maxImageCount > 0)
        {
            images = llmin(images, caps.maxImageCount);
        }

        mPresentMode = choosePresentMode();

        VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface          = mSurface;
        info.minImageCount    = images;
        info.imageFormat      = chosen.format;
        info.imageColorSpace  = chosen.colorSpace;
        info.imageExtent      = extent;
        info.imageArrayLayers = 1;
        info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform     = caps.currentTransform;
        info.compositeAlpha   = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                    : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                                   (~caps.supportedCompositeAlpha + 1));
        info.presentMode      = mPresentMode;
        info.clipped          = VK_TRUE;
        info.oldSwapchain     = mSwapchain;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        result = vkCreateSwapchainKHR(mDevice, &info, nullptr, &swapchain);
        destroySwapchainImages();
        if (mSwapchain)
        {
            vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
        }
        if (result != VK_SUCCESS)
        {
            return failVK("vkCreateSwapchainKHR", result);
        }
        mSwapchain  = swapchain;
        mSwapFormat = chosen.format;
        mSwapWidth  = extent.width;
        mSwapHeight = extent.height;

        U32 image_count = 0;
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &image_count, nullptr);
        mSwapImages.resize(image_count);
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &image_count, mSwapImages.data());
        mRenderDone.resize(image_count, VK_NULL_HANDLE);
        for (VkSemaphore& sem : mRenderDone)
        {
            VkSemaphoreCreateInfo sem_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            result = vkCreateSemaphore(mDevice, &sem_info, nullptr, &sem);
            if (result != VK_SUCCESS)
            {
                sem = VK_NULL_HANDLE;
                return failVK("vkCreateSemaphore (present)", result);
            }
        }
        mSwapchainDirty = false;
        LL_INFOS("Vulkan") << "Vulkan swapchain " << mSwapWidth << "x" << mSwapHeight << ", " << image_count
                           << " images, " << present_mode_name(mPresentMode) << LL_ENDL;
        return true;
    }

    void destroySwapchainImages()
    {
        for (VkSemaphore sem : mRenderDone)
        {
            if (sem)
            {
                vkDestroySemaphore(mDevice, sem, nullptr);
            }
        }
        mRenderDone.clear();
        mSwapImages.clear();
    }

    // Both APIs done with everything in flight: GL's copies, Vulkan's copies and presents.
    void waitIdle()
    {
        if (mGLMustWait && mGLTexture)
        {
            // Vulkan's last read signalled the semaphore: GL takes it, so it is left unsignalled for what comes next.
            const GLenum layout = GL_LAYOUT_GENERAL_EXT;
            gl.WaitSemaphoreEXT(mVKDoneGL, 0, nullptr, 1, &mGLTexture, &layout);
            mGLMustWait = false;
        }
        if (gl.Finish)
        {
            gl.Finish();
        }
        if (mDevice)
        {
            vkDeviceWaitIdle(mDevice);
        }
    }

    // Whether the shared image's memory really is shared, each way, told by pixels read back rather than by colours on
    // the screen: Vulkan fills it blue and GL reads a pixel; then GL fills it green and Vulkan reads a pixel into memory
    // the CPU maps. Both reads go to the log. Run once, at start-up, through the same semaphores the frames use; it
    // leaves the image as a frame does, Vulkan's read signalled for GL to wait on.
    void selfTest()
    {
        const GLenum general = GL_LAYOUT_GENERAL_EXT;
        VkCommandBuffer cmd = mCommands[0];
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        // Vulkan to GL.
        vkResetFences(mDevice, 1, &mFences[0]);
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        imageBarrier(cmd, mShared, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue blue = { { 0.f, 0.f, 1.f, 1.f } };
        const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, mShared, VK_IMAGE_LAYOUT_GENERAL, &blue, 1, &range);
        imageBarrier(cmd, mShared, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &mVKDoneVK;
        VkResult result = vkQueueSubmit(mQueue, 1, &submit, mFences[0]);
        if (result != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Self-test: Vulkan's fill not submitted (VkResult " << (S32)result << ")" << LL_ENDL;
            return;
        }
        vkWaitForFences(mDevice, 1, &mFences[0], VK_TRUE, UINT64_MAX);

        logGLErrors("before the self-test");
        gl.WaitSemaphoreEXT(mVKDoneGL, 0, nullptr, 1, &mGLTexture, &general);
        GLubyte gl_px[4] = { 7, 7, 7, 7 };
        gl.GetTextureSubImage(mGLTexture, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(gl_px), gl_px);
        logGLErrors("GL's read of Vulkan's fill");
        LL_INFOS("Vulkan") << "Self-test, Vulkan to GL: Vulkan filled blue (0 0 255 255), GL read " << (S32)gl_px[0]
                           << " " << (S32)gl_px[1] << " " << (S32)gl_px[2] << " " << (S32)gl_px[3] << LL_ENDL;

        // Memory the CPU maps, for Vulkan to read a pixel of GL's fill into: made before GL signals, so that a failure
        // here leaves no signal untaken.
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
        VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size        = 256;
        buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Self-test: no readback buffer" << LL_ENDL;
            return;
        }
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(mDevice, buffer, &req);
        VkPhysicalDeviceMemoryProperties mem = {};
        vkGetPhysicalDeviceMemoryProperties(mPhysical, &mem);
        U32 type = U32_MAX;
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (U32 i = 0; i < mem.memoryTypeCount; ++i)
        {
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & host) == host)
            {
                type = i;
                break;
            }
        }
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = type;
        if (type == U32_MAX || vkAllocateMemory(mDevice, &alloc, nullptr, &buffer_memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(mDevice, buffer, nullptr);
            LL_WARNS("Vulkan") << "Self-test: no readback memory" << LL_ENDL;
            return;
        }
        vkBindBufferMemory(mDevice, buffer, buffer_memory, 0);

        // GL to Vulkan.
        GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
        const GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (scissor)
        {
            gl.Disable(GL_SCISSOR_TEST);
        }
        const GLfloat green[4] = { 0.f, 1.f, 0.f, 1.f };
        gl.ClearNamedFramebufferfv(mGLFramebuffer, GL_COLOR, 0, green);
        GLubyte gl_own[4] = { 7, 7, 7, 7 };
        gl.GetTextureSubImage(mGLTexture, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(gl_own), gl_own);
        if (scissor)
        {
            gl.Enable(GL_SCISSOR_TEST);
        }
        gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
        gl.SignalSemaphoreEXT(mGLDoneGL, 0, nullptr, 1, &mGLTexture, &general);
        gl.Flush();
        logGLErrors("GL's fill");
        LL_INFOS("Vulkan") << "Self-test: GL filled green and reads its own texture as " << (S32)gl_own[0] << " "
                           << (S32)gl_own[1] << " " << (S32)gl_own[2] << " " << (S32)gl_own[3] << " (color mask was "
                           << (S32)mask[0] << (S32)mask[1] << (S32)mask[2] << (S32)mask[3] << ")" << LL_ENDL;

        vkResetFences(mDevice, 1, &mFences[0]);
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        imageBarrier(cmd, mShared, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent      = { 1, 1, 1 };
        vkCmdCopyImageToBuffer(cmd, mShared, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
        imageBarrier(cmd, mShared, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores    = &mGLDoneVK;
        submit.pWaitDstStageMask  = &stage;
        result = vkQueueSubmit(mQueue, 1, &submit, mFences[0]);
        if (result == VK_SUCCESS)
        {
            mGLMustWait = true;  // this read signalled Vulkan's semaphore, which the first frame's copy waits for
            vkWaitForFences(mDevice, 1, &mFences[0], VK_TRUE, UINT64_MAX);
            void* mapped = nullptr;
            if (vkMapMemory(mDevice, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped)
            {
                const U8* px = static_cast<const U8*>(mapped);
                LL_INFOS("Vulkan") << "Self-test, GL to Vulkan: GL filled green (0 255 0 255), Vulkan read "
                                   << (S32)px[0] << " " << (S32)px[1] << " " << (S32)px[2] << " " << (S32)px[3]
                                   << LL_ENDL;
                vkUnmapMemory(mDevice, buffer_memory);
            }
        }
        else
        {
            LL_WARNS("Vulkan") << "Self-test: Vulkan's read not submitted (VkResult " << (S32)result << ")" << LL_ENDL;
        }
        vkDestroyBuffer(mDevice, buffer, nullptr);
        vkFreeMemory(mDevice, buffer_memory, nullptr);
    }

    bool init(HWND window, bool vsync)
    {
        mWindow = window;
        mVSync  = vsync;
        if (!checkGL() || !createInstance() || !createSurface() || !pickDevice() || !createDevice() ||
            !createSharedSemaphore(mGLDoneVK, mGLDoneGL) || !createSharedSemaphore(mVKDoneVK, mVKDoneGL))
        {
            return false;
        }

        RECT rect = {};
        GetClientRect(mWindow, &rect);
        const U32 width  = (U32)llmax(0L, rect.right - rect.left);
        const U32 height = (U32)llmax(0L, rect.bottom - rect.top);
        if (width > 0 && height > 0 && (!createSwapchain(width, height) || !createSharedImage(width, height)))
        {
            return false;
        }
        if (mShared)
        {
            selfTest();
        }

        mActive = true;
        mNote.clear();
        return true;
    }

    void shutdown()
    {
        if (mDevice)
        {
            waitIdle();
        }
        destroySharedImage();
        if (mGLDoneGL)
        {
            gl.DeleteSemaphoresEXT(1, &mGLDoneGL);
            mGLDoneGL = 0;
        }
        if (mVKDoneGL)
        {
            gl.DeleteSemaphoresEXT(1, &mVKDoneGL);
            mVKDoneGL = 0;
        }
        if (mDevice)
        {
            destroySwapchainImages();
            if (mSwapchain)
            {
                vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
                mSwapchain = VK_NULL_HANDLE;
            }
            for (U32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
            {
                if (mFences[i])
                {
                    vkDestroyFence(mDevice, mFences[i], nullptr);
                    mFences[i] = VK_NULL_HANDLE;
                }
                if (mAcquired[i])
                {
                    vkDestroySemaphore(mDevice, mAcquired[i], nullptr);
                    mAcquired[i] = VK_NULL_HANDLE;
                }
                mCommands[i] = VK_NULL_HANDLE;
            }
            if (mGLDoneVK)
            {
                vkDestroySemaphore(mDevice, mGLDoneVK, nullptr);
                mGLDoneVK = VK_NULL_HANDLE;
            }
            if (mVKDoneVK)
            {
                vkDestroySemaphore(mDevice, mVKDoneVK, nullptr);
                mVKDoneVK = VK_NULL_HANDLE;
            }
            if (mCommandPool)
            {
                vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
                mCommandPool = VK_NULL_HANDLE;
            }
            vkDestroyDevice(mDevice, nullptr);
            mDevice = VK_NULL_HANDLE;
        }
        if (mInstance)
        {
            if (mSurface && vkDestroySurfaceKHR)
            {
                vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
            }
            mSurface = VK_NULL_HANDLE;
            if (mMessenger && vkDestroyDebugUtilsMessengerEXT)
            {
                vkDestroyDebugUtilsMessengerEXT(mInstance, mMessenger, nullptr);
            }
            mMessenger = VK_NULL_HANDLE;
            if (vkDestroyInstance)
            {
                vkDestroyInstance(mInstance, nullptr);
            }
            mInstance = VK_NULL_HANDLE;
        }
        if (mLibrary)
        {
            FreeLibrary(mLibrary);
            mLibrary = nullptr;
        }
        mActive = false;
    }

    // ---- each frame ---------------------------------------------------------------------------------------------

    // The window's size changed, or the swapchain went out of date: both are made again at the new size.
    bool resize(U32 width, U32 height)
    {
        waitIdle();
        if (mSwapWidth != width || mSwapHeight != height || mSwapchainDirty || !mSwapchain)
        {
            if (!createSwapchain(width, height))
            {
                return false;
            }
        }
        if (mSharedWidth != width || mSharedHeight != height || !mShared)
        {
            destroySharedImage();
            if (!createSharedImage(width, height))
            {
                return false;
            }
        }
        return true;
    }

    void imageBarrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                      VkImageLayout old_layout, VkImageLayout new_layout, U32 src_family, U32 dst_family,
                      VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
    {
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask       = src_access;
        barrier.dstAccessMask       = dst_access;
        barrier.oldLayout           = old_layout;
        barrier.newLayout           = new_layout;
        barrier.srcQueueFamilyIndex = src_family;
        barrier.dstQueueFamilyIndex = dst_family;
        barrier.image               = image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // GL's side: the back buffer into the shared image, once Vulkan is done reading the last frame out of it.
    void copyBackBuffer(U32 width, U32 height)
    {
        LL_PROFILE_ZONE_NAMED("vulkan present - gl copy");
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        if (!mLoggedFirstCopy)
        {
            logGLErrors("the viewer's frame, before the first copy");
        }
        if (mGLMustWait)
        {
            gl.WaitSemaphoreEXT(mVKDoneGL, 0, nullptr, 1, &mGLTexture, &layout);
            mGLMustWait = false;
        }

        // A blit obeys the scissor test and, into an sRGB-capable buffer, the sRGB write: neither is wanted here.
        const GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        const GLboolean srgb    = gl.IsEnabled(GL_FRAMEBUFFER_SRGB);
        if (scissor)
        {
            gl.Disable(GL_SCISSOR_TEST);
        }
        if (srgb)
        {
            gl.Disable(GL_FRAMEBUFFER_SRGB);
        }
        if (mFrame >= DIAGNOSE_FRAMES && mFrame < 2 * DIAGNOSE_FRAMES)
        {
            if (mFrame == DIAGNOSE_FRAMES)
            {
                LL_INFOS("Vulkan") << "Diagnosis: the next " << DIAGNOSE_FRAMES
                                   << " frames are green, filled by GL into the shared image" << LL_ENDL;
            }
            const GLfloat green[4] = { 0.f, 1.f, 0.f, 1.f };
            gl.ClearNamedFramebufferfv(mGLFramebuffer, GL_COLOR, 0, green);
        }
        else
        {
            gl.NamedFramebufferReadBuffer(0, GL_BACK);
            gl.BlitNamedFramebuffer(0, mGLFramebuffer, 0, 0, (GLint)width, (GLint)height, 0, 0, (GLint)width,
                                    (GLint)height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (srgb)
        {
            gl.Enable(GL_FRAMEBUFFER_SRGB);
        }
        if (scissor)
        {
            gl.Enable(GL_SCISSOR_TEST);
        }

        gl.SignalSemaphoreEXT(mGLDoneGL, 0, nullptr, 1, &mGLTexture, &layout);
        gl.Flush();
        if (!mLoggedFirstCopy)
        {
            GLint read_fb = -1;
            GLint read_buffer = -1;
            gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
            gl.GetIntegerv(GL_READ_BUFFER, &read_buffer);
            logGLErrors("the first copy into the shared image");
            LL_INFOS("Vulkan") << "First copy " << width << "x" << height << " (read framebuffer " << read_fb
                               << ", read buffer 0x" << std::hex << read_buffer << std::dec << ", scissor "
                               << (S32)scissor << ", sRGB " << (S32)srgb << ")" << LL_ENDL;
            mLoggedFirstCopy = true;
        }
    }

    bool present()
    {
        LL_PROFILE_ZONE_NAMED("vulkan present");
        if (!mActive)
        {
            return false;
        }

        RECT rect = {};
        GetClientRect(mWindow, &rect);
        const U32 width  = (U32)llmax(0L, rect.right - rect.left);
        const U32 height = (U32)llmax(0L, rect.bottom - rect.top);
        if (width == 0 || height == 0)
        {
            return true;  // minimized: nothing to show
        }
        if (width != mSwapWidth || height != mSwapHeight || width != mSharedWidth || height != mSharedHeight ||
            mSwapchainDirty || !mSwapchain || !mShared)
        {
            if (!resize(width, height))
            {
                return false;
            }
            if (mSwapWidth != width || mSwapHeight != height)
            {
                // The surface would not take the window's size yet (it is still being resized): try again next frame.
                mSwapchainDirty = true;
                return true;
            }
        }

        const U32 slot = (U32)(mFrame % FRAMES_IN_FLIGHT);
        VkResult result = vkWaitForFences(mDevice, 1, &mFences[slot], VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS)
        {
            return failVK("vkWaitForFences", result);
        }

        U32 index = 0;
        {
            LL_PROFILE_ZONE_NAMED("vulkan present - acquire");
            result = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, mAcquired[slot], VK_NULL_HANDLE, &index);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            mSwapchainDirty = true;
            return true;  // made again next frame
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            return failVK("vkAcquireNextImageKHR", result);
        }
        if (result == VK_SUBOPTIMAL_KHR)
        {
            mSwapchainDirty = true;
        }

        copyBackBuffer(width, height);

        VkCommandBuffer cmd = mCommands[slot];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        // The shared image, from GL, as it left it; the swapchain's, whatever it held, to be written over.
        imageBarrier(cmd, mShared, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(cmd, mSwapImages[index], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // GL's rows run bottom up and Vulkan's top down: the copy turns the image over.
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0]  = { 0, (S32)height, 0 };
        blit.srcOffsets[1]  = { (S32)width, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { (S32)mSwapWidth, (S32)mSwapHeight, 1 };
        if (mFrame < DIAGNOSE_FRAMES)
        {
            if (mFrame == 0)
            {
                LL_INFOS("Vulkan") << "Diagnosis: the first " << DIAGNOSE_FRAMES
                                   << " frames are magenta, then the viewer's frames" << LL_ENDL;
            }
            const VkClearColorValue magenta = { { 1.f, 0.f, 1.f, 1.f } };
            const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, mSwapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
        }
        else
        {
            if (mFrame == 2 * DIAGNOSE_FRAMES)
            {
                logGLErrors("the green frames");
                LL_INFOS("Vulkan") << "Diagnosis over: presenting the viewer's frames" << LL_ENDL;
            }
            vkCmdBlitImage(cmd, mShared, VK_IMAGE_LAYOUT_GENERAL, mSwapImages[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        }

        imageBarrier(cmd, mSwapImages[index], VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // And the shared image back to GL.
        imageBarrier(cmd, mShared, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL, mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cmd);

        VkSemaphore waits[2] = { mGLDoneVK, mAcquired[slot] };
        VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
        VkSemaphore signals[2] = { mRenderDone[index], mVKDoneVK };
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 2;
        submit.pWaitSemaphores      = waits;
        submit.pWaitDstStageMask    = stages;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 2;
        submit.pSignalSemaphores    = signals;

        vkResetFences(mDevice, 1, &mFences[slot]);
        result = vkQueueSubmit(mQueue, 1, &submit, mFences[slot]);
        if (result != VK_SUCCESS)
        {
            // GL's signal is left with no one to take it: the semaphores cannot be trusted again.
            return failVK("vkQueueSubmit", result);
        }
        mGLMustWait = true;

        VkPresentInfoKHR present_info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores    = &mRenderDone[index];
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = &mSwapchain;
        present_info.pImageIndices      = &index;
        {
            LL_PROFILE_ZONE_NAMED("vulkan present - queue present");
            result = vkQueuePresentKHR(mQueue, &present_info);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            mSwapchainDirty = true;
        }
        else if (result != VK_SUCCESS)
        {
            return failVK("vkQueuePresentKHR", result);
        }

        ++mFrame;
        return true;
    }
};

QLVulkanPresenter::QLVulkanPresenter() : mImpl(std::make_unique<Impl>()) {}

QLVulkanPresenter::~QLVulkanPresenter()
{
    // shutdown() needs the GL context current, which only the window can promise: it is called there first.
    llassert(!mImpl->mActive && !mImpl->mDevice);
}

bool QLVulkanPresenter::init(HWND window, bool vsync)
{
    if (!mImpl->init(window, vsync))
    {
        const std::string note = mImpl->mNote;
        mImpl->shutdown();
        mImpl->mNote = note;
        return false;
    }
    return true;
}

void QLVulkanPresenter::shutdown()
{
    mImpl->shutdown();
}

bool QLVulkanPresenter::active() const
{
    return mImpl->mActive;
}

bool QLVulkanPresenter::present()
{
    if (!mImpl->present())
    {
        mImpl->mActive = false;
        return false;
    }
    return true;
}

void QLVulkanPresenter::setVSync(bool vsync)
{
    if (mImpl->mVSync != vsync)
    {
        mImpl->mVSync          = vsync;
        mImpl->mSwapchainDirty = true;
    }
}

std::string QLVulkanPresenter::describe() const
{
    if (!mImpl->mActive)
    {
        return "OpenGL (Vulkan: " + mImpl->mNote + ")";
    }
    return llformat("Vulkan, %s, %ux%u", present_mode_name(mImpl->mPresentMode), mImpl->mSwapWidth,
                    mImpl->mSwapHeight);
}
