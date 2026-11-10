# Circuit consumers: READY bundle; cache request retention HELD

Session metadata: thread `01a19cf9-154c-7751-92bd-f8bf72a3939f`, `gpt-6.1-sol`, effort high. Base `82fa4fb875`; owned worktree `agents/worktrees/sol-circuit-consumers`. Read batch2 `doc/CODING_RULES.md`. No rel15 writes, viewer build, push, or GPU execution.

Cherry-pick **9bc8d22951**, then **the consumer bundle commit containing this report**. Skip **4f67e17794**. That optional object-request retention commit is HELD: duplicate and distinct miss IDs have no demonstrated queue bound during prolonged admission failure; the cache-entry cap does not bound the miss list. No runtime evidence establishes that its source defect caused the historical cold-arrival stall.

## Root evidence

Preserved log `agents/validation/owner-cold-arrival/Slipstream-20261102T155435Z.log`: line2320 starts transport before first circuit at15:51:12; lines3223–7863 report connected regions with `pkt=no-circuit` and root silence at15:51:19–15:54:32. At base `message.cpp:563` requires the legacy table empty for owner handoff, but `llworld.cpp:2379` unconditionally queries it. `mSincePacket=-1` feeds the false global-stall guard. Existing `getCircuitSnapshot` already publishes coherent values; the fix uses its steady-clock packet/birth age and distinguishes unavailable from absent.

## Consumer disposition

Locations below refer to base82fa unless described otherwise. No worker circuit pointer or map iterator is exposed.

| File / consumer | Status under owner |
| --- | --- |
| `llworld.cpp:2052` ensureCrossingDestination | Unified snapshot read; ordered peer enable/UseCircuitCode continuations preserved; unavailable still takes the pre-existing reconnect path. |
| `llworld.cpp:2379` ring age/global stall | Fixed packet/birth ages; unavailable defers sweep without inventing silence. |
| `llworld.cpp` ring dropRegion/updateRegionRing | Stale-ring removals explicitly deferred; only existing ordered owner close/timeout and simulator-disable paths operate. See limitation below. |
| `message.cpp:461,2475` isTrustedSender/getCircuitTrust | Fixed: copied trusted/present values. |
| `message.cpp:2810,2832,2846` blocked/alive helpers | Fixed: copied values; unknown blocks permission and cannot claim alive. Circuit-code overloads have no viewer callers; host lookup remains the existing code-map API. |
| `llxfermanager.cpp:955` processFileRequest overflow | Fixed: copied presence/trust. Unknown rejects/removes only this new request, never closes circuit; repeated unknown cannot retain an over-limit suffix. Known trusted exception unchanged. |
| `llxfermanager.cpp:1054` receive maintenance | Fixed: unavailable preserves receive/callback, absent/dead aborts once. |
| `llworld.cpp:973–974` actual traffic bits | Fixed: generation-tagged cumulative ring snapshot deltas; no worker reset; defer unavailable/regressing values. Owner packet counts are wire totals including internal control. |
| `sldatausage.cpp:140–143` session totals | Fixed: coherent ring values; last same-generation totals retained on unavailable; fresh generation resets prior cache. |
| `llviewerstats.cpp:830–850` stats upload | Fixed: published wire byte/packet totals; unavailable compression/failure aggregates omitted rather than legacy zeros. |
| `llviewermenu.cpp:2390` drop-packet debug action | Guarded legacy-only; no owner ring mutation. |
| `llstartup.cpp:819` initial drop percentage | Already safe: before owner installation. |
| `message.cpp` dumpPacketToLog | Sender comes from main-owned receive metadata, not worker ring. |
| `lltransfermanager.cpp:755` updateTransfers source throttle | Explicit owner guard; no speculative source throttle. Incoming TransferRequest can create a source; normal viewer asset requests use target channels. Source serving is unsupported under owner. |
| `message.cpp:2452,2458,2469` show/dump/getCircuitInfo | Aggregate legacy directory explicitly unavailable under owner; per-host snapshot API is authoritative. No viewer aggregate-directory caller found. |
| `message.cpp:2489,2492` enableCircuit | Owner guard prevents shadow-table creation. All viewer callers in startup/world/viewermessage already use ordered owner alternatives. |
| `message.cpp:2680` allow-timeout policy | Legacy-only guarded server API; no viewer caller found. |
| `message.cpp:2795` timeout callback | Existing snapshot/incarnation registry, APP barrier and owner finish command preserved. |
| `message.cpp:2944,2959,3401,3416` ping/ACK handlers | Explicit owner guards; worker already consumes controls. |
| `message.cpp:3151,3155,3452,3528` inbound UseCircuitCode/shared-secret trust handlers | Explicit legacy-only guards; viewer owner peers use outbound ordered trusted enables. |
| `message.cpp:4294,4305` remote sender UUID/session | Legacy-only guarded identity API; no viewer caller found; owner returns null, no borrowed worker identity. |
| `message.cpp:4513,4589` deny/trust establishment | Guarded legacy-only server handshake; no viewer caller found. |
| `message.cpp:508,521,1384,1406,1511` legacy receive bookkeeping | Existing checkMessages owner branch returns to owned decoder before these reads. |
| `message.cpp:1617,1621,1625,1665` legacy watchdog/retry/ACK pump | Existing processAcks owner branch bypasses these mutations. |
| `message.cpp:1844,1871,2001` reliable timeout reads | Existing owner retry metadata delegates current ping-based timeout to owner state; no legacy pointer read. |
| `message.cpp:2075,2097,2174,2541` send/add/remove circuits | Existing owner send/retire branches bypass legacy table. Close/shutdown order unchanged. |
| `message.cpp:250,563,903` construction/handoff/snapshot bridge | Main-only construction/pristine handoff and explicitly pre-owner legacy bridge; no fallback while facade exists. |
| `llcircuit.cpp:617,637,1085` and other LLCircuitData internals | Legacy-only table/scheduler implementation, reached through the guarded legacy paths above; owner uses LLTransportCircuitState. |
| `lltransportowneradmission.h` findCircuit | Main-owned terminal-credit record lookup, not LLCircuitData; identity and bounded credits unchanged. |
| `llcrossingtrace.cpp:367`, `llviewerstats.cpp:538`, `llviewerobject.cpp:2327,2674`, `llviewerregion.cpp:2191,2240`, `llworld.cpp:1009`, `sltempobjectedge.cpp` | Already value consumers: diagnostics/ping/interpolation/region counters/loss/probe respectively. Unavailable defers; loss quality and incarnation rebasing preserved. Probe sends remain revalidated by owner. |
| `llviewerregion.cpp:3473` object request batching | Separate source defect found: void wrapper discards send rejection and clears unsent IDs. Optional4f67 retains accepted prefix correctly but is HELD for queue boundedness. |
| `llviewerthrottle.cpp` AgentThrottle, `llxfermanager.cpp` ACK throttle | Main-owned application values, sent through owner API; no raw circuit access. No throttle tighten/ease evidence in failing historical logs. |
| `message.cpp` reliable limits/admission | Main terminal credits256; owner state reliable slots4096, retries and terminal drain remain owner controlled. No legacy unacked counter governs viewer admission; getUnackedListSize has no caller found. |

