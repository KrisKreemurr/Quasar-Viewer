/**
* @file slchatcommand.cpp
* @brief SLChatCommand implementation for chat input commands
*
* $LicenseInfo:firstyear=2013&license=viewerlgpl$
* Copyright (C) 2013 Drake Arconis
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
* $/LicenseInfo$
**/

#include "llviewerprecompiledheaders.h"

#include "slchatcommand.h"

// lib includes
#include "llcalc.h"
#include "llclipboard.h"
#include "llparcel.h"
#include "llstring.h"
#include "material_codes.h"
#include "object_flags.h"

// viewer includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llagentui.h"
#include "llavatarnamecache.h"
#include "llcommandhandler.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterreg.h"
#include "llfloaterregioninfo.h"
#include "llfloaterworldmap.h"
#include "llnotificationsutil.h"
#include "llregioninfomodel.h"
#include "llslurl.h"
#include "llstartup.h"
#include "lltoolgrab.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llvieweraudio.h"
#include "llviewermenu.h"
#include "llviewermessage.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmedia.h"
#include "llviewerparcelmediaautoplay.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerthrottle.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvolumemessage.h"
#include "llworld.h"
#include "fscommon.h"
#include "fsradar.h"
#include "fsradarentry.h"
#include "slaoengine.h"
#include "slavataractions.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <boost/algorithm/string/join.hpp>
#include <iterator>
#include <optional>

namespace
{
    //------------------------------------------------------------------------
    // Arguments
    //------------------------------------------------------------------------

    // Parses all of word as a T
    template<typename T>
    bool parseWord(const std::string& word, T& value)
    {
        std::istringstream input(word);
        return (input >> value) && input.eof();
    }

    // An avatar or object key (not the null one)
    bool parseWord(const std::string& word, LLUUID& id)
    {
        return LLUUID::validate(word) && id.set(word, false) && id.notNull();
    }

    // A chat line taken as a command: its first word, then its arguments as typed
    class CommandArgs
    {
    public:
        explicit CommandArgs(const std::string& text)
        :   mInput(text)
        {
            mInput >> mCommand;
        }

        // The command word, as typed
        const std::string& command() const { return mCommand; }

        // Reads the next word; false when there is none
        bool next(std::string& word)
        {
            return static_cast<bool>(mInput >> word);
        }

        // Reads the next word as a T (all of it); false when there is none or it isn't one
        template<typename T>
        bool next(T& value)
        {
            std::string word;
            return next(word) && parseWord(word, value);
        }

        // Reads the next word as a T if there is one; false only when it isn't one
        template<typename T>
        bool nextOptional(T& value)
        {
            return atEnd() || next(value);
        }

        // Reads a number: "64" is absolute, "+64" and "-64" are relative to base
        bool nextAbsoluteOrRelative(F64 base, F64& value)
        {
            std::string word;
            F64 number;
            if (!next(word) || !parseWord(word, number))
            {
                return false;
            }
            value = (word[0] == '+' || word[0] == '-') ? base + number : number;
            return true;
        }

        // All that is left, trimmed (newlines and all)
        std::string rest()
        {
            std::string text{ std::istreambuf_iterator<char>(mInput), std::istreambuf_iterator<char>() };
            LLStringUtil::trim(text);
            return text;
        }

        // Whether nothing but space is left
        bool atEnd()
        {
            mInput >> std::ws;
            return mInput.eof();
        }

    private:
        std::istringstream mInput;
        std::string mCommand;
    };

    //------------------------------------------------------------------------
    // Helpers
    //------------------------------------------------------------------------

    // Says something in nearby chat (as local system text) from a strings.xml entry
    void report(std::string_view name, const LLSD& args = LLSD())
    {
        FSCommon::report_to_nearby_chat(LLTrans::getString(name, args));
    }

    // Case-insensitive search for (part of) a user name among the avatars on the radar. RLVa
    // anonymises those under @shownames and empties the radar under @shownearby.
    std::shared_ptr<FSRadarEntry> findAvatarOnRadar(std::string partial_name)
    {
        LLStringUtil::toLower(partial_name);
        LLStringUtil::replaceString(partial_name, ".", " ");

        for (const auto& [avatar_id, entry] : FSRadar::getInstance()->getRadarList())
        {
            std::string user_name = entry->getUserName();
            LLStringUtil::toLower(user_name);
            if (user_name.find(partial_name) != std::string::npos)
            {
                return entry;
            }
        }
        return nullptr;
    }

    // Sets the avatar's hover height, within the Hover Height floater's range
    void applyHoverOffset(F32 offset)
    {
        gSavedPerAccountSettings.setF32("AvatarHoverOffsetZ", llclamp(offset, MIN_HOVER_Z, MAX_HOVER_Z));
    }

    F32 hoverOffset()
    {
        return gSavedPerAccountSettings.getF32("AvatarHoverOffsetZ");
    }

