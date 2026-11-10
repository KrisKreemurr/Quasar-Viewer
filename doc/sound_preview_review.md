# Sound preview review follow-up (#1340 / #1311)

Original contribution history is preserved. The follow-up rearms existing previews
when the audio engine becomes absent or returns, without autoplay or an in-world
sound send. Availability is checked outside the `gAudiop` idle gate; a temporarily
refused observer dispatch retains the old identity so a later idle can retry.
An idle, ready player does not add a per-panel polling callback.

## Fixed budgets and refusal

- At most128 outstanding preview request REFERENCES, including multiple panels
  requesting the same UUID. Only successful `requestSound` calls need matching
  release. Reservation/queue allocation refusal rolls back that reference.
- At most128 retained PCM panels,64MiB shared-vector ownership charged by vector
  CAPACITY, not logical length. Shared data is conservatively charged per panel.
  PCM with an incomplete stereo frame, invalid signed backend rate or oversized
  physical reserve is refused. No copy is made merely to reduce a reserve.
- At most64 OpenAL preview clips and64MiB submitted clip PCM. Weather loops/shots
  keep their separate existing policy; this is not a total driver-memory claim.
  Failed OpenAL allocation or map publication releases newly created handles.
- At most4096 waveform columns per control, stretched over the plot width. A
  failed peak allocation drops only the waveform, not admitted PCM/playback.

Decoded-file input is bounded BEFORE allocation using the already-open file's
length, a fixed read snapshot, and a growth/truncation check. The file ceiling is
remaining retained-PCM budget (at most64MiB) plus1MiB format/header overhead.
Decoded sample allocation is checked against that same remaining budget. This
means file-read workspace can temporarily add up to65MiB; the64MiB retained ledger
is not falsely presented as whole-process peak RSS. OpenAL and general viewer/audio
state have additional allocations outside the retained PCM ledger.

Capacity/allocation refusal does not evict or redownload a healthy cached decoded
file. The player retries bounded loading at one-second intervals (five seconds for
shared-owner allocation refusal). Closing/replacing a preview releases its ledger,
clip and request; engine replacement never sends stale clip IDs to another engine.
Pointer-identity/absence notifications do not claim diagnosis of driver failures or
all same-object device-context reset semantics. No persistent settings are added.

## Validation scope

A device-free fixture extracts the ACTUAL production request/release and engine-
availability functions and uses the production bounds/file-read helpers. It covers
128 same-UUID references,128 distinct requests, balanced release, map allocation/
service/transfer refusal rollback, null/return/replacement observer delivery and
retry after dispatch refusal, physical-vector reserve accounting, signed backend
sizes/rates and bounded on-disk input refusal. It does not open an audio device.
Strict syntax checks use the actual configured main/beta component target flags,
fresh own-tree source/includes, cached package/generated headers and no PCH,
`-O1 -g0 -fsyntax-only -Werror`. These are not full viewer/platform builds or runtime
playback acceptance. Exact results and provenance are in the private owner handoff
`agents/beta6_theslowly_sound_status.md`; no GPU/viewer/server/push/tracker work.

## Availability callback allocation failures

A connected panel callback lets refreshState allocation failure propagate to the
existing dispatcher catch. That dispatcher keeps the prior observed engine identity
and retries on the next existing audio idle tick. This matters when updateControls
fails before refreshState reattaches loading idle work: swallowing it in the panel
would falsely acknowledge recovery and leave a NO_AUDIO preview dormant. Retry is
idempotent and never autoplay. Subscription-allocation failure remains a separate
degraded case with a warning: no new background polling/registration retry service
is claimed, and visibility/same-asset refresh still uses existing entry points.

The focused device-free regression extracts the actual connected lambda, refreshState,
hasWork, getState, forgetStaleEngine and dispatcher; updateControls is the injected
failure seam. It starts NO_AUDIO with no idle callback, fails the first online
refresh, then proves a later notification attaches loading idle without playing.
