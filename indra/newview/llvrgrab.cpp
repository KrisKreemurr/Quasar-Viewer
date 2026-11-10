/**
 * @file llvrgrab.cpp
 * @brief VR #1143: grab and manipulate objects with the grip. See llvrgrab.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrgrab.h"

#include "llagent.h"
#include "llbbox.h"
#include "llcleancursorpick.h"
#include "llselectmgr.h"
#include "llstartup.h"
#include "lltoolgrab.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvrcamera.h"
#include "llworld.h"
#include "message.h"
#include "rlvactions.h"
#include "vkgfx/llxrsession.h"
#include "xform.h"

#include <chrono>
#include <new>

bool LLVRGrab::sHeld = false;

namespace
{
using Clock = std::chrono::steady_clock;

constexpr F32 kMaxVolume = 1.f;                                    // m^3, the linkset's bounding box
constexpr auto kSendInterval = std::chrono::microseconds(33333);   // ~30 Hz to the sim
constexpr F32 kMoveEpsilon = 0.001f;                               // m: a change worth sending
constexpr F32 kTurnEpsilon = 0.9999999f;                           // |dot| of unit quaternions (~0.05 deg)

struct Held
{
    bool active = false;
    bool physical = false;                 // the grab path (else the edit path)
    bool offsetValid = false;              // offPos / offRot taken (the first follow after the press)
    bool selected = false;                 // the edit path selected the linkset (deselect on release)
    LLPointer<LLViewerObject> root;        // the linkset's root (what moves)
    LLPointer<LLViewerObject> prim;        // the prim the ray hit (the grab messages name it, as Ctrl+drag)
    LLPickInfo pick;                       // its surface (the grab messages' SurfaceInfo)
    LLObjectSelectionHandle sel;           // keeps the edit selection alive (LLSelectMgr::deselectUnused)
    LLVector3 offPos;                      // the root relative to the grip pose (grip frame)
    LLQuaternion offRot;
    LLVector3 grabOffset;                  // ObjectGrab's offset (root - prim, in the root's frame)
    LLVector3 targetPos;                   // this frame's root target (agent space)
    LLQuaternion targetRot;
    bool targetValid = false;
    LLVector3 sentPos;                     // as last sent
    LLQuaternion sentRot;
    bool sentAny = false;
    Clock::time_point lastSend{};
    Clock::time_point start{};
    U32 sends = 0;
    // The grip pressed: waiting for its asynchronous pick (want: not begun yet, another hand's
    // pick is in flight).
    bool want = false;
    // The edit path's last released linkset, left selected as the build tools leave it, so
    // Ctrl+Z (LLSelectMgr::undo, the sim's undo) can put it back. Deselected by the next grab.
    LLPointer<LLViewerObject> keptRoot;
    LLObjectSelectionHandle keptSel;
};
// VR-only state, made on the first VR frame and deliberately never destroyed: no static
// constructor / destructor runs LLPointer or selection-handle code outside the object list's
// lifetime, and the desktop allocates nothing.
Held* sHand = nullptr;
bool makeState()
{
    if (!sHand) sHand = new (std::nothrow) Held[LLXR_HAND_COUNT];
    return sHand != nullptr;
}
bool sPrevSqueeze[LLXR_HAND_COUNT] = {false, false};
S32 sSideHand = -1;   // the hand whose pick is in flight (LLCleanCursorPick's side pick)
U32 sRefuseLogs = 0;

const char* handName(U32 h)
{
    return h == LLXR_HAND_LEFT ? "left" : "right";
}

bool anyHeld()
{
    if (!sHand) return false;
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
        if (sHand[h].active) return true;
    return false;
}

void refuse(U32 h, const LLViewerObject* obj, const char* why)
{
    LLXrSession::instance().haptic(h, 0.12f, 0.015f);
    if (sRefuseLogs < 40 || (sRefuseLogs % 25) == 0)
    {
        LL_INFOS("VR") << "grab: " << handName(h) << " grip on " << (obj ? obj->getID().asString() : std::string("nothing"))
                       << " refused (" << why << ")" << LL_ENDL;
    }
    ++sRefuseLogs;
}

// The linkset's bounding box volume (every prim's box, in the root's frame), or -1 when someone
// sits on it (an avatar is a child of what it sits on).
F32 linksetVolume(LLViewerObject* root, U32& prims)
{
    LLBBox box(root->getPositionAgent(), root->getRotationRegion(), LLVector3::zero, LLVector3::zero);
    box.addBBoxAgent(root->getBoundingBoxAgent());
    prims = 1;
    for (LLViewerObject* child : root->getChildren())
    {
        if (!child) continue;
        if (child->isAvatar()) return -1.f;
        box.addBBoxAgent(child->getBoundingBoxAgent());
        ++prims;
    }
    const LLVector3 e = box.getExtentLocal();
    return e.mV[VX] * e.mV[VY] * e.mV[VZ];
}

// Hand h's aim ray in agent space.
bool handRay(U32 h, LLVector3& o, LLVector3& d)
{
    LLQuaternion rot;
    if (!LLVRCamera::handPose(h, /*aim=*/true, o, rot)) return false;
    d = LLVector3::x_axis * rot;
    return true;
}

