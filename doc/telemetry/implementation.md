# Usage statistics implementation and review notes

Author: SkyLine. Source base: `502c4e0ba76f9495d2279271362e9bb3389f5084`.
Beta6 backport: owner candidate `403df1306324bd99b017a80204e66c87b91a185b`, copied from completed main telemetry `0a927c688a78e650dc71c4f872a4239df2dfd009` using its verified beta5 patch without conflict or adaptation. See [beta6-provenance.json](beta6-provenance.json) for the patch digest and unchanged server file hashes. G2/G3 dashboard and deployment work is separate.

Original targets: pinned main base and rel-1.5.0-beta5. The owner explicitly abandoned the 1.4 telemetry target; no further 1.4 validation or backport work is in scope. No service or website has been deployed. The owner builds the viewer.

The canonical contract is [schema_v1.json](schema_v1.json). Regenerate its embedded
C++ schema, graphics-setting descriptors and [field guide](field-guide.html) with
`python3 telemetry_server/generate_schema.py`. The server uses that same JSON
schema directly. The client additionally requires canonical serialized JSON on
disk, so duplicate keys and alternate encodings are refused before sending.
Every object rejects unknown fields, and the server also rejects duplicate keys.
All reject diagnostics use fixed codes; attacker-controlled keys, values, body,
headers and addresses are never echoed in telemetry rejection logs.

## Collected categories

- Version, closed channel/build/ISA/install enums; OS family, Windows build,
  Linux kernel and glibc numeric major/minor, allowlisted distro/desktop/session,
  distro major (0 means rolling/unknown), Flatpak.
- Conservative full CPU/GPU model grammars, with unrecognized hardware mapped to
  `other`; thread/RAM/VRAM capacity buckets. Numeric zero means unavailable.
  CPU clock/trademark suffix normalization cannot turn arbitrary appended text
  into a valid model. Swap/PSI capability uses existing governor data plus a bounded
  Linux probe. No hostname, serial, arbitrary OS build text or environment value
  is serialized.
- Display resolution class, refresh bucket, quarter-step scale, HDR, monitor count,
  window mode. An unavailable/focusless SDL display is marked unavailable.
- The complete graphics-preference allowlist from design section 2.9, each
  boolean/numeric type checked against its actual control. Absent or incompatible
  release controls are omitted and named in the closed `settings_missing` array.
  Shadow modes and DLSS network remain enums, not booleans. Features include voice,
  media autoplay, RLVa, Discord, AO, external-editor presence (never its path),
  auto-tuning, plus a main/beta/other grid class (never its label).
- Four fixed frame histograms (steady/loading/background/VR), percentiles,
  low-FPS estimates, spikes, approximate running-median spikes, steady jitter,
  stutter seconds and coarse durations. Login-screen frames are excluded; the
  first 20 seconds after observed login/teleport are loading. VR classification is
  conditionally available when that release has the existing XR API.
- Cached GPU/rig timings younger than three seconds, observed upscaler seconds,
  CPU/GPU/capped classification, VRAM usage/ratio/overhead/ladder/rung,
  RAM state/availability/RSS/PSI and allocation-failure/retry/drop/shed/reserve
  counters. The numeric bridge reads only tuner and VRAM APIs already present in
  the supported releases; no per-avatar vector or GPU timers are enabled or copied.
- Object/active-object/avatar/visible-avatar/texture counts as fixed log2 gauges
  with p50/max; texture worker/inflight/queued/decode/governor-wait/OOM counts,
  decode pending, governor waiters/inflight/budget, texture-cache deltas and HTTP
  byte deltas in texture/mesh/other groups. HTTP deltas exclude URLs and labels.
- Previous-session result, teleport counts/coarse durations, region crossings,
  disconnect counts, coarse observed login/world-ready duration and long frames.
  A recovered checkpoint preserves the actual previous-session result, including
  normal; its existence alone is not evidence of a crash.

