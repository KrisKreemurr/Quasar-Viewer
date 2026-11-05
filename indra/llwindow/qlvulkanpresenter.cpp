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

#if QL_HAS_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>
#endif

#include "qlvulkanshaders.h"

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
#ifndef GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT
#define GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT 0x9588
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
    typedef GLboolean (APIENTRY* QL_PFN_IsObject)(GLuint);
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
        QL_PFN_IsObject                     IsMemoryObjectEXT = nullptr;
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
            QL_GL_LOAD(IsMemoryObjectEXT, "glIsMemoryObjectEXT");
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
    // QuasarStreamline, as the viewer set it before the presenter started (QLVulkanPresenter::configureStreamline).
    struct QLStreamlineConfig
    {
        bool         mEnable = false;
        std::wstring mLogDir;
    };
    QLStreamlineConfig sStreamlineConfig;

    std::wstring ql_widen(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return std::wstring();
        }
        const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        std::wstring wide(count > 0 ? count : 0, L'\0');
        if (count > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), count);
        }
        return wide;
    }

    const char* reflex_mode_name(int mode)
    {
        switch (mode)
        {
        case 1:  return "low latency";
        case 2:  return "low latency + boost";
        default: return "off";
        }
    }

#if QL_HAS_STREAMLINE
    // Streamline's own messages, into the log: its warnings and errors always, its information only at debug level.
    void ql_sl_log(sl::LogType type, const char* msg)
    {
        std::string text = msg ? msg : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }
        if (type == sl::LogType::eError)
        {
            LL_WARNS("Streamline") << "Error: " << text << LL_ENDL;
        }
        else if (type == sl::LogType::eWarn)
        {
            LL_WARNS("Streamline") << text << LL_ENDL;
        }
        else
        {
            LL_DEBUGS("Streamline") << text << LL_ENDL;
        }
    }
#endif
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
    QL_VK_FN(vkCreateImageView);
    QL_VK_FN(vkDestroyImageView);
    QL_VK_FN(vkCreateShaderModule);
    QL_VK_FN(vkDestroyShaderModule);
    QL_VK_FN(vkCreateDescriptorSetLayout);
    QL_VK_FN(vkDestroyDescriptorSetLayout);
    QL_VK_FN(vkCreatePipelineLayout);
    QL_VK_FN(vkDestroyPipelineLayout);
    QL_VK_FN(vkCreateComputePipelines);
    QL_VK_FN(vkDestroyPipeline);
    QL_VK_FN(vkCreateDescriptorPool);
    QL_VK_FN(vkDestroyDescriptorPool);
    QL_VK_FN(vkAllocateDescriptorSets);
    QL_VK_FN(vkUpdateDescriptorSets);
    QL_VK_FN(vkCreateSampler);
    QL_VK_FN(vkDestroySampler);
    QL_VK_FN(vkCmdBindPipeline);
    QL_VK_FN(vkCmdBindDescriptorSets);
    QL_VK_FN(vkCmdPushConstants);
    QL_VK_FN(vkCmdDispatch);
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
    // The shared memory's handle: an NT handle, kept open while GL holds the import (a driver that imports on its own
    // thread can come to it after the call returns), or, where those do not share, a global (KMT) handle, never closed.
    bool           mKMT = false;
    HANDLE         mSharedHandle = nullptr;
    GLuint         mGLTexture = 0;
    GLuint         mGLFramebuffer = 0;

    // GL signals mGLDone once the frame is in the shared image; Vulkan signals mVKDone once it has read it.
    VkSemaphore mGLDoneVK = VK_NULL_HANDLE;
    VkSemaphore mVKDoneVK = VK_NULL_HANDLE;
    GLuint      mGLDoneGL = 0;
    GLuint      mVKDoneGL = 0;
    HANDLE      mGLDoneHandle = nullptr;
    HANDLE      mVKDoneHandle = nullptr;
    bool        mGLMustWait = false;  // a read Vulkan signalled the end of that GL has not yet waited for

    VkCommandBuffer mCommands[FRAMES_IN_FLIGHT] = {};
    VkFence         mFences[FRAMES_IN_FLIGHT] = {};
    VkSemaphore     mAcquired[FRAMES_IN_FLIGHT] = {};
    U64             mFrame = 0;

    bool mLoggedFirstCopy = false;

    // DLSS Frame Generation's inputs: images Vulkan allocates and GL imports, as the presentation image is, each
    // written by GL (turned over to Vulkan's top-down rows) after the world is drawn and read by Streamline in the
    // present's command buffer. They take turns through the same two semaphores as the presentation image.
    enum { FG_DEPTH, FG_MOTION, FG_HUDLESS, FG_COUNT };
    struct FGImage
    {
        VkImage           image = VK_NULL_HANDLE;
        VkDeviceMemory    memory = VK_NULL_HANDLE;
        VkImageView       view = VK_NULL_HANDLE;
        VkFormat          format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        HANDLE            handle = nullptr;  // NT handle, kept open while GL holds the import
        GLuint            glMemory = 0;
        GLuint            glTexture = 0;
        GLuint            glFramebuffer = 0;
    };
    FGImage mFG[FG_COUNT];
    U32     mFGWidth = 0;
    U32     mFGHeight = 0;
    GLuint  mGLReadFramebuffer = 0;  // the viewer's depth and motion textures, attached in turn to be copied from

    bool                              mFGWanted = false;        // QuasarDLSSFrameGen
    bool                              mFGLoaded = false;        // the DLSS-G plugin and its functions
    bool                              mFGOn = false;            // as Streamline was last told
    bool                              mFGInputsThisFrame = false;
    bool                              mFGFailed = false;        // its inputs could not be made: not tried again
    QLVulkanPresenter::FrameGenCamera mFGCamera = {};
    U32                               mFramesSinceSwapchain = 0;
    U32                               mFGStatus = 0;            // sl::DLSSGStatus, last read
    U32                               mFGLoggedStatus = 0;
    U32                               mFGPresented = 0;         // frames Streamline presented for the last of ours
    U32                               mFGMinSize = 0;
    std::string                       mFGNote = "off";

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

#if QL_HAS_STREAMLINE
    // NVIDIA Streamline, when QuasarStreamline loaded it: Vulkan comes through its interposer, which hooks the few
    // calls Reflex and frame generation need (instance and device creation, the swapchain, present).
    HMODULE                    mStreamline = nullptr;
    std::wstring               mStreamlineDir;
    PFun_slInit*               slInit = nullptr;
    PFun_slShutdown*           slShutdown = nullptr;
    PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
    PFun_slGetNewFrameToken*   slGetNewFrameToken = nullptr;
    PFun_slReflexSetOptions*   slReflexSetOptions = nullptr;
    PFun_slReflexSleep*        slReflexSleep = nullptr;
    PFun_slPCLSetMarker*       slPCLSetMarker = nullptr;
    PFun_slSetTagForFrame*     slSetTagForFrame = nullptr;
    PFun_slSetConstants*       slSetConstants = nullptr;
    PFun_slDLSSGSetOptions*    slDLSSGSetOptions = nullptr;
    PFun_slDLSSGGetState*      slDLSSGGetState = nullptr;
    sl::FrameToken*            mFrameToken = nullptr;
#endif
    bool        mStreamlineRunning = false;
    int         mReflexMode = -1;  // as last applied; -1 before the first frame
    std::string mStreamlineNote = "off";

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
        QL_VK_DEVICE(vkCreateImageView);
        QL_VK_DEVICE(vkDestroyImageView);
        QL_VK_DEVICE(vkCreateShaderModule);
        QL_VK_DEVICE(vkDestroyShaderModule);
        QL_VK_DEVICE(vkCreateDescriptorSetLayout);
        QL_VK_DEVICE(vkDestroyDescriptorSetLayout);
        QL_VK_DEVICE(vkCreatePipelineLayout);
        QL_VK_DEVICE(vkDestroyPipelineLayout);
        QL_VK_DEVICE(vkCreateComputePipelines);
        QL_VK_DEVICE(vkDestroyPipeline);
        QL_VK_DEVICE(vkCreateDescriptorPool);
        QL_VK_DEVICE(vkDestroyDescriptorPool);
        QL_VK_DEVICE(vkAllocateDescriptorSets);
        QL_VK_DEVICE(vkUpdateDescriptorSets);
        QL_VK_DEVICE(vkCreateSampler);
        QL_VK_DEVICE(vkDestroySampler);
        QL_VK_DEVICE(vkCmdBindPipeline);
        QL_VK_DEVICE(vkCmdBindDescriptorSets);
        QL_VK_DEVICE(vkCmdPushConstants);
        QL_VK_DEVICE(vkCmdDispatch);
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
#if QL_HAS_STREAMLINE
        if (sStreamlineConfig.mEnable && loadStreamline())
        {
            vkGetInstanceProcAddr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mStreamline, "vkGetInstanceProcAddr"));
        }
        else