// The pick for an answered side pick: that answer's own ray as the VR ray, inside a click scope
// -- the newest per-frame answer is the one asked for it, so this never waits on the GPU. The
// world pointer's ray is put back after.
bool resolvePick(const LLVector3& o, const LLVector3& d, S32 x, S32 y, LLPickInfo& out)
{
    if (!gViewerWindow) return false;
    LLVector3 keepO, keepD;
    const bool hadRay = LLCleanCursorPick::vrRay(keepO, keepD);
    LLCleanCursorPick::setVRRay(o, d);
    if (LLCleanCursorPick::vrRayOn())
    {
        LLCleanCursorPick::ClickScope scope(true);
        out = gViewerWindow->pickImmediate(x, y, /*pick_transparent=*/false, /*pick_rigged=*/false);
    }
    if (hadRay) LLCleanCursorPick::setVRRay(keepO, keepD);
    else LLCleanCursorPick::clearVRRay();
    return out.getObject().notNull();
}

// ---- The messages (standard ones only).

// The edit path: the root's position (region) and rotation, applied to the whole linked set --
// what the build tools' move / rotate send (LLSelectMgr::sendSelectionMove's packing).
void sendEditUpdate(Held& g)
{
    LLViewerRegion* region = g.root->getRegion();
    if (!region || g.root->isLocalOnly()) return;
    const U8 type = UPD_POSITION | UPD_ROTATION | UPD_LINKED_SETS;
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessage("MultipleObjectUpdate");
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, g.root->getLocalID());
    msg->addU8Fast(_PREHASH_Type, type);
    U8 data[24];
    htolememcpy(&data[0], g.root->getPosition().mV, MVT_LLVector3, 12);
    const LLVector3 packed = g.root->getRotation().packToVector3();
    htolememcpy(&data[12], packed.mV, MVT_LLQuaternion, 12);
    msg->addBinaryDataFast(_PREHASH_Data, data, 24);
    msg->sendReliable(region->getHost());
}

// The grab path: where the root should be (ObjectGrabUpdate, as Ctrl+drag) and how it should
// be turned (ObjectSpinUpdate, as Ctrl+Shift+drag), for the prim that was grabbed.
void sendGrabUpdate(Held& g, U32 dtMs)
{
    LLViewerRegion* region = g.prim->getRegion();
    if (!region || g.prim->isLocalOnly()) return;
    const LLVector3 posRegion = region->getPosRegionFromAgent(g.targetPos);
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectGrabUpdate);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addUUIDFast(_PREHASH_ObjectID, g.prim->getID());
    msg->addVector3Fast(_PREHASH_GrabOffsetInitial, g.grabOffset);
    msg->addVector3Fast(_PREHASH_GrabPosition, posRegion);
    msg->addU32Fast(_PREHASH_TimeSinceLast, dtMs);
    msg->nextBlock("SurfaceInfo");
    msg->addVector3("UVCoord", LLVector3(g.pick.mUVCoords));
    msg->addVector3("STCoord", LLVector3(g.pick.mSTCoords));
    msg->addS32Fast(_PREHASH_FaceIndex, g.pick.mObjectFace);
    msg->addVector3("Position", g.pick.mIntersection);
    msg->addVector3("Normal", g.pick.mNormal);
    msg->addVector3("Binormal", g.pick.mBinormal);
    msg->sendMessage(region->getHost());

    msg->newMessageFast(_PREHASH_ObjectSpinUpdate);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addUUIDFast(_PREHASH_ObjectID, g.prim->getID());
    msg->addQuatFast(_PREHASH_Rotation, g.targetRot);
    msg->sendMessage(region->getHost());
}

void sendSpin(Held& g, bool startSpin)
{
    LLViewerRegion* region = g.prim->getRegion();
    if (!region || g.prim->isLocalOnly()) return;
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(startSpin ? _PREHASH_ObjectSpinStart : _PREHASH_ObjectSpinStop);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addUUIDFast(_PREHASH_ObjectID, g.prim->getID());
    msg->sendMessage(region->getHost());
}

