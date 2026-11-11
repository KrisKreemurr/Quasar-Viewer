// Quasar frame generation: the settings the viewer hands the Vulkan side (no Vulkan
// headers here, so llappviewer can include it). See qlframegenvk.h.
#pragma once

// The startup setting (llappviewer reads QuasarFrameGen before the Vulkan context starts;
// LLVulkanContext copies it into VkSwapchain::sFrameGen at startup). Tuning values may change at any time.
struct QLFrameGenTuning
{
    float motionScale = 1.f;   // QuasarFrameGenMotionScale
    bool  flipMotionY = false; // QuasarFrameGenFlipMotionY
    bool  blendPrev = false;    // QuasarFrameGenBlend
    bool  showGenerated = true;// QuasarFrameGenShowGenerated (false: the generated frame is a copy of the real one)
};
void qlSetFrameGenWanted(bool on);
bool qlFrameGenWanted();
void qlSetFrameGenTuning(const QLFrameGenTuning& t);
const QLFrameGenTuning& qlFrameGenTuning();

// Generated frames since the last take (the Vulkan frame bumps it, the viewer's stats take
// it once a frame for the FPS* statistic: real frames + generated ones).
void qlFrameGenNoteGenerated(unsigned n = 1);
unsigned qlFrameGenTakeGenerated();

// QuasarStreamline, read once right after the settings load and before anything uses
// Vulkan (the delay-load hook decides then whether Vulkan goes through NVIDIA's
// Streamline interposer). See qlstreamlinevk.h.
void qlSetStreamlineWanted(bool on);
