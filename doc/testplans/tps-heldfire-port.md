# TPS held-fire port contract (#1094 / L3)

This is a narrow port of the existing main implementation, not a new firing
algorithm. Beta base: `0b0ab7ccdac17a8501577862f741b47f74e818b4`.
Reference main: `aa7609acca09354ed06c7654c74175ccacb10e4d`.

Apply original commits in dependency order: 657d9987a6, 54f9a27c93,
f35d480d8d, 6c3bec42e6, c4d9764b5a, a7bb16d0f3, ae5a243897.
Keep original authors and cherry-pick provenance. Do not port shoulder framing,
preferences suspension or additional CombatFX features. Preserve beta's existing
403df13063 right-button release consumption and seated-camera policy.

## Packet contract, written before production edits

- An explicit physical press selects ML_LBUTTON and also ordinary LBUTTON if
  its existing script subscription (including pass_on) requires it. Latch that
  family until its release; do not re-read subscriptions for an old gesture.
- Send the press edge once. Subsequent held packets carry scoped wire mouselook
  context, not repeated DOWN edges. Local camera mode and persistent local
  mouselook flag must remain unchanged. A matching release has the same wire
  context; the following genuine baseline drops that injected context.
- A fixed 64-edge FIFO preserves quick press/release and release/repress across
  ephemeral flag resets and skipped packets. Consume an edge only after matching
  packed intent is submitted to transport. This is not simulator acknowledgement;
  do not add retransmission loops or a new AgentUpdate cadence.
- Bound pathological overflow by releasing the last submitted hold and retaining
  final physical state. An ineligible owner cancels unsent presses and releases
  transmitted state: focus/capture loss, modifiers, RLV restrictions, teleport or
  camera disengagement. Preserve a subsequent genuine native mouselook gesture.
- Manual seated TPS remains subject to existing camera policy; physical release
  must drain it. A seat/follow-camera takeover must cancel ownership. Do not add
  a new vehicle/input policy or claim a simulator result from an offline test.

## Actual-source fixture and limits

`indra/newview/tests/tps_heldfire_contract.py` extracts actual production owner,
queue, flag/reset and packet selection code into a small C++ fixture. It stubs
viewer eligibility and transport, never starts a viewer or network listener.
The fixture must assert final main behavior (including neutral held edges), not
the superseded repeated-DOWN design. Run serially only after parent CPU token.

Coverage: subscriptions/pass_on, ordered quick click/repress, actual reset and
forced packet snapshots, no consumption on skipped send, cancellation causes,
overflow, seated/manual release, native handoff, absent singleton, and unchanged
right-release source guard. Syntax-check actual changed viewer TUs separately
with existing compile flags under the same token; no viewer build/link/GPU.

Offline packing proves submitted intent only. UDP delivery, simulator script
level/edge interpretation, script-visible AGENT_MOUSELOOK and weapon/vehicle/AO
compatibility require owner runtime testing. Wire mouselook is an intentional
existing-main compatibility behavior, not a claim of no server-side effects.

## Port validation receipt

Production tip: `85f2f4456b94019cde8fbfb21f9880b8fabe7020`.
All seven cherry-picks applied cleanly, without extra production dependencies.
Net production changes affect only llagent.cpp/.h, llviewermessage.cpp and
sltpscamera.cpp/.h. llappviewer and llviewerwindow have no net changes.

Under the parent's exclusive CPU grant, the actual-source fixture passed against
both the main reference and beta candidate. Actual beta sltpscamera.cpp and
llagent.cpp passed serial C++20 `-O1 -fsyntax-only -Werror` checks using existing
Ninja compile flags, beta source includes and no PCH. llviewermessage.cpp's strict
check failed solely on the existing process_script_question unused local
`has_not_only_debit`. The exact base TU reproduced that same diagnostic with
the candidate headers. A second candidate check using
`-Wno-error=unused-variable` completed with that warning alone; other diagnostics
remained errors. This is two strict passes and one baseline-warning-qualified
pass, not three strict passes. No unrelated source fix was made.

The reusable syntax checker deliberately retains strict behavior, including the
known baseline failure. No viewer build, link, GPU or simulator test was run.
The CPU slot was released after all compiler processes exited.