// The send gate: ~30 Hz while it changes; `force` = the exact final one.
void maybeSend(Held& g, Clock::time_point now, bool force)
{
    if (!g.targetValid) return;
    const bool moved = !g.sentAny || dist_vec(g.targetPos, g.sentPos) > kMoveEpsilon ||
                       std::fabs(dot(g.targetRot, g.sentRot)) < kTurnEpsilon;
    if (!force && (!moved || now - g.lastSend < kSendInterval)) return;
    const U32 dtMs = g.sentAny ? (U32)llclamp(std::chrono::duration_cast<std::chrono::milliseconds>(now - g.lastSend).count(),
                                              (long long)0, (long long)60000)
                               : 0u;
    if (g.physical) sendGrabUpdate(g, dtMs);
    else sendEditUpdate(g);
    g.sentPos = g.targetPos;
    g.sentRot = g.targetRot;
    g.sentAny = true;
    g.lastSend = now;
    ++g.sends;
}

// Ends hand h's grab: the exact final update (unless the object is gone), then let go.
void release(U32 h, const char* why)
{
    Held& g = sHand[h];
    if (!g.active) return;
    const bool alive = g.root.notNull() && !g.root->isDead() && g.root->getRegion() && g.prim.notNull() &&
                       !g.prim->isDead() && g.prim->getRegion();
    if (alive)
    {
        maybeSend(g, Clock::now(), /*force=*/true);
        if (g.physical)
        {
            sendSpin(g, /*startSpin=*/false);
            send_ObjectDeGrab_message(g.prim, g.pick);
        }
    }
    const F32 secs = std::chrono::duration<F32>(Clock::now() - g.start).count();
    LL_INFOS("VR") << "grab: " << handName(h) << " hand released " << (g.root.notNull() ? g.root->getID().asString() : std::string("?"))
                   << " (" << why << ") after " << secs << " s, " << g.sends << " updates ("
                   << (secs > 0.f ? (F32)g.sends / secs : 0.f) << "/s)"
                   << (alive && g.targetValid ? ", final at " : "")
                   << (alive && g.targetValid ? llformat("%.3f, %.3f, %.3f", g.targetPos.mV[VX], g.targetPos.mV[VY], g.targetPos.mV[VZ]) : std::string())
                   << LL_ENDL;
    if (alive) LLXrSession::instance().haptic(h, 0.2f, 0.02f);
    // The edit path leaves the linkset selected (its undo point), as the build tools do.
    LLPointer<LLViewerObject> keepRoot = g.keptRoot;
    LLObjectSelectionHandle keepSel = g.keptSel;
    if (alive && !g.physical && g.selected)
    {
        keepRoot = g.root;
        keepSel = g.sel;
    }
    g = Held{};
    g.keptRoot = keepRoot;
    g.keptSel = keepSel;
}

// A new grab: the linksets earlier grabs left selected are deselected (unless it is the one
// grabbed again).
void dropKeptSelections(const LLViewerObject* except)
{
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        Held& k = sHand[h];
        if (k.keptRoot.notNull() && k.keptRoot != except && !k.keptRoot->isDead() && k.keptRoot->isSelected())
            LLSelectMgr::getInstance()->deselectObjectAndFamily(k.keptRoot);
        if (k.keptRoot != except)
        {
            k.keptRoot = nullptr;
            k.keptSel = nullptr;
        }
    }
}

