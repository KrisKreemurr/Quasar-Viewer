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

#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercontrol.h"

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
