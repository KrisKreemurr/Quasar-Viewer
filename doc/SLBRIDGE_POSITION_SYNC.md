# Avatar position sync over the Slipstream Helper (#1417)

Each Slipstream user shares the position their own viewer draws their avatar at. Other
Slipstream viewers nearby draw that avatar there, instead of where their own reading of the
sim's updates puts it. Code: `indra/newview/slpossync.{h,cpp}` (feature),
`slpossyncmath.h` (pure rules, tested by `scripts/check/test_pos_sync.cpp`), the helper's
`posSend` / `posSee` / `posHeard` in `app_settings/slbridge/slipstream_helper.lsl`, and the
apply point in `LLVOAvatar::updateRootPositionAndRotation`. Check: `pos-sync`
(`scripts/check/checks/pos_sync.py`, stage `source`).

## Why it cannot be used to cheat

Two rules, enforced by the receiver, make it safe in combat:

1. **Visual only.** A shared position moves only the receiver's drawn root joint (so the
   skeleton and the attachments). The avatar's drawable, its sim-driven position, picking,
   sits, collisions and every camera keep the sim's position. The sim never sees any of
   this traffic as position: it is chat between two scripts. Combat 2.0 hits, collisions,
   sit targets and camera constraints are all decided on the sim against the sim's
   position, so hit boxes stay where the sim has them.
2. **2 m bound.** A receiver uses a shared position only while it is within 2 m of where the
   sim's updates put that avatar (`SLPosSyncMath::BOUND`). It checks this itself, on every
   line and again every frame, because a modified helper or viewer can send anything. Over
   2 m, the update is rejected and the avatar eases back to the sim's placement. The drawn
   offset is clamped to 2 m even while blending. The sender's helper also clamps against
   `llGetPos()` before speaking, as a first line of defence.

Together these mean a sender can at most look up to 2 m away from where it really is. It
can't be somewhere else, and it can't move anyone else. That is the same order of error as
ordinary extrapolation under lag, and it never changes where it can be hit.

**Identity** comes from the sim only. The receiving helper attributes a line to
`llGetOwnerKey()` of the attachment that spoke (rezzed objects and avatars chatting on the
channel are not attachments, and are ignored). The line itself names nobody, and a line
with an extra field, such as a smuggled key, is rejected whole (`parseFields`). Nobody can
send a position for anyone but themselves. Muted avatars are ignored.

**Privacy.** Sharing is the option *Preferences > Privacy > General > "Share my precise
position with nearby Slipstream users"* (`SLPosSyncShare`). It is persisted and on by
default. When the option is off, or while RLV hides location, names or nearby avatars
(`@showloc`, `@shownames`, `@shownametags`, `@shownearby`, `@showminimap`, `@showworldmap`, or
a block on the channel), the viewer sends nothing and tells the helper `pos/share/1|0`. The
helper then says nothing for us either: it starts with sharing off and only shares after
the viewer says `1`. Receiving is always on.

## Messages

| Direction | Line |
| --- | --- |
| viewer -> own helper (private channel) | `pos/share/1\|<0\|1>` (control: held until ready, never dropped) |
| viewer -> own helper | `pos/send/1\|<seq>\|<x,y,z>\|<vx,vy,vz>\|<qx,qy,qz,qs>\|<flags>` (superseding slot) |
| viewer -> own helper | `pos/see/1\|<key>,<key>,...` (at most 24 keys, nearest the camera first; superseding slot) |
| helper -> region, `POS_CHANNEL` -1397510224 ("SLPP"), `llShout` (100 m) | `pos/1\|<seq>\|<pos>\|<vel>\|<rot>\|<flags>` |
| helper -> own viewer (`llOwnerSay`) | `#slb:pos/1\|<owner from the sim>\|<seq>\|<pos>\|<vel>\|<rot>\|<flags>` |

- `pos` is the region-local *drawn* position (`getRenderPosition()`). The sender's helper
  re-states it with `(string)vector`, which is 5 decimals, so it is millimetre precision.
- `rot` is the avatar's root rotation.
- `flags` is any of `S` (sitting), `T` (still), `F` (flying).
- `seq` is the sender's U16 counter. It wraps. Repeated and older lines are dropped, and a
  counter that restarts is accepted after 10 s of quiet.
- There is no shared clock. Receivers age a line by its receive time and extrapolate a
  moving line by its velocity for at most 0.5 s.