Inventory: `git grep -n -E 'mCircuitInfo|findCircuit\(' 82fa4fb875 -- indra`, followed by all LLCircuitData, snapshot, circuit-control caller, packet-ring, object-request, throttle and reliable-counter searches. Test-only fake table references are not production consumers. Commented `llviewerobject.cpp:2525` and warning text are not reads.

## Validation

Actual configured Linux object commands were extracted read-only with `ninja -t commands`, source/includes retargeted to this worktree, textual PCH and all outputs/temp owned under `.validation`. Eight affected production objects pass: `llworld`, `message`, `llxfermanager`, `lltransfermanager`, `llviewermenu`, `sldatausage`, `llviewerstats`, plus the held patch's `llviewerregion`. All final source hashes match receipts. All use `-Werror`; **message alone additionally uses `-Wno-error=format-truncation`**: baseline82fa and changed source reproduce the identical existing GCC warning in dumpPacketToLog (`snprintf` bound2147483904 vs256). No new warning is waived. See `.validation/object-summary.json` and `.validation/objects/*-receipt.json`.

Strict C++20 `-Wall -Wextra -Werror` production-body fixtures PASS:

- `indra/newview/tests/llworld_circuit_snapshot_fixture.py`: receiving root; birth/packet age; absent/unavailable/future/corrupt samples; replacement incarnation; explicit no owner age-only removal.
- `indra/llmessage/tests/lltransport_main_consumers_fixture.py`: trusted/alive/blocked values; unknown Xfer receive retention; absent/dead completion once;1000 rejected unknown overflow requests preserving500 prior entries; generation-aware totals.
- `indra/newview/tests/llworld_net_stats_fixture.py`: cumulative deltas/no double reset, unavailable/regressing samples, new generation, legacy compatibility.
- Existing `transport_retired_circuit_fixture.py`: actual retained-close/world/terminal/shutdown methods plus real circuit state PASS. Separate shutdown fixture needed two missing fake members added only to generated `.validation/shutdown.cpp`, then passed; no production shutdown change.
- Held patch fixture `llviewerregion_cache_requests_fixture.py`:600 IDs, reject-first/accepted255/rejected suffix/resume/no duplicate accepted IDs, exact255/final1 PASS; does not establish a bounded retained queue.
- `python3 -I scripts/check/run.py --only win-macros` PASS3552 files; `git diff --check` PASS.

## Remaining limitations

Owner stale-ring removals stay deferred: stale neighbors may remain until simulator disable/owner lifecycle handling. A snapshot supplies a conditional token, and the worker's DisableIfUnchanged/conditionalClose already checks incarnation/activity and returns an APP barrier. The main dispatcher currently classifies Enable, retired unconditional Disable and watchdog terminals; safe ring removal needs its own bounded continuation, terminal routing, drain-through-barrier and shutdown handling. Calling the existing retirement path directly would invalidate crossing peers before conditional approval. No crossing/close/shutdown redesign is included.

Aggregate owner directory enumeration, server source serving, sender server UUIDs and shared-secret server trust handshakes are unsupported; per-host circuit and ring snapshots are supported. Aggregate owner compression/failure/loss metrics remain unavailable, not inferred from legacy zeros.

Historical failing logs lack VOCacheMetrics and tighten/ease events; sampled idleNetwork <=0.11ms does not establish generic5ms decode exhaustion. AgentUpdate Far ramps256→512 in~6s identically in C2/B1/tex3/tex4, excluding that ramp divergence. No viewer/GPU run or proof of the historical object-arrival cause is claimed.
