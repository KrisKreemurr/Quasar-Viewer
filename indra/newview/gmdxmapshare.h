/**
 * @file gmdxmapshare.h
 * @brief GunMetal-DX: pings shared with people in the same region (CombatFX).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4. Slipstream: pings only, and only
// with CombatFX on. GunMetal-DX also shares mini-map marks and zones this way; those need its mini-map
// marks (a separate feature, not ported here), so a received mark or zone line stays plain chat text
// and nothing here sends one. GMDXMapMark below mirrors the fields of its GMDXMapMark that pings use.
//
// <GMDX> Shared map items, between people in the same region only. Everything goes as one ordinary,
// readable line (gmdxmapsharetext.h: what, a name, a map link, a visible "(gmdx:...)" tail -- kept as
// is, so GunMetal-DX and Slipstream read each other's pings) that the sender's helper (slbridge.h,
// combat/mapto/1 and combat/mapsay/1) says privately to each recipient in the region (llRegionSayTo
// on channel 0, under the name "Ping from <username>"): any viewer shows it as object chat with its
// map link, Slipstream draws it. Nobody outside the region is ever sent anything, and nothing goes
// over IM, group chat, conferences or local chat. Nothing is sent unless the user asks (the ping keys,
// the ping wheel), with rate limits (pings: 1 in 2 s, 5 in 30 s; the helper's own: 6 at once, 1 a
// second), and only while sending is on (SLMapShareSend, which keeps the helper worn).
//
// Who gets them (the targets): my group, the one whose tag I wear (SLMapShareMyGroup), and the groups
// and people kept in SLMapShareTargetGroups / SLMapShareTargetPeople. Each send goes to those of them
// in my region at that moment (a group: its members here, from the group's member list), at most 50
// people. No targets here means nobody: nothing ever goes to everyone in the region.
//
// Receiving (llviewermessage's object chat hook, consumeObjectChat; CombatFX only): a well-formed ping
// said to me by an object named for a share, whose owner is an avatar in my region (and, when I can
// see the object, worn by them) counts as from that owner. When the owner is in my list or a member of
// a group I send to, and not blocked (SLMapShareBlocked, the per-sender mute) or muted, it is taken out
// of the chat and drawn over the world, on the World Map and on the mini-map for SLPingSeconds.
// Anyone else's line stays the readable chat line it is (no marker, no sound). SLMapShareShow off (the
// off switch): lines stay chat text and are not drawn. What was shared lives only while both are in
// the region: a sender's pings go when they leave it, and all of them when I do.

#pragma once

#include "gmdxmapsharetext.h"
#include "v3dmath.h"
#include "v4color.h"
#include "lluuid.h"

#include <functional>
#include <string>
#include <vector>

class LLMenuGL;
class LLWorldMapView;

// What a ping draws: the position, name and colour (GunMetal-DX's GMDXMapMark, minimal).
struct GMDXMapMark
{
    LLUUID id;
    std::string name;
    std::string region;     // the region's name
    LLVector3d pos_global;
    S32 color = 0;          // index into GMDXMapShare::markColor()
};

struct GMDXSharedItem
{
    LLUUID id;
    GMDXShareText::Type type = GMDXShareText::Type::Ping;
    LLUUID sender;
    std::string sender_name;    // from the helper's name, cleaned: shown until the name cache has the avatar
    GMDXMapMark mark;           // pings: the position, name and colour
    S32 ping = 0;               // 0 ping, 1 enemy, 2 rally
    F64 arrived = 0.0;          // LLFrameTimer::getTotalSeconds
    F64 expires = 0.0;          // pings
    LLUUID anchor;              // pings: the object / avatar it is on, followed while this viewer has it
    std::string on;             // pings: that one's name as sent ("" unknown)
    LLVector3 anchor_off;       // pings: the point picked on it, in its own frame (zero: its origin)
    bool map_only = false;      // pings placed on a map: on maps only, never over the world
};

class GMDXMapShare
{
public:
    // After login (llstartup): the helper while CombatFX and sending are on, the targets kept from
    // before, the watch on my region.
    static void init();

    // Where my pings go, besides my group (SLMapShareMyGroup).
    struct Target
    {
        enum Kind { GROUP, PERSON } kind = GROUP;
        LLUUID id;
        bool on = true;     // ticked: gets my pings
        bool kept = false;  // stays for the next login
        F64 gone_since = 0.0;   // a person: when they were last seen leaving my region (0 = here)
    };
    static const std::vector<Target>& targets();
    static void addTarget(Target::Kind kind, const LLUUID& id);   // for this session, ticked
    static void removeTarget(LLUUID id);
    static void setTargetOn(const LLUUID& id, bool on);
    static void setTargetKept(const LLUUID& id, bool kept);
    static std::string targetName(const Target& t);

    // Ping a place (the ping keys: under the cursor / at the crosshair; a ping that lands on an
    // object or avatar is on it, at the picked point, and follows it on receivers' maps), to the
    // targets in my region. A ping placed on a map (ping) shows on maps only, for me and for them;
    // one from the world (pingAtCursor) also over the world.
    static void ping(const LLVector3d& pos_global, S32 kind);
    static void pingAtCursor(S32 kind);
    // pingAtCursor in two steps (the ping wheel: the spot when its key goes down, the kind when it
    // comes up): where (false: nothing there, and a tip says so), then send to it.
    struct PingTarget
    {
        LLVector3d pos;
        LLUUID on;          // the object / avatar it landed on (null: none)
        LLVector3 off;      // the spot on it, in its own frame
    };
    static bool pingTarget(PingTarget& target);
    static void pingTo(const PingTarget& target, S32 kind);
    static LLColor4 pingColor(S32 kind);   // ping yellow, enemy red, rally green
    static LLColor4 markColor(S32 index);  // GunMetal-DX's mini-map mark palette

    // An object's line said to me alone (llRegionSayTo, chat type DIRECT): true = a ping from
    // someone's helper, taken out of the chat (and drawn). False with CombatFX off.
    static bool consumeObjectChat(const LLUUID& object_id, const LLUUID& owner_id, const std::string& object_name,
                                  const std::string& text);

    // What others shared (expired pings gone), drawn by the maps.
    static const std::vector<GMDXSharedItem>& items();
    static const GMDXSharedItem* find(const LLUUID& id);
    static std::string senderName(const GMDXSharedItem& item);
    static LLColor4 senderColor(const GMDXSharedItem& item);   // light grey (GunMetal-DX: the group colour)
    static std::string anchorName(const GMDXSharedItem& item); // what a ping is on, as I may see it ("" none)
    static void remove(LLUUID id);              // (by value: callers pass ids from the items)
    static void removeFrom(LLUUID sender);
    // SLMapShareBlocked (the per-sender mute): their pings gone, and from now on nothing of theirs
    // is drawn (their lines stay plain chat)
    static void block(LLUUID sender);
    static void unblockAll();
    static S32 blockedCount();
    static void clear();

    // Pings over the world (LLHUDView::draw), on the World Map (LLWorldMapView::draw) and on the
    // mini-map (LLNetMap::draw).
    static void drawWorld();
    static void drawWorldMap(LLWorldMapView* view);
    static void drawMiniMap(const std::function<bool(const LLVector3d&, F32&, F32&)>& to_map);
    // A ping's radar blip at (x, y) (a flash and three rings sonar-style out to max_radius px on
    // arrival), and how faded its dot is now (on a map: over its whole life; else over its last 1.5 s).
    static void drawBlip(F32 x, F32 y, const GMDXSharedItem& item, F32 max_radius);
    static F32 pingAlpha(const GMDXSharedItem& item, bool on_map);
};
