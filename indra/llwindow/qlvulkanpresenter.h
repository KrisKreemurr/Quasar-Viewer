/**
 * @file qlvulkanpresenter.h
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

#pragma once

#include "llwin32headers.h"

#include <memory>
#include <string>

/// Stage 0 of the Vulkan plan (claude/vulkan-plan.md in the project): GL goes on drawing every frame into the window's
/// back buffer as it always has; at the swap, the back buffer is copied into an image whose memory Vulkan owns and GL
/// imports (GL_EXT_memory_object_win32), and Vulkan copies that into its swapchain image and presents it. GL never
/// calls SwapBuffers while this is active.
///
/// The two APIs take turns on the shared image through two semaphores both of them hold (GL_EXT_semaphore_win32): GL
/// signals one once it has written the frame, which Vulkan's copy waits for; Vulkan signals the other once it has read
/// it, which GL waits for before it writes the next. Neither waits on the CPU for the other.
///
/// Vulkan is loaded at run time (vulkan-1.dll), so a machine without it stays on GL. Anything that goes wrong in setting
/// up leaves GL presenting; anything that goes wrong after makes present() return false, and the window goes back to
/// SwapBuffers for good.
///
/// Every call is on the main thread, with the window's GL context current.
class QLVulkanPresenter
{
public:
    QLVulkanPresenter();
    ~QLVulkanPresenter();

    QLVulkanPresenter(const QLVulkanPresenter&) = delete;
    QLVulkanPresenter& operator=(const QLVulkanPresenter&) = delete;

    /// Sets up Vulkan on this window. False, with why in describe(), where it cannot: then nothing is left set up.
    bool init(HWND window, bool vsync);

    /// Lets everything go, GL's imports and Vulkan's objects, waiting for both to finish with them first.
    void shutdown();

    bool active() const;

    /// Presents what GL has drawn into the back buffer. True once it is presented, or skipped for this frame (a
    /// minimized window, a swapchain being made again); false if Vulkan can no longer present, and the caller should
    /// shut this down and swap through GL.
    bool present();

    /// VSync on: the swapchain waits for the display (FIFO). Off: it presents at once where it can (IMMEDIATE, then
    /// MAILBOX). Takes effect at the next present.
    void setVSync(bool vsync);

    /// "Vulkan, <mode>, <w>x<h>", or what stopped it.
    std::string describe() const;

    // ---- NVIDIA Streamline (QuasarStreamline): Reflex now, DLSS Frame Generation next ----------------------------

    /// Before init: load Vulkan through NVIDIA Streamline (sl.interposer.dll beside the viewer, its signature checked)
    /// rather than vulkan-1.dll alone. log_dir is where Streamline writes its log, UTF-8. Ignored in a build without the
    /// SDK; a Streamline that cannot be loaded leaves Vulkan loaded as before.
    static void configureStreamline(bool enable, const std::string& log_dir);

    /// What the frame's latency markers mark (Reflex, PC Latency).
    enum ELatencyMark
    {
        SIMULATION_END,       // input, messages and object updates done for the frame
        RENDER_SUBMIT_START,  // the frame's drawing begins
    };

    /// Start of each frame, on the main thread: a new frame for Streamline, Reflex's sleep if Reflex is on (mode 0 off,
    /// 1 low latency, 2 low latency with boost), and the simulation's start. Nothing while Streamline is not running.
    static void frameBegin(int reflex_mode);
    static void latencyMark(ELatencyMark mark);

    /// For the status line: "Streamline off", or Streamline's state and Reflex's mode.
    static std::string streamlineInfo();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
    static Impl* sStreamline;  // the presenter Streamline runs under, for the per-frame calls; null when none does
};