Count gauge values are log2 bucket indices, not exact counts. GPU/frame
percentiles are histogram approximations, rounded to 0.1 ms. RAM RSS and VRAM
usage are half-GB units; overhead uses quarter-GB units; PSI uses five-percentage
point units. `bound` indices are GPU/CPU/capped. Allocation arrays follow the nine
existing governor allocation kinds; byte totals are log2 MB buckets. Unsupported
or unobserved timing measurements use zero/empty counts, never fabricated samples.

## Explicit omissions

The schema marks GPU identity detail and VR identity as unsupported. There is no
portable numeric PCI/decoded driver/display-present/XR-model accessor in this
patch. Omitted fields: PCI vendor/device, decoded driver ID/version, Vulkan
version, integrated/RT/async/device count, present mode, internal render resolution,
headset/runtime, VR missed frames/safety caps; renderer-only instance/mesh/light/
particle counts; renderer soft-fail counters and auto-tuner cap/degradation
transitions; texture stage age/stuck/stall warnings and fetch-gate queue-age/slot
limits; mesh repo/decode/LOD counters; pipeline batch/cache/cap events, swapchain
recreates, exclusive-fullscreen losses, teleport failure reasons and a global
non-default-render-setting count. There are no arbitrary textual substitutes.
The mesh repository pending-byte accessor takes a mutex and walks containers;
this patch deliberately does not call it from the sampler.

`loading`, `settings`, `events`, `os_details` and `renderer_metrics` explicitly say
`portable_subset`; `settings_missing` lists the exact absent controls. Public
survey/performance dashboards and detailed rollup exports are separate reporting
work, not part of this ingest patch. Ingest maintains daily counts and survey
counts transactionally; any future public export must enforce k >= 10. Nothing
here publishes raw data or copies website operations files.

## Consent, preview and cancellation

Only the exact current positive signed consent enables collection. Zero is never
asked; negative is declined permanently; old positive consent pauses pending a
new answer. Crash and first-run dialogs take precedence. Bench replay and second
instances are excluded. Consent answers are saved immediately; Preferences Cancel
does not restore a previous consent choice. Direct debug-setting changes also
cancel through the same epoch guard.

Opening the preview while off takes one explicit snapshot with empty performance
sections, without a queue, worker or request. All preview text is the serialized
string, never LLSD reserialization. A preview of a queued report is byte-identical
to the body handed to the POST; a live snapshot is what would be sent if frozen
now, and can change before the window actually closes. Last sent is the literal
successful body and local display day, held only in memory. Signature timestamp
and digest are generated immediately before a request, not stored as report data.

A hidden persisted `SLTelemetryPurgePending` latch is saved with opt-out and before explicit Delete queued reports. Startup restores it before activation; declined startup also schedules privacy cleanup without creating files, collecting or sending. Never-asked consent with no pending latch schedules nothing. Removal errors and failed settings writes leave the gate closed and show a fixed user-facing error. Only current-epoch confirmed removal followed by a successful settings save clears the latch; an interrupted erase or confirmation is repeated after restart. The persisted setting is local metadata, never part of the wire schema. If the initial settings save fails, the current process stays paused and the visible error requires retrying before closing; no cross-restart persistence promise can be made until that save succeeds.

Opt-out publishes cancellation before asynchronous deletion. Every worker task,
file write, completion and send compares its captured consent epoch. Fast off/on
cannot revive old work. A request already started before cancellation can finish;
its callback cannot recreate queued state or a last-sent copy while off. Queue
purging precedes new-epoch writes. At most one request is active, redirects and
cookies are disabled, TLS verification is forced on, response bodies are capped
at 4 KiB, and HTTP body tracing is suppressed even under global tracing. Only
mapped channel plus numeric version appear in the version header; no account or
UDP-listen-port headers are added.

## Budgets and shutdown

