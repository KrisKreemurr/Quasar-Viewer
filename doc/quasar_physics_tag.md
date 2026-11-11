# Quasar physics tag: the `[qlphys ...]` tag

Quasar Viewer dents breasts, butts and bellies when an arm presses into them, and lets them
spring back ("Soft breasts, butt and belly" in Advanced Graphics Preferences > Avatars >
Avatar motion (Quasar)). By default every avatar gets the same viewer settings, tuned from
their body's measured size and their own Physics layer. The physics tag lets the person
wearing a body choose how it feels to everyone on Quasar Viewer.

Nothing is added on the server and nothing is sent anywhere. The tag is ordinary hover text
on an object the avatar wears. It uses the same convention as Slipstream's speaker tags
(`doc/parcel_speakers_protocol.md`). Other viewers are not affected: they show the tag as a
faint line of text and nothing else changes.

## 1. Wearing a tag

Put `indra/newview/app_settings/quasar/quasar_physics_tag.lsl` (shipped with the viewer in
`app_settings/quasar`) in a small prim and **wear** it on a body attachment point. Not a HUD
point: HUD objects are only sent to their wearer, so no one else would receive the tag.
Touch it for a menu, or use chat channel 7 (`/7 soft 0.6`, `/7 off`, `/7 show`, ...).

The script writes the tag as faintly visible hover text, because the simulator does not send
near-invisible hover text:

```lsl
llSetText("[qlphys soft=0.6 wobble=1.4]", <1.0, 1.0, 1.0>, 0.1);
```

The text stays on the prim after the script is removed and after a take and re-rez.

## 2. Format

- The tag starts with `[qlphys`, in any case, followed by a space or `]`. It ends at the
  next `]`. Text before and after it is ignored, and only the first tag counts.
- It is made of space-separated `key=value` tokens and bare words. Numbers use `.` as the
  decimal separator, with at most 12 characters.
- Out-of-range values are clamped. A malformed value drops that one token and the rest
  still applies.
- Unknown keys are ignored, so later versions can add keys without breaking older viewers.

| key | value | meaning | when absent |
|---|---|---|---|
| `soft` | 0..1 | how much every soft part gives way under an arm (0 = the arm is pushed out, 1 = the part takes the whole press) | the viewer's Softness slider |
| `breast`, `butt`, `belly` | 0..1 | the same for one part, in place of `soft` | `soft` |
| `wobble` | 0..2 | wobble when a press lets go (0 = none, 1 = the avatar's own Physics layer, 2 = twice as lively) | the viewer's Wobble slider |
| `off` | (bare word) | no dents on this avatar at all | |

The shortest tag is `[qlphys]`: it is present, with every value left to the viewer.

## 3. Trust

Only objects the avatar itself wears are read: its attachments and their linked prims,
except HUDs. So nobody can set someone else's tag. Values only adjust soft contact within
the ranges above, so a hostile tag can at most turn one avatar's dents off or make them as
soft or wobbly as the ranges allow.

## 4. Viewer side (for developers)

- `LLVOAvatar::qlReadPhysTag()` scans the avatar's worn objects' `mHudText` every 2 s
  (only for avatars soft contact runs on). `qlParsePhysTag()` parses the tag and
  `qlStripPhysTag()` removes it from the drawn text. `LLViewerObject` passes hover text
  through it before `SLParcelSpeakers::displayText()`, so the tag is never drawn in
  Quasar Viewer.
- `QuasarAvatarTags` ("Follow people's own Quasar physics tags", **default off for now**:
  the default physics comes first): when it is off, tags are ignored and everyone gets this
  viewer's sliders. The tag is still never drawn.
- Tag values replace `QuasarSoftContactGive` and `QuasarSoftContactWobble` for that
  avatar. `QuasarSoftContactNoPhysics` still decides what a part with no Physics layer
  does.

## 5. Policy note

This is a feature only Quasar Viewer users can see and set, which is the kind of thing
Linden Lab's Third-Party Viewer Policy section 2.k ("shared experience") is about. It is
opt-in, carries no personal data, and sends nothing off the grid. Review it before any
public release or Viewer Directory listing.