    // A camera view as cpcampos copies it and pstcampos and trkpos take it, agent-local:
    // "<x, y, z>", optionally followed by "|<focus x, y, z>" and then "|roll"
    struct CameraView
    {
        LLVector3 pos;
        std::optional<LLVector3> focus;
        std::optional<F32> roll;
    };

    bool parseBracketedVector(std::string text, LLVector3& value)
    {
        LLStringUtil::trim(text);
        if (text.size() < 2 || text.front() != '<' || text.back() != '>')
        {
            return false;
        }
        text = text.substr(1, text.size() - 2);
        LLStringUtil::replaceChar(text, ',', ' ');
        return LLVector3::parseVector3(text, &value);
    }

    bool parseCameraView(const std::string& text, CameraView& view)
    {
        std::vector<std::string> parts;
        std::istringstream input(text);
        for (std::string part; std::getline(input, part, '|'); )
        {
            parts.push_back(part);
        }
        if (parts.empty() || parts.size() > 3 || !parseBracketedVector(parts[0], view.pos))
        {
            return false;
        }

        if (parts.size() > 1)
        {
            LLVector3 focus;
            if (!parseBracketedVector(parts[1], focus))
            {
                return false;
            }
            view.focus = focus;
        }

        if (parts.size() > 2)
        {
            std::string roll_text = parts[2];
            LLStringUtil::trim(roll_text);
            F32 roll;
            if (!parseWord(roll_text, roll))
            {
                return false;
            }
            view.roll = roll;
        }
        return true;
    }

    std::string formatCameraView(const LLVector3& pos, const LLVector3& focus, F32 roll)
    {
        return llformat("<%.3f, %.3f, %.3f>|<%.3f, %.3f, %.3f>|%.3f",
                        pos.mV[VX], pos.mV[VY], pos.mV[VZ], focus.mV[VX], focus.mV[VY], focus.mV[VZ], roll);
    }

    void reportAvatarName(const LLUUID& id, const LLAvatarName& av_name)
    {
        const std::string name = RlvActions::canShowName(RlvActions::SNC_DEFAULT, id) ? av_name.getCompleteName()
                                                                                         : RlvStrings::getAnonym(av_name);
        FSCommon::report_to_nearby_chat(llformat("%s: (%s)", id.asString().c_str(), name.c_str()));
    }

    // The dice modifiers: what a die is checked for and what happens when it matches
    struct DiceModifier
    {
        enum EKind { NONE, BONUS, PENALTY, SUCCESS, EXPLODE, PENETRATE, REROLL };

        EKind kind = NONE;
        char compare = '=';     // a die matches when it is equal to ('='), at least ('>') or at most ('<') value
        S32 value = 0;

        bool matches(S32 roll) const
        {
            return compare == '>' ? roll >= value : (compare == '<' ? roll <= value : roll == value);
        }
    };

    // "+", "-", ">", "<", or "!", "!p" or "r" each alone or followed by ">" or "<" (any case)
    bool parseDiceModifierType(std::string type, DiceModifier& modifier)
    {
        LLStringUtil::toLower(type);
        if (type == "+" || type == "-")
        {
            modifier.kind = (type == "+") ? DiceModifier::BONUS : DiceModifier::PENALTY;
            return true;
        }
        if (type == ">" || type == "<")
        {
            modifier.kind = DiceModifier::SUCCESS;
            modifier.compare = type[0];
            return true;
        }

        std::string_view compare(type);
        if (compare.starts_with("!p"))
        {
            modifier.kind = DiceModifier::PENETRATE;
            compare.remove_prefix(2);
        }
        else if (compare.starts_with('!') || compare.starts_with('r'))
        {
            modifier.kind = compare.starts_with('!') ? DiceModifier::EXPLODE : DiceModifier::REROLL;
            compare.remove_prefix(1);
        }
        else
        {
            return false;
        }

        if (compare == ">" || compare == "<")
        {
            modifier.compare = compare[0];
        }
        return compare.empty() || compare == ">" || compare == "<";
    }

    //------------------------------------------------------------------------
    // The commands. Each takes its arguments and returns false when they aren't ones it takes
    // (missing, extra or not parsing): the line then goes out as chat.
    //------------------------------------------------------------------------

    // dd <meters>, or +/-meters for relative: the draw distance
    bool setDrawDistance(CommandArgs& args)
    {
        F64 distance;
        if (!args.nextAbsoluteOrRelative(gSavedSettings.getF32("RenderFarClip"), distance) || !args.atEnd())
        {
            return false;
        }
        const F32 draw_distance = static_cast<F32>(llclamp(distance, 16.0, 4096.0));
        gSavedSettings.setF32("RenderFarClip", draw_distance);
        gAgentCamera.mDrawDistance = draw_distance;
        return true;
    }