// Hand h's pick answered: check the object, take it.
void tryGrab(U32 h, const LLVector3& o, const LLVector3& d, S32 x, S32 y)
{
    LLPickInfo pick;
    if (!resolvePick(o, d, x, y, pick))
    {
        refuse(h, nullptr, "the ray hits no object");
        return;
    }
    LLViewerObject* prim = pick.getObject();
    if (prim->isDead() || prim->isAvatar()) return refuse(h, prim, "an avatar");
    if (prim->isAttachment() || prim->isHUDAttachment()) return refuse(h, prim, "an attachment");
    LLViewerObject* root = prim->getRootEdit();
    if (!root || root->isAvatar() || root->isAttachment()) return refuse(h, prim, "an attachment");
    if (root->isLocalOnly()) return refuse(h, prim, "a local preview");
    if (root->getPCode() != LL_PCODE_VOLUME || !root->getRegion()) return refuse(h, prim, "not a prim / mesh object");
    if (root->flagCharacter()) return refuse(h, prim, "a pathfinding character");
    if (!root->permMove() || root->isPermanentEnforced()) return refuse(h, prim, "no move permission");
    for (U32 o = 0; o < LLXR_HAND_COUNT; ++o)
        if (sHand[o].active && sHand[o].root == root) return refuse(h, prim, "held by the other hand");
    U32 prims = 0;
    const F32 volume = linksetVolume(root, prims);
    if (volume < 0.f) return refuse(h, prim, "someone sits on it");
    if (volume >= kMaxVolume) return refuse(h, prim, llformat("too big: %.2f m^3", volume).c_str());
    const bool physical = root->flagUsePhysics();
    if (RlvActions::isRlvEnabled())
    {
        if (physical ? !RlvActions::canTouch(prim, pick.mObjectOffset) : !RlvActions::canEdit(root))
            return refuse(h, prim, "RLVa restriction");
    }

    dropKeptSelections(root);
    Held& g = sHand[h];
    const LLPointer<LLViewerObject> keptRoot = g.keptRoot;
    const LLObjectSelectionHandle keptSel = g.keptSel;
    g = Held{};
    g.keptRoot = keptRoot;
    g.keptSel = keptSel;
    g.active = true;
    g.physical = physical;
    g.root = root;
    g.prim = prim;
    g.pick = pick;
    g.start = Clock::now();
    if (physical)
    {
        // Ctrl+drag's grab, from the root's centre (LLToolGrabBase::startGrab) plus its spin.
        LLVector3 off;
        off.setVec(root->getPositionGlobal() - prim->getPositionGlobal());
        LLQuaternion inv = root->getRotation();
        inv.conjQuat();
        g.grabOffset = off * inv;
        send_ObjectGrab_message(prim, pick, g.grabOffset);
        sendSpin(g, /*startSpin=*/true);
    }
    else
    {
        // The edit selection (the build tools' path); kept while held.
        g.selected = !root->isSelected();
        if (g.selected) LLSelectMgr::getInstance()->selectObjectAndFamily(root, /*add_to_end=*/true);
        if (g.selected && !root->isSelected())
        {
            g.active = false;
            g.root = nullptr;
            g.prim = nullptr;
            return refuse(h, prim, "not selectable");
        }
        if (g.keptRoot == root)
        {
            g.selected = true;   // (left selected by this hand's last grab: still ours to keep)
            g.keptRoot = nullptr;
            g.keptSel = nullptr;
        }
        g.sel = LLSelectMgr::getInstance()->getSelection();
        // The transform the build tools save at a drag's start (LLManipTranslate / Rotate).
        LLSelectMgr::getInstance()->saveSelectedObjectTransform(SELECT_ACTION_TYPE_MOVE);
    }
    LLXrSession::instance().haptic(h, 0.45f, 0.04f);
    LL_INFOS("VR") << "grab: " << handName(h) << " hand holds " << root->getID() << " (" << prims << " prims, "
                   << llformat("%.3f", volume) << " m^3, " << (physical ? "physical: grab path" : "edit path")
                   << (g.selected ? ", selected" : "") << ")" << LL_ENDL;
}

// This frame's root target for hand h from its grip pose; false = no pose this frame (it stays).
bool computeTarget(Held& g, U32 h)
{
    LLVector3 hp;
    LLQuaternion hr;
    if (!LLVRCamera::handPose(h, /*aim=*/false, hp, hr)) return false;
    if (!g.offsetValid)
    {
        // Rigid from here: root = off * hand (LL's v * q order). The root as shown now.
        LLQuaternion inv = hr;
        inv.conjQuat();
        g.offPos = (g.root->getPositionAgent() - hp) * inv;
        g.offRot = g.root->getRotationRegion() * inv;
        g.offsetValid = true;
    }
    LLVector3 pos = hp + g.offPos * hr;
    LLQuaternion rot = g.offRot * hr;
    rot.normalize();
    // As Ctrl+drag: not under the ground, not above the build ceiling, not out of the regions
    // you see.
    LLVector3d global = gAgent.getPosGlobalFromAgent(pos);
    const F32 land = LLWorld::getInstance()->resolveLandHeightGlobal(global);
    if (global.mdV[VZ] < land) global.mdV[VZ] = land;
    if (global.mdV[VZ] > MAX_OBJECT_Z) global.mdV[VZ] = MAX_OBJECT_Z;
    global = LLWorld::getInstance()->clipToVisibleRegions(g.root->getPositionGlobal(), global);
    g.targetPos = gAgent.getPosAgentFromGlobal(global);
    g.targetRot = rot;
    g.targetValid = true;
    return true;
}
} // namespace

bool LLVRGrab::wantsPick()
{
    if (!sHand) return false;
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
        if (sHand[h].want) return true;
    return sSideHand >= 0;
}

