/**
 * @file slsnapshotscope.h
 * @brief Snapshot avatar scope (SLSnapshotAvatarScope, #1328): which avatars a world snapshot
 *        or an avatar layer takes in. No viewer objects here (llsnapshotavatars.cpp resolves
 *        an avatar to its kind and id), so the rule is unit-tested
 *        (tests/slsnapshotscope_test.cpp).
 *
 * THE RULE.
 *  - "All": everyone in view, free-standing animesh included.
 *  - "Selected": the avatars ticked in the docked picker, and you while ticked (you are
 *    ticked to begin with; the picker lists you first). Never free-standing animesh: the
 *    picker does not list them.
 *  - "Self only": you.
 * An animesh worn by someone counts as its wearer, and what an avatar wears goes with it;
 * the caller resolves that before asking.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#ifndef SL_SLSNAPSHOTSCOPE_H
#define SL_SLSNAPSHOTSCOPE_H

#include "lluuid.h"
#include "stdtypes.h"

#include <set>

// SLSnapshotAvatarScope values.
enum ESnapshotAvatarScope
{
    SNAPSHOT_AVATARS_SELF = 0,
    SNAPSHOT_AVATARS_SELECTED = 1,
    SNAPSHOT_AVATARS_ALL = 2
};

// What the avatar being asked about is (after an animesh worn by someone was resolved to
// its wearer).
enum class SLSnapshotAvatarKind
{
    SELF,       // your own avatar
    AVATAR,     // someone else's
    ANIMESH     // a free-standing animesh
};

// The scope in force and the picker's ticks.
struct SLSnapshotScopePick
{
    U32 scope = SNAPSHOT_AVATARS_ALL;
    bool self = true;                               // "Selected": you are ticked
    const std::set<LLUUID>* selected = nullptr;     // "Selected": the other avatars ticked
};

// A setting value as a scope (out of range: "All").
U32 slSnapshotScopeClamp(U32 value);

// The scope takes this avatar in.
bool slSnapshotScopeTakes(const SLSnapshotScopePick& pick, SLSnapshotAvatarKind kind, const LLUUID& id);

#endif // SL_SLSNAPSHOTSCOPE_H