    // gth <z>, or +/-meters for relative: a teleport to that height
    bool teleportToHeight(CommandArgs& args)
    {
        LLVector3d pos_global = gAgent.getPositionGlobal();
        F64 z;
        if (!args.nextAbsoluteOrRelative(pos_global.mdV[VZ], z) || !args.atEnd())
        {
            return false;
        }
        pos_global.mdV[VZ] = z;
        gAgent.teleportViaLocation(pos_global);
        return true;
    }

    // flr [offset]: a teleport to the terrain under you, raised by offset (never lowered)
    bool teleportToGround(CommandArgs& args)
    {
        F64 offset = 0.0;
        if (!args.nextOptional(offset) || !args.atEnd())
        {
            return false;
        }
        LLVector3d pos_global = gAgent.getPositionGlobal();
        pos_global.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(pos_global) + llmax(offset, 0.0);
        gAgent.teleportViaLocation(pos_global);
        return true;
    }

    // pos <x> <y> [z]: a teleport to there in this region (at your height without z)
    bool teleportToPosition(CommandArgs& args)
    {
        F64 x, y;
        F64 z = gAgent.getPositionGlobal().mdV[VZ];
        if (!args.next(x) || !args.next(y) || !args.nextOptional(z) || !args.atEnd())
        {
            return false;
        }
        if (LLViewerRegion* regionp = gAgent.getRegion())
        {
            gAgent.teleportViaLocation(regionp->getPosGlobalFromRegion(LLVector3((F32)x, (F32)y, (F32)z)));
        }
        return true;
    }