#endif
        {
            if (sStreamlineConfig.mEnable)
            {
#if QL_HAS_STREAMLINE
                LL_WARNS("Vulkan") << "Streamline not loaded (" << mStreamlineNote << "): Vulkan alone" << LL_ENDL;
#else
                mStreamlineNote = "not in this build";
                LL_WARNS("Vulkan") << "QuasarStreamline is on, but this build has no Streamline SDK (the configure did "
                                      "not fetch it: see its output for 'NVIDIA Streamline'): Vulkan alone" << LL_ENDL;
#endif
            }
            mLibrary = LoadLibraryA("vulkan-1.dll");
            if (!mLibrary)
            {
                return fail("no Vulkan runtime (vulkan-1.dll)");
            }
            vkGetInstanceProcAddr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mLibrary, "vkGetInstanceProcAddr"));
        }
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
        // 1.3 where the loader has it: frame generation's optical flow runs natively from 1.1, best on 1.3.
        app.apiVersion = VK_API_VERSION_1_1;
        auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        U32 loader_version = VK_API_VERSION_1_0;
        if (enumerate_version && enumerate_version(&loader_version) == VK_SUCCESS && loader_version >= VK_API_VERSION_1_3)
        {
            app.apiVersion = VK_API_VERSION_1_3;
        }

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

#if QL_HAS_STREAMLINE
    // Streamline's interposer, from beside the viewer once NVIDIA's signature on it checks out, started with Reflex,
    // PC Latency and DLSS Frame Generation asked for (the last stays off until it is turned on). False, with why in
    // mStreamlineNote, leaves nothing loaded.
    bool loadStreamline()
    {
        wchar_t exe[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        mStreamlineDir.assign(exe, length);
        const size_t slash = mStreamlineDir.find_last_of(L"\\/");
        mStreamlineDir = slash == std::wstring::npos ? std::wstring(L".") : mStreamlineDir.substr(0, slash);
        const std::wstring path = mStreamlineDir + L"\\sl.interposer.dll";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            mStreamlineNote = "sl.interposer.dll is not beside the viewer";
            return false;
        }
        if (!sl::security::verifyEmbeddedSignature(path.c_str()))
        {
            mStreamlineNote = "sl.interposer.dll does not carry NVIDIA's signature";
            return false;
        }
        mStreamline = LoadLibraryW(path.c_str());
        if (!mStreamline)
        {
            mStreamlineNote = llformat("sl.interposer.dll did not load (error %u)", (U32)GetLastError());
            return false;
        }
        slInit               = reinterpret_cast<PFun_slInit*>(GetProcAddress(mStreamline, "slInit"));
        slShutdown           = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(mStreamline, "slShutdown"));
        slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(mStreamline, "slGetFeatureFunction"));
        slGetNewFrameToken   = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(mStreamline, "slGetNewFrameToken"));
        slSetTagForFrame     = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(mStreamline, "slSetTagForFrame"));
        slSetConstants       = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(mStreamline, "slSetConstants"));
        if (!slInit || !slShutdown || !slGetFeatureFunction || !slGetNewFrameToken ||
            !GetProcAddress(mStreamline, "vkGetInstanceProcAddr"))
        {
            mStreamlineNote = "sl.interposer.dll lacks the functions it should have";
            FreeLibrary(mStreamline);
            mStreamline = nullptr;
            return false;
        }

        const sl::Feature features[] = { sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G };
        const wchar_t* plugin_dirs[] = { mStreamlineDir.c_str() };
        sl::Preferences pref{};
        pref.showConsole        = false;
        pref.logLevel           = sl::LogLevel::eDefault;
        pref.pathsToPlugins     = plugin_dirs;
        pref.numPathsToPlugins  = 1;
        pref.pathToLogsAndData  = sStreamlineConfig.mLogDir.empty() ? nullptr : sStreamlineConfig.mLogDir.c_str();
        pref.logMessageCallback = ql_sl_log;
        // No updates downloaded over the air: the viewer runs the plugins it ships with.
        // Resources are tagged for a frame (slSetTagForFrame), as DLSS Frame Generation wants them.
        pref.flags              = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText |
                                  sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.featuresToLoad     = features;
        pref.numFeaturesToLoad  = (U32)LL_ARRAY_SIZE(features);
        pref.engine             = sl::EngineType::eCustom;
        pref.engineVersion      = "Quasar Viewer";
        pref.projectId          = "3f1c6e9a-5b2d-4c7e-9a41-8d0b6f2e7c53";
        pref.renderAPI          = sl::RenderAPI::eVulkan;
        const sl::Result result = slInit(pref, sl::kSDKVersion);
        if (result != sl::Result::eOk)
        {
            mStreamlineNote = llformat("slInit failed (sl::Result %d)", (S32)result);
            FreeLibrary(mStreamline);
            mStreamline = nullptr;
            return false;
        }
        mStreamlineRunning = true;
        mStreamlineNote    = "running";
        LL_INFOS("Vulkan") << "Vulkan through NVIDIA Streamline " << SL_VERSION_MAJOR << "." << SL_VERSION_MINOR << "."
                           << SL_VERSION_PATCH << LL_ENDL;
        return true;
    }

    // Once the device is made: the feature functions the frames call. A feature that did not load leaves its calls
    // out; the presentation goes on either way.
    void startStreamlineFeatures()
    {
        if (!mStreamlineRunning)
        {
            return;
        }
        void* fn = nullptr;
        if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk)
        {
            slReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fn);
        }
        fn = nullptr;
        if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fn) == sl::Result::eOk)
        {
            slReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fn);
        }
        fn = nullptr;
        if (slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk)
        {
            slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fn);
        }
        fn = nullptr;
        if (slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn) == sl::Result::eOk)
        {
            slDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(fn);
        }
        fn = nullptr;
        if (slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn) == sl::Result::eOk)
        {
            slDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState*>(fn);
        }
        mFGLoaded = slDLSSGSetOptions && slDLSSGGetState && slSetTagForFrame && slSetConstants && slPCLSetMarker &&
                    slReflexSetOptions;
        mFGNote   = mFGLoaded ? "off" : "not available (see the Streamline lines above for why)";
        LL_INFOS("Vulkan") << "Streamline features: Reflex " << (slReflexSetOptions && slReflexSleep ? "loaded" : "not loaded")
                           << ", PC Latency markers " << (slPCLSetMarker ? "loaded" : "not loaded")
                           << ", DLSS Frame Generation " << (mFGLoaded ? "loaded" : "not loaded") << LL_ENDL;
    }

    void mark(sl::PCLMarker marker)
    {
        if (slPCLSetMarker && mFrameToken)
        {
            slPCLSetMarker(marker, *mFrameToken);
        }
    }

    void frameBegin(int reflex_mode, bool frame_gen)
    {
        if (!mStreamlineRunning)
        {
            return;
        }
        if (frame_gen != mFGWanted)
        {
            mFGWanted = frame_gen;
            if (mFGLoaded)
            {
                // Streamline asks for the swapchain to be made again whenever frame generation is switched.
                mSwapchainDirty = true;
                LL_INFOS("Vulkan") << "DLSS Frame Generation " << (frame_gen ? "wanted" : "not wanted") << LL_ENDL;
            }
        }
        mFrameToken = nullptr;
        if (slGetNewFrameToken(mFrameToken, nullptr) != sl::Result::eOk)
        {
            mFrameToken = nullptr;
            return;
        }
        reflex_mode = llclamp(reflex_mode, 0, 2);
        if (mFGWanted && mFGLoaded && reflex_mode == 0)
        {
            reflex_mode = 1;  // frame generation paces itself through Reflex
        }
        if (reflex_mode != mReflexMode && slReflexSetOptions)
        {
            sl::ReflexOptions options{};
            options.mode = reflex_mode == 2 ? sl::ReflexMode::eLowLatencyWithBoost
                                            : (reflex_mode == 1 ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff);
            const sl::Result result = slReflexSetOptions(options);
            LL_INFOS("Vulkan") << "Reflex " << reflex_mode_name(reflex_mode)
                               << (result == sl::Result::eOk ? "" : llformat(" (sl::Result %d)", (S32)result))
                               << LL_ENDL;
            mReflexMode = reflex_mode;
        }
        // Reflex holds the CPU back here just long enough that the frame it begins reaches the GPU as it frees up.
        if (mReflexMode > 0 && slReflexSleep)
        {
            LL_PROFILE_ZONE_NAMED("reflex sleep");
            slReflexSleep(*mFrameToken);
        }
        mark(sl::PCLMarker::eSimulationStart);
    }
