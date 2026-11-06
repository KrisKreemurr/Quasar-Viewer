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

    // ---- NVIDIA Streamline (QuasarStreamline): Reflex, and DLSS Frame Generation below ----------------------------

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
    /// frame_gen is QuasarDLSSFrameGen: whether NVIDIA DLSS Frame Generation is wanted (it needs Reflex, so asking for
    /// it runs Reflex at low latency at the least).
    static void frameBegin(int reflex_mode, bool frame_gen);
    static void latencyMark(ELatencyMark mark);

    /// For the status line: "Streamline off", or Streamline's state and Reflex's mode.
    static std::string streamlineInfo();

    // ---- NVIDIA DLSS Frame Generation (QuasarDLSSFrameGen) ------------------------------------------------------

    /// The camera of the frame whose inputs are handed over, for Streamline's constants. The matrices are Streamline's:
    /// row-major, for row vectors, which is exactly how LLMatrix4a lays GL's column-vector matrices out in memory, so
    /// the viewer copies its matrices' floats in order.
    struct FrameGenCamera
    {
        float viewToClip[16];      // the projection
        float clipToView[16];      // and its inverse
        float clipToPrevClip[16];  // this frame's clip space to the last frame's
        float prevClipToClip[16];  // and back
        float position[3];         // the camera, in the agent's space
        float up[3];
        float right[3];
        float forward[3];
        float nearPlane;
        float farPlane;
        float fovY;                // radians
        float aspect;              // width / height
        bool  depthInverted;       // reverse-Z: nearer is larger
        bool  reset;               // the last frame does not lead to this one (a teleport, a cut, the first frame)
    };

    /// Whether DLSS Frame Generation is loaded and the viewer should make its inputs this frame.
    static bool frameGenWanted();

    /// After the world is drawn and before the interface, on the main thread with the window's GL context current: the
    /// frame's inputs. The back buffer as it is now is the HUD-less colour; depth_texture (R32F) and motion_texture
    /// (RG16F, window UV, top down) are GL textures of the window's size, rows bottom up as GL draws them. All three are
    /// copied (turned over) into images Vulkan shares, and tagged for Streamline in the frame's present.
    static void frameGenInputs(unsigned depth_texture, unsigned motion_texture, unsigned width, unsigned height,
                               const FrameGenCamera& camera);

    /// For the status line: what DLSS Frame Generation is doing, or why it is not.
    static std::string frameGenInfo();

    /// Frames shown for each frame drawn while DLSS Frame Generation runs (2 at 2x), as Streamline last said; 0 when it
    /// does not run.
    static unsigned frameGenShownPerDrawn();

    // ---- Post passes in Vulkan (QuasarVulkanPost) ---------------------------------------------------------------

    /// Whether Vulkan presents and can run post passes for the viewer.
    static bool postPassesAvailable();

    /// AMD CAS sharpening, run in Vulkan: GL's src_texture sharpened into dst_texture, both width x height, any colour
    /// format GL can blit. cas_const0 and cas_const1 are CasSetup's. GL copies src into an image Vulkan shares, Vulkan
    /// runs the filter, and GL copies the result into dst, each side waiting for the other on the GPU, not the CPU.
    /// False where it could not run, dst untouched: the caller runs GL's pass instead.
    static bool sharpen(unsigned src_texture, unsigned dst_texture, unsigned width, unsigned height,
                        const unsigned cas_const0[4], const unsigned cas_const1[4]);

    /// For the status line.
    static std::string postInfo();

    // ---- The interface layer (QuasarUILayer) ---------------------------------------------------------------------

    /// Whether Vulkan can lay the viewer's interface layer over the frame (its compute pipeline made, or makeable).
    static bool uiLayerAvailable();

    /// Before the swap, once the interface is drawn: this frame's interface layer, a GL texture (RGBA8, premultiplied,
    /// the window's size, GL's rows). The present lays it over the frame in Vulkan, and hands it to DLSS Frame
    /// Generation as the frame's UI. A frame that hands none over shows the window's back buffer as it is.
    static void uiLayer(unsigned texture, unsigned width, unsigned height);

    static std::string uiLayerInfo();

    // ---- Shared geometry (step C) --------------------------------------------------------------------------------

    /// Once, with the window's GL context current: whether a buffer Vulkan allocates can be imported by GL and written
    /// and read both ways. Waits on the CPU for each step, so it is run once, after login, never per frame.
    static bool geometryTest();

    static std::string geometryInfo();

    /// Once, after the geometry test, with the window's GL context current: whether a depth image Vulkan draws into
    /// can be copied by GL into a depth texture made as the viewer's shadow maps are, every texel as drawn. Waits on
    /// the CPU, so it is run once, never per frame.
    static bool depthTest();

    static std::string depthInfo();

    // ---- Shadow casters drawn by Vulkan (QuasarVulkanShadows) ---------------------------------------------------

    /// A shadow map's unrigged opaque casters, drawn by Vulkan into GL's map in one indirect draw. On the main thread,
    /// with the window's GL context current, between the map's clear and the rest of its passes:
    ///   shadowBegin, then for each caster shadowArena (its mesh) and shadowDraw, then shadowEnd with the map's depth
    ///   texture (DEPTH_COMPONENT32F). Vulkan draws over the map as it is and GL has it back before its next command.
    /// shadowBegin false: nothing is drawn by Vulkan, GL draws the map. shadowArena or shadowDraw false: that caster is
    /// GL's to draw. shadowEnd false: none of the map's casters were drawn, and GL draws them all.
    /// clamp: GL_DEPTH_CLAMP is on; greater: the depth test is GREATER (reverse-Z); remap_depth: GL's clip space holds
    /// depth from -1 to 1.
    static bool shadowBegin(unsigned width, unsigned height, bool clamp, bool greater, bool remap_depth);

    /// A mesh's positions (16 bytes a vertex, from vertex_offset in vertex_buffer) and 16-bit indices, found by its
    /// content stamp or copied now; base_vertex and base_index say where it is.
    static bool shadowArena(unsigned long long stamp, unsigned vertex_buffer, unsigned vertex_offset,
                            unsigned vertex_count, unsigned index_buffer, unsigned index_count, unsigned& base_vertex,
                            unsigned& base_index);

    /// One caster: its model-view-projection matrix (GL's, column-major), and its indices from first_index (into the
    /// arena: base_index plus the draw's own offset), base_vertex added to each.
    static bool shadowDraw(const float mvp[16], unsigned base_vertex, unsigned first_index, unsigned index_count);

    /// Rigged casters (avatars, rigged mesh), after all of a map's unrigged ones. Their meshes go into an arena of their
    /// own with each vertex's joint weights (16 bytes a vertex, from weight_offset), and each draw names its palette:
    /// shadowPalette returns where this frame's copy of it is (written the first time any map asks for it this frame;
    /// avatar and skin tell them apart; floats is LLVOAvatar::MatrixPaletteCache::mGLMp, the origin then 12 a joint),
    /// or ~0u when it could not be kept, and the caster is then GL's to draw.
    static bool shadowArenaRigged(unsigned long long stamp, unsigned vertex_buffer, unsigned vertex_offset,
                                  unsigned weight_offset, unsigned vertex_count, unsigned index_buffer,
                                  unsigned index_count, unsigned& base_vertex, unsigned& base_index);
    static unsigned shadowPalette(unsigned long long avatar, unsigned long long skin, const float* floats,
                                  unsigned count);
    static bool shadowDrawRigged(const float mvp[16], unsigned palette, unsigned base_vertex, unsigned first_index,
                                 unsigned index_count);

    static bool shadowEnd(unsigned depth_texture);

    /// Leaves a begun map without drawing it.
    static void shadowAbandon();

    /// Once a frame, for the status line's counts.
    static void shadowFrame(unsigned frame);

    static std::string shadowInfo();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
    static Impl* sStreamline;  // the presenter Streamline runs under, for the per-frame calls; null when none does
    static Impl* sActive;      // the presenter that presents, for the post passes; null when none does
};
