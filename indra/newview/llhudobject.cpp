/**
 * @file llhudobject.cpp
 * @brief LLHUDObject class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2002-2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llhudobject.h"
#include "llviewerobject.h"

#include "llhudtext.h"
#include "llhudicon.h"
#include "llhudeffectbeam.h"
#include "llhudeffectblob.h"
#include "llhudeffecttrail.h"
#include "llhudeffectlookat.h"
#include "llhudeffectpointat.h"
#include "llhudeffectresetskeleton.h"
#include "llhudnametag.h"
#include "llvoicevisualizer.h"

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

// statics
std::list<LLPointer<LLHUDObject> > LLHUDObject::sHUDObjects;
bool LLHUDObject::sWorldBillboards = false;

// static
void LLHUDObject::worldBillboardPixelVectors(const LLVector3& pos_agent, LLVector3& x_px, LLVector3& y_px)
{
    // Physical size (metres per label pixel) while that keeps the pixel between the two
    // angles, else the nearer angle: ~2 m .. ~4 m physical, nearer = smaller in the world,
    // farther = held readable.
    constexpr F32 kMetresPerPx = 0.005236f;              // = 4 m x 0.075 deg
    constexpr F32 kMinRad = 0.075f * DEG_TO_RAD;
    constexpr F32 kMaxRad = 0.15f * DEG_TO_RAD;
    static LLCachedControl<F32> s_scale(gSavedSettings, "SlipstreamVRWorldTextScale", 1.f);
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    LLVector3 to_head = cam->getOrigin() - pos_agent;
    const F32 dist = to_head.normVec();
    if (dist < 0.01f)
    {
        cam->getPixelVectors(pos_agent, y_px, x_px);   // at the head: degenerate, LL's own
        return;
    }
    // Upright: right is horizontal, up tilts toward the head (the plane faces the head's
    // position, not its orientation: rolling or turning the head never turns a label).
    LLVector3 right = LLVector3::z_axis % to_head;
    if (right.normVec() < 1e-3f)
    {
        right = -cam->getLeftAxis();   // straight above / below the head
    }
    const LLVector3 up = to_head % right;
    const F32 scale = llclamp((F32)s_scale, 0.25f, 4.f);
    const F32 rad = llclamp(kMetresPerPx / dist, kMinRad, kMaxRad) * scale;
    const F32 m = dist * tanf(rad);
    x_px = right * m;
    y_px = up * m;
}

struct hud_object_further_away
{
    bool operator()(const LLPointer<LLHUDObject>& lhs, const LLPointer<LLHUDObject>& rhs) const;
};


bool hud_object_further_away::operator()(const LLPointer<LLHUDObject>& lhs, const LLPointer<LLHUDObject>& rhs) const
{
    return lhs->getDistance() > rhs->getDistance();
}


LLHUDObject::LLHUDObject(const U8 type) :
    mPositionGlobal(),
    mSourceObject(NULL),
    mTargetObject(NULL)
{
    mVisible = true;
    mType = type;
    mDead = false;
}

LLHUDObject::~LLHUDObject()
{
}

void LLHUDObject::markDead()
{
    mVisible = false;
    mDead = true;
    mSourceObject = NULL;
    mTargetObject = NULL;
}

F32 LLHUDObject::getDistance() const
{
    return 0.f;
}

void LLHUDObject::setSourceObject(LLViewerObject* objectp)
{
    if (objectp == mSourceObject)
    {
        return;
    }

    mSourceObject = objectp;
}

void LLHUDObject::setTargetObject(LLViewerObject* objectp)
{
    if (objectp == mTargetObject)
    {
        return;
    }

    mTargetObject = objectp;
}

void LLHUDObject::setPositionGlobal(const LLVector3d &position_global)
{
    mPositionGlobal = position_global;
}

void LLHUDObject::setPositionAgent(const LLVector3 &position_agent)
{
    mPositionGlobal = gAgent.getPosGlobalFromAgent(position_agent);
}

// static
void LLHUDObject::cleanupHUDObjects()
{
    LLHUDIcon::cleanupDeadIcons();
    hud_object_list_t::iterator object_it;
    for (object_it = sHUDObjects.begin(); object_it != sHUDObjects.end(); ++object_it)
    {
        (*object_it)->markDead();
        if ((*object_it)->getNumRefs() > 1)
        {
            LL_INFOS() << "LLHUDObject " << (LLHUDObject *)(*object_it) << " type " << (S32)(*object_it)->getType() << " has " << (*object_it)->getNumRefs() << " refs!" << LL_ENDL;
        }
    }
    sHUDObjects.clear();
}

// static
LLHUDObject *LLHUDObject::addHUDObject(const U8 type)
{
    LLHUDObject *hud_objectp = NULL;

    switch (type)
    {
    case LL_HUD_TEXT:
        hud_objectp = new LLHUDText(type);
        break;
    case LL_HUD_ICON:
        hud_objectp = new LLHUDIcon(type);
        break;
    case LL_HUD_NAME_TAG:
        hud_objectp = new LLHUDNameTag(type);
        break;
    default:
        LL_WARNS() << "Unknown type of hud object:" << (U32) type << LL_ENDL;
    }
    if (hud_objectp)
    {
        sHUDObjects.push_back(hud_objectp);
    }
    return hud_objectp;
}

LLHUDEffect *LLHUDObject::addHUDEffect(const U8 type)
{
    LLHUDEffect *hud_objectp = NULL;

    switch (type)
    {
    case LL_HUD_EFFECT_BEAM:
        hud_objectp = new LLHUDEffectSpiral(type);
        ((LLHUDEffectSpiral *)hud_objectp)->setDuration(0.7f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVMag(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVOffset(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setInitialRadius(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFinalRadius(0.2f);
        ((LLHUDEffectSpiral *)hud_objectp)->setSpinRate(10.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFlickerRate(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleBase(0.05f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleVar(0.02f);
        ((LLHUDEffectSpiral *)hud_objectp)->setColor(LLColor4U(255, 255, 255, 255));
        break;
    case LL_HUD_EFFECT_GLOW:
        // deprecated
        break;
    case LL_HUD_EFFECT_POINT:
        hud_objectp = new LLHUDEffectSpiral(type);
        ((LLHUDEffectSpiral *)hud_objectp)->setDuration(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVMag(1.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVOffset(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setInitialRadius(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFinalRadius(1.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setSpinRate(10.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFlickerRate(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleBase(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleVar(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setColor(LLColor4U(255, 255, 255, 255));
        break;
    case LL_HUD_EFFECT_SPHERE:
        hud_objectp = new LLHUDEffectSpiral(type);
        ((LLHUDEffectSpiral *)hud_objectp)->setDuration(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVMag(1.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVOffset(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setInitialRadius(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFinalRadius(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setSpinRate(20.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFlickerRate(0.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleBase(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleVar(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setColor(LLColor4U(255, 255, 255, 255));
        break;
    case LL_HUD_EFFECT_SPIRAL:
        hud_objectp = new LLHUDEffectSpiral(type);
        ((LLHUDEffectSpiral *)hud_objectp)->setDuration(2.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVMag(-2.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVOffset(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setInitialRadius(1.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFinalRadius(0.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setSpinRate(10.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFlickerRate(20.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleBase(0.02f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleVar(0.02f);
        ((LLHUDEffectSpiral *)hud_objectp)->setColor(LLColor4U(255, 255, 255, 255));
        break;
    case LL_HUD_EFFECT_EDIT:
        hud_objectp = new LLHUDEffectSpiral(type);
        ((LLHUDEffectSpiral *)hud_objectp)->setDuration(2.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVMag(2.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setVOffset(-1.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setInitialRadius(1.5f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFinalRadius(1.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setSpinRate(4.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setFlickerRate(200.f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleBase(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setScaleVar(0.1f);
        ((LLHUDEffectSpiral *)hud_objectp)->setColor(LLColor4U(255, 255, 255, 255));
        break;
    case LL_HUD_EFFECT_LOOKAT:
        hud_objectp = new LLHUDEffectLookAt(type);
        break;
    case LL_HUD_EFFECT_VOICE_VISUALIZER:
        hud_objectp = new LLVoiceVisualizer(type);
        break;
    case LL_HUD_EFFECT_POINTAT:
        hud_objectp = new LLHUDEffectPointAt(type);
        break;
    case LL_HUD_EFFECT_BLOB:
        hud_objectp = new LLHUDEffectBlob(type);
        break;
    case LL_HUD_EFFECT_RESET_SKELETON:
        hud_objectp = new LLHUDEffectResetSkeleton(type);
        break;
    default:
        LL_WARNS() << "Unknown type of hud effect:" << (U32) type << LL_ENDL;
    }

    if (hud_objectp)
    {
        sHUDObjects.push_back(hud_objectp);
    }
    return hud_objectp;
}

static LLTrace::BlockTimerStatHandle FTM_HUD_UPDATE("Update Hud");

// static
void LLHUDObject::updateAll()
{
    LL_RECORD_BLOCK_TIME(FTM_HUD_UPDATE);
    LLHUDText::updateAll();
    LLHUDIcon::updateAll();
    LLHUDNameTag::updateAll();
    sortObjects();
}

// static
void LLHUDObject::renderAll()
{
    LLRender::UIScope gls_ui;

    gGL.color4f(1, 1, 1, 1);

    LLRender::DepthScope depth(false, false);

    LLHUDObject *hud_objp;

    hud_object_list_t::iterator object_it;
    for (object_it = sHUDObjects.begin(); object_it != sHUDObjects.end(); )
    {
        hud_object_list_t::iterator cur_it = object_it++;
        hud_objp = (*cur_it);
        if (hud_objp->getNumRefs() == 1)
        {
            sHUDObjects.erase(cur_it);
        }
        else if (hud_objp->isVisible())
        {
            hud_objp->render();
        }
    }

    gGL.flush();
}

// static
void LLHUDObject::renderAllForTimer()
{
    LLHUDObject *hud_objp;

    hud_object_list_t::iterator object_it;
    for (object_it = sHUDObjects.begin(); object_it != sHUDObjects.end(); )
    {
        hud_object_list_t::iterator cur_it = object_it++;
        hud_objp = (*cur_it);
        if (hud_objp->getNumRefs() == 1)
        {
            sHUDObjects.erase(cur_it);
        }
        else if (hud_objp->isVisible())
        {
            hud_objp->renderForTimer();
        }
    }
}

// static
void LLHUDObject::reshapeAll()
{
    // only hud objects that use fonts care about window size/scale changes
    LLHUDText::reshape();
    LLHUDNameTag::reshape();
}

// static
void LLHUDObject::sortObjects()
{
    sHUDObjects.sort(hud_object_further_away());
}

// static
void LLHUDObject::markViewerEffectsDead()
{
    for (LLPointer<LLHUDObject> hud_objp : sHUDObjects)
    {
        U8 type = hud_objp->getType();
        if (hud_objp->isVisible() && (type & LL_HUD_EFFECT_LOOKAT ||
            type & LL_HUD_EFFECT_POINTAT || type & LL_HUD_EFFECT_POINT ||
            type & LL_HUD_EFFECT_BEAM || type & LL_HUD_EFFECT_SPIRAL))
        {
            hud_objp->markDead();
        }
    }
}

