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
#include <set>
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
#include "qlvkglsl.h"  // GL's own programs compiled for Vulkan (QuasarVulkanLighting)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <array>
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
    typedef void (APIENTRY* QL_PFN_TextureStorageMem3D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLuint,
                                                        GLuint64);
    typedef void (APIENTRY* QL_PFN_ImportSemaphoreWin32Handle)(GLuint, GLenum, void*);
    typedef void (APIENTRY* QL_PFN_SemaphoreOp)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    typedef void (APIENTRY* QL_PFN_GetUnsignedBytei_v)(GLenum, GLuint, GLubyte*);
    typedef void (APIENTRY* QL_PFN_TextureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
    typedef void (APIENTRY* QL_PFN_CopyNamedBufferSubData)(GLuint, GLuint, GLintptr, GLintptr, GLsizeiptr);
    typedef void (APIENTRY* QL_PFN_NamedBufferStorageMemEXT)(GLuint, GLsizeiptr, GLuint, GLuint64);
    typedef void (APIENTRY* QL_PFN_GetTextureParameteriv)(GLuint, GLenum, GLint*);
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
        QL_PFN_TextureStorageMem3D          TextureStorageMem3DEXT = nullptr;  // optional: the probes' cube arrays
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
        QL_PFN_GetTextureParameteriv        GetTextureParameteriv = nullptr;
        QL_PFN_NamedFramebufferReadBuffer   NamedFramebufferDrawBuffer = nullptr;
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
            TextureStorageMem3DEXT = reinterpret_cast<decltype(TextureStorageMem3DEXT)>(
                LLWindowWin32::getProcAddress("glTextureStorageMem3DEXT"));
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
            QL_GL_LOAD(GetTextureParameteriv, "glGetTextureParameteriv");
            QL_GL_LOAD(NamedFramebufferDrawBuffer, "glNamedFramebufferDrawBuffer");
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
    QL_VK_FN(vkGetPhysicalDeviceImageFormatProperties2);  // optional (Vulkan 1.1): asked, never required
    QL_VK_FN(vkGetPhysicalDeviceMemoryProperties2);       // optional (Vulkan 1.1): the memory budget
    QL_VK_FN(vkGetImageMemoryRequirements2);              // optional (Vulkan 1.1)
    QL_VK_FN(vkFreeCommandBuffers);
    QL_VK_FN(vkFreeDescriptorSets);
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
    QL_VK_FN(vkResetDescriptorPool);
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
    QL_VK_FN(vkCmdSetDepthBounds);
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
        vkGetPhysicalDeviceImageFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            vkGetInstanceProcAddr(mInstance, "vkGetPhysicalDeviceImageFormatProperties2"));
        vkGetPhysicalDeviceMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            vkGetInstanceProcAddr(mInstance, "vkGetPhysicalDeviceMemoryProperties2"));
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
        vkGetImageMemoryRequirements2 = reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(
            vkGetDeviceProcAddr(mDevice, "vkGetImageMemoryRequirements2"));
        QL_VK_DEVICE(vkFreeCommandBuffers);
        QL_VK_DEVICE(vkFreeDescriptorSets);
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
        QL_VK_DEVICE(vkResetDescriptorPool);
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
        QL_VK_DEVICE(vkCmdSetDepthBounds);
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
    bool mHasDepthBounds = false;
    bool mHasAniso = false;
    bool mHasMultiIndirect = false;
    bool mHasMemoryBudget = false;

    // Every 30 frames: the process's video memory use and budget over the device-local heaps, for the texture budget
    // (gQLVideoMemoryUsedBytes, gQLVideoMemoryBudgetBytes; LLViewerTexture::updateClass).
    void pollMemoryBudget()
    {
        if (!mHasMemoryBudget || (mFrame % 30) != 0)
        {
            return;
        }
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
        VkPhysicalDeviceMemoryProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
        props.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(mPhysical, &props);
        // The largest device-local heap: the card's memory (a small BAR heap beside it may overlap it).
        U64 used = 0, total = 0, largest = 0;
        for (U32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap& heap = props.memoryProperties.memoryHeaps[i];
            if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && heap.size > largest)
            {
                largest = heap.size;
                used    = budget.heapUsage[i];
                total   = budget.heapBudget[i];
            }
        }
        // Windows counts the memory Vulkan exports and GL imports twice in the process's use (Task Manager's dedicated
        // memory, the real figure, stays gigabytes under it): the texture budget is given the use with it counted once.
        // Streamline reads the raw figure (QueryVideoMemoryInfo), which the status line shows too.
        const U64 shared = mExtBytes + mTexBlockBytes;
        mVramRawUsed = used;
        gQLVideoMemoryUsedBytes.store(used > shared ? used - shared : 0, std::memory_order_relaxed);
        gQLVideoMemoryBudgetBytes.store(total, std::memory_order_relaxed);
    }
    U64 mVramRawUsed = 0;

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
        // Optional: the process's video memory use and the system's budget for it (pollMemoryBudget).
        std::vector<const char*> enabled_extensions(std::begin(wanted), std::end(wanted));
        for (const VkExtensionProperties& p : props)
        {
            if (strcmp(p.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0 && vkGetPhysicalDeviceMemoryProperties2)
            {
                enabled_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                mHasMemoryBudget = true;
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
        enabled.samplerAnisotropy         = supported.samplerAnisotropy;  // the G-buffer's textures, as GL filters them
        enabled.tessellationShader        = supported.tessellationShader;  // the terrain Vulkan draws
        // GL's own programs drawn by Vulkan (QuasarVulkanLighting): the lighting's depth bounds (ALDrawnOnly), the
        // probes' cube arrays, blending per attachment.
        enabled.depthBounds               = supported.depthBounds;
        enabled.imageCubeArray            = supported.imageCubeArray;
        enabled.independentBlend          = supported.independentBlend;
        mHasDepthBounds  = enabled.depthBounds == VK_TRUE;
        mHasAniso        = enabled.samplerAnisotropy == VK_TRUE;
        mHasTessellation = enabled.tessellationShader == VK_TRUE;
        mHasDepthClamp   = enabled.depthClamp == VK_TRUE;
        mHasMultiIndirect = enabled.multiDrawIndirect == VK_TRUE && enabled.drawIndirectFirstInstance == VK_TRUE;

        VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.queueCreateInfoCount    = 1;
        info.pQueueCreateInfos       = &queue;
        info.enabledExtensionCount   = (U32)enabled_extensions.size();
        info.ppEnabledExtensionNames = enabled_extensions.data();
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
        bool     integer;  // read and cleared as unsigned integers (the probes' SH coefficients)
    };
    static constexpr ExtFormat EXT_FORMATS[] = {
        { GL_SRGB8_ALPHA8, VK_FORMAT_R8G8B8A8_SRGB, 4, false, false, true, "SRGB8_ALPHA8", false },
        { GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4, false, false, false, "RGBA8", false },
        { GL_RGB10_A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, false, false, false, "RGB10_A2", false },
        { GL_RGBA16, VK_FORMAT_R16G16B16A16_UNORM, 4, false, false, false, "RGBA16", false },
        { GL_R11F_G11F_B10F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 3, false, false, false, "R11F_G11F_B10F", false },
        { GL_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 4, false, false, false, "RGBA16F", false },
        { GL_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 4, false, false, false, "RGBA32F", false },
        { GL_RG16F, VK_FORMAT_R16G16_SFLOAT, 2, false, false, false, "RG16F", false },
        { GL_RG8, VK_FORMAT_R8G8_UNORM, 2, false, false, false, "RG8", false },
        { GL_R8, VK_FORMAT_R8_UNORM, 1, false, false, false, "R8", false },
        { GL_DEPTH_COMPONENT32F, VK_FORMAT_D32_SFLOAT, 0, true, false, false, "DEPTH32F", false },
        { GL_DEPTH_COMPONENT24, VK_FORMAT_X8_D24_UNORM_PACK32, 0, true, false, false, "DEPTH24", false },
        { GL_DEPTH24_STENCIL8, VK_FORMAT_D24_UNORM_S8_UINT, 0, true, true, false, "DEPTH24_STENCIL8", false },
        { GL_DEPTH32F_STENCIL8, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, true, true, false, "DEPTH32F_STENCIL8", false },
        { GL_RGBA32UI, VK_FORMAT_R32G32B32A32_UINT, 4, false, false, false, "RGBA32UI", true },
    };
    static constexpr U32 EXT_FORMAT_COUNT = (U32)(sizeof(EXT_FORMATS) / sizeof(EXT_FORMATS[0]));

    struct ExtTexture
    {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        HANDLE         handle = nullptr;
        GLuint         glMemory = 0;
        U32            format = 0;  // index into EXT_FORMATS; U32_MAX for a fetched texture in a shared block
        U32            width = 0, height = 0, levels = 0;
        U32            layers = 1;     // a cube map array's faces (six a cube); 1 otherwise
        bool           cube = false;   // a cube map array (the probes')
        bool           mutable_format = false;  // made viewable as its linear twin (gpLinearTwin)
        S32            block = -1;  // a fetched texture's block (mTexBlocks), its place and size there
        VkDeviceSize   offset = 0, size = 0;
        U64            counted = 0;  // what the texture budget counts it as
        VkFormat       vk = VK_FORMAT_UNDEFINED;   // the image's format
        VkImageView    view = VK_NULL_HANDLE;      // made the first time Vulkan draws with it or into it
        VkDescriptorSet texSet = VK_NULL_HANDLE;   // a texture's set for the G-buffer draws (mGbTexPool)
    };

    // What llrender talks to: allocation on the main thread, deletions from any (queued for the main thread).
    // It outlives the presenter (never deleted: llrender's threads may hold the pointer); shutdown clears its owner under
    // sExtOwnerMutex, which every call holds, so no call runs into a presenter going or gone.
    class ExtStorage : public QLExternalStorage
    {
    public:
        explicit ExtStorage(Impl& owner) : mOwnerPtr(&owner) {}
        void close()
        {
            std::lock_guard<std::mutex> lock(sExtOwnerMutex);
            mOwnerPtr = nullptr;
        }
        bool allocate(unsigned texture, unsigned target, int internal_format, int width, int height,
                      int levels) override
        {
            std::lock_guard<std::mutex> lock(sExtOwnerMutex);
            const bool given = mOwnerPtr && mOwnerPtr->extAllocate(texture, target, internal_format, width, height, levels);
            // Render targets are made rarely (as the render inputs change): every one is logged.
            LL_INFOS("Vulkan") << "Render target in Vulkan's memory: GL name " << texture << ", " << width << "x" << height
                               << " format 0x" << std::hex << internal_format << std::dec << ", " << levels
                               << " levels: " << (given ? "given" : "declined, GL's own") << LL_ENDL;
            return given;
        }
        void deleted(int count, const unsigned* textures) override
        {
            std::lock_guard<std::mutex> owner_lock(sExtOwnerMutex);
            if (!mOwnerPtr)
            {
                return;
            }
            Impl& mOwner = *mOwnerPtr;
            // Out of the map at once, by value: GL may hand the name out again before the next present, to a target
            // given Vulkan's memory anew.
            std::lock_guard<std::mutex> lock(mOwner.mExtMutex);
            for (int i = 0; i < count; ++i)
            {
                auto found = mOwner.mExtTextures.find(textures[i]);
                if (found != mOwner.mExtTextures.end())
                {
                    mOwner.mExtFreeing.emplace_back(mOwner.mExtPresents, found->second);
                    mOwner.mExtTextures.erase(found);
                }
            }
        }
        bool allocateArray(unsigned texture, unsigned target, int internal_format, int width, int height, int layers,
                           int levels) override
        {
            std::lock_guard<std::mutex> lock(sExtOwnerMutex);
            const bool given = mOwnerPtr && mOwnerPtr->extAllocate(texture, target, internal_format, width, height,
                                                                   levels, true, (U32)layers);
            LL_INFOS("Vulkan") << "Cube map array in Vulkan's memory: GL name " << texture << ", " << width << "x"
                               << height << " x" << layers << " format 0x" << std::hex << internal_format << std::dec
                               << ", " << levels << " levels: " << (given ? "given" : "declined, GL's own") << LL_ENDL;
            return given;
        }
        bool allocateTexture(unsigned texture, int internal_format, int width, int height, int levels,
                             unsigned long long counted_bytes) override
        {
            std::lock_guard<std::mutex> lock(sExtOwnerMutex);
            return mOwnerPtr && mOwnerPtr->texAllocate(texture, internal_format, width, height, levels, counted_bytes);
        }
        bool owns(unsigned texture) const override
        {
            std::lock_guard<std::mutex> owner_lock(sExtOwnerMutex);
            if (!mOwnerPtr)
            {
                return false;
            }
            std::lock_guard<std::mutex> lock(mOwnerPtr->mExtMutex);
            return mOwnerPtr->mExtTextures.count(texture) != 0;
        }

    private:
        Impl* mOwnerPtr;
    };
    static inline std::mutex sExtOwnerMutex;  // held by every ExtStorage call, and by shutdown as it closes it

    mutable std::mutex mExtMutex;
    std::unordered_map<GLuint, ExtTexture> mExtTextures;
    // Taken out of mExtTextures as GL deleted their names, with the present they were taken at: freed (their images, their
    // ranges back to their blocks) EXT_FREE_PRESENTS presents on, when GL's GPU is past every frame that could read them
    // (the pacing depth is 3; GL's deferred delete is a frame).
    static constexpr U64 EXT_FREE_PRESENTS = 8;
    std::vector<std::pair<U64, ExtTexture>> mExtFreeing;
    U64         mExtPresents = 0;
    bool        mExtKeptAlive = false;    // shut down while GL still held some: the device was kept for them

    size_t extLiveCount() const
    {
        size_t blocks = 0;
        {
            std::lock_guard<std::mutex> tex_lock(mTexMutex);
            for (const TexBlock& b : mTexBlocks)
            {
                blocks += b.memory ? 1 : 0;
            }
        }
        std::lock_guard<std::mutex> lock(mExtMutex);
        return mExtTextures.size() + mExtFreeing.size() + blocks;
    }
    bool        mExtFormatOK[EXT_FORMAT_COUNT] = {};
    bool        mExtTested = false;
    bool        mExtPassed = false;  // at least the G-buffer's formats passed
    std::string mExtNote = "not tested yet (QuasarVulkanTargets)";
    ExtStorage* mExtStorage = nullptr;  // never deleted (see ExtStorage)
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
                     GLsizei levels, bool tested_only = true, U32 layers = 1)
    {
        // A 2D texture, or a cube map array of `layers` faces (QuasarVulkanLighting: the probes').
        const bool cube = target == GL_TEXTURE_CUBE_MAP_ARRAY;
        if (!mActive || !mDevice || (target != GL_TEXTURE_2D && !cube) || width <= 0 || height <= 0 || levels <= 0 ||
            (cube && (layers == 0 || layers % 6 != 0 || !gl.TextureStorageMem3DEXT)) || (!cube && layers != 1))
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
        ext.vk     = format.vk;
        ext.width  = (U32)width;
        ext.height = (U32)height;
        ext.levels = (U32)levels;
        ext.layers = layers;
        ext.cube   = cube;
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
        info.arrayLayers   = layers;
        info.flags         = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
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
        gl.CreateMemoryObjectsEXT(1, &ext.glMemory);
        const GLint dedicated_flag = GL_TRUE;
        gl.MemoryObjectParameterivEXT(ext.glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_flag);
        gl.ImportMemoryWin32HandleEXT(ext.glMemory, req.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        gl.TextureParameteri(texture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        if (cube)
        {
            gl.TextureStorageMem3DEXT(texture, levels, (GLenum)internal_format, width, height, (GLsizei)layers,
                                      ext.glMemory, 0);
        }
        else
        {
            gl.TextureStorageMem2DEXT(texture, levels, (GLenum)internal_format, width, height, ext.glMemory, 0);
        }
        // Asked once an allocation, off the frame's path (render targets are made as the render inputs change): every
        // flag these calls raised is taken, so none is left for llrender's own check of the allocation, and whether
        // the texture has storage now decides, not the flags.
        GLenum error = GL_NO_ERROR;
        for (GLenum e = gl.GetError(); e != GL_NO_ERROR; e = gl.GetError())
        {
            error = error == GL_NO_ERROR ? e : error;
        }
        GLint immutable = GL_FALSE;
        gl.GetTextureParameteriv(texture, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
        while (gl.GetError() != GL_NO_ERROR)
        {
        }
        if (!immutable)
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
        // What Vulkan made to draw with it or into it goes first (the G-buffer draws, gbuffer*; GL's programs, gp*).
        gbForget(ext);
        gpForget(ext.image);
        if (ext.view) vkDestroyImageView(mDevice, ext.view, nullptr);
        if (ext.texSet && mGbTexPool) vkFreeDescriptorSets(mDevice, mGbTexPool, 1, &ext.texSet);
        ext.view = VK_NULL_HANDLE;
        ext.texSet = VK_NULL_HANDLE;
        std::unique_lock<std::mutex> tex_lock(mTexMutex);
        if (ext.block >= 0 && ext.block < (S32)mTexBlocks.size())
        {
            // A fetched texture: its image goes, its range goes back to its block, and a block left empty goes too
            // while another remains.
            if (ext.image) vkDestroyImage(mDevice, ext.image, nullptr);
            TexBlock& block = mTexBlocks[ext.block];
            giveRange(block, ext.offset, ext.size);
            mTexBytes -= llmin(mTexBytes, (U64)ext.size);
            mTexCounted -= llmin(mTexCounted, ext.counted);
            size_t live = 0;
            for (const TexBlock& b : mTexBlocks)
            {
                live += b.memory ? 1 : 0;
            }
            if (block.used == 0 && live > 1)
            {
                if (block.glMemory) gl.DeleteMemoryObjectsEXT(1, &block.glMemory);
                vkFreeMemory(mDevice, block.memory, nullptr);
                if (block.handle) CloseHandle(block.handle);
                mTexBlockBytes -= llmin(mTexBlockBytes, (U64)block.size);
                block = TexBlock();
            }
            publishTexOverhead();
            ext = ExtTexture();
            return;
        }
        tex_lock.unlock();
        if (ext.glMemory) gl.DeleteMemoryObjectsEXT(1, &ext.glMemory);
        if (ext.image) vkDestroyImage(mDevice, ext.image, nullptr);
        if (ext.memory) vkFreeMemory(mDevice, ext.memory, nullptr);
        if (ext.handle) CloseHandle(ext.handle);
        ext = ExtTexture();
    }

    // The images of the names GL has deleted, on the main thread (each present).
    void extCollect()
    {
        std::vector<ExtTexture> freeing;
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            ++mExtPresents;
            auto due = std::stable_partition(mExtFreeing.begin(), mExtFreeing.end(),
                                             [this](const std::pair<U64, ExtTexture>& f)
                                             { return mExtPresents < f.first + EXT_FREE_PRESENTS; });
            for (auto it = due; it != mExtFreeing.end(); ++it)
            {
                freeing.push_back(it->second);
            }
            mExtFreeing.erase(due, mExtFreeing.end());
        }
        for (ExtTexture& ext : freeing)
        {
            if (ext.block < 0)
            {
                VkMemoryRequirements req = {};
                vkGetImageMemoryRequirements(mDevice, ext.image, &req);
                mExtBytes -= llmin(mExtBytes, (U64)req.size);
            }
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
        constexpr U32 CLEAR_UINT[4] = { 7u, 42u, 1000u, 0x80000001u };
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
            gl.Finish();  // GL's signal and wait done before their semaphores go
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
                if (format.integer)
                {
                    memcpy(value.uint32, CLEAR_UINT, sizeof(CLEAR_UINT));
                }
                else
                {
                    memcpy(value.float32, CLEAR, sizeof(CLEAR));
                }
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

        // Each must be one GL can draw into: attached to a framebuffer, complete.
        bool drawable[EXT_FORMAT_COUNT] = {};
        GLuint fbo = 0;
        gl.CreateFramebuffers(1, &fbo);
        for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
        {
            if (!made[i])
            {
                continue;
            }
            const ExtFormat& format = EXT_FORMATS[i];
            const GLenum attachment = !format.depth ? GL_COLOR_ATTACHMENT0
                                                    : (format.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT);
            gl.NamedFramebufferTexture(fbo, attachment, textures[i], 0);
            gl.NamedFramebufferDrawBuffer(fbo, format.depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
            gl.NamedFramebufferReadBuffer(fbo, format.depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
            drawable[i] = gl.CheckNamedFramebufferStatus(fbo, GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            gl.NamedFramebufferTexture(fbo, attachment, 0, 0);
        }
        gl.DeleteFramebuffers(1, &fbo);

        // Each read back where GL's texel (8, 8) is: colour as floats, depth as a float. An sRGB format may come back
        // as stored or decoded: either passes.
        std::string results;
        U32 passed = 0;
        for (U32 i = 0; i < EXT_FORMAT_COUNT; ++i)
        {
            const ExtFormat& format = EXT_FORMATS[i];
            bool ok = false;
            std::string got;
            if (made[i] && !drawable[i])
            {
                got = "no complete framebuffer";
            }
            else if (made[i])
            {
                if (format.integer)
                {
                    U32 rgba[4] = {};
                    gl.GetTextureSubImage(textures[i], 0, (GLint)SIZE / 2, (GLint)SIZE / 2, 0, 1, 1, 1,
                                          GL_RGBA_INTEGER, GL_UNSIGNED_INT, sizeof(rgba), rgba);
                    ok = memcmp(rgba, CLEAR_UINT, sizeof(rgba)) == 0;
                    got = llformat("%u %u %u %u", rgba[0], rgba[1], rgba[2], rgba[3]);
                }
                else if (format.depth)
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
            const std::string verdict = !made[i] ? std::string("not made") : (ok ? std::string("ok") : "WRONG (" + got + ")");
            results += llformat("%s%s %s", results.empty() ? "" : ", ", format.name, verdict.c_str());
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
            mExtStorage = new ExtStorage(*this);
            gQLExternalStorage = mExtStorage;
            textureTest();  // piece 2a: whether textures could share Vulkan's memory (nothing uses the answer yet)
        }
        return mExtPassed;
    }

    // ---- textures in Vulkan's memory: can they share it? (piece 2a, with the render target self-test) ---------
    //
    // A region holds thousands of textures, and a device allows a few thousand allocations at most
    // (maxMemoryAllocationCount: 4096 on NVIDIA's Windows driver): textures in Vulkan's memory must share it, many
    // images placed in one allocation and GL taking each at its offset. That needs the formats to be exportable
    // without a dedicated allocation, and GL to take a texture's storage from part of a memory object. Asked and tried
    // once: the formats' external properties, and two textures placed in one allocation, cleared by Vulkan to
    // different values, read back by GL.
    std::string mTexNote = "not tested";
    bool        mTexShared = false;  // textures can share an allocation

    bool textureTest()
    {
        LL_PROFILE_ZONE_NAMED("vulkan texture memory self-test");
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(mPhysical, &props);
        std::string report = llformat("maxMemoryAllocationCount %u", props.limits.maxMemoryAllocationCount);
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        // What each texture format's external memory allows, as the device reports it.
        struct TexFormat
        {
            VkFormat    vk;
            const char* name;
        };
        static const TexFormat formats[] = {
            { VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" },
            { VK_FORMAT_R8G8B8A8_SRGB, "SRGB8_ALPHA8" },
            { VK_FORMAT_R8_UNORM, "R8" },
            { VK_FORMAT_R8G8_UNORM, "RG8" },
            { VK_FORMAT_BC1_RGB_UNORM_BLOCK, "DXT1" },
            { VK_FORMAT_BC3_UNORM_BLOCK, "DXT5" },
        };
        bool all_shareable = true;
        if (!vkGetPhysicalDeviceImageFormatProperties2)
        {
            report += ", no vkGetPhysicalDeviceImageFormatProperties2";
            all_shareable = false;
        }
        else
        {
            for (const TexFormat& f : formats)
            {
                VkPhysicalDeviceExternalImageFormatInfo external_info = {
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
                external_info.handleType = handle_type;
                VkPhysicalDeviceImageFormatInfo2 info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
                info.pNext  = &external_info;
                info.format = f.vk;
                info.type   = VK_IMAGE_TYPE_2D;
                info.tiling = VK_IMAGE_TILING_OPTIMAL;
                info.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                VkExternalImageFormatProperties external_props = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
                VkImageFormatProperties2 out = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
                out.pNext = &external_props;
                const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(mPhysical, &info, &out);
                const VkExternalMemoryFeatureFlags features = external_props.externalMemoryProperties.externalMemoryFeatures;
                const bool exportable = result == VK_SUCCESS && (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
                const bool dedicated_only = (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
                report += llformat(", %s %s", f.name,
                                   !exportable ? "not exportable" : (dedicated_only ? "dedicated only" : "shareable"));
                all_shareable = all_shareable && exportable && !dedicated_only;
            }
        }

        // Two RGBA8 textures in one allocation, at their own offsets.
        constexpr U32 SIZE = 64;
        VkImage images[2] = {};
        VkDeviceMemory memory = VK_NULL_HANDLE;
        HANDLE handle = nullptr;
        GLuint gl_memory = 0;
        GLuint textures[2] = {};
        VkSemaphore gl_done_vk = VK_NULL_HANDLE, vk_done_vk = VK_NULL_HANDLE;
        GLuint gl_done_gl = 0, vk_done_gl = 0;
        HANDLE gl_done_handle = nullptr, vk_done_handle = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        auto finish = [&](bool shared, const std::string& how)
        {
            gl.Finish();
            vkDeviceWaitIdle(mDevice);
            if (textures[0]) gl.DeleteTextures(2, textures);
            if (gl_memory) gl.DeleteMemoryObjectsEXT(1, &gl_memory);
            if (gl_done_gl) gl.DeleteSemaphoresEXT(1, &gl_done_gl);
            if (vk_done_gl) gl.DeleteSemaphoresEXT(1, &vk_done_gl);
            if (fence) vkDestroyFence(mDevice, fence, nullptr);
            if (cmd) vkFreeCommandBuffers(mDevice, mCommandPool, 1, &cmd);
            if (gl_done_vk) vkDestroySemaphore(mDevice, gl_done_vk, nullptr);
            if (vk_done_vk) vkDestroySemaphore(mDevice, vk_done_vk, nullptr);
            for (VkImage image : images)
            {
                if (image) vkDestroyImage(mDevice, image, nullptr);
            }
            if (memory) vkFreeMemory(mDevice, memory, nullptr);
            for (HANDLE h : { handle, gl_done_handle, vk_done_handle })
            {
                if (h) CloseHandle(h);
            }
            mTexShared = shared && all_shareable;
            mTexNote = llformat("textures %s (%s)", mTexShared ? "can share memory" : "cannot share memory",
                                how.c_str());
            LL_INFOS("Vulkan") << "Texture memory self-test: " << report << "; two textures in one allocation: " << how
                               << ": " << (mTexShared ? "passed" : "FAILED") << LL_ENDL;
            return mTexShared;
        };

        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent        = { SIZE, SIZE, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkMemoryRequirements req[2] = {};
        for (U32 i = 0; i < 2; ++i)
        {
            if (vkCreateImage(mDevice, &info, nullptr, &images[i]) != VK_SUCCESS)
            {
                images[i] = VK_NULL_HANDLE;
                return finish(false, "vkCreateImage");
            }
            vkGetImageMemoryRequirements(mDevice, images[i], &req[i]);
        }
        if (vkGetImageMemoryRequirements2)
        {
            VkMemoryDedicatedRequirements dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
            VkMemoryRequirements2 req2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
            req2.pNext = &dedicated;
            VkImageMemoryRequirementsInfo2 which = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
            which.image = images[0];
            vkGetImageMemoryRequirements2(mDevice, &which, &req2);
            report += llformat(", an external RGBA8 image: %s dedicated", dedicated.requiresDedicatedAllocation
                                                                              ? "requires"
                                                                              : (dedicated.prefersDedicatedAllocation
                                                                                     ? "prefers"
                                                                                     : "needs no"));
            if (dedicated.requiresDedicatedAllocation)
            {
                return finish(false, "the device requires each its own allocation");
            }
        }
        const VkDeviceSize align = llmax(req[0].alignment, req[1].alignment);
        const VkDeviceSize offset1 = (req[0].size + align - 1) / align * align;
        const VkDeviceSize total = offset1 + req[1].size;
        const U32 type = memoryType(req[0].memoryTypeBits & req[1].memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == U32_MAX)
        {
            return finish(false, "no memory type both can use");
        }
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = total;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &memory) != VK_SUCCESS)
        {
            memory = VK_NULL_HANDLE;
            return finish(false, "vkAllocateMemory");
        }
        const VkDeviceSize offsets[2] = { 0, offset1 };
        for (U32 i = 0; i < 2; ++i)
        {
            if (vkBindImageMemory(mDevice, images[i], memory, offsets[i]) != VK_SUCCESS)
            {
                return finish(false, "vkBindImageMemory");
            }
        }
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = memory;
        get.handleType = handle_type;
        HANDLE exported_handle = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &exported_handle) != VK_SUCCESS || !exported_handle)
        {
            return finish(false, "vkGetMemoryWin32HandleKHR");
        }
        if (!mKMT)
        {
            handle = exported_handle;
        }
        gl.CreateMemoryObjectsEXT(1, &gl_memory);  // not dedicated: two textures take their storage from it
        gl.ImportMemoryWin32HandleEXT(gl_memory, total,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                      exported_handle);
        gl.CreateTextures(GL_TEXTURE_2D, 2, textures);
        for (U32 i = 0; i < 2; ++i)
        {
            gl.TextureParameteri(textures[i], GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
            gl.TextureStorageMem2DEXT(textures[i], 1, GL_RGBA8, (GLsizei)SIZE, (GLsizei)SIZE, gl_memory,
                                      (GLuint64)offsets[i]);
        }
        for (U32 i = 0; i < 2; ++i)
        {
            GLint immutable = GL_FALSE;
            gl.GetTextureParameteriv(textures[i], GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
            if (!immutable)
            {
                while (gl.GetError() != GL_NO_ERROR)
                {
                }
                return finish(false, llformat("GL would not take texture %u at its offset", i));
            }
        }
        if (!createSharedSemaphore(gl_done_vk, gl_done_gl, gl_done_handle) ||
            !createSharedSemaphore(vk_done_vk, vk_done_gl, vk_done_handle))
        {
            return finish(false, "semaphores");
        }
        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkAllocateCommandBuffers(mDevice, &cmd_info, &cmd) != VK_SUCCESS ||
            vkCreateFence(mDevice, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            return finish(false, "no command buffer");
        }
        const GLenum layouts[2] = { GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT };
        gl.SignalSemaphoreEXT(gl_done_gl, 0, nullptr, 2, textures, layouts);
        gl.Flush();
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        const VkClearColorValue values[2] = { { { 0.25f, 0.5f, 0.75f, 1.f } }, { { 1.f, 0.f, 0.5f, 0.25f } } };
        for (U32 i = 0; i < 2; ++i)
        {
            VkImageMemoryBarrier acquire = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            acquire.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            acquire.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            acquire.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            acquire.dstQueueFamilyIndex = mQueueFamily;
            acquire.image               = images[i];
            acquire.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &acquire);
            vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, &values[i], 1, &acquire.subresourceRange);
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
            return finish(false, "the clears could not be submitted");
        }
        vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX);
        gl.WaitSemaphoreEXT(vk_done_gl, 0, nullptr, 2, textures, layouts);
        bool right = true;
        std::string got;
        for (U32 i = 0; i < 2; ++i)
        {
            F32 rgba[4] = { -1.f, -1.f, -1.f, -1.f };
            gl.GetTextureSubImage(textures[i], 0, (GLint)SIZE / 2, (GLint)SIZE / 2, 0, 1, 1, 1, GL_RGBA, GL_FLOAT,
                                  sizeof(rgba), rgba);
            for (U32 c = 0; c < 4; ++c)
            {
                right = right && std::fabs(rgba[c] - values[i].float32[c]) < 0.01f;
            }
            got += llformat("%s%.2f %.2f %.2f %.2f", i ? " / " : "", rgba[0], rgba[1], rgba[2], rgba[3]);
        }
        logGLErrors("the texture memory self-test");
        return finish(right, right ? llformat("both right at offsets 0 and %llu", (unsigned long long)offset1)
                                   : "read back " + got);
    }

    // ---- fetched textures in Vulkan's memory (QuasarVulkanTextures; piece 2b) ----------------------------------
    //
    // Each texture's image is placed in a block: 256 MB of device-local memory Vulkan allocates and exports, which GL
    // imports once as a memory object; the texture takes its storage at its offset (glTextureStorageMem2DEXT). A block's
    // free ranges are kept by offset and merged as textures go. A texture larger than a block gets a block of its own
    // size. Only once the texture memory self-test has passed (textures can share an allocation), on the main thread,
    // and in the formats the device can export without a dedicated allocation. GL's uploads into them are as ever.
    static constexpr VkDeviceSize TEX_BLOCK_BYTES = 256ull << 20;  // fewer memory objects for GL to track (64 MB: 91 blocks in the club)
    struct TexBlock
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        HANDLE         handle = nullptr;
        GLuint         glMemory = 0;
        VkDeviceSize   size = 0;
        U32            type = 0;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset to size
        VkDeviceSize   used = 0;
    };
    // An sRGB format's linear twin (the same bits read without decoding); UNDEFINED for a format with none.
    static VkFormat gpLinearTwin(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
            case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    struct TexFormat
    {
        GLenum   gl;
        VkFormat vk;
        const char* name;
    };
    static constexpr TexFormat TEX_FORMATS[] = {
        { GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" },
        { GL_SRGB8_ALPHA8, VK_FORMAT_R8G8B8A8_SRGB, "SRGB8_ALPHA8" },
        { GL_R8, VK_FORMAT_R8_UNORM, "R8" },
        { GL_RG8, VK_FORMAT_R8G8_UNORM, "RG8" },
        { GL_COMPRESSED_RGB_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGB_UNORM_BLOCK, "DXT1 RGB" },
        { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "DXT1" },
        { GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "SRGB DXT1" },
        { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, VK_FORMAT_BC2_UNORM_BLOCK, "DXT3" },
        { GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, VK_FORMAT_BC2_SRGB_BLOCK, "SRGB DXT3" },
        { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, VK_FORMAT_BC3_UNORM_BLOCK, "DXT5" },
        { GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, VK_FORMAT_BC3_SRGB_BLOCK, "SRGB DXT5" },
        // The terrain's maps (ALTerrainSurfaceMaps): its heights and its composition, for the terrain Vulkan draws.
        { GL_R32F, VK_FORMAT_R32_SFLOAT, "R32F" },
        { GL_RG16F, VK_FORMAT_R16G16_SFLOAT, "RG16F" },
    };
    static constexpr U32 TEX_FORMAT_COUNT = (U32)(sizeof(TEX_FORMATS) / sizeof(TEX_FORMATS[0]));
    // Per format: 0 not asked yet, 1 the device can share it, 2 it cannot. And the allocations still checked with GL
    // (an allocation GL would not take is asked after, for the first ones of each format; later ones are trusted, since
    // each question waits for a threaded driver).
    U8   mTexFormatState[TEX_FORMAT_COUNT] = {};
    U32  mTexChecksLeft[TEX_FORMAT_COUNT] = {};
    // The blocks, the formats' state and the counts: the main thread and the LLImageGL thread allocate textures, the
    // main thread frees them. Taken before mExtMutex where both are held (texAllocate), never after it.
    mutable std::mutex mTexMutex;
    std::vector<TexBlock> mTexBlocks;
    U32  mTexGiven = 0, mTexDeclined = 0, mTexRefused = 0, mTexWasteful = 0;
    U64  mTexBytes = 0;
    U64  mTexBlockBytes = 0;  // every live block's size
    std::unordered_map<U64, U64> mTexWastefulShapes;  // format, levels and size of images found wasteful: the largest
                                                      // count they were found wasteful at
    U64  mTexCounted = 0;     // what the budget counts the textures in them as
    // A texture whose image would take more than this times what the budget counts it as stays GL's: small ones, which
    // NVIDIA's alignment rounds up to far more than their size.
    static constexpr double TEX_MAX_WASTE = 1.5;

    // Under mTexMutex.
    void publishTexOverhead()
    {
        gQLExternalTextureOverheadBytes.store(mTexBlockBytes > mTexCounted ? mTexBlockBytes - mTexCounted : 0,
                                              std::memory_order_relaxed);
    }

    S32 texFormatIndex(GLenum gl_format)
    {
        for (U32 i = 0; i < TEX_FORMAT_COUNT; ++i)
        {
            if (TEX_FORMATS[i].gl == gl_format)
            {
                if (mTexFormatState[i] == 0)
                {
                    mTexFormatState[i] = texFormatShareable(TEX_FORMATS[i].vk) ? 1 : 2;
                    mTexChecksLeft[i]  = 4;  // each check waits for a threaded driver: ~1 ms
                    LL_INFOS("Vulkan") << "Textures in Vulkan's memory: " << TEX_FORMATS[i].name
                                       << (mTexFormatState[i] == 1 ? " shared" : " stays GL's") << LL_ENDL;
                }
                return mTexFormatState[i] == 1 ? (S32)i : -1;
            }
        }
        return -1;
    }

    // What GL may do with a texture, as Vulkan must be told: sample it, copy in and out, and, uncompressed, draw into it
    // (GL's mipmaps and readbacks go through framebuffers).
    static VkImageUsageFlags texUsage(VkFormat format)
    {
        const bool compressed = format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
        return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               (compressed ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    }

    bool texFormatShareable(VkFormat format)
    {
        if (!vkGetPhysicalDeviceImageFormatProperties2)
        {
            return false;
        }
        VkPhysicalDeviceExternalImageFormatInfo external_info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
        external_info.handleType = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                        : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkPhysicalDeviceImageFormatInfo2 info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
        info.pNext  = &external_info;
        info.format = format;
        info.type   = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage  = texUsage(format);
        VkExternalImageFormatProperties external_props = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 out = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        out.pNext = &external_props;
        if (vkGetPhysicalDeviceImageFormatProperties2(mPhysical, &info, &out) != VK_SUCCESS)
        {
            return false;
        }
        const VkExternalMemoryFeatureFlags features = external_props.externalMemoryProperties.externalMemoryFeatures;
        return (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
               !(features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
    }

    // A block of at least `bytes`, in a memory type `type_bits` allows, exported and imported by GL. -1 where none.
    S32 newTexBlock(VkDeviceSize bytes, U32 type_bits)
    {
        const U32 type = memoryType(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == U32_MAX)
        {
            return -1;
        }
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        TexBlock block;
        block.size = llmax(bytes, TEX_BLOCK_BYTES);
        block.type = type;
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exported.handleTypes = handle_type;
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext           = &exported;
        alloc.allocationSize  = block.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(mDevice, &alloc, nullptr, &block.memory) != VK_SUCCESS)
        {
            return -1;
        }
        VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
        get.memory     = block.memory;
        get.handleType = handle_type;
        HANDLE handle  = nullptr;
        if (vkGetMemoryWin32HandleKHR(mDevice, &get, &handle) != VK_SUCCESS || !handle)
        {
            vkFreeMemory(mDevice, block.memory, nullptr);
            return -1;
        }
        if (!mKMT)
        {
            block.handle = handle;
        }
        gl.CreateMemoryObjectsEXT(1, &block.glMemory);
        gl.ImportMemoryWin32HandleEXT(block.glMemory, block.size,
                                      mKMT ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                      handle);
        // Done before the block is given out: the other context of the share group (the main thread's or the LLImageGL
        // thread's) may take a texture from it next, and a threaded driver may not have run the import yet. Rare (a
        // block holds many textures), so a finish is no cost.
        gl.Finish();
        block.free[0] = block.size;
        mTexBlockBytes += block.size;
        publishTexOverhead();
        // A free block, or a new one at the end.
        for (size_t i = 0; i < mTexBlocks.size(); ++i)
        {
            if (!mTexBlocks[i].memory)
            {
                mTexBlocks[i] = std::move(block);
                return (S32)i;
            }
        }
        mTexBlocks.push_back(std::move(block));
        LL_INFOS("Vulkan") << "Textures in Vulkan's memory: block " << mTexBlocks.size() << " ("
                           << (mTexBlocks.back().size >> 20) << " MB)" << LL_ENDL;
        return (S32)mTexBlocks.size() - 1;
    }

    // First fit, aligned. False where the block has no room.
    static bool takeRange(TexBlock& block, VkDeviceSize size, VkDeviceSize align, VkDeviceSize& offset)
    {
        for (auto it = block.free.begin(); it != block.free.end(); ++it)
        {
            const VkDeviceSize start = (it->first + align - 1) / align * align;
            const VkDeviceSize end   = it->first + it->second;
            if (start + size > end)
            {
                continue;
            }
            const VkDeviceSize before_start = it->first;
            const VkDeviceSize before = start - it->first;
            const VkDeviceSize after  = end - (start + size);
            block.free.erase(it);
            if (before)
            {
                block.free[before_start] = before;
            }
            if (after)
            {
                block.free[start + size] = after;
            }
            block.used += size;
            offset = start;
            return true;
        }
        return false;
    }

    static void giveRange(TexBlock& block, VkDeviceSize offset, VkDeviceSize size)
    {
        const auto placed = block.free.emplace(offset, size);
        if (!placed.second)
        {
            LL_WARNS("Vulkan") << "Textures in Vulkan's memory: a range given back twice, at " << offset << LL_ENDL;
            return;
        }
        block.used -= llmin(block.used, size);
        auto it = placed.first;
        // With the free range after it, and the one before.
        auto next = std::next(it);
        if (next != block.free.end() && it->first + it->second == next->first)
        {
            it->second += next->second;
            block.free.erase(next);
        }
        if (it != block.free.begin())
        {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first)
            {
                prev->second += it->second;
                block.free.erase(it);
            }
        }
    }

    bool texAllocate(GLuint texture, GLint internal_format, GLsizei width, GLsizei height, GLsizei levels,
                     U64 counted_bytes)
    {
        if (!mActive || !mDevice || !mTexShared || width <= 0 || height <= 0 || levels <= 0)
        {
            return false;
        }
        std::lock_guard<std::mutex> tex_lock(mTexMutex);
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            if (mExtTextures.count(texture))
            {
                ++mTexDeclined;
                return false;  // never: a name with storage already
            }
        }
        // RGB textures, the commonest there are, are stored as RGBA with alpha 1 (GL pads RGB8 to four bytes in its own
        // storage, and Vulkan's R8G8B8 is packed and seldom offered): GL's RGB uploads into RGBA storage fill alpha with
        // 1, and sampling reads the same. A third more memory for those.
        if (internal_format == GL_RGB8)
        {
            internal_format = GL_RGBA8;
        }
        else if (internal_format == GL_SRGB8)
        {
            internal_format = GL_SRGB8_ALPHA8;
        }
        const S32 index = texFormatIndex((GLenum)internal_format);
        if (index < 0)
        {
            ++mTexDeclined;
            return false;
        }
        const VkExternalMemoryHandleTypeFlagBits handle_type = mKMT ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT
                                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        ExtTexture ext;
        ext.width  = (U32)width;
        ext.height = (U32)height;
        ext.levels = (U32)levels;
        ext.mutable_format = gpLinearTwin(TEX_FORMATS[index].vk) != VK_FORMAT_UNDEFINED;
        VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handle_type;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext         = &external;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = TEX_FORMATS[index].vk;
        // An sRGB texture may be viewed as its linear twin: GL's programs read some without decoding (the sky's
        // moon, QuasarVulkanLighting), which Vulkan does through a view of the UNORM format.
        info.flags         = gpLinearTwin(TEX_FORMATS[index].vk) != VK_FORMAT_UNDEFINED ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                                                                                    : 0;
        info.extent        = { (U32)width, (U32)height, 1 };
        info.mipLevels     = (U32)levels;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = texUsage(TEX_FORMATS[index].vk);
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // An image's size is the same for the same format, size and levels: a wasteful one is known without making it.
        const U64 shape = (U64)index | ((U64)levels << 5) | ((U64)(width & 0xFFFF) << 10) | ((U64)(height & 0xFFFF) << 26);
        // The terrain's maps are wanted whatever they waste: a few hundred KB a region, and the terrain Vulkan draws
        // reads nothing else (their odd size, the grid and its apron, pads past the rule).
        const bool wanted_anyway =
            TEX_FORMATS[index].vk == VK_FORMAT_R32_SFLOAT || TEX_FORMATS[index].vk == VK_FORMAT_R16G16_SFLOAT;
        const auto known = mTexWastefulShapes.find(shape);
        if (!wanted_anyway && counted_bytes && known != mTexWastefulShapes.end() && counted_bytes <= known->second)
        {
            ++mTexWasteful;
            return false;
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan texture - vk image");
            if (vkCreateImage(mDevice, &info, nullptr, &ext.image) != VK_SUCCESS)
            {
                ext.image = VK_NULL_HANDLE;
                ++mTexDeclined;
                return false;
            }
        }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(mDevice, ext.image, &req);
        if (!wanted_anyway && counted_bytes && (double)req.size > TEX_MAX_WASTE * (double)counted_bytes)
        {
            vkDestroyImage(mDevice, ext.image, nullptr);
            ++mTexWasteful;
            U64& largest = mTexWastefulShapes[shape];
            largest = llmax(largest, counted_bytes);  // the image is the same: any smaller count is as wasteful
            return false;  // GL's: Vulkan would take far more for it than it is counted as
        }
        // A block with room, in a memory type the image can use, or a new one.
        S32 block = -1;
        VkDeviceSize offset = 0;
        for (size_t i = 0; i < mTexBlocks.size() && block < 0; ++i)
        {
            TexBlock& b = mTexBlocks[i];
            if (b.memory && (req.memoryTypeBits & (1u << b.type)) && takeRange(b, req.size, req.alignment, offset))
            {
                block = (S32)i;
            }
        }
        if (block < 0)
        {
            block = newTexBlock(req.size + req.alignment, req.memoryTypeBits);
            if (block < 0 || !takeRange(mTexBlocks[block], req.size, req.alignment, offset))
            {
                vkDestroyImage(mDevice, ext.image, nullptr);
                ++mTexDeclined;
                return false;
            }
        }
        TexBlock& b = mTexBlocks[block];
        if (vkBindImageMemory(mDevice, ext.image, b.memory, offset) != VK_SUCCESS)
        {
            giveRange(b, offset, req.size);
            vkDestroyImage(mDevice, ext.image, nullptr);
            ++mTexDeclined;
            return false;
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan texture - gl storage");
            gl.TextureParameteri(texture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
            gl.TextureStorageMem2DEXT(texture, levels, (GLenum)internal_format, width, height, b.glMemory,
                                      (GLuint64)offset);
        }
        if (mTexChecksLeft[index] > 0)
        {
            LL_PROFILE_ZONE_NAMED("vulkan texture - gl check");
            --mTexChecksLeft[index];
            GLint immutable = GL_FALSE;
            gl.GetTextureParameteriv(texture, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
            while (gl.GetError() != GL_NO_ERROR)
            {
            }
            if (!immutable)
            {
                // GL would not take this format from shared memory: it stays GL's from now on.
                LL_WARNS("Vulkan") << "Textures in Vulkan's memory: GL would not take a " << width << "x" << height << " "
                                   << TEX_FORMATS[index].name << " texture: the format stays GL's" << LL_ENDL;
                mTexFormatState[index] = 2;
                giveRange(b, offset, req.size);
                vkDestroyImage(mDevice, ext.image, nullptr);
                ++mTexRefused;
                return false;
            }
        }
        ext.format = U32_MAX;  // a texture (the EXT_FORMATS index is the render targets')
        ext.vk     = TEX_FORMATS[index].vk;
        ext.block  = block;
        ext.offset  = offset;
        ext.size    = req.size;
        ext.counted = counted_bytes;
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            mExtTextures[texture] = ext;
        }
        ++mTexGiven;
        mTexBytes += req.size;
        mTexCounted += counted_bytes;
        publishTexOverhead();
        return true;
    }

    // The textures' own line: how many are in Vulkan's memory first (the line is cut at the screen's edge), then where
    // the rest went. Logged every half minute too.
    std::string textureInfo() const
    {
        if (!mTexShared)
        {
            return "not in Vulkan's memory (QuasarVulkanTargets and QuasarVulkanTextures, restart; " + mTexNote + ")";
        }
        U32 textures = 0;
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            for (const auto& entry : mExtTextures)
            {
                textures += entry.second.block >= 0 ? 1 : 0;
            }
        }
        size_t blocks = 0;
        U64 bytes = 0;
        U32 declined = 0, refused = 0, wasteful = 0;
        U64 block_bytes = 0, counted = 0;
        {
            std::lock_guard<std::mutex> tex_lock(mTexMutex);
            for (const TexBlock& b : mTexBlocks)
            {
                blocks += b.memory ? 1 : 0;
            }
            bytes       = mTexBytes;
            declined    = mTexDeclined;
            refused     = mTexRefused;
            wasteful    = mTexWasteful;
            block_bytes = mTexBlockBytes;
            counted     = mTexCounted;
        }
        const std::string info = llformat(
            "%u in Vulkan's memory (%.0f MB in %u blocks of %.0f MB; the budget counted %.0f MB) | %u small left to GL | "
            "made since start: %u on the main thread, %u on the texture thread | %u GL's by format or room, %u refused",
            textures, (double)bytes / (1 << 20), (U32)blocks, (double)block_bytes / (1 << 20),
            (double)counted / (1 << 20), wasteful, gQLExternalTexturesOffered.load(std::memory_order_relaxed),
            gQLExternalTexturesOffThread.load(std::memory_order_relaxed), declined, refused);
        static U64 logged_at = 0;
        const U64 now = (U64)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - logged_at > 30000)
        {
            logged_at = now;
            LL_INFOS("Vulkan") << "Textures: " << info << LL_ENDL;
        }
        return info;
    }

    std::string targetInfo() const
    {
        if (!mExtPassed)
        {
            return "targets: " + mExtNote;
        }
        U32 targets = 0, textures = 0;
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            for (const auto& entry : mExtTextures)
            {
                (entry.second.block >= 0 ? textures : targets) += 1;
            }
        }
        size_t blocks = 0;
        U64 tex_bytes = 0;
        U32 tex_declined = 0, tex_refused = 0;
        {
            // Never with mExtMutex held: texAllocate takes mTexMutex, then mExtMutex.
            std::lock_guard<std::mutex> tex_lock(mTexMutex);
            for (const TexBlock& b : mTexBlocks)
            {
                blocks += b.memory ? 1 : 0;
            }
            tex_bytes    = mTexBytes;
            tex_declined = mTexDeclined;
            tex_refused  = mTexRefused;
        }
        return llformat("targets: %s, %u in Vulkan's memory (%.0f MB) | textures: %u (%.0f MB in %u blocks), %u GL's "
                        "(format or room), %u refused | %s",
                        mExtNote.c_str(), targets, (double)mExtBytes / (1 << 20), textures,
                        (double)tex_bytes / (1 << 20), (U32)blocks, tex_declined, tex_refused, mTexNote.c_str());
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
    // A masked draw of several textures (a batch GL draws through the indexed lookup): each texture's tile, by the index
    // each vertex carries (position.w's bits), SH_TABLE_ENTRIES a draw (qlshadowmask.vert's Tiles).
    static constexpr U32          SH_TABLE_ENTRIES = 16;
    static constexpr U32          SH_MAX_TABLES = 1024;  // a slot's batched masked draws; past them, GL's
    static constexpr VkDeviceSize SH_TABLE_BYTES = (VkDeviceSize)SH_MAX_TABLES * SH_TABLE_ENTRIES * 4;
    static constexpr VkDeviceSize SH_SLOT_BYTES =
        ((SH_MVP_BYTES + SH_CMD_BYTES + SH_PALBASE_BYTES + SH_MASK_BYTES + SH_TABLE_BYTES) + 255) & ~(VkDeviceSize)255;
    static_assert((SH_MVP_BYTES + SH_CMD_BYTES) % 256 == 0, "each slot's palette bases start on a storage offset");
    static_assert((SH_MVP_BYTES + SH_CMD_BYTES + SH_PALBASE_BYTES) % 256 == 0, "each slot's masks start on a storage offset");
    static constexpr VkDeviceSize SH_MASK_START = SH_MVP_BYTES + SH_CMD_BYTES + SH_PALBASE_BYTES;
    static constexpr VkDeviceSize SH_TABLE_START = SH_MASK_START + SH_MASK_BYTES;
    static_assert(SH_TABLE_ENTRIES == 16, "qlshadowmask.vert's Tiles: 16 a draw");
    static_assert(SH_TABLE_START % 256 == 0, "each slot's tile tables start on a storage offset");
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
    static_assert(SH_TILE == 128 && SH_TILES_ACROSS == 32, "qlshadowmask.vert's SH_TILE and SH_TILES_ACROSS");
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
        VkDeviceSize   vertexBytes = 0, weightBytes = 0, normalBytes = 0, tangentBytes = 0, uvBytes = 0, colorBytes = 0,
                       indexBytes = 0;
        VkDeviceSize   extraUvBytes = 0;  // each of the second and third texture coordinates (legacy materials' maps)
        VkDeviceSize   emissiveBytes = 0;  // the emissive colours (4 bytes), beside the extra coordinates
        VkDeviceSize   vertexTop = 0, indexTop = 0;
        U32            resets = 0;
        std::unordered_map<U64, ShArenaEntry> entries;

        VkDeviceSize normalStart() const { return vertexBytes + weightBytes; }
        VkDeviceSize tangentStart() const { return vertexBytes + weightBytes + normalBytes; }
        VkDeviceSize uvStart() const { return vertexBytes + weightBytes + normalBytes + tangentBytes; }
        VkDeviceSize uv1Start() const { return uvStart() + uvBytes; }
        VkDeviceSize uv2Start() const { return uv1Start() + extraUvBytes; }
        VkDeviceSize colorStart() const { return uv2Start() + extraUvBytes; }
        VkDeviceSize emissiveStart() const { return colorStart() + colorBytes; }
        VkDeviceSize indexStart() const { return emissiveStart() + emissiveBytes; }
        VkDeviceSize totalBytes() const { return indexStart() + indexBytes; }
        VkDeviceSize used() const
        {
            return vertexTop * (1 + (weightBytes ? 1 : 0) + (normalBytes ? 1 : 0) + (tangentBytes ? 1 : 0)) +
                   (uvBytes ? vertexTop / 2 + vertexTop / 4 : 0) + (extraUvBytes ? vertexTop : 0) +
                   (emissiveBytes ? vertexTop / 4 : 0) + indexTop;
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
    U32      mShTables = 0;  // the slot's tile tables given out (batched masked draws)
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
        // And binding 5: the slot's tile tables (masked draws of several textures).
        VkDescriptorSetLayoutBinding bindings[6] = {};
        for (U32 b = 0; b < 6; ++b)
        {
            bindings[b].binding         = b;
            bindings[b].descriptorType  = b == 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags      = b == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 6;
        set_info.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mShSetLayout) != VK_SUCCESS)
        {
            mShSetLayout = VK_NULL_HANDLE;
            return shFail("vkCreateDescriptorSetLayout (shadows)");
        }
        const VkDescriptorPoolSize pool_sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SH_SLOTS * 5 },
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
            const VkDescriptorBufferInfo buffers[5] = {
                { mShHost, SH_SLOT_BYTES * i, SH_MVP_BYTES },
                { mShHost, SH_SLOT_BYTES * i + SH_MVP_BYTES + SH_CMD_BYTES, SH_PALBASE_BYTES },
                { mShPalettes, 0, SH_PALETTE_SEGMENT_BYTES * SH_PALETTE_SEGMENTS },
                { mShHost, SH_SLOT_BYTES * i + SH_MASK_START, SH_MASK_BYTES },
                { mShHost, SH_SLOT_BYTES * i + SH_TABLE_START, SH_TABLE_BYTES },
            };
            VkWriteDescriptorSet writes[5] = {};
            for (U32 b = 0; b < 5; ++b)
            {
                const U32 binding = b < 4 ? b : 5;  // 4 is the atlas
                writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet          = slot.set;
                writes[b].dstBinding      = binding;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].pBufferInfo     = &buffers[b];
            }
            vkUpdateDescriptorSets(mDevice, 5, writes, 0, nullptr);
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
        VkVertexInputAttributeDescription attributes[5] = {};
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
        U32 attribute_count = streams;
        if (masked)
        {
            stream(2, 8, VK_FORMAT_R32G32_SFLOAT);
            stream(3, 4, VK_FORMAT_R8G8B8A8_UNORM);
            attribute_count = streams;
            // The texture index a batched face's vertices carry in position.w, its bits an integer (LLFace).
            attributes[attribute_count++] = { 5, 0, VK_FORMAT_R32_SINT, 12 };
        }
        else
        {
            attribute_count = streams;
        }
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex_input.vertexBindingDescriptionCount   = streams;
        vertex_input.pVertexBindingDescriptions      = vertex_bindings;
        vertex_input.vertexAttributeDescriptionCount = attribute_count;
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
        // The G-buffer's sets name the palettes: the stand-in, before they go (every slot is idle now).
        for (U32 i = 0; i < GB_SLOTS; ++i)
        {
            if (mGbSlots[i].set && mGbSlots[i].palettes)
            {
                gbWritePalettes(i, false);
            }
        }
        for (auto& readers : mGbPaletteReaders)
        {
            readers.clear();
        }
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
            mShMergedLast = mShMergedNow;
            mShMergedNow = 0;
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
        mShTables     = 0;
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
                    U32 uv_offset = 0, U32 color_offset = U32_MAX, U32 normal_offset = 0, bool any_pass = false,
                    U32 tangent_offset = U32_MAX, U32 uv1_offset = U32_MAX, U32 uv2_offset = U32_MAX,
                    U32 emissive_offset = U32_MAX)
    {
        if (!mShInPass && !any_pass)
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
        if (arena.weightBytes && weight_offset != U32_MAX)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)weight_offset,
                                      (GLintptr)(arena.vertexBytes + arena.vertexTop), (GLsizeiptr)vertex_bytes);
        }
        if (arena.normalBytes)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)normal_offset,
                                      (GLintptr)(arena.normalStart() + arena.vertexTop), (GLsizeiptr)vertex_bytes);
        }
        // Tangents where the mesh has them (the PBR and material draws need them; the rest leave the space unread).
        if (arena.tangentBytes && tangent_offset != U32_MAX)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)tangent_offset,
                                      (GLintptr)(arena.tangentStart() + arena.vertexTop), (GLsizeiptr)vertex_bytes);
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
        // The legacy materials' normal and specular maps' coordinates, where the mesh has them.
        if (arena.extraUvBytes && uv1_offset != U32_MAX)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)uv1_offset,
                                      (GLintptr)(arena.uv1Start() + arena.vertexTop / 2), (GLsizeiptr)vertex_count * 8);
        }
        if (arena.extraUvBytes && uv2_offset != U32_MAX)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)uv2_offset,
                                      (GLintptr)(arena.uv2Start() + arena.vertexTop / 2), (GLsizeiptr)vertex_count * 8);
        }
        if (arena.emissiveBytes && emissive_offset != U32_MAX)
        {
            gl.CopyNamedBufferSubData(vertex_buffer, arena.gl, (GLintptr)emissive_offset,
                                      (GLintptr)(arena.emissiveStart() + arena.vertexTop / 4),
                                      (GLsizeiptr)vertex_count * 4);
        }
        gl.CopyNamedBufferSubData(index_buffer, arena.gl, 0, (GLintptr)(arena.indexStart() + arena.indexTop),
                                  (GLsizeiptr)index_count * 2);
        ShArenaEntry entry;
        entry.mBaseVertex = (U32)(arena.vertexTop / 16);
        entry.mBaseIndex  = (U32)(arena.indexTop / 2);
        arena.entries.emplace(stamp, entry);
        arena.vertexTop += vertex_bytes;
        arena.indexTop += index_bytes;
        mShCopiedNow += vertex_bytes * (1 + (arena.weightBytes ? 1 : 0) + (arena.normalBytes ? 1 : 0) +
                                        (arena.tangentBytes && tangent_offset != U32_MAX ? 1 : 0)) +
                        (arena.uvBytes ? vertex_count * 12ull : 0) + index_bytes;
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
            // The G-buffer submissions that read it too (rigged draws), never waited for either.
            auto& readers = mGbPaletteReaders[mShFrame % SH_PALETTE_SEGMENTS];
            for (auto it = readers.begin(); it != readers.end();)
            {
                const GbSlot& reader = mGbSlots[it->first];
                const bool done = !reader.submitted || reader.serial != it->second ||
                                  vkGetFenceStatus(mDevice, reader.fence) == VK_SUCCESS;
                it = done ? readers.erase(it) : std::next(it);
            }
            mShPaletteSegmentFree = mShPaletteSegmentFree && readers.empty();
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
                     const char* what, bool normals = false, bool tangents = false, bool extra_uvs = false)
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
        arena.normalBytes = normals ? vertex_bytes : 0;
        arena.tangentBytes = tangents ? vertex_bytes : 0;
        arena.extraUvBytes = extra_uvs ? vertex_bytes / 2 : 0;
        arena.emissiveBytes = extra_uvs ? vertex_bytes / 4 : 0;
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
                          U32 test, U32 base_vertex, U32 first_index, U32 index_count, const U32* tiles = nullptr,
                          U32 tile_count = 0)
    {
        const bool rigged = palette_base != U32_MAX;
        if (tile_count > SH_TABLE_ENTRIES || (tile_count && mShTables >= SH_MAX_TABLES))
        {
            return false;
        }
        for (U32 i = 0; i < tile_count; ++i)
        {
            if (tiles[i] >= SH_TILES)
            {
                return false;
            }
        }
        if (tile_count)
        {
            tile = tiles[0];
        }
        if (!mShMaskReady || tile >= SH_TILES || !enterKind(rigged ? SH_KIND_MASKED_RIGGED : SH_KIND_MASKED))
        {
            return false;
        }
        if (tile_count)
        {
            U32 table[SH_TABLE_ENTRIES];
            for (U32 i = 0; i < SH_TABLE_ENTRIES; ++i)
            {
                table[i] = tiles[i < tile_count ? i : 0];
            }
            memcpy(mShHostMapped + SH_SLOT_BYTES * mShSlot + SH_TABLE_START + (size_t)mShTables * sizeof(table), table,
                   sizeof(table));
            ++mShTables;
        }
        // The texture matrix's rows for x and y over (u, v, 1), as GL's texture_matrix0 * vec4(uv, 0, 1) takes them
        // (column major: element [column * 4 + row]).
        static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float* m = texture_matrix ? texture_matrix : identity;
        const float mask[12] = { m[0], m[4], m[12], cutoff,
                                 m[1], m[5], m[13], (float)test,
                                 (float)((tile % SH_TILES_ACROSS) * SH_TILE), (float)((tile / SH_TILES_ACROSS) * SH_TILE),
                                 (float)SH_TILE, tile_count ? (float)mShTables : 0.f };  // w: its table + 1, or 0
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
    // A shadow hand-over's parts, so the G-buffer's can carry it (QuasarVulkanMergeHandOvers): what shadowRecord
    // gathered and recorded, which the crossing then hands over, and shadowFinish completes.
    struct ShFlush
    {
        std::vector<ShPending> pending;
        std::vector<ShDepth*>  images;
        std::vector<U32>       image_of;
        std::vector<GLuint>    gl_buffers;
        std::vector<GLuint>    gl_textures;
        std::vector<GLenum>    layouts;
        VkCommandBuffer        cmd = VK_NULL_HANDLE;
        ShSlot*                head = nullptr;
        bool                   masked = false, rigged = false;
        U32                    draws = 0, rigged_draws = 0, masked_draws = 0, masked_rigged_draws = 0;
    };

    // The queued maps copied in from GL and recorded into the first's command buffer, its fence reset: everything a
    // hand-over does before GL signals. False where nothing can be drawn (the queue is then let go).
    bool shadowRecord(ShFlush& f)
    {
        std::vector<ShPending>& pending = f.pending;
        pending.swap(mShPending);
        // The atlas's tiles drawn with until now may go to other textures once GL has waited for this hand-over.
        mShPassAtFlush = mShPassSerial;

        // The images, each once (a map drawn twice in a view is queued twice).
        std::vector<ShDepth*>& images = f.images;
        std::vector<U32>& image_of = f.image_of;
        image_of.assign(pending.size(), 0);
        bool& masked = f.masked;
        bool& rigged = f.rigged;
        U32& draws = f.draws;
        U32& rigged_draws = f.rigged_draws;
        U32& masked_draws = f.masked_draws;
        U32& masked_rigged_draws = f.masked_rigged_draws;
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
        std::vector<GLuint>& gl_buffers = f.gl_buffers;
        gl_buffers = { mShStatic.gl, mShRigged.gl };
        if (mShMaskReady)
        {
            gl_buffers.push_back(mShMasked.gl);
            gl_buffers.push_back(mShMaskedRigged.gl);
        }
        std::vector<GLuint>& gl_textures = f.gl_textures;
        for (const ShDepth* image : images)
        {
            gl_textures.push_back(image->glTexture);
        }
        if (masked)
        {
            gl_textures.push_back(mShAtlasGL);
        }
        f.layouts.assign(gl_textures.size(), GL_LAYOUT_GENERAL_EXT);
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
        // To the function's end (its own variable, beside the hand-over's zone): its self time is the recording, the
        // steps after have zones of their own.
        LL_PROFILE_ZONE_NAMED_COLOR("vulkan shadow - vk record", 0x40A0FF);

        // The first map's command buffer and fence carry the hand-over.
        ShSlot& head = mShSlots[pending[0].slot];
        f.head = &head;
        VkCommandBuffer cmd = head.cmd;
        f.cmd = cmd;
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

        return true;
    }

    // A recorded hand-over submitted (result): the slots' serials, or the failure.
    bool shadowSubmitted(ShFlush& f, VkResult result)
    {
        const std::vector<ShPending>& pending = f.pending;
        ShSlot& head = *f.head;
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
        if (f.rigged)
        {
            mShSegmentSerial[mShFrame % SH_PALETTE_SEGMENTS] = serial;
        }

        return true;
    }

    // After GL has waited for the hand-over: each map back into GL's own texture.
    void shadowFinish(ShFlush& f)
    {
        const std::vector<ShPending>& pending = f.pending;
        const std::vector<ShDepth*>& images = f.images;
        const std::vector<U32>& image_of = f.image_of;
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
        mShDrawsNow += f.draws;
        mShRiggedNow += f.rigged_draws;
        mShMaskedNow += f.masked_draws;
        mShMaskedRiggedNow += f.masked_rigged_draws;
    }

    // Whether the next G-buffer hand-over is to carry the queued shadow maps (QuasarVulkanMergeHandOvers): set by the
    // viewer for the main view's maps, which nothing reads before its G-buffer is drawn; shadowFlush still hands them
    // over if no G-buffer hand-over takes them.
    bool mShMergeNext = false;
    bool mShMergeFailed = false;  // a carried hand-over's shadows were lost: the next shadowFlush says so
    U32  mShMergedNow = 0, mShMergedLast = 0;

    bool shadowFlush()
    {
        mShMergeNext = false;
        if (mShMergeFailed)
        {
            // Maps a G-buffer hand-over was to carry, lost: the caller throws away what its caches kept of them.
            mShMergeFailed = false;
            mShPending.clear();
            return false;
        }
        if (mShPending.empty())
        {
            return true;
        }
        LL_PROFILE_ZONE_NAMED("vulkan shadow hand-over");
        ShFlush f;
        if (!shadowRecord(f))
        {
            return false;
        }
        const std::vector<GLuint>& gl_buffers = f.gl_buffers;
        const std::vector<GLuint>& gl_textures = f.gl_textures;
        const std::vector<GLenum>& layouts = f.layouts;
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
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &mShGLDoneVK;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &f.cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &mShVKDoneVK;
        VkResult result = VK_SUCCESS;
        {
            LL_PROFILE_ZONE_NAMED("vulkan shadow - vk submit");
            result = vkQueueSubmit(mQueue, 1, &submit, f.head->fence);
        }
        if (!shadowSubmitted(f, result))
        {
            return false;
        }
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
        shadowFinish(f);
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
            llformat("%u maps in %u hand-overs (%u carried by the G-buffer's), %u draws (%u rigged) last frame | arenas "
                     "%.0f/%.0f + %.0f/%.0f MB, %.1f MB copied, %u+%u resets, %u palettes",
                     mShPassesLast, mShFlushesLast, mShMergedLast, mShDrawsLast, mShRiggedLast, (double)mShStatic.used() / (1 << 20),
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

    // ---- simple prims drawn into a probe face's G-buffer by Vulkan (QuasarVulkanProbeDraws; piece 3) --------
    //
    // The first of the scene's own draws Vulkan takes from GL: a reflection probe face's unrigged PASS_SIMPLE prims
    // (diffuse texture and tint), drawn into the face's G-buffer, whose attachments are in Vulkan's memory already
    // (QuasarVulkanTargets), with their textures from Vulkan's memory too (QuasarVulkanTextures). GL draws everything
    // else into the G-buffer first; at the end of renderGeomDeferred one hand-over lets Vulkan draw these over it (the
    // depth test sorts out what is in front, as it does between GL's own draws), and GL lights the face after.
    //   - Meshes go into an arena of their own: positions, normals, texture coordinates, colours.
    //   - Each draw: its model-view-projection and normal matrix (qlgbuffer.vert), its texture's descriptor set (one a
    //     texture, kept until the texture goes); draws of one texture in a row are one indirect draw.
    //   - A render pass and framebuffer for each set of attachments, kept until one of their images goes.
    static constexpr U32          GB_SLOTS = 16;
    static constexpr U32          GB_MAX_DRAWS = 8192;
    // A draw's data (qlgbuffer.vert's Draw): mvp, normal matrix, texture matrix, the four KHR transforms, the PBR
    // factors and kind, the emissive colour, and room for the legacy materials' to come.
    static constexpr VkDeviceSize GB_DRAW_BYTES = 400;  // and a rigged draw's palette
    static constexpr VkDeviceSize GB_SLOT_BYTES =
        ((VkDeviceSize)GB_MAX_DRAWS * (GB_DRAW_BYTES + sizeof(VkDrawIndexedIndirectCommand)) + 255) & ~(VkDeviceSize)255;
    static constexpr VkDeviceSize GB_ARENA_VERTEX_BYTES = 64ull << 20;  // positions; normals the same, uvs half, colours a quarter
    static constexpr VkDeviceSize GB_ARENA_INDEX_BYTES = 32ull << 20;
    static constexpr U32          GB_MAX_TEXTURE_SETS = 16384;

    struct GbSlot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool            submitted = false;
        bool            palettes = false;  // set 0's binding 1 is the shadows' palettes, not the stand-in
        U64             serial = 0;        // its submission's
    };
    struct GbTarget
    {
        GLuint        names[5] = {};  // the colour attachments' GL names, then the depth's
        VkImage       images[5] = {}; // their images (a name can be handed out again; an image is never)
        U32           colors = 0;
        U32           width = 0, height = 0;
        VkImageView   views[5] = {};
        VkRenderPass  pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkPipeline    pipelines[8] = {};  // [masked * 4 + greater * 2 + double-sided]
        VkPipeline    terrain[2] = {};    // [greater] (QuasarVulkanTerrainDraws), where the terrain's objects were made
        bool          normHdr = false;    // the normals attachment is RGBA16 (GL's GBUFFER_NORM_HDR)
    };
    struct GbQueued
    {
        VkDescriptorSet set;  // its textures'
        U32             index;
        U32             pipe;  // masked * 4 + double-sided
        VkDrawIndexedIndirectCommand cmd;  // as written (the mapped copy is not read back: it is write-combined)
    };
    // A texture slot's default where a draw names none (GL binds sWhiteImagep, or sFlatNormalImagep in the normal
    // slot): images of Vulkan's own.
    struct GbDefault
    {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
    };
    // A set's images: the four slots, then a batch's sixteen (white where there is none).
    static constexpr U32 GB_SET_IMAGES = 20;
    using GbMatKey = std::array<VkImage, GB_SET_IMAGES>;

    // ---- the terrain in the G-buffer (QuasarVulkanTerrainDraws; terrain step 2) ----
    // A region a draw: its patches (corner, edge) instanced over four corner vertices with no vertex buffer, tessellated
    // from its height map (qlterrain.vert/.tesc/.tese/.frag). Each slot has its regions' data and patches in a host
    // buffer of its own, and a descriptor pool for their textures, reset as the slot is taken again.
    static constexpr U32          TR_MAX_REGIONS = 16;
    static constexpr U32          TR_TEXTURES = 19;  // qlterrain's set 1
    static constexpr U32          TR_MAX_PATCHES = 8192;
    static constexpr VkDeviceSize TR_REGION_BYTES = 624;  // qlterrain's Region
    static constexpr VkDeviceSize TR_PATCHES_AT = ((VkDeviceSize)TR_MAX_REGIONS * TR_REGION_BYTES + 255) & ~(VkDeviceSize)255;
    static constexpr VkDeviceSize TR_SLOT_BYTES =
        (TR_PATCHES_AT + (VkDeviceSize)TR_MAX_PATCHES * 16 + 255) & ~(VkDeviceSize)255;
    struct TrQueued
    {
        VkDescriptorSet set;
        U32             first, count;
    };
    bool                  mHasTessellation = false;
    bool                  mTrReady = false;
    std::string           mTrNote = "not made yet";
    VkDescriptorSetLayout mTrSetLayout = VK_NULL_HANDLE, mTrTexLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mTrLayout = VK_NULL_HANDLE;
    VkDescriptorPool      mTrPool = VK_NULL_HANDLE;
    VkDescriptorPool      mTrTexPools[16] = {};  // a slot's (GB_SLOTS)
    VkDescriptorSet       mTrSets[16] = {};      // a slot's set 0
    VkSampler             mTrPointClamp = VK_NULL_HANDLE, mTrLinearClamp = VK_NULL_HANDLE, mTrAnisoClamp = VK_NULL_HANDLE;
    VkShaderModule        mTrModules[4] = {};    // vertex, control, evaluation, fragment
    VkBuffer              mTrHost = VK_NULL_HANDLE;
    VkDeviceMemory        mTrHostMemory = VK_NULL_HANDLE;
    U8*                   mTrHostMapped = nullptr;
    std::vector<TrQueued> mTrQueued;
    U32                   mTrPatches = 0;       // the open pass's
    U32                   mTrRegionsLast = 0, mTrRegionsNow = 0, mTrPatchesLast = 0, mTrPatchesNow = 0;

    bool        mGbTried = false, mGbReady = false;
    std::string mGbNote = "off (QuasarVulkanProbeDraws)";
    VkDescriptorSetLayout mGbSetLayout = VK_NULL_HANDLE, mGbTexLayout = VK_NULL_HANDLE;
    VkPipelineLayout mGbLayout = VK_NULL_HANDLE;
    VkDescriptorPool mGbPool = VK_NULL_HANDLE, mGbTexPool = VK_NULL_HANDLE;
    VkSampler        mGbSampler = VK_NULL_HANDLE;
    VkShaderModule   mGbVert = VK_NULL_HANDLE, mGbFrag = VK_NULL_HANDLE;
    ShArena          mGbArena;
    VkBuffer         mGbHost = VK_NULL_HANDLE;
    VkDeviceMemory   mGbHostMemory = VK_NULL_HANDLE;
    U8*              mGbHostMapped = nullptr;
    GbSlot           mGbSlots[GB_SLOTS];
    U32              mGbNextSlot = 0, mGbSlot = 0;
    VkSemaphore      mGbGLDoneVK = VK_NULL_HANDLE, mGbVKDoneVK = VK_NULL_HANDLE;
    GLuint           mGbGLDoneGL = 0, mGbVKDoneGL = 0;
    HANDLE           mGbGLDoneHandle = nullptr, mGbVKDoneHandle = nullptr;
    std::vector<GbTarget> mGbTargets;
    // The pass being collected.
    bool             mGbInPass = false;
    S32              mGbTarget = -1;
    bool             mGbGreater = false, mGbRemap = false;
    F32              mGbAnisotropy = 1.f;  // GL's level (LLRender::sAnisotropicFilteringLevel) as the sampler was made
    std::vector<GbQueued> mGbDraws;
    std::vector<GLuint>   mGbPassTextures;        // the textures the pass draws with, each once, for the hand-over
    std::vector<VkImage>  mGbPassImages;
    GbDefault             mGbWhite, mGbFlat, mGbBlack, mGbBlackCube;  // the cube: a cube array of one, black
    bool                  mGbPassRigged = false;  // the open pass took a rigged draw
    U64                   mGbSubmitSerial = 0;
    // The G-buffer submissions that read a palette segment (slot, serial): shadowPalette reuses the segment only once
    // they are done.
    std::vector<std::pair<U32, U64>> mGbPaletteReaders[4];

    // A slot's set 0 binding 1: the shadows' palettes, or a stand-in (its own draws' buffer) where there are none. Only
    // while the slot is idle.
    void gbWritePalettes(U32 i, bool palettes)
    {
        GbSlot& slot = mGbSlots[i];
        const VkDescriptorBufferInfo info = palettes ? VkDescriptorBufferInfo{ mShPalettes, 0, VK_WHOLE_SIZE }
                                                     : VkDescriptorBufferInfo{ mGbHost, GB_SLOT_BYTES * i, GB_DRAW_BYTES };
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = slot.set;
        write.dstBinding      = 1;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &info;
        vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        slot.palettes = palettes;
    }
    std::map<GbMatKey, VkDescriptorSet> mGbMatSets;  // a set for each four images drawn with together
    std::unordered_multimap<VkImage, GbMatKey> mGbMatUse;  // the sets an image is in
    // For the status line: this frame's and the last whole frame's.
    U32              mGbFrame = 0;
    U32              mGbPassesNow = 0, mGbPassesLast = 0, mGbDrawsNow = 0, mGbDrawsLast = 0;
    U32              mGbTexturesNow = 0, mGbTexturesLast = 0;
    // Why the last face Vulkan was offered stayed GL's, for the status line and the log (once a reason).
    std::string      mGbWhy;
    std::set<std::string> mGbWhyLogged;

    void gbWhy(const std::string& why)
    {
        mGbWhy = why;
        if (mGbWhyLogged.insert(why).second)
        {
            LL_INFOS("Vulkan") << "Probe draws in Vulkan: a face stays GL's: " << why << LL_ENDL;
        }
    }

    bool gbFail(const std::string& why)
    {
        mGbNote = "failed: " + why;
        LL_WARNS("Vulkan") << "Probe draws in Vulkan " << mGbNote << LL_ENDL;
        destroyGbObjects();
        return false;
    }

    bool createGbObjects()
    {
        if (mGbTried)
        {
            return mGbReady;
        }
        mGbTried = true;
        if (!mActive || !mDevice || !mExtPassed || !mTexShared)
        {
            mGbNote = "needs QuasarVulkanTargets and QuasarVulkanTextures, their self-tests passed";
            return false;
        }
        if (!createArena(mGbArena, GB_ARENA_VERTEX_BYTES, true, true, GB_ARENA_INDEX_BYTES, "probe draws' arena", true,
                         true, true))
        {
            return gbFail("its arena");
        }
        // The draws' ring, host-visible.
        VkBufferCreateInfo host_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        host_info.size        = GB_SLOT_BYTES * GB_SLOTS;
        host_info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        host_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &host_info, nullptr, &mGbHost) != VK_SUCCESS)
        {
            mGbHost = VK_NULL_HANDLE;
            return gbFail("vkCreateBuffer (draws)");
        }
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(mDevice, mGbHost, &req);
        const U32 host_type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo host_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        host_alloc.allocationSize  = req.size;
        host_alloc.memoryTypeIndex = host_type;
        if (host_type == U32_MAX || vkAllocateMemory(mDevice, &host_alloc, nullptr, &mGbHostMemory) != VK_SUCCESS)
        {
            mGbHostMemory = VK_NULL_HANDLE;
            return gbFail("no host memory for the draws");
        }
        vkBindBufferMemory(mDevice, mGbHost, mGbHostMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(mDevice, mGbHostMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            return gbFail("vkMapMemory (draws)");
        }
        mGbHostMapped = static_cast<U8*>(mapped);

        // Set 0: the slot's draws, and the skins' palettes (the shadows', or a stand-in where there are none).
        VkDescriptorSetLayoutBinding draws_bindings[2] = {};
        draws_bindings[0].binding         = 0;
        draws_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        draws_bindings[0].descriptorCount = 1;
        draws_bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;  // both read it
        draws_bindings[1]                 = draws_bindings[0];
        draws_bindings[1].binding         = 1;
        draws_bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 2;
        set_info.pBindings    = draws_bindings;
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mGbSetLayout) != VK_SUCCESS)
        {
            mGbSetLayout = VK_NULL_HANDLE;
            return gbFail("vkCreateDescriptorSetLayout (draws)");
        }
        // Set 1: a draw's four textures (qlgbuffer.frag: base colour or diffuse, normal, ORM, emissive), and a batch's
        // sixteen diffuse textures (batchMap).
        VkDescriptorSetLayoutBinding tex_bindings[5] = {};
        for (U32 i = 0; i < 5; ++i)
        {
            tex_bindings[i].binding         = i;
            tex_bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tex_bindings[i].descriptorCount = i < 4 ? 1 : 16;
            tex_bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        set_info.bindingCount = 5;
        set_info.pBindings    = tex_bindings;
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mGbTexLayout) != VK_SUCCESS)
        {
            mGbTexLayout = VK_NULL_HANDLE;
            return gbFail("vkCreateDescriptorSetLayout (textures)");
        }
        const VkDescriptorSetLayout layouts[2] = { mGbSetLayout, mGbTexLayout };
        VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount         = 2;
        layout_info.pSetLayouts            = layouts;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &push;
        if (vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &mGbLayout) != VK_SUCCESS)
        {
            mGbLayout = VK_NULL_HANDLE;
            return gbFail("vkCreatePipelineLayout");
        }
        const VkDescriptorPoolSize slot_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GB_SLOTS * 2 };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets       = GB_SLOTS;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes    = &slot_size;
        if (vkCreateDescriptorPool(mDevice, &pool_info, nullptr, &mGbPool) != VK_SUCCESS)
        {
            mGbPool = VK_NULL_HANDLE;
            return gbFail("vkCreateDescriptorPool (draws)");
        }
        const VkDescriptorPoolSize tex_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                GB_MAX_TEXTURE_SETS * GB_SET_IMAGES };
        pool_info.flags      = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets    = GB_MAX_TEXTURE_SETS;
        pool_info.pPoolSizes = &tex_size;
        if (vkCreateDescriptorPool(mDevice, &pool_info, nullptr, &mGbTexPool) != VK_SUCCESS)
        {
            mGbTexPool = VK_NULL_HANDLE;
            return gbFail("vkCreateDescriptorPool (textures)");
        }
        // The diffuse's sampling as GL's ALSamplers::AnisoWrap: trilinear, repeating, anisotropic where the device
        // allows it.
        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler_info.magFilter        = VK_FILTER_LINEAR;
        sampler_info.minFilter        = VK_FILTER_LINEAR;
        sampler_info.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.anisotropyEnable = (mHasAniso && mGbAnisotropy > 1.f) ? VK_TRUE : VK_FALSE;
        sampler_info.maxAnisotropy    = mHasAniso ? llclamp(mGbAnisotropy, 1.f, 16.f) : 1.f;
        sampler_info.maxLod           = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(mDevice, &sampler_info, nullptr, &mGbSampler) != VK_SUCCESS)
        {
            mGbSampler = VK_NULL_HANDLE;
            return gbFail("vkCreateSampler");
        }
        VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        module_info.codeSize = sizeof(ql_spv_qlgbuffer_vert);
        module_info.pCode    = ql_spv_qlgbuffer_vert;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mGbVert) != VK_SUCCESS)
        {
            mGbVert = VK_NULL_HANDLE;
            return gbFail("vkCreateShaderModule (vertex)");
        }
        module_info.codeSize = sizeof(ql_spv_qlgbuffer_frag);
        module_info.pCode    = ql_spv_qlgbuffer_frag;
        if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mGbFrag) != VK_SUCCESS)
        {
            mGbFrag = VK_NULL_HANDLE;
            return gbFail("vkCreateShaderModule (fragment)");
        }
        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        for (U32 i = 0; i < GB_SLOTS; ++i)
        {
            GbSlot& slot = mGbSlots[i];
            VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            set_alloc.descriptorPool     = mGbPool;
            set_alloc.descriptorSetCount = 1;
            set_alloc.pSetLayouts        = &mGbSetLayout;
            if (vkAllocateCommandBuffers(mDevice, &cmd_info, &slot.cmd) != VK_SUCCESS ||
                vkCreateFence(mDevice, &fence_info, nullptr, &slot.fence) != VK_SUCCESS ||
                vkAllocateDescriptorSets(mDevice, &set_alloc, &slot.set) != VK_SUCCESS)
            {
                return gbFail("no command buffers or sets");
            }
            // Binding 1 starts as the stand-in (the slot's own draws' buffer); gbufferBegin points it at the shadows'
            // palettes once they exist (rigged draws need them).
            const VkDescriptorBufferInfo infos[2] = {
                { mGbHost, GB_SLOT_BYTES * i, (VkDeviceSize)GB_MAX_DRAWS * GB_DRAW_BYTES },
                { mGbHost, GB_SLOT_BYTES * i, GB_DRAW_BYTES },
            };
            VkWriteDescriptorSet writes[2] = {};
            for (U32 b = 0; b < 2; ++b)
            {
                writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet          = slot.set;
                writes[b].dstBinding      = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].pBufferInfo     = &infos[b];
            }
            vkUpdateDescriptorSets(mDevice, 2, writes, 0, nullptr);
            slot.palettes = false;
        }
        if (!createSharedSemaphore(mGbGLDoneVK, mGbGLDoneGL, mGbGLDoneHandle) ||
            !createSharedSemaphore(mGbVKDoneVK, mGbVKDoneGL, mGbVKDoneHandle))
        {
            return gbFail("its semaphores could not be shared");
        }
        if (!mGbWhite.image && !createGbDefaults())
        {
            return gbFail("its default textures");
        }
        mGbReady = true;
        mGbNote  = "ready";
        mTrReady = createTrObjects();
        LL_INFOS("Vulkan") << "Probe draws in Vulkan ready: an arena of " << (mGbArena.totalBytes() >> 20) << " MB, "
                           << GB_SLOTS << " slots of " << GB_MAX_DRAWS << " draws" << LL_ENDL;
        return true;
    }

    // White and the flat normal (128, 128, 255), as GL's defaults hold them, small, cleared once.
    bool createGbDefaults()
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmd_info.commandPool        = mCommandPool;
        cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(mDevice, &cmd_info, &cmd) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        bool made = true;
        // Black too: what GL's programs read through a unit with nothing bound (QuasarVulkanLighting).
        const float colors[4][4] = { { 1.f, 1.f, 1.f, 1.f }, { 128.f / 255.f, 128.f / 255.f, 1.f, 1.f },
                                     { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f, 1.f } };
        GbDefault* defaults[4] = { &mGbWhite, &mGbFlat, &mGbBlack, &mGbBlackCube };
        for (U32 i = 0; i < 4 && made; ++i)
        {
            GbDefault& d = *defaults[i];
            const bool cube = i == 3;  // what GL reads through a cube array unit with nothing bound
            const U32 layers = cube ? 6 : 1;
            VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            info.flags         = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            info.imageType     = VK_IMAGE_TYPE_2D;
            info.format        = VK_FORMAT_R8G8B8A8_UNORM;
            info.extent        = { 4, 4, 1 };
            info.mipLevels     = 1;
            info.arrayLayers   = layers;
            info.samples       = VK_SAMPLE_COUNT_1_BIT;
            info.tiling        = VK_IMAGE_TILING_OPTIMAL;
            info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(mDevice, &info, nullptr, &d.image) != VK_SUCCESS)
            {
                d.image = VK_NULL_HANDLE;
                made = false;
                break;
            }
            VkMemoryRequirements req = {};
            vkGetImageMemoryRequirements(mDevice, d.image, &req);
            VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            alloc.allocationSize  = req.size;
            alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (alloc.memoryTypeIndex == U32_MAX || vkAllocateMemory(mDevice, &alloc, nullptr, &d.memory) != VK_SUCCESS)
            {
                d.memory = VK_NULL_HANDLE;
                made = false;
                break;
            }
            vkBindImageMemory(mDevice, d.image, d.memory, 0);
            VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image            = d.image;
            view_info.viewType         = cube ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            view_info.format           = VK_FORMAT_R8G8B8A8_UNORM;
            view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
            if (vkCreateImageView(mDevice, &view_info, nullptr, &d.view) != VK_SUCCESS)
            {
                d.view = VK_NULL_HANDLE;
                made = false;
                break;
            }
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = d.image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
            VkClearColorValue value = {};
            memcpy(value.float32, colors[i], sizeof(colors[i]));
            vkCmdClearColorImage(cmd, d.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &b.subresourceRange);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        }
        vkEndCommandBuffer(cmd);
        if (made)
        {
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers    = &cmd;
            made = vkQueueSubmit(mQueue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
            vkDeviceWaitIdle(mDevice);  // once, as the draws are made ready
        }
        vkFreeCommandBuffers(mDevice, mCommandPool, 1, &cmd);
        return made;
    }

    void destroyGbDefault(GbDefault& d)
    {
        if (d.view) vkDestroyImageView(mDevice, d.view, nullptr);
        if (d.image) vkDestroyImage(mDevice, d.image, nullptr);
        if (d.memory) vkFreeMemory(mDevice, d.memory, nullptr);
        d = GbDefault();
    }

    void destroyGbTarget(GbTarget& t)
    {
        for (VkPipeline& p : t.pipelines)
        {
            if (p) vkDestroyPipeline(mDevice, p, nullptr);
        }
        for (VkPipeline& p : t.terrain)
        {
            if (p) vkDestroyPipeline(mDevice, p, nullptr);
        }
        if (t.framebuffer) vkDestroyFramebuffer(mDevice, t.framebuffer, nullptr);
        if (t.pass) vkDestroyRenderPass(mDevice, t.pass, nullptr);
        for (VkImageView& v : t.views)
        {
            if (v) vkDestroyImageView(mDevice, v, nullptr);
        }
        t = GbTarget();
    }

    // A target or texture is going: what the G-buffer draws made of it goes first. Main thread; the GPU is long done
    // with it (frees come EXT_FREE_PRESENTS presents after GL deleted the name).
    void gbForget(const ExtTexture& ext)
    {
        if (ext.block >= 0)
        {
            // A texture: the sets it is in go (made again, without it, when next drawn with).
            const auto range = mGbMatUse.equal_range(ext.image);
            std::vector<GbMatKey> keys;
            for (auto it = range.first; it != range.second; ++it)
            {
                keys.push_back(it->second);
            }
            for (const GbMatKey& key : keys)
            {
                const auto found = mGbMatSets.find(key);
                if (found == mGbMatSets.end())
                {
                    continue;
                }
                if (mGbTexPool) vkFreeDescriptorSets(mDevice, mGbTexPool, 1, &found->second);
                mGbMatSets.erase(found);
                for (VkImage image : key)
                {
                    auto use = mGbMatUse.equal_range(image);
                    for (auto it = use.first; it != use.second;)
                    {
                        it = it->second == key ? mGbMatUse.erase(it) : std::next(it);
                    }
                }
            }
            return;
        }
        if (mGbTargets.empty())
        {
            return;
        }
        for (auto it = mGbTargets.begin(); it != mGbTargets.end();)
        {
            bool uses = false;
            for (U32 i = 0; i < it->colors + 1; ++i)
            {
                uses = uses || (ext.image && it->images[i] == ext.image);
            }
            // The G-buffer lost an image: made again when next drawn.
            if (uses)
            {
                destroyGbTarget(*it);
                it = mGbTargets.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void destroyGbObjects()
    {
        mGbReady = false;
        mGbInPass = false;
        if (!mDevice)
        {
            return;
        }
        vkDeviceWaitIdle(mDevice);
        if (gl.Finish)
        {
            gl.Finish();
        }
        for (GbTarget& t : mGbTargets)
        {
            destroyGbTarget(t);
        }
        mGbTargets.clear();
        destroyTrObjects();
        mGbMatSets.clear();  // the pool goes, and its sets with it
        mGbMatUse.clear();
        destroyGbDefault(mGbWhite);
        destroyGbDefault(mGbBlack);
        destroyGbDefault(mGbBlackCube);
        destroyGbDefault(mGbFlat);
        for (GbSlot& slot : mGbSlots)
        {
            if (slot.fence) vkDestroyFence(mDevice, slot.fence, nullptr);
            if (slot.cmd) vkFreeCommandBuffers(mDevice, mCommandPool, 1, &slot.cmd);
            slot = GbSlot();
        }
        if (mGbTexPool) vkDestroyDescriptorPool(mDevice, mGbTexPool, nullptr);
        if (mGbPool) vkDestroyDescriptorPool(mDevice, mGbPool, nullptr);
        if (mGbLayout) vkDestroyPipelineLayout(mDevice, mGbLayout, nullptr);
        if (mGbSetLayout) vkDestroyDescriptorSetLayout(mDevice, mGbSetLayout, nullptr);
        if (mGbTexLayout) vkDestroyDescriptorSetLayout(mDevice, mGbTexLayout, nullptr);
        if (mGbSampler) vkDestroySampler(mDevice, mGbSampler, nullptr);
        if (mGbVert) vkDestroyShaderModule(mDevice, mGbVert, nullptr);
        if (mGbFrag) vkDestroyShaderModule(mDevice, mGbFrag, nullptr);
        mGbTexPool = mGbPool = VK_NULL_HANDLE;
        mGbLayout = VK_NULL_HANDLE;
        mGbSetLayout = mGbTexLayout = VK_NULL_HANDLE;
        mGbSampler = VK_NULL_HANDLE;
        mGbVert = mGbFrag = VK_NULL_HANDLE;
        if (mGbHostMapped)
        {
            vkUnmapMemory(mDevice, mGbHostMemory);
            mGbHostMapped = nullptr;
        }
        if (mGbHost) vkDestroyBuffer(mDevice, mGbHost, nullptr);
        if (mGbHostMemory) vkFreeMemory(mDevice, mGbHostMemory, nullptr);
        mGbHost = VK_NULL_HANDLE;
        mGbHostMemory = VK_NULL_HANDLE;
        destroyArena(mGbArena);
        if (mGbGLDoneGL) gl.DeleteSemaphoresEXT(1, &mGbGLDoneGL);
        if (mGbVKDoneGL) gl.DeleteSemaphoresEXT(1, &mGbVKDoneGL);
        if (mGbGLDoneVK) vkDestroySemaphore(mDevice, mGbGLDoneVK, nullptr);
        if (mGbVKDoneVK) vkDestroySemaphore(mDevice, mGbVKDoneVK, nullptr);
        if (mGbGLDoneHandle) CloseHandle(mGbGLDoneHandle);
        if (mGbVKDoneHandle) CloseHandle(mGbVKDoneHandle);
        mGbGLDoneGL = mGbVKDoneGL = 0;
        mGbGLDoneVK = mGbVKDoneVK = VK_NULL_HANDLE;
        mGbGLDoneHandle = mGbVKDoneHandle = nullptr;
    }

    // The Vulkan image behind a GL name in Vulkan's memory, or none.
    VkImage extImageOf(GLuint name) const
    {
        std::lock_guard<std::mutex> lock(mExtMutex);
        const auto found = mExtTextures.find(name);
        return found == mExtTextures.end() ? VK_NULL_HANDLE : found->second.image;
    }

    // A view of the whole of a name's image, made the first time it is asked for. Null where the name is not Vulkan's.
    VkImageView extView(GLuint name, VkImageAspectFlags aspect, VkFormat* format = nullptr, U32 levels = U32_MAX)
    {
        std::lock_guard<std::mutex> lock(mExtMutex);
        auto found = mExtTextures.find(name);
        if (found == mExtTextures.end())
        {
            return VK_NULL_HANDLE;
        }
        ExtTexture& ext = found->second;
        if (format)
        {
            *format = ext.vk;
        }
        if (!ext.view)
        {
            VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image            = ext.image;
            view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format           = ext.vk;
            // A name's levels GL samples are fixed for its life: a new discard level makes a new name.
            view_info.subresourceRange = { aspect, 0, llclamp(levels, 1u, llmax(1u, ext.levels)), 0, 1 };
            if (vkCreateImageView(mDevice, &view_info, nullptr, &ext.view) != VK_SUCCESS)
            {
                ext.view = VK_NULL_HANDLE;
            }
        }
        return ext.view;
    }

    // A texture slot's view: the texture's own (in Vulkan's memory, as far as GL samples it), or the slot's default
    // where none is named. Null where the texture cannot be drawn with: not Vulkan's, or one or two channels (they hold
    // luminance or alpha read through a swizzle GL's texture keeps, LLImageGL::applySwizzleForDeprecatedFormat, which a
    // view would not). srgb: the view decodes sRGB (the image is an sRGB format), which GL's data samplers do not.
    VkImageView gbSlotView(GLuint texture, U32 levels, U32 slot, VkImage& image, bool& srgb)
    {
        srgb = false;
        if (!texture)
        {
            const GbDefault& d = slot == 1 ? mGbFlat : mGbWhite;
            image = d.image;
            return d.view;
        }
        // The image, its format and its view in one look: another thread may delete the name and GL hand it out again.
        std::lock_guard<std::mutex> lock(mExtMutex);
        const auto found = mExtTextures.find(texture);
        if (found == mExtTextures.end() || found->second.block < 0 || found->second.vk == VK_FORMAT_R8_UNORM ||
            found->second.vk == VK_FORMAT_R8G8_UNORM || found->second.vk == VK_FORMAT_R32_SFLOAT ||
            found->second.vk == VK_FORMAT_R16G16_SFLOAT)
        {
            return VK_NULL_HANDLE;
        }
        ExtTexture& ext = found->second;
        if (!ext.view)
        {
            VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image            = ext.image;
            view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format           = ext.vk;
            // A name's levels GL samples are fixed for its life: a new discard level makes a new name.
            view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, llclamp(levels, 1u, llmax(1u, ext.levels)), 0, 1 };
            if (vkCreateImageView(mDevice, &view_info, nullptr, &ext.view) != VK_SUCCESS)
            {
                ext.view = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
        }
        image = ext.image;
        srgb  = ext.vk == VK_FORMAT_R8G8B8A8_SRGB || ext.vk == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                ext.vk == VK_FORMAT_BC2_SRGB_BLOCK || ext.vk == VK_FORMAT_BC3_SRGB_BLOCK;
        return ext.view;
    }

    // The descriptor set of a draw's four textures, made the first time they are drawn with together, kept until one
    // of them goes (gbForget). Null where one cannot be drawn with, or the pool is full: the draw is GL's.
    // batch: a batch's diffuse textures, batch_count of them (none for a draw of one).
    VkDescriptorSet gbMaterialSet(const unsigned textures[4], const unsigned levels[4], const unsigned* batch,
                                  const unsigned* batch_levels, U32 batch_count, U32& srgb_bits)
    {
        GbMatKey key = {};
        VkImageView views[GB_SET_IMAGES] = {};
        unsigned names[GB_SET_IMAGES] = {};
        srgb_bits = 0;
        for (U32 i = 0; i < GB_SET_IMAGES; ++i)
        {
            const U32 b = i - 4;
            names[i] = i < 4 ? textures[i] : (b < batch_count ? batch[b] : 0u);
            bool srgb = false;
            // The batch's slots are colour (decoded where sRGB, as the ladder's LINEAR_DIFFUSE samplers), white where
            // empty.
            views[i] = gbSlotView(names[i], i < 4 ? levels[i] : (b < batch_count ? batch_levels[b] : 1u), i < 4 ? i : 0,
                                  key[i], srgb);
            if (!views[i])
            {
                return VK_NULL_HANDLE;
            }
            srgb_bits |= (i < 4 && srgb) ? (1u << i) : 0u;
        }
        const auto found = mGbMatSets.find(key);
        if (found != mGbMatSets.end())
        {
            return found->second;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        set_alloc.descriptorPool     = mGbTexPool;
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts        = &mGbTexLayout;
        if (vkAllocateDescriptorSets(mDevice, &set_alloc, &set) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;  // the pool is full: these draws stay GL's
        }
        VkDescriptorImageInfo images[GB_SET_IMAGES];
        VkWriteDescriptorSet writes[5];
        for (U32 i = 0; i < GB_SET_IMAGES; ++i)
        {
            images[i] = { mGbSampler, views[i], VK_IMAGE_LAYOUT_GENERAL };
        }
        for (U32 i = 0; i < 5; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet          = set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = i < 4 ? 1 : 16;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &images[i];
        }
        vkUpdateDescriptorSets(mDevice, 5, writes, 0, nullptr);
        mGbMatSets.emplace(key, set);
        for (U32 i = 0; i < GB_SET_IMAGES; ++i)
        {
            // Defaults never go, and an image twice in a set is listed once.
            const bool repeated = std::find(key.begin(), key.begin() + i, key[i]) != key.begin() + i;
            if (names[i] && !repeated)
            {
                mGbMatUse.emplace(key[i], key);
            }
        }
        return set;
    }

    // The render pass, framebuffer and pipelines for a G-buffer: its colour attachments and depth, each in Vulkan's
    // memory. -1 where they are not.
    S32 gbTarget(const GLuint* colors, U32 color_count, GLuint depth, U32 width, U32 height)
    {
        if (color_count == 0 || color_count > 4 || !depth)
        {
            gbWhy(llformat("the G-buffer has %u colour attachments and depth %u", color_count, depth));
            return -1;
        }
        for (size_t i = 0; i < mGbTargets.size(); ++i)
        {
            const GbTarget& t = mGbTargets[i];
            bool same = t.colors == color_count && t.width == width && t.height == height && t.names[color_count] == depth &&
                        t.images[color_count] == extImageOf(depth);
            for (U32 c = 0; c < color_count && same; ++c)
            {
                same = t.names[c] == colors[c] && t.images[c] == extImageOf(colors[c]);
            }
            if (same)
            {
                return (S32)i;
            }
        }
        GbTarget t;
        t.colors = color_count;
        t.width  = width;
        t.height = height;
        VkAttachmentDescription attachments[5] = {};
        VkAttachmentReference color_refs[4] = {};
        for (U32 i = 0; i < color_count + 1; ++i)
        {
            const bool is_depth = i == color_count;
            const GLuint name = is_depth ? depth : colors[i];
            t.names[i] = name;
            const VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            // Views of targets are this module's own, of the attachment's aspect and first level.
            VkImage image = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            {
                std::lock_guard<std::mutex> lock(mExtMutex);
                const auto found = mExtTextures.find(name);
                if (found != mExtTextures.end())
                {
                    image  = found->second.image;
                    format = found->second.vk;
                }
            }
            if (!image)
            {
                gbWhy(llformat("its %s (GL name %u) is not in Vulkan's memory", is_depth ? "depth" : "colour attachment",
                               name));
                destroyGbTarget(t);
                return -1;
            }
            t.images[i] = image;
            VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image            = image;
            view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format           = format;
            view_info.subresourceRange = { aspect, 0, 1, 0, 1 };
            if (vkCreateImageView(mDevice, &view_info, nullptr, &t.views[i]) != VK_SUCCESS)
            {
                gbWhy("vkCreateImageView failed for an attachment");
                t.views[i] = VK_NULL_HANDLE;
                destroyGbTarget(t);
                return -1;
            }
            const bool has_stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
            VkAttachmentDescription& a = attachments[i];
            a.format         = format;
            a.samples        = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp  = has_stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = has_stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
            a.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;
            if (!is_depth)
            {
                color_refs[i] = { i, VK_IMAGE_LAYOUT_GENERAL };
            }
        }
        const VkAttachmentReference depth_ref = { color_count, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = color_count;
        subpass.pColorAttachments       = color_refs;
        subpass.pDepthStencilAttachment = &depth_ref;
        VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        pass_info.attachmentCount = color_count + 1;
        pass_info.pAttachments    = attachments;
        pass_info.subpassCount    = 1;
        pass_info.pSubpasses      = &subpass;
        if (vkCreateRenderPass(mDevice, &pass_info, nullptr, &t.pass) != VK_SUCCESS)
        {
            gbWhy("vkCreateRenderPass failed");
            t.pass = VK_NULL_HANDLE;
            destroyGbTarget(t);
            return -1;
        }
        VkFramebufferCreateInfo fb_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb_info.renderPass      = t.pass;
        fb_info.attachmentCount = color_count + 1;
        fb_info.pAttachments    = t.views;
        fb_info.width           = width;
        fb_info.height          = height;
        fb_info.layers          = 1;
        if (vkCreateFramebuffer(mDevice, &fb_info, nullptr, &t.framebuffer) != VK_SUCCESS)
        {
            gbWhy("vkCreateFramebuffer failed");
            t.framebuffer = VK_NULL_HANDLE;
            destroyGbTarget(t);
            return -1;
        }
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            const auto normals = color_count > 2 ? mExtTextures.find(colors[2]) : mExtTextures.end();
            t.normHdr = normals != mExtTextures.end() && normals->second.vk == VK_FORMAT_R16G16B16A16_UNORM;
        }
        for (U32 p = 0; p < 8; ++p)
        {
            if (!createGbPipeline(t, (p & 2) != 0, (p & 1) != 0, (p & 4) != 0, t.pipelines[p]))
            {
                gbWhy("its pipeline failed");
                destroyGbTarget(t);
                return -1;
            }
        }
        if (mTrReady)
        {
            for (U32 g = 0; g < 2; ++g)
            {
                if (!createTrPipeline(t, g != 0, t.terrain[g]))
                {
                    for (VkPipeline& p : t.terrain)  // the terrain stays GL's here; the rest is drawn
                    {
                        if (p) vkDestroyPipeline(mDevice, p, nullptr);
                        p = VK_NULL_HANDLE;
                    }
                    break;
                }
            }
        }
        mGbTargets.push_back(t);
        LL_INFOS("Vulkan") << "Probe draws in Vulkan: a " << width << "x" << height << " G-buffer of " << color_count
                           << " colour attachments" << LL_ENDL;
        return (S32)mGbTargets.size() - 1;
    }

    // masked: the fragment stage's discards are compiled in (qlgbuffer.frag's MASKED). Only the cut-outs' draws bind
    // it: a stage that can discard has its depth tested and written after it shades, which the opaque draws are spared.
    bool createGbPipeline(const GbTarget& t, bool greater, bool double_sided, bool masked, VkPipeline& pipeline)
    {
        const VkBool32 masked_value = masked ? VK_TRUE : VK_FALSE;
        const VkSpecializationMapEntry masked_entry = { 0, 0, sizeof(VkBool32) };
        VkSpecializationInfo specialization = {};
        specialization.mapEntryCount = 1;
        specialization.pMapEntries   = &masked_entry;
        specialization.dataSize      = sizeof(masked_value);
        specialization.pData         = &masked_value;
        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = mGbVert;
        stages[0].pName  = "main";
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = mGbFrag;
        stages[1].pName  = "main";
        stages[1].pSpecializationInfo = &specialization;
        // Positions, normals (16 bytes a vertex, as LLVertexBuffer keeps both), texture coordinates, colours, tangents.
        // And the legacy materials' second and third texture coordinates (normal and specular maps).
        const VkVertexInputBindingDescription bindings[8] = { { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 2, 8, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 3, 4, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 4, 16, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 5, 8, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 6, 8, VK_VERTEX_INPUT_RATE_VERTEX },
                                                              { 7, 16, VK_VERTEX_INPUT_RATE_VERTEX } };
        const VkVertexInputAttributeDescription attributes[9] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
                                                                  { 1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0 },
                                                                  { 2, 2, VK_FORMAT_R32G32_SFLOAT, 0 },
                                                                  { 3, 3, VK_FORMAT_R8G8B8A8_UNORM, 0 },
                                                                  { 4, 4, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
                                                                  { 5, 5, VK_FORMAT_R32G32_SFLOAT, 0 },
                                                                  { 6, 6, VK_FORMAT_R32G32_SFLOAT, 0 },
                                                                  { 7, 7, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
                                                                  // A batch's texture index: position.w's bits.
                                                                  { 8, 0, VK_FORMAT_R32_SINT, 12 } };
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex_input.vertexBindingDescriptionCount   = 8;
        vertex_input.pVertexBindingDescriptions      = bindings;
        vertex_input.vertexAttributeDescriptionCount = 9;
        vertex_input.pVertexAttributeDescriptions    = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;
        // As the shadows' pipelines: GL's counter-clockwise front, turned over by Vulkan's framebuffer rows.
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = double_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;  // a double-sided material
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        // Strict: GL's simple pool draws first, so the pools after it win its ties (LLGLDepthTest's GL_LEQUAL); drawn
        // last here, the simple prims must lose them to keep that.
        depth.depthCompareOp   = greater ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend_attachments[4] = {};
        for (VkPipelineColorBlendAttachmentState& b : blend_attachments)
        {
            b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = t.colors;
        blend.pAttachments    = blend_attachments;
        const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline_info.stageCount          = 2;
        pipeline_info.pStages             = stages;
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState   = &multisample;
        pipeline_info.pDepthStencilState  = &depth;
        pipeline_info.pColorBlendState    = &blend;
        pipeline_info.pDynamicState       = &dynamic;
        pipeline_info.layout              = mGbLayout;
        pipeline_info.renderPass          = t.pass;
        if (vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
            LL_WARNS("Vulkan") << "Probe draws in Vulkan: vkCreateGraphicsPipelines failed" << LL_ENDL;
            return false;
        }
        return true;
    }

    void gbufferFrame(U32 frame)
    {
        // The rigged draws' palettes are the shadows', kept a frame at a time: the shadows' frame moves on too, where
        // the G-buffer draws come first in it.
        if (mShReady)
        {
            shadowFrame(frame);
        }
        if (frame != mGbFrame)
        {
            mGbFrame = frame;
            mGbPassesLast = mGbPassesNow;
            mGbDrawsLast = mGbDrawsNow;
            mGbTexturesLast = mGbTexturesNow;
            mGbPassesNow = mGbDrawsNow = mGbTexturesNow = 0;
            mTrRegionsLast = mTrRegionsNow;
            mTrPatchesLast = mTrPatchesNow;
            mTrRegionsNow = mTrPatchesNow = 0;
        }
    }

    bool gbufferBegin(const GLuint* colors, U32 color_count, GLuint depth, U32 width, U32 height, bool greater,
                      bool remap_depth, F32 anisotropy)
    {
        if (!mGbTried)
        {
            mGbAnisotropy = anisotropy;  // the sampler's; a later change of GL's level is not followed (a restart is)
        }
        if (mGbInPass)
        {
            gbWhy("a face began inside another");
            return false;
        }
        if (!createGbObjects())
        {
            gbWhy(mGbNote);
            return false;
        }
        GbSlot& slot = mGbSlots[mGbNextSlot];
        if (slot.submitted)
        {
            if (vkGetFenceStatus(mDevice, slot.fence) != VK_SUCCESS)
            {
                gbWhy("all 16 submission slots still busy");
                return false;  // never waited for: the face stays GL's
            }
            slot.submitted = false;
        }
        // The idle slot's palettes: the shadows', where they exist now, or the stand-in.
        const bool palettes = mShReady && mShPalettes != VK_NULL_HANDLE;
        if (slot.palettes != palettes)
        {
            gbWritePalettes(mGbNextSlot, palettes);
        }
        const S32 target = gbTarget(colors, color_count, depth, width, height);
        if (target < 0)
        {
            return false;
        }
        if (arenaFull(mGbArena))
        {
            mGbArena.entries.clear();
            mGbArena.vertexTop = mGbArena.indexTop = 0;
            ++mGbArena.resets;
        }
        mGbSlot     = mGbNextSlot;
        mGbNextSlot = (mGbNextSlot + 1) % GB_SLOTS;
        mGbTarget   = target;
        mGbGreater  = greater;
        mGbRemap    = remap_depth;
        mGbDraws.clear();
        mGbPassTextures.clear();
        mGbPassImages.clear();
        mGbPassRigged = false;
        mTrQueued.clear();
        mTrPatches = 0;
        if (mTrReady)
        {
            vkResetDescriptorPool(mDevice, mTrTexPools[mGbSlot], 0);  // the slot is idle: its sets are done with
        }
        mGbInPass   = true;
        return true;
    }

    bool gbufferArena(U64 stamp, GLuint vertex_buffer, U32 position_offset, U32 normal_offset, U32 uv_offset,
                      U32 color_offset, U32 vertex_count, GLuint index_buffer, U32 index_count, U32& base_vertex,
                      U32& base_index, U32 tangent_offset, U32 uv1_offset, U32 uv2_offset, U32 weight_offset)
    {
        if (!mGbInPass)
        {
            return false;
        }
        return arenaPlace(mGbArena, stamp, vertex_buffer, position_offset, weight_offset, vertex_count, index_buffer,
                          index_count,
                          base_vertex, base_index, uv_offset, color_offset, normal_offset, true, tangent_offset,
                          uv1_offset, uv2_offset);
    }

    static constexpr VkShaderStageFlags TR_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                                                    VK_SHADER_STAGE_FRAGMENT_BIT;

    bool trFail(const char* what)
    {
        mTrNote = std::string("could not be made: ") + what;
        LL_WARNS("Vulkan") << "Terrain in Vulkan " << mTrNote << LL_ENDL;
        destroyTrObjects();
        return false;
    }

    // After the G-buffer's objects (its slots, its anisotropic sampler for the detail textures).
    bool createTrObjects()
    {
        if (!mHasTessellation)
        {
            mTrNote = "the device has no tessellation";
            return false;
        }
        // Set 0: the slot's regions (every stage) and patches (the vertex stage).
        VkDescriptorSetLayoutBinding set0[2] = {};
        for (U32 b = 0; b < 2; ++b)
        {
            set0[b].binding         = b;
            set0[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            set0[b].descriptorCount = 1;
            set0[b].stageFlags      = TR_STAGES;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 2;
        set_info.pBindings    = set0;
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mTrSetLayout) != VK_SUCCESS)
        {
            mTrSetLayout = VK_NULL_HANDLE;
            return trFail("vkCreateDescriptorSetLayout (regions)");
        }
        // Set 1: the heights, the composition, the alpha ramp, the four details (PBR: base colours), the PBR normals,
        // ORMs and emissives.
        VkDescriptorSetLayoutBinding set1[TR_TEXTURES] = {};
        for (U32 b = 0; b < TR_TEXTURES; ++b)
        {
            set1[b].binding         = b;
            set1[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            set1[b].descriptorCount = 1;
            // The maps the fragment stage alone reads (the ramp, the details, the PBR maps) are its alone: no stage
            // then sees more than the 16 samplers every device has.
            set1[b].stageFlags      = b < 2 ? TR_STAGES : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        set_info.bindingCount = TR_TEXTURES;
        set_info.pBindings    = set1;
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &mTrTexLayout) != VK_SUCCESS)
        {
            mTrTexLayout = VK_NULL_HANDLE;
            return trFail("vkCreateDescriptorSetLayout (textures)");
        }
        const VkDescriptorSetLayout layouts[2] = { mTrSetLayout, mTrTexLayout };
        const VkPushConstantRange push = { TR_STAGES, 0, 16 };
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount         = 2;
        layout_info.pSetLayouts            = layouts;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &push;
        if (vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &mTrLayout) != VK_SUCCESS)
        {
            mTrLayout = VK_NULL_HANDLE;
            return trFail("vkCreatePipelineLayout");
        }
        // The regions' host buffer, a slot's share each.
        VkBufferCreateInfo host_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        host_info.size        = TR_SLOT_BYTES * GB_SLOTS;
        host_info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        host_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &host_info, nullptr, &mTrHost) != VK_SUCCESS)
        {
            mTrHost = VK_NULL_HANDLE;
            return trFail("vkCreateBuffer");
        }
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(mDevice, mTrHost, &req);
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == U32_MAX || vkAllocateMemory(mDevice, &alloc, nullptr, &mTrHostMemory) != VK_SUCCESS)
        {
            mTrHostMemory = VK_NULL_HANDLE;
            return trFail("no host memory");
        }
        vkBindBufferMemory(mDevice, mTrHost, mTrHostMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(mDevice, mTrHostMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        {
            return trFail("vkMapMemory");
        }
        mTrHostMapped = static_cast<U8*>(mapped);
        const VkDescriptorPoolSize buffers = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GB_SLOTS * 2 };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets       = GB_SLOTS;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes    = &buffers;
        if (vkCreateDescriptorPool(mDevice, &pool_info, nullptr, &mTrPool) != VK_SUCCESS)
        {
            mTrPool = VK_NULL_HANDLE;
            return trFail("vkCreateDescriptorPool");
        }
        const VkDescriptorPoolSize samplers = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TR_MAX_REGIONS * TR_TEXTURES };
        VkDescriptorPoolCreateInfo tex_pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        tex_pool_info.maxSets       = TR_MAX_REGIONS;
        tex_pool_info.poolSizeCount = 1;
        tex_pool_info.pPoolSizes    = &samplers;
        for (U32 i = 0; i < GB_SLOTS; ++i)
        {
            if (vkCreateDescriptorPool(mDevice, &tex_pool_info, nullptr, &mTrTexPools[i]) != VK_SUCCESS)
            {
                mTrTexPools[i] = VK_NULL_HANDLE;
                return trFail("vkCreateDescriptorPool (textures)");
            }
            VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            set_alloc.descriptorPool     = mTrPool;
            set_alloc.descriptorSetCount = 1;
            set_alloc.pSetLayouts        = &mTrSetLayout;
            if (vkAllocateDescriptorSets(mDevice, &set_alloc, &mTrSets[i]) != VK_SUCCESS)
            {
                return trFail("vkAllocateDescriptorSets");
            }
            const VkDescriptorBufferInfo infos[2] = {
                { mTrHost, TR_SLOT_BYTES * i, TR_PATCHES_AT },
                { mTrHost, TR_SLOT_BYTES * i + TR_PATCHES_AT, (VkDeviceSize)TR_MAX_PATCHES * 16 },
            };
            VkWriteDescriptorSet writes[2] = {};
            for (U32 b = 0; b < 2; ++b)
            {
                writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet          = mTrSets[i];
                writes[b].dstBinding      = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].pBufferInfo     = &infos[b];
            }
            vkUpdateDescriptorSets(mDevice, 2, writes, 0, nullptr);
        }
        // GL's samplers for them: the heights PointClamp (texelFetch and textureGather ignore the filter, not the
        // edge), the composition BilinearClamp, the ramp AnisoClamp; the details AnisoWrapSRGB, the G-buffer's own.
        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler_info.magFilter    = VK_FILTER_NEAREST;
        sampler_info.minFilter    = VK_FILTER_NEAREST;
        sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(mDevice, &sampler_info, nullptr, &mTrPointClamp) != VK_SUCCESS)
        {
            mTrPointClamp = VK_NULL_HANDLE;
            return trFail("vkCreateSampler");
        }
        sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(mDevice, &sampler_info, nullptr, &mTrLinearClamp) != VK_SUCCESS)
        {
            mTrLinearClamp = VK_NULL_HANDLE;
            return trFail("vkCreateSampler");
        }
        sampler_info.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.anisotropyEnable = (mHasAniso && mGbAnisotropy > 1.f) ? VK_TRUE : VK_FALSE;
        sampler_info.maxAnisotropy    = mHasAniso ? llclamp(mGbAnisotropy, 1.f, 16.f) : 1.f;
        sampler_info.maxLod           = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(mDevice, &sampler_info, nullptr, &mTrAnisoClamp) != VK_SUCCESS)
        {
            mTrAnisoClamp = VK_NULL_HANDLE;
            return trFail("vkCreateSampler");
        }
        const struct
        {
            const uint32_t* code;
            size_t          size;
        } codes[4] = { { ql_spv_qlterrain_vert, sizeof(ql_spv_qlterrain_vert) },
                       { ql_spv_qlterrain_tesc, sizeof(ql_spv_qlterrain_tesc) },
                       { ql_spv_qlterrain_tese, sizeof(ql_spv_qlterrain_tese) },
                       { ql_spv_qlterrain_frag, sizeof(ql_spv_qlterrain_frag) } };
        for (U32 i = 0; i < 4; ++i)
        {
            VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            module_info.codeSize = codes[i].size;
            module_info.pCode    = codes[i].code;
            if (vkCreateShaderModule(mDevice, &module_info, nullptr, &mTrModules[i]) != VK_SUCCESS)
            {
                mTrModules[i] = VK_NULL_HANDLE;
                return trFail("vkCreateShaderModule");
            }
        }
        mTrNote = "ready";
        LL_INFOS("Vulkan") << "Terrain in Vulkan ready" << LL_ENDL;
        return true;
    }

    void destroyTrObjects()
    {
        mTrReady = false;
        if (!mDevice)
        {
            return;
        }
        for (VkShaderModule& m : mTrModules)
        {
            if (m) vkDestroyShaderModule(mDevice, m, nullptr);
            m = VK_NULL_HANDLE;
        }
        for (VkSampler* sampler : { &mTrPointClamp, &mTrLinearClamp, &mTrAnisoClamp })
        {
            if (*sampler) vkDestroySampler(mDevice, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
        for (U32 i = 0; i < GB_SLOTS; ++i)
        {
            if (mTrTexPools[i]) vkDestroyDescriptorPool(mDevice, mTrTexPools[i], nullptr);
            mTrTexPools[i] = VK_NULL_HANDLE;
            mTrSets[i]     = VK_NULL_HANDLE;
        }
        if (mTrPool) vkDestroyDescriptorPool(mDevice, mTrPool, nullptr);
        mTrPool = VK_NULL_HANDLE;
        if (mTrHostMapped)
        {
            vkUnmapMemory(mDevice, mTrHostMemory);
            mTrHostMapped = nullptr;
        }
        if (mTrHost) vkDestroyBuffer(mDevice, mTrHost, nullptr);
        if (mTrHostMemory) vkFreeMemory(mDevice, mTrHostMemory, nullptr);
        mTrHost = VK_NULL_HANDLE;
        mTrHostMemory = VK_NULL_HANDLE;
        if (mTrLayout) vkDestroyPipelineLayout(mDevice, mTrLayout, nullptr);
        if (mTrSetLayout) vkDestroyDescriptorSetLayout(mDevice, mTrSetLayout, nullptr);
        if (mTrTexLayout) vkDestroyDescriptorSetLayout(mDevice, mTrTexLayout, nullptr);
        mTrLayout = VK_NULL_HANDLE;
        mTrSetLayout = mTrTexLayout = VK_NULL_HANDLE;
    }

    // As createGbPipeline, for patches of four corners with no vertex input.
    bool createTrPipeline(const GbTarget& t, bool greater, VkPipeline& pipeline)
    {
        VkPipelineShaderStageCreateInfo stages[4] = {};
        const VkShaderStageFlagBits stage_bits[4] = { VK_SHADER_STAGE_VERTEX_BIT,
                                                      VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                      VK_SHADER_STAGE_FRAGMENT_BIT };
        for (U32 i = 0; i < 4; ++i)
        {
            stages[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage  = stage_bits[i];
            stages[i].module = mTrModules[i];
            stages[i].pName  = "main";
        }
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        // GL's domain origin (lower left; Vulkan 1.1), so the patches' triangles wind as GL's do.
        VkPipelineTessellationDomainOriginStateCreateInfo origin = {
            VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO };
        origin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
        VkPipelineTessellationStateCreateInfo tess = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
        tess.pNext              = &origin;
        tess.patchControlPoints = 4;
        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_BACK_BIT;  // GL's GL_CULL_FACE over the deferred pools
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp   = greater ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend_attachments[4] = {};
        for (VkPipelineColorBlendAttachmentState& b : blend_attachments)
        {
            b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = t.colors;
        blend.pAttachments    = blend_attachments;
        const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline_info.stageCount          = 4;
        pipeline_info.pStages             = stages;
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pTessellationState  = &tess;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState   = &multisample;
        pipeline_info.pDepthStencilState  = &depth;
        pipeline_info.pColorBlendState    = &blend;
        pipeline_info.pDynamicState       = &dynamic;
        pipeline_info.layout              = mTrLayout;
        pipeline_info.renderPass          = t.pass;
        if (vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
            mTrNote = "its pipeline could not be made";
            LL_WARNS("Vulkan") << "Terrain in Vulkan: vkCreateGraphicsPipelines failed" << LL_ENDL;
            return false;
        }
        return true;
    }

    // A region's terrain into the open pass. False where Vulkan cannot draw it (its maps or textures not Vulkan's,
    // the pass full, no pipeline): GL draws it.
    bool gbufferTerrain(const QLVulkanPresenter::TerrainDraw& d)
    {
        if (!mGbInPass || !mTrReady || mTrQueued.size() >= TR_MAX_REGIONS || !d.patch_count ||
            mTrPatches + d.patch_count > TR_MAX_PATCHES || !mGbTargets[mGbTarget].terrain[mGbGreater ? 1 : 0])
        {
            return false;
        }
        VkFormat height_format = VK_FORMAT_UNDEFINED, composition_format = VK_FORMAT_UNDEFINED;
        VkImageView views[TR_TEXTURES] = {};
        VkImage images[TR_TEXTURES] = {};
        GLuint names[TR_TEXTURES] = { d.height,    d.composition, d.ramp,        d.detail[0],   d.detail[1],
                                      d.detail[2], d.detail[3],   d.normal[0],   d.normal[1],   d.normal[2],
                                      d.normal[3], d.orm[0],      d.orm[1],      d.orm[2],      d.orm[3],
                                      d.emissive[0], d.emissive[1], d.emissive[2], d.emissive[3] };
        if (!d.pbr)
        {
            for (U32 i = 7; i < TR_TEXTURES; ++i)
            {
                names[i] = 0;  // the defaults; the four-texture terrain reads none of them
            }
        }
        views[0] = d.height ? extView(d.height, VK_IMAGE_ASPECT_COLOR_BIT, &height_format, 1) : VK_NULL_HANDLE;
        views[1] = d.composition ? extView(d.composition, VK_IMAGE_ASPECT_COLOR_BIT, &composition_format, 1)
                                 : VK_NULL_HANDLE;
        if (!views[0] || !views[1] || height_format != VK_FORMAT_R32_SFLOAT ||
            composition_format != VK_FORMAT_R16G16_SFLOAT)
        {
            return false;
        }
        images[0] = extImageOf(d.height);
        images[1] = extImageOf(d.composition);
        // The alpha ramp: GL_ALPHA8, kept as R8 (applySwizzleForDeprecatedFormat), read through its red channel.
        VkFormat ramp_format = VK_FORMAT_UNDEFINED;
        views[2] = d.ramp ? extView(d.ramp, VK_IMAGE_ASPECT_COLOR_BIT, &ramp_format, d.ramp_levels) : VK_NULL_HANDLE;
        if (!views[2] || (ramp_format != VK_FORMAT_R8_UNORM && ramp_format != VK_FORMAT_R8G8B8A8_UNORM &&
                          ramp_format != VK_FORMAT_R8G8B8A8_SRGB))
        {
            return false;
        }
        images[2] = extImageOf(d.ramp);
        const F32 ramp_red = ramp_format == VK_FORMAT_R8_UNORM ? 1.f : 0.f;
        // The details (base colours) must be there; a PBR material's other maps may be the slot's default (0). The
        // data maps (normal, ORM) read through a decoding view are read raw (qlterrain.frag's dataGrad).
        U32 raw_bits = 0;
        for (U32 i = 3; i < TR_TEXTURES; ++i)
        {
            const U32 m = (i - 3) % 4;
            const U32 kind = (i - 3) / 4;  // 0 base colour, 1 normal, 2 ORM, 3 emissive
            const U32 levels = kind == 0 ? d.detail_levels[m]
                               : kind == 1 ? d.normal_levels[m]
                               : kind == 2 ? d.orm_levels[m]
                                           : d.emissive_levels[m];
            if (kind == 0 && !names[i] && !d.pbr)
            {
                return false;  // a detail not loaded; a PBR base colour of 0 is the slot's white
            }
            bool srgb = false;
            views[i] = gbSlotView(names[i], levels, kind == 1 ? 1u : 0u, images[i], srgb);
            if (!views[i])
            {
                return false;
            }
            if (srgb && kind == 1)
            {
                raw_bits |= 1u << m;
            }
            else if (srgb && kind == 2)
            {
                raw_bits |= 16u << m;
            }
        }
        VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        set_alloc.descriptorPool     = mTrTexPools[mGbSlot];
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts        = &mTrTexLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(mDevice, &set_alloc, &set) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorImageInfo infos[TR_TEXTURES];
        VkWriteDescriptorSet writes[TR_TEXTURES];
        for (U32 i = 0; i < TR_TEXTURES; ++i)
        {
            const VkSampler sampler = i == 0 ? mTrPointClamp : i == 1 ? mTrLinearClamp : i == 2 ? mTrAnisoClamp
                                                                                            : mGbSampler;
            infos[i] = { sampler, views[i], VK_IMAGE_LAYOUT_GENERAL };
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet          = set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(mDevice, TR_TEXTURES, writes, 0, nullptr);

        // The region's data (qlterrain's Region) and its patches.
        const U32 region = (U32)mTrQueued.size();
        F32* out = reinterpret_cast<F32*>(mTrHostMapped + TR_SLOT_BYTES * mGbSlot + TR_REGION_BYTES * region);
        memcpy(out + 0, d.mvp, 16 * sizeof(F32));
        memcpy(out + 16, d.normal_matrix, 16 * sizeof(F32));
        memcpy(out + 32, d.tess_clip, 16 * sizeof(F32));
        memcpy(out + 48, d.tess_clip_origin, 4 * sizeof(F32));
        const F32 rest[20] = { d.tess_origin[0], d.tess_origin[1], d.tess_origin[2], d.density,
                               d.grid_scale, d.region_scale, d.smoothing ? 1.f : 0.f, d.triplanar_blend,
                               d.plane_s[0], d.plane_s[1], d.plane_s[2], d.plane_s[3],
                               d.plane_t[0], d.plane_t[1], d.plane_t[2], d.plane_t[3],
                               d.raster[0], d.raster[1], (F32)d.planar_count, ramp_red };
        memcpy(out + 52, rest, sizeof(rest));
        F32 pbr[84] = {};
        pbr[0] = d.pbr ? 1.f : 0.f;
        pbr[1] = (F32)raw_bits;
        for (U32 m = 0; m < 4; ++m)
        {
            memcpy(pbr + 4 + 4 * m, d.uv_transform[m], 4 * sizeof(F32));
            pbr[20 + 2 * m]     = d.uv_offset[m][0];
            pbr[20 + 2 * m + 1] = d.uv_offset[m][1];
            memcpy(pbr + 28 + 4 * m, d.normal_axes[m], 4 * sizeof(F32));
            memcpy(pbr + 44 + 4 * m, d.base_color[m], 4 * sizeof(F32));
            pbr[60 + m] = d.metallic[m];
            pbr[64 + m] = d.roughness[m];
            memcpy(pbr + 68 + 4 * m, d.emissive_color[m], 3 * sizeof(F32));
        }
        memcpy(out + 72, pbr, sizeof(pbr));
        F32* patches = reinterpret_cast<F32*>(mTrHostMapped + TR_SLOT_BYTES * mGbSlot + TR_PATCHES_AT) + 4 * mTrPatches;
        for (U32 i = 0; i < d.patch_count; ++i)
        {
            patches[4 * i + 0] = d.patches[3 * i + 0];
            patches[4 * i + 1] = d.patches[3 * i + 1];
            patches[4 * i + 2] = d.patches[3 * i + 2];
            patches[4 * i + 3] = (F32)region;
        }
        mTrQueued.push_back({ set, mTrPatches, d.patch_count });
        mTrPatches += d.patch_count;
        for (U32 i = 0; i < TR_TEXTURES; ++i)
        {
            if (names[i] && std::find(mGbPassTextures.begin(), mGbPassTextures.end(), names[i]) == mGbPassTextures.end())
            {
                mGbPassTextures.push_back(names[i]);
                mGbPassImages.push_back(images[i]);
            }
        }
        return true;
    }

    std::string terrainInfo() const
    {
        if (!mTrReady)
        {
            return mTrNote;
        }
        return llformat("%u region(s), %u patches last frame", mTrRegionsLast, mTrPatchesLast);
    }

    bool gbufferDraw(const QLVulkanPresenter::GbDraw& d)
    {
        if (!mGbInPass || mGbDraws.size() >= GB_MAX_DRAWS || d.kind > QLVulkanPresenter::GB_BUMP ||
            (d.palette != ~0u && (!mGbSlots[mGbSlot].palettes || d.palette >= (1u << 24))))
        {
            return false;
        }
        // A simple draw reads its diffuse alone: its other slots take the defaults, whatever it names.
        // A material reads its maps where it has them, and no fourth.
        const bool simple   = d.kind == QLVulkanPresenter::GB_SIMPLE;
        const bool material = d.kind == QLVulkanPresenter::GB_MATERIAL;
        const bool bump     = d.kind == QLVulkanPresenter::GB_BUMP;  // the diffuse and the bump image alone
        const unsigned textures[4] = { d.textures[0],
                                       simple || (material && !d.has_normal_map) ? 0u : d.textures[1],
                                       simple || bump || (material && !d.has_specular_map) ? 0u : d.textures[2],
                                       simple || material || bump ? 0u : d.textures[3] };
        if (bump && (!textures[0] || !textures[1]))
        {
            return false;
        }
        const bool batched = simple && d.batch_count > 1;
        if (d.batch_count > 16 || (d.batch_count && !batched) || ((simple || material) && !textures[0] && !batched))
        {
            return false;
        }
        if (material && ((d.has_normal_map && !textures[1]) || (d.has_specular_map && !textures[2])))
        {
            return false;  // a map named but not given: GL's (it binds the map itself)
        }
        U32 srgb_bits = 0;
        const VkDescriptorSet set =
            gbMaterialSet(textures, d.levels, d.batch, d.batch_levels, batched ? d.batch_count : 0u, srgb_bits);
        if (!set)
        {
            return false;
        }
        const U32 index = (U32)mGbDraws.size();
        U8* slot = mGbHostMapped + GB_SLOT_BYTES * mGbSlot;
        // qlgbuffer.vert's Draw.
        float* out = reinterpret_cast<float*>(slot + (size_t)index * GB_DRAW_BYTES);
        memcpy(out, d.mvp, 64);
        memcpy(out + 16, d.normal_matrix, 64);
        memcpy(out + 32, d.texture_matrix, 64);
        memcpy(out + 48, d.transforms, sizeof(d.transforms));  // 32 floats
        out[80] = d.metallic;
        out[81] = d.roughness;
        out[82] = (float)d.kind;
        out[83] = (float)srgb_bits;
        out[84] = d.emissive[0];
        out[85] = d.emissive[1];
        out[86] = d.emissive[2];
        out[87] = 0.f;
        memcpy(out + 88, d.specular, sizeof(d.specular));  // legacy material: specular colour, glossiness
        out[92] = d.env_intensity;
        out[93] = d.alpha_cutoff;
        out[94] = d.fullbright;
        out[95] = (float)(d.alpha_mode | (d.has_normal_map ? 4u : 0u) | (d.has_specular_map ? 8u : 0u));
        // A rigged draw: its palette's first vec4 (shadowPalette), and that it skins.
        const bool rigged = d.palette != ~0u;
        out[96] = rigged ? (float)d.palette : 0.f;
        out[97] = rigged ? 1.f : 0.f;
        out[98] = batched ? 1.f : 0.f;  // qlgbuffer.frag's diffuseLookup reads the batch
        out[99] = 0.f;
        VkDrawIndexedIndirectCommand cmd = {};
        cmd.indexCount    = d.index_count;
        cmd.instanceCount = 1;
        cmd.firstIndex    = d.first_index;
        cmd.vertexOffset  = (S32)d.base_vertex;
        cmd.firstInstance = index;
        memcpy(slot + (size_t)GB_MAX_DRAWS * GB_DRAW_BYTES + (size_t)index * sizeof(cmd), &cmd, sizeof(cmd));
        // A cut-out (alpha mode MASK: the simple and PBR kinds' cut-out passes, a legacy material's mask) binds the
        // pipeline that can discard.
        mGbDraws.push_back({ set, index, (d.alpha_mode == 2 ? 4u : 0u) | (d.double_sided ? 1u : 0u), cmd });
        mGbPassRigged = mGbPassRigged || d.palette != ~0u;
        for (U32 i = 0; i < 4 + (batched ? d.batch_count : 0u); ++i)
        {
            const unsigned name = i < 4 ? textures[i] : d.batch[i - 4];
            if (name && std::find(mGbPassTextures.begin(), mGbPassTextures.end(), (GLuint)name) == mGbPassTextures.end())
            {
                mGbPassTextures.push_back(name);
                mGbPassImages.push_back(extImageOf(name));
            }
        }
        return true;
    }

    // The face's draws, over what GL drew of its G-buffer: one hand-over, waited for on the GPU by GL before it lights.
    bool gbufferEnd()
    {
        if (!mGbInPass)
        {
            return false;
        }
        mGbInPass = false;
        if (mGbDraws.empty() && mTrQueued.empty())
        {
            return true;
        }
        LL_PROFILE_ZONE_NAMED("vulkan probe draws");
        const GbTarget& target = mGbTargets[mGbTarget];
        // The shadow maps queued for this view, carried by this hand-over (QuasarVulkanMergeHandOvers): one crossing
        // for both. Recorded first, so their copies in from GL come before the signal.
        ShFlush sh;
        bool merged = false;
        if (mShMergeNext && !mShPending.empty())
        {
            LL_PROFILE_ZONE_NAMED("vulkan probe draws - shadows record");
            merged = shadowRecord(sh);
            mShMergeFailed = !merged;
        }
        mShMergeNext = false;
        // What GL hands over: the arena, the attachments, and every texture drawn with (once each); and the shadows'.
        std::vector<GLuint> textures(target.names, target.names + target.colors + 1);
        textures.insert(textures.end(), mGbPassTextures.begin(), mGbPassTextures.end());
        const std::vector<VkImage>& texture_images = mGbPassImages;
        std::vector<GLuint> gl_buffers = { mGbArena.gl };
        if (merged && sShSignalObjects)
        {
            gl_buffers.insert(gl_buffers.end(), sh.gl_buffers.begin(), sh.gl_buffers.end());
            textures.insert(textures.end(), sh.gl_textures.begin(), sh.gl_textures.end());
        }
        const std::vector<GLenum> layouts(textures.size(), GL_LAYOUT_GENERAL_EXT);
        {
            LL_PROFILE_ZONE_NAMED("vulkan probe draws - gl signal");
            gl.SignalSemaphoreEXT(mGbGLDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(), (GLuint)textures.size(),
                                  textures.data(), layouts.data());
            gl.Flush();
        }
        LL_PROFILE_ZONE_NAMED_COLOR("vulkan probe draws - vk record", 0x40A0FF);
        GbSlot& slot = mGbSlots[mGbSlot];
        vkResetFences(mDevice, 1, &slot.fence);
        vkResetCommandBuffer(slot.cmd, 0);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(slot.cmd, &begin);
        bufferBarrier(slot.cmd, mGbArena.buffer, 0, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT,
                      VK_QUEUE_FAMILY_EXTERNAL, mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
        // The attachments and textures from the external queue, GENERAL as GL leaves them.
        std::vector<VkImageMemoryBarrier> acquires;
        for (U32 i = 0; i < target.colors + 1; ++i)
        {
            const bool is_depth = i == target.colors;
            VkFormat format = VK_FORMAT_UNDEFINED;
            {
                std::lock_guard<std::mutex> lock(mExtMutex);
                format = mExtTextures[target.names[i]].vk;
            }
            const bool stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.dstAccessMask       = is_depth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                             : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            b.dstQueueFamilyIndex = mQueueFamily;
            b.image               = extImageOf(target.names[i]);
            b.subresourceRange    = { is_depth ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                                       (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                               : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                                      0, VK_REMAINING_MIP_LEVELS, 0, 1 };
            acquires.push_back(b);
        }
        for (VkImage image : texture_images)
        {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            b.dstQueueFamilyIndex = mQueueFamily;
            b.image               = image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
            acquires.push_back(b);
        }
        // The terrain reads its maps from its vertex and tessellation stages too.
        const VkPipelineStageFlags texture_stages =
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
            VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             texture_stages | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, (U32)acquires.size(), acquires.data());

        VkRenderPassBeginInfo pass_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        pass_begin.renderPass  = target.pass;
        pass_begin.framebuffer = target.framebuffer;
        pass_begin.renderArea  = { { 0, 0 }, { target.width, target.height } };
        vkCmdBeginRenderPass(slot.cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport = { 0.f, 0.f, (float)target.width, (float)target.height, 0.f, 1.f };
        const VkRect2D scissor = { { 0, 0 }, { target.width, target.height } };
        vkCmdSetViewport(slot.cmd, 0, 1, &viewport);
        vkCmdSetScissor(slot.cmd, 0, 1, &scissor);
        // Flags: the depth remap, the normals' format (GBUFFER_NORM_HDR), whether there is an emissive attachment.
        const float flags[4] = { mGbRemap ? 1.f : 0.f, target.normHdr ? 1.f : 0.f, target.colors > 3 ? 1.f : 0.f, 0.f };
        // The terrain first: the largest occluder, so the depth it lays turns away what it hides of the rest.
        if (!mTrQueued.empty())
        {
            vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, target.terrain[mGbGreater ? 1 : 0]);
            vkCmdPushConstants(slot.cmd, mTrLayout, TR_STAGES, 0, sizeof(flags), flags);
            vkCmdBindDescriptorSets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mTrLayout, 0, 1, &mTrSets[mGbSlot], 0,
                                    nullptr);
            for (const TrQueued& q : mTrQueued)
            {
                vkCmdBindDescriptorSets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mTrLayout, 1, 1, &q.set, 0, nullptr);
                vkCmdDraw(slot.cmd, 4, q.count, 0, q.first);
            }
        }
        vkCmdPushConstants(slot.cmd, mGbLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(flags), flags);
        vkCmdBindDescriptorSets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mGbLayout, 0, 1, &slot.set, 0, nullptr);
        const VkBuffer buffers[8] = { mGbArena.buffer, mGbArena.buffer, mGbArena.buffer, mGbArena.buffer,
                                      mGbArena.buffer, mGbArena.buffer, mGbArena.buffer, mGbArena.buffer };
        const VkDeviceSize offsets[8] = { 0, mGbArena.normalStart(), mGbArena.uvStart(), mGbArena.colorStart(),
                                          mGbArena.tangentStart(), mGbArena.uv1Start(), mGbArena.uv2Start(),
                                          mGbArena.vertexBytes };  // the weights follow the positions
        vkCmdBindVertexBuffers(slot.cmd, 0, 8, buffers, offsets);
        vkCmdBindIndexBuffer(slot.cmd, mGbArena.buffer, mGbArena.indexStart(), VK_INDEX_TYPE_UINT16);
        const VkDeviceSize commands = GB_SLOT_BYTES * mGbSlot + (VkDeviceSize)GB_MAX_DRAWS * GB_DRAW_BYTES;
        VkDrawIndexedIndirectCommand* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(mGbHostMapped + commands);
        // The cut-outs last, so the opaque draws' early depth test turns away what they hide before it is shaded. The
        // commands follow them (each keeps its draw's index as its first instance), so a run of draws is still a run.
        const auto masked_from =
            std::stable_partition(mGbDraws.begin(), mGbDraws.end(), [](const GbQueued& q) { return (q.pipe & 4) == 0; });
        if (masked_from != mGbDraws.end() && masked_from != mGbDraws.begin())
        {
            for (size_t k = 0; k < mGbDraws.size(); ++k)
            {
                memcpy(cmds + k, &mGbDraws[k].cmd, sizeof(VkDrawIndexedIndirectCommand));
            }
        }
        // Draws of one set and pipeline in a row: one indirect draw.
        U32 bound_pipe = U32_MAX;
        for (size_t first = 0; first < mGbDraws.size();)
        {
            size_t last = first + 1;
            while (last < mGbDraws.size() && mGbDraws[last].set == mGbDraws[first].set &&
                   mGbDraws[last].pipe == mGbDraws[first].pipe)
            {
                ++last;
            }
            if (mGbDraws[first].pipe != bound_pipe)
            {
                bound_pipe = mGbDraws[first].pipe;
                vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  target.pipelines[(bound_pipe & 4) + (mGbGreater ? 2 : 0) + (bound_pipe & 1)]);
            }
            const VkDescriptorSet tex_set = mGbDraws[first].set;
            vkCmdBindDescriptorSets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mGbLayout, 1, 1, &tex_set, 0, nullptr);
            const U32 count = (U32)(last - first);
            if (mHasMultiIndirect)
            {
                vkCmdDrawIndexedIndirect(slot.cmd, mGbHost, commands + (VkDeviceSize)first * sizeof(*cmds), count,
                                         sizeof(VkDrawIndexedIndirectCommand));
            }
            else
            {
                for (size_t d = first; d < last; ++d)
                {
                    const VkDrawIndexedIndirectCommand& c = mGbDraws[d].cmd;
                    vkCmdDrawIndexed(slot.cmd, c.indexCount, 1, c.firstIndex, c.vertexOffset, c.firstInstance);
                }
            }
            first = last;
        }
        vkCmdEndRenderPass(slot.cmd);
        bufferBarrier(slot.cmd, mGbArena.buffer, 0, 0, mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        std::vector<VkImageMemoryBarrier> releases = acquires;
        for (size_t i = 0; i < releases.size(); ++i)
        {
            VkImageMemoryBarrier& r = releases[i];
            r.srcAccessMask       = i < target.colors ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                                      : (i == target.colors ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                                                            : 0);
            r.dstAccessMask       = 0;
            r.srcQueueFamilyIndex = mQueueFamily;
            r.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        vkCmdPipelineBarrier(slot.cmd,
                             texture_stages | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, (U32)releases.size(),
                             releases.data());
        vkEndCommandBuffer(slot.cmd);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &mGbGLDoneVK;
        submit.pWaitDstStageMask    = &wait_stage;
        // The shadows' commands first in the batch: one wait on GL, one signal back.
        const VkCommandBuffer submitted[2] = { merged ? sh.cmd : slot.cmd, slot.cmd };
        submit.commandBufferCount   = merged ? 2 : 1;
        submit.pCommandBuffers      = submitted;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &mGbVKDoneVK;
        VkResult result = VK_SUCCESS;
        {
            LL_PROFILE_ZONE_NAMED("vulkan probe draws - vk submit");
            result = vkQueueSubmit(mQueue, 1, &submit, slot.fence);
        }
        if (merged)
        {
            // The shadow slots' fence, signalled once all of it (the batch above) is done. Its own result: a batch that
            // went is the G-buffer's either way, and GL still waits for it.
            const VkResult fence_result =
                result == VK_SUCCESS ? vkQueueSubmit(mQueue, 0, nullptr, sh.head->fence) : result;
            if (!shadowSubmitted(sh, fence_result))
            {
                merged         = false;  // let go (shFail); the viewer's next shadowFlush reports it
                mShMergeFailed = true;
            }
        }
        if (result != VK_SUCCESS)
        {
            // These draws are lost this once; nothing is tried again.
            gbFail(llformat("could not be submitted (VkResult %d)", (S32)result));
            return false;
        }
        slot.submitted = true;
        slot.serial    = ++mGbSubmitSerial;
        if (mGbPassRigged)
        {
            mGbPaletteReaders[mShFrame % SH_PALETTE_SEGMENTS].emplace_back(mGbSlot, slot.serial);
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan probe draws - gl wait");
            gl.WaitSemaphoreEXT(mGbVKDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(), (GLuint)textures.size(),
                                textures.data(), layouts.data());
        }
        if (merged)
        {
            shadowFinish(sh);
            ++mShMergedNow;
        }
        ++mGbPassesNow;
        mTrRegionsNow += (U32)mTrQueued.size();
        mTrPatchesNow += mTrPatches;
        mGbDrawsNow += (U32)mGbDraws.size();
        mGbTexturesNow += (U32)texture_images.size();
        return true;
    }

    void gbufferAbandon()
    {
        mGbInPass = false;
        mGbDraws.clear();
    }

    std::string gbufferInfo() const
    {
        if (!mGbReady)
        {
            return mGbNote + (mGbWhy.empty() || mGbWhy == mGbNote ? std::string() : " | " + mGbWhy);
        }
        return llformat("%u draws (%u textures) in %u faces last frame | arena %.0f/%.0f MB, %u resets, %u G-buffers",
                        mGbDrawsLast, mGbTexturesLast, mGbPassesLast, (double)mGbArena.used() / (1 << 20),
                        (double)mGbArena.totalBytes() / (1 << 20), mGbArena.resets, (U32)mGbTargets.size()) +
               (mGbWhy.empty() ? std::string() : " | last kept by GL: " + mGbWhy);
    }

    // ---- GL's own programs drawn by Vulkan (QuasarVulkanLighting) ---------------------------------------------
    //
    // A GL program's sources (as GL compiled them) are compiled for Vulkan as they stand (qlvkglsl.h): loose uniforms in
    // one block, every resource in set 0 at the binding glslang gave it. A draw fills that block from what GL holds for
    // the program (its regions, by name), copies the engine's blocks bound now, and binds each sampler's texture, every
    // one in Vulkan's memory. Draws are recorded as they come and handed over at glPassEnd as the G-buffer's are: one GL
    // signal naming every image they touch, one submission, one GL wait. Images stay GENERAL; between a draw that writes
    // an image and one that reads it, a barrier.
    struct GpPipe
    {
        U64        key = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    struct GpProgram
    {
        U64                   key = 0;
        U32                   link = 0;
        std::string           name;
        bool                  ok = false;
        std::string           note;
        ql_glsl::Program      refl;
        VkShaderModule        modules[ql_glsl::STAGE_COUNT] = {};
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout = VK_NULL_HANDLE;
        std::unordered_map<std::string, U32> members;  // the default block's, by name
        S32                   position = -1;            // the "position" attribute's location
        bool                  reads_emissive = false;   // diffuse_color then reads the emissive stream, as GL feeds it
        U32                   outputs = 0;              // colour outputs (the highest location + 1)
        // Its attributes, each a stream of a mesh in the arena (GpStream): where a draw gives positions alone, only
        // "position" may be read.
        struct Input
        {
            U32      stream = 0;
            U32      location = 0;
            VkFormat format = VK_FORMAT_UNDEFINED;
        };
        std::vector<Input>    inputs;
        std::vector<GpPipe>   pipes;
    };
    std::vector<GpProgram> mGpPrograms;
    std::vector<GpProgram> mGpRetiredPrograms;  // relinked while a pass was recorded; destroyed as the next begins

    static constexpr U32 GP_SLOTS = 16;  // runs in flight: a frame's lighting, alpha and post chain take several each
    static constexpr VkDeviceSize GP_SLOT_BYTES = 8ull << 20;  // a run's blocks and vertices (0.1 MB with blocks reused)
    static constexpr U32          GP_SETS = 1024;               // a pass's draws
    static constexpr VkDeviceSize GP_NONE = ~(VkDeviceSize)0;  // no room
    struct GpSlot
    {
        VkCommandBuffer  cmd = VK_NULL_HANDLE;
        VkFence          fence = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        bool             submitted = false;
    };
    GpSlot           mGpSlots[GP_SLOTS];
    U32              mGpSlot = 0, mGpNextSlot = 0;
    VkBuffer         mGpHost = VK_NULL_HANDLE;
    VkDeviceMemory   mGpHostMemory = VK_NULL_HANDLE;
    U8*              mGpMapped = nullptr;
    VkDeviceSize     mGpTop = 0;  // the pass's next free byte, from its slot's start
    // Blocks already written this pass, by what they hold: a draw whose block reads the same bytes as one before it
    // binds that one (the engine's blocks barely change within a pass, and many draws share their loose uniforms).
    struct GpWritten
    {
        VkDeviceSize    at = 0;
        std::vector<U8> bytes;
    };
    std::unordered_map<U64, GpWritten> mGpBlocksDone;
    std::vector<U8>                    mGpBlockScratch;
    U32                                mGpBlocksReused = 0, mGpBlocksWritten = 0;
    U32                                mGpBlocksReusedLast = 0, mGpBlocksWrittenLast = 0;
    VkDeviceSize                       mGpHostPeak = 0, mGpHostPeakLast = 0;
    VkDeviceSize     mGpAlign = 256;
    VkSemaphore      mGpGLDoneVK = VK_NULL_HANDLE, mGpVKDoneVK = VK_NULL_HANDLE;
    GLuint           mGpGLDoneGL = 0, mGpVKDoneGL = 0;
    HANDLE           mGpGLDoneHandle = nullptr, mGpVKDoneHandle = nullptr;
    bool             mGpTried = false, mGpReady = false;
    std::string      mGpNote = "not used yet";
    std::string      mGpWhy;  // the last draw or pass GL kept, and why

    // A target: its attachments' views, a render pass that loads them (and one that clears the colours), its framebuffer.
    struct GpTarget
    {
        GLuint        names[5] = {};
        VkImage       images[5] = {};
        VkImageView   views[5] = {};
        VkFormat      formats[5] = {};
        U32           colors = 0;
        bool          depth = false;
        U32           width = 0, height = 0;
        VkRenderPass  load = VK_NULL_HANDLE, clear = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        U64           formatKey = 0;
    };
    std::vector<GpTarget> mGpTargets;

    // Sampled views (a texture's whole, one aspect), and samplers by their description.
    struct GpView
    {
        GLuint             name = 0;
        VkImage            image = VK_NULL_HANDLE;
        VkImageAspectFlags aspect = 0;
        VkFormat           format = VK_FORMAT_UNDEFINED;  // the view's (a linear twin where read undecoded)
        VkImageView        view = VK_NULL_HANDLE;
    };
    std::vector<GpView> mGpViews;
    std::vector<VkImageView> mGpRetired;  // views of images GL has made again, destroyed as a pass begins
    struct GpSamplerKey
    {
        U32 min = 0, mag = 0, s = 0, t = 0, r = 0, compare = 0;
        F32 aniso = 1.f;
        bool operator==(const GpSamplerKey&) const = default;
    };
    std::vector<std::pair<GpSamplerKey, VkSampler>> mGpSamplers;

    // The pass being recorded.
    bool                 mGpInPass = false;
    bool                 mGpRecorded = false;   // a draw is in the command buffer
    bool                 mGpUsedArena = false;  // a draw read a mesh from the G-buffer's arena
    bool                 mGpArenaAcquired = false;
    S32                  mGpOpenTarget = -1;    // the target whose render pass is open
    std::vector<GLuint>  mGpNames;              // every GL texture the pass touches, once each
    struct GpAcquired
    {
        VkImage            image;
        VkImageAspectFlags aspect;
        bool               attachment;
    };
    std::vector<GpAcquired> mGpAcquired;
    std::set<VkImage>    mGpWritten;   // written since the last barrier
    std::set<VkImage>    mGpRead;      // sampled since the last barrier
    U32                  mGpDrawsNow = 0, mGpDrawsLast = 0, mGpPassesNow = 0, mGpPassesLast = 0;
    U32                  mGpKeptNow = 0, mGpKeptLast = 0;

    bool gpFail(const std::string& why)
    {
        mGpNote = "could not be made: " + why;
        LL_WARNS("Vulkan") << "GL programs in Vulkan " << mGpNote << LL_ENDL;
        return false;
    }

    void gpWhy(const std::string& why)
    {
        mGpWhy = why;
        ++mGpKeptNow;
        // Each reason once in the log (the status line runs off a narrow screen): the first 64.
        if (mGpWhysLogged.size() < 64 && mGpWhysLogged.insert(why).second)
        {
            LL_INFOS("QuasarVulkan") << "GL keeps a draw: " << why << LL_ENDL;
        }
    }
    std::set<std::string> mGpWhysLogged;

    bool createGpObjects()
    {
        if (mGpTried)
        {
            return mGpReady;
        }
        mGpTried = true;
        if (!mActive || !mDevice || !mExtPassed)
        {
            mGpNote = "needs QuasarVulkanTargets, its self-test passed";
            return false;
        }
        if (!mGbWhite.image && !createGbDefaults())
        {
            return gpFail("its default textures");
        }
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(mPhysical, &props);
        mGpAlign = llmax((VkDeviceSize)props.limits.minUniformBufferOffsetAlignment, (VkDeviceSize)16);
        VkBufferCreateInfo host_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        host_info.size        = GP_SLOT_BYTES * GP_SLOTS;
        host_info.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        host_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &host_info, nullptr, &mGpHost) != VK_SUCCESS)
        {
            mGpHost = VK_NULL_HANDLE;
            return gpFail("vkCreateBuffer");
        }
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(mDevice, mGpHost, &req);
        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == U32_MAX || vkAllocateMemory(mDevice, &alloc, nullptr, &mGpHostMemory) != VK_SUCCESS)
        {
            mGpHostMemory = VK_NULL_HANDLE;
            return gpFail("its host memory");
        }
        vkBindBufferMemory(mDevice, mGpHost, mGpHostMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(mDevice, mGpHostMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        {
            return gpFail("vkMapMemory");
        }
        mGpMapped = (U8*)mapped;
        const VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, GP_SETS * 6 },
                                                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GP_SETS * 16 },
                                                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
        for (GpSlot& slot : mGpSlots)
        {
            VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cmd_info.commandPool        = mCommandPool;
            cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmd_info.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(mDevice, &cmd_info, &slot.cmd) != VK_SUCCESS)
            {
                slot.cmd = VK_NULL_HANDLE;
                return gpFail("vkAllocateCommandBuffers");
            }
            VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(mDevice, &fence, nullptr, &slot.fence) != VK_SUCCESS)
            {
                slot.fence = VK_NULL_HANDLE;
                return gpFail("vkCreateFence");
            }
            VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pool_info.maxSets       = GP_SETS;
            pool_info.poolSizeCount = 3;
            pool_info.pPoolSizes    = sizes;
            if (vkCreateDescriptorPool(mDevice, &pool_info, nullptr, &slot.pool) != VK_SUCCESS)
            {
                slot.pool = VK_NULL_HANDLE;
                return gpFail("vkCreateDescriptorPool");
            }
        }
        if (!createSharedSemaphore(mGpGLDoneVK, mGpGLDoneGL, mGpGLDoneHandle) ||
            !createSharedSemaphore(mGpVKDoneVK, mGpVKDoneGL, mGpVKDoneHandle))
        {
            return gpFail("its semaphores could not be shared");
        }
        ql_glsl::initialize();
        mGpReady = true;
        mGpNote  = "ready";
        LL_INFOS("Vulkan") << "GL programs in Vulkan: ready" << LL_ENDL;
        return true;
    }

    void destroyGpProgram(GpProgram& p)
    {
        for (GpPipe& pipe : p.pipes)
        {
            if (pipe.pipeline) vkDestroyPipeline(mDevice, pipe.pipeline, nullptr);
        }
        p.pipes.clear();
        for (VkShaderModule& m : p.modules)
        {
            if (m) vkDestroyShaderModule(mDevice, m, nullptr);
            m = VK_NULL_HANDLE;
        }
        if (p.layout) vkDestroyPipelineLayout(mDevice, p.layout, nullptr);
        if (p.setLayout) vkDestroyDescriptorSetLayout(mDevice, p.setLayout, nullptr);
        p.layout    = VK_NULL_HANDLE;
        p.setLayout = VK_NULL_HANDLE;
        p.ok        = false;
    }

    void destroyGpTarget(GpTarget& t)
    {
        if (t.framebuffer) vkDestroyFramebuffer(mDevice, t.framebuffer, nullptr);
        if (t.load) vkDestroyRenderPass(mDevice, t.load, nullptr);
        if (t.clear) vkDestroyRenderPass(mDevice, t.clear, nullptr);
        for (VkImageView& v : t.views)
        {
            if (v) vkDestroyImageView(mDevice, v, nullptr);
            v = VK_NULL_HANDLE;
        }
        t = GpTarget();
    }

    // Every slot's work done: before a program, target or view still named by a recorded pass goes.
    void gpWaitSlots()
    {
        for (GpSlot& slot : mGpSlots)
        {
            if (slot.submitted)
            {
                vkWaitForFences(mDevice, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                slot.submitted = false;
            }
        }
    }

    void destroyGpObjects()
    {
        if (!mDevice)
        {
            return;
        }
        if (mGpTried)
        {
            vkDeviceWaitIdle(mDevice);
        }
        mGpInPass = false;
        for (GpProgram& p : mGpPrograms)
        {
            destroyGpProgram(p);
        }
        mGpPrograms.clear();
        for (GpProgram& p : mGpRetiredPrograms)
        {
            destroyGpProgram(p);
        }
        mGpRetiredPrograms.clear();
        for (GpTarget& t : mGpTargets)
        {
            destroyGpTarget(t);
        }
        mGpTargets.clear();
        for (GpView& v : mGpViews)
        {
            if (v.view) vkDestroyImageView(mDevice, v.view, nullptr);
        }
        mGpViews.clear();
        for (VkImageView view : mGpRetired)
        {
            if (view) vkDestroyImageView(mDevice, view, nullptr);
        }
        mGpRetired.clear();
        for (auto& s : mGpSamplers)
        {
            vkDestroySampler(mDevice, s.second, nullptr);
        }
        mGpSamplers.clear();
        for (GpSlot& slot : mGpSlots)
        {
            if (slot.pool) vkDestroyDescriptorPool(mDevice, slot.pool, nullptr);
            if (slot.fence) vkDestroyFence(mDevice, slot.fence, nullptr);
            if (slot.cmd) vkFreeCommandBuffers(mDevice, mCommandPool, 1, &slot.cmd);
            slot = GpSlot();
        }
        if (mGpMapped)
        {
            vkUnmapMemory(mDevice, mGpHostMemory);
            mGpMapped = nullptr;
        }
        if (mGpHost) vkDestroyBuffer(mDevice, mGpHost, nullptr);
        if (mGpHostMemory) vkFreeMemory(mDevice, mGpHostMemory, nullptr);
        mGpHost       = VK_NULL_HANDLE;
        mGpHostMemory = VK_NULL_HANDLE;
        if (mGpGLDoneGL) gl.DeleteSemaphoresEXT(1, &mGpGLDoneGL);
        if (mGpVKDoneGL) gl.DeleteSemaphoresEXT(1, &mGpVKDoneGL);
        if (mGpGLDoneVK) vkDestroySemaphore(mDevice, mGpGLDoneVK, nullptr);
        if (mGpVKDoneVK) vkDestroySemaphore(mDevice, mGpVKDoneVK, nullptr);
        mGpGLDoneGL = mGpVKDoneGL = 0;
        mGpGLDoneVK = mGpVKDoneVK = VK_NULL_HANDLE;
        for (HANDLE* h : { &mGpGLDoneHandle, &mGpVKDoneHandle })
        {
            if (*h)
            {
                CloseHandle(*h);
                *h = nullptr;
            }
        }
        if (mGpTried)
        {
            ql_glsl::finalize();
        }
        mGpReady = false;
        mGpTried = false;
    }

    static ql_glsl::Stage gpStage(unsigned gl_type, bool& known)
    {
        known = true;
        switch (gl_type)
        {
            case GL_VERTEX_SHADER: return ql_glsl::STAGE_VERTEX;
            case GL_TESS_CONTROL_SHADER: return ql_glsl::STAGE_TESS_CONTROL;
            case GL_TESS_EVALUATION_SHADER: return ql_glsl::STAGE_TESS_EVALUATION;
            case GL_FRAGMENT_SHADER: return ql_glsl::STAGE_FRAGMENT;
            case GL_COMPUTE_SHADER: return ql_glsl::STAGE_COMPUTE;
            default: known = false; return ql_glsl::STAGE_VERTEX;
        }
    }

    static VkShaderStageFlags gpStages(U32 bits)
    {
        VkShaderStageFlags flags = 0;
        if (bits & (1u << ql_glsl::STAGE_VERTEX)) flags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (bits & (1u << ql_glsl::STAGE_TESS_CONTROL)) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        if (bits & (1u << ql_glsl::STAGE_TESS_EVALUATION)) flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        if (bits & (1u << ql_glsl::STAGE_FRAGMENT)) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (bits & (1u << ql_glsl::STAGE_COMPUTE)) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
        return flags;
    }

    // What made the program what it is, for the note: its first lines of complaint.
    static std::string gpFirstLines(const std::string& log, size_t lines)
    {
        std::string out;
        size_t start = 0;
        for (size_t n = 0; n < lines && start < log.size(); ++n)
        {
            size_t end = log.find('\n', start);
            if (end == std::string::npos)
            {
                end = log.size();
            }
            if (end > start)
            {
                out += (out.empty() ? "" : " / ") + log.substr(start, end - start);
            }
            start = end + 1;
        }
        return out;
    }

    // A mesh's attribute streams in the arena (arenaPlace's layout), by GL's attribute name: the stride each keeps.
    enum GpStream : U32
    {
        GP_POSITION = 0, GP_NORMAL, GP_TEXCOORD0, GP_TEXCOORD1, GP_TEXCOORD2, GP_COLOR, GP_TANGENT, GP_WEIGHT4,
        GP_TEXTURE_INDEX,  // GL keeps it as position.w, read as an int: the positions' stream, 12 bytes in
        GP_EMISSIVE,       // LLColor4U; GL feeds it to diffuse_color as well where a program reads both
        GP_STREAMS
    };
    static S32 gpStreamOf(const std::string& name)
    {
        static const char* const names[GP_STREAMS] = { "position", "normal", "texcoord0", "texcoord1",
                                                       "texcoord2", "diffuse_color", "tangent", "weight4",
                                                       "texture_index", "emissive" };
        for (U32 i = 0; i < GP_STREAMS; ++i)
        {
            if (name == names[i])
            {
                return (S32)i;
            }
        }
        return -1;
    }
    static U32 gpStreamStride(U32 stream)
    {
        switch (stream)
        {
            case GP_TEXCOORD0: case GP_TEXCOORD1: case GP_TEXCOORD2: return 8;
            case GP_COLOR: case GP_EMISSIVE: return 4;
            default: return 16;
        }
    }
    VkDeviceSize gpStreamStart(U32 stream) const
    {
        switch (stream)
        {
            case GP_POSITION: return 0;
            case GP_TEXTURE_INDEX: return 12;
            case GP_NORMAL: return mGbArena.normalStart();
            case GP_TEXCOORD0: return mGbArena.uvStart();
            case GP_TEXCOORD1: return mGbArena.uv1Start();
            case GP_TEXCOORD2: return mGbArena.uv2Start();
            case GP_COLOR: return mGbArena.colorStart();
            case GP_EMISSIVE: return mGbArena.emissiveStart();
            case GP_TANGENT: return mGbArena.tangentStart();
            case GP_WEIGHT4: return mGbArena.vertexBytes;
            default: return 0;
        }
    }

    int gpProgram(U64 key, U32 link, const char* name, const QLVulkanPresenter::GpSource* sources, U32 count)
    {
        if (!createGpObjects())
        {
            return -1;
        }
        S32 index = -1;
        for (size_t i = 0; i < mGpPrograms.size(); ++i)
        {
            if (mGpPrograms[i].key == key)
            {
                index = (S32)i;
                break;
            }
        }
        if (index >= 0 && mGpPrograms[index].link == link)
        {
            return mGpPrograms[index].ok ? index : -1;
        }

        if (index < 0)
        {
            index = (S32)mGpPrograms.size();
            mGpPrograms.emplace_back();
        }
        else if (mGpInPass)
        {
            // The pass being recorded may name the old pipelines: they go as the next pass begins, every slot done.
            mGpRetiredPrograms.push_back(std::move(mGpPrograms[index]));
        }
        else
        {
            gpWaitSlots();  // a pass may still name the old pipelines
            destroyGpProgram(mGpPrograms[index]);
        }
        GpProgram& p = mGpPrograms[index];
        p      = GpProgram();
        p.key  = key;
        p.link = link;
        p.name = name ? name : "";
        LL_PROFILE_ZONE_NAMED("vulkan gl program compile");
        std::vector<ql_glsl::Source> src;
        for (U32 i = 0; i < count; ++i)
        {
            bool known = false;
            const ql_glsl::Stage stage = gpStage(sources[i].type, known);
            if (!known || !sources[i].text)
            {
                p.note = "a stage Vulkan is not given";
                return -1;
            }
            src.push_back({ stage, sources[i].name ? sources[i].name : "", std::string(sources[i].text, sources[i].length) });
        }
        if (src.empty())
        {
            p.note = "GL kept none of its sources (LLShaderMgr::sQLKeepSources was off as it linked)";
            return -1;
        }
        const auto started = std::chrono::steady_clock::now();
        if (!ql_glsl::compile(src, p.refl))
        {
            p.note = "does not compile for Vulkan: " + gpFirstLines(p.refl.log, 3);
            LL_WARNS("Vulkan") << "GL program " << p.name << " " << p.note << "\n" << p.refl.log << LL_ENDL;
            return -1;
        }
        if (p.refl.stage_mask & ((1u << ql_glsl::STAGE_TESS_CONTROL) | (1u << ql_glsl::STAGE_TESS_EVALUATION)))
        {
            p.note = "is tessellated (not yet drawn by Vulkan)";
            return -1;
        }
        for (U32 st = 0; st < ql_glsl::STAGE_COUNT; ++st)
        {
            if (p.refl.spirv[st].empty())
            {
                continue;
            }
            VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            module.codeSize = p.refl.spirv[st].size() * sizeof(uint32_t);
            module.pCode    = p.refl.spirv[st].data();
            if (vkCreateShaderModule(mDevice, &module, nullptr, &p.modules[st]) != VK_SUCCESS)
            {
                p.modules[st] = VK_NULL_HANDLE;
                p.note        = "vkCreateShaderModule failed";
                destroyGpProgram(p);
                return -1;
            }
        }
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const ql_glsl::Resource& r : p.refl.resources)
        {
            if (r.set != 0)
            {
                p.note = "a resource outside set 0";
                destroyGpProgram(p);
                return -1;
            }
            VkDescriptorSetLayoutBinding b = {};
            b.binding         = r.binding;
            b.descriptorCount = r.count;
            b.stageFlags      = gpStages(r.stages ? r.stages : p.refl.stage_mask);
            switch (r.kind)
            {
                case ql_glsl::RES_UNIFORM_BLOCK: b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b.descriptorCount = 1; break;
                case ql_glsl::RES_SAMPLER: b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
                case ql_glsl::RES_STORAGE_IMAGE: b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
                default:
                    p.note = "reads " + r.name + ", a kind of resource Vulkan is not given yet";
                    destroyGpProgram(p);
                    return -1;
            }
            bindings.push_back(b);
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = (U32)bindings.size();
        set_info.pBindings    = bindings.data();
        if (vkCreateDescriptorSetLayout(mDevice, &set_info, nullptr, &p.setLayout) != VK_SUCCESS)
        {
            p.setLayout = VK_NULL_HANDLE;
            p.note      = "vkCreateDescriptorSetLayout failed";
            destroyGpProgram(p);
            return -1;
        }
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts    = &p.setLayout;
        if (vkCreatePipelineLayout(mDevice, &layout_info, nullptr, &p.layout) != VK_SUCCESS)
        {
            p.layout = VK_NULL_HANDLE;
            p.note   = "vkCreatePipelineLayout failed";
            destroyGpProgram(p);
            return -1;
        }
        for (size_t m = 0; m < p.refl.members.size(); ++m)
        {
            p.members[p.refl.members[m].name] = (U32)m;
        }
        for (const ql_glsl::Attribute& a : p.refl.inputs)
        {
            const S32 stream = gpStreamOf(a.name);
            if (stream < 0)
            {
                p.note = "reads the attribute " + a.name + ", which the arena does not keep";
                destroyGpProgram(p);
                return -1;
            }
            if (stream == GP_POSITION)
            {
                p.position = (S32)a.location;
            }
            if (stream == GP_EMISSIVE)
            {
                p.reads_emissive = true;
            }
            GpProgram::Input in;
            in.stream   = (U32)stream;
            in.location = a.location;
            switch (a.gl_type)
            {
                case GL_FLOAT: in.format = VK_FORMAT_R32_SFLOAT; break;
                case GL_INT: in.format = VK_FORMAT_R32_SINT; break;
                case GL_UNSIGNED_INT: in.format = VK_FORMAT_R32_UINT; break;
                case GL_INT_VEC2: in.format = VK_FORMAT_R32G32_SINT; break;
                case GL_INT_VEC4: in.format = VK_FORMAT_R32G32B32A32_SINT; break;
                case GL_UNSIGNED_INT_VEC4: in.format = VK_FORMAT_R32G32B32A32_UINT; break;
                case GL_FLOAT_VEC2: in.format = VK_FORMAT_R32G32_SFLOAT; break;
                case GL_FLOAT_VEC3: in.format = VK_FORMAT_R32G32B32_SFLOAT; break;
                default: in.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            }
            if (stream == GP_COLOR || stream == GP_EMISSIVE)
            {
                in.format = VK_FORMAT_R8G8B8A8_UNORM;  // LLColor4U, normalized as GL's attribute
            }
            p.inputs.push_back(in);
        }
        for (const ql_glsl::Attribute& a : p.refl.outputs)
        {
            p.outputs = llmax(p.outputs, a.location + llmax(a.count, 1u));
        }
        p.ok = true;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        p.note = llformat("compiled in %.0f ms: %u loose uniforms in %u bytes, %u resources", ms,
                          (U32)p.refl.members.size(), p.refl.default_size, (U32)p.refl.resources.size());
        LL_INFOS("Vulkan") << "GL program " << p.name << " in Vulkan: " << p.note << LL_ENDL;
        return index;
    }

    std::string gpProgramNote(U64 key) const
    {
        for (const GpProgram& p : mGpPrograms)
        {
            if (p.key == key)
            {
                return p.note;
            }
        }
        return "not asked for";
    }

    // A render pass for a target's formats: its attachments loaded, or its colours cleared (the depth loaded either way).
    VkRenderPass gpRenderPass(const GpTarget& t, bool clear)
    {
        VkAttachmentDescription attachments[5] = {};
        VkAttachmentReference   color_refs[4] = {};
        const U32 count = t.colors + (t.depth ? 1 : 0);
        for (U32 i = 0; i < count; ++i)
        {
            const bool is_depth = t.depth && i == t.colors;
            const bool stencil  = t.formats[i] == VK_FORMAT_D24_UNORM_S8_UINT || t.formats[i] == VK_FORMAT_D32_SFLOAT_S8_UINT;
            VkAttachmentDescription& a = attachments[i];
            a.format         = t.formats[i];
            a.samples        = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp         = (clear && !is_depth) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp  = stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
            a.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;
            if (!is_depth)
            {
                color_refs[i] = { i, VK_IMAGE_LAYOUT_GENERAL };
            }
        }
        const VkAttachmentReference depth_ref = { t.colors, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = t.colors;
        subpass.pColorAttachments       = color_refs;
        subpass.pDepthStencilAttachment = t.depth ? &depth_ref : nullptr;
        // What the pass's commands before this render pass wrote (an earlier render pass on these attachments, or one
        // whose output a draw here samples) done and visible before it loads, tests and writes them.
        VkSubpassDependency dependency = {};
        dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass    = 0;
        dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        pass_info.attachmentCount = count;
        pass_info.pAttachments    = attachments;
        pass_info.subpassCount    = 1;
        pass_info.pSubpasses      = &subpass;
        pass_info.dependencyCount = 1;
        pass_info.pDependencies   = &dependency;
        VkRenderPass pass = VK_NULL_HANDLE;
        if (vkCreateRenderPass(mDevice, &pass_info, nullptr, &pass) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        return pass;
    }

    S32 gpTarget(const QLVulkanPresenter::GpDraw& d)
    {
        const U32 count = d.color_count + (d.depth ? 1 : 0);
        if (d.color_count > 4 || count == 0 || !d.width || !d.height)
        {
            gpWhy("a target of no attachments or more than four colours");
            return -1;
        }
        GLuint names[5] = {};
        VkImage images[5] = {};
        VkFormat formats[5] = {};
        for (U32 i = 0; i < count; ++i)
        {
            names[i] = i < d.color_count ? d.colors[i] : d.depth;
            std::lock_guard<std::mutex> lock(mExtMutex);
            const auto found = mExtTextures.find(names[i]);
            if (found == mExtTextures.end() || found->second.block >= 0)
            {
                gpWhy(llformat("its %s (GL name %u) is not in Vulkan's memory",
                               i < d.color_count ? "colour attachment" : "depth", names[i]));
                return -1;
            }
            images[i]  = found->second.image;
            formats[i] = found->second.vk;
        }
        for (size_t t = 0; t < mGpTargets.size(); ++t)
        {
            const GpTarget& g = mGpTargets[t];
            bool same = g.colors == d.color_count && g.depth == (d.depth != 0) && g.width == d.width && g.height == d.height;
            for (U32 i = 0; i < count && same; ++i)
            {
                same = g.names[i] == names[i] && g.images[i] == images[i] && g.formats[i] == formats[i];
            }
            if (same)
            {
                return (S32)t;
            }
        }
        // Past 64 targets the oldest goes, before anything of the pass is recorded (a recorded draw names its target,
        // and the open render pass its framebuffer).
        if (mGpTargets.size() >= 64 && !mGpRecorded)
        {
            gpEndRenderPass();
            gpWaitSlots();
            destroyGpTarget(mGpTargets.front());
            mGpTargets.erase(mGpTargets.begin());
        }
        GpTarget t;
        t.colors = d.color_count;
        t.depth  = d.depth != 0;
        t.width  = d.width;
        t.height = d.height;
        for (U32 i = 0; i < count; ++i)
        {
            const bool is_depth = t.depth && i == t.colors;
            const bool stencil  = formats[i] == VK_FORMAT_D24_UNORM_S8_UINT || formats[i] == VK_FORMAT_D32_SFLOAT_S8_UINT;
            t.names[i]   = names[i];
            t.images[i]  = images[i];
            t.formats[i] = formats[i];
            t.formatKey  = t.formatKey * 131 + (U64)formats[i] + 1;
            VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view_info.image            = images[i];
            view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format           = formats[i];
            view_info.subresourceRange = { is_depth ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                                           (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                                    : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                                           0, 1, 0, 1 };
            if (vkCreateImageView(mDevice, &view_info, nullptr, &t.views[i]) != VK_SUCCESS)
            {
                t.views[i] = VK_NULL_HANDLE;
                destroyGpTarget(t);
                gpWhy("vkCreateImageView failed for an attachment");
                return -1;
            }
        }
        t.formatKey = t.formatKey * 7 + (t.depth ? 1 : 0);
        t.load  = gpRenderPass(t, false);
        t.clear = gpRenderPass(t, true);
        if (!t.load || !t.clear)
        {
            destroyGpTarget(t);
            gpWhy("vkCreateRenderPass failed");
            return -1;
        }
        VkFramebufferCreateInfo fb_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb_info.renderPass      = t.load;
        fb_info.attachmentCount = count;
        fb_info.pAttachments    = t.views;
        fb_info.width           = t.width;
        fb_info.height          = t.height;
        fb_info.layers          = 1;
        if (vkCreateFramebuffer(mDevice, &fb_info, nullptr, &t.framebuffer) != VK_SUCCESS)
        {
            t.framebuffer = VK_NULL_HANDLE;
            destroyGpTarget(t);
            gpWhy("vkCreateFramebuffer failed");
            return -1;
        }
        mGpTargets.push_back(t);
        return (S32)mGpTargets.size() - 1;
    }

    static VkCompareOp gpCompare(unsigned func)
    {
        switch (func)
        {
            case GL_NEVER: return VK_COMPARE_OP_NEVER;
            case GL_LESS: return VK_COMPARE_OP_LESS;
            case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
            case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case GL_GREATER: return VK_COMPARE_OP_GREATER;
            case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
            case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    VkPipeline gpPipeline(GpProgram& p, const GpTarget& t, const QLVulkanPresenter::GpDraw& d)
    {
        const bool bounds = d.bounds_max > d.bounds_min && mHasDepthBounds && t.depth;
        const bool mesh = d.mesh.stamp != 0;
        U64 key = t.formatKey * 1000003ull + ((U64)d.blend) + ((U64)d.depth_func << 4) +
                  ((U64)(d.depth_write ? 1 : 0) << 20) + ((U64)(bounds ? 1 : 0) << 21) +
                  ((U64)(d.cull_back ? 1 : 0) << 22) + ((U64)d.topology << 24) + ((U64)t.colors << 40) +
                  ((U64)(mesh ? 1 : 0) << 44) + ((U64)(d.cull_front ? 1 : 0) << 45) + ((U64)(d.color_mask & 0xF) << 46);
        if (d.blend == 3)
        {
            key = key * 31 + d.blend_src;
            key = key * 31 + d.blend_dst;
            key = key * 31 + d.blend_src_alpha;
            key = key * 31 + d.blend_dst_alpha;
            key = key * 31 + d.blend_eq;
            key = key * 31 + d.blend_eq_alpha;
        }
        for (const GpPipe& pipe : p.pipes)
        {
            if (pipe.key == key)
            {
                return pipe.pipeline;
            }
        }
        VkPipelineShaderStageCreateInfo stages[2] = {};
        U32 stage_count = 0;
        for (U32 st : { (U32)ql_glsl::STAGE_VERTEX, (U32)ql_glsl::STAGE_FRAGMENT })
        {
            if (!p.modules[st])
            {
                continue;
            }
            VkPipelineShaderStageCreateInfo& s = stages[stage_count++];
            s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.stage  = st == ql_glsl::STAGE_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            s.module = p.modules[st];
            s.pName  = "main";
        }
        VkVertexInputBindingDescription binding = { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attribute = { (U32)llmax(p.position, 0), 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        // A mesh's: a binding a stream the program reads, its stride the arena's.
        std::vector<VkVertexInputBindingDescription>   mesh_bindings;
        std::vector<VkVertexInputAttributeDescription> mesh_attributes;
        if (mesh)
        {
            for (size_t i = 0; i < p.inputs.size(); ++i)
            {
                mesh_bindings.push_back({ (U32)i, gpStreamStride(p.inputs[i].stream), VK_VERTEX_INPUT_RATE_VERTEX });
                mesh_attributes.push_back({ p.inputs[i].location, (U32)i, p.inputs[i].format, 0 });
            }
            vertex_input.vertexBindingDescriptionCount   = (U32)mesh_bindings.size();
            vertex_input.pVertexBindingDescriptions      = mesh_bindings.data();
            vertex_input.vertexAttributeDescriptionCount = (U32)mesh_attributes.size();
            vertex_input.pVertexAttributeDescriptions    = mesh_attributes.data();
        }
        else if (p.position >= 0)
        {
            vertex_input.vertexBindingDescriptionCount   = 1;
            vertex_input.pVertexBindingDescriptions      = &binding;
            vertex_input.vertexAttributeDescriptionCount = 1;
            vertex_input.pVertexAttributeDescriptions    = &attribute;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = d.topology == GL_TRIANGLE_STRIP ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                            : d.topology == GL_TRIANGLE_FAN ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                                                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;
        // GL's counter-clockwise front, turned over by Vulkan's framebuffer rows (as the G-buffer's pipelines).
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = (d.cull_back ? VK_CULL_MODE_BACK_BIT : 0) | (d.cull_front ? VK_CULL_MODE_FRONT_BIT : 0);
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable       = d.depth_func != 0 && t.depth ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable      = d.depth_write && t.depth ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp        = gpCompare(d.depth_func);
        depth.depthBoundsTestEnable = bounds ? VK_TRUE : VK_FALSE;
        depth.minDepthBounds        = 0.f;
        depth.maxDepthBounds        = 1.f;
        VkPipelineColorBlendAttachmentState blends[4] = {};
        for (U32 i = 0; i < t.colors; ++i)
        {
            VkPipelineColorBlendAttachmentState& b = blends[i];
            // An attachment the program writes nothing to keeps what it holds (Vulkan would leave it undefined).
            b.colorWriteMask = i < p.outputs ? (VkColorComponentFlags)(d.color_mask & 0xF) : 0;
            if (d.blend == 3)
            {
                b.blendEnable         = VK_TRUE;
                auto op = [](unsigned eq)
                {
                    switch (eq)
                    {
                        case GL_MIN: return VK_BLEND_OP_MIN;
                        case GL_MAX: return VK_BLEND_OP_MAX;
                        case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
                        case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
                        default: return VK_BLEND_OP_ADD;
                    }
                };
                b.srcColorBlendFactor = gpBlendFactor(d.blend_src);
                b.dstColorBlendFactor = gpBlendFactor(d.blend_dst);
                b.colorBlendOp        = op(d.blend_eq);
                b.srcAlphaBlendFactor = gpBlendFactor(d.blend_src_alpha);
                b.dstAlphaBlendFactor = gpBlendFactor(d.blend_dst_alpha);
                b.alphaBlendOp        = op(d.blend_eq_alpha);
            }
            else if (d.blend != 0)
            {
                b.blendEnable         = VK_TRUE;
                b.srcColorBlendFactor = d.blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
                b.dstColorBlendFactor = d.blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                b.colorBlendOp        = VK_BLEND_OP_ADD;
                b.srcAlphaBlendFactor = d.blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
                b.dstAlphaBlendFactor = d.blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                b.alphaBlendOp        = VK_BLEND_OP_ADD;
            }
        }
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = t.colors;
        blend.pAttachments    = blends;
        VkDynamicState dynamic_states[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_BOUNDS };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = bounds ? 3 : 2;
        dynamic.pDynamicStates    = dynamic_states;
        VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.stageCount          = stage_count;
        info.pStages             = stages;
        info.pVertexInputState   = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport_state;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depth;
        info.pColorBlendState    = &blend;
        info.pDynamicState       = &dynamic;
        info.layout              = p.layout;
        info.renderPass          = t.load;
        VkPipeline pipeline = VK_NULL_HANDLE;
        LL_PROFILE_ZONE_NAMED("vulkan gl program pipeline");
        if (vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        p.pipes.push_back({ key, pipeline });
        return pipeline;
    }

    static VkBlendFactor gpBlendFactor(unsigned gl_factor)
    {
        switch (gl_factor)
        {
            case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
            case GL_ONE: return VK_BLEND_FACTOR_ONE;
            case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
            case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
            case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
            case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
            case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
        }
    }

    VkSampler gpSampler(const QLVulkanPresenter::GpTexture& tex)
    {
        GpSamplerKey key;
        key.min     = tex.min_filter;
        key.mag     = tex.mag_filter;
        key.s       = tex.wrap_s;
        key.t       = tex.wrap_t;
        key.r       = tex.wrap_r;
        key.compare = tex.compare_func;
        key.aniso   = mHasAniso ? llmax(tex.anisotropy, 1.f) : 1.f;
        for (const auto& s : mGpSamplers)
        {
            if (s.first == key)
            {
                return s.second;
            }
        }
        auto address = [](U32 wrap)
        {
            switch (wrap)
            {
                case GL_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
        };
        const bool min_linear = key.min == GL_LINEAR || key.min == GL_LINEAR_MIPMAP_NEAREST ||
                                key.min == GL_LINEAR_MIPMAP_LINEAR;
        const bool mip_linear = key.min == GL_LINEAR_MIPMAP_LINEAR || key.min == GL_NEAREST_MIPMAP_LINEAR;
        const bool mipmapped  = key.min != GL_LINEAR && key.min != GL_NEAREST;
        VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter     = key.mag == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter     = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.mipmapMode    = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU  = address(key.s);
        info.addressModeV  = address(key.t);
        info.addressModeW  = address(key.r);
        info.anisotropyEnable = key.aniso > 1.f ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy    = key.aniso;
        info.compareEnable    = key.compare ? VK_TRUE : VK_FALSE;
        info.compareOp        = gpCompare(key.compare);
        info.minLod           = 0.f;
        // GL_LINEAR / GL_NEAREST minification reads level 0 alone.
        info.maxLod           = mipmapped ? VK_LOD_CLAMP_NONE : 0.f;
        info.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(mDevice, &info, nullptr, &sampler) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        mGpSamplers.emplace_back(key, sampler);
        return sampler;
    }

    // A sampled view of a GL texture in Vulkan's memory (a depth texture's depth alone); null where it is not there.
    // linear: an sRGB texture read without decoding, through its linear twin's view (null where it was not made so).
    VkImageView gpView(GLuint name, VkImage& image, VkFormat& format, bool& is_depth, bool* is_cube = nullptr,
                       bool linear = false)
    {
        U32 levels = 1, layers = 1;
        bool cube = false;
        bool mutable_format = false;
        {
            std::lock_guard<std::mutex> lock(mExtMutex);
            const auto found = mExtTextures.find(name);
            if (found == mExtTextures.end())
            {
                return VK_NULL_HANDLE;
            }
            image  = found->second.image;
            format = found->second.vk;
            levels = llmax(1u, found->second.levels);
            layers = llmax(1u, found->second.layers);
            cube   = found->second.cube;
            mutable_format = found->second.mutable_format;
        }
        if (linear)
        {
            const VkFormat twin = gpLinearTwin(format);
            if (twin == VK_FORMAT_UNDEFINED || !mutable_format)
            {
                return VK_NULL_HANDLE;
            }
            format = twin;
        }
        if (is_cube)
        {
            *is_cube = cube;
        }
        const VkImageViewType view_type = cube ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        is_depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                   format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        for (GpView& v : mGpViews)
        {
            if (v.name == name && v.aspect == aspect && v.format == format)
            {
                if (v.image == image)
                {
                    return v.view;
                }
                // The name has another image now (GL made it again): the old view goes once nothing reads it (the
                // pass being recorded may: it goes as the next pass begins, every slot done).
                mGpRetired.push_back(v.view);
                v.view  = VK_NULL_HANDLE;
                v.image = image;
                VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                view_info.image            = image;
                view_info.viewType         = view_type;
                view_info.format           = format;
                view_info.subresourceRange = { aspect, 0, levels, 0, layers };
                if (vkCreateImageView(mDevice, &view_info, nullptr, &v.view) != VK_SUCCESS)
                {
                    v.view = VK_NULL_HANDLE;
                }
                return v.view;
            }
        }
        GpView v;
        v.name   = name;
        v.image  = image;
        v.aspect = aspect;
        v.format = format;
        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image            = image;
        view_info.viewType         = view_type;
        view_info.format           = format;
        view_info.subresourceRange = { aspect, 0, levels, 0, layers };
        if (vkCreateImageView(mDevice, &view_info, nullptr, &v.view) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        mGpViews.push_back(v);
        return v.view;
    }

    // Room in the slot's share of the host buffer; GP_NONE when the pass has used it all.
    VkDeviceSize gpAllocate(VkDeviceSize bytes)
    {
        const VkDeviceSize at = (mGpTop + mGpAlign - 1) / mGpAlign * mGpAlign;
        if (at + bytes > GP_SLOT_BYTES)
        {
            return GP_NONE;
        }
        mGpTop = at + bytes;
        mGpHostPeak = llmax(mGpHostPeak, mGpTop);
        return GP_SLOT_BYTES * mGpSlot + at;
    }

    // The default block's bytes, from what GL holds for the program's loose uniforms (std140, as glslang laid it out).
    void gpFillDefaults(const GpProgram& p, const QLVulkanPresenter::GpDraw& d, U8* out)
    {
        memset(out, 0, p.refl.default_size);
        for (U32 r = 0; r < d.region_count; ++r)
        {
            const QLVulkanPresenter::GpRegion& region = d.regions[r];
            if (!region.name)
            {
                continue;
            }
            const auto found = p.members.find(region.name);
            if (found == p.members.end())
            {
                continue;  // not read by the program (GL keeps what Vulkan's compiler dropped, and samplers' units)
            }
            const ql_glsl::Member& m = p.refl.members[found->second];
            const U32* words = d.words + region.words;
            const U32 elements = llmin(region.elements, m.count);
            const U32 stride = m.array_stride ? m.array_stride : 16;
            const bool is_bool = m.gl_type == GL_BOOL || m.gl_type == GL_BOOL_VEC2 || m.gl_type == GL_BOOL_VEC3 ||
                                 m.gl_type == GL_BOOL_VEC4;
            for (U32 e = 0; e < elements; ++e)
            {
                U8* dst = out + m.offset + (size_t)e * stride;
                const U32* src = words + (size_t)e * region.components;
                if (dst + 4 > out + p.refl.default_size)
                {
                    break;
                }
                if (m.matrix_stride)
                {
                    // Columns (GL's, or its rows where it was handed over transposed) of `rows` floats each.
                    U32 cols = 4, rows = 4;
                    switch (m.gl_type)
                    {
                        case GL_FLOAT_MAT2: cols = 2; rows = 2; break;
                        case GL_FLOAT_MAT3: cols = 3; rows = 3; break;
                        case GL_FLOAT_MAT2x3: cols = 2; rows = 3; break;
                        case GL_FLOAT_MAT2x4: cols = 2; rows = 4; break;
                        case GL_FLOAT_MAT3x2: cols = 3; rows = 2; break;
                        case GL_FLOAT_MAT3x4: cols = 3; rows = 4; break;
                        case GL_FLOAT_MAT4x2: cols = 4; rows = 2; break;
                        case GL_FLOAT_MAT4x3: cols = 4; rows = 3; break;
                        default: break;
                    }
                    for (U32 c = 0; c < cols; ++c)
                    {
                        for (U32 row = 0; row < rows; ++row)
                        {
                            const U32 at = region.transpose ? row * cols + c : c * rows + row;
                            U8* cell = dst + c * m.matrix_stride + row * 4;
                            if (at < region.components && cell + 4 <= out + p.refl.default_size)
                            {
                                memcpy(cell, src + at, 4);
                            }
                        }
                    }
                    continue;
                }
                const U32 comps = llmin(region.components, 4u);
                for (U32 c = 0; c < comps; ++c)
                {
                    U32 value = src[c];
                    if (is_bool)
                    {
                        F32 f;
                        memcpy(&f, &value, 4);
                        value = region.kind == 0 ? (f != 0.f ? 1u : 0u) : (value != 0 ? 1u : 0u);
                    }
                    if (dst + c * 4 + 4 <= out + p.refl.default_size)
                    {
                        memcpy(dst + c * 4, &value, 4);
                    }
                }
            }
        }
    }

    // An image GL has let go of (extFree): the targets and views made of it go (another image may come to have its
    // handle). During a pass, which may name them, it goes as the next begins.
    std::vector<VkImage> mGpForgetLater;
    void gpForget(VkImage image)
    {
        if (!image || !mDevice)
        {
            return;
        }
        if (mGpInPass)
        {
            mGpForgetLater.push_back(image);
            return;
        }
        bool named = false;
        for (const GpTarget& t : mGpTargets)
        {
            for (VkImage i : t.images)
            {
                named = named || i == image;
            }
        }
        for (const GpView& v : mGpViews)
        {
            named = named || v.image == image;
        }
        if (!named)
        {
            return;
        }
        gpWaitSlots();
        for (size_t t = 0; t < mGpTargets.size();)
        {
            const GpTarget& g = mGpTargets[t];
            if (std::find(std::begin(g.images), std::end(g.images), image) != std::end(g.images))
            {
                destroyGpTarget(mGpTargets[t]);
                mGpTargets.erase(mGpTargets.begin() + (std::ptrdiff_t)t);
                continue;
            }
            ++t;
        }
        for (size_t v = 0; v < mGpViews.size();)
        {
            if (mGpViews[v].image == image)
            {
                if (mGpViews[v].view) vkDestroyImageView(mDevice, mGpViews[v].view, nullptr);
                mGpViews.erase(mGpViews.begin() + (std::ptrdiff_t)v);
                continue;
            }
            ++v;
        }
    }

    bool gpPassBegin()
    {
        if (!createGpObjects())
        {
            return false;
        }
        if (mGpInPass)
        {
            gpWhy("a pass began inside another");
            return false;
        }
        if (!mGpForgetLater.empty())
        {
            const std::vector<VkImage> later = std::move(mGpForgetLater);
            mGpForgetLater.clear();
            for (VkImage image : later)
            {
                gpForget(image);
            }
        }
        if (!mGpRetired.empty() || !mGpRetiredPrograms.empty())
        {
            gpWaitSlots();
            for (VkImageView view : mGpRetired)
            {
                if (view) vkDestroyImageView(mDevice, view, nullptr);
            }
            mGpRetired.clear();
            for (GpProgram& old : mGpRetiredPrograms)
            {
                destroyGpProgram(old);
            }
            mGpRetiredPrograms.clear();
        }
        GpSlot& slot = mGpSlots[mGpNextSlot];
        if (slot.submitted)
        {
            if (vkGetFenceStatus(mDevice, slot.fence) != VK_SUCCESS)
            {
                gpWhy("all its submission slots still busy");
                return false;
            }
            slot.submitted = false;
        }
        mGpSlot     = mGpNextSlot;
        mGpNextSlot = (mGpNextSlot + 1) % GP_SLOTS;
        mGpTop      = 0;
        mGpBlocksDone.clear();
        vkResetDescriptorPool(mDevice, slot.pool, 0);
        vkResetCommandBuffer(slot.cmd, 0);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(slot.cmd, &begin);
        mGpInPass     = true;
        mGpRecorded   = false;
        mGpOpenTarget = -1;
        mGpUsedArena  = false;
        mGpArenaAcquired = false;
        mGpNames.clear();
        mGpAcquired.clear();
        mGpWritten.clear();
        mGpRead.clear();
        return true;
    }

    void gpEndRenderPass()
    {
        if (mGpOpenTarget >= 0)
        {
            vkCmdEndRenderPass(mGpSlots[mGpSlot].cmd);
            mGpOpenTarget = -1;
        }
    }

    // Everything written so far visible to everything after: between a draw that writes an image and one that reads it
    // (or writes it from another target).
    void gpBarrier()
    {
        gpEndRenderPass();
        VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(mGpSlots[mGpSlot].cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
        mGpWritten.clear();
        mGpRead.clear();
    }

    // A GL name's image taken from GL's queue the first time the pass touches it (outside a render pass).
    void gpAcquire(GLuint name, VkImage image, VkImageAspectFlags aspect, bool attachment)
    {
        for (GpAcquired& a : mGpAcquired)
        {
            if (a.image == image)
            {
                a.attachment = a.attachment || attachment;  // its release makes its attachment writes available
                return;
            }
        }
        gpEndRenderPass();
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        b.dstQueueFamilyIndex = mQueueFamily;
        b.image               = image;
        b.subresourceRange    = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        vkCmdPipelineBarrier(mGpSlots[mGpSlot].cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        mGpAcquired.push_back({ image, aspect, attachment });
        if (std::find(mGpNames.begin(), mGpNames.end(), name) == mGpNames.end())
        {
            mGpNames.push_back(name);
        }
    }

    bool gpDraw(const QLVulkanPresenter::GpDraw& d)
    {
        if (!mGpInPass)
        {
            return false;
        }
        if (d.program < 0 || (size_t)d.program >= mGpPrograms.size() || !mGpPrograms[d.program].ok)
        {
            gpWhy("a program Vulkan does not have");
            return false;
        }
        GpProgram& p = mGpPrograms[d.program];
        const bool mesh = d.mesh.stamp != 0;
        if (mesh)
        {
            // Every attribute the program reads, from the mesh.
            const unsigned offsets[GP_STREAMS] = { d.mesh.position, d.mesh.normal, d.mesh.texcoord0, d.mesh.texcoord1,
                                                   d.mesh.texcoord2, d.mesh.color, d.mesh.tangent, d.mesh.weight4,
                                                   d.mesh.position, d.mesh.emissive };
            for (const GpProgram::Input& in : p.inputs)
            {
                const U32 stream = in.stream == GP_COLOR && p.reads_emissive ? (U32)GP_EMISSIVE : in.stream;
                if (offsets[stream] == ~0u)
                {
                    gpWhy(llformat("%s reads an attribute its mesh has not (stream %u)", p.name.c_str(), in.stream));
                    return false;
                }
            }
            if (!mGbReady || !mGbArena.buffer || d.mesh.position == ~0u || !d.mesh.index_count)
            {
                gpWhy(p.name + ": a mesh, and no arena to put it in (QuasarVulkanMainDraws)");
                return false;
            }
        }
        else if (p.inputs.size() > (p.position >= 0 ? 1u : 0u))
        {
            gpWhy(p.name + ": reads more than positions, and was given no mesh");
            return false;
        }
        else if (p.position >= 0 && (!d.positions || d.vertex_count == 0))
        {
            gpWhy(p.name + ": no vertices");
            return false;
        }
        if (p.outputs > d.color_count)
        {
            gpWhy(llformat("%s writes %u colours into a target of %u", p.name.c_str(), p.outputs, d.color_count));
            return false;
        }
        if (d.bounds_max > d.bounds_min && (!mHasDepthBounds || !d.depth))
        {
            gpWhy(p.name + ": held to depth bounds, which this device or target has not");
            return false;
        }
        // The default textures, made again if the G-buffer's failure took them (destroyGbObjects).
        if ((!mGbWhite.view || !mGbBlack.view || !mGbBlackCube.view) && !createGbDefaults())
        {
            gpWhy("its default textures could not be made");
            return false;
        }
        const S32 target_index = gpTarget(d);
        if (target_index < 0)
        {
            return false;
        }
        const GpTarget& t = mGpTargets[target_index];
        const VkPipeline pipeline = gpPipeline(p, t, d);
        if (!pipeline)
        {
            gpWhy(p.name + ": its pipeline failed");
            return false;
        }

        // Every resource found before anything is recorded: a draw that cannot be made leaves the pass as it was.
        struct Bound
        {
            const ql_glsl::Resource* r;
            VkDescriptorBufferInfo   buffer;
            VkDescriptorImageInfo    images[16];
            GLuint                   names[16];
            VkImage                  vk_images[16];
        };
        std::vector<Bound> bound;
        bound.reserve(p.refl.resources.size());
        VkDeviceSize vertices_at = GP_NONE;
        const VkDeviceSize mark = mGpTop;
        auto give_back = [this, mark]()
        {
            mGpTop = mark;
            const VkDeviceSize slot_mark = GP_SLOT_BYTES * mGpSlot + mark;
            for (auto it = mGpBlocksDone.begin(); it != mGpBlocksDone.end();)
            {
                it = it->second.at >= slot_mark ? mGpBlocksDone.erase(it) : std::next(it);
            }
        };
        for (const ql_glsl::Resource& r : p.refl.resources)
        {
            Bound b = {};
            b.r = &r;
            if (r.kind == ql_glsl::RES_UNIFORM_BLOCK)
            {
                const VkDeviceSize bytes = llmax((VkDeviceSize)r.size, (VkDeviceSize)16);
                // Filled here first (the host buffer is write-combined: never read back), then written only where no
                // block of this pass holds the same bytes.
                mGpBlockScratch.assign((size_t)bytes, 0);
                U8* dst = mGpBlockScratch.data();
                U64 key = 0;
                if (r.default_block)
                {
                    key = 0x8000000000000000ull | (U64)(U32)d.program;
                    gpFillDefaults(p, d, dst);
                }
                else
                {
                    key = std::hash<std::string>()(r.name) & 0x7fffffffffffffffull;
                    const QLVulkanPresenter::GpBlock* block = nullptr;
                    for (U32 i = 0; i < d.block_count; ++i)
                    {
                        if (d.blocks[i].name && r.name == d.blocks[i].name)
                        {
                            block = &d.blocks[i];
                        }
                    }
                    if (!block || !block->data)
                    {
                        give_back();
                        gpWhy(p.name + ": the block " + r.name + " is not bound");
                        return false;
                    }
                    memcpy(dst, block->data, llmin((size_t)r.size, block->bytes));
                }
                GpWritten& written = mGpBlocksDone[key];
                if (!written.bytes.empty() && written.bytes == mGpBlockScratch)
                {
                    ++mGpBlocksReused;
                }
                else
                {
                    const VkDeviceSize at = gpAllocate(bytes);
                    if (at == GP_NONE)
                    {
                        mGpBlocksDone.erase(key);
                        give_back();
                        gpWhy("the pass's host memory is full");
                        return false;
                    }
                    memcpy(mGpMapped + at, dst, (size_t)bytes);
                    written.at    = at;
                    written.bytes = mGpBlockScratch;
                    ++mGpBlocksWritten;
                }
                b.buffer = { mGpHost, written.at, bytes };
            }
            else if (r.kind == ql_glsl::RES_SAMPLER)
            {
                for (U32 e = 0; e < r.count && e < 16; ++e)
                {
                    const std::string element = r.count > 1 ? llformat("%s[%u]", r.name.c_str(), e) : r.name;
                    const QLVulkanPresenter::GpTexture* tex = nullptr;
                    for (U32 i = 0; i < d.texture_count; ++i)
                    {
                        if (d.textures[i].name && (element == d.textures[i].name || (e == 0 && r.name == d.textures[i].name)))
                        {
                            tex = &d.textures[i];
                        }
                    }
                    // A sampler GL has no unit for: GL's compiler found the program never reads it (Vulkan's, which
                    // does not optimize, keeps it). Nothing reads it here either: an empty unit's black stands in.
                    static const QLVulkanPresenter::GpTexture unread = {};
                    if (!tex)
                    {
                        tex = &unread;
                    }
                    VkImageView view = VK_NULL_HANDLE;
                    VkImage image = VK_NULL_HANDLE;
                    bool is_depth = false;
                    // GL_SAMPLER_CUBE_MAP_ARRAY (and its shadow, int and uint kinds) read a cube array; every other
                    // sampler here a 2D texture.
                    const bool wants_cube = r.gl_type >= 0x900C && r.gl_type <= 0x900F;
                    // GL's shadow samplers (2D, 2D array, cube, cube array) compare against a depth texture.
                    const bool wants_depth = r.gl_type == 0x8B62 || r.gl_type == 0x8DC4 || r.gl_type == 0x8DC5 ||
                                             r.gl_type == 0x900D;
                    // A cube array unit with nothing bound reads black in GL (an incomplete texture): so here. A shadow
                    // sampler's has no answer GL defines.
                    if (wants_cube && !wants_depth && (tex->texture == 0 || tex->texture == d.white_texture))
                    {
                        view  = mGbBlackCube.view;
                        image = VK_NULL_HANDLE;
                    }
                    else if ((wants_cube || wants_depth) && (tex->texture == 0 || tex->texture == d.white_texture))
                    {
                        give_back();
                        gpWhy(p.name + ": " + element + " has nothing of its kind bound");
                        return false;
                    }
                    else if (tex->texture == 0)
                    {
                        view  = mGbBlack.view;
                        image = VK_NULL_HANDLE;
                    }
                    else if (tex->texture == d.white_texture)
                    {
                        view  = mGbWhite.view;
                        image = VK_NULL_HANDLE;
                    }
                    else
                    {
                        VkFormat format = VK_FORMAT_UNDEFINED;
                        bool cube = false;
                        view = gpView(tex->texture, image, format, is_depth, &cube);
                        if (view && gpLinearTwin(format) != VK_FORMAT_UNDEFINED && !tex->srgb_decode)
                        {
                            // Read without decoding, as GL's sampler says (GL_SKIP_DECODE_EXT): its linear twin.
                            view = gpView(tex->texture, image, format, is_depth, &cube, true);
                            if (!view)
                            {
                                give_back();
                                gpWhy(p.name + ": " + element + " is sRGB, read without decoding, and was made before it "
                                               "could be viewed so");
                                return false;
                            }
                        }
                        if (view && cube != wants_cube && !wants_depth)
                        {
                            // A unit still holding what an earlier draw left there, of another kind (an indexed batch
                            // binds tex0..texN for the textures it has, and its texture_index never reaches the rest):
                            // GL cannot sample it through this sampler, so a correct program does not. Black of the
                            // kind it reads stands in.
                            view     = wants_cube ? mGbBlackCube.view : mGbBlack.view;
                            image    = VK_NULL_HANDLE;
                            is_depth = false;
                        }
                        else if (view && (cube != wants_cube || (wants_depth && !is_depth)))
                        {
                            give_back();
                            gpWhy(llformat("%s: %s (GL name %u) is not the kind of texture it reads", p.name.c_str(),
                                           element.c_str(), tex->texture));
                            return false;
                        }
                        if (!view)
                        {
                            give_back();
                            gpWhy(llformat("%s: %s (GL name %u) is not in Vulkan's memory", p.name.c_str(),
                                           element.c_str(), tex->texture));
                            return false;
                        }

                    }
                    const VkSampler sampler = gpSampler(*tex);
                    if (!sampler)
                    {
                        give_back();
                        gpWhy(p.name + ": vkCreateSampler failed");
                        return false;
                    }
                    b.images[e]    = { sampler, view, VK_IMAGE_LAYOUT_GENERAL };
                    b.names[e]     = image ? tex->texture : 0;
                    b.vk_images[e] = image;
                }
            }
            else
            {
                give_back();
                gpWhy(p.name + ": reads " + r.name + ", a kind of resource Vulkan does not bind yet");
                return false;
            }
            bound.push_back(b);
        }
        U32 base_vertex = 0, base_index = 0;
        if (mesh)
        {
            // Into the arena once a content stamp (GL copies it in, before the hand-over's signal).
            if (!arenaPlace(mGbArena, d.mesh.stamp, d.mesh.vertex_buffer, d.mesh.position, d.mesh.weight4,
                            d.mesh.vertex_count, d.mesh.index_buffer, d.mesh.total_indices, base_vertex, base_index,
                            d.mesh.texcoord0 != ~0u ? d.mesh.texcoord0 : 0, d.mesh.color,
                            d.mesh.normal != ~0u ? d.mesh.normal : 0, true, d.mesh.tangent, d.mesh.texcoord1,
                            d.mesh.texcoord2, d.mesh.emissive))
            {
                give_back();
                gpWhy("the arena is full");
                return false;
            }
        }
        else if (p.position >= 0)
        {
            vertices_at = gpAllocate((VkDeviceSize)d.vertex_count * 12);
            if (vertices_at == GP_NONE)
            {
                give_back();
                gpWhy("the pass's host memory is full");
                return false;
            }
            memcpy(mGpMapped + vertices_at, d.positions, (size_t)d.vertex_count * 12);
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        set_alloc.descriptorPool     = mGpSlots[mGpSlot].pool;
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts        = &p.setLayout;
        if (vkAllocateDescriptorSets(mDevice, &set_alloc, &set) != VK_SUCCESS)
        {
            give_back();
            gpWhy("the pass's descriptor pool is full");
            return false;
        }
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(bound.size());
        for (const Bound& b : bound)
        {
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = set;
            w.dstBinding      = b.r->binding;
            if (b.r->kind == ql_glsl::RES_UNIFORM_BLOCK)
            {
                w.descriptorCount = 1;
                w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo     = &b.buffer;
            }
            else
            {
                w.descriptorCount = llmin(b.r->count, 16u);
                w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo      = b.images;
            }
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(mDevice, (U32)writes.size(), writes.data(), 0, nullptr);

        // Recorded from here: GL's images taken over, a barrier where this draw reads what an earlier one wrote (or
        // writes what one read), then the render pass.
        const U32 attachments = t.colors + (t.depth ? 1 : 0);
        for (U32 i = 0; i < attachments; ++i)
        {
            const bool is_depth = t.depth && i == t.colors;
            const bool stencil  = t.formats[i] == VK_FORMAT_D24_UNORM_S8_UINT || t.formats[i] == VK_FORMAT_D32_SFLOAT_S8_UINT;
            gpAcquire(t.names[i], t.images[i],
                      is_depth ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                               : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                      true);
        }
        if (mesh && !mGpArenaAcquired)
        {
            gpEndRenderPass();
            bufferBarrier(mGpSlots[mGpSlot].cmd, mGbArena.buffer, 0,
                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL,
                          mQueueFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
            mGpArenaAcquired = true;
        }
        bool hazard = false;
        for (const Bound& b : bound)
        {
            for (U32 e = 0; e < 16; ++e)
            {
                if (b.vk_images[e])
                {
                    VkFormat format = VK_FORMAT_UNDEFINED;
                    VkImage image = VK_NULL_HANDLE;
                    bool is_depth = false;
                    gpView(b.names[e], image, format, is_depth);
                    const bool stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
                    gpAcquire(b.names[e], b.vk_images[e],
                              is_depth ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                              (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                       : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                              false);
                    // The depth a draw both tests against and reads (depth bounds, read-only) is no hazard.
                    const bool own_depth = t.depth && b.vk_images[e] == t.images[t.colors] && !d.depth_write;
                    hazard = hazard || (mGpWritten.count(b.vk_images[e]) != 0 && !own_depth);
                }
            }
        }
        for (U32 i = 0; i < t.colors; ++i)
        {
            hazard = hazard || mGpRead.count(t.images[i]) != 0;
        }
        if (d.depth_write && t.depth)
        {
            hazard = hazard || mGpRead.count(t.images[t.colors]) != 0;
        }
        // A target other than the open one, written before: a barrier as well (its writes are ordered by nothing).
        if (hazard || d.clear)
        {
            if (mGpRecorded)
            {
                gpBarrier();
            }
            else
            {
                gpEndRenderPass();
            }
        }
        VkCommandBuffer cmd = mGpSlots[mGpSlot].cmd;
        if (mGpOpenTarget != target_index || d.clear)
        {
            gpEndRenderPass();
            VkClearValue clears[5] = {};
            for (U32 i = 0; i < t.colors; ++i)
            {
                memcpy(clears[i].color.float32, d.clear_color, sizeof(d.clear_color));
            }
            VkRenderPassBeginInfo pass_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            pass_begin.renderPass      = d.clear ? t.clear : t.load;
            pass_begin.framebuffer     = t.framebuffer;
            pass_begin.renderArea      = { { 0, 0 }, { t.width, t.height } };
            pass_begin.clearValueCount = attachments;
            pass_begin.pClearValues    = clears;
            vkCmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
            mGpOpenTarget = target_index;
            const VkRect2D scissor = { { 0, 0 }, { t.width, t.height } };
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }
        {
            // GL's viewport, its y from the bottom row: Vulkan's rows run the same way in memory (no flip), so the
            // same numbers.
            VkViewport viewport = { 0.f, 0.f, (float)t.width, (float)t.height, 0.f, 1.f };
            if (d.viewport[2] > 0 && d.viewport[3] > 0)
            {
                viewport = { (float)d.viewport[0], (float)d.viewport[1], (float)d.viewport[2], (float)d.viewport[3],
                             0.f, 1.f };
            }
            vkCmdSetViewport(cmd, 0, 1, &viewport);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (d.bounds_max > d.bounds_min && mHasDepthBounds && t.depth)
        {
            vkCmdSetDepthBounds(cmd, d.bounds_min, d.bounds_max);
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &set, 0, nullptr);
        if (mesh)
        {
            VkBuffer buffers[GP_STREAMS];
            VkDeviceSize starts[GP_STREAMS];
            for (size_t i = 0; i < p.inputs.size(); ++i)
            {
                buffers[i] = mGbArena.buffer;
                // A program reading the emissive colour reads it as diffuse_color too (LLVertexBuffer::setupVertexBuffer).
                const U32 stream = p.inputs[i].stream == GP_COLOR && p.reads_emissive ? (U32)GP_EMISSIVE
                                                                                       : p.inputs[i].stream;
                starts[i] = gpStreamStart(stream);
            }
            if (!p.inputs.empty())
            {
                vkCmdBindVertexBuffers(cmd, 0, (U32)p.inputs.size(), buffers, starts);
            }
            vkCmdBindIndexBuffer(cmd, mGbArena.buffer, mGbArena.indexStart(), VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, d.mesh.index_count, llmax(d.instances, 1u), base_index + d.mesh.first_index,
                             (S32)base_vertex, 0);
        }
        else
        {
            if (vertices_at != GP_NONE)
            {
                vkCmdBindVertexBuffers(cmd, 0, 1, &mGpHost, &vertices_at);
            }
            vkCmdDraw(cmd, p.position >= 0 ? llmax(d.vertex_count, 3u) : d.vertex_count, llmax(d.instances, 1u), 0, 0);
        }
        mGpUsedArena = mGpUsedArena || mesh;
        mGpRecorded = true;
        for (U32 i = 0; i < t.colors; ++i)
        {
            mGpWritten.insert(t.images[i]);
        }
        if (d.depth_write && t.depth)
        {
            mGpWritten.insert(t.images[t.colors]);
        }
        for (const Bound& b : bound)
        {
            for (U32 e = 0; e < 16; ++e)
            {
                if (b.vk_images[e])
                {
                    mGpRead.insert(b.vk_images[e]);
                }
            }
        }
        ++mGpDrawsNow;
        return true;
    }

    void gpPassAbandon()
    {
        if (!mGpInPass)
        {
            return;
        }
        gpEndRenderPass();
        vkEndCommandBuffer(mGpSlots[mGpSlot].cmd);  // recorded, never submitted: reset at the slot's next pass
        mGpInPass = false;
    }

    bool gpPassEnd()
    {
        if (!mGpInPass)
        {
            return false;
        }
        if (!mGpRecorded)
        {
            gpPassAbandon();
            return true;
        }
        LL_PROFILE_ZONE_NAMED("vulkan gl programs");
        gpEndRenderPass();
        GpSlot& slot = mGpSlots[mGpSlot];
        // Back to GL's queue, every image taken.
        std::vector<VkImageMemoryBarrier> releases;
        for (const GpAcquired& a : mGpAcquired)
        {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask       = a.attachment ? (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                                 : 0;
            b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = mQueueFamily;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            b.image               = a.image;
            b.subresourceRange    = { a.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
            releases.push_back(b);
        }
        vkCmdPipelineBarrier(slot.cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, (U32)releases.size(),
                             releases.data());
        if (mGpArenaAcquired)
        {
            bufferBarrier(slot.cmd, mGbArena.buffer, 0, 0, mQueueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        vkEndCommandBuffer(slot.cmd);
        mGpInPass = false;

        const std::vector<GLenum> layouts(mGpNames.size(), GL_LAYOUT_GENERAL_EXT);
        std::vector<GLuint> gl_buffers;
        if (mGpUsedArena)
        {
            gl_buffers.push_back(mGbArena.gl);  // its copies in, GL's, before the signal
        }
        {
            LL_PROFILE_ZONE_NAMED("vulkan gl programs - gl signal");
            gl.SignalSemaphoreEXT(mGpGLDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(), (GLuint)mGpNames.size(),
                                  mGpNames.data(), layouts.data());
            gl.Flush();
        }
        vkResetFences(mDevice, 1, &slot.fence);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &mGpGLDoneVK;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &slot.cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &mGpVKDoneVK;
        VkResult result = VK_SUCCESS;
        {
            LL_PROFILE_ZONE_NAMED("vulkan gl programs - vk submit");
            result = vkQueueSubmit(mQueue, 1, &submit, slot.fence);
        }
        if (result != VK_SUCCESS)
        {
            // GL signalled and gave its images up: an empty batch takes the signal and gives one back, which GL waits
            // for, so GL has its images again (Vulkan never took them). The draws are lost this once, and the runner
            // is given up for good: GL draws the lighting from now on.
            VkSubmitInfo hand_back = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            hand_back.waitSemaphoreCount   = 1;
            hand_back.pWaitSemaphores      = &mGpGLDoneVK;
            hand_back.pWaitDstStageMask    = &wait_stage;
            hand_back.signalSemaphoreCount = 1;
            hand_back.pSignalSemaphores    = &mGpVKDoneVK;
            if (vkQueueSubmit(mQueue, 1, &hand_back, VK_NULL_HANDLE) == VK_SUCCESS)
            {
                gl.WaitSemaphoreEXT(mGpVKDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(), (GLuint)mGpNames.size(),
                                    mGpNames.data(), layouts.data());
            }
            mGpReady = false;
            mGpNote  = llformat("stopped: a pass could not be submitted (VkResult %d)", (S32)result);
            LL_WARNS("Vulkan") << "GL programs in Vulkan " << mGpNote << LL_ENDL;
            return false;
        }
        slot.submitted = true;
        {
            LL_PROFILE_ZONE_NAMED("vulkan gl programs - gl wait");
            gl.WaitSemaphoreEXT(mGpVKDoneGL, (GLuint)gl_buffers.size(), gl_buffers.data(), (GLuint)mGpNames.size(),
                                mGpNames.data(), layouts.data());
        }
        ++mGpPassesNow;
        return true;
    }

    void gpFrame()
    {
        mGpDrawsLast  = mGpDrawsNow;
        mGpPassesLast = mGpPassesNow;
        mGpKeptLast   = mGpKeptNow;
        mGpDrawsNow = mGpPassesNow = mGpKeptNow = 0;
        mGpBlocksReusedLast  = mGpBlocksReused;
        mGpBlocksWrittenLast = mGpBlocksWritten;
        mGpBlocksReused = mGpBlocksWritten = 0;
        mGpHostPeakLast = mGpHostPeak;
        mGpHostPeak     = 0;
    }

    std::string gpInfo() const
    {
        if (!mGpReady)
        {
            return mGpNote + (mGpWhy.empty() ? std::string() : " | last kept by GL: " + mGpWhy);
        }
        U32 programs = 0;
        for (const GpProgram& p : mGpPrograms)
        {
            programs += p.ok ? 1 : 0;
        }
        return llformat("%u draws in %u passes last frame, %u kept by GL | %u of %u programs compiled | blocks %u "
                        "written, %u reused, fullest pass %.1f of %u MB",
                        mGpDrawsLast, mGpPassesLast, mGpKeptLast, programs, (U32)mGpPrograms.size(),
                        mGpBlocksWrittenLast, mGpBlocksReusedLast, (double)mGpHostPeakLast / (1 << 20),
                        (U32)(GP_SLOT_BYTES >> 20)) +
               (mGpWhy.empty() ? std::string() : " | last kept by GL: " + mGpWhy);
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
        if (mExtStorage)
        {
            if (gQLExternalStorage == mExtStorage)
            {
                gQLExternalStorage = nullptr;
            }
            mExtStorage->close();  // waits for a call in flight (the LLImageGL thread's), then lets none in
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
            destroyGpObjects();  // before the G-buffer's, whose default textures it reads
            destroyGbObjects();  // before the targets and textures whose views and sets it holds
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
            // Render targets GL still draws into live in this device's memory (QuasarVulkanTargets): the device, its
            // memory, the instance and the library stay, for the process's life, rather than go from under them.
            mExtKeptAlive = extLiveCount() > 0;
            if (mExtKeptAlive)
            {
                LL_WARNS("Vulkan") << "Vulkan presentation ends with " << extLiveCount()
                                   << " render targets in its memory: the device is kept for them" << LL_ENDL;
            }
            else
            {
                vkDestroyDevice(mDevice, nullptr);
            }
            mDevice = VK_NULL_HANDLE;
        }
        if (mInstance && !mExtKeptAlive)
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
        if (mLibrary && !mExtKeptAlive)
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
        pollMemoryBudget();
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
        gQLVideoMemoryBudgetBytes.store(0, std::memory_order_relaxed);  // unknown again: the budget goes back to its own
        gQLVideoMemoryUsedBytes.store(0, std::memory_order_relaxed);
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
                                         unsigned base_vertex, unsigned first_index, unsigned index_count,
                                         const unsigned* tiles, unsigned tile_count)
{
    return sActive && sActive->shadowDrawMasked(mvp, palette, tile, texture_matrix, cutoff, test, base_vertex,
                                                first_index, index_count, tiles, tile_count);
}

// static
bool QLVulkanPresenter::shadowEnd(unsigned depth_texture)
{
    return sActive && sActive->shadowEnd((GLuint)depth_texture);
}

// static
void QLVulkanPresenter::gbufferFrame(unsigned frame)
{
    if (sActive)
    {
        sActive->gbufferFrame(frame);
    }
}

// static
bool QLVulkanPresenter::gbufferBegin(const unsigned* colors, unsigned color_count, unsigned depth, unsigned width,
                                     unsigned height, bool greater, bool remap_depth, float anisotropy)
{
    return sActive && sActive->gbufferBegin(colors, color_count, depth, width, height, greater, remap_depth,
                                            anisotropy);
}

// static
bool QLVulkanPresenter::gbufferArena(unsigned long long stamp, unsigned vertex_buffer, unsigned position_offset,
                                     unsigned normal_offset, unsigned uv_offset, unsigned color_offset,
                                     unsigned vertex_count, unsigned index_buffer, unsigned index_count,
                                     unsigned& base_vertex, unsigned& base_index, unsigned tangent_offset,
                                     unsigned uv1_offset, unsigned uv2_offset, unsigned weight_offset)
{
    return sActive && sActive->gbufferArena((U64)stamp, vertex_buffer, position_offset, normal_offset, uv_offset,
                                            color_offset, vertex_count, index_buffer, index_count, base_vertex,
                                            base_index, tangent_offset, uv1_offset, uv2_offset, weight_offset);
}

// static
bool QLVulkanPresenter::gbufferDraw(const GbDraw& draw)
{
    return sActive && sActive->gbufferDraw(draw);
}

// static
bool QLVulkanPresenter::gbufferTerrain(const TerrainDraw& draw)
{
    return sActive && sActive->gbufferTerrain(draw);
}

// static
std::string QLVulkanPresenter::terrainInfo()
{
    return sActive ? sActive->terrainInfo() : std::string("no Vulkan presenter");
}

// static
bool QLVulkanPresenter::gbufferEnd()
{
    return sActive && sActive->gbufferEnd();
}

// static
void QLVulkanPresenter::gbufferAbandon()
{
    if (sActive)
    {
        sActive->gbufferAbandon();
    }
}

// static
std::string QLVulkanPresenter::gbufferInfo()
{
    return sActive ? sActive->gbufferInfo() : std::string("needs Vulkan presentation");
}

// static
bool QLVulkanPresenter::shadowFlush()
{
    return !sActive || sActive->shadowFlush();
}

// static
void QLVulkanPresenter::shadowMergeNext()
{
    if (sActive)
    {
        sActive->mShMergeNext = true;
    }
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
int QLVulkanPresenter::glProgram(unsigned long long key, unsigned link_count, const char* name, const GpSource* sources,
                                 unsigned source_count)
{
    return sActive ? sActive->gpProgram(key, link_count, name, sources, source_count) : -1;
}

// static
std::string QLVulkanPresenter::glProgramNote(unsigned long long key)
{
    return sActive ? sActive->gpProgramNote(key) : std::string("needs Vulkan presentation (QuasarVulkanPresent)");
}

// static
bool QLVulkanPresenter::glPassBegin()
{
    return sActive && sActive->gpPassBegin();
}

// static
bool QLVulkanPresenter::glDraw(const GpDraw& draw)
{
    return sActive && sActive->gpDraw(draw);
}

// static
bool QLVulkanPresenter::glPassEnd()
{
    return sActive && sActive->gpPassEnd();
}

// static
void QLVulkanPresenter::glPassAbandon()
{
    if (sActive)
    {
        sActive->gpPassAbandon();
    }
}

// static
void QLVulkanPresenter::glFrame(unsigned)
{
    if (sActive)
    {
        sActive->gpFrame();
    }
}

// static
std::string QLVulkanPresenter::glInfo()
{
    return sActive ? sActive->gpInfo() : std::string("needs Vulkan presentation (QuasarVulkanPresent)");
}

// static
bool QLVulkanPresenter::depthTest()
{
    return sActive && sActive->depthTest();
}

// static
std::string QLVulkanPresenter::textureInfo()
{
    return sActive ? sActive->textureInfo() : std::string("needs Vulkan presentation");
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
        // The process's video memory against its budget first: over it, Streamline stops making frames.
        // Raw: as Windows (and Streamline) count it, shared memory twice; real: shared memory once.
        const U64 vram_real   = gQLVideoMemoryUsedBytes.load(std::memory_order_relaxed);
        const U64 vram_raw    = impl.mVramRawUsed;
        const U64 vram_budget = gQLVideoMemoryBudgetBytes.load(std::memory_order_relaxed);
        const std::string vram =
            vram_budget ? llformat("VRAM %.1f/%.1f GB%s (real %.1f), ", (double)vram_raw / (1ull << 30),
                                   (double)vram_budget / (1ull << 30), vram_raw > vram_budget ? " OVER" : "",
                                   (double)vram_real / (1ull << 30))
                        : std::string();
        // Streamline's own reason, where it stopped making frames while on.
        const std::string stopped = impl.mFGStatus != 0 ? " | " + impl.mFGNote : std::string();
        return llformat("on, %u shown a frame drawn, %sinputs %ux%u%s", impl.mFGPresented, vram.c_str(), impl.mFGWidth,
                        impl.mFGHeight, stopped.c_str());
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
