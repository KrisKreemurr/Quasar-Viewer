// Quasar: NVIDIA Streamline on Slipstream's Vulkan renderer -- Reflex, and DLSS Frame
// Generation (QuasarDLSSFrameGen). Ported from the old Quasar presenter
// (llwindow/qlvulkanpresenter.cpp on quasar/vulkan).
//
// How Vulkan reaches Streamline: the viewer delay-loads vulkan-1.dll (Windows), and the
// delay-load hook (qlstreamline_win.cpp) hands it NVIDIA's sl.interposer.dll instead when
// QuasarStreamline is on and the interposer is beside the viewer with NVIDIA's signature.
// The interposer forwards every call to the real loader and hooks the few frame
// generation needs (instance and device creation, the swapchain, present). Without it
// everything here is inert and Vulkan is the plain loader.
//
// Per frame (LLVulkanContext): frameBegin() at the top (frame token, Reflex sleep),
// marker()s around recording, submit and present, recordInputs() in the frame's command
// buffer once the world is drawn and before the HUD attachments (it copies the HUD-less
// colour, the motion vectors and the depth, and tags them for the frame). Streamline then
// presents the generated frame itself, inside vkQueuePresentKHR.
#pragma once

#include "vk_device.h"

#include <cstdint>

namespace qlsl
{
// The viewer's choice, before Vulkan is first used (llappviewer). The delay-load hook
// reads it; false (or a hook that ran before it was set) leaves the plain loader.
void setWanted(bool streamline);
bool wanted();
// From the delay-load hook: loads + checks + starts the interposer. Its module (an
// HMODULE), or nullptr for the plain loader. Windows only; nullptr elsewhere.
void* bootInterposer();
// The interposer is in and slInit succeeded.
bool running();
// Why not, or "running" (for the log and the stats line).
const char* note();

// Once the device exists: the feature functions the frames call.
void startFeatures();

struct FrameSettings
{
    int  reflexMode = 0;      // QuasarReflex: 0 off, 1 low latency, 2 low latency + boost
    bool frameGen = false;    // QuasarDLSSFrameGen
    bool vsync = false;       // Streamline's Vulkan frame generation needs vsync off
};
void frameBegin(const FrameSettings& s);

enum Marker { M_SIM_END, M_RENDER_SUBMIT_START, M_RENDER_SUBMIT_END, M_PRESENT_START, M_PRESENT_END };
void marker(Marker m);

// Frame generation was switched (wanted or not): Streamline wants the swapchain made
// again. Cleared by the call.
bool takeSwapchainRebuild();
// The swapchain was (re)made: its images, size and format for the frame generation
// options. framesSince counts from 0.
void swapchainChanged(uint32_t imageCount, uint32_t w, uint32_t h, VkFormat format);

// The frame's camera, from the renderer (column-major, column vectors).
struct Camera
{
    float view[16] = {};        // world -> view
    float proj[16] = {};        // view -> clip, unjittered
    float reproj[16] = {};      // clip -> previous frame's clip, unjittered
    float jitterPx[2] = {};     // render pixels
    bool  reset = false;        // a camera cut / teleport: no history
    bool  reversedZ = true;
};
struct Inputs
{
    VkImage     swapImage = VK_NULL_HANDLE;   // COLOR_ATTACHMENT_OPTIMAL: the world, no HUD attachments
    VkFormat    swapFormat = VK_FORMAT_UNDEFINED;
    uint32_t    w = 0, h = 0;                 // display size (the swap image's)
    VkImageView motionView = VK_NULL_HANDLE;  // render size, SHADER_READ_ONLY_OPTIMAL; null = none
    VkImage     depthImage = VK_NULL_HANDLE;  // display size, DEPTH_ATTACHMENT_OPTIMAL (and left so)
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat    depthFormat = VK_FORMAT_D32_SFLOAT;
    uint32_t    slot = 0;
    bool        flipMotionY = false;
};
// The frame's inputs into the command buffer and Streamline, when frame generation runs
// for this frame (else nothing). true = it does.
bool recordInputs(VkDeviceContext& ctx, VkCommandBuffer cmd, const Inputs& in, const Camera& cam);
// Frame generation is on for the frame being recorded.
bool frameGenOn();
// Frames Streamline generated since the last call (for FPS*).
uint32_t takeGeneratedFrames();

void destroy(VkDeviceContext& ctx);
void shutdown();
} // namespace qlsl