#endif

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
    bool createSharedSemaphore(VkSemaphore& vk, GLuint& gl_sem, HANDLE& kept)
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
        // Importing takes no ownership of the handle. It is kept open until shutdown all the same: a driver that
        // imports on its own thread can come to it after the call returns.
        kept = handle;
        return gl_sem != 0;
    }

    // The image GL copies the frame into and Vulkan copies it out of, at the back buffer's size.
    bool createSharedImage(U32 width, U32 height)
    {
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;

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
        exported.handleTypes = handle_type;
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
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        result = vkGetMemoryWin32HandleKHR(mDevice, &get, &handle);
        if (result != VK_SUCCESS || !handle)
        {
            return failVK("vkGetMemoryWin32HandleKHR", result);
        }
        if (!mKMT)
        {
            mSharedHandle = handle;  // closed with the image
        }

        // GL's side: a memory object over Vulkan's allocation, a texture stored in it, and a framebuffer to copy into.
        gl.CreateMemoryObjectsEXT(1, &mGLMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(mGLMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(mGLMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);

        gl.CreateTextures(GL_TEXTURE_2D, 1, &mGLTexture);
        gl.TextureParameteri(mGLTexture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(mGLTexture, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height, mGLMemory, 0);
        // The import and the texture's storage done with before anything is drawn into it.
        gl.Finish();

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
        LL_INFOS("Vulkan") << "Shared image " << width << "x" << height << ", " << req.size << " bytes, "
                           << (mKMT ? "KMT" : "NT") << " handle, GL memory object " << mGLMemory
                           << (gl.IsMemoryObjectEXT(mGLMemory) ? " (valid)" : " (not a memory object)")
                           << ", texture " << mGLTexture << ", framebuffer " << mGLFramebuffer << LL_ENDL;
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
        if (mSharedHandle)
        {
            CloseHandle(mSharedHandle);
            mSharedHandle = nullptr;
        }
        mSharedWidth = mSharedHeight = 0;
    }

    // ---- DLSS Frame Generation's inputs -------------------------------------------------------------------------

    // One input image: Vulkan's, exported, with a view for Streamline; GL's import of it, and a framebuffer to copy into.
    bool createFGImage(FGImage& img, VkFormat format, GLenum gl_format, U32 width, U32 height, const char* what)
    {
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, format, &props);
        const VkFormatFeatureFlags features = props.optimalTilingFeatures;
        if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !(features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        {
            return fgFail(llformat("the %s format cannot be read", what));
        }
        // Every use GL may make of it, as for the presentation image, so both lay the memory out alike.
        img.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        {
            img.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
        {
            img.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        img.format = format;

        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = { width, height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = img.usage;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(mDevice, &info, nullptr, &img.image);
        if (result != VK_SUCCESS)
        {
            img.image = VK_NULL_HANDLE;
            return fgFail(llformat("vkCreateImage (%s) failed (VkResult %d)", what, (S32)result));
        }

        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, img.image, &req);
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
            return fgFail(llformat("no device-local memory for the %s", what));
        }
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = img.image;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = type;
        result = vkAllocateMemory(mDevice, &alloc, nullptr, &img.memory);
        if (result != VK_SUCCESS)
        {
            img.memory = VK_NULL_HANDLE;
            return fgFail(llformat("vkAllocateMemory (%s) failed (VkResult %d)", what, (S32)result));
        }
        result = vkBindImageMemory(mDevice, img.image, img.memory, 0);
        if (result != VK_SUCCESS)
        {
            return fgFail(llformat("vkBindImageMemory (%s) failed (VkResult %d)", what, (S32)result));
        }

        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image            = img.image;
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        result = vkCreateImageView(mDevice, &view, nullptr, &img.view);
        if (result != VK_SUCCESS)
        {
            img.view = VK_NULL_HANDLE;
            return fgFail(llformat("vkCreateImageView (%s) failed (VkResult %d)", what, (S32)result));
        }

        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = img.memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        result = vkGetMemoryWin32HandleKHR(mDevice, &get, &handle);
        if (result != VK_SUCCESS || !handle)
        {
            return fgFail(llformat("vkGetMemoryWin32HandleKHR (%s) failed (VkResult %d)", what, (S32)result));
        }
        if (!mKMT)
        {
            img.handle = handle;
        }

        gl.CreateMemoryObjectsEXT(1, &img.glMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(img.glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(img.glMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.CreateTextures(GL_TEXTURE_2D, 1, &img.glTexture);
        gl.TextureParameteri(img.glTexture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(img.glTexture, 1, gl_format, (GLsizei)width, (GLsizei)height, img.glMemory, 0);
        gl.Finish();
        gl.CreateFramebuffers(1, &img.glFramebuffer);
        gl.NamedFramebufferTexture(img.glFramebuffer, GL_COLOR_ATTACHMENT0, img.glTexture, 0);
        logGLErrors(what);
        if (gl.CheckNamedFramebufferStatus(img.glFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            return fgFail(llformat("the %s is not a complete GL framebuffer", what));
        }
        return true;
    }

    void destroyFGImage(FGImage& img)
    {
        if (img.glFramebuffer)
        {
            gl.DeleteFramebuffers(1, &img.glFramebuffer);
        }
        if (img.glTexture)
        {
            gl.DeleteTextures(1, &img.glTexture);
        }
        if (img.glMemory)
        {
            gl.DeleteMemoryObjectsEXT(1, &img.glMemory);
        }
        if (img.view)
        {
            vkDestroyImageView(mDevice, img.view, nullptr);
        }
        if (img.image)
        {
            vkDestroyImage(mDevice, img.image, nullptr);
        }
        if (img.memory)
        {
            vkFreeMemory(mDevice, img.memory, nullptr);
        }
        if (img.handle)
        {
            CloseHandle(img.handle);
        }
        img = FGImage();
    }

    // Called only once both APIs are idle (waitIdle), as the presentation image's are made and let go.
    void destroyFGImages()
    {
        for (FGImage& img : mFG)
        {
            destroyFGImage(img);
        }
        if (mGLReadFramebuffer)
        {
            gl.DeleteFramebuffers(1, &mGLReadFramebuffer);
            mGLReadFramebuffer = 0;
        }
        mFGWidth = mFGHeight = 0;
    }

    bool createFGImages(U32 width, U32 height)
    {
        if (!createFGImage(mFG[FG_DEPTH], VK_FORMAT_R32_SFLOAT, GL_R32F, width, height, "frame generation depth") ||
            !createFGImage(mFG[FG_MOTION], VK_FORMAT_R16G16_SFLOAT, GL_RG16F, width, height, "frame generation motion") ||
            !createFGImage(mFG[FG_HUDLESS], VK_FORMAT_R8G8B8A8_UNORM, GL_RGBA8, width, height, "frame generation colour"))
        {
            destroyFGImages();
            return false;
        }
        gl.CreateFramebuffers(1, &mGLReadFramebuffer);
        mFGWidth  = width;
        mFGHeight = height;
        LL_INFOS("Vulkan") << "DLSS Frame Generation inputs " << width << "x" << height << " (depth R32F, motion RG16F, "
                           << "colour RGBA8)" << LL_ENDL;
        return true;
    }

    bool fgFail(const std::string& why)
    {
        if (mMakingPassImages)
        {
            mPassNote = "failed: " + why;
            LL_WARNS("Vulkan") << "Vulkan post passes: " << why << LL_ENDL;
            return false;
        }
        mFGNote = "inputs failed: " + why;
        LL_WARNS("Vulkan") << "DLSS Frame Generation: " << why << LL_ENDL;
        return false;
    }
    bool mMakingPassImages = false;  // createFGImage's failures are the post passes' (fgFail)

    // ---- post passes in Vulkan (QuasarVulkanPost) -----------------------------------------------------------

    // GL hands a frame over mid-chain: it copies the frame into mPassIn and signals mPassGLDone; Vulkan waits for that,
    // runs the pass into mPassOut and signals mPassVKDone; GL waits for that and copies mPassOut back. Each call balances
    // both semaphores, so nothing is pending between calls.
    FGImage               mPassIn;
    FGImage               mPassOut;
    U32                   mPassWidth = 0;
    U32                   mPassHeight = 0;
    VkSemaphore           mPassGLDoneVK = VK_NULL_HANDLE;
    VkSemaphore           mPassVKDoneVK = VK_NULL_HANDLE;
    GLuint                mPassGLDoneGL = 0;
    GLuint                mPassVKDoneGL = 0;
    HANDLE                mPassGLDoneHandle = nullptr;
    HANDLE                mPassVKDoneHandle = nullptr;
    VkShaderModule        mCASModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout mPassSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mPassLayout = VK_NULL_HANDLE;
    VkPipeline            mCASPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      mPassPool = VK_NULL_HANDLE;
    VkDescriptorSet       mPassSet = VK_NULL_HANDLE;
    VkSampler             mPassSampler = VK_NULL_HANDLE;
    VkCommandBuffer       mPassCommands[FRAMES_IN_FLIGHT] = {};
    VkFence               mPassFences[FRAMES_IN_FLIGHT] = {};
    GLuint                mGLPassReadFB = 0;
    GLuint                mGLPassDrawFB = 0;
    U64                   mPassCount = 0;
    bool                  mPassReady = false;   // the pipeline is made
    bool                  mPassFailed = false;  // something failed: GL runs its own passes for the rest of the session
    std::string           mPassNote = "not used yet";

    bool passFail(const std::string& why)
    {
        mPassNote = "failed: " + why;
        mPassFailed = true;
        LL_WARNS("Vulkan") << "Vulkan post passes: " << why << LL_ENDL;
        return false;
    }

    // The semaphores, the compute pipeline and what it binds, made once.
    bool createPassPipeline()
    {
        if (!createSharedSemaphore(mPassGLDoneVK, mPassGLDoneGL, mPassGLDoneHandle) ||
            !createSharedSemaphore(mPassVKDoneVK, mPassVKDoneGL, mPassVKDoneHandle))
        {
            return passFail("its semaphores could not be shared with GL");
        }

        VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module.codeSize = sizeof(ql_spv_qlcas_comp);
        module.pCode    = ql_spv_qlcas_comp;
        VkResult result = vkCreateShaderModule(mDevice, &module, nullptr, &mCASModule);
        if (result != VK_SUCCESS)
        {
            mCASModule = VK_NULL_HANDLE;
            return passFail(llformat("vkCreateShaderModule (CAS) failed (VkResult %d)", (S32)result));
        }

        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 2;
        set_info.pBindings    = bindings;
        result = vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mPassSetLayout);
        if (result != VK_SUCCESS)
        {
            mPassSetLayout = VK_NULL_HANDLE;
            return passFail(llformat("vkCreateDescriptorSetLayout failed (VkResult %d)", (S32)result));
        }

        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = 40;  // two uvec4 and a uvec2
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount         = 1;
        layout_info.pSetLayouts            = &mPassSetLayout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &push;
        result = vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &mPassLayout);
        if (result != VK_SUCCESS)
        {
            mPassLayout = VK_NULL_HANDLE;
            return passFail(llformat("vkCreatePipelineLayout failed (VkResult %d)", (S32)result));
        }

        VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipe.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipe.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        pipe.stage.module = mCASModule;
        pipe.stage.pName  = "main";
        pipe.layout       = mPassLayout;
        result = vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &pipe, nullptr, &mCASPipeline);
        if (result != VK_SUCCESS)
        {
            mCASPipeline = VK_NULL_HANDLE;
            return passFail(llformat("vkCreateComputePipelines (CAS) failed (VkResult %d)", (S32)result));
        }

        VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
        VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets       = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes    = sizes;
        result = vkCreateDescriptorPool(mDevice, &pool, nullptr, &mPassPool);
        if (result != VK_SUCCESS)
        {
            mPassPool = VK_NULL_HANDLE;
            return passFail(llformat("vkCreateDescriptorPool failed (VkResult %d)", (S32)result));
        }
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool     = mPassPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &mPassSetLayout;
        result = vkAllocateDescriptorSets(mDevice, &alloc, &mPassSet);
        if (result != VK_SUCCESS)
        {
            mPassSet = VK_NULL_HANDLE;
            return passFail(llformat("vkAllocateDescriptorSets failed (VkResult %d)", (S32)result));
        }

        VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter    = VK_FILTER_NEAREST;
        sampler.minFilter    = VK_FILTER_NEAREST;
        sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        result = vkCreateSampler(mDevice, &sampler, nullptr, &mPassSampler);
        if (result != VK_SUCCESS)
        {
            mPassSampler = VK_NULL_HANDLE;
            return passFail(llformat("vkCreateSampler failed (VkResult %d)", (S32)result));
        }

        VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd.commandPool        = mCommandPool;
        cmd.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd.commandBufferCount = FRAMES_IN_FLIGHT;
        result = vkAllocateCommandBuffers(mDevice, &cmd, mPassCommands);
        if (result != VK_SUCCESS)
        {
            return passFail(llformat("vkAllocateCommandBuffers (passes) failed (VkResult %d)", (S32)result));
        }
        for (VkFence& fence : mPassFences)
        {
            VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(mDevice, &fence_info, nullptr, &fence);
            if (result != VK_SUCCESS)
            {
                fence = VK_NULL_HANDLE;
                return passFail(llformat("vkCreateFence (passes) failed (VkResult %d)", (S32)result));
            }
        }
        gl.CreateFramebuffers(1, &mGLPassReadFB);
        gl.CreateFramebuffers(1, &mGLPassDrawFB);
        mPassReady = true;
        LL_INFOS("Vulkan") << "Vulkan post passes ready (CAS compute pipeline)" << LL_ENDL;
        return true;
    }

    // The two hand-over images at the frame's size (RGBA16F, which holds the post chain's 10-bit colour as it is),
    // made again only when the size changes, with both APIs idle.
    bool ensurePassImages(U32 width, U32 height)
    {
        if (mPassIn.image && mPassWidth == width && mPassHeight == height)
        {
            return true;
        }
        waitIdle();
        destroyFGImage(mPassIn);
        destroyFGImage(mPassOut);
        mMakingPassImages = true;
        const bool made = createFGImage(mPassIn, VK_FORMAT_R16G16B16A16_SFLOAT, GL_RGBA16F, width, height, "post pass input") &&
                          createFGImage(mPassOut, VK_FORMAT_R16G16B16A16_SFLOAT, GL_RGBA16F, width, height, "post pass output");
        mMakingPassImages = false;
        if (!made || !(mPassOut.usage & VK_IMAGE_USAGE_STORAGE_BIT))
        {
            destroyFGImage(mPassIn);
            destroyFGImage(mPassOut);
            return passFail(made ? "RGBA16F images cannot be written by a compute shader here" : mPassNote);
        }
        VkDescriptorImageInfo in_info = { mPassSampler, mPassIn.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo out_info = { VK_NULL_HANDLE, mPassOut.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        writes[0].dstSet          = mPassSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &in_info;
        writes[1].dstSet          = mPassSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo      = &out_info;
        vkUpdateDescriptorSets(mDevice, 2, writes, 0, nullptr);
        mPassWidth  = width;
        mPassHeight = height;
        LL_INFOS("Vulkan") << "Vulkan post pass images " << width << "x" << height << " (RGBA16F)" << LL_ENDL;
        return true;
    }

    bool sharpen(GLuint src, GLuint dst, U32 width, U32 height, const U32 c0[4], const U32 c1[4])
    {
        if (!mActive || mPassFailed || !src || !dst || width == 0 || height == 0)
        {
            return false;
        }
        LL_PROFILE_ZONE_NAMED("vulkan post - cas");
        if (!mPassReady && !createPassPipeline())
        {
            return false;
        }
        if (!ensurePassImages(width, height))
        {
            return false;
        }

        const GLint w = (GLint)width;
        const GLint h = (GLint)height;
        GLuint textures[2] = { mPassIn.glTexture, mPassOut.glTexture };
        const GLenum layouts[2] = { GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT };

        // GL: the frame into Vulkan's input, then the signal.
        GLCopyState state = beginGLCopy();
        gl.NamedFramebufferTexture(mGLPassReadFB, GL_COLOR_ATTACHMENT0, src, 0);
        gl.NamedFramebufferReadBuffer(mGLPassReadFB, GL_COLOR_ATTACHMENT0);
        gl.BlitNamedFramebuffer(mGLPassReadFB, mPassIn.glFramebuffer, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                                GL_NEAREST);
        gl.NamedFramebufferTexture(mGLPassReadFB, GL_COLOR_ATTACHMENT0, 0, 0);
        endGLCopy(state);
        gl.SignalSemaphoreEXT(mPassGLDoneGL, 0, nullptr, 2, textures, layouts);
        gl.Flush();

        // Vulkan: the filter.
        const U32 slot = (U32)(mPassCount % FRAMES_IN_FLIGHT);
        VkResult result = vkWaitForFences(mDevice, 1, &mPassFences[slot], VK_TRUE, UINT64_MAX);
        VkCommandBuffer cmd = mPassCommands[slot];
        if (result == VK_SUCCESS)
        {
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            imageBarrier(cmd, mPassIn.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            imageBarrier(cmd, mPassOut.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            struct { U32 c0[4]; U32 c1[4]; U32 size[2]; } push;
            memcpy(push.c0, c0, sizeof(push.c0));
            memcpy(push.c1, c1, sizeof(push.c1));
            push.size[0] = width;
            push.size[1] = height;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCASPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPassLayout, 0, 1, &mPassSet, 0, nullptr);
            vkCmdPushConstants(cmd, mPassLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
            imageBarrier(cmd, mPassIn.image, VK_ACCESS_SHADER_READ_BIT, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            imageBarrier(cmd, mPassOut.image, VK_ACCESS_SHADER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            vkEndCommandBuffer(cmd);

            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.waitSemaphoreCount   = 1;
            submit.pWaitSemaphores      = &mPassGLDoneVK;
            submit.pWaitDstStageMask    = &stage;
            submit.commandBufferCount   = 1;
            submit.pCommandBuffers      = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores    = &mPassVKDoneVK;
            vkResetFences(mDevice, 1, &mPassFences[slot]);
            result = vkQueueSubmit(mQueue, 1, &submit, mPassFences[slot]);
        }
        if (result != VK_SUCCESS)
        {
            // GL's signal is left with no one to take it: the passes' semaphores cannot be trusted again. GL's own pass
            // runs from src, which nothing here has touched.
            return passFail(llformat("the CAS pass could not be submitted (VkResult %d)", (S32)result));
        }

        // GL: wait for Vulkan on the GPU, then the result into dst.
        gl.WaitSemaphoreEXT(mPassVKDoneGL, 0, nullptr, 2, textures, layouts);
        state = beginGLCopy();
        gl.NamedFramebufferTexture(mGLPassDrawFB, GL_COLOR_ATTACHMENT0, dst, 0);
        gl.NamedFramebufferReadBuffer(mPassOut.glFramebuffer, GL_COLOR_ATTACHMENT0);
        gl.BlitNamedFramebuffer(mPassOut.glFramebuffer, mGLPassDrawFB, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                                GL_NEAREST);
        gl.NamedFramebufferTexture(mGLPassDrawFB, GL_COLOR_ATTACHMENT0, 0, 0);
        endGLCopy(state);
        if (mPassCount == 0)
        {
            logGLErrors("the first Vulkan CAS pass");
            LL_INFOS("Vulkan") << "First Vulkan CAS pass " << width << "x" << height << LL_ENDL;
        }
        ++mPassCount;
        mPassNote = "CAS in Vulkan";
        return true;
    }

    void destroyPassObjects()
    {
        destroyFGImage(mPassIn);
        destroyFGImage(mPassOut);
        mPassWidth = mPassHeight = 0;
        for (VkFence& fence : mPassFences)
        {
            if (fence)
            {
                vkDestroyFence(mDevice, fence, nullptr);
                fence = VK_NULL_HANDLE;
            }
        }
        if (mPassSampler)
        {
            vkDestroySampler(mDevice, mPassSampler, nullptr);
            mPassSampler = VK_NULL_HANDLE;
        }
        if (mPassPool)
        {
            vkDestroyDescriptorPool(mDevice, mPassPool, nullptr);  // frees mPassSet
            mPassPool = VK_NULL_HANDLE;
            mPassSet  = VK_NULL_HANDLE;
        }
        if (mCASPipeline)
        {
            vkDestroyPipeline(mDevice, mCASPipeline, nullptr);
            mCASPipeline = VK_NULL_HANDLE;
        }
        if (mPassLayout)
        {
            vkDestroyPipelineLayout(mDevice, mPassLayout, nullptr);
            mPassLayout = VK_NULL_HANDLE;
        }
        if (mPassSetLayout)
        {
            vkDestroyDescriptorSetLayout(mDevice, mPassSetLayout, nullptr);
            mPassSetLayout = VK_NULL_HANDLE;
        }
        if (mCASModule)
        {
            vkDestroyShaderModule(mDevice, mCASModule, nullptr);
            mCASModule = VK_NULL_HANDLE;
        }
        for (GLuint* fb : { &mGLPassReadFB, &mGLPassDrawFB })
        {
            if (*fb)
            {
                gl.DeleteFramebuffers(1, fb);
                *fb = 0;
            }
        }
        if (mPassGLDoneGL)
        {
            gl.DeleteSemaphoresEXT(1, &mPassGLDoneGL);
            mPassGLDoneGL = 0;
        }
        if (mPassVKDoneGL)
        {
            gl.DeleteSemaphoresEXT(1, &mPassVKDoneGL);
            mPassVKDoneGL = 0;
        }
        for (VkSemaphore* sem : { &mPassGLDoneVK, &mPassVKDoneVK })
        {
            if (*sem)
            {
                vkDestroySemaphore(mDevice, *sem, nullptr);
                *sem = VK_NULL_HANDLE;
            }
        }
        for (HANDLE* handle : { &mPassGLDoneHandle, &mPassVKDoneHandle })
        {
            if (*handle)
            {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
        mPassReady = false;
    }

    // Every GL texture over shared memory, in the layout both APIs leave it in, for GL's semaphore calls.
    U32 glInteropTextures(GLuint* textures, GLenum* layouts) const
    {
        U32 count = 0;
        if (mGLTexture)
        {
            textures[count] = mGLTexture;
            layouts[count++] = GL_LAYOUT_GENERAL_EXT;
        }
        for (const FGImage& img : mFG)
        {
            if (img.glTexture)
            {
                textures[count] = img.glTexture;
                layouts[count++] = GL_LAYOUT_GENERAL_EXT;
            }
        }
        return count;
    }

    // GL waits for Vulkan's last read of the shared images before it writes any of them again.
    void glWaitForVulkan()
    {
        if (!mGLMustWait)
        {
            return;
        }
        GLuint textures[1 + FG_COUNT];
        GLenum layouts[1 + FG_COUNT];
        const U32 count = glInteropTextures(textures, layouts);
        gl.WaitSemaphoreEXT(mVKDoneGL, 0, nullptr, count, textures, layouts);
        mGLMustWait = false;
    }

    // A blit obeys the scissor test and, into an sRGB-capable buffer, the sRGB write: neither is wanted in a copy.
    struct GLCopyState
    {
        GLboolean scissor = GL_FALSE;
        GLboolean srgb = GL_FALSE;
    };
    GLCopyState beginGLCopy()
    {
        GLCopyState state;
        state.scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        state.srgb    = gl.IsEnabled(GL_FRAMEBUFFER_SRGB);
        if (state.scissor)
        {
            gl.Disable(GL_SCISSOR_TEST);
        }
        if (state.srgb)
        {
            gl.Disable(GL_FRAMEBUFFER_SRGB);
        }
        return state;
    }
    void endGLCopy(const GLCopyState& state)
    {
        if (state.srgb)
        {
            gl.Enable(GL_FRAMEBUFFER_SRGB);
        }
        if (state.scissor)
        {
            gl.Enable(GL_SCISSOR_TEST);
        }
    }

    // The viewer's inputs for this frame, copied into the shared images, rows turned over to Vulkan's order.
    void frameGenInputs(GLuint depth_texture, GLuint motion_texture, U32 width, U32 height,
                        const QLVulkanPresenter::FrameGenCamera& camera)
    {
        if (!mActive || !mFGWanted || !mFGLoaded || mFGFailed || !depth_texture || !motion_texture)
        {
            return;
        }
        if (width != mSharedWidth || height != mSharedHeight)
        {
            return;  // the window is being resized: the presentation catches up first
        }
        LL_PROFILE_ZONE_NAMED("vulkan present - frame generation inputs");
        if (mFGWidth != width || mFGHeight != height || !mFG[FG_DEPTH].image)
        {
            waitIdle();
            destroyFGImages();
            if (!createFGImages(width, height))
            {
                mFGFailed = true;
                return;
            }
        }

        glWaitForVulkan();
        const GLCopyState state = beginGLCopy();
        const GLint w = (GLint)width;
        const GLint h = (GLint)height;
        gl.NamedFramebufferReadBuffer(0, GL_BACK);
        gl.BlitNamedFramebuffer(0, mFG[FG_HUDLESS].glFramebuffer, 0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT,
                                GL_NEAREST);
        gl.NamedFramebufferTexture(mGLReadFramebuffer, GL_COLOR_ATTACHMENT0, depth_texture, 0);
        gl.NamedFramebufferReadBuffer(mGLReadFramebuffer, GL_COLOR_ATTACHMENT0);
        gl.BlitNamedFramebuffer(mGLReadFramebuffer, mFG[FG_DEPTH].glFramebuffer, 0, 0, w, h, 0, h, w, 0,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.NamedFramebufferTexture(mGLReadFramebuffer, GL_COLOR_ATTACHMENT0, motion_texture, 0);
        gl.BlitNamedFramebuffer(mGLReadFramebuffer, mFG[FG_MOTION].glFramebuffer, 0, 0, w, h, 0, h, w, 0,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.NamedFramebufferTexture(mGLReadFramebuffer, GL_COLOR_ATTACHMENT0, 0, 0);
        endGLCopy(state);
        if (!mFGInputsLogged)
        {
            logGLErrors("copying the frame generation inputs");
            mFGInputsLogged = true;
        }

        mFGCamera          = camera;
        mFGInputsThisFrame = true;
    }
    bool mFGInputsLogged = false;

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
        // What frame generation does with the back buffer too (it reads it, and writes the frames it makes into it),
        // where the surface allows it.
        info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                (caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) &&
            (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        {
            info.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
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
        mSwapchainDirty       = false;
        mFramesSinceSwapchain = 0;
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
            glWaitForVulkan();
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

    // Whether the shared image's memory really is shared, each way, told by pixels read back: Vulkan fills it blue and
    // GL reads a pixel; then GL fills it green and Vulkan reads a pixel into memory the CPU maps. Both reads go to the
    // log. Run once per shared image made at start-up, through the same semaphores the frames use; it leaves the image
    // as a frame does, Vulkan's read signalled for GL to wait on. Sharing that fails without an error leaves the window
    // black, so a failed test keeps Vulkan from presenting at all (init).
    bool selfTest()
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
            return false;
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
            return false;
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
            return false;
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
        bool vk_saw_green = false;
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
                vk_saw_green = px[0] == 0 && px[1] == 255 && px[2] == 0;
                vkUnmapMemory(mDevice, buffer_memory);
            }
        }
        else
        {
            LL_WARNS("Vulkan") << "Self-test: Vulkan's read not submitted (VkResult " << (S32)result << ")" << LL_ENDL;
        }
        vkDestroyBuffer(mDevice, buffer, nullptr);
        vkFreeMemory(mDevice, buffer_memory, nullptr);
        const bool gl_saw_blue = gl_px[0] == 0 && gl_px[1] == 0 && gl_px[2] == 255;
        LL_INFOS("Vulkan") << "Self-test " << (gl_saw_blue && vk_saw_green ? "passed" : "failed") << LL_ENDL;
        return gl_saw_blue && vk_saw_green;
    }

    // After the device: Streamline's feature functions, where it runs. Never fails the start.
    bool startFeatures()
    {
#if QL_HAS_STREAMLINE
        startStreamlineFeatures();
#endif
        return true;
    }

    bool init(HWND window, bool vsync)
    {
        mWindow = window;
        mVSync  = vsync;
        if (!checkGL() || !createInstance() || !createSurface() || !pickDevice() || !createDevice() ||
            !startFeatures() ||
            !createSharedSemaphore(mGLDoneVK, mGLDoneGL, mGLDoneHandle) ||
            !createSharedSemaphore(mVKDoneVK, mVKDoneGL, mVKDoneHandle))
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
        if (mShared && !selfTest())
        {
            // An NT handle the driver took no share of: the same image again, through a global (KMT) handle.
            LL_WARNS("Vulkan") << "The presentation image is not shared through an NT handle; trying a KMT handle"
                               << LL_ENDL;
            waitIdle();
            destroySharedImage();
            mKMT = true;
            if (!createSharedImage(width, height))
            {
                return false;
            }
            if (!selfTest())
            {
                return fail("GL and Vulkan do not share the presentation image's memory with this driver");
            }
        }

        mActive = true;
        mNote.clear();
        return true;
    }

    void shutdown()
    {
#if QL_HAS_STREAMLINE
        if (mFGOn)
        {
            setFrameGenMode(false);
        }
#endif
        if (mDevice)
        {
            waitIdle();
        }
        destroySharedImage();
        destroyFGImages();
        if (mDevice)
        {
            destroyPassObjects();
        }
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
        for (HANDLE* handle : { &mGLDoneHandle, &mVKDoneHandle })
        {
            if (*handle)
            {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
        if (mDevice)
        {
            destroySwapchainImages();
            if (mSwapchain)
            {
                vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
                mSwapchain = VK_NULL_HANDLE;
            }
#if QL_HAS_STREAMLINE
            // Streamline let go once the swapchain it hooks is gone, before the device and instance.
            if (mStreamlineRunning)
            {
                slShutdown();
                mStreamlineRunning = false;
                mFrameToken        = nullptr;
            }
#endif
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
#if QL_HAS_STREAMLINE
        if (mStreamlineRunning && slShutdown)
        {
            slShutdown();  // started, but no device was made to let it go with
            mStreamlineRunning = false;
        }
        if (mStreamline)
        {
            FreeLibrary(mStreamline);
            mStreamline = nullptr;
        }
#endif
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
        if (mFGWidth != width || mFGHeight != height)
        {
            destroyFGImages();  // made again at the new size by the next frame's inputs
        }
        return true;
    }

#if QL_HAS_STREAMLINE
    static sl::ViewportHandle viewport() { return sl::ViewportHandle(0u); }

    void setFrameGenMode(bool on)
    {
        if (!slDLSSGSetOptions)
        {
            return;
        }
        sl::DLSSGOptions options{};
        options.mode                = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate = 1;
        // Kept while off: it goes off for a frame whenever the viewer draws one without inputs (a snapshot).
        options.flags               = sl::DLSSGFlags::eRetainResourcesWhenOff;
        options.numBackBuffers      = (uint32_t)mSwapImages.size();
        options.mvecDepthWidth      = mFGWidth;
        options.mvecDepthHeight     = mFGHeight;
        options.colorWidth          = mSwapWidth;
        options.colorHeight         = mSwapHeight;
        options.colorBufferFormat   = (uint32_t)mSwapFormat;
        options.mvecBufferFormat    = (uint32_t)VK_FORMAT_R16G16_SFLOAT;
        options.depthBufferFormat   = (uint32_t)VK_FORMAT_R32_SFLOAT;
        options.hudLessBufferFormat = (uint32_t)VK_FORMAT_R8G8B8A8_UNORM;
        const sl::Result result = slDLSSGSetOptions(viewport(), options);
        if (result != sl::Result::eOk)
        {
            LL_WARNS("Vulkan") << "DLSS Frame Generation " << (on ? "on" : "off") << ": slDLSSGSetOptions failed (sl::Result "
                               << (S32)result << ")" << LL_ENDL;
            if (on)
            {
                mFGNote = llformat("Streamline would not switch it on (sl::Result %d)", (S32)result);
                mFGFailed = true;
                return;
            }
        }
        mFGOn = on;
        LL_INFOS("Vulkan") << "DLSS Frame Generation " << (on ? "on" : "off") << ", " << mSwapWidth << "x" << mSwapHeight
                           << LL_ENDL;
    }

    // Each frame, once its inputs are in: whether frame generation runs for it, and if so the frame's camera.
    void updateFrameGen()
    {
        if (!mFGLoaded)
        {
            return;
        }
        bool want = mFGWanted && !mFGFailed;
        if (want && mVSync)
        {
            // Streamline runs frame generation under VSync only on D3D12.
            mFGNote = "waiting: VSync is on, and frame generation needs it off under Vulkan";
            want = false;
        }
        else if (want && mFGStatus != 0)
        {
            want = false;  // what slDLSSGGetState said, in mFGNote; tried again once the swapchain is made again
        }
        else if (want && mFGMinSize && llmin(mSwapWidth, mSwapHeight) < mFGMinSize)
        {
            mFGNote = llformat("waiting: the window is smaller than the %u pixels it needs", mFGMinSize);
            want = false;
        }
        else if (want && (!mFGInputsThisFrame || !mFrameToken))
        {
            want = false;  // a frame without the world's inputs (the login screen, a snapshot): shown as drawn
            if (!mFGOn)
            {
                mFGNote = "waiting for the world's inputs";
            }
        }
        else if (want && mFramesSinceSwapchain < 3)
        {
            want = false;  // a new swapchain settles first
        }
        if (!mFGWanted && !mFGFailed)
        {
            mFGNote = "off";
        }
        if (want != mFGOn)
        {
            setFrameGenMode(want);
        }
        if (!mFGOn)
        {
            return;
        }

        const QLVulkanPresenter::FrameGenCamera& cam = mFGCamera;
        auto matrix = [](sl::float4x4& dst, const float* m)
        {
            for (U32 row = 0; row < 4; ++row)
            {
                dst.setRow(row, sl::float4(m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]));
            }
        };
        sl::Constants c{};
        matrix(c.cameraViewToClip, cam.viewToClip);
        matrix(c.clipToCameraView, cam.clipToView);
        matrix(c.clipToPrevClip, cam.clipToPrevClip);
        matrix(c.prevClipToClip, cam.prevClipToClip);
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        matrix(c.clipToLensClip, identity);
        c.jitterOffset         = sl::float2(0.f, 0.f);  // the viewer does not jitter
        c.mvecScale            = sl::float2(1.f, 1.f);  // the motion is in window UV already
        c.cameraPinholeOffset  = sl::float2(0.f, 0.f);
        c.cameraPos            = sl::float3(cam.position[0], cam.position[1], cam.position[2]);
        c.cameraUp             = sl::float3(cam.up[0], cam.up[1], cam.up[2]);
        c.cameraRight          = sl::float3(cam.right[0], cam.right[1], cam.right[2]);
        c.cameraFwd            = sl::float3(cam.forward[0], cam.forward[1], cam.forward[2]);
        c.cameraNear           = cam.nearPlane;
        c.cameraFar            = cam.farPlane;
        c.cameraFOV            = cam.fovY;
        c.cameraAspectRatio    = cam.aspect;
        c.depthInverted        = cam.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D      = sl::Boolean::eFalse;
        c.reset                = cam.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse;
        c.motionVectorsDilated   = sl::Boolean::eFalse;
        c.motionVectorsJittered  = sl::Boolean::eFalse;
        const sl::Result result = slSetConstants(c, *mFrameToken, viewport());
        if (result != sl::Result::eOk && !mFGLoggedConstants)
        {
            LL_WARNS("Vulkan") << "DLSS Frame Generation: slSetConstants failed (sl::Result " << (S32)result << ")"
                               << LL_ENDL;
            mFGLoggedConstants = true;
        }
    }
    bool mFGLoggedConstants = false;

    // In the present's command buffer, with the inputs Vulkan's: Streamline copies them where it is told of them.
    void tagFrameGenInputs(VkCommandBuffer cmd)
    {
        sl::Resource resources[FG_COUNT];
        for (U32 i = 0; i < FG_COUNT; ++i)
        {
            const FGImage& img = mFG[i];
            sl::Resource& res = resources[i];
            res = sl::Resource(sl::ResourceType::eTex2d, (void*)img.image, (void*)img.memory, (void*)img.view,
                               (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
            res.width        = mFGWidth;
            res.height       = mFGHeight;
            res.nativeFormat = (uint32_t)img.format;
            res.mipLevels    = 1;
            res.arrayLayers  = 1;
            res.flags        = 0;
            res.usage        = (uint32_t)img.usage;
        }
        const sl::Extent extent{ 0, 0, mFGWidth, mFGHeight };
        const sl::ResourceTag tags[FG_COUNT] = {
            sl::ResourceTag(&resources[FG_DEPTH], sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
            sl::ResourceTag(&resources[FG_MOTION], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
                            &extent),
            sl::ResourceTag(&resources[FG_HUDLESS], sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow,
                            &extent),
        };
        const sl::Result result = slSetTagForFrame(*mFrameToken, viewport(), tags, FG_COUNT,
                                                   reinterpret_cast<sl::CommandBuffer*>(cmd));
        if (result != sl::Result::eOk && !mFGLoggedTags)
        {
            LL_WARNS("Vulkan") << "DLSS Frame Generation: slSetTagForFrame failed (sl::Result " << (S32)result << ")"
                               << LL_ENDL;
            mFGLoggedTags = true;
        }
    }
    bool mFGLoggedTags = false;

    // Now and then while it runs: what Streamline says of it.
    void pollFrameGen()
    {
        if (!mFGOn || !slDLSSGGetState || (mFrame % 30) != 0)
        {
            return;
        }
        sl::DLSSGState state{};
        if (slDLSSGGetState(viewport(), state, nullptr) != sl::Result::eOk)
        {
            return;
        }
        mFGPresented = state.numFramesActuallyPresented;
        mFGMinSize   = state.minWidthOrHeight;
        mFGStatus    = (U32)state.status;
        if (mFGStatus != 0 && mFGStatus != mFGLoggedStatus)
        {
            std::string why;
            if (state.status & sl::DLSSGStatus::eFailResolutionTooLow)
            {
                why += " the window is too small;";
            }
            if (state.status & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)
            {
                why += " Reflex was not seen running;";
            }
            if (state.status & sl::DLSSGStatus::eFailHDRFormatNotSupported)
            {
                why += " the back buffer's format is not supported;";
            }
            if (state.status & sl::DLSSGStatus::eFailCommonConstantsInvalid)
            {
                why += " the camera constants were refused;";
            }
            mFGNote = "stopped by Streamline:" + why;
            LL_WARNS("Vulkan") << "DLSS Frame Generation " << mFGNote << " (status 0x" << std::hex << mFGStatus
                               << std::dec << ")" << LL_ENDL;
            mFGLoggedStatus = mFGStatus;
        }
    }
#endif

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
        if (!mLoggedFirstCopy)
        {
            logGLErrors("the viewer's frame, before the first copy");
        }
        glWaitForVulkan();

        const GLCopyState state = beginGLCopy();
        gl.NamedFramebufferReadBuffer(0, GL_BACK);
        gl.BlitNamedFramebuffer(0, mGLFramebuffer, 0, 0, (GLint)width, (GLint)height, 0, 0, (GLint)width,
                                (GLint)height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        endGLCopy(state);
        const GLboolean scissor = state.scissor;
        const GLboolean srgb    = state.srgb;

        // Every shared image, the frame generation inputs too: Vulkan takes them all with this frame.
        GLuint textures[1 + FG_COUNT];
        GLenum layouts[1 + FG_COUNT];
        const U32 count = glInteropTextures(textures, layouts);
        gl.SignalSemaphoreEXT(mGLDoneGL, 0, nullptr, count, textures, layouts);
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
#if QL_HAS_STREAMLINE
        mark(sl::PCLMarker::eRenderSubmitEnd);
#endif

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
#if QL_HAS_STREAMLINE
            // Streamline wants frame generation off before the window changes under it.
            if (mFGOn)
            {
                setFrameGenMode(false);
            }
            mFGStatus = 0;  // and tried again on the new swapchain
            mFGInputsThisFrame = false;
#endif
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
#if QL_HAS_STREAMLINE
        updateFrameGen();
#endif

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
        // The frame generation inputs, which GL signalled with the frame, every frame they exist, read or not.
        for (const FGImage& img : mFG)
        {
            if (img.image)
            {
                imageBarrier(cmd, img.image, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        }
#if QL_HAS_STREAMLINE
        if (mFGOn && mFGInputsThisFrame)
        {
            tagFrameGenInputs(cmd);
        }
#endif

        // GL's rows run bottom up and Vulkan's top down: the copy turns the image over.
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0]  = { 0, (S32)height, 0 };
        blit.srcOffsets[1]  = { (S32)width, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { (S32)mSwapWidth, (S32)mSwapHeight, 1 };
        vkCmdBlitImage(cmd, mShared, VK_IMAGE_LAYOUT_GENERAL, mSwapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_NEAREST);

        imageBarrier(cmd, mSwapImages[index], VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // And the shared images back to GL.
        imageBarrier(cmd, mShared, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL, mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        for (const FGImage& img : mFG)
        {
            if (img.image)
            {
                imageBarrier(cmd, img.image, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, 0,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            }
        }
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
#if QL_HAS_STREAMLINE
            mark(sl::PCLMarker::ePresentStart);
#endif
            result = vkQueuePresentKHR(mQueue, &present_info);
#if QL_HAS_STREAMLINE
            mark(sl::PCLMarker::ePresentEnd);
#endif
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
        ++mFramesSinceSwapchain;
        mFGInputsThisFrame = false;
#if QL_HAS_STREAMLINE
        pollFrameGen();
#endif
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
    if (mImpl->mStreamlineRunning)
    {
        sStreamline = mImpl.get();
    }
    sActive = mImpl.get();
    return true;
}

void QLVulkanPresenter::shutdown()
{
    if (sStreamline == mImpl.get())
    {
        sStreamline = nullptr;
    }
    if (sActive == mImpl.get())
    {
        sActive = nullptr;
    }
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

QLVulkanPresenter::Impl* QLVulkanPresenter::sStreamline = nullptr;
QLVulkanPresenter::Impl* QLVulkanPresenter::sActive = nullptr;

// static
bool QLVulkanPresenter::postPassesAvailable()
{
    return sActive && sActive->mActive && !sActive->mPassFailed;
}

// static
bool QLVulkanPresenter::sharpen(unsigned src_texture, unsigned dst_texture, unsigned width, unsigned height,
                                const unsigned cas_const0[4], const unsigned cas_const1[4])
{
    return sActive && sActive->sharpen((GLuint)src_texture, (GLuint)dst_texture, (U32)width, (U32)height,
                                       reinterpret_cast<const U32*>(cas_const0), reinterpret_cast<const U32*>(cas_const1));
}

// static
std::string QLVulkanPresenter::postInfo()
{
    if (!sActive || !sActive->mActive)
    {
        return "needs Vulkan presentation (QuasarVulkanPresent)";
    }
    return sActive->mPassNote + llformat(", %llu passes", (unsigned long long)sActive->mPassCount);
}

// static
void QLVulkanPresenter::configureStreamline(bool enable, const std::string& log_dir)
{
    sStreamlineConfig.mEnable = enable;
    sStreamlineConfig.mLogDir = ql_widen(log_dir);
}

// static
void QLVulkanPresenter::frameBegin(int reflex_mode, bool frame_gen)
{
#if QL_HAS_STREAMLINE
    if (sStreamline)
    {
        sStreamline->frameBegin(reflex_mode, frame_gen);
    }
#endif
}

// static
bool QLVulkanPresenter::frameGenWanted()
{
    return sStreamline && sStreamline->mActive && sStreamline->mFGLoaded && sStreamline->mFGWanted &&
           !sStreamline->mFGFailed;
}

// static
void QLVulkanPresenter::frameGenInputs(unsigned depth_texture, unsigned motion_texture, unsigned width,
                                       unsigned height, const FrameGenCamera& camera)
{
    if (sStreamline)
    {
        sStreamline->frameGenInputs((GLuint)depth_texture, (GLuint)motion_texture, (U32)width, (U32)height, camera);
    }
}

// static
unsigned QLVulkanPresenter::frameGenShownPerDrawn()
{
    return (sStreamline && sStreamline->mActive && sStreamline->mFGOn) ? sStreamline->mFGPresented : 0u;
}

// static
std::string QLVulkanPresenter::frameGenInfo()
{
#if !QL_HAS_STREAMLINE
    return "not in this build (no Streamline SDK)";
#else
    if (!sStreamline || !sStreamline->mActive)
    {
        return "needs Vulkan presentation and Streamline (QuasarVulkanPresent, QuasarStreamline)";
    }
    const Impl& impl = *sStreamline;
    if (!impl.mFGLoaded)
    {
        return impl.mFGNote;
    }
    if (!impl.mFGWanted)
    {
        return "off (QuasarDLSSFrameGen)";
    }
    if (impl.mFGOn)
    {
        return llformat("on, %u shown a frame drawn, inputs %ux%u", impl.mFGPresented, impl.mFGWidth, impl.mFGHeight);
    }
    return impl.mFGNote;
#endif
}

// static
void QLVulkanPresenter::latencyMark(ELatencyMark mark)
{
#if QL_HAS_STREAMLINE
    if (sStreamline)
    {
        sStreamline->mark(mark == SIMULATION_END ? sl::PCLMarker::eSimulationEnd : sl::PCLMarker::eRenderSubmitStart);
    }
#endif
}

// static
std::string QLVulkanPresenter::streamlineInfo()
{
    if (!sStreamline || !sStreamline->mStreamlineRunning)
    {
        return "Streamline off";
    }
    return std::string("Streamline, Reflex ") + reflex_mode_name(sStreamline->mReflexMode);
}

std::string QLVulkanPresenter::describe() const
{
    if (!mImpl->mActive)
    {
        return "OpenGL (Vulkan: " + mImpl->mNote + ")";
    }
    std::string text = llformat("Vulkan, %s, %ux%u", present_mode_name(mImpl->mPresentMode), mImpl->mSwapWidth,
                                mImpl->mSwapHeight);
    if (mImpl->mStreamlineRunning)
    {
        text += std::string(" | Streamline, Reflex ") + reflex_mode_name(mImpl->mReflexMode);
    }
    else if (sStreamlineConfig.mEnable)
    {
        text += " | Streamline: " + mImpl->mStreamlineNote;
    }
    return text;
}
