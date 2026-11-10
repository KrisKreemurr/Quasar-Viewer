/**
 * @file slwaterevents.h
 * @brief Water contact events (#1019 / #1020): splashes, wading steps, swimming strokes, falls
 *        into the water and breaking crests, posted by whoever sees them (the water interaction
 *        and the waves) and heard through the weather / water audio (slweatheraudio.h, WATER).
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
 *
 * PRODUCERS (main thread only):
 *   - SLWaterInteraction (slwaterinteraction.h) -- the one CPU water-contact detector, the same
 *     one that drives the ripples and the splash particles:
 *       SPLASH_SMALL / SPLASH  an object or avatar hits the surface (landing, a jump in, a thrown /
 *                  dropped physical object, a fast run through the shallows' spray) or leaves it
 *                  fast -- splashKind(energy) splits them: below kSmallSplash is the small one;
 *       WADE       one wading stride (a leg pushing through the water), once per stride and leg;
 *       FALL_IN    a body falling into deep water from height (the big one);
 *       SWIM       a swimming stroke at the surface (no producer yet: the detector does not
 *                  track strokes; the audio plays them as small splashes when one appears).
 *     These carry the detector's distance cull: none farther than kMaxDistance from the camera.
 *   - the waves (SLSeaState, slseastate.h):
 *       BREAK      a breaker reaching the waterline at the shore nearest the camera, posted when
 *                  the sea state's swash clock wraps -- the moment the surface's bore front
 *                  reaches the shore line (clean_sea_state.glsl seaSurface) -- so the audio's
 *                  breaker crashes follow the breaking the surface draws. Waves off (flat water):
 *                  none; with no BREAK for a while the audio keeps its own sea-state-timed rhythm.
 *
 * CONSUMERS: two sides of the one ring.
 *   - drain(out, max): the audio, once per audio update -- the events since its last drain
 *     (oldest first); anything beyond `max` is dropped (a fixed number of voices, never a backlog).
 *   - since(seen, out, max, &missed): any other poller with its own cursor (start at lastSerial()).
 *     A poller more than kCapacity events behind loses the oldest (missed says how many).
 *
 * ENERGY is 0..1 and already folds in speed, size and how deep the contact is (a gentle wading
 * step ~0.1-0.3, a run ~0.4-0.6, a cannonball 1; a breaker: the surf x its height); scale the
 * volume / pick the variant from it. posGlobal is the contact point ON the water surface (global:
 * survives region crossings / teleports), posAgent the same in the agent frame when posted.
 * The parcel's sound permission is the consumer's to test.
 *
 * COST: a fixed ring of kCapacity events (no allocation); a post into a full ring overwrites
 * the oldest. Posting is a copy; the per-kind rate limit (kMinInterval; the user's own avatar
 * on its own clock, so a crowd never hides one's own steps) thins floods.
 */

#ifndef SL_WATEREVENTS_H
#define SL_WATEREVENTS_H

#include "stdtypes.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

class SLWaterEvents
{
public:
    enum EKind : U8
    {
        SPLASH_SMALL = 0,   // a drop, a small or slow object entering the water
        SPLASH,             // an object or an avatar hitting the water
        FALL_IN,            // a body falling or jumping into the water (the big one)
        WADE,               // a step / stride through shallow water
        SWIM,               // a swimming stroke at the surface
        BREAK,              // a breaker reaching the shore (the waves): times the breaker crashes
        KIND_COUNT
    };

    struct Event
    {
        U32        serial = 0;      // increasing from 1 (0 = none); set by post()
        EKind      kind = SPLASH;
        F32        energy = 0.5f;   // 0..1: how big (the level and the variant class follow it)
        LLVector3d posGlobal;       // where it touches the water (global metres)
        LLVector3  posAgent;        // the same, agent frame at the time of the post
        LLUUID     source;          // the avatar / object that made it (null: unknown / the waves)
        bool       self = false;    // the user's own avatar (or what it sits on): never thinned by a crowd
        F64        time = 0.0;      // LLFrameTimer::getElapsedSeconds when posted; set by post()
    };

    static constexpr U32 kCapacity = 64;
    static constexpr F32 kMaxDistance = 64.f;   // m from the camera (the detector's cull)
    static constexpr F32 kMinInterval = 0.04f;  // s between two accepted events of one kind
    static constexpr F32 kSmallSplash = 0.3f;   // energy below this: SPLASH_SMALL

    static EKind splashKind(F32 energy) { return energy < kSmallSplash ? SPLASH_SMALL : SPLASH; }

    // Producers, main thread. Cheap: a copy into the ring (or a drop by the rate limit).
    // post(e): e.posGlobal set by the caller (posAgent is derived when left zero).
    static void post(const Event& e);
    // The detector's form: a contact point in the agent frame.
    static void post(EKind kind, F32 energy, const LLVector3& posAgent, const LLUUID& source, bool self);

    // The audio: up to `max` events posted since its last drain (oldest first); returns the count.
    static U32 drain(Event* out, U32 max);
    // Any other poller: the events newer than `seen` (oldest first, at most `max`); advances `seen`.
    static U32 since(U32& seen, Event* out, U32 max, U32* missed = nullptr);
    static U32 lastSerial();
    // The newest BREAK, its time (LLFrameTimer::getElapsedSeconds; < 0: never).
    static F64 lastBreakTime();
};

#endif
