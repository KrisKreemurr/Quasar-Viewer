/**
 * @file fscombatads.cpp
 * @brief Aim down sights (ADS): double-tap-hold right mouse zoom, the OTS to
 *        first person dive, and the ADS screen-edge vignette.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

// Ported from SoapStorm's LLToolCompGun (lltoolcomp.cpp, c3cbbfe737). SoapStorm runs
// ADS beside the NaCl right-click zoom and writes CameraAngle; here the zoom is
// Slipstream's (LLToolCompGun, SlipstreamMouselookAlternativeFOV), which drives the
// camera's default FOV without touching CameraAngle, so ADS does the same. The
// vignette was a GL post-process (LLPipeline::renderVignette, exoVignetteF); here it
// is a term in the Vulkan final pass (clean_final.frag).
//
// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4. Slipstream: CombatFX
// only (a right press never aims down sights and the vignette stays 0 with it off).

#include "llviewerprecompiledheaders.h"

#include "fscombatads.h"
#include "slcombatfx.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "sltpscamera.h"
#include "llcriticaldamp.h"
#include "llframetimer.h"
#include "llsmoothstep.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

namespace
{
// Slipstream (minimal config space): GunMetal-DX's timing / step / per-view vignette settings
// are fixed at their defaults; FSADSVignetteStrength 0 turns the vignette off.
constexpr F32  ADS_SWAP_MS        = 150.f;   // was FSADSSwapTransitionSpeed
constexpr F32  ADS_DOUBLE_TAP_SEC = 0.25f;   // was FSADSDoubleTapTime
constexpr F32  ADS_ZOOM_STEP      = 0.1f;    // was FSADSZoomSensitivity (radians per wheel click)
constexpr F32  ADS_ZOOM_MS        = 100.f;   // was FSADSZoomTransitionSpeed

bool    sCameraSwap = false;
bool    sHeld = false;          // holding ADS
bool    sEasing = false;        // the FOV is easing into or out of ADS
bool    sFromOTS = false;       // entered from OTS: return there on release
bool    sLastTapQuick = false;  // the previous press was a quick tap (not a hold-zoom)
LLTimer sDownTimer;             // the current press's duration
LLTimer sUpTimer;               // since the last release (the double-tap window)
LLTimer sEaseTimer;
F32     sBaseFOV = 0.f;         // un-zoomed FOV
F32     sADSFOV = 0.f;          // ADS FOV (scroll adjusts it)
F32     sEaseFrom = 0.f;
F32     sEaseTo = 0.f;
F32     sProportion = 1.f;      // how far a release starts into the zoom (shortens the ease)
F32     sVignette = 0.f;        // displayed vignette darkness
U32     sVignetteFrame = 0;

// The OTS <-> first person swap, at FSADSSwapTransitionSpeed (0 = a snap).
void swapCamera(bool to_first_person)
{
    sCameraSwap = true;
    const F32 secs = ADS_SWAP_MS / 1000.f;
    const bool animate = secs > 0.f;
    gAgentCamera.setNextCameraAnimationDuration(animate ? secs : -1.f);
    if (to_first_person)
    {
        gAgentCamera.changeCameraToMouselook(animate);
    }
    else
    {
        SLTPSCamera::getInstance()->returnFromADS(animate);
    }
    gAgentCamera.setNextCameraAnimationDuration(-1.f); // unused if no animation started
    sCameraSwap = false;
}

void startEase(F32 to_fov)
{
    sEaseFrom = LLViewerCamera::getInstance()->getDefaultFOV();
    sEaseTo = to_fov;
    sEaseTimer.reset();
    sEasing = true;
}

void stop()
{
    sHeld = false;
    sEasing = false;
    sFromOTS = false;
    sLastTapQuick = false;
}
} // namespace

// static
bool FSCombatADS::onRightMouseDown(F32 base_fov)
{
    const bool recovering = sEasing;
    sDownTimer.reset();
    if (!SLCombatFX::enabled())
    {
        sEasing = false;
        return false;
    }

    // The held second tap of two quick taps inside the window; the first press must
    // have been a tap, not a hold-zoom, so a hold-then-click or two spaced-out clicks
    // never fire ADS. With FSADSDefaultZoom every press aims down sights.
    static LLCachedControl<bool> ads_enable(gSavedSettings, "FSDoubleTapADS", true);
    static LLCachedControl<bool> ads_default(gSavedSettings, "FSADSDefaultZoom", false);
    if (!ads_default && !(ads_enable && sLastTapQuick && sUpTimer.getElapsedTimeF32() < ADS_DOUBLE_TAP_SEC))
    {
        // A normal zoom; if it cuts into an ADS release, it takes the FOV from here
        // (the vignette still fades out on its own clock).
        if (!SLTPSCamera::isShoulderActive()) sEasing = false;
        return false;
    }
    sLastTapQuick = false; // consume the gesture

    // From OTS, ADS dives into first person and comes back out on release; in first
    // person it only zooms.
    sFromOTS = SLTPSCamera::isShoulderActive();
    if (sFromOTS)
    {
        if (!recovering) sBaseFOV = LLViewerCamera::getInstance()->getAndSaveDefaultFOV();
    }
    else
    {
        sBaseFOV = base_fov;
    }
    sHeld = true;
    if (sFromOTS)
    {
        swapCamera(true);
        if (gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK)
        {
            cancel(); // mouselook refused by settings, joystick or RLV
            return false;
        }
    }

    static LLCachedControl<F32> ads_fov(gSavedSettings, "FSADSZoomFOV", 0.5f);
    sADSFOV = (F32)ads_fov;
    sProportion = 1.f;
    startEase(sADSFOV);
    sHeld = true;
    return true;
}

// static
bool FSCombatADS::onRightMouseUp()
{
    sLastTapQuick = sDownTimer.getElapsedTimeF32() < ADS_DOUBLE_TAP_SEC;
    sUpTimer.reset();

    if (!sHeld)
    {
        return false;
    }
    sHeld = false;
    if (sFromOTS)
    {
        sFromOTS = false;
        swapCamera(false);
    }
    const F32 current = LLViewerCamera::getInstance()->getDefaultFOV();
    const F32 span = fabsf(sBaseFOV - sADSFOV);
    sProportion = (span > 0.001f) ? llclamp(fabsf(sBaseFOV - current) / span, 0.f, 1.f) : 1.f;
    startEase(sBaseFOV);
    return true;
}

// static
bool FSCombatADS::onScrollWheel(S32 clicks)
{
    if (!sHeld)
    {
        return false;
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const F32 fov = llclamp(camera->getDefaultFOV() + (F32)clicks * ADS_ZOOM_STEP,
                            camera->getMinView(), camera->getMaxView());
    camera->setDefaultFOV(fov);
    sADSFOV = camera->getDefaultFOV(); // after the RLVa FOV limits
    sEasing = false;
    gSavedSettings.setF32("FSADSZoomFOV", sADSFOV); // remember the ADS level
    return true;
}

// static
void FSCombatADS::update()
{
    if (!SLCombatFX::enabled())
    {
        if (sFromOTS) onRightMouseUp();
        cancel();
        return;
    }
    if (sFromOTS && (gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK
        || !SLTPSCamera::isEnabled() || !gFocusMgr.getAppHasFocus()
        || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE))
    {
        cancel(); // Esc, another camera, or a script ended the temporary dive
        return;
    }
    if (!sEasing)
    {
        return;
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (camera->mSavedFOVLoaded)
    {
        sEasing = false; // the gun tool put the base FOV back
        return;
    }
    // Milliseconds, 0 = instant; a release that starts part way into the zoom
    // finishes proportionally sooner.
    const F32 duration = ADS_ZOOM_MS / 1000.f;
    F32 effective = duration * sProportion;
    if (effective <= 0.f)
    {
        effective = duration;
    }
    const F32 t = (effective > 0.f) ? llclamp((F32)sEaseTimer.getElapsedTimeF32() / effective, 0.f, 1.f) : 1.f;
    camera->setDefaultFOV(sEaseFrom + (sEaseTo - sEaseFrom) * llsmoothstep(0.f, 1.f, t));
    if (t >= 1.f)
    {
        sEasing = false;
    }
}

// static
void FSCombatADS::reset()
{
    if (!sCameraSwap) stop();
}

// static
void FSCombatADS::cancel()
{
    if (sCameraSwap) return;
    if (ownsFOV()) LLViewerCamera::getInstance()->loadDefaultFOV();
    stop();
    sVignette = 0.f;
}

// static
bool FSCombatADS::isCameraSwap() { return sCameraSwap; }

// static
bool FSCombatADS::returnsToTPS() { return sFromOTS; }

// static
bool FSCombatADS::isHeld()
{
    return sHeld;
}

// static
bool FSCombatADS::ownsFOV()
{
    return sHeld || sEasing;
}

// static
F32 FSCombatADS::getVignetteAmount()
{
    update();
    if (!SLCombatFX::enabled()) return 0.f;
    // Safety net: the camera left the aim modes without the gun tool's deselect
    // (which normally restores the FOV and resets ADS). Put the base FOV back.
    if (ownsFOV() && gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK && !SLTPSCamera::isEngagedNow())
    {
        LLViewerCamera::getInstance()->loadDefaultFOV();
        stop();
    }
    if (!ownsFOV() && sVignette == 0.f)
    {
        return 0.f;   // Slipstream: nothing to ease (always so with CombatFX off)
    }

    // Live darkness: while held or easing out, scaled by how far the FOV has gone
    // toward the ADS level, so it creeps in and fades out with the zoom.
    F32 live = 0.f;
    if (ownsFOV())
    {
        // (Slipstream: GunMetal-DX's per-view vignette switches and FSADSVignetteScaleWithZoom
        // are gone; the vignette is on in both views and FSADSVignetteStrength 0 turns it off.)
        {
            // 0 at the base FOV, 1 at the ADS FOV.
            LLViewerCamera* camera = LLViewerCamera::getInstance();
            F32 progress = 1.f;
            const F32 span = sBaseFOV - sADSFOV;
            if (fabsf(span) > 0.0001f)
            {
                progress = llclamp((sBaseFOV - camera->getDefaultFOV()) / span, 0.f, 1.f);
            }
            static LLCachedControl<F32> vig_strength(gSavedSettings, "FSADSVignetteStrength", 0.5f);
            live = llclamp((F32)vig_strength * progress, 0.f, 1.f);
        }
    }

    // Ease the shown darkness toward the live value once per frame, on its own clock,
    // so the release fade finishes smoothly even when a normal zoom cuts it off.
    const F32 VIGNETTE_FADE_HALF_LIFE = 0.09f; // seconds
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != sVignetteFrame)
    {
        sVignetteFrame = frame;
        sVignette = lerp(sVignette, live, LLSmoothInterpolation::getInterpolant(VIGNETTE_FADE_HALF_LIFE));
        if (fabsf(sVignette - live) < 0.001f)
        {
            sVignette = live; // settle on a clean 0 / full
        }
    }
    return sVignette;
}
