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
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "qlexternalstorage.h"  // llrender: render targets in Vulkan's memory

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


// llgl.cpp: the main context's caps as llrender has set them (see GLCopyState below).
bool ql_gl_cap_tracked_enabled(unsigned int cap);

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
    typedef void (APIENTRY* QL_PFN_TextureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
    typedef void (APIENTRY* QL_PFN_CopyNamedBufferSubData)(GLuint, GLuint, GLintptr, GLintptr, GLsizeiptr);
    typedef void (APIENTRY* QL_PFN_NamedBufferStorageMemEXT)(GLuint, GLsizeiptr, GLuint, GLuint64);
    typedef void (APIENTRY* QL_PFN_ClearTexSubImage)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                                     GLenum, GLenum, const void*);
    typedef void (APIENTRY* QL_PFN_CopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint,
                                                     GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);

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
        QL_PFN_TextureStorage2D             TextureStorage2D = nullptr;
        QL_PFN_CopyImageSubData             CopyImageSubData = nullptr;
        QL_PFN_CopyNamedBufferSubData       CopyNamedBufferSubData = nullptr;
        QL_PFN_NamedBufferStorageMemEXT     NamedBufferStorageMemEXT = nullptr;
        QL_PFN_CreateObjects                CreateBuffers = nullptr;
        QL_PFN_ClearTexSubImage             ClearTexSubImage = nullptr;
        QL_PFN_DeleteObjects                DeleteBuffers = nullptr;

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
            QL_GL_LOAD(TextureStorage2D, "glTextureStorage2D");
            QL_GL_LOAD(CopyImageSubData, "glCopyImageSubData");
            QL_GL_LOAD(CopyNamedBufferSubData, "glCopyNamedBufferSubData");
            QL_GL_LOAD(NamedBufferStorageMemEXT, "glNamedBufferStorageMemEXT");
            QL_GL_LOAD(CreateBuffers, "glCreateBuffers");
            QL_GL_LOAD(ClearTexSubImage, "glClearTexSubImage");
            QL_GL_LOAD(DeleteBuffers, "glDeleteBuffers");
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
    QL_VK_FN(vkGetPhysicalDeviceFeatures);
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
    QL_VK_FN(vkGetFenceStatus);
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
    QL_VK_FN(vkCmdClearDepthStencilImage);
    QL_VK_FN(vkFreeCommandBuffers);
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
    QL_VK_FN(vkCmdCopyBuffer);
    QL_VK_FN(vkCmdFillBuffer);
    QL_VK_FN(vkCreateRenderPass);
    QL_VK_FN(vkDestroyRenderPass);
    QL_VK_FN(vkCreateFramebuffer);
    QL_VK_FN(vkDestroyFramebuffer);
    QL_VK_FN(vkCreateGraphicsPipelines);
    QL_VK_FN(vkCmdBeginRenderPass);
    QL_VK_FN(vkCmdEndRenderPass);
    QL_VK_FN(vkCmdDraw);
    QL_VK_FN(vkCmdDrawIndexed);
    QL_VK_FN(vkCmdBindVertexBuffers);
    QL_VK_FN(vkCmdBindIndexBuffer);
    QL_VK_FN(vkCmdSetViewport);
    QL_VK_FN(vkCmdSetScissor);
    QL_VK_FN(vkCmdCopyBufferToImage);
    QL_VK_FN(vkCmdDrawIndexedIndirect);
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
    GLuint  mGLReadFramebuffer = 0;  // the viewer's depth texture, kept attached while it is the same texture
    GLuint  mGLMotionFramebuffer = 0;  // and its motion texture
    GLuint  mGLReadDepth = 0, mGLReadMotion = 0;  // what each has attached
    U32     mGLReadAge = 0;  // frames since they were attached

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
        QL_VK_INSTANCE(vkGetPhysicalDeviceFeatures);
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
        QL_VK_DEVICE(vkGetFenceStatus);
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
        QL_VK_DEVICE(vkCmdClearDepthStencilImage);
        QL_VK_DEVICE(vkFreeCommandBuffers);
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
        QL_VK_DEVICE(vkCmdCopyBuffer);
        QL_VK_DEVICE(vkCmdFillBuffer);
        QL_VK_DEVICE(vkCreateRenderPass);
        QL_VK_DEVICE(vkDestroyRenderPass);
        QL_VK_DEVICE(vkCreateFramebuffer);
        QL_VK_DEVICE(vkDestroyFramebuffer);
        QL_VK_DEVICE(vkCreateGraphicsPipelines);
        QL_VK_DEVICE(vkCmdBeginRenderPass);
        QL_VK_DEVICE(vkCmdEndRenderPass);
        QL_VK_DEVICE(vkCmdDraw);
        QL_VK_DEVICE(vkCmdDrawIndexed);
        QL_VK_DEVICE(vkCmdBindVertexBuffers);
        QL_VK_DEVICE(vkCmdBindIndexBuffer);
        QL_VK_DEVICE(vkCmdSetViewport);
        QL_VK_DEVICE(vkCmdSetScissor);
        QL_VK_DEVICE(vkCmdCopyBufferToImage);
        QL_VK_DEVICE(vkCmdDrawIndexedIndirect);
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

    bool mHasDepthClamp = false;
    bool mHasMultiIndirect = false;

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

        // Quasar: what the shadow casters drawn in Vulkan use where the device has it: depth clamping, as the sun's
        // cascades clamp in GL, and one indirect draw for a whole map, each draw its own first instance.
        VkPhysicalDeviceFeatures supported = {};
        vkGetPhysicalDeviceFeatures(mPhysical, &supported);
        VkPhysicalDeviceFeatures enabled = {};
        enabled.depthClamp                = supported.depthClamp;
        enabled.multiDrawIndirect         = supported.multiDrawIndirect;
        enabled.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;
        mHasDepthClamp   = enabled.depthClamp == VK_TRUE;
        mHasMultiIndirect = enabled.multiDrawIndirect == VK_TRUE && enabled.drawIndirectFirstInstance == VK_TRUE;

        VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.queueCreateInfoCount    = 1;
        info.pQueueCreateInfos       = &queue;
        info.enabledExtensionCount   = (U32)LL_ARRAY_SIZE(wanted);
        info.ppEnabledExtensionNames = wanted;
        info.pEnabledFeatures        = &enabled;

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
        if (mGLMotionFramebuffer)
        {
            gl.DeleteFramebuffers(1, &mGLMotionFramebuffer);
            mGLMotionFramebuffer = 0;
        }
        mGLReadDepth = mGLReadMotion = 0;
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
        gl.CreateFramebuffers(1, &mGLMotionFramebuffer);
        gl.NamedFramebufferReadBuffer(mGLReadFramebuffer, GL_COLOR_ATTACHMENT0);
        gl.NamedFramebufferReadBuffer(mGLMotionFramebuffer, GL_COLOR_ATTACHMENT0);
        mGLReadDepth = mGLReadMotion = 0;
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

    // ---- shared geometry (step C of the Vulkan plan) ---------------------------------------------------------

    // Buffers both APIs see, the way to Vulkan drawing the viewer's own meshes (shadows first). For now one test: a
    // buffer Vulkan allocates and GL imports as an ordinary buffer object; GL writes a pattern and Vulkan reads it back,
    // then Vulkan writes one and GL reads it back, through two semaphores of its own. Run once, after login, by the
    // viewer (geometryTest); it waits for each step on the CPU, which once is nothing.
    typedef void (APIENTRY* QL_PFN_NamedBufferStorageMem)(GLuint, GLsizeiptr, GLuint, GLuint64);
    typedef void (APIENTRY* QL_PFN_NamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, const void*);
    typedef void (APIENTRY* QL_PFN_GetNamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, void*);
    bool        mGeoTested = false;
    bool        mGeoPassed = false;
    std::string mGeoNote = "not tested yet";

    void bufferBarrier(VkCommandBuffer cmd, VkBuffer buffer, VkAccessFlags src_access, VkAccessFlags dst_access,
                       U32 src_family, U32 dst_family, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
    {
        VkBufferMemoryBarrier barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        barrier.srcAccessMask       = src_access;
        barrier.dstAccessMask       = dst_access;
        barrier.srcQueueFamilyIndex = src_family;
        barrier.dstQueueFamilyIndex = dst_family;
        barrier.buffer              = buffer;
        barrier.offset              = 0;
        barrier.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    }

    // Memory of the first type with all of `flags` that `bits` allows, or U32_MAX.
    U32 memoryType(U32 bits, VkMemoryPropertyFlags flags)
    {
        VkPhysicalDeviceMemoryProperties mem = {};
        vkGetPhysicalDeviceMemoryProperties(mPhysical, &mem);
        for (U32 i = 0; i < mem.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & flags) == flags)
            {
                return i;
            }
        }
        return U32_MAX;
    }

    bool geometryTest()
    {
        if (mGeoTested)
        {
            return mGeoPassed;
        }
        mGeoTested = true;
        if (!mActive)
        {
            mGeoNote = "not tested: Vulkan does not present";
            return false;
        }
        LL_PROFILE_ZONE_NAMED("vulkan geometry self-test");

        auto storage_mem = reinterpret_cast<QL_PFN_NamedBufferStorageMem>(LLWindowWin32::getProcAddress("glNamedBufferStorageMemEXT"));
        auto create_buffers = reinterpret_cast<QL_PFN_CreateObjects>(LLWindowWin32::getProcAddress("glCreateBuffers"));
        auto delete_buffers = reinterpret_cast<QL_PFN_DeleteObjects>(LLWindowWin32::getProcAddress("glDeleteBuffers"));
        auto buffer_sub_data = reinterpret_cast<QL_PFN_NamedBufferSubData>(LLWindowWin32::getProcAddress("glNamedBufferSubData"));
        auto get_buffer_sub_data = reinterpret_cast<QL_PFN_GetNamedBufferSubData>(LLWindowWin32::getProcAddress("glGetNamedBufferSubData"));
        if (!storage_mem || !create_buffers || !delete_buffers || !buffer_sub_data || !get_buffer_sub_data)
        {
            mGeoNote = "failed: the GL driver lacks glNamedBufferStorageMemEXT or the DSA buffer calls";
            LL_WARNS("Vulkan") << "Geometry self-test " << mGeoNote << LL_ENDL;
            return false;
        }

        constexpr U32 TEST_BYTES = 1 << 20;  // 1 MB
        constexpr U32 PATTERN_WORDS = 1024;  // 4 KB each way
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        // Everything made, then let go on every way out.
        VkBuffer       shared = VK_NULL_HANDLE;
        VkDeviceMemory shared_memory = VK_NULL_HANDLE;
        HANDLE         shared_handle = nullptr;
        VkBuffer       host = VK_NULL_HANDLE;
        VkDeviceMemory host_memory = VK_NULL_HANDLE;
        VkSemaphore    gl_done_vk = VK_NULL_HANDLE, vk_done_vk = VK_NULL_HANDLE;
        GLuint         gl_done_gl = 0, vk_done_gl = 0;
        HANDLE         gl_done_handle = nullptr, vk_done_handle = nullptr;
        GLuint         gl_memory = 0, gl_buffer = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence        fence = VK_NULL_HANDLE;
        auto cleanup = [&]()
        {
            if (mDevice)
            {
                vkDeviceWaitIdle(mDevice);
            }
            if (gl.Finish)
            {
                gl.Finish();
            }
            if (gl_buffer) delete_buffers(1, &gl_buffer);
            if (gl_memory) gl.DeleteMemoryObjectsEXT(1, &gl_memory);
            if (gl_done_gl) gl.DeleteSemaphoresEXT(1, &gl_done_gl);
            if (vk_done_gl) gl.DeleteSemaphoresEXT(1, &vk_done_gl);
            if (gl_done_vk) vkDestroySemaphore(mDevice, gl_done_vk, nullptr);
            if (vk_done_vk) vkDestroySemaphore(mDevice, vk_done_vk, nullptr);
            if (gl_done_handle) CloseHandle(gl_done_handle);
            if (vk_done_handle) CloseHandle(vk_done_handle);
            if (fence) vkDestroyFence(mDevice, fence, nullptr);
            if (host) vkDestroyBuffer(mDevice, host, nullptr);
            if (host_memory) vkFreeMemory(mDevice, host_memory, nullptr);
            if (shared) vkDestroyBuffer(mDevice, shared, nullptr);
            if (shared_memory) vkFreeMemory(mDevice, shared_memory, nullptr);
            if (shared_handle) CloseHandle(shared_handle);
        };
        auto fail = [&](const std::string& why)
        {
            mGeoNote = "failed: " + why;
            LL_WARNS("Vulkan") << "Geometry self-test " << mGeoNote << LL_ENDL;
            cleanup();
            return false;
        };

        // The shared buffer, declared for every use the viewer's geometry could be put to.
        VkExternalMemoryBufferCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = handle_type;
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.pNext       = &external;
        info.size        = TEST_BYTES;
        info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &info, nullptr, &shared) != VK_SUCCESS)
        {
            shared = VK_NULL_HANDLE;
            return fail("vkCreateBuffer (shared)");
        }
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(mDevice, shared, &req);
        const U32 device_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (device_type == U32_MAX)
        {
            return fail("no device-local memory for a shared buffer");
        }
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = shared;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = device_type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &shared_memory) != VK_SUCCESS)
        {
            shared_memory = VK_NULL_HANDLE;
            return fail("vkAllocateMemory (shared)");
        }
        vkBindBufferMemory(mDevice, shared, shared_memory, 0);
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = shared_memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            return fail("vkGetMemoryWin32HandleKHR (shared buffer)");
        }
        if (!mKMT)
        {
            shared_handle = handle;  // kept open while GL holds the import, as the images' are
        }

        // GL's side: a memory object over it, and an ordinary buffer object stored in it.
        gl.CreateMemoryObjectsEXT(1, &gl_memory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(gl_memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(gl_memory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        create_buffers(1, &gl_buffer);
        storage_mem(gl_buffer, (GLsizeiptr)TEST_BYTES, gl_memory, 0);
        gl.Finish();
        logGLErrors("importing the shared buffer");

        // A buffer the CPU maps, for Vulkan's reads; the semaphores; a command buffer and a fence.
        VkBufferCreateInfo host_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        host_info.size        = PATTERN_WORDS * 4;
        host_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        host_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &host_info, nullptr, &host) != VK_SUCCESS)
        {
            host = VK_NULL_HANDLE;
            return fail("vkCreateBuffer (readback)");
        }
        vkGetBufferMemoryRequirements(mDevice, host, &req);
        const U32 host_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo host_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        host_alloc.allocationSize  = req.size;
        host_alloc.memoryTypeIndex = host_type;
        if (host_type == U32_MAX || vkAllocateMemory(mDevice, &host_alloc, nullptr, &host_memory) != VK_SUCCESS)
        {
            host_memory = VK_NULL_HANDLE;
            return fail("no readback memory");
        }
        vkBindBufferMemory(mDevice, host, host_memory, 0);
        if (!createSharedSemaphore(gl_done_vk, gl_done_gl, gl_done_handle) ||
            !createSharedSemaphore(vk_done_vk, vk_done_gl, vk_done_handle))
        {
            return fail("its semaphores could not be shared");
        }
        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkAllocateCommandBuffers(mDevice, &cmd_info, &cmd) != VK_SUCCESS ||
            vkCreateFence(mDevice, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            return fail("no command buffer");
        }

        // Runs one batch of Vulkan work between GL's signal and GL's wait.
        auto run = [&](auto&& record) -> bool
        {
            gl.SignalSemaphoreEXT(gl_done_gl, 1, &gl_buffer, 0, nullptr, nullptr);
            gl.Flush();
            vkResetFences(mDevice, 1, &fence);
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            bufferBarrier(cmd, shared, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            record();
            bufferBarrier(cmd, shared, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, 0, mQueueFamily,
                          VK_QUEUE_FAMILY_EXTERNAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            vkEndCommandBuffer(cmd);
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.waitSemaphoreCount   = 1;
            submit.pWaitSemaphores      = &gl_done_vk;
            submit.pWaitDstStageMask    = &stage;
            submit.commandBufferCount   = 1;
            submit.pCommandBuffers      = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores    = &vk_done_vk;
            if (vkQueueSubmit(mQueue, 1, &submit, fence) != VK_SUCCESS)
            {
                return false;  // GL's signal untaken: cleanup waits for the device and lets the semaphores go
            }
            vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX);
            gl.WaitSemaphoreEXT(vk_done_gl, 1, &gl_buffer, 0, nullptr, nullptr);
            return true;
        };

        // GL to Vulkan: GL writes a pattern, Vulkan copies it to memory the CPU reads.
        std::vector<U32> pattern(PATTERN_WORDS);
        for (U32 i = 0; i < PATTERN_WORDS; ++i)
        {
            pattern[i] = 0x51A50000u + i;
        }
        buffer_sub_data(gl_buffer, 0, PATTERN_WORDS * 4, pattern.data());
        if (!run([&]()
                 {
                     VkBufferCopy copy = { 0, 0, PATTERN_WORDS * 4 };
                     vkCmdCopyBuffer(cmd, shared, host, 1, &copy);
                 }))
        {
            return fail("Vulkan's read could not be submitted");
        }
        U32 gl_to_vk_bad = PATTERN_WORDS;
        void* mapped = nullptr;
        if (vkMapMemory(mDevice, host_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped)
        {
            gl_to_vk_bad = 0;
            const U32* words = static_cast<const U32*>(mapped);
            for (U32 i = 0; i < PATTERN_WORDS; ++i)
            {
                gl_to_vk_bad += words[i] != pattern[i] ? 1 : 0;
            }
            vkUnmapMemory(mDevice, host_memory);
        }

        // Vulkan to GL: Vulkan fills the next 4 KB, GL reads it.
        constexpr U32 FILL = 0xC0FFEE42u;
        if (!run([&]() { vkCmdFillBuffer(cmd, shared, PATTERN_WORDS * 4, PATTERN_WORDS * 4, FILL); }))
        {
            return fail("Vulkan's write could not be submitted");
        }
        std::vector<U32> read_back(PATTERN_WORDS, 0);
        get_buffer_sub_data(gl_buffer, PATTERN_WORDS * 4, PATTERN_WORDS * 4, read_back.data());
        logGLErrors("the geometry self-test");
        U32 vk_to_gl_bad = 0;
        for (U32 word : read_back)
        {
            vk_to_gl_bad += word != FILL ? 1 : 0;
        }

        mGeoPassed = gl_to_vk_bad == 0 && vk_to_gl_bad == 0;
        mGeoNote = mGeoPassed ? "shared buffers work (self-test passed)"
                              : llformat("failed: %u of %u words wrong from GL to Vulkan, %u from Vulkan to GL",
                                         gl_to_vk_bad, PATTERN_WORDS, vk_to_gl_bad);
        LL_INFOS("Vulkan") << "Geometry self-test, 1 MB buffer through " << (mKMT ? "a KMT" : "an NT")
                           << " handle: GL to Vulkan " << (PATTERN_WORDS - gl_to_vk_bad) << "/" << PATTERN_WORDS
                           << " words right, Vulkan to GL " << (PATTERN_WORDS - vk_to_gl_bad) << "/" << PATTERN_WORDS
                           << " right: " << (mGeoPassed ? "passed" : "FAILED") << LL_ENDL;
        cleanup();
        return mGeoPassed;
    }

    // ---- step 1 of geometry in Vulkan: the shadow map hand-over (QuasarVulkanGeometryTest) ----------------------

    // Once, after login, with the window's GL context current: Vulkan draws into a depth image it allocates and GL
    // imports, GL copies it into a depth texture of its own made as the viewer's shadow maps are (DEPTH_COMPONENT32F,
    // immutable), and reads every texel back against what was drawn. That is the hand-over a shadow map drawn in Vulkan
    // takes before the lighting samples it, and the first thing Vulkan draws rather than copies: a render pass, a
    // graphics pipeline with no fragment stage, depth only. Waits on the CPU for each step, so it is run once.
    bool        mDepthTested = false;
    bool        mDepthPassed = false;
    std::string mDepthNote = "not tested yet";

    bool depthTest()
    {
        if (mDepthTested)
        {
            return mDepthPassed;
        }
        mDepthTested = true;
        if (!mActive)
        {
            mDepthNote = "not tested: Vulkan does not present";
            return false;
        }
        LL_PROFILE_ZONE_NAMED("vulkan shadow map self-test");

        constexpr U32 SIZE = 256;
        constexpr VkFormat FORMAT = VK_FORMAT_D32_SFLOAT;
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        VkImage         image = VK_NULL_HANDLE;
        VkDeviceMemory  memory = VK_NULL_HANDLE;
        HANDLE          memory_handle = nullptr;
        VkImageView     view = VK_NULL_HANDLE;
        VkRenderPass    pass = VK_NULL_HANDLE;
        VkFramebuffer   framebuffer = VK_NULL_HANDLE;
        VkShaderModule  module = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline      pipeline = VK_NULL_HANDLE;
        VkSemaphore     gl_done_vk = VK_NULL_HANDLE, vk_done_vk = VK_NULL_HANDLE;
        GLuint          gl_done_gl = 0, vk_done_gl = 0;
        HANDLE          gl_done_handle = nullptr, vk_done_handle = nullptr;
        GLuint          gl_memory = 0, gl_imported = 0, gl_own = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        auto cleanup = [&]()
        {
            if (mDevice)
            {
                vkDeviceWaitIdle(mDevice);
            }
            if (gl.Finish)
            {
                gl.Finish();
            }
            if (gl_own) gl.DeleteTextures(1, &gl_own);
            if (gl_imported) gl.DeleteTextures(1, &gl_imported);
            if (gl_memory) gl.DeleteMemoryObjectsEXT(1, &gl_memory);
            if (gl_done_gl) gl.DeleteSemaphoresEXT(1, &gl_done_gl);
            if (vk_done_gl) gl.DeleteSemaphoresEXT(1, &vk_done_gl);
            if (gl_done_vk) vkDestroySemaphore(mDevice, gl_done_vk, nullptr);
            if (vk_done_vk) vkDestroySemaphore(mDevice, vk_done_vk, nullptr);
            if (gl_done_handle) CloseHandle(gl_done_handle);
            if (vk_done_handle) CloseHandle(vk_done_handle);
            if (fence) vkDestroyFence(mDevice, fence, nullptr);
            if (pipeline) vkDestroyPipeline(mDevice, pipeline, nullptr);
            if (layout) vkDestroyPipelineLayout(mDevice, layout, nullptr);
            if (module) vkDestroyShaderModule(mDevice, module, nullptr);
            if (framebuffer) vkDestroyFramebuffer(mDevice, framebuffer, nullptr);
            if (pass) vkDestroyRenderPass(mDevice, pass, nullptr);
            if (view) vkDestroyImageView(mDevice, view, nullptr);
            if (image) vkDestroyImage(mDevice, image, nullptr);
            if (memory) vkFreeMemory(mDevice, memory, nullptr);
            if (memory_handle) CloseHandle(memory_handle);
        };
        auto fail = [&](const std::string& why)
        {
            mDepthNote = "failed: " + why;
            LL_WARNS("Vulkan") << "Shadow map self-test " << mDepthNote << LL_ENDL;
            cleanup();
            return false;
        };

        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, FORMAT, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            return fail("D32_SFLOAT cannot be drawn into");
        }

        // The depth image, exported, with every use a shadow map could be put to.
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = FORMAT;
        info.extent        = { SIZE, SIZE, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(mDevice, &info, nullptr, &image) != VK_SUCCESS)
        {
            image = VK_NULL_HANDLE;
            return fail("vkCreateImage (D32_SFLOAT)");
        }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, image, &req);
        const U32 type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == U32_MAX)
        {
            return fail("no device-local memory for a depth image");
        }
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = image;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &memory) != VK_SUCCESS)
        {
            memory = VK_NULL_HANDLE;
            return fail("vkAllocateMemory (depth image)");
        }
        vkBindImageMemory(mDevice, image, memory, 0);
        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image            = image;
        view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format           = FORMAT;
        view_info.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(mDevice, &view_info, nullptr, &view) != VK_SUCCESS)
        {
            view = VK_NULL_HANDLE;
            return fail("vkCreateImageView (depth)");
        }
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            return fail("vkGetMemoryWin32HandleKHR (depth image)");
        }
        if (!mKMT)
        {
            memory_handle = handle;  // kept open while GL holds the import
        }

        // GL's import of it, and a depth texture of GL's own, made as a shadow map is.
        gl.CreateMemoryObjectsEXT(1, &gl_memory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(gl_memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(gl_memory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.CreateTextures(GL_TEXTURE_2D, 1, &gl_imported);
        gl.TextureParameteri(gl_imported, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(gl_imported, 1, GL_DEPTH_COMPONENT32F, (GLsizei)SIZE, (GLsizei)SIZE, gl_memory, 0);
        gl.CreateTextures(GL_TEXTURE_2D, 1, &gl_own);
        gl.TextureStorage2D(gl_own, 1, GL_DEPTH_COMPONENT32F, (GLsizei)SIZE, (GLsizei)SIZE);
        gl.Finish();
        logGLErrors("importing the shadow map self-test's depth image");

        // A render pass with one depth attachment, kept in GENERAL as every shared image is, cleared to 0 (reverse-Z's
        // far) and stored; a pipeline with no vertex input and no fragment stage.
        VkAttachmentDescription attachment = {};
        attachment.format         = FORMAT;
        attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
        attachment.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference depth_ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depth_ref;
        VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        pass_info.attachmentCount = 1;
        pass_info.pAttachments    = &attachment;
        pass_info.subpassCount    = 1;
        pass_info.pSubpasses      = &subpass;
        if (vkCreateRenderPass(mDevice, &pass_info, nullptr, &pass) != VK_SUCCESS)
        {
            pass = VK_NULL_HANDLE;
            return fail("vkCreateRenderPass (depth only)");
        }
        VkFramebufferCreateInfo fb_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb_info.renderPass      = pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments    = &view;
        fb_info.width           = SIZE;
        fb_info.height          = SIZE;
        fb_info.layers          = 1;
        if (vkCreateFramebuffer(mDevice, &fb_info, nullptr, &framebuffer) != VK_SUCCESS)
        {
            framebuffer = VK_NULL_HANDLE;
            return fail("vkCreateFramebuffer (depth only)");
        }
        VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module_info.codeSize = sizeof(ql_spv_qldepthtest_vert);
        module_info.pCode    = ql_spv_qldepthtest_vert;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &module) != VK_SUCCESS)
        {
            module = VK_NULL_HANDLE;
            return fail("vkCreateShaderModule (depth test)");
        }
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        if (vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &layout) != VK_SUCCESS)
        {
            layout = VK_NULL_HANDLE;
            return fail("vkCreatePipelineLayout (depth test)");
        }
        VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = module;
        stage.pName  = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport = { 0.f, 0.f, (float)SIZE, (float)SIZE, 0.f, 1.f };
        VkRect2D scissor = { { 0, 0 }, { SIZE, SIZE } };
        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.pViewports    = &viewport;
        viewport_state.scissorCount  = 1;
        viewport_state.pScissors     = &scissor;
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp   = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline_info.stageCount          = 1;
        pipeline_info.pStages             = &stage;
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState   = &multisample;
        pipeline_info.pDepthStencilState  = &depth;
        pipeline_info.pColorBlendState    = &blend;
        pipeline_info.layout              = layout;
        pipeline_info.renderPass          = pass;
        pipeline_info.subpass             = 0;
        if (vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
            return fail("vkCreateGraphicsPipelines (depth only)");
        }

        if (!createSharedSemaphore(gl_done_vk, gl_done_gl, gl_done_handle) ||
            !createSharedSemaphore(vk_done_vk, vk_done_gl, vk_done_handle))
        {
            return fail("its semaphores could not be shared");
        }
        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkAllocateCommandBuffers(mDevice, &cmd_info, &cmd) != VK_SUCCESS ||
            vkCreateFence(mDevice, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            return fail("no command buffer");
        }

        // GL hands the image over; Vulkan takes it from the external queue, draws, and gives it back.
        const GLenum general = GL_LAYOUT_GENERAL_EXT;
        gl.SignalSemaphoreEXT(gl_done_gl, 0, nullptr, 1, &gl_imported, &general);
        gl.Flush();
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier acquire = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        acquire.srcAccessMask       = 0;
        acquire.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        acquire.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = mQueueFamily;
        acquire.image               = image;
        acquire.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &acquire);
        VkClearValue clear = {};
        clear.depthStencil = { 0.f, 0 };
        VkRenderPassBeginInfo pass_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        pass_begin.renderPass      = pass;
        pass_begin.framebuffer     = framebuffer;
        pass_begin.renderArea      = scissor;
        pass_begin.clearValueCount = 1;
        pass_begin.pClearValues    = &clear;
        vkCmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        VkImageMemoryBarrier release = acquire;
        release.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        release.dstAccessMask       = 0;
        release.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        release.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = mQueueFamily;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &release);
        vkEndCommandBuffer(cmd);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &gl_done_vk;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &vk_done_vk;
        if (vkQueueSubmit(mQueue, 1, &submit, fence) != VK_SUCCESS)
        {
            return fail("the depth pass could not be submitted");
        }
        vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX);

        // GL takes it back, copies it into its own depth texture, and reads that.
        gl.WaitSemaphoreEXT(vk_done_gl, 0, nullptr, 1, &gl_imported, &general);
        gl.CopyImageSubData(gl_imported, GL_TEXTURE_2D, 0, 0, 0, 0, gl_own, GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)SIZE,
                            (GLsizei)SIZE, 1);
        std::vector<F32> texels(SIZE * SIZE, -1.f);
        gl.GetTextureSubImage(gl_own, 0, 0, 0, 0, (GLsizei)SIZE, (GLsizei)SIZE, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                              (GLsizei)(texels.size() * sizeof(F32)), texels.data());
        logGLErrors("the shadow map self-test");

        // What was drawn at each texel, with Vulkan's row r as GL's row r (the memory is one), and the same turned over,
        // to tell the two apart if the first does not hold.
        constexpr F32 TOLERANCE = 1e-5f;
        U32 bad = 0, bad_flipped = 0;
        F32 worst = 0.f;
        for (U32 r = 0; r < SIZE; ++r)
        {
            for (U32 x = 0; x < SIZE; ++x)
            {
                const F32 u = ((F32)x + 0.5f) / (F32)SIZE;
                const F32 v = ((F32)r + 0.5f) / (F32)SIZE;
                const F32 got = texels[r * SIZE + x];
                const F32 want = 0.1f + 0.4f * u + 0.4f * v;
                const F32 want_flipped = 0.1f + 0.4f * u + 0.4f * (1.f - v);
                const F32 err = std::fabs(got - want);
                worst = llmax(worst, err);
                bad += err > TOLERANCE ? 1 : 0;
                bad_flipped += std::fabs(got - want_flipped) > TOLERANCE ? 1 : 0;
            }
        }

        const U32 total = SIZE * SIZE;
        mDepthPassed = bad == 0;
        if (mDepthPassed)
        {
            mDepthNote = "shadow map hand-over works (self-test passed)";
        }
        else if (bad_flipped == 0)
        {
            mDepthNote = "hand-over works, rows turned over (self-test: GL's row 0 is Vulkan's last)";
        }
        else
        {
            mDepthNote = llformat("failed: %u of %u texels wrong (worst by %g; centre reads %g)", bad, total, worst,
                                  texels[(SIZE / 2) * SIZE + SIZE / 2]);
        }
        LL_INFOS("Vulkan") << "Shadow map self-test, " << SIZE << "x" << SIZE << " D32_SFLOAT through "
                           << (mKMT ? "a KMT" : "an NT") << " handle, drawn by Vulkan, copied by GL: " << (total - bad)
                           << "/" << total << " texels right (worst by " << worst << "), " << (total - bad_flipped)
                           << " if turned over: " << (mDepthPassed ? "passed" : "FAILED") << LL_ENDL;
        cleanup();
        return mDepthPassed;
    }

    // ---- render targets in Vulkan's memory (QuasarVulkanTargets; step 1 of the probes and the main view) --------
    //
    // GL can use memory Vulkan allocates, Vulkan cannot use GL's: a render target both are to draw into or read is
    // given storage from Vulkan's memory. llrender asks (QLExternalStorage, qlexternalstorage.h) as it allocates a
    // target inside a QLExternalStorageScope; the presenter makes a Vulkan image of the target's format, size and
    // levels, exports its memory and has GL take it as the texture's storage (glTextureStorageMem2DEXT). The formats
    // are each tested once first (targetTest: Vulkan clears a small image of the format, GL reads it back), and only
    // those that pass are given. A name GL deletes (LLImageGL's deferred ring, a few frames after its owner let it go)
    // frees its image on the main thread, at the next present.
    struct ExtFormat
    {
        GLenum   gl;
        VkFormat vk;
        U32      channels;  // colour channels read back (0: depth)
        bool     depth;
        bool     stencil;
        bool     srgb;
        const char* name;
    };
    static constexpr ExtFormat EXT_FORMATS[] = {
        { GL_SRGB8_ALPHA8, VK_FORMAT_R8G8B8A8_SRGB, 4, false, false, true, "SRGB8_ALPHA8" },
        { GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4, false, false, false, "RGBA8" },
        { GL_RGB10_A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, false, false, false, "RGB10_A2" },
        { GL_RGBA16, VK_FORMAT_R16G16B16A16_UNORM, 4, false, false, false, "RGBA16" },
        { GL_R11F_G11F_B10F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 3, false, false, false, "R11F_G11F_B10F" },
        { GL_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 4, false, false, false, "RGBA16F" },
        { GL_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 4, false, false, false, "RGBA32F" },
        { GL_RG16F, VK_FORMAT_R16G16_SFLOAT, 2, false, false, false, "RG16F" },
        { GL_RG8, VK_FORMAT_R8G8_UNORM, 2, false, false, false, "RG8" },
        { GL_R8, VK_FORMAT_R8_UNORM, 1, false, false, false, "R8" },
        { GL_DEPTH_COMPONENT32F, VK_FORMAT_D32_SFLOAT, 0, true, false, false, "DEPTH32F" },
        { GL_DEPTH_COMPONENT24, VK_FORMAT_X8_D24_UNORM_PACK32, 0, true, false, false, "DEPTH24" },
        { GL_DEPTH24_STENCIL8, VK_FORMAT_D24_UNORM_S8_UINT, 0, true, true, false, "DEPTH24_STENCIL8" },
        { GL_DEPTH32F_STENCIL8, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, true, true, false, "DEPTH32F_STENCIL8" },
    };
    static constexpr U32 EXT_FORMAT_COUNT = (U32)(sizeof(EXT_FORMATS) / sizeof(EXT_FORMATS[0]));

    struct ExtTexture
    {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        HANDLE         handle = nullptr;
        GLuint         glMemory = 0;
        U32            format = 0;  // index into EXT_FORMATS
        U32            width = 0, height = 0, levels = 0;
    };

    // What llrender talks to: allocation on the main thread, deletions from any (queued for the main thread).
    class ExtStorage : public QLExternalStorage
    {
    public:
        explicit ExtStorage(Impl& owner) : mOwner(owner) {}
        bool allocate(unsigned texture, unsigned target, int internal_format, int width, int height,
                      int levels) override
        {
            return mOwner.extAllocate(texture, target, internal_format, width, height, levels);
        }
        void deleted(int count, const unsigned* textures) override
        {
            std::lock_guard<std::mutex> lock(mOwner.mExtMutex);
            for (int i = 0; i < count; ++i)
            {
                if (mOwner.mExtTextures.count(textures[i]))
                {
                    mOwner.mExtDeleted.push_back(textures[i]);
                }
            }
        }
        bool owns(unsigned texture) const override
        {
            std::lock_guard<std::mutex> lock(mOwner.mExtMutex);
            return mOwner.mExtTextures.count(texture) != 0;
        }

    private:
        Impl& mOwner;
    };

    mutable std::mutex mExtMutex;
    std::unordered_map<GLuint, ExtTexture> mExtTextures;
    std::vector<GLuint> mExtDeleted;
    bool        mExtFormatOK[EXT_FORMAT_COUNT] = {};
    bool        mExtTested = false;
    bool        mExtPassed = false;  // at least the G-buffer's formats passed
    std::string mExtNote = "not tested yet (QuasarVulkanTargets)";
    std::unique_ptr<ExtStorage> mExtStorage;
    U64         mExtBytes = 0;

    static S32 extFormatIndex(GLenum gl_format)
    {
        for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
        {
            if (EXT_FORMATS[i].gl == gl_format)
            {
                return (S32)i;
            }
        }
        return -1;
    }

    // Storage for texture from Vulkan's memory: an image of the format, exported, imported by GL as the texture's
    // storage. False, with nothing left made, where it cannot be; GL then allocates as before.
    bool extAllocate(GLuint texture, GLenum target, GLint internal_format, GLsizei width, GLsizei height,
                     GLsizei levels, bool tested_only = true)
    {
        if (!mActive || !mDevice || target != GL_TEXTURE_2D || width <= 0 || height <= 0 || levels <= 0)
        {
            return false;
        }
        const S32 index = extFormatIndex((GLenum)internal_format);
        if (index < 0 || (tested_only && !mExtFormatOK[index]))
        {
            return false;
        }
        const ExtFormat& format = EXT_FORMATS[index];
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, format.vk, &props);
        const VkFormatFeatureFlags needed = format.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                         : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if ((props.optimalTilingFeatures & needed) != needed)
        {
            return false;
        }
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        ExtTexture ext;
        ext.format = (U32)index;
        ext.width  = (U32)width;
        ext.height = (U32)height;
        ext.levels = (U32)levels;
        auto undo = [&]()
        {
            if (ext.image) vkDestroyImage(mDevice, ext.image, nullptr);
            if (ext.memory) vkFreeMemory(mDevice, ext.memory, nullptr);
            if (ext.handle) CloseHandle(ext.handle);
            return false;
        };
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format.vk;
        info.extent        = { (U32)width, (U32)height, 1 };
        info.mipLevels     = (U32)levels;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             (format.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(mDevice, &info, nullptr, &ext.image) != VK_SUCCESS)
        {
            ext.image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, ext.image, &req);
        const U32 device_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (device_type == U32_MAX)
        {
            return undo();
        }
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = ext.image;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = device_type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &ext.memory) != VK_SUCCESS)
        {
            ext.memory = VK_NULL_HANDLE;
            return undo();
        }
        vkBindImageMemory(mDevice, ext.image, ext.memory, 0);
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = ext.memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            return undo();
        }
        if (!mKMT)
        {
            ext.handle = handle;  // kept open while GL holds the import (a threaded driver imports on its own thread)
        }
        while (gl.GetError() != GL_NO_ERROR)
        {
        }
        gl.CreateMemoryObjectsEXT(1, &ext.glMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(ext.glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(ext.glMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.TextureParameteri(texture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(texture, levels, (GLenum)internal_format, width, height, ext.glMemory, 0);
        // Asked once an allocation, off the frame's path (render targets are made as the render inputs change).
        const GLenum error = gl.GetError();
        if (error != GL_NO_ERROR)
        {
            LL_WARNS("Vulkan") << "Vulkan memory for a " << width << "x" << height << " " << format.name
                               << " texture: GL would not take it (0x" << std::hex << error << std::dec << ")" << LL_ENDL;
            gl.DeleteMemoryObjectsEXT(1, &ext.glMemory);
            ext.glMemory = 0;
            return undo();
        }
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            mExtTextures[texture] = ext;
        }
        mExtBytes += req.size;
        return true;
    }

    void extFree(ExtTexture& ext)
    {
        if (ext.glMemory) gl.DeleteMemoryObjectsEXT(1, &ext.glMemory);
        if (ext.image) vkDestroyImage(mDevice, ext.image, nullptr);
        if (ext.memory) vkFreeMemory(mDevice, ext.memory, nullptr);
        if (ext.handle) CloseHandle(ext.handle);
        ext = ExtTexture();
    }

    // The images of the names GL has deleted, on the main thread (each present).
    void extCollect()
    {
        std::vector<GLuint> deleted;
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            deleted.swap(mExtDeleted);
        }
        for (GLuint name : deleted)
        {
            ExtTexture ext;
            {
                std::lock_guard<std::mutex> lock(mExtMutex);
                auto found = mExtTextures.find(name);
                if (found == mExtTextures.end())
                {
                    continue;
                }
                ext = found->second;
                mExtTextures.erase(found);
            }
            VkMemoryRequirements req = {};
            vkGetImageMemoryRequirements(mDevice, ext.image, &req);
            mExtBytes -= llmin(mExtBytes, (U64)req.size);
            extFree(ext);
        }
    }

    // Each format, once: a small texture of it given Vulkan's memory, cleared by Vulkan to a known value, read back by
    // GL. The formats that pass are the ones given from then on; llrender is offered the storage once the G-buffer's
    // own pass (the sRGB albedo, RGBA8, a normals format and a depth-stencil format).
    bool targetTest()
    {
        if (mExtTested)
        {
            return mExtPassed;
        }
        mExtTested = true;
        if (!mActive || !mDevice)
        {
            mExtNote = "not tested: Vulkan does not present";
            return false;
        }
        LL_PROFILE_ZONE_NAMED("vulkan render target self-test");
        constexpr U32 SIZE = 16;
        constexpr F32 CLEAR[4] = { 0.25f, 0.5f, 0.75f, 1.f };
        constexpr F32 CLEAR_DEPTH = 0.5f;

        GLuint textures[EXT_FORMAT_COUNT] = {};
        bool made[EXT_FORMAT_COUNT] = {};
        std::vector<GLuint> made_names;
        for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
        {
            gl.CreateTextures(GL_TEXTURE_2D, 1, &textures[i]);
            made[i] = extAllocate(textures[i], GL_TEXTURE_2D, (GLint)EXT_FORMATS[i].gl, SIZE, SIZE, 1, false);
            if (made[i])
            {
                made_names.push_back(textures[i]);
            }
        }
        VkSemaphore gl_done_vk = VK_NULL_HANDLE, vk_done_vk = VK_NULL_HANDLE;
        GLuint gl_done_gl = 0, vk_done_gl = 0;
        HANDLE gl_done_handle = nullptr, vk_done_handle = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        auto cleanup = [&]()
        {
            if (fence) vkDestroyFence(mDevice, fence, nullptr);
            if (cmd) vkFreeCommandBuffers(mDevice, mCommandPool, 1, &cmd);
            if (gl_done_gl) gl.DeleteSemaphoresEXT(1, &gl_done_gl);
            if (vk_done_gl) gl.DeleteSemaphoresEXT(1, &vk_done_gl);
            if (gl_done_vk) vkDestroySemaphore(mDevice, gl_done_vk, nullptr);
            if (vk_done_vk) vkDestroySemaphore(mDevice, vk_done_vk, nullptr);
            if (gl_done_handle) CloseHandle(gl_done_handle);
            if (vk_done_handle) CloseHandle(vk_done_handle);
            gl.Finish();
            for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
            {
                if (textures[i])
                {
                    gl.DeleteTextures(1, &textures[i]);
                }
                ExtTexture ext;
                bool had = false;
                {
                    std::lock_guard<std::mutex> lock(mExtMutex);
                    auto found = mExtTextures.find(textures[i]);
                    if (found != mExtTextures.end())
                    {
                        ext = found->second;
                        mExtTextures.erase(found);
                        had = true;
                    }
                }
                if (had)
                {
                    VkMemoryRequirements req = {};
                    vkGetImageMemoryRequirements(mDevice, ext.image, &req);
                    mExtBytes -= llmin(mExtBytes, (U64)req.size);
                    extFree(ext);
                }
            }
        };
        auto fail = [&](const std::string& why)
        {
            mExtNote = "failed: " + why;
            LL_WARNS("Vulkan") << "Render target self-test " << mExtNote << LL_ENDL;
            vkDeviceWaitIdle(mDevice);
            cleanup();
            return false;
        };
        if (made_names.empty())
        {
            return fail("no format could be given Vulkan's memory");
        }
        if (!createSharedSemaphore(gl_done_vk, gl_done_gl, gl_done_handle) ||
            !createSharedSemaphore(vk_done_vk, vk_done_gl, vk_done_handle))
        {
            return fail("its semaphores could not be shared");
        }
        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkAllocateCommandBuffers(mDevice, &cmd_info, &cmd) != VK_SUCCESS ||
            vkCreateFence(mDevice, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            return fail("no command buffer");
        }

        // GL hands them all over; Vulkan clears each and gives them back.
        const std::vector<GLenum> layouts(made_names.size(), GL_LAYOUT_GENERAL_EXT);
        gl.SignalSemaphoreEXT(gl_done_gl, 0, nullptr, (GLuint)made_names.size(), made_names.data(), layouts.data());
        gl.Flush();
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
        {
            if (!made[i])
            {
                continue;
            }
            const ExtFormat& format = EXT_FORMATS[i];
            VkImage image = VK_NULL_HANDLE;
            {
                std::lock_guard<std::mutex> lock(mExtMutex);
                image = mExtTextures[textures[i]].image;
            }
            const VkImageAspectFlags aspect = format.depth
                ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (format.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageMemoryBarrier acquire = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            acquire.srcAccessMask       = 0;
            acquire.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            acquire.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            acquire.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            acquire.dstQueueFamilyIndex = mQueueFamily;
            acquire.image               = image;
            acquire.subresourceRange    = { aspect, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &acquire);
            const VkImageSubresourceRange range = { aspect, 0, 1, 0, 1 };
            if (format.depth)
            {
                const VkClearDepthStencilValue value = { CLEAR_DEPTH, 0x5A };
                vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            }
            else
            {
                VkClearColorValue value = {};
                memcpy(value.float32, CLEAR, sizeof(CLEAR));
                vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            }
            VkImageMemoryBarrier release = acquire;
            release.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            release.dstAccessMask       = 0;
            release.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            release.srcQueueFamilyIndex = mQueueFamily;
            release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &release);
        }
        vkEndCommandBuffer(cmd);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &gl_done_vk;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &vk_done_vk;
        if (vkQueueSubmit(mQueue, 1, &submit, fence) != VK_SUCCESS)
        {
            return fail("the clears could not be submitted");
        }
        vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX);
        gl.WaitSemaphoreEXT(vk_done_gl, 0, nullptr, (GLuint)made_names.size(), made_names.data(), layouts.data());

        // Each read back where GL's texel (8, 8) is: colour as floats, depth as a float. An sRGB format may come back
        // as stored or decoded: either passes.
        std::string results;
        U32 passed = 0;
        for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
        {
            const ExtFormat& format = EXT_FORMATS[i];
            bool ok = false;
            std::string got;
            if (made[i])
            {
                if (format.depth)
                {
                    F32 depth = -1.f;
                    gl.GetTextureSubImage(textures[i], 0, (GLint)SIZE / 2, (GLint)SIZE / 2, 0, 1, 1, 1,
                                          GL_DEPTH_COMPONENT, GL_FLOAT, sizeof(depth), &depth);
                    ok = std::fabs(depth - CLEAR_DEPTH) < 1e-3f;
                    got = llformat("%.4f", depth);
                }
                else
                {
                    F32 rgba[4] = { -1.f, -1.f, -1.f, -1.f };
                    gl.GetTextureSubImage(textures[i], 0, (GLint)SIZE / 2, (GLint)SIZE / 2, 0, 1, 1, 1, GL_RGBA,
                                          GL_FLOAT, sizeof(rgba), rgba);
                    auto close = [&](const F32* want, F32 tolerance)
                    {
                        for (U32 c = 0; c < format.channels; ++c)
                        {
                            if (std::fabs(rgba[c] - want[c]) > tolerance)
                            {
                                return false;
                            }
                        }
                        return true;
                    };
                    // The clear's linear values as an sRGB format stores them.
                    auto encode = [](F32 v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f; };
                    const F32 encoded[4] = { encode(CLEAR[0]), encode(CLEAR[1]), encode(CLEAR[2]), CLEAR[3] };
                    ok = close(CLEAR, 0.01f) || (format.srgb && close(encoded, 0.01f));
                    got = llformat("%.3f %.3f %.3f %.3f", rgba[0], rgba[1], rgba[2], rgba[3]);
                }
            }
            mExtFormatOK[i] = ok;
            passed += ok ? 1 : 0;
            results += llformat("%s%s %s", results.empty() ? "" : ", ", format.name,
                                !made[i] ? "not made" : (ok ? "ok" : ("WRONG (" + got + ")").c_str()));
        }
        logGLErrors("the render target self-test");

        auto ok = [&](GLenum f) { const S32 i = extFormatIndex(f); return i >= 0 && mExtFormatOK[i]; };
        mExtPassed = ok(GL_SRGB8_ALPHA8) && ok(GL_RGBA8) && (ok(GL_RGB10_A2) || ok(GL_RGBA16)) &&
                     (ok(GL_DEPTH24_STENCIL8) || ok(GL_DEPTH32F_STENCIL8));
        mExtNote = llformat("%s, %u of %u formats", mExtPassed ? "self-test passed" : "self-test FAILED", passed,
                            EXT_FORMAT_COUNT);
        LL_INFOS("Vulkan") << "Render target self-test (Vulkan's memory, cleared by Vulkan, read by GL): " << results
                           << ": " << (mExtPassed ? "passed" : "FAILED") << LL_ENDL;
        cleanup();
        if (mExtPassed)
        {
            mExtStorage = std::make_unique<ExtStorage>(*this);
            gQLExternalStorage = mExtStorage.get();
        }
        return mExtPassed;
    }

    std::string targetInfo() const
    {
        if (!mExtPassed)
        {
            return "targets: " + mExtNote;
        }
        std::lock_guard<std::mutex> lock(mExtMutex);
        return llformat("targets: %s, %u in Vulkan's memory (%.0f MB)", mExtNote.c_str(), (U32)mExtTextures.size(),
                        (double)mExtBytes / (1 << 20));
    }

    // ---- step 2 of geometry in Vulkan: shadow casters drawn by Vulkan (QuasarVulkanShadows) ---------------------
    //
    // A shadow map's unrigged, opaque casters, which GL drew one call each, drawn by Vulkan in one indirect draw:
    //   - Their meshes, positions and indices, are copied by GL (glCopyNamedBufferSubData, on the GPU) into an arena: a
    //     buffer Vulkan allocates and GL imports, positions in its first part, 16 bytes a vertex as LLVertexBuffer keeps
    //     them, 16-bit indices in its second. A mesh is copied once and found again by its content stamp, which every
    //     allocation, map and write of an LLVertexBuffer takes anew; the arena starts over when it fills.
    //   - Each draw is a model-view-projection matrix and an indirect command, written into memory the CPU maps, a ring
    //     of slots, one a shadow map; the vertex shader reads the draw's matrix by its instance (qlshadow.vert).
    //   - The map itself: GL copies its depth (glCopyImageSubData) into a depth image Vulkan allocates and GL imports, of
    //     the map's size; Vulkan draws over it (load, no clear, so whatever GL had there stays: a kept still map, what GL
    //     drew before); GL copies it back and draws the rest (rigged, alpha-masked, terrain, trees) on top as before.
    //     Each side waits for the other on the GPU through two semaphores of their own, never on the CPU.
    //   - Rigged casters (step 4: avatars, rigged mesh) go into an arena of their own, which keeps each vertex's joint
    //     weights beside its position (same index, the second half), and each draw names its skeleton: the avatar's
    //     palette for the skin, written once a frame into a ring every map of the frame reads (qlshadowskin.vert).
    //     A map draws its unrigged casters, then its rigged ones: two indirect draws.
    //   - Alpha-masked casters (step 3: hair, leaves, cut-out clothes), unrigged and rigged, after those: arenas of their
    //     own that keep each vertex's texture coordinate and colour beside its position (and weights, rigged), and each
    //     draw's texture as a tile of an alpha atlas, an RGBA8 image Vulkan allocates and GL imports and fills: GL blits
    //     a texture into a tile the first time a map draws it (by its GL name and content stamp), from the mip level
    //     nearest the tile's size. Each draw carries its tile, its texture matrix, its cutoff and which of GL's alpha
    //     tests it takes (qlshadowmask.vert, qlshadowmaskskin.vert, qlshadowmask.frag). Up to four indirect draws a map.
    static constexpr U32          SH_SLOTS = 96;  // ~34 maps a frame at most (probe faces, hero faces, cascades, spots), times the pacing depth
    static constexpr U32          SH_MAX_DRAWS = 8192;
    static constexpr VkDeviceSize SH_MVP_BYTES = (VkDeviceSize)SH_MAX_DRAWS * 64;
    static constexpr VkDeviceSize SH_CMD_BYTES = (VkDeviceSize)SH_MAX_DRAWS * sizeof(VkDrawIndexedIndirectCommand);
    static constexpr VkDeviceSize SH_PALBASE_BYTES = (VkDeviceSize)SH_MAX_DRAWS * 4;
    static constexpr VkDeviceSize SH_MASK_BYTES = (VkDeviceSize)SH_MAX_DRAWS * 48;  // three vec4s a draw (qlshadowmask.vert)
    static constexpr VkDeviceSize SH_SLOT_BYTES =
        ((SH_MVP_BYTES + SH_CMD_BYTES + SH_PALBASE_BYTES + SH_MASK_BYTES) + 255) & ~(VkDeviceSize)255;
    static_assert((SH_MVP_BYTES + SH_CMD_BYTES) % 256 == 0, "each slot's palette bases start on a storage offset");
    static_assert((SH_MVP_BYTES + SH_CMD_BYTES + SH_PALBASE_BYTES) % 256 == 0, "each slot's masks start on a storage offset");
    static constexpr VkDeviceSize SH_MASK_START = SH_MVP_BYTES + SH_CMD_BYTES + SH_PALBASE_BYTES;
    // The unrigged arena: positions, then indices. The rigged one: positions, weights beside them, then indices.
    static constexpr VkDeviceSize SH_ARENA_VERTEX_BYTES = 192ull << 20;
    static constexpr VkDeviceSize SH_ARENA_INDEX_BYTES = 64ull << 20;
    static constexpr VkDeviceSize SH_RIGGED_VERTEX_BYTES = 128ull << 20;
    static constexpr VkDeviceSize SH_RIGGED_INDEX_BYTES = 48ull << 20;
    // The masked arenas: positions, (weights,) texture coordinates (8 bytes a vertex), colours (4), then indices.
    static constexpr VkDeviceSize SH_MASKED_VERTEX_BYTES = 64ull << 20;
    static constexpr VkDeviceSize SH_MASKED_INDEX_BYTES = 32ull << 20;
    static constexpr VkDeviceSize SH_MASKED_RIGGED_VERTEX_BYTES = 96ull << 20;
    static constexpr VkDeviceSize SH_MASKED_RIGGED_INDEX_BYTES = 64ull << 20;
    // The alpha atlas: 4096 square, tiles of 128, 1024 of them (qlshadowmask.frag's ATLAS_SIZE).
    static constexpr U32          SH_ATLAS_SIZE = 4096;
    static constexpr U32          SH_TILE = 128;
    static constexpr U32          SH_TILES_ACROSS = SH_ATLAS_SIZE / SH_TILE;
    static constexpr U32          SH_TILES = SH_TILES_ACROSS * SH_TILES_ACROSS;
    // The kinds of draw, in the order a map's draws are written.
    enum : U32 { SH_KIND_STATIC = 0, SH_KIND_RIGGED, SH_KIND_MASKED, SH_KIND_MASKED_RIGGED, SH_KINDS };
    // A palette as LLVOAvatar packs it for GL: the origin, then three columns a joint, each a vec4; a slot of the ring
    // holds the most a mesh can have. The ring is a segment a frame, four frames round: what the GPU reads of a frame is
    // done three frames on, by the pacing depth GL's frame fences keep.
    static constexpr U32          SH_PALETTE_VEC4S = 1 + 3 * 110;
    static constexpr VkDeviceSize SH_PALETTE_BYTES = (VkDeviceSize)SH_PALETTE_VEC4S * 16;
    static constexpr U32          SH_PALETTES_PER_FRAME = 1536;
    static constexpr U32          SH_PALETTE_SEGMENTS = 4;
    static constexpr VkDeviceSize SH_PALETTE_SEGMENT_BYTES = SH_PALETTE_BYTES * SH_PALETTES_PER_FRAME;

    struct ShArenaEntry
    {
        U32 mBaseVertex = 0;
        U32 mBaseIndex = 0;
    };
    // A buffer Vulkan allocates and GL imports, positions (16 bytes a vertex), weights beside them where it keeps any
    // (the same index, from vertexBytes on), texture coordinates (8 bytes) and colours (4) after those where it keeps
    // them, then 16-bit indices; filled by GL's copies, found again by content stamp.
    struct ShArena
    {
        VkBuffer       buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        HANDLE         handle = nullptr;
        GLuint         glMemory = 0, gl = 0;
        VkDeviceSize   vertexBytes = 0, weightBytes = 0, uvBytes = 0, colorBytes = 0, indexBytes = 0;
        VkDeviceSize   vertexTop = 0, indexTop = 0;
        U32            resets = 0;
        std::unordered_map<U64, ShArenaEntry> entries;

        VkDeviceSize uvStart() const { return vertexBytes + weightBytes; }
        VkDeviceSize colorStart() const { return uvStart() + uvBytes; }
        VkDeviceSize indexStart() const { return colorStart() + colorBytes; }
        VkDeviceSize totalBytes() const { return indexStart() + indexBytes; }
        VkDeviceSize used() const
        {
            return vertexTop * (weightBytes ? 2 : 1) + (uvBytes ? vertexTop / 2 + vertexTop / 4 : 0) + indexTop;
        }
    };
    // A texture's place in the alpha atlas: which texture, as it held what (its content stamp), and the last map that
    // drew with it (a tile drawn with in the map being collected is never given to another texture).
    struct ShTile
    {
        GLuint texture = 0;
        U64    stamp = 0;
        U64    lastPass = 0;
    };
    struct ShDepth
    {
        GLuint         glMap = 0;  // the GL map this image stands in for
        U32            width = 0, height = 0;
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        HANDLE         handle = nullptr;
        VkImageView    view = VK_NULL_HANDLE;
        VkFramebuffer  framebuffer = VK_NULL_HANDLE;
        GLuint         glMemory = 0, glTexture = 0;
        U32            lastFrame = 0;
    };
    // A map's draws: its matrices, commands, palette bases and masks in the host ring, and its descriptor set. A
    // hand-over (shadowFlush) carries every map queued since the last in one submission, recorded into the first
    // map's command buffer and fenced by its fence.
    struct ShSlot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool            submitted = false;  // its fence is a submission's, not yet seen signalled
        bool            pending = false;    // queued for the next hand-over
        U64             serial = 0;         // which submission this slot last went in
    };
    // A map queued for the next hand-over: what shadowEnd knew of it.
    struct ShPending
    {
        U32    slot = 0;
        GLuint depth = 0;
        U32    width = 0, height = 0;
        U32    variant = 0;
        bool   remap = false;
        U32    kindFirst[SH_KINDS + 1] = {};
    };

    static inline bool sShSignalObjects = true;
    bool        mShTried = false;
    bool        mShReady = false;
    std::string mShNote = "off (QuasarVulkanShadows)";

    ShArena        mShStatic;
    ShArena        mShRigged;

    // The masked casters (step 3): their arenas, the atlas and what GL fills it through; made after the rest, and
    // where they cannot be, the masked casters stay GL's and the rest goes on (mShMaskNote says why).
    ShArena        mShMasked;
    ShArena        mShMaskedRigged;
    bool           mShMaskReady = false;
    std::string    mShMaskNote = "not made yet";
    VkImage        mShAtlas = VK_NULL_HANDLE;
    VkDeviceMemory mShAtlasMemory = VK_NULL_HANDLE;
    HANDLE         mShAtlasHandle = nullptr;
    VkImageView    mShAtlasView = VK_NULL_HANDLE;
    VkSampler      mShAtlasSampler = VK_NULL_HANDLE;
    GLuint         mShAtlasGLMemory = 0, mShAtlasGL = 0;
    GLuint         mShReadFbo = 0, mShAtlasFbo = 0;  // a caster's texture, and the atlas: a blit from one to the other
    std::vector<ShTile> mShTiles;
    std::map<std::pair<GLuint, U64>, U32> mShTileOf;
    U64            mShPassSerial = 0;
    U64            mShPassAtFlush = 0;  // the last map handed over: a tile drawn with since is a queued map's
    U32            mShTilesTaken = 0;  // tiles given out so far; once all are, the least recently drawn goes
    U32            mShTileUploadsNow = 0, mShTileUploadsLast = 0;
    U32            mShMaskedNow = 0, mShMaskedLast = 0;
    U32            mShMaskedRiggedNow = 0, mShMaskedRiggedLast = 0;
    VkShaderModule mShMaskModule = VK_NULL_HANDLE, mShMaskSkinModule = VK_NULL_HANDLE, mShMaskFragModule = VK_NULL_HANDLE;
    VkPipeline     mShMaskPipelines[4] = {};      // [clamp][greater]
    VkPipeline     mShMaskSkinPipelines[4] = {};

    // The palettes: a host-visible ring Vulkan reads, a segment a frame; this frame's, found by avatar and skin.
    VkBuffer       mShPalettes = VK_NULL_HANDLE;
    VkDeviceMemory mShPalettesMemory = VK_NULL_HANDLE;
    U8*            mShPalettesMapped = nullptr;
    U32            mShPaletteFrame = U32_MAX;
    U32            mShPaletteCount = 0, mShPaletteCountLast = 0;
    bool           mShPaletteSegmentFree = false;  // this frame's segment is the GPU's no longer
    std::map<std::pair<U64, U64>, U32> mShPaletteBases;  // (avatar, skin) to the vec4 its palette starts at
    // Each segment's last submission: a segment is written again only once every map that read it is done.
    U64            mShSegmentSerial[SH_PALETTE_SEGMENTS] = {};
    U64            mShSubmitSerial = 0;
    U64            mShDoneSerial = 0;  // every submission up to this one is done (one queue: they finish in order)
    std::vector<ShPending> mShPending;
    U32            mShFlushesNow = 0, mShFlushesLast = 0;

    VkBuffer       mShHost = VK_NULL_HANDLE;
    VkDeviceMemory mShHostMemory = VK_NULL_HANDLE;
    U8*            mShHostMapped = nullptr;
    ShSlot         mShSlots[SH_SLOTS];
    U32            mShNextSlot = 0;

    VkDescriptorSetLayout mShSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      mShDescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout      mShLayout = VK_NULL_HANDLE;
    VkShaderModule        mShModule = VK_NULL_HANDLE;
    VkRenderPass          mShPass = VK_NULL_HANDLE;
    VkShaderModule        mShSkinModule = VK_NULL_HANDLE;
    VkPipeline            mShPipelines[4] = {};  // [clamp][greater]
    VkPipeline            mShSkinPipelines[4] = {};
    std::vector<ShDepth>  mShDepths;

    VkSemaphore mShGLDoneVK = VK_NULL_HANDLE, mShVKDoneVK = VK_NULL_HANDLE;
    GLuint      mShGLDoneGL = 0, mShVKDoneGL = 0;
    HANDLE      mShGLDoneHandle = nullptr, mShVKDoneHandle = nullptr;

    // The pass being collected.
    bool     mShInPass = false;
    U32      mShSlot = 0;
    U32      mShDraws = 0;
    U32      mShKind = SH_KIND_STATIC;  // the kind of the draws being written; a map's draws come kind by kind
    U32      mShKindFirst[SH_KINDS + 1] = {};
    U32      mShWidth = 0, mShHeight = 0;
    bool     mShClamp = false, mShGreater = false, mShRemapDepth = false;

    // For the status line: this frame's so far, and the last whole frame's.
    U32      mShFrame = 0;
    U32      mShPassesNow = 0, mShDrawsNow = 0, mShPassesLast = 0, mShDrawsLast = 0;
    U32      mShRiggedNow = 0, mShRiggedLast = 0;
    U64      mShCopiedNow = 0, mShCopiedLast = 0;

    bool shFail(const std::string& why)
    {
        mShNote = "failed: " + why;
        LL_WARNS("Vulkan") << "Shadows in Vulkan " << mShNote << LL_ENDL;
        destroyShadowObjects();
        mShReady = false;
        return false;
    }

    // Everything the shadow casters need, made the first time they are wanted. False, with why in mShNote, where it
    // cannot be; then nothing is left made and it is not tried again.
    bool createShadowObjects()
    {
        if (mShTried)
        {
            return mShReady;
        }
        mShTried = true;
        if (!mActive || !mDevice)
        {
            mShNote = "needs Vulkan presentation";
            return false;
        }
        if (!mDepthPassed)
        {
            mShNote = "waits for the shadow map self-test to pass";
            mShTried = false;  // tried again once it has
            return false;
        }

        if (!createArena(mShStatic, SH_ARENA_VERTEX_BYTES, false, false, SH_ARENA_INDEX_BYTES, "unrigged arena") ||
            !createArena(mShRigged, SH_RIGGED_VERTEX_BYTES, true, false, SH_RIGGED_INDEX_BYTES, "rigged arena"))
        {
            return false;
        }
        VkMemoryRequirements req = {};

        // The ring the CPU writes each map's draws into.
        VkBufferCreateInfo host_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        host_info.size        = SH_SLOT_BYTES * SH_SLOTS;
        host_info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        host_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &host_info, nullptr, &mShHost) != VK_SUCCESS)
        {
            mShHost = VK_NULL_HANDLE;
            return shFail("vkCreateBuffer (draws)");
        }
        vkGetBufferMemoryRequirements(mDevice, mShHost, &req);
        const U32 host_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo host_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        host_alloc.allocationSize  = req.size;
        host_alloc.memoryTypeIndex = host_type;
        if (host_type == U32_MAX || vkAllocateMemory(mDevice, &host_alloc, nullptr, &mShHostMemory) != VK_SUCCESS)
        {
            mShHostMemory = VK_NULL_HANDLE;
            return shFail("no host memory for the draws");
        }
        vkBindBufferMemory(mDevice, mShHost, mShHostMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(mDevice, mShHostMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            return shFail("vkMapMemory (draws)");
        }
        mShHostMapped = static_cast<U8*>(mapped);

        // The palettes' ring.
        VkBufferCreateInfo pal_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        pal_info.size        = SH_PALETTE_SEGMENT_BYTES * SH_PALETTE_SEGMENTS;
        pal_info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        pal_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &pal_info, nullptr, &mShPalettes) != VK_SUCCESS)
        {
            mShPalettes = VK_NULL_HANDLE;
            return shFail("vkCreateBuffer (palettes)");
        }
        vkGetBufferMemoryRequirements(mDevice, mShPalettes, &req);
        const U32 pal_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo pal_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        pal_alloc.allocationSize  = req.size;
        pal_alloc.memoryTypeIndex = pal_type;
        if (pal_type == U32_MAX || vkAllocateMemory(mDevice, &pal_alloc, nullptr, &mShPalettesMemory) != VK_SUCCESS)
        {
            mShPalettesMemory = VK_NULL_HANDLE;
            return shFail("no host memory for the palettes");
        }
        vkBindBufferMemory(mDevice, mShPalettes, mShPalettesMemory, 0);
        mapped = nullptr;
        if (vkMapMemory(mDevice, mShPalettesMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            return shFail("vkMapMemory (palettes)");
        }
        mShPalettesMapped = static_cast<U8*>(mapped);

        // The descriptor set of each slot: the slot's matrices, its palette bases, the palettes' ring, the slot's masks,
        // and the alpha atlas (written once the masked casters' objects are made).
        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (U32 b = 0; b < 5; ++b)
        {
            bindings[b].binding         = b;
            bindings[b].descriptorType  = b == 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags      = b == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 5;
        set_info.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mShSetLayout) != VK_SUCCESS)
        {
            mShSetLayout = VK_NULL_HANDLE;
            return shFail("vkCreateDescriptorSetLayout (shadows)");
        }
        const VkDescriptorPoolSize pool_sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SH_SLOTS * 4 },
                                                     { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SH_SLOTS } };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets       = SH_SLOTS;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes    = pool_sizes;
        if (vkCreateDescriptorPool(mDevice, &pool_info, nullptr, &mShDescriptorPool) != VK_SUCCESS)
        {
            mShDescriptorPool = VK_NULL_HANDLE;
            return shFail("vkCreateDescriptorPool (shadows)");
        }
        VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount         = 1;
        layout_info.pSetLayouts            = &mShSetLayout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &push;
        if (vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &mShLayout) != VK_SUCCESS)
        {
            mShLayout = VK_NULL_HANDLE;
            return shFail("vkCreatePipelineLayout (shadows)");
        }

        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        for (U32 i = 0; i < SH_SLOTS; ++i)
        {
            ShSlot& slot = mShSlots[i];
            VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkAllocateCommandBuffers(mDevice, &cmd_info, &slot.cmd) != VK_SUCCESS ||
                vkCreateFence(mDevice, &fence_info, nullptr, &slot.fence) != VK_SUCCESS)
            {
                return shFail("no command buffers for the shadows");
            }
            VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            set_alloc.descriptorPool     = mShDescriptorPool;
            set_alloc.descriptorSetCount = 1;
            set_alloc.pSetLayouts        = &mShSetLayout;
            if (vkAllocateDescriptorSets(mDevice, &set_alloc, &slot.set) != VK_SUCCESS)
            {
                return shFail("vkAllocateDescriptorSets (shadows)");
            }
            const VkDescriptorBufferInfo buffers[4] = {
                { mShHost, SH_SLOT_BYTES * i, SH_MVP_BYTES },
                { mShHost, SH_SLOT_BYTES * i + SH_MVP_BYTES + SH_CMD_BYTES, SH_PALBASE_BYTES },
                { mShPalettes, 0, SH_PALETTE_SEGMENT_BYTES * SH_PALETTE_SEGMENTS },
                { mShHost, SH_SLOT_BYTES * i + SH_MASK_START, SH_MASK_BYTES },
            };
            VkWriteDescriptorSet writes[4] = {};
            for (U32 b = 0; b < 4; ++b)
            {
                writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet          = slot.set;
                writes[b].dstBinding      = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].pBufferInfo     = &buffers[b];
            }
            vkUpdateDescriptorSets(mDevice, 4, writes, 0, nullptr);
        }

        // The render pass: a depth attachment loaded and stored, GENERAL throughout as every shared image is kept.
        VkAttachmentDescription attachment = {};
        attachment.format         = VK_FORMAT_D32_SFLOAT;
        attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
        attachment.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference depth_ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depth_ref;
        VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        pass_info.attachmentCount = 1;
        pass_info.pAttachments    = &attachment;
        pass_info.subpassCount    = 1;
        pass_info.pSubpasses      = &subpass;
        if (vkCreateRenderPass(mDevice, &pass_info, nullptr, &mShPass) != VK_SUCCESS)
        {
            mShPass = VK_NULL_HANDLE;
            return shFail("vkCreateRenderPass (shadows)");
        }
        VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module_info.codeSize = sizeof(ql_spv_qlshadow_vert);
        module_info.pCode    = ql_spv_qlshadow_vert;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mShModule) != VK_SUCCESS)
        {
            mShModule = VK_NULL_HANDLE;
            return shFail("vkCreateShaderModule (shadows)");
        }
        module_info.codeSize = sizeof(ql_spv_qlshadowskin_vert);
        module_info.pCode    = ql_spv_qlshadowskin_vert;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mShSkinModule) != VK_SUCCESS)
        {
            mShSkinModule = VK_NULL_HANDLE;
            return shFail("vkCreateShaderModule (rigged shadows)");
        }
        for (U32 clamp = 0; clamp < 2; ++clamp)
        {
            for (U32 greater = 0; greater < 2; ++greater)
            {
                if (clamp && !mHasDepthClamp)
                {
                    continue;
                }
                if (!createShadowPipeline(clamp != 0, greater != 0, false, false, mShPipelines[clamp * 2 + greater]) ||
                    !createShadowPipeline(clamp != 0, greater != 0, true, false, mShSkinPipelines[clamp * 2 + greater]))
                {
                    return false;
                }
            }
        }

        if (!createSharedSemaphore(mShGLDoneVK, mShGLDoneGL, mShGLDoneHandle) ||
            !createSharedSemaphore(mShVKDoneVK, mShVKDoneGL, mShVKDoneHandle))
        {
            return shFail("its semaphores could not be shared");
        }

        mShReady = true;
        mShNote  = llformat("ready%s%s", mHasMultiIndirect ? "" : " (no multi-draw indirect: one call a draw)",
                            mHasDepthClamp ? "" : " (no depth clamp: the sun stays in GL)");
        createMaskObjects();
        LL_INFOS("Vulkan") << "Shadows in Vulkan " << mShNote << ": arenas of "
                           << (mShStatic.totalBytes() >> 20) << " MB (unrigged) and " << (mShRigged.totalBytes() >> 20)
                           << " MB (rigged), " << SH_SLOTS << " slots of " << SH_MAX_DRAWS << " draws" << LL_ENDL;
        return true;
    }

    // A pipeline for one kind of caster. Masked: a fragment stage too (the alpha test), and failure says so in
    // mShMaskNote rather than failing the shadows.
    bool createShadowPipeline(bool clamp, bool greater, bool rigged, bool masked, VkPipeline& pipeline)
    {
        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = masked ? (rigged ? mShMaskSkinModule : mShMaskModule) : (rigged ? mShSkinModule : mShModule);
        stages[0].pName  = "main";
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = mShMaskFragModule;
        stages[1].pName  = "main";
        // Positions at binding 0; for a rigged caster, its weights at binding 1 (the arena's second part, bound there);
        // for a masked one, its texture coordinates and colours at the next two (locations 2 and 3).
        VkVertexInputBindingDescription vertex_bindings[4] = {};
        VkVertexInputAttributeDescription attributes[4] = {};
        U32 streams = 0;
        auto stream = [&](U32 location, U32 stride, VkFormat format)
        {
            vertex_bindings[streams] = { streams, stride, VK_VERTEX_INPUT_RATE_VERTEX };
            attributes[streams] = { location, streams, format, 0 };
            ++streams;
        };
        stream(0, 16, VK_FORMAT_R32G32B32_SFLOAT);
        if (rigged)
        {
            stream(1, 16, VK_FORMAT_R32G32B32A32_SFLOAT);
        }
        if (masked)
        {
            stream(2, 8, VK_FORMAT_R32G32_SFLOAT);
            stream(3, 4, VK_FORMAT_R8G8B8A8_UNORM);
        }
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex_input.vertexBindingDescriptionCount   = streams;
        vertex_input.pVertexBindingDescriptions      = vertex_bindings;
        vertex_input.vertexAttributeDescriptionCount = streams;
        vertex_input.pVertexAttributeDescriptions    = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;
        // GL culls back faces, counter-clockwise in front, in window space y up; Vulkan's framebuffer runs y down for the
        // same clip space (its rows are GL's rows: the self-test), which turns every triangle's winding over.
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.depthClampEnable = clamp ? VK_TRUE : VK_FALSE;
        raster.polygonMode      = VK_POLYGON_MODE_FILL;
        raster.cullMode         = VK_CULL_MODE_BACK_BIT;
        raster.frontFace        = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth        = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp   = greater ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline_info.stageCount          = masked ? 2 : 1;
        pipeline_info.pStages             = stages;
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState   = &multisample;
        pipeline_info.pDepthStencilState  = &depth;
        pipeline_info.pColorBlendState    = &blend;
        pipeline_info.pDynamicState       = &dynamic;
        pipeline_info.layout              = mShLayout;
        pipeline_info.renderPass          = mShPass;
        if (vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
            if (masked)
            {
                mShMaskNote = rigged ? "failed: vkCreateGraphicsPipelines (rigged masked shadows)"
                                     : "failed: vkCreateGraphicsPipelines (masked shadows)";
                return false;
            }
            return shFail(rigged ? "vkCreateGraphicsPipelines (rigged shadows)" : "vkCreateGraphicsPipelines (shadows)");
        }
        return true;
    }

    // The masked casters' objects: their two arenas, the atlas (an RGBA8 image Vulkan allocates, GL imports, GL's blits
    // fill and Vulkan samples), its sampler, the framebuffers GL blits through, and the pipelines. Where any cannot be
    // made, nothing of them is left and the masked casters stay GL's (mShMaskNote says why); the rest goes on.
    bool createMaskObjects()
    {
        mShMaskReady = false;
        mShMaskNote = "being made";
        auto fail = [&](const std::string& why)
        {
            if (mShMaskNote.compare(0, 8, "failed: ") != 0)
            {
                mShMaskNote = "failed: " + why;
            }
            LL_WARNS("Vulkan") << "Masked shadow casters in Vulkan " << mShMaskNote << LL_ENDL;
            destroyMaskObjects();
            return false;
        };
        if (!createArena(mShMasked, SH_MASKED_VERTEX_BYTES, false, true, SH_MASKED_INDEX_BYTES, "masked arena") ||
            !createArena(mShMaskedRigged, SH_MASKED_RIGGED_VERTEX_BYTES, true, true, SH_MASKED_RIGGED_INDEX_BYTES,
                         "rigged masked arena"))
        {
            return fail("an arena");
        }

        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent        = { SH_ATLAS_SIZE, SH_ATLAS_SIZE, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(mDevice, &info, nullptr, &mShAtlas) != VK_SUCCESS)
        {
            mShAtlas = VK_NULL_HANDLE;
            return fail("vkCreateImage (atlas)");
        }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, mShAtlas, &req);
        const U32 device_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (device_type == U32_MAX)
        {
            return fail("no device-local memory for the atlas");
        }
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = mShAtlas;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = device_type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &mShAtlasMemory) != VK_SUCCESS)
        {
            mShAtlasMemory = VK_NULL_HANDLE;
            return fail("vkAllocateMemory (atlas)");
        }
        vkBindImageMemory(mDevice, mShAtlas, mShAtlasMemory, 0);
        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image            = mShAtlas;
        view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format           = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(mDevice, &view_info, nullptr, &mShAtlasView) != VK_SUCCESS)
        {
            mShAtlasView = VK_NULL_HANDLE;
            return fail("vkCreateImageView (atlas)");
        }
        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler_info.magFilter    = VK_FILTER_LINEAR;
        sampler_info.minFilter    = VK_FILTER_LINEAR;
        sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod       = 0.f;
        if (vkCreateSampler(mDevice, &sampler_info, nullptr, &mShAtlasSampler) != VK_SUCCESS)
        {
            mShAtlasSampler = VK_NULL_HANDLE;
            return fail("vkCreateSampler (atlas)");
        }
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = mShAtlasMemory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            return fail("vkGetMemoryWin32HandleKHR (atlas)");
        }
        if (!mKMT)
        {
            mShAtlasHandle = handle;
        }
        gl.CreateMemoryObjectsEXT(1, &mShAtlasGLMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(mShAtlasGLMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(mShAtlasGLMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.CreateTextures(GL_TEXTURE_2D, 1, &mShAtlasGL);
        gl.TextureParameteri(mShAtlasGL, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(mShAtlasGL, 1, GL_RGBA8, (GLsizei)SH_ATLAS_SIZE, (GLsizei)SH_ATLAS_SIZE,
                                  mShAtlasGLMemory, 0);
        // Opaque throughout to start: tile 0 stays so, the tile of every texture with no alpha channel (whose alpha
        // is 1 everywhere, so nothing to copy), and a tile a blit fails to fill casts as solid rather than as whatever
        // texture had it before.
        const U8 opaque[4] = { 255, 255, 255, 255 };
        gl.ClearTexSubImage(mShAtlasGL, 0, 0, 0, 0, (GLsizei)SH_ATLAS_SIZE, (GLsizei)SH_ATLAS_SIZE, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, opaque);
        gl.CreateFramebuffers(1, &mShReadFbo);
        gl.CreateFramebuffers(1, &mShAtlasFbo);
        gl.NamedFramebufferTexture(mShAtlasFbo, GL_COLOR_ATTACHMENT0, mShAtlasGL, 0);
        // Asked once, as it is made (a query waits for a threaded driver, so never a frame): the atlas must be one GL
        // can blit into.
        const GLenum status = gl.CheckNamedFramebufferStatus(mShAtlasFbo, GL_DRAW_FRAMEBUFFER);
        gl.Finish();
        logGLErrors("importing the shadow alpha atlas");
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            return fail(llformat("GL cannot draw into the atlas (framebuffer status 0x%x)", (U32)status));
        }

        VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module_info.codeSize = sizeof(ql_spv_qlshadowmask_vert);
        module_info.pCode    = ql_spv_qlshadowmask_vert;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mShMaskModule) != VK_SUCCESS)
        {
            mShMaskModule = VK_NULL_HANDLE;
            return fail("vkCreateShaderModule (masked shadows)");
        }
        module_info.codeSize = sizeof(ql_spv_qlshadowmaskskin_vert);
        module_info.pCode    = ql_spv_qlshadowmaskskin_vert;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mShMaskSkinModule) != VK_SUCCESS)
        {
            mShMaskSkinModule = VK_NULL_HANDLE;
            return fail("vkCreateShaderModule (rigged masked shadows)");
        }
        module_info.codeSize = sizeof(ql_spv_qlshadowmask_frag);
        module_info.pCode    = ql_spv_qlshadowmask_frag;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mShMaskFragModule) != VK_SUCCESS)
        {
            mShMaskFragModule = VK_NULL_HANDLE;
            return fail("vkCreateShaderModule (masked shadows' alpha test)");
        }
        for (U32 clamp = 0; clamp < 2; ++clamp)
        {
            for (U32 greater = 0; greater < 2; ++greater)
            {
                if (clamp && !mHasDepthClamp)
                {
                    continue;
                }
                if (!createShadowPipeline(clamp != 0, greater != 0, false, true, mShMaskPipelines[clamp * 2 + greater]) ||
                    !createShadowPipeline(clamp != 0, greater != 0, true, true, mShMaskSkinPipelines[clamp * 2 + greater]))
                {
                    return fail("a pipeline");
                }
            }
        }

        // Every slot's set samples the atlas.
        const VkDescriptorImageInfo image = { mShAtlasSampler, mShAtlasView, VK_IMAGE_LAYOUT_GENERAL };
        for (ShSlot& slot : mShSlots)
        {
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet          = slot.set;
            write.dstBinding      = 4;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo      = &image;
            vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        }

        mShTiles.assign(SH_TILES, ShTile());
        mShTileOf.clear();
        mShTilesTaken = 1;  // tile 0 is the opaque one
        mShMaskReady = true;
        mShMaskNote  = "ready";
        LL_INFOS("Vulkan") << "Masked shadow casters in Vulkan ready: arenas of " << (mShMasked.totalBytes() >> 20)
                           << " MB and " << (mShMaskedRigged.totalBytes() >> 20) << " MB (rigged), an atlas of "
                           << SH_TILES << " tiles of " << SH_TILE << " square" << LL_ENDL;
        return true;
    }

    // Everything createMaskObjects makes, once GL and Vulkan are done with it (made and destroyed only off the frame's
    // path: as the shadows are made, on failure, at shutdown).
    void destroyMaskObjects()
    {
        mShMaskReady = false;
        if (!mDevice)
        {
            return;
        }
        vkDeviceWaitIdle(mDevice);
        if (gl.Finish)
        {
            gl.Finish();
        }
        for (VkPipeline& p : mShMaskPipelines)
        {
            if (p) vkDestroyPipeline(mDevice, p, nullptr);
            p = VK_NULL_HANDLE;
        }
        for (VkPipeline& p : mShMaskSkinPipelines)
        {
            if (p) vkDestroyPipeline(mDevice, p, nullptr);
            p = VK_NULL_HANDLE;
        }
        for (VkShaderModule* m : { &mShMaskModule, &mShMaskSkinModule, &mShMaskFragModule })
        {
            if (*m) vkDestroyShaderModule(mDevice, *m, nullptr);
            *m = VK_NULL_HANDLE;
        }
        if (mShReadFbo) gl.DeleteFramebuffers(1, &mShReadFbo);
        if (mShAtlasFbo) gl.DeleteFramebuffers(1, &mShAtlasFbo);
        if (mShAtlasGL) gl.DeleteTextures(1, &mShAtlasGL);
        if (mShAtlasGLMemory) gl.DeleteMemoryObjectsEXT(1, &mShAtlasGLMemory);
        mShReadFbo = mShAtlasFbo = mShAtlasGL = mShAtlasGLMemory = 0;
        if (mShAtlasSampler) vkDestroySampler(mDevice, mShAtlasSampler, nullptr);
        if (mShAtlasView) vkDestroyImageView(mDevice, mShAtlasView, nullptr);
        if (mShAtlas) vkDestroyImage(mDevice, mShAtlas, nullptr);
        if (mShAtlasMemory) vkFreeMemory(mDevice, mShAtlasMemory, nullptr);
        if (mShAtlasHandle) CloseHandle(mShAtlasHandle);
        mShAtlasSampler = VK_NULL_HANDLE;
        mShAtlasView = VK_NULL_HANDLE;
        mShAtlas = VK_NULL_HANDLE;
        mShAtlasMemory = VK_NULL_HANDLE;
        mShAtlasHandle = nullptr;
        destroyArena(mShMasked);
        destroyArena(mShMaskedRigged);
        mShTiles.clear();
        mShTileOf.clear();
        mShTilesTaken = 0;
    }


    // The depth image standing in for a GL map, made the first time that map is drawn: each map has its own, since a
    // hand-over carries several. The least recently used of more than SH_MAX_DEPTHS goes, never one a queued map
    // names. Null where none can be.
    static constexpr size_t SH_MAX_DEPTHS = 16;  // a frame's maps: 4 sun and 2 spots, the probes' and the hero's
    ShDepth* findShadowDepth(GLuint gl_map, U32 width, U32 height)
    {
        for (ShDepth& d : mShDepths)
        {
            if (d.glMap == gl_map && d.width == width && d.height == height)
            {
                return &d;
            }
        }
        return nullptr;
    }

    ShDepth* shadowDepth(GLuint gl_map, U32 width, U32 height)
    {
        const U32 frame = mShFrame;
        if (ShDepth* found = findShadowDepth(gl_map, width, height))
        {
            found->lastFrame = frame;
            return found;
        }
        if (mShDepths.size() >= SH_MAX_DEPTHS)
        {
            auto queued = [&](const ShDepth& d)
            {
                for (const ShPending& p : mShPending)
                {
                    if (p.depth == d.glMap && p.width == d.width && p.height == d.height)
                    {
                        return true;
                    }
                }
                return false;
            };
            auto oldest = mShDepths.end();
            for (auto it = mShDepths.begin(); it != mShDepths.end(); ++it)
            {
                if (!queued(*it) && (oldest == mShDepths.end() || it->lastFrame < oldest->lastFrame))
                {
                    oldest = it;
                }
            }
            if (oldest == mShDepths.end())
            {
                return nullptr;
            }
            // Rare (a map not drawn for a while, or remade): both sides done with it before it goes.
            waitShadowSlots();
            gl.Finish();
            destroyShadowDepth(*oldest);
            mShDepths.erase(oldest);
        }
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        ShDepth d;
        d.glMap = gl_map;
        d.width = width;
        d.height = height;
        d.lastFrame = frame;
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_D32_SFLOAT;
        info.extent        = { width, height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto bad = [&](const char* what)
        {
            LL_WARNS("Vulkan") << "Shadows in Vulkan: no " << width << "x" << height << " depth image (" << what << ")"
                               << LL_ENDL;
            (void)gl_map;
            destroyShadowDepth(d);
            return (ShDepth*)nullptr;
        };
        if (vkCreateImage(mDevice, &info, nullptr, &d.image) != VK_SUCCESS)
        {
            d.image = VK_NULL_HANDLE;
            return bad("vkCreateImage");
        }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, d.image, &req);
        const U32 type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = d.image;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = type;
        if (type == U32_MAX || vkAllocateMemory(mDevice, &alloc, nullptr, &d.memory) != VK_SUCCESS)
        {
            d.memory = VK_NULL_HANDLE;
            return bad("vkAllocateMemory");
        }
        vkBindImageMemory(mDevice, d.image, d.memory, 0);
        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image            = d.image;
        view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format           = VK_FORMAT_D32_SFLOAT;
        view_info.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(mDevice, &view_info, nullptr, &d.view) != VK_SUCCESS)
        {
            d.view = VK_NULL_HANDLE;
            return bad("vkCreateImageView");
        }
        VkFramebufferCreateInfo fb_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb_info.renderPass      = mShPass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments    = &d.view;
        fb_info.width           = width;
        fb_info.height          = height;
        fb_info.layers          = 1;
        if (vkCreateFramebuffer(mDevice, &fb_info, nullptr, &d.framebuffer) != VK_SUCCESS)
        {
            d.framebuffer = VK_NULL_HANDLE;
            return bad("vkCreateFramebuffer");
        }
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = d.memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            return bad("vkGetMemoryWin32HandleKHR");
        }
        if (!mKMT)
        {
            d.handle = handle;
        }
        gl.CreateMemoryObjectsEXT(1, &d.glMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(d.glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(d.glMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.CreateTextures(GL_TEXTURE_2D, 1, &d.glTexture);
        gl.TextureParameteri(d.glTexture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl.TextureStorageMem2DEXT(d.glTexture, 1, GL_DEPTH_COMPONENT32F, (GLsizei)width, (GLsizei)height, d.glMemory, 0);
        gl.Finish();
        logGLErrors("importing a shadow depth image");
        mShDepths.push_back(d);
        LL_INFOS("Vulkan") << "Shadows in Vulkan: a " << width << "x" << height << " depth image" << LL_ENDL;
        return &mShDepths.back();
    }

    void destroyShadowDepth(ShDepth& d)
    {
        if (d.glTexture) gl.DeleteTextures(1, &d.glTexture);
        if (d.glMemory) gl.DeleteMemoryObjectsEXT(1, &d.glMemory);
        if (d.framebuffer) vkDestroyFramebuffer(mDevice, d.framebuffer, nullptr);
        if (d.view) vkDestroyImageView(mDevice, d.view, nullptr);
        if (d.image) vkDestroyImage(mDevice, d.image, nullptr);
        if (d.memory) vkFreeMemory(mDevice, d.memory, nullptr);
        if (d.handle) CloseHandle(d.handle);
        d = ShDepth();
    }

    void waitShadowSlots()
    {
        for (ShSlot& slot : mShSlots)
        {
            if (slot.submitted && slot.fence)
            {
                vkWaitForFences(mDevice, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                slot.submitted = false;
            }
        }
        mShDoneSerial = mShSubmitSerial;
    }

    // Asks every submission's fence, never waits: what has finished moves mShDoneSerial on.
    void pollShadowSubmissions()
    {
        for (ShSlot& slot : mShSlots)
        {
            if (slot.submitted && vkGetFenceStatus(mDevice, slot.fence) == VK_SUCCESS)
            {
                slot.submitted = false;
                mShDoneSerial = llmax(mShDoneSerial, slot.serial);
            }
        }
    }

    void destroyShadowObjects()
    {
        if (!mDevice)
        {
            return;
        }
        vkDeviceWaitIdle(mDevice);
        if (gl.Finish)
        {
            gl.Finish();
        }
        destroyMaskObjects();
        mShPending.clear();
        mShDoneSerial = mShSubmitSerial = 0;
        for (ShDepth& d : mShDepths)
        {
            destroyShadowDepth(d);
        }
        mShDepths.clear();
        for (ShSlot& slot : mShSlots)
        {
            if (slot.fence) vkDestroyFence(mDevice, slot.fence, nullptr);
            slot = ShSlot();  // the command buffers go with the pool, the sets with theirs
        }
        for (VkPipeline& p : mShPipelines)
        {
            if (p) vkDestroyPipeline(mDevice, p, nullptr);
            p = VK_NULL_HANDLE;
        }
        for (VkPipeline& p : mShSkinPipelines)
        {
            if (p) vkDestroyPipeline(mDevice, p, nullptr);
            p = VK_NULL_HANDLE;
        }
        if (mShModule) vkDestroyShaderModule(mDevice, mShModule, nullptr);
        if (mShSkinModule) vkDestroyShaderModule(mDevice, mShSkinModule, nullptr);
        mShSkinModule = VK_NULL_HANDLE;
        if (mShPass) vkDestroyRenderPass(mDevice, mShPass, nullptr);
        if (mShLayout) vkDestroyPipelineLayout(mDevice, mShLayout, nullptr);
        if (mShDescriptorPool) vkDestroyDescriptorPool(mDevice, mShDescriptorPool, nullptr);
        if (mShSetLayout) vkDestroyDescriptorSetLayout(mDevice, mShSetLayout, nullptr);
        mShModule = VK_NULL_HANDLE;
        mShPass = VK_NULL_HANDLE;
        mShLayout = VK_NULL_HANDLE;
        mShDescriptorPool = VK_NULL_HANDLE;
        mShSetLayout = VK_NULL_HANDLE;
        if (mShHostMapped)
        {
            vkUnmapMemory(mDevice, mShHostMemory);
            mShHostMapped = nullptr;
        }
        if (mShHost) vkDestroyBuffer(mDevice, mShHost, nullptr);
        if (mShHostMemory) vkFreeMemory(mDevice, mShHostMemory, nullptr);
        mShHost = VK_NULL_HANDLE;
        mShHostMemory = VK_NULL_HANDLE;
        if (mShPalettesMapped)
        {
            vkUnmapMemory(mDevice, mShPalettesMemory);
            mShPalettesMapped = nullptr;
        }
        if (mShPalettes) vkDestroyBuffer(mDevice, mShPalettes, nullptr);
        if (mShPalettesMemory) vkFreeMemory(mDevice, mShPalettesMemory, nullptr);
        mShPalettes = VK_NULL_HANDLE;
        mShPalettesMemory = VK_NULL_HANDLE;
        mShPaletteBases.clear();
        mShPaletteFrame = U32_MAX;
        destroyArena(mShStatic);
        destroyArena(mShRigged);
        if (mShGLDoneGL) gl.DeleteSemaphoresEXT(1, &mShGLDoneGL);
        if (mShVKDoneGL) gl.DeleteSemaphoresEXT(1, &mShVKDoneGL);
        if (mShGLDoneVK) vkDestroySemaphore(mDevice, mShGLDoneVK, nullptr);
        if (mShVKDoneVK) vkDestroySemaphore(mDevice, mShVKDoneVK, nullptr);
        if (mShGLDoneHandle) CloseHandle(mShGLDoneHandle);
        if (mShVKDoneHandle) CloseHandle(mShVKDoneHandle);
        mShGLDoneGL = mShVKDoneGL = 0;
        mShGLDoneVK = mShVKDoneVK = VK_NULL_HANDLE;
        mShGLDoneHandle = mShVKDoneHandle = nullptr;
        mShReady = false;
        mShInPass = false;
    }

    void shadowFrame(U32 frame)
    {
        if (frame != mShFrame)
        {
            shadowFlush();  // normally nothing: each view hands its maps over as it ends
            mShFlushesLast = mShFlushesNow;
            mShFlushesNow = 0;
            mShPassesLast = mShPassesNow;
            mShDrawsLast  = mShDrawsNow;
            mShRiggedLast = mShRiggedNow;
            mShRiggedNow  = 0;
            mShCopiedLast = mShCopiedNow;
            mShPassesNow = mShDrawsNow = 0;
            mShCopiedNow = 0;
            mShMaskedLast = mShMaskedNow;
            mShMaskedRiggedLast = mShMaskedRiggedNow;
            mShTileUploadsLast = mShTileUploadsNow;
            mShMaskedNow = mShMaskedRiggedNow = mShTileUploadsNow = 0;
            mShFrame = frame;
        }
    }

    bool shadowBegin(U32 width, U32 height, bool clamp, bool greater, bool remap_depth)
    {
        if (mShInPass || !createShadowObjects())
        {
            return false;
        }
        if (clamp && !mHasDepthClamp)
        {
            return false;
        }
        // Start an arena over when it is nearly full, before any of this map's draws is in it; the queued maps are
        // handed over first, since their meshes are in it.
        if (!mShPending.empty() &&
            (arenaFull(mShStatic) || arenaFull(mShRigged) ||
             (mShMaskReady && (arenaFull(mShMasked) || arenaFull(mShMaskedRigged)))))
        {
            shadowFlush();
        }
        resetArenaIfFull(mShStatic);
        resetArenaIfFull(mShRigged);
        if (mShMaskReady)
        {
            resetArenaIfFull(mShMasked);
            resetArenaIfFull(mShMaskedRigged);
        }
        ShSlot& slot = mShSlots[mShNextSlot];
        if (slot.pending)
        {
            return false;  // the ring has come round to a map still queued: never, short of SH_SLOTS maps in one view
        }
        if (slot.serial > mShDoneSerial)
        {
            // Normally several frames back and long done. Never waited for: one still in flight leaves the map to GL.
            pollShadowSubmissions();
            if (slot.serial > mShDoneSerial)
            {
                return false;
            }
        }
        // Its last submission is done: a fence still marked submitted (a poll that asked it a moment early) is
        // signalled, and must not be read as a later submission's once the slot rides in one without heading it.
        slot.submitted = false;
        mShSlot = mShNextSlot;
        mShNextSlot = (mShNextSlot + 1) % SH_SLOTS;
        mShInPass     = true;
        mShDraws      = 0;
        mShKind       = SH_KIND_STATIC;
        for (U32& first : mShKindFirst)
        {
            first = 0;
        }
        ++mShPassSerial;
        mShWidth      = width;
        mShHeight     = height;
        mShClamp      = clamp;
        mShGreater    = greater;
        mShRemapDepth = remap_depth;
        return true;
    }

    // A mesh in an arena: found by its content stamp, or copied in now (its weights too, where the arena keeps them).
    // False when it does not fit.
    // A masked arena's mesh copies its texture coordinates and colours from uv_offset and color_offset as well; a
    // colour offset of U32_MAX (a mesh without colours, for a test that reads none) leaves its colours as they are.
    bool arenaPlace(ShArena& arena, U64 stamp, GLuint vertex_buffer, U32 vertex_offset, U32 weight_offset,
                    U32 vertex_count, GLuint index_buffer, U32 index_count, U32& base_vertex, U32& base_index,
                    U32 uv_offset = 0, U32 color_offset = U32_MAX)
    {
        if (!mShInPass)
        {
            return false;
        }
        const auto found = arena.entries.find(stamp);
        if (found != arena.entries.end())
        {
            base_vertex = found->second.mBaseVertex;
            base_index  = found->second.mBaseIndex;
            return true;
        }
        const VkDeviceSize vertex_bytes = (VkDeviceSize)vertex_count * 16;
        const VkDeviceSize index_bytes  = ((VkDeviceSize)index_count * 2 + 3) & ~(VkDeviceSize)3;
        if (!vertex_buffer || !index_buffer || vertex_count == 0 || index_count == 0 ||
            arena.vertexTop + vertex_bytes > arena.vertexBytes || arena.indexTop + index_bytes > arena.indexBytes)
        {
            return false;
        }
        gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)vertex_offset, (GLintptr)arena.vertexTop,
                                  (GLsizeiptr)vertex_bytes);
        if (arena.weightBytes)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)weight_offset,
                                      (GLintptr)(arena.vertexBytes + arena.vertexTop), (GLsizeiptr)vertex_bytes);
        }
        if (arena.uvBytes)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)uv_offset,
                                      (GLintptr)(arena.uvStart() + arena.vertexTop / 2), (GLsizeiptr)vertex_count * 8);
            if (color_offset != U32_MAX)
            {
                gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)color_offset,
                                          (GLintptr)(arena.colorStart() + arena.vertexTop / 4),
                                          (GLsizeiptr)vertex_count * 4);
            }
        }
        gl.CopyNamedBufferSubData(index_buffer, arena.gl, 0, (GLintptr)(arena.indexStart() + arena.indexTop),
                                  (GLsizeiptr)index_count * 2);
        ShArenaEntry entry;
        entry.mBaseVertex = (U32)(arena.vertexTop / 16);
        entry.mBaseIndex  = (U32)(arena.indexTop / 2);
        arena.entries.emplace(stamp, entry);
        arena.vertexTop += vertex_bytes;
        arena.indexTop += index_bytes;
        mShCopiedNow += vertex_bytes * (arena.weightBytes ? 2 : 1) + (arena.uvBytes ? vertex_count * 12ull : 0) +
                        index_bytes;
        base_vertex = entry.mBaseVertex;
        base_index  = entry.mBaseIndex;
        return true;
    }

    bool shadowArena(U64 stamp, GLuint vertex_buffer, U32 vertex_offset, U32 vertex_count, GLuint index_buffer,
                     U32 index_count, U32& base_vertex, U32& base_index)
    {
        return arenaPlace(mShStatic, stamp, vertex_buffer, vertex_offset, 0, vertex_count, index_buffer, index_count,
                          base_vertex, base_index);
    }

    bool shadowArenaRigged(U64 stamp, GLuint vertex_buffer, U32 vertex_offset, U32 weight_offset, U32 vertex_count,
                           GLuint index_buffer, U32 index_count, U32& base_vertex, U32& base_index)
    {
        return arenaPlace(mShRigged, stamp, vertex_buffer, vertex_offset, weight_offset, vertex_count, index_buffer,
                          index_count, base_vertex, base_index);
    }

    bool shadowArenaMasked(bool rigged, U64 stamp, GLuint vertex_buffer, U32 vertex_offset, U32 weight_offset,
                           U32 uv_offset, U32 color_offset, U32 vertex_count, GLuint index_buffer, U32 index_count,
                           U32& base_vertex, U32& base_index)
    {
        if (!mShMaskReady)
        {
            return false;
        }
        return arenaPlace(rigged ? mShMaskedRigged : mShMasked, stamp, vertex_buffer, vertex_offset, weight_offset,
                          vertex_count, index_buffer, index_count, base_vertex, base_index, uv_offset, color_offset);
    }

    // A texture's tile in the alpha atlas: found by its GL name and content stamp, or blitted in now by GL from the
    // level nearest the tile's size (levels: how many it has). The tile is the least recently drawn once all are
    // given out, never one a map not yet handed over draws with. False when none is free: the caster is GL's.
    bool shadowMaskTexture(GLuint texture, U64 stamp, U32 width, U32 height, U32 levels, bool has_alpha, U32& tile)
    {
        if (!mShInPass || !mShMaskReady || !texture || width == 0 || height == 0)
        {
            return false;
        }
        if (!has_alpha)
        {
            tile = 0;  // alpha 1 everywhere: the opaque tile
            return true;
        }
        const std::pair<GLuint, U64> key(texture, stamp);
        const auto found = mShTileOf.find(key);
        if (found != mShTileOf.end())
        {
            tile = found->second;
            mShTiles[tile].lastPass = mShPassSerial;
            return true;
        }
        U32 at = U32_MAX;
        if (mShTilesTaken < SH_TILES)
        {
            at = mShTilesTaken++;
        }
        else
        {
            U64 oldest = ~0ull;
            for (U32 i = 1; i < SH_TILES; ++i)
            {
                if (mShTiles[i].lastPass <= mShPassAtFlush && mShTiles[i].lastPass < oldest)
                {
                    oldest = mShTiles[i].lastPass;
                    at = i;
                }
            }
            if (at == U32_MAX)
            {
                return false;
            }
            mShTileOf.erase(std::make_pair(mShTiles[at].texture, mShTiles[at].stamp));
        }
        // The level whose size is nearest the tile's from above (a linear blit then takes at most two texels to one).
        U32 level = 0;
        while (level + 1 < levels && llmax(width >> (level + 1), height >> (level + 1)) >= SH_TILE)
        {
            ++level;
        }
        const GLint src_w = (GLint)llmax(1u, width >> level);
        const GLint src_h = (GLint)llmax(1u, height >> level);
        const GLint x = (GLint)((at % SH_TILES_ACROSS) * SH_TILE);
        const GLint y = (GLint)((at / SH_TILES_ACROSS) * SH_TILE);
        // A blit is cut by the scissor test, and nothing else of the fragment pipeline: off for it where llrender has it
        // on. The read framebuffer lets go of the texture after, so the texture can go when its owner deletes it.
        const bool scissor = ql_gl_cap_tracked_enabled(GL_SCISSOR_TEST);
        if (scissor)
        {
            gl.Disable(GL_SCISSOR_TEST);
        }
        // Opaque first, so a blit that cannot read the texture (a level it does not have) leaves a solid tile, not the
        // last texture's. A clear of the image, which no mask or scissor touches.
        const U8 opaque[4] = { 255, 255, 255, 255 };
        gl.ClearTexSubImage(mShAtlasGL, 0, x, y, 0, (GLsizei)SH_TILE, (GLsizei)SH_TILE, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            opaque);
        gl.NamedFramebufferTexture(mShReadFbo, GL_COLOR_ATTACHMENT0, texture, (GLint)level);
        gl.BlitNamedFramebuffer(mShReadFbo, mShAtlasFbo, 0, 0, src_w, src_h, x, y, x + (GLint)SH_TILE,
                                y + (GLint)SH_TILE, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gl.NamedFramebufferTexture(mShReadFbo, GL_COLOR_ATTACHMENT0, 0, 0);
        if (scissor)
        {
            gl.Enable(GL_SCISSOR_TEST);
        }
        ++mShTileUploadsNow;
        mShTiles[at].texture  = texture;
        mShTiles[at].stamp    = stamp;
        mShTiles[at].lastPass = mShPassSerial;
        mShTileOf.emplace(key, at);
        tile = at;
        return true;
    }

    static bool arenaFull(const ShArena& arena)
    {
        return arena.vertexTop > arena.vertexBytes - arena.vertexBytes / 8 ||
               arena.indexTop > arena.indexBytes - arena.indexBytes / 8;
    }

    void resetArenaIfFull(ShArena& arena)
    {
        if (arenaFull(arena))
        {
            arena.entries.clear();
            arena.vertexTop = arena.indexTop = 0;
            ++arena.resets;
        }
    }

    // A skin's palette for this frame, written into this frame's segment of the ring the first time any map asks for
    // it. key identifies the avatar and the skin; floats is mGLMp (the origin, then 12 a joint). U32_MAX when the
    // segment is full or the palette is larger than a mesh can have.
    U32 shadowPalette(U64 avatar, U64 skin, const float* data, U32 floats)
    {
        if (!mShPalettesMapped || !data || floats < 4 || floats > SH_PALETTE_VEC4S * 4)
        {
            return U32_MAX;
        }
        if (mShPaletteFrame != mShFrame)
        {
            mShPaletteFrame = mShFrame;
            mShPaletteBases.clear();
            mShPaletteCountLast = mShPaletteCount;
            mShPaletteCount = 0;
            // The segment this frame writes was last read SH_PALETTE_SEGMENTS frames ago: normally long done, but never
            // assumed (a pacing depth past it, or none). Asked, never waited for: while it is still read, this frame's
            // rigged casters are GL's.
            const U64 last = mShSegmentSerial[mShFrame % SH_PALETTE_SEGMENTS];
            if (last > mShDoneSerial)
            {
                pollShadowSubmissions();
            }
            mShPaletteSegmentFree = last <= mShDoneSerial;
        }
        if (!mShPaletteSegmentFree)
        {
            return U32_MAX;
        }
        const std::pair<U64, U64> key(avatar, skin);
        const auto found = mShPaletteBases.find(key);
        if (found != mShPaletteBases.end())
        {
            return found->second;
        }
        if (mShPaletteCount >= SH_PALETTES_PER_FRAME)
        {
            return U32_MAX;
        }
        const U32 segment = mShFrame % SH_PALETTE_SEGMENTS;
        const VkDeviceSize offset = SH_PALETTE_SEGMENT_BYTES * segment + SH_PALETTE_BYTES * mShPaletteCount;
        memcpy(mShPalettesMapped + offset, data, (size_t)floats * sizeof(float));
        const U32 base = (U32)(offset / 16);
        ++mShPaletteCount;
        mShPaletteBases.emplace(key, base);
        return base;
    }

    // An arena (see ShArena), with texture coordinates and colours where masked. A failure is the shadows' (shFail) for
    // the arenas every map needs; a masked arena's leaves the rest made and says why in mShMaskNote.
    bool createArena(ShArena& arena, VkDeviceSize vertex_bytes, bool weights, bool masked, VkDeviceSize index_bytes,
                     const char* what)
    {
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        auto shFail = [&](const std::string& why)
        {
            if (masked)
            {
                mShMaskNote = "failed: " + why;
                LL_WARNS("Vulkan") << "Masked shadow casters in Vulkan " << mShMaskNote << LL_ENDL;
                return false;
            }
            return this->shFail(why);
        };
        arena.vertexBytes = vertex_bytes;
        arena.weightBytes = weights ? vertex_bytes : 0;
        arena.uvBytes     = masked ? vertex_bytes / 2 : 0;
        arena.colorBytes  = masked ? vertex_bytes / 4 : 0;
        arena.indexBytes  = index_bytes;
        VkExternalMemoryBufferCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = handle_type;
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.pNext       = &external;
        info.size        = arena.totalBytes();
        info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &info, nullptr, &arena.buffer) != VK_SUCCESS)
        {
            arena.buffer = VK_NULL_HANDLE;
            return shFail(llformat("vkCreateBuffer (%s)", what));
        }
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(mDevice, arena.buffer, &req);
        const U32 device_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (device_type == U32_MAX)
        {
            return shFail(llformat("no device-local memory for the %s", what));
        }
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = arena.buffer;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.pNext       = &dedicated;
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = device_type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &arena.memory) != VK_SUCCESS)
        {
            arena.memory = VK_NULL_HANDLE;
            return shFail(llformat("vkAllocateMemory (%s, %u MB)", what, (U32)(arena.totalBytes() >> 20)));
        }
        vkBindBufferMemory(mDevice, arena.buffer, arena.memory, 0);
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = arena.memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            return shFail(llformat("vkGetMemoryWin32HandleKHR (%s)", what));
        }
        if (!mKMT)
        {
            arena.handle = handle;
        }
        gl.CreateMemoryObjectsEXT(1, &arena.glMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(arena.glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(arena.glMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.CreateBuffers(1, &arena.gl);
        gl.NamedBufferStorageMemEXT(arena.gl, (GLsizeiptr)arena.totalBytes(), arena.glMemory, 0);
        gl.Finish();
        logGLErrors(what);
        return true;
    }

    void destroyArena(ShArena& arena)
    {
        if (arena.gl) gl.DeleteBuffers(1, &arena.gl);
        if (arena.glMemory) gl.DeleteMemoryObjectsEXT(1, &arena.glMemory);
        if (arena.buffer) vkDestroyBuffer(mDevice, arena.buffer, nullptr);
        if (arena.memory) vkFreeMemory(mDevice, arena.memory, nullptr);
        if (arena.handle) CloseHandle(arena.handle);
        arena = ShArena();
    }

    // The map's draws come kind by kind (SH_KIND_*): a draw of a kind before the one being written is refused.
    bool enterKind(U32 kind)
    {
        if (!mShInPass || kind < mShKind || mShDraws >= SH_MAX_DRAWS)
        {
            return false;
        }
        while (mShKind < kind)
        {
            mShKindFirst[++mShKind] = mShDraws;
        }
        return true;
    }

    bool shadowDraw(const float mvp[16], U32 base_vertex, U32 first_index, U32 index_count)
    {
        if (!enterKind(SH_KIND_STATIC))
        {
            return false;
        }
        writeShadowDraw(mvp, base_vertex, first_index, index_count, 0);
        return true;
    }

    // A rigged caster, after all the map's unrigged ones: its matrix, its palette (shadowPalette) and its mesh in the
    // rigged arena.
    bool shadowDrawRigged(const float mvp[16], U32 palette_base, U32 base_vertex, U32 first_index, U32 index_count)
    {
        if (palette_base == U32_MAX || !enterKind(SH_KIND_RIGGED))
        {
            return false;
        }
        writeShadowDraw(mvp, base_vertex, first_index, index_count, palette_base);
        return true;
    }

    // A masked caster, after all the map's opaque ones (and unrigged before rigged): its matrix, its palette (U32_MAX
    // unrigged), its texture's tile (shadowMaskTexture), its texture matrix (null: none), its cutoff and its test (as
    // qlshadowmask.frag numbers them), its mesh in the masked arena of its kind.
    bool shadowDrawMasked(const float mvp[16], U32 palette_base, U32 tile, const float* texture_matrix, float cutoff,
                          U32 test, U32 base_vertex, U32 first_index, U32 index_count)
    {
        const bool rigged = palette_base != U32_MAX;
        if (!mShMaskReady || tile >= SH_TILES || !enterKind(rigged ? SH_KIND_MASKED_RIGGED : SH_KIND_MASKED))
        {
            return false;
        }
        // The texture matrix's rows for x and y over (u, v, 1), as GL's texture_matrix0 * vec4(uv, 0, 1) takes them
        // (column major: element [column * 4 + row]).
        static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float* m = texture_matrix ? texture_matrix : identity;
        const float mask[12] = { m[0], m[4], m[12], cutoff,
                                 m[1], m[5], m[13], (float)test,
                                 (float)((tile % SH_TILES_ACROSS) * SH_TILE), (float)((tile / SH_TILES_ACROSS) * SH_TILE),
                                 (float)SH_TILE, 0.f };
        memcpy(mShHostMapped + SH_SLOT_BYTES * mShSlot + SH_MASK_START + (size_t)mShDraws * sizeof(mask), mask,
               sizeof(mask));
        writeShadowDraw(mvp, base_vertex, first_index, index_count, rigged ? palette_base : 0);
        return true;
    }

    void writeShadowDraw(const float mvp[16], U32 base_vertex, U32 first_index, U32 index_count, U32 palette_base)
    {
        U8* slot = mShHostMapped + SH_SLOT_BYTES * mShSlot;
        memcpy(slot + SH_MVP_BYTES + SH_CMD_BYTES + (size_t)mShDraws * 4, &palette_base, 4);
        memcpy(slot + (size_t)mShDraws * 64, mvp, 64);
        VkDrawIndexedIndirectCommand cmd = {};
        cmd.indexCount    = index_count;
        cmd.instanceCount = 1;
        cmd.firstIndex    = first_index;
        cmd.vertexOffset  = (S32)base_vertex;
        cmd.firstInstance = mShDraws;
        memcpy(slot + SH_MVP_BYTES + (size_t)mShDraws * sizeof(cmd), &cmd, sizeof(cmd));
        ++mShDraws;
    }

    // The map's casters, queued: Vulkan draws them over what GL's depth_texture holds at the next hand-over
    // (shadowFlush), when GL has drawn the rest of the map. False when they cannot be (no depth image, no pipeline):
    // nothing of the map is then Vulkan's, and GL draws it all.
    bool shadowEnd(GLuint depth_texture)
    {
        if (!mShInPass)
        {
            return false;
        }
        mShInPass = false;
        if (mShDraws == 0)
        {
            return true;
        }
        while (mShKind < SH_KINDS)
        {
            mShKindFirst[++mShKind] = mShDraws;  // the kinds not reached are empty, and the last ends the draws
        }
        const U32 variant = (mShClamp ? 2 : 0) + (mShGreater ? 1 : 0);
        const bool masked = mShKindFirst[SH_KINDS] > mShKindFirst[SH_KIND_MASKED];
        if (!depth_texture || !mShPipelines[variant] || !mShSkinPipelines[variant] ||
            (masked && (!mShMaskPipelines[variant] || !mShMaskSkinPipelines[variant])) ||
            !shadowDepth(depth_texture, mShWidth, mShHeight))
        {
            return false;
        }
        ShPending pending;
        pending.slot    = mShSlot;
        pending.depth   = depth_texture;
        pending.width   = mShWidth;
        pending.height  = mShHeight;
        pending.variant = variant;
        pending.remap   = mShRemapDepth;
        memcpy(pending.kindFirst, mShKindFirst, sizeof(pending.kindFirst));
        mShSlots[mShSlot].pending = true;
        mShPending.push_back(pending);
        return true;
    }

    // Every queued map, handed over at once: GL copies each map as it now stands into its depth image, signals once,
    // Vulkan draws every map's casters over them in one submission, GL waits once and copies each back. A view's maps
    // are drawn by GL first (what Vulkan left) and finished by Vulkan here, before anything reads them; depth tests
    // commute, so the order changes nothing drawn. On NVIDIA's threaded driver the signal waits for the driver's
    // thread to catch up: once a view, rather than once a map.
    bool shadowFlush()
    {
        if (mShPending.empty())
        {
            return true;
        }
        LL_PROFILE_ZONE_NAMED("vulkan shadow hand-over");
        std::vector<ShPending> pending;
        pending.swap(mShPending);
        // The atlas's tiles drawn with until now may go to other textures once GL has waited for this hand-over.
        mShPassAtFlush = mShPassSerial;

        // The images, each once (a map drawn twice in a view is queued twice).
        std::vector<ShDepth*> images;
        std::vector<U32> image_of(pending.size());
        bool masked = false, rigged = false;
        U32 draws = 0, rigged_draws = 0, masked_draws = 0, masked_rigged_draws = 0;
        for (size_t i = 0; i < pending.size(); ++i)
        {
            const ShPending& p = pending[i];
            ShDepth* image = findShadowDepth(p.depth, p.width, p.height);
            U32 at = U32_MAX;
            for (U32 k = 0; k < images.size(); ++k)
            {
                if (images[k] == image)
                {
                    at = k;
                }
            }
            if (at == U32_MAX)
            {
                at = (U32)images.size();
                images.push_back(image);
            }
            image_of[i] = at;
            auto count = [&](U32 kind) { return p.kindFirst[kind + 1] - p.kindFirst[kind]; };
            draws += p.kindFirst[SH_KINDS];
            rigged_draws += count(SH_KIND_RIGGED);
            masked_draws += count(SH_KIND_MASKED);
            masked_rigged_draws += count(SH_KIND_MASKED_RIGGED);
        }
        masked = masked_draws + masked_rigged_draws > 0;
        rigged = rigged_draws + masked_rigged_draws > 0;
        for (const ShDepth* image : images)
        {
            if (!image)
            {
                // Never: shadowEnd made it, and nothing lets a queued map's image go. Nothing drawn this time.
                for (const ShPending& p : pending)
                {
                    mShSlots[p.slot].pending = false;
                }
                LL_WARNS("Vulkan") << "Shadows in Vulkan: a queued map lost its depth image" << LL_ENDL;
                return false;
            }
        }

        // What GL hands over: every arena, every map's image and, with masked casters, the atlas its blits wrote.
        std::vector<GLuint> gl_buffers = { mShStatic.gl, mShRigged.gl };
        if (mShMaskReady)
        {
            gl_buffers.push_back(mShMasked.gl);
            gl_buffers.push_back(mShMaskedRigged.gl);
        }
        std::vector<GLuint> gl_textures;
        for (const ShDepth* image : images)
        {
            gl_textures.push_back(image->glTexture);
        }
        if (masked)
        {
            gl_textures.push_back(mShAtlasGL);
        }
        const std::vector<GLenum> layouts(gl_textures.size(), GL_LAYOUT_GENERAL_EXT);
        const ShArena* arenas[4] = { &mShStatic, &mShRigged, &mShMasked, &mShMaskedRigged };
        const U32 arena_count = mShMaskReady ? 4 : 2;

        // GL: each map as it stands into its image, and all of it over to Vulkan. Each step its own zone, to find
        // which, if any, waits for a threaded driver's thread.
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - gl copy in");
            for (size_t i = 0; i < pending.size(); ++i)
            {
                bool first = true;
                for (size_t k = 0; k < i; ++k)
                {
                    first = first && image_of[k] != image_of[i];
                }
                if (first)
                {
                    const ShPending& p = pending[i];
                    gl.CopyImageSubData(p.depth, GL_TEXTURE_2D, 0, 0, 0, 0, images[image_of[i]]->glTexture,
                                        GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)p.width, (GLsizei)p.height, 1);
                }
            }
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - gl signal");
            // QuasarVulkanShadowSignalObjects off (an experiment): the signal names nothing it hands over, in case naming
            // the arenas and the images is what makes NVIDIA's driver wait for its thread here.
            if (sShSignalObjects)
            {
                gl.SignalSemaphoreEXT(mShGLDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(),
                                      (GLuint)gl_textures.size(), gl_textures.data(), layouts.data());
            }
            else
            {
                gl.SignalSemaphoreEXT(mShGLDoneGL, 0, nullptr, 0, nullptr, nullptr);
            }
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - gl flush");
            gl.Flush();
        }
        // To the function's end (its own variable, beside the hand-over's zone): its self time is the recording, the
        // steps after have zones of their own.
        LL_PROFILE_ZONE_NAMED_COLOR("vulkan shadow - vk record", 0x40A0FF);

        // The first map's command buffer and fence carry the hand-over.
        ShSlot& head = mShSlots[pending[0].slot];
        VkCommandBuffer cmd = head.cmd;
        vkResetFences(mDevice, 1, &head.fence);
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        for (U32 a = 0; a < arena_count; ++a)
        {
            bufferBarrier(cmd, arenas[a]->buffer, 0, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT,
                          VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
        }
        VkImageMemoryBarrier atlas_acquire = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        atlas_acquire.srcAccessMask       = 0;
        atlas_acquire.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        atlas_acquire.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        atlas_acquire.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        atlas_acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        atlas_acquire.dstQueueFamilyIndex = mQueueFamily;
        atlas_acquire.image               = mShAtlas;
        atlas_acquire.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (masked)
        {
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &atlas_acquire);
        }
        std::vector<VkImageMemoryBarrier> acquires(images.size());
        for (size_t k = 0; k < images.size(); ++k)
        {
            VkImageMemoryBarrier& acquire = acquires[k];
            acquire = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            acquire.srcAccessMask       = 0;
            acquire.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            acquire.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            acquire.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            acquire.dstQueueFamilyIndex = mQueueFamily;
            acquire.image               = images[k]->image;
            acquire.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                             0, nullptr, 0, nullptr, (U32)acquires.size(), acquires.data());

        for (size_t i = 0; i < pending.size(); ++i)
        {
            const ShPending& p = pending[i];
            const ShSlot& slot = mShSlots[p.slot];
            const ShDepth* target = images[image_of[i]];
            bool drawn_before = false;
            for (size_t k = 0; k < i; ++k)
            {
                drawn_before = drawn_before || image_of[k] == image_of[i];
            }
            if (drawn_before)
            {
                // The same map's second pass: the first's writes before these tests.
                VkMemoryBarrier depth_order = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                depth_order.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depth_order.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     0, 1, &depth_order, 0, nullptr, 0, nullptr);
            }
            VkRenderPassBeginInfo pass_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            pass_begin.renderPass  = mShPass;
            pass_begin.framebuffer = target->framebuffer;
            pass_begin.renderArea  = { { 0, 0 }, { p.width, p.height } };
            vkCmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
            const VkViewport viewport = { 0.f, 0.f, (float)p.width, (float)p.height, 0.f, 1.f };
            const VkRect2D scissor = { { 0, 0 }, { p.width, p.height } };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mShLayout, 0, 1, &slot.set, 0, nullptr);
            const float flags[4] = { p.remap ? 1.f : 0.f, 0.f, 0.f, 0.f };
            vkCmdPushConstants(cmd, mShLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(flags), flags);
            const VkDeviceSize commands = SH_SLOT_BYTES * p.slot + SH_MVP_BYTES;
            const VkDrawIndexedIndirectCommand* cmds =
                reinterpret_cast<const VkDrawIndexedIndirectCommand*>(mShHostMapped + commands);
            const VkPipeline pipelines[SH_KINDS] = { mShPipelines[p.variant], mShSkinPipelines[p.variant],
                                                     mShMaskPipelines[p.variant], mShMaskSkinPipelines[p.variant] };
            // Each kind: its pipeline, its arena's streams as the pipeline's bindings take them (positions, weights
            // where rigged, texture coordinates and colours where masked), its range of the slot's draws, by one
            // indirect draw where the device can.
            for (U32 kind = 0; kind < SH_KINDS; ++kind)
            {
                const U32 first = p.kindFirst[kind];
                const U32 count = p.kindFirst[kind + 1] - first;
                if (count == 0)
                {
                    continue;
                }
                const ShArena& arena = *arenas[kind];
                VkBuffer buffers[4];
                VkDeviceSize offsets[4];
                U32 streams = 0;
                buffers[streams] = arena.buffer;
                offsets[streams++] = 0;
                if (arena.weightBytes)
                {
                    buffers[streams] = arena.buffer;
                    offsets[streams++] = arena.vertexBytes;
                }
                if (arena.uvBytes)
                {
                    buffers[streams] = arena.buffer;
                    offsets[streams++] = arena.uvStart();
                    buffers[streams] = arena.buffer;
                    offsets[streams++] = arena.colorStart();
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[kind]);
                vkCmdBindVertexBuffers(cmd, 0, streams, buffers, offsets);
                vkCmdBindIndexBuffer(cmd, arena.buffer, arena.indexStart(), VK_INDEX_TYPE_UINT16);
                if (mHasMultiIndirect)
                {
                    vkCmdDrawIndexedIndirect(cmd, mShHost, commands + (VkDeviceSize)first * sizeof(*cmds), count,
                                             sizeof(VkDrawIndexedIndirectCommand));
                }
                else
                {
                    for (U32 d = first; d < first + count; ++d)
                    {
                        vkCmdDrawIndexed(cmd, cmds[d].indexCount, 1, cmds[d].firstIndex, cmds[d].vertexOffset, d);
                    }
                }
            }
            vkCmdEndRenderPass(cmd);
        }

        for (U32 a = 0; a < arena_count; ++a)
        {
            bufferBarrier(cmd, arenas[a]->buffer, 0, 0, mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        if (masked)
        {
            VkImageMemoryBarrier atlas_release = atlas_acquire;
            atlas_release.srcAccessMask       = 0;
            atlas_release.dstAccessMask       = 0;
            atlas_release.srcQueueFamilyIndex = mQueueFamily;
            atlas_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &atlas_release);
        }
        std::vector<VkImageMemoryBarrier> releases = acquires;
        for (VkImageMemoryBarrier& release : releases)
        {
            release.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            release.dstAccessMask       = 0;
            release.srcQueueFamilyIndex = mQueueFamily;
            release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, (U32)releases.size(), releases.data());
        vkEndCommandBuffer(cmd);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &mShGLDoneVK;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &mShVKDoneVK;
        VkResult result = VK_SUCCESS;
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - vk submit");
            result = vkQueueSubmit(mQueue, 1, &submit, head.fence);
        }
        for (const ShPending& p : pending)
        {
            mShSlots[p.slot].pending = false;
        }
        if (result != VK_SUCCESS)
        {
            // GL's signal is left untaken: everything is let go, waiting for both first, and not tried again. These
            // maps miss Vulkan's casters this once.
            shFail(llformat("shadow maps could not be submitted (VkResult %d)", (S32)result));
            return false;
        }
        const U64 serial = ++mShSubmitSerial;
        for (const ShPending& p : pending)
        {
            mShSlots[p.slot].serial    = serial;
            mShSlots[p.slot].submitted = false;  // only the head's fence is this submission's
        }
        head.submitted = true;
        if (rigged)
        {
            mShSegmentSerial[mShFrame % SH_PALETTE_SEGMENTS] = serial;
        }

        // GL: back once Vulkan is done (on the GPU), and each map into GL's own texture.
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - gl wait");
            if (sShSignalObjects)
            {
                gl.WaitSemaphoreEXT(mShVKDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(),
                                    (GLuint)gl_textures.size(), gl_textures.data(), layouts.data());
            }
            else
            {
                gl.WaitSemaphoreEXT(mShVKDoneGL, 0, nullptr, 0, nullptr, nullptr);
            }
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - gl copy out");
            for (size_t i = 0; i < pending.size(); ++i)
            {
                bool last = true;
                for (size_t k = i + 1; k < pending.size(); ++k)
                {
                    last = last && image_of[k] != image_of[i];
                }
                if (last)
                {
                    const ShPending& p = pending[i];
                    gl.CopyImageSubData(images[image_of[i]]->glTexture, GL_TEXTURE_2D, 0, 0, 0, 0, p.depth,
                                        GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)p.width, (GLsizei)p.height, 1);
                }
            }
        }
        ++mShFlushesNow;
        mShPassesNow += (U32)pending.size();
        mShDrawsNow += draws;
        mShRiggedNow += rigged_draws;
        mShMaskedNow += masked_draws;
        mShMaskedRiggedNow += masked_rigged_draws;
        return true;
    }

    // Leaves the pass without drawing it (the caller draws it with GL).
    void shadowAbandon()
    {
        mShInPass = false;
    }

    std::string shadowInfo() const
    {
        if (!mShReady)
        {
            return mShNote;
        }
        std::string info =
            llformat("%u maps in %u hand-overs, %u draws (%u rigged) last frame | arenas %.0f/%.0f + %.0f/%.0f MB, %.1f MB copied, "
                     "%u+%u resets, %u palettes",
                     mShPassesLast, mShFlushesLast, mShDrawsLast, mShRiggedLast, (double)mShStatic.used() / (1 << 20),
                     (double)mShStatic.totalBytes() / (1 << 20), (double)mShRigged.used() / (1 << 20),
                     (double)mShRigged.totalBytes() / (1 << 20), (double)mShCopiedLast / (1 << 20), mShStatic.resets,
                     mShRigged.resets, mShPaletteCountLast);
        // The masked casters first: the line is long, and the screen cuts its end.
        if (!mShMaskReady)
        {
            return "masked: " + mShMaskNote + " | " + info;
        }
        return llformat("masked %u (%u rigged), atlas %u/%u, %u new, arenas %.0f + %.0f MB, %u+%u resets | ",
                        mShMaskedLast + mShMaskedRiggedLast, mShMaskedRiggedLast, mShTilesTaken, SH_TILES,
                        mShTileUploadsLast, (double)mShMasked.used() / (1 << 20),
                        (double)mShMaskedRigged.used() / (1 << 20), mShMasked.resets, mShMaskedRigged.resets) +
               info;
    }

    // ---- the interface layer (QuasarUILayer) ----------------------------------------------------------------

    // GL draws the viewer's 2D interface into a layer of its own (newview's QLUILayer), premultiplied, and hands it over
    // before the swap; copyBackBuffer copies it, turned over to Vulkan's rows, into mUI with the frame, and the present
    // lays it over the frame (qlcomposite.comp) before the frame goes into the swapchain. The same image is DLSS Frame
    // Generation's UI. mUI takes turns with GL through the presentation's own two semaphores, as the inputs do.
    FGImage               mUI;
    GLuint                mUIPendingTexture = 0;
    U32                   mUIPendingWidth = 0;
    U32                   mUIPendingHeight = 0;
    bool                  mUIThisFrame = false;
    VkShaderModule        mCompModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout mCompSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCompLayout = VK_NULL_HANDLE;
    VkPipeline            mCompPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      mCompPool = VK_NULL_HANDLE;
    VkDescriptorSet       mCompSet = VK_NULL_HANDLE;
    VkImageView           mSharedView = VK_NULL_HANDLE;  // the presentation image, for the composite to write
    bool                  mCompReady = false;
    bool                  mCompFailed = false;
    bool                  mCompDirty = true;  // its descriptors name images made again since
    U64                   mUIFrames = 0;
    std::string           mUINote = "not used yet";
    bool                  mFGRecompose = false;  // as Streamline was last told (enableUserInterfaceRecomposition)

    bool compFail(const std::string& why)
    {
        mUINote = "failed: " + why;
        mCompFailed = true;
        LL_WARNS("Vulkan") << "Vulkan interface layer: " << why << LL_ENDL;
        return false;
    }

    bool createCompositePipeline()
    {
        if (mCompReady || mCompFailed)
        {
            return mCompReady;
        }
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(mPhysical, VK_FORMAT_R8G8B8A8_UNORM, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        {
            return compFail("RGBA8 images cannot be written by a compute shader here");
        }
        VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module.codeSize = sizeof(ql_spv_qlcomposite_comp);
        module.pCode    = ql_spv_qlcomposite_comp;
        VkResult result = vkCreateShaderModule(mDevice, &module, nullptr, &mCompModule);
        if (result != VK_SUCCESS)
        {
            mCompModule = VK_NULL_HANDLE;
            return compFail(llformat("vkCreateShaderModule (composite) failed (VkResult %d)", (S32)result));
        }
        VkDescriptorSetLayoutBinding bindings[2] = {};
        for (U32 i = 0; i < 2; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 2;
        set_info.pBindings    = bindings;
        result = vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mCompSetLayout);
        if (result != VK_SUCCESS)
        {
            mCompSetLayout = VK_NULL_HANDLE;
            return compFail(llformat("vkCreateDescriptorSetLayout (composite) failed (VkResult %d)", (S32)result));
        }
        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = 8;
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount         = 1;
        layout_info.pSetLayouts            = &mCompSetLayout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &push;
        result = vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &mCompLayout);
        if (result != VK_SUCCESS)
        {
            mCompLayout = VK_NULL_HANDLE;
            return compFail(llformat("vkCreatePipelineLayout (composite) failed (VkResult %d)", (S32)result));
        }
        VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipe.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipe.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        pipe.stage.module = mCompModule;
        pipe.stage.pName  = "main";
        pipe.layout       = mCompLayout;
        result = vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &pipe, nullptr, &mCompPipeline);
        if (result != VK_SUCCESS)
        {
            mCompPipeline = VK_NULL_HANDLE;
            return compFail(llformat("vkCreateComputePipelines (composite) failed (VkResult %d)", (S32)result));
        }
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
        VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets       = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes    = &size;
        result = vkCreateDescriptorPool(mDevice, &pool, nullptr, &mCompPool);
        if (result != VK_SUCCESS)
        {
            mCompPool = VK_NULL_HANDLE;
            return compFail(llformat("vkCreateDescriptorPool (composite) failed (VkResult %d)", (S32)result));
        }
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool     = mCompPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &mCompSetLayout;
        result = vkAllocateDescriptorSets(mDevice, &alloc, &mCompSet);
        if (result != VK_SUCCESS)
        {
            mCompSet = VK_NULL_HANDLE;
            return compFail(llformat("vkAllocateDescriptorSets (composite) failed (VkResult %d)", (S32)result));
        }
        mCompReady = true;
        mCompDirty = true;
        mUINote    = "ready";
        LL_INFOS("Vulkan") << "Vulkan interface layer ready (composite compute pipeline)" << LL_ENDL;
        return true;
    }

    // The interface image at the window's size, made again only when it changes, with both APIs idle.
    bool ensureUIImage(U32 width, U32 height)
    {
        if (mUI.image && mUIWidth == width && mUIHeight == height)
        {
            return true;
        }
        waitIdle();
        destroyFGImage(mUI);
        mMakingPassImages = true;
        const bool made = createFGImage(mUI, VK_FORMAT_R8G8B8A8_UNORM, GL_RGBA8, width, height, "interface layer");
        mMakingPassImages = false;
        if (!made || !(mUI.usage & VK_IMAGE_USAGE_STORAGE_BIT))
        {
            destroyFGImage(mUI);
            return compFail("the interface image could not be made");
        }
        mUIWidth   = width;
        mUIHeight  = height;
        mCompDirty = true;
        return true;
    }
    U32 mUIWidth = 0;
    U32 mUIHeight = 0;

    // Names the presentation image and the interface image in the composite's descriptors, with nothing in flight.
    bool updateCompositeDescriptors()
    {
        if (!mCompDirty)
        {
            return true;
        }
        vkDeviceWaitIdle(mDevice);
        if (mSharedView)
        {
            vkDestroyImageView(mDevice, mSharedView, nullptr);
            mSharedView = VK_NULL_HANDLE;
        }
        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image            = mShared;
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(mDevice, &view, nullptr, &mSharedView) != VK_SUCCESS)
        {
            mSharedView = VK_NULL_HANDLE;
            return compFail("no view of the presentation image");
        }
        VkDescriptorImageInfo infos[2] = { { VK_NULL_HANDLE, mSharedView, VK_IMAGE_LAYOUT_GENERAL },
                                           { VK_NULL_HANDLE, mUI.view, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        for (U32 i = 0; i < 2; ++i)
        {
            writes[i].dstSet          = mCompSet;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(mDevice, 2, writes, 0, nullptr);
        mCompDirty = false;
        return true;
    }

    // In copyBackBuffer, with GL's wait for Vulkan done: this frame's interface layer, if one was handed over at the
    // window's size, into mUI; and, where frame generation's inputs are made, the back buffer as it is now (the world,
    // HUD attachments and 3D overlays, without the interface) as their HUD-less colour, so that the two add up to the
    // frame exactly.
    void copyInterfaceLayer(U32 width, U32 height)
    {
        mUIThisFrame = false;
        const GLuint texture = mUIPendingTexture;
        const bool fits = mUIPendingWidth == width && mUIPendingHeight == height;
        mUIPendingTexture = 0;
        if (!texture || !fits || !createCompositePipeline() || !ensureUIImage(width, height) || !mShared)
        {
            return;
        }
        const GLint w = (GLint)width;
        const GLint h = (GLint)height;
        if (!mGLPassReadFB)
        {
            gl.CreateFramebuffers(1, &mGLPassReadFB);
        }
        gl.NamedFramebufferTexture(mGLPassReadFB, GL_COLOR_ATTACHMENT0, texture, 0);
        gl.NamedFramebufferReadBuffer(mGLPassReadFB, GL_COLOR_ATTACHMENT0);
        gl.BlitNamedFramebuffer(mGLPassReadFB, mUI.glFramebuffer, 0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.NamedFramebufferTexture(mGLPassReadFB, GL_COLOR_ATTACHMENT0, 0, 0);
        if (mFG[FG_HUDLESS].glFramebuffer && mFGWidth == width && mFGHeight == height)
        {
            gl.NamedFramebufferReadBuffer(0, GL_BACK);
            gl.BlitNamedFramebuffer(0, mFG[FG_HUDLESS].glFramebuffer, 0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT,
                                    GL_NEAREST);
        }
        mUIThisFrame = true;
    }

    void destroyCompositeObjects()
    {
        destroyFGImage(mUI);
        mUIWidth = mUIHeight = 0;
        if (mSharedView)
        {
            vkDestroyImageView(mDevice, mSharedView, nullptr);
            mSharedView = VK_NULL_HANDLE;
        }
        if (mCompPool)
        {
            vkDestroyDescriptorPool(mDevice, mCompPool, nullptr);
            mCompPool = VK_NULL_HANDLE;
            mCompSet  = VK_NULL_HANDLE;
        }
        if (mCompPipeline)
        {
            vkDestroyPipeline(mDevice, mCompPipeline, nullptr);
            mCompPipeline = VK_NULL_HANDLE;
        }
        if (mCompLayout)
        {
            vkDestroyPipelineLayout(mDevice, mCompLayout, nullptr);
            mCompLayout = VK_NULL_HANDLE;
        }
        if (mCompSetLayout)
        {
            vkDestroyDescriptorSetLayout(mDevice, mCompSetLayout, nullptr);
            mCompSetLayout = VK_NULL_HANDLE;
        }
        if (mCompModule)
        {
            vkDestroyShaderModule(mDevice, mCompModule, nullptr);
            mCompModule = VK_NULL_HANDLE;
        }
        mCompReady = false;
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
        if (mUI.glTexture)
        {
            textures[count] = mUI.glTexture;
            layouts[count++] = GL_LAYOUT_GENERAL_EXT;
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
        GLuint textures[2 + FG_COUNT];
        GLenum layouts[2 + FG_COUNT];
        const U32 count = glInteropTextures(textures, layouts);
        gl.WaitSemaphoreEXT(mVKDoneGL, 0, nullptr, count, textures, layouts);
        mGLMustWait = false;
    }

    // A blit obeys the scissor test and, into an sRGB-capable buffer, the sRGB write: neither is wanted in a copy.
    // Whether either is on is read from what llrender has set (ql_gl_cap_tracked_enabled), never asked of GL: on
    // NVIDIA's threaded driver glIsEnabled waits for the driver's thread to run everything queued before it, and here,
    // at the end of the frame, that was most of it (~0.8 ms a frame in the frame generation inputs alone).
    struct GLCopyState
    {
        GLboolean scissor = GL_FALSE;
        GLboolean srgb = GL_FALSE;
    };
    GLCopyState beginGLCopy()
    {
        GLCopyState state;
        state.scissor = ql_gl_cap_tracked_enabled(GL_SCISSOR_TEST) ? GL_TRUE : GL_FALSE;
        state.srgb    = ql_gl_cap_tracked_enabled(GL_FRAMEBUFFER_SRGB) ? GL_TRUE : GL_FALSE;
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

        {
            // The frame's one wait for Vulkan's last read of the shared images (copyBackBuffer needs none after).
            LL_PROFILE_ZONE_NAMED("vulkan present - fg gl wait");
            glWaitForVulkan();
        }
        LL_PROFILE_ZONE_NAMED_COLOR("vulkan present - fg blits", 0xA040FF);
        const GLCopyState state = beginGLCopy();
        const GLint w = (GLint)width;
        const GLint h = (GLint)height;
        gl.NamedFramebufferReadBuffer(0, GL_BACK);
        gl.BlitNamedFramebuffer(0, mFG[FG_HUDLESS].glFramebuffer, 0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT,
                                GL_NEAREST);
        // The viewer's textures stay attached from frame to frame: attached again only when the viewer remade them, so
        // the driver does not check a framebuffer anew every frame. (A texture attached to a framebuffer that is not
        // bound lives on until it is let go of here.)
        // Once a second regardless: a texture the viewer deleted can hand its name to a new one, which the name alone
        // would not tell.
        if (++mGLReadAge >= 60)
        {
            mGLReadAge = 0;
            mGLReadDepth = mGLReadMotion = 0;
        }
        if (mGLReadDepth != depth_texture)
        {
            gl.NamedFramebufferTexture(mGLReadFramebuffer, GL_COLOR_ATTACHMENT0, depth_texture, 0);
            mGLReadDepth = depth_texture;
        }
        if (mGLReadMotion != motion_texture)
        {
            gl.NamedFramebufferTexture(mGLMotionFramebuffer, GL_COLOR_ATTACHMENT0, motion_texture, 0);
            mGLReadMotion = motion_texture;
        }
        gl.BlitNamedFramebuffer(mGLReadFramebuffer, mFG[FG_DEPTH].glFramebuffer, 0, 0, w, h, 0, h, w, 0,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.BlitNamedFramebuffer(mGLMotionFramebuffer, mFG[FG_MOTION].glFramebuffer, 0, 0, w, h, 0, h, w, 0,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
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
        // Render targets made from now on are GL's own; those already in Vulkan's memory keep GL's hold on it.
        if (mExtStorage && gQLExternalStorage == mExtStorage.get())
        {
            gQLExternalStorage = nullptr;
        }
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
            destroyCompositeObjects();
            destroyShadowObjects();
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
            mCompDirty = true;
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
        options.uiBufferFormat      = (uint32_t)VK_FORMAT_R8G8B8A8_UNORM;
        options.enableUserInterfaceRecomposition = mUIThisFrame ? sl::Boolean::eTrue : sl::Boolean::eFalse;
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
        mFGRecompose = on && mUIThisFrame;
        LL_INFOS("Vulkan") << "DLSS Frame Generation " << (on ? "on" : "off") << ", " << mSwapWidth << "x" << mSwapHeight
                           << (mFGRecompose ? ", the interface interpolated apart" : "") << LL_ENDL;
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
        if (want != mFGOn || (want && mFGRecompose != mUIThisFrame))
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
        // The interface layer, where it was handed over this frame: Streamline then interpolates the world and the
        // interface apart and lays the one over the other (enableUserInterfaceRecomposition).
        sl::Resource ui_resource(sl::ResourceType::eTex2d, (void*)mUI.image, (void*)mUI.memory, (void*)mUI.view,
                                 (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
        ui_resource.width        = mFGWidth;
        ui_resource.height       = mFGHeight;
        ui_resource.nativeFormat = (uint32_t)mUI.format;
        ui_resource.mipLevels    = 1;
        ui_resource.arrayLayers  = 1;
        ui_resource.flags        = 0;
        ui_resource.usage        = (uint32_t)mUI.usage;
        sl::ResourceTag all_tags[FG_COUNT + 1] = {
            tags[0], tags[1], tags[2],
            sl::ResourceTag(&ui_resource, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow, &extent),
        };
        const bool with_ui = mUIThisFrame && mUI.image && mUIWidth == mFGWidth && mUIHeight == mFGHeight;
        const sl::Result result = slSetTagForFrame(*mFrameToken, viewport(), all_tags, with_ui ? FG_COUNT + 1 : FG_COUNT,
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
        copyInterfaceLayer(width, height);
        endGLCopy(state);
        const GLboolean scissor = state.scissor;
        const GLboolean srgb    = state.srgb;

        // Every shared image, the frame generation inputs too: Vulkan takes them all with this frame.
        GLuint textures[2 + FG_COUNT];
        GLenum layouts[2 + FG_COUNT];
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
        extCollect();  // the images of render targets GL has deleted
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
        imageBarrier(cmd, mShared, 0,
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        imageBarrier(cmd, mSwapImages[index], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // The frame generation inputs and the interface layer, which GL signalled with the frame, every frame they exist,
        // read or not.
        const FGImage* handed[] = { &mFG[FG_DEPTH], &mFG[FG_MOTION], &mFG[FG_HUDLESS], &mUI };
        for (const FGImage* handed_img : handed)
        {
            const FGImage& img = *handed_img;
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

        // The interface laid over the frame, in place, before the frame is turned into the swapchain.
        if (mUIThisFrame && mCompReady && updateCompositeDescriptors())
        {
            const U32 size[2] = { width, height };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompLayout, 0, 1, &mCompSet, 0, nullptr);
            vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
            vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
            imageBarrier(cmd, mShared, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            ++mUIFrames;
            mUINote = "laid over by Vulkan";
        }

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
        for (const FGImage* handed_img : handed)
        {
            const FGImage& img = *handed_img;
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
bool QLVulkanPresenter::geometryTest()
{
    return sActive && sActive->geometryTest();
}

// static
std::string QLVulkanPresenter::geometryInfo()
{
    if (!sActive)
    {
        return "needs Vulkan presentation (QuasarVulkanPresent)";
    }
    return sActive->mGeoNote;
}

// static
bool QLVulkanPresenter::shadowBegin(unsigned width, unsigned height, bool clamp, bool greater, bool remap_depth)
{
    return sActive && sActive->shadowBegin(width, height, clamp, greater, remap_depth);
}

// static
bool QLVulkanPresenter::shadowArena(unsigned long long stamp, unsigned vertex_buffer, unsigned vertex_offset,
                                    unsigned vertex_count, unsigned index_buffer, unsigned index_count,
                                    unsigned& base_vertex, unsigned& base_index)
{
    U32 bv = 0, bi = 0;
    if (!sActive || !sActive->shadowArena((U64)stamp, (GLuint)vertex_buffer, vertex_offset, vertex_count,
                                          (GLuint)index_buffer, index_count, bv, bi))
    {
        return false;
    }
    base_vertex = bv;
    base_index  = bi;
    return true;
}

// static
bool QLVulkanPresenter::shadowDraw(const float mvp[16], unsigned base_vertex, unsigned first_index,
                                   unsigned index_count)
{
    return sActive && sActive->shadowDraw(mvp, base_vertex, first_index, index_count);
}

// static
bool QLVulkanPresenter::shadowArenaRigged(unsigned long long stamp, unsigned vertex_buffer, unsigned vertex_offset,
                                          unsigned weight_offset, unsigned vertex_count, unsigned index_buffer,
                                          unsigned index_count, unsigned& base_vertex, unsigned& base_index)
{
    U32 bv = 0, bi = 0;
    if (!sActive || !sActive->shadowArenaRigged((U64)stamp, (GLuint)vertex_buffer, vertex_offset, weight_offset,
                                                vertex_count, (GLuint)index_buffer, index_count, bv, bi))
    {
        return false;
    }
    base_vertex = bv;
    base_index  = bi;
    return true;
}

// static
unsigned QLVulkanPresenter::shadowPalette(unsigned long long avatar, unsigned long long skin, const float* floats,
                                         unsigned count)
{
    return sActive ? sActive->shadowPalette((U64)avatar, (U64)skin, floats, count) : ~0u;
}

// static
bool QLVulkanPresenter::shadowDrawRigged(const float mvp[16], unsigned palette, unsigned base_vertex,
                                         unsigned first_index, unsigned index_count)
{
    return sActive && sActive->shadowDrawRigged(mvp, palette, base_vertex, first_index, index_count);
}

// static
bool QLVulkanPresenter::shadowArenaMasked(bool rigged, unsigned long long stamp, unsigned vertex_buffer,
                                          unsigned vertex_offset, unsigned weight_offset, unsigned uv_offset,
                                          unsigned color_offset, unsigned vertex_count, unsigned index_buffer,
                                          unsigned index_count, unsigned& base_vertex, unsigned& base_index)
{
    return sActive && sActive->shadowArenaMasked(rigged, (U64)stamp, vertex_buffer, vertex_offset, weight_offset,
                                                 uv_offset, color_offset, vertex_count, index_buffer, index_count,
                                                 base_vertex, base_index);
}

// static
bool QLVulkanPresenter::shadowMaskTexture(unsigned texture, unsigned long long stamp, unsigned width, unsigned height,
                                          unsigned levels, bool has_alpha, unsigned& tile)
{
    return sActive && sActive->shadowMaskTexture(texture, (U64)stamp, width, height, levels, has_alpha, tile);
}

// static
bool QLVulkanPresenter::shadowDrawMasked(const float mvp[16], unsigned palette, unsigned tile,
                                         const float* texture_matrix, float cutoff, unsigned test,
                                         unsigned base_vertex, unsigned first_index, unsigned index_count)
{
    return sActive && sActive->shadowDrawMasked(mvp, palette, tile, texture_matrix, cutoff, test, base_vertex,
                                                first_index, index_count);
}

// static
bool QLVulkanPresenter::shadowEnd(unsigned depth_texture)
{
    return sActive && sActive->shadowEnd((GLuint)depth_texture);
}

// static
bool QLVulkanPresenter::shadowFlush()
{
    return !sActive || sActive->shadowFlush();
}

// static
void QLVulkanPresenter::shadowSignalObjects(bool name_them)
{
    Impl::sShSignalObjects = name_them;
}

// static
void QLVulkanPresenter::shadowAbandon()
{
    if (sActive)
    {
        sActive->shadowAbandon();
    }
}

// static
void QLVulkanPresenter::shadowFrame(unsigned frame)
{
    if (sActive)
    {
        sActive->shadowFrame(frame);
    }
}

// static
std::string QLVulkanPresenter::shadowInfo()
{
    if (!sActive)
    {
        return "needs Vulkan presentation (QuasarVulkanPresent)";
    }
    return sActive->shadowInfo();
}

// static
bool QLVulkanPresenter::depthTest()
{
    return sActive && sActive->depthTest();
}

// static
bool QLVulkanPresenter::targetTest()
{
    return sActive && sActive->targetTest();
}

// static
std::string QLVulkanPresenter::targetInfo()
{
    return sActive ? sActive->targetInfo() : std::string("targets: needs Vulkan presentation");
}

// static
std::string QLVulkanPresenter::depthInfo()
{
    if (!sActive)
    {
        return "needs Vulkan presentation (QuasarVulkanPresent)";
    }
    return sActive->mDepthNote;
}

// static
bool QLVulkanPresenter::uiLayerAvailable()
{
    return sActive && sActive->mActive && sActive->createCompositePipeline();
}

// static
void QLVulkanPresenter::uiLayer(unsigned texture, unsigned width, unsigned height)
{
    if (sActive)
    {
        sActive->mUIPendingTexture = (GLuint)texture;
        sActive->mUIPendingWidth   = (U32)width;
        sActive->mUIPendingHeight  = (U32)height;
    }
}

// static
std::string QLVulkanPresenter::uiLayerInfo()
{
    if (!sActive || !sActive->mActive)
    {
        return "needs Vulkan presentation (QuasarVulkanPresent)";
    }
    return sActive->mUINote + llformat(", %llu frames", (unsigned long long)sActive->mUIFrames);
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