bool LLVRGrab::holding(U32 hand)
{
    return sHand && hand < LLXR_HAND_COUNT && sHand[hand].active;
}

void LLVRGrab::tick(const LLXrInputState& in, bool inWorld, const bool busy[2])
{
    if (!makeState()) return;   // (allocation failed: no grabbing; tried again next frame)
    const bool canStart = inWorld && in.focused && LLStartUp::getStartupState() == STATE_STARTED && isAgentAvatarValid() &&
                          gAgent.getRegion() != nullptr;
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        const LLXrHandState& hs = in.hand[h];
        const bool sq = in.focused && hs.active && hs.squeeze;
        const bool edge = sq && !sPrevSqueeze[h];
        sPrevSqueeze[h] = sq;
        Held& g = sHand[h];
        if (g.active)
        {
            if (!sq) release(h, in.focused ? "grip let go" : "input focus lost");
            else if (g.root.isNull() || g.root->isDead() || g.prim.isNull() || g.prim->isDead()) release(h, "the object is gone");
            else if (!inWorld) release(h, "the world is not shown");
        }
        else if ((g.want || sSideHand == (S32)h) && (!sq || !canStart))
        {
            // Let go (or the world went away) before the pick answered: cancelled.
            if (sSideHand == (S32)h)
            {
                LLCleanCursorPick::vrSidePickEnd();
                sSideHand = -1;
            }
            g.want = false;
            LL_INFOS("VR") << "grab: " << handName(h) << " grip let go before its pick answered (cancelled)" << LL_ENDL;
        }
        else if (edge && canStart && !busy[h])
        {
            g.want = true;   // (the pick starts below: asynchronous, no GPU wait)
        }
    }
    // The pick in flight: answered -> take the object; else keep its ray on the hand.
    if (sSideHand >= 0)
    {
        const U32 h = (U32)sSideHand;
        LLVector3 o, d;
        S32 x = 0, y = 0;
        const LLCleanCursorPick::SidePickState st = LLCleanCursorPick::vrSidePickPoll(o, d, x, y);
        if (st == LLCleanCursorPick::SIDE_READY)
        {
            sSideHand = -1;
            tryGrab(h, o, d, x, y);   // (picks on the answer that is newest now)
            LLCleanCursorPick::vrSidePickEnd();
        }
        else if (st == LLCleanCursorPick::SIDE_PENDING)
        {
            if (handRay(h, o, d)) LLCleanCursorPick::vrSidePickBegin(o, d, x, y);
        }
        else
        {
            sSideHand = -1;
            refuse(h, nullptr, st == LLCleanCursorPick::SIDE_EXPIRED ? "its pick got no answer in time" : "its pick was dropped");
        }
    }
    if (sSideHand < 0 && gViewerWindow && !LLCleanCursorPick::vrSidePickActive())
    {
        for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
        {
            Held& g = sHand[h];
            if (!g.want) continue;
            g.want = false;
            LLVector3 o, d;
            if (!handRay(h, o, d))
            {
                refuse(h, nullptr, "the hand is not tracked");
                continue;
            }
            LLCleanCursorPick::vrSidePickBegin(o, d, gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY());
            sSideHand = (S32)h;
            break;
        }
    }
    sHeld = anyHeld();
}

void LLVRGrab::followHeld()
{
    if (!sHand) return;
    const auto now = Clock::now();
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        Held& g = sHand[h];
        if (!g.active) continue;
        if (g.root.isNull() || g.root->isDead() || g.prim.isNull() || g.prim->isDead())
        {
            release(h, "the object is gone");
            continue;
        }
        if (!computeTarget(g, h)) continue;   // (the hand is not tracked this frame: it stays)
        if (!g.physical)
        {
            // Locally every frame (smooth in the hand); this also overrides the sim's echo of
            // an earlier update, which idle() applied before this.
            g.root->setPositionAgent(g.targetPos);
            g.root->setRotation(g.targetRot);
        }
        maybeSend(g, now, /*force=*/false);
    }
    sHeld = anyHeld();
}

void LLVRGrab::releaseAll()
{
    if (sSideHand >= 0)
    {
        LLCleanCursorPick::vrSidePickEnd();
        sSideHand = -1;
    }
    if (sHand)
        for (U32 h = 0; h < LLXR_HAND_COUNT; ++h) sHand[h].want = false;
    if (!sHeld || !sHand) return;
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h) release(h, "the session stopped");
    sPrevSqueeze[0] = sPrevSqueeze[1] = false;
    sHeld = false;
}
