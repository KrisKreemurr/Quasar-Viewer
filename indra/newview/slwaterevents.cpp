/**
 * @file slwaterevents.cpp
 * @brief Water contact events (#1019 / #1020): see slwaterevents.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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

#include "slwaterevents.h"

#include "llagent.h"
#include "llframetimer.h"

#include <algorithm>

namespace
{
struct Ring
{
    SLWaterEvents::Event ev[SLWaterEvents::kCapacity];
    U32 serial = 0;       // the newest event's serial (0 = none yet)
    U32 drainSeen = 0;    // the audio's cursor (drain)
    F64 lastAny[SLWaterEvents::KIND_COUNT];
    F64 lastSelf[SLWaterEvents::KIND_COUNT];
    F64 lastBreak = -1.0;
    Ring()
    {
        for (U32 k = 0; k < SLWaterEvents::KIND_COUNT; ++k) lastAny[k] = lastSelf[k] = -1.0e9;
    }
};

Ring& ring()
{
    static Ring r;
    return r;
}
} // namespace

void SLWaterEvents::post(const Event& e)
{
    if (e.kind >= KIND_COUNT) return;
    Ring& r = ring();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    F64& last = e.self ? r.lastSelf[e.kind] : r.lastAny[e.kind];
    if (now - last < (F64)kMinInterval) return;   // (a flood: thinned)
    last = now;
    if (e.kind == BREAK) r.lastBreak = now;
    if (++r.serial == 0) r.serial = 1;   // (0 = none; a wrap after 4e9 events)
    Event& o = r.ev[r.serial % kCapacity];
    o = e;
    o.serial = r.serial;
    o.time = now;
    o.energy = llclamp(e.energy, 0.f, 1.f);
    if (o.posAgent.isExactlyZero()) o.posAgent = gAgent.getPosAgentFromGlobal(e.posGlobal);
}

void SLWaterEvents::post(EKind kind, F32 energy, const LLVector3& posAgent, const LLUUID& source, bool self)
{
    Event e;
    e.kind = kind;
    e.energy = energy;
    e.posAgent = posAgent;
    e.posGlobal = gAgent.getPosGlobalFromAgent(posAgent);
    e.source = source;
    e.self = self;
    post(e);
}

U32 SLWaterEvents::since(U32& seen, Event* out, U32 max, U32* missed)
{
    if (missed) *missed = 0;
    const Ring& r = ring();
    if (!out || max == 0 || seen == r.serial) return 0;
    // The oldest event still in the ring.
    const U32 oldest = r.serial >= kCapacity ? r.serial - kCapacity + 1 : 1;
    U32 first = seen + 1;
    if (seen > r.serial) first = oldest;   // (the serial wrapped: start from what is there)
    if (first < oldest)
    {
        if (missed) *missed = oldest - first;
        first = oldest;
    }
    U32 n = 0;
    for (U32 s = first; s <= r.serial && n < max; ++s)
    {
        const Event& e = r.ev[s % kCapacity];
        if (e.serial != s) continue;
        out[n++] = e;
        seen = s;
    }
    return n;
}

U32 SLWaterEvents::drain(Event* out, U32 max)
{
    Ring& r = ring();
    const U32 n = since(r.drainSeen, out, max);
    r.drainSeen = r.serial;   // (anything beyond `max` is dropped: never a backlog)
    return n;
}

U32 SLWaterEvents::lastSerial()
{
    return ring().serial;
}

F64 SLWaterEvents::lastBreakTime()
{
    return ring().lastBreak;
}