    // plat [size]: rezzes a platform under you (SlipstreamChatCommandRezPlatSize wide without size)
    bool rezPlatform(CommandArgs& args)
    {
        F32 size = gSavedSettings.getF32("SlipstreamChatCommandRezPlatSize");
        if (!args.nextOptional(size) || !args.atEnd())
        {
            return false;
        }
        // Rezzing like any other rez: not while RLV forbids it.
        if (!isAgentAvatarValid() || (RlvActions::isRlvEnabled() && (!RlvActions::canRez() || RlvActions::hasBehaviour(RLV_BHVR_INTERACT))))
        {
            return true;
        }

        const LLVector3& agent_pos = gAgent.getPositionAgent();
        const LLVector3 rez_pos(agent_pos.mV[VX], agent_pos.mV[VY], agent_pos.mV[VZ] - ((gAgentAvatarp->getScale().mV[VZ] / 2.f) + 0.25f + (gAgent.getVelocity().magVec() * 0.333f)));

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectAdd);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupForRezzing());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
        msg->addU8Fast(_PREHASH_Material, LL_MCODE_STONE);
        msg->addU32Fast(_PREHASH_AddFlags, agent_pos.mV[VZ] > 4096.f ? FLAGS_CREATE_SELECTED : 0U);

        LLVolumeParams volume_params;
        volume_params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
        volume_params.setBeginAndEndS(0.f, 1.f);
        volume_params.setBeginAndEndT(0.f, 1.f);
        volume_params.setRatio(1.f, 1.f);
        volume_params.setShear(0.f, 0.f);
        LLVolumeMessage::packVolumeParams(&volume_params, msg);

        msg->addVector3Fast(_PREHASH_Scale, LLVector3(size, size, 0.25f));
        msg->addQuatFast(_PREHASH_Rotation, LLQuaternion());
        msg->addVector3Fast(_PREHASH_RayStart, rez_pos);
        msg->addVector3Fast(_PREHASH_RayEnd, rez_pos);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, true);
        msg->addU8Fast(_PREHASH_RayEndIsIntersection, false);
        msg->addU8Fast(_PREHASH_State, false);
        msg->sendReliable(gAgent.getRegionHost());
        return true;
    }

    // home: a teleport home
    bool teleportHome(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        gAgent.teleportHome();
        return true;
    }

    // sethome: makes here home
    bool setHomeLocation(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        // Asks first: a mistyped line must not move the home location.
        LLNotificationsUtil::add("ChatCommandSetHome", LLSD(), LLSD(), [](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                gAgent.setStartPosition(START_LOCATION_ID_HOME);
            }
            return false;
        });
        return true;
    }

    // calc <expression>: the result (what doesn't calculate is chat)
    bool calculate(CommandArgs& args)
    {
        std::string expression = args.rest();
        LLStringUtil::toUpper(expression);
        F32 result = 0.f;
        if (expression.empty() || !LLCalc::getInstance()->evalString(expression, result))
        {
            return false;
        }

        LLSD notify_args;
        notify_args["EXPRESSION"] = expression;
        notify_args["RESULT"] = result;
        LLNotificationsUtil::add("ChatCommandCalc", notify_args);
        return true;
    }

    // mapto <region>: a teleport to your position in that region. mapto <region>|<x> <y> <z> goes to
    // that spot instead (128, 128, 0 when the coordinates don't parse).
    bool teleportToRegion(CommandArgs& args)
    {
        std::string region = args.rest();
        const LLVector3d& pos = gAgent.getPositionGlobal();
        F32 x = fmodf(static_cast<F32>(pos.mdV[VX]), REGION_WIDTH_METERS);
        F32 y = fmodf(static_cast<F32>(pos.mdV[VY]), REGION_WIDTH_METERS);
        F32 z = static_cast<F32>(pos.mdV[VZ]);

        if (const std::string::size_type bar = region.find('|'); bar != std::string::npos)
        {
            std::istringstream coords(region.substr(bar + 1));
            if (!(coords >> x >> y >> z))
            {
                x = 128.f;
                y = 128.f;
                z = 0.f;
            }
            region.resize(bar);
            LLStringUtil::trim(region);
        }
        if (region.empty())
        {
            return false;
        }

        LLSD params;
        params.append(region);
        params.append(x);
        params.append(y);
        params.append(z);
        LLCommandDispatcher::dispatch("teleport", params, LLSD(), LLGridManager::getInstance()->getGrid(), nullptr, "clicked", true);
        return true;
    }

    // clr: clears nearby chat
    bool clearNearbyChat(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        if (LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat"))
        {
            nearby_chat->reloadMessages(true);
        }
        return true;
    }

    // regionmsg <message>: a message to everyone in the region (estate managers)
    bool sendRegionMessage(CommandArgs& args)
    {
        const std::string message = args.rest();
        if (message.empty())
        {
            return false;
        }

        std::string name;
        LLAgentUI::buildFullname(name);
        std::vector<std::string> strings(5, "-1");
        // [0] grid_x, unused here
        // [1] grid_y, unused here
        strings[2] = gAgentID.asString();   // [2] agent_id of sender
        strings[3] = name;                  // [3] sender name
        strings[4] = message;               // [4] message
        LLRegionInfoModel::sendEstateOwnerMessage(gMessageSystem, "simulatormessage", LLFloaterRegionInfo::getLastInvoice(), strings);
        return true;
    }

    // setchannel <channel>: the channel nearby chat goes to without a /channel prefix
    bool setChatChannel(CommandArgs& args)
    {
        S32 channel;
        if (!args.next(channel) || !args.atEnd())
        {
            return false;
        }
        gSavedSettings.setS32("SlipstreamNearbyChatChannel", channel);
        return true;
    }

    // tp2cam: a teleport to the camera
    bool teleportToCamera(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        gAgent.teleportViaLocation(gAgentCamera.getCameraPositionGlobal());
        return true;
    }

    // hover <height>: the hover height
    bool setHoverHeight(CommandArgs& args)
    {
        F32 height;
        if (!args.next(height) || !args.atEnd())
        {
            return false;
        }
        applyHoverOffset(height);
        return true;
    }

    // resync: restarts every avatar's animations, so they play in step again
    bool resyncAnimations(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        for (S32 i = 0; i < gObjectList.getNumObjects(); i++)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (object && object->isAvatar())
            {
                LLVOAvatar* avatarp = static_cast<LLVOAvatar*>(object);
                for (const auto& [anim_id, sequence] : avatarp->mPlayingAnimations)
                {
                    avatarp->stopMotion(anim_id, true);
                    avatarp->startMotion(anim_id);
                }
            }
        }
        return true;
    }

    // ao [subcommand]: the client AO (#496, see SLAOEngine::processChatCommand)
    bool animationOverride(CommandArgs& args)
    {
        return SLAOEngine::instance().processChatCommand(args.rest());
    }

    // tp2 <part of a name>: a teleport to that avatar on the radar
    bool teleportToAvatar(CommandArgs& args)
    {
        const std::string name = args.rest();
        if (name.empty())
        {
            return false;
        }

        if (const auto entry = findAvatarOnRadar(name))
        {
            SLAvatarActions::teleportTo(entry->getId());
        }
        else
        {
            report("ChatCommandAvatarNotFound", LLSD().with("NAME", name));
        }
        return true;
    }

    // offertp <avatar key>: offers that avatar a teleport to you
    bool offerTeleport(CommandArgs& args)
    {
        LLUUID avatar_id;
        if (!args.next(avatar_id) || !args.atEnd())
        {
            return false;
        }

        if (send_lure(avatar_id, "Join me!"))   // English on purpose: it is what the other avatar gets
        {
            const bool show_name = RlvActions::canShowName(RlvActions::SNC_TELEPORTOFFER, avatar_id);
            report("ChatCommandTeleportOffered", LLSD().with("NAME", LLSLURL("agent", avatar_id, show_name ? "inspect" : "rlvanonym").getSLURLString()));
        }
        return true;
    }

    // key2name <avatar key>: that avatar's name
    bool showAvatarName(CommandArgs& args)
    {
        LLUUID avatar_id;
        if (!args.next(avatar_id) || !args.atEnd())
        {
            return false;
        }
        LLAvatarNameCache::get(avatar_id, &reportAvatarName);
        return true;
    }

    // trkpos <x, y, z> (as cpcampos copies it) or trkpos <part of a name> (an avatar on the radar):
    // tracks that on the world map
    bool trackOnMap(CommandArgs& args)
    {
        const std::string target = args.rest();
        if (target.empty())
        {
            return false;
        }

        if (target.front() == '<')
        {
            CameraView view;
            if (!parseCameraView(target, view))
            {
                return false;
            }

            const LLViewerRegion* regionp = gAgent.getRegion();
            if (!regionp)
            {
                report("ChatCommandNoRegion");
                return true;
            }

            const S32 x = ll_round(view.pos.mV[VX]);
            const S32 y = ll_round(view.pos.mV[VY]);
            const S32 z = ll_round(view.pos.mV[VZ]);
            LLFloaterWorldMap::getInstance()->trackURL(regionp->getName(), x, y, z);

            LLSD report_args;
            report_args["POS"] = llformat("<%d, %d, %d>", x, y, z);
            report_args["REGION"] = RlvActions::canShowLocation() ? regionp->getName() : RlvStrings::getString(RlvStringKeys::Hidden::Region);
            report("ChatCommandTrackPosLocation", report_args);
            return true;
        }

        if (const auto entry = findAvatarOnRadar(target))
        {
            FSRadar::getInstance()->startTracking(entry->getId());
            report("ChatCommandTrackPosAvatar", LLSD().with("NAME", entry->getName()));
        }
        else
        {
            report("ChatCommandAvatarNotFound", LLSD().with("NAME", target));
        }
        return true;
    }

    // cpcampos: copies the camera view, for pstcampos
    bool copyCameraPosition(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        if (!gAgent.getRegion())
        {
            report("ChatCommandNoRegion");
            return true;
        }

        const std::string view = formatCameraView(gAgentCamera.getCameraPositionAgent(),
                                                  gAgent.getPosAgentFromGlobal(gAgentCamera.getCameraForwardPointGlobal()),
                                                  gAgentCamera.getRollAngle());
        const LLWString wview = utf8str_to_wstring(view);
        LLClipboard::instance().copyToClipboard(wview, 0, static_cast<S32>(wview.length()));
        report("ChatCommandCameraPositionCopied", LLSD().with("POS", view));
        return true;
    }

    // pstcampos <view, as cpcampos copies it>: puts the camera there. Without a focus it looks the
    // way it looks now; without a roll it keeps its roll.
    bool pasteCameraPosition(CommandArgs& args)
    {
        CameraView view;
        if (!parseCameraView(args.rest(), view))
        {
            return false;
        }

        const LLVector3d camera_pos = gAgent.getPosGlobalFromAgent(view.pos);
        const LLVector3d focus = view.focus ? gAgent.getPosGlobalFromAgent(*view.focus)
                                            : camera_pos + (gAgentCamera.getFocusTargetGlobal() - gAgentCamera.getCameraPositionGlobal());
        const F32 roll = view.roll.value_or(gAgentCamera.getRollAngle());
        if (gAgentCamera.restoreCameraView(camera_pos, focus, roll))
        {
            report("ChatCommandCameraPositionPasted", LLSD().with("POS", formatCameraView(view.pos, gAgent.getPosAgentFromGlobal(focus), roll)));
        }
        return true;
    }

    // bw <kbps>: the maximum bandwidth
    bool setMaxBandwidth(CommandArgs& args)
    {
        S32 kbps;
        if (!args.next(kbps) || !args.atEnd())
        {
            return false;
        }
        const F32 max_bandwidth = LLViewerThrottle::clampMaxBandwidthKbps(static_cast<F32>(kbps));
        gSavedSettings.setF32("ThrottleBandwidthKBPS", max_bandwidth);
        report("ChatCommandBandwidthSet", LLSD().with("VALUE", llformat("%.0f", max_bandwidth)));
        return true;
    }

    // media <url> <mime type>: plays that as the parcel's media (for you only)
    bool playParcelMedia(CommandArgs& args)
    {
        std::string url, media_type;
        if (!args.next(url) || !args.next(media_type) || !args.atEnd())
        {
            return false;
        }
        if (LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel())
        {
            parcel->setMediaURL(url);
            parcel->setMediaType(media_type);
            LLViewerParcelMedia::getInstance()->play(parcel);
            LLViewerParcelMediaAutoPlay::playStarted();
        }
        return true;
    }

    // music <url>: plays that audio stream
    bool playMusicStream(CommandArgs& args)
    {
        std::string url;
        if (!args.next(url) || !args.atEnd())
        {
            return false;
        }
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);
        return true;
    }

    // touch <object key>: touches that object, as a click does
    bool touchObject(CommandArgs& args)
    {
        LLUUID object_id;
        if (!args.next(object_id) || !args.atEnd())
        {
            return false;
        }

        const LLSD report_args = LLSD().with("KEY", object_id.asString());
        LLViewerObject* object = gObjectList.findObject(object_id);
        if (!object)
        {
            report("ChatCommandObjectNotFound", report_args);
        }
        else if (!RlvActions::isRlvEnabled() || RlvActions::canTouch(object))
        {
            const LLPickInfo pick;
            send_ObjectGrab_message(object, pick, LLVector3::zero);
            send_ObjectDeGrab_message(object, pick);
            report("ChatCommandTouched", report_args);
        }
        return true;
    }

    // siton <object key>: sits on that object
    bool sitOnObject(CommandArgs& args)
    {
        LLUUID object_id;
        if (!args.next(object_id) || !args.atEnd())
        {
            return false;
        }

        const LLSD report_args = LLSD().with("KEY", object_id.asString());
        if (!gObjectList.findObject(object_id))
        {
            report("ChatCommandObjectNotFound", report_args);
        }
        else if (handle_object_sit(object_id))
        {
            report("ChatCommandSat", report_args);
        }
        return true;
    }

    // standup: stands up
    bool standUp(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        if (RlvActions::canStand())     // as gAgent.standUp() checks (RLVa @unsit)
        {
            gAgent.standUp();
            report("ChatCommandStandingUp");
        }
        return true;
    }

    // zoffset <offset>, or +/-meters for relative: the hover height
    bool setHoverOffset(CommandArgs& args)
    {
        F64 offset;
        if (!args.nextAbsoluteOrRelative(hoverOffset(), offset) || !args.atEnd())
        {
            return false;
        }
        applyHoverOffset(static_cast<F32>(offset));
        return true;
    }

    // zoffset_up / zoffset_down: the hover height up or down by AvatarHoverOffsetStepSize
    bool stepHoverOffset(CommandArgs& args, F32 steps)
    {
        if (!args.atEnd())
        {
            return false;
        }
        applyHoverOffset(hoverOffset() + steps * gSavedPerAccountSettings.getF32("AvatarHoverOffsetStepSize"));
        return true;
    }

    bool raiseHoverOffset(CommandArgs& args)
    {
        return stepHoverOffset(args, 1.f);
    }

    bool lowerHoverOffset(CommandArgs& args)
    {
        return stepHoverOffset(args, -1.f);
    }

    // zoffset_reset: no hover height
    bool resetHoverOffset(CommandArgs& args)
    {
        if (!args.atEnd())
        {
            return false;
        }
        applyHoverOffset(0.f);
        return true;
    }

    // droll [faces], or rolld <dice> <faces> [<modifier> <value>]. The modifiers:
    //  + and -            add value to the total, or take it off
    //  > and <            dice at least / at most value are successes, and only they count
    //  !, !> and !<       a die equal to / at least / at most value explodes: one more die is rolled
    //  !p, !p> and !p<    a die penetrates: like exploding, but the extra die counts one less
    //  r, r> and r<       a die is rerolled: it doesn't count, and one more is rolled
    bool rollDice(CommandArgs& args)
    {
        constexpr S32 MAX_DICE = 100;
        constexpr S32 MAX_FACES = 1000;
        constexpr S32 MAX_MODIFIER = 1000;
        constexpr size_t MAX_ROLLS = 1000; // exploding dice can go on for ever ("rolld 1 6 !> 0")

        S32 dice = 1;
        S32 faces = 6;
        DiceModifier modifier;
        std::string modifier_text;          // as shown after "<dice>d<faces>": "+5", "!p>6"
        if (!args.atEnd())
        {
            S32 first;
            if (!args.next(first))
            {
                return false;
            }
            if (args.atEnd())
            {
                faces = first;
            }
            else
            {
                dice = first;
                if (!args.next(faces))
                {
                    return false;
                }
                if (!args.atEnd())
                {
                    std::string type;
                    args.next(type);
                    if (!parseDiceModifierType(type, modifier) || !args.next(modifier.value) || !args.atEnd()
                        || llabs(modifier.value) > MAX_MODIFIER)
                    {
                        LLNotificationsUtil::add("ChatCommandDiceRollModifierInvalid", LLSD().with("COMMAND", args.command()));
                        return true;
                    }
                    LLStringUtil::toLower(type);
                    modifier_text = type + llformat("%d", modifier.value);
                }
            }
        }
        if (dice < 1 || dice > MAX_DICE || faces < 1 || faces > MAX_FACES)
        {
            LLNotificationsUtil::add("ChatCommandDiceRollLimits");
            return true;
        }

        S32 total = 0;
        S32 successes = 0;
        std::vector<std::string> rolls;     // each die as shown
        bool penetrating = false;           // the die before penetrated: this one counts one less
        for (S32 left = dice; left > 0; --left)
        {
            if (rolls.size() == MAX_ROLLS)
            {
                LLNotificationsUtil::add("ChatCommandDiceRollFreezeGuard");
                return true;
            }

            S32 roll = ll_rand(faces) + 1;
            if (penetrating)
            {
                --roll;
                penetrating = false;
            }

            const char* outcome = nullptr;  // strings.xml name of what became of it
            const bool matches = modifier.matches(roll);
            switch (modifier.kind)
            {
            case DiceModifier::SUCCESS:
                if (matches)
                {
                    total += roll;
                    ++successes;
                    outcome = "ChatCommandDiceRollSuccess";
                }
                break;
            case DiceModifier::EXPLODE:
            case DiceModifier::PENETRATE:
                total += roll;
                if (matches)
                {
                    ++left;
                    penetrating = (modifier.kind == DiceModifier::PENETRATE);
                    outcome = penetrating ? "ChatCommandDiceRollPenetrated" : "ChatCommandDiceRollExploded";
                }
                break;
            case DiceModifier::REROLL:
                if (matches)
                {
                    ++left;
                    outcome = "ChatCommandDiceRollRerolled";
                }
                else
                {
                    total += roll;
                }
                break;
            default:
                total += roll;
                break;
            }
            rolls.push_back(outcome ? LLTrans::getString(outcome, LLSD().with("VALUE", roll)) : llformat("%d", roll));
        }

        if (modifier.kind == DiceModifier::BONUS)
        {
            total += modifier.value;
        }
        else if (modifier.kind == DiceModifier::PENALTY)
        {
            total -= modifier.value;
        }

        // The total, then each die when there was more than one or a modifier
        std::string result = llformat("%d", total);
        if (rolls.size() > 1 || modifier.kind != DiceModifier::NONE)
        {
            LLSD result_args;
            result_args["TOTAL"] = total;
            result_args["ROLLS"] = boost::algorithm::join(rolls, ", ");
            result_args["SUCCESSES"] = successes;
            result = LLTrans::getString(modifier.kind == DiceModifier::SUCCESS ? "ChatCommandDiceRollSuccesses" : "ChatCommandDiceRollDetails", result_args);
        }

        LLSD notify_args;
        notify_args["DICE"] = dice;
        notify_args["FACES"] = faces;
        notify_args["MODIFIER"] = modifier_text;
        notify_args["RESULT"] = result;
        LLNotificationsUtil::add("ChatCommandDiceRoll", notify_args);
        return true;
    }

    // sendmenu <channel> <button>: answers a script dialog on that channel as clicking the button does
    bool sendMenuReply(CommandArgs& args)
    {
        S32 channel;
        if (!args.next(channel))
        {
            return false;
        }
        const std::string button = args.rest();
        if (button.empty())
        {
            return false;
        }
        // RLV @sendchannel: no reply on a channel the restriction closes (the command is still
        // used up, so the text isn't sent as chat either)
        if (RlvActions::isRlvEnabled() && !RlvActions::canSendChannel(channel))
        {
            return true;
        }

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ScriptDialogReply);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_Data);
        msg->addUUIDFast(_PREHASH_ObjectID, gAgent.getID());
        msg->addS32(_PREHASH_ChatChannel, channel);
        msg->addS32Fast(_PREHASH_ButtonIndex, 0);
        msg->addStringFast(_PREHASH_ButtonLabel, button);
        gAgent.sendReliableMessage();
        return true;
    }

    //------------------------------------------------------------------------
    // Which command a word runs
    //------------------------------------------------------------------------

    using CommandHandler = bool (*)(CommandArgs& args);

    // A command whose word the user sets (Preferences > Chat > Commands)
    struct ConfigurableCommand
    {
        LLCachedControl<std::string> word;  // empty: switched off
        const char* bare_word;              // its word without a slash (case-sensitive), or nullptr
        CommandHandler run;
    };

    auto& configurableCommands()
    {
        static ConfigurableCommand commands[] =
        {
            { { gSavedSettings, "SlipstreamChatCommandDrawDistance", "/dd" },              "dd",        &setDrawDistance },
            { { gSavedSettings, "SlipstreamChatCommandHeight", "/gth" },                   "gth",       &teleportToHeight },
            { { gSavedSettings, "SlipstreamChatCommandGround", "/flr" },                   "flr",       &teleportToGround },
            { { gSavedSettings, "SlipstreamChatCommandPos", "/pos" },                      "gtp",       &teleportToPosition },
            { { gSavedSettings, "SlipstreamChatCommandRezPlat", "/plat" },                 "rezplat",   &rezPlatform },
            { { gSavedSettings, "SlipstreamChatCommandHome", "/home" },                    "tph",       &teleportHome },
            { { gSavedSettings, "SlipstreamChatCommandSetHome", "/sethome" },              nullptr,     &setHomeLocation },
            { { gSavedSettings, "SlipstreamChatCommandCalc", "/calc" },                    "calc",      &calculate },
            { { gSavedSettings, "SlipstreamChatCommandMapto", "/mapto" },                  "mapto",     &teleportToRegion },
            { { gSavedSettings, "SlipstreamChatCommandClearNearby", "/clr" },              "clrchat",   &clearNearbyChat },
            { { gSavedSettings, "SlipstreamChatCommandRegionMessage", "/regionmsg" },      nullptr,     &sendRegionMessage },
            { { gSavedSettings, "SlipstreamChatCommandSetChatChannel", "/setchannel" },    nullptr,     &setChatChannel },
            { { gSavedSettings, "SlipstreamChatCommandTeleportToCam", "/tp2cam" },         "tp2cam",    &teleportToCamera },
            { { gSavedSettings, "SlipstreamChatCommandHoverHeight", "/hover" },            nullptr,     &setHoverHeight },
            { { gSavedSettings, "SlipstreamChatCommandResyncAnim", "/resync" },            nullptr,     &resyncAnimations },
            { { gSavedSettings, "SlipstreamChatCommandAnimationOverride", "/ao" },         "cao",       &animationOverride },
            { { gSavedSettings, "SlipstreamChatCommandTeleportToName", "/tp2" },           "tp2",       &teleportToAvatar },
            { { gSavedSettings, "SlipstreamChatCommandOfferTeleport", "/offertp" },        "offertp",   &offerTeleport },
            { { gSavedSettings, "SlipstreamChatCommandKeyToName", "/key2name" },           "key2name",  &showAvatarName },
            { { gSavedSettings, "SlipstreamChatCommandTrackPos", "/trkpos" },              "trkpos",    &trackOnMap },
            { { gSavedSettings, "SlipstreamChatCommandCopyCamPos", "/cpcampos" },          "cpcampos",  &copyCameraPosition },
            { { gSavedSettings, "SlipstreamChatCommandPasteCamPos", "/pstcampos" },        "pstcampos", &pasteCameraPosition },
            { { gSavedSettings, "SlipstreamChatCommandBandwidth", "/bw" },                 "bw",        &setMaxBandwidth },
            { { gSavedSettings, "SlipstreamChatCommandMedia", "/media" },                  nullptr,     &playParcelMedia },
            { { gSavedSettings, "SlipstreamChatCommandMusic", "/music" },                  nullptr,     &playMusicStream },
            { { gSavedSettings, "SlipstreamChatCommandTouch", "/touch" },                  nullptr,     &touchObject },
            { { gSavedSettings, "SlipstreamChatCommandSitOn", "/siton" },                  nullptr,     &sitOnObject },
            { { gSavedSettings, "SlipstreamChatCommandStandUp", "/standup" },              nullptr,     &standUp },
            { { gSavedSettings, "SlipstreamChatCommandHoverOffset", "/zoffset" },          nullptr,     &setHoverOffset },
            { { gSavedSettings, "SlipstreamChatCommandHoverOffsetUp", "/zoffset_up" },     nullptr,     &raiseHoverOffset },
            { { gSavedSettings, "SlipstreamChatCommandHoverOffsetDown", "/zoffset_down" }, nullptr,     &lowerHoverOffset },
            { { gSavedSettings, "SlipstreamChatCommandHoverOffsetReset", "/zoffset_reset" }, nullptr,   &resetHoverOffset },
        };
        return commands;
    }

    // A command with a word of its own
    struct FixedCommand
    {
        const char* word;
        const char* bare_word;              // its word without a slash (case-sensitive), or nullptr
        CommandHandler run;
    };

    constexpr FixedCommand FIXED_COMMANDS[] =
    {
        { "/droll",    "rolld", &rollDice },
        { "/sendmenu", nullptr, &sendMenuReply },
    };

    // What word runs: a command's word the user set (any case), then a fixed word, then (with
    // SlipstreamChatCommandBareWords) the bare word of a command that isn't switched off.
    CommandHandler findCommand(const std::string& word)
    {
        const std::string lower_word = utf8str_tolower(word);
        for (const ConfigurableCommand& command : configurableCommands())
        {
            const std::string& command_word = command.word;
            if (!command_word.empty() && utf8str_tolower(command_word) == lower_word)
            {
                return command.run;
            }
        }
        for (const FixedCommand& command : FIXED_COMMANDS)
        {
            if (lower_word == command.word)
            {
                return command.run;
            }
        }

        static LLCachedControl<bool> bare_words(gSavedSettings, "SlipstreamChatCommandBareWords", true);
        if (bare_words)
        {
            for (const ConfigurableCommand& command : configurableCommands())
            {
                if (command.bare_word && word == command.bare_word)
                {
                    const std::string& command_word = command.word;
                    return command_word.empty() ? nullptr : command.run;
                }
            }
            for (const FixedCommand& command : FIXED_COMMANDS)
            {
                if (command.bare_word && word == command.bare_word)
                {
                    return command.run;
                }
            }
        }
        return nullptr;
    }
}

bool SLChatCommand::parseCommand(const std::string& text)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SlipstreamChatCommandEnable", true);
    if (!enabled)
    {
        return false;
    }

    CommandArgs args(text);
    const CommandHandler run = findCommand(args.command());
    return run && run(args);
}
