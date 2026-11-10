/**
 * @file slsnapshotscope_test.cpp
 * @brief Snapshot avatar scope (#1328): who "All", "Selected" and "Self only" take in --
 *        you only while ticked in "Selected", the ticked avatars, free-standing animesh only
 *        under "All".
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../slsnapshotscope.h"

#include "../test/lltut.h"

namespace
{
    LLUUID avatar(U8 n)
    {
        LLUUID id;
        id.mData[0] = n;
        id.mData[15] = 0x6b;
        return id;
    }

    SLSnapshotScopePick pick(U32 scope, bool self, const std::set<LLUUID>& selected)
    {
        SLSnapshotScopePick p;
        p.scope = scope;
        p.self = self;
        p.selected = &selected;
        return p;
    }
}

namespace tut
{
    struct snapshotscope_data {};
    typedef test_group<snapshotscope_data> snapshotscope_test;
    typedef snapshotscope_test::object snapshotscope_object;
    tut::snapshotscope_test snapshotscope("SLSnapshotScope");

    // "All": everyone, animesh included, whatever is ticked.
    template<> template<>
    void snapshotscope_object::test<1>()
    {
        const std::set<LLUUID> none;
        const SLSnapshotScopePick p = pick(SNAPSHOT_AVATARS_ALL, false, none);
        ensure("all: you", slSnapshotScopeTakes(p, SLSnapshotAvatarKind::SELF, avatar(1)));
        ensure("all: an avatar nobody ticked", slSnapshotScopeTakes(p, SLSnapshotAvatarKind::AVATAR, avatar(2)));
        ensure("all: free-standing animesh", slSnapshotScopeTakes(p, SLSnapshotAvatarKind::ANIMESH, avatar(3)));
    }

    // "Self only": you, nobody else, no animesh -- even the avatars ticked for "Selected".
    template<> template<>
    void snapshotscope_object::test<2>()
    {
        const std::set<LLUUID> ticked = { avatar(2), avatar(3) };
        const SLSnapshotScopePick p = pick(SNAPSHOT_AVATARS_SELF, false, ticked);
        ensure("self only: you, even unticked in Selected", slSnapshotScopeTakes(p, SLSnapshotAvatarKind::SELF, avatar(1)));
        ensure("self only: a ticked avatar is out", !slSnapshotScopeTakes(p, SLSnapshotAvatarKind::AVATAR, avatar(2)));
        ensure("self only: animesh is out", !slSnapshotScopeTakes(p, SLSnapshotAvatarKind::ANIMESH, avatar(3)));
    }

    // "Selected": the ticked avatars, and you while ticked.
    template<> template<>
    void snapshotscope_object::test<3>()
    {
        const std::set<LLUUID> ticked = { avatar(2) };
        const SLSnapshotScopePick with_me = pick(SNAPSHOT_AVATARS_SELECTED, true, ticked);
        ensure("selected: you, ticked", slSnapshotScopeTakes(with_me, SLSnapshotAvatarKind::SELF, avatar(1)));
        ensure("selected: a ticked avatar", slSnapshotScopeTakes(with_me, SLSnapshotAvatarKind::AVATAR, avatar(2)));
        ensure("selected: an unticked avatar is out", !slSnapshotScopeTakes(with_me, SLSnapshotAvatarKind::AVATAR, avatar(4)));

        const SLSnapshotScopePick without_me = pick(SNAPSHOT_AVATARS_SELECTED, false, ticked);
        ensure("selected: you, unticked, are out", !slSnapshotScopeTakes(without_me, SLSnapshotAvatarKind::SELF, avatar(1)));
        ensure("selected: the ticked avatar stays", slSnapshotScopeTakes(without_me, SLSnapshotAvatarKind::AVATAR, avatar(2)));
    }

    // "Selected" never takes free-standing animesh: the picker does not list them, and an id
    // that happens to be in the ticked set does not bring one in.
    template<> template<>
    void snapshotscope_object::test<4>()
    {
        const std::set<LLUUID> ticked = { avatar(3) };
        const SLSnapshotScopePick p = pick(SNAPSHOT_AVATARS_SELECTED, true, ticked);
        ensure("selected: animesh is out", !slSnapshotScopeTakes(p, SLSnapshotAvatarKind::ANIMESH, avatar(3)));
    }

    // Defensive: no ticked set (a caller that has none) and a scope value out of range.
    template<> template<>
    void snapshotscope_object::test<5>()
    {
        SLSnapshotScopePick p;
        p.scope = SNAPSHOT_AVATARS_SELECTED;
        p.self = true;
        p.selected = nullptr;
        ensure("selected, no set: you", slSnapshotScopeTakes(p, SLSnapshotAvatarKind::SELF, avatar(1)));
        ensure("selected, no set: nobody else", !slSnapshotScopeTakes(p, SLSnapshotAvatarKind::AVATAR, avatar(2)));

        const std::set<LLUUID> none;
        const SLSnapshotScopePick wild = pick(7, false, none);
        ensure("out-of-range scope reads as All", slSnapshotScopeTakes(wild, SLSnapshotAvatarKind::AVATAR, avatar(2)));
        ensure_equals("clamp", slSnapshotScopeClamp(7), (U32)SNAPSHOT_AVATARS_ALL);
        ensure_equals("clamp keeps Selected", slSnapshotScopeClamp(SNAPSHOT_AVATARS_SELECTED), (U32)SNAPSHOT_AVATARS_SELECTED);
    }
}
