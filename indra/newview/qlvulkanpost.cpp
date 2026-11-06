/**
 * @file qlvulkanpost.cpp
 * @brief Quasar Viewer: post passes run in Vulkan (QuasarVulkanPost).
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

#include "llviewerprecompiledheaders.h"

#include "qlvulkanpost.h"

#include "llframetimer.h"
#include "llglstates.h"
#include "llstartup.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#if LL_WINDOWS && !LL_SDL_WINDOW
#include "qlvulkanpresenter.h"
#define QL_VULKAN_PRESENTER 1
#endif

// static
bool QLVulkanPost::wanted()
{
#if QL_VULKAN_PRESENTER
    static LLCachedControl<bool> vulkan_post(gSavedSettings, "QuasarVulkanPost", false);
    return vulkan_post() && QLVulkanPresenter::postPassesAvailable();
#else
    return false;
#endif
}

// static
bool QLVulkanPost::sharpen(LLRenderTarget* src, LLRenderTarget* dst, const U32 cas_const0[4], const U32 cas_const1[4])
{
#if QL_VULKAN_PRESENTER
    if (!src || !dst || src->getWidth() != dst->getWidth() || src->getHeight() != dst->getHeight())
    {
        return false;
    }
    gGL.flush();
    return QLVulkanPresenter::sharpen(src->getTexture(), dst->getTexture(), src->getWidth(), src->getHeight(),
                                      cas_const0, cas_const1);
#else
    return false;
#endif
}

#if QL_VULKAN_PRESENTER
// llrender.cpp: while set, blending writes the interface layer's alpha as coverage (QLUILayer).
extern bool gQLUILayerAlpha;
extern bool gSnapshot;      // llviewerdisplay.cpp
extern bool gCubeSnapshot;

namespace
{
    LLRenderTarget sUILayer;
    bool           sUILayerBound = false;
}
#endif

// static
bool QLUILayer::begin()
{
#if QL_VULKAN_PRESENTER
    static LLCachedControl<bool> ui_layer(gSavedSettings, "QuasarUILayer", false);
    if (!ui_layer() || gSnapshot || gCubeSnapshot || !gViewerWindow || !QLVulkanPresenter::uiLayerAvailable())
    {
        if (!ui_layer() && sUILayer.isComplete())
        {
            sUILayer.release();
        }
        return false;
    }
    const U32 width  = (U32)gViewerWindow->getWindowWidthRaw();
    const U32 height = (U32)gViewerWindow->getWindowHeightRaw();
    if (width == 0 || height == 0)
    {
        return false;
    }
    if (!sUILayer.isComplete() || sUILayer.getWidth() != width || sUILayer.getHeight() != height)
    {
        sUILayer.release();
        if (!sUILayer.allocate(width, height, GL_RGBA8))
        {
            return false;
        }
    }
    gGL.flush();
    sUILayer.bindTarget();
    // Cleared whole, alpha too: a clear obeys the colour mask and the scissor, and whatever alpha a frame leaves is the
    // next frame's interface if it is not cleared (black where a window was).
    gQLUILayerAlpha = true;
    gGL.setColorMask(true, true);
    {
        LLGLDisable no_scissor(GL_SCISSOR_TEST);
        const LLColor4 clear_color = gGL.getClearColor();
        gGL.setClearColor(LLColor4(0.f, 0.f, 0.f, 0.f));
        sUILayer.clear(GL_COLOR_BUFFER_BIT);
        gGL.setClearColor(clear_color);
    }
    // Issued again under the layer's alpha rules: the cached state names the window's.
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    sUILayerBound = true;
    return true;
#else
    return false;
#endif
}

// static
void QLUILayer::end()
{
#if QL_VULKAN_PRESENTER
    if (!sUILayerBound)
    {
        return;
    }
    gGL.flush();
    gQLUILayerAlpha = false;
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    sUILayer.flush();
    sUILayerBound = false;
    QLVulkanPresenter::uiLayer(sUILayer.getTexture(), sUILayer.getWidth(), sUILayer.getHeight());
#endif
}

// static
std::string QLUILayer::statusLine()
{
#if QL_VULKAN_PRESENTER
    static LLCachedControl<bool> ui_layer(gSavedSettings, "QuasarUILayer", false);
    if (!ui_layer())
    {
        return "off (QuasarUILayer)";
    }
    return QLVulkanPresenter::uiLayerInfo();
#else
    return "not on this platform";
#endif
}

// static
void QLVulkanPost::tick()
{
#if QL_VULKAN_PRESENTER
    static bool done = false;
    static U32 started_frame = 0;
    if (done || LLStartUp::getStartupState() < STATE_STARTED)
    {
        return;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    if (started_frame == 0)
    {
        started_frame = frame;
        return;
    }
    static LLCachedControl<bool> geometry_test(gSavedSettings, "QuasarVulkanGeometryTest", true);
    if (frame - started_frame < 300 || !geometry_test())
    {
        return;
    }
    done = true;
    gGL.flush();
    QLVulkanPresenter::geometryTest();
    QLVulkanPresenter::depthTest();  // the shadow map hand-over (step 1 of shadows in Vulkan)
#endif
}

// static
std::string QLVulkanPost::geometryLine()
{
#if QL_VULKAN_PRESENTER
    return QLVulkanPresenter::geometryInfo() + " | " + QLVulkanPresenter::depthInfo();
#else
    return "not on this platform";
#endif
}

// static
std::string QLVulkanPost::statusLine()
{
#if QL_VULKAN_PRESENTER
    static LLCachedControl<bool> vulkan_post(gSavedSettings, "QuasarVulkanPost", false);
    if (!vulkan_post())
    {
        return "off (QuasarVulkanPost)";
    }
    return QLVulkanPresenter::postInfo();
#else
    return "not on this platform";
#endif
}