The type names follow the helper's existing `combat/<name>/<version>` style and the planned
v1 envelope `#slb:<type>/<ver>|<fields>` (#1213-#1216). The viewer registers its handler
under `"pos/1"`, so it moves to `registerHandler(type, version, ...)` as-is when #1215 lands.
The new public channel means helpers without position sync never hear (or pay for) it. A
non-Slipstream viewer ignores the channel. A Slipstream viewer with an older helper simply
doesn't send. The helper version hash makes every worn helper update.

Numbers are written with `formatFixed` and read with `std::from_chars`, so a user locale with
a decimal comma can't change them.

**What this version does not correct:**
- **Seated avatars.** They send `S`, and receivers keep the seat's placement. Drawing an
  avatar off a seat that the receiver itself extrapolates (vehicles) would separate it from
  the seat. Sending seat-relative offsets is a possible follow-up.
- **Hover height.** Both sides compare the drawn (drawable) position, and hover applies
  further down, at the root.

## Measuring

Every 20 s while lines arrive, the receiver logs a `PosSync` line with three figures:
- lines used and rejected;
- **gap** (mean / max): the distance between the sim-driven placement and the sender's
  shared position, extrapolated to now. This is the desync without the feature;
- **residual** (mean / max): the distance from the drawn position to that shared position
  after the blend.

The debug tag `PosSync` adds one line per received update: the sender, seq, flags, and the
gap on arrival.

## Rates and budgets

**Sender (viewer):**
- One line when the drawn position is more than 5 cm off where the last line predicts, the
  velocity changed by more than 0.2 m/s, the avatar turned more than 5°, or the
  sit/still/fly state changed.
- At most 4 lines a second.
- Keep-alive: every 1 s while moving, every 10 s while still or seated.

**Sender (helper):**
- Its own thresholds, slightly finer: 4 cm, 0.15 m/s, 4°, keep-alives 0.9 s / 9.5 s.
- At most one line per 0.2 s.
- Clamped to 2 m of `llGetPos()`.

**Receiver (helper):**
- Forwards only listed avatars: at most 24, and at most one line per avatar per 0.15 s.
- At most 30 lines a second in total (burst 45), in its own buckets, separate from the
  pose/consent budgets.
- Each line must also be within 2.5 m of the speaker's `OBJECT_POS` (same region only).

**Receiver (viewer):**
- `pos/1` lines have their own route lane: 30/s, burst 45.
- Per-avatar table: at most 32 avatars. While it is full, a newcomer waits until an entry
  expires.
- Entries expire after 3 s without a line while moving, or 25 s while still. A "still"
  entry also goes stale as soon as the sim moves the avatar at more than 0.5 m/s.
- Blend: ease in with τ = 0.12 s, and ease back to sim placement with τ = 0.3 s. Never a
  snap, except on sitting down, where the sit itself repositions.
- Per-frame cost: one branch per avatar, plus `View::step` (a few dozen flops) for followed
  avatars only.

**Bridge commands:**
- `pos/send/1` and `pos/see/1` use superseding slots (`SLBridge::sendLatest`): a newer line
  replaces a pending one.
- The slots take turns with the pose data queue inside the existing 5 commands/s pacing,
  after control lines.

### Volume and script time (estimates; measure in-world)

**Line size:** about 115 bytes on `POS_CHANNEL`, and about 100 bytes viewer -> helper.

**Lines per sender:**

| State | Lines a second | Bytes a second |
| --- | --- | --- |
| Still or seated | 0.1 | ~12 |
| Walking straight | about 1 (keep-alives) | ~115 |
| Turning, stopping, jumping | up to 4 | ~460 (peak) |

**Script time per helper (Mono):**
- Own line: about 60-100 µs per line sent, so about 0.1 ms/s typical and 0.4 ms/s peak.
- Each heard line not in the visible list: about 20 µs (dropped after one list lookup).
- Each forwarded line: about 100-150 µs (parse, two casts, `llGetObjectDetails`,
  `llOwnerSay`).
- With K moving Slipstream users in view at about 1.5 lines/s each: about 0.2·K ms of script
  time a second. For K = 10, that is about 2 ms/s, or about 0.04 ms per sim frame at 45 fps.
- The receiving cap of 30 lines/s bounds it at about 4.5 ms/s, whatever the crowd.