The frame path updates fixed arrays/counters without allocating or taking locks.
Whole elapsed monotonic seconds advance hourly windows, with no catch-up loop and
no client clock/time-zone value in the payload. A full window freezes until its
worker handoff succeeds. Only one worker handoff is outstanding; snapshots have
fixed size. The worker builds/validates JSON and performs all queue IO. OS probes
are bounded and cached per consent epoch; explicit preview probes are one-shot.
The main thread copies bounded existing hardware strings and typed settings at flush; string normalization and report construction run in the worker.
Empty queue polling waits at least 60 seconds, without repeating hardware probes.

The worker queue has at most eight reports, each <= 16 KiB and <= 30 days old.
Checkpoint promotion makes room before writing, including the temporary file;
recovery scans at most 64 directory entries using fixed storage. Every outgoing
or promoted file is validated again. Truncated, oversized, edited, stale and
symlinked reports are refused. The private root/queue are owner-only; files are
owner read/write. Ten attempts total are bounded by the filename counter.

Shutdown starts no network request and performs no synchronous disk wait. It
posts a final >= five-minute window if the worker slot is free; otherwise the
existing ten-minute checkpoint is the recovery fallback. This adds no telemetry
join to viewer shutdown. The existing General pool owns posted work and can
abandon work during application teardown. GUI shutdown timing and the owner's
platform builds remain runtime validation, not claims established by syntax tests.

## Backport and validation

All additions are local files plus small hooks. Release-specific patch checks
and translation-unit syntax results are recorded in the private owner status
file. No release branch is merged or edited by validation. Optional bench/XR headers remain compile-time guarded. The collector does not add new renderer APIs; numeric sampling uses existing supported-release interfaces. New code avoids Windows SDK macro identifiers.

Offline server tests: `python3 -I -m unittest discover -s telemetry_server -p test_ingest.py`.
Standalone core test: compile `indra/newview/tests/sltelemetrycore_test.cpp` with
`-std=c++17 -Wall -Wextra -Werror -pedantic -Iindra/newview` and run the result.
Standalone production queue/report tests: compile
`indra/newview/tests/sltelemetryqueue_test.cpp` plus
`indra/newview/sltelemetryreport.cpp` with `-DSL_TELEMETRY_STANDALONE`, C++17,
`-Iindra/newview`, the existing Boost headers, `-lcrypto -pthread`, then run with
`doc/telemetry/fixtures` as its argument. No test starts a viewer or listener.
Windows-macro source check: `python3 -I scripts/check/run.py --only win-macros`.

No viewer build, Windows cross-build, GUI run, GPU test, production listener,
server mutation, deployment, push or release merge has been performed. The owner
must perform platform builds and the design's explicit opt-out/runtime review
before shipping, and review proxy/tunnel logging and retention before deployment.

Repeat source syntax checks serially with the durable runner (no build):
`python3 telemetry_server/check_sources.py --build /path/to/existing/build-linux-x86_64 --logs /tmp/telemetry-source-logs`.
The existing release app TU has independently reproduced GCC 16 warnings for
`u8path`, class `memset`, and indentation. Startup has unchanged legacy
`u8path` statements producing the same GCC 16 deprecation diagnostic.
The optional `--legacy-app-warnings` flag relaxes those three classes in the app
TU and only deprecation in startup; new telemetry TUs stay strict. New telemetry TUs
retain strict `-Werror`. The baseline and exact results belong in the owner status.

Actual settings persistence fixture (requires existing built LL support libraries only, never builds a viewer): `python3 doc/telemetry/check_settings.py --build /path/to/existing/build-linux-x86_64 --output /tmp/sltelemetry-settings-test`. It compiles the branch actual `llcontrol.cpp`, verifies nonzero persisted counts/reload for consent zero/negative/positive and both pending states, and checks open, write/close and atomic-replace failures return zero.

Collection elapsed-hour minor-1 contract, server-first rollout and histogram estimate definitions: [elapsed-hour.md](elapsed-hour.md). Legacy `segment.index` is not an elapsed clock.
