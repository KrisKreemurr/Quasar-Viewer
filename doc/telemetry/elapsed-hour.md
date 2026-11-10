# Collection elapsed hour (minor 1)

Author: SkyLine. Optional `segment.elapsed_hour` is an integer from 0 to 24.
Absent means **unknown**. Values 0–23 describe the real monotonic hour containing
the window's first accepted logged-in frame; 24 means **24 hours or later** and
must be excluded from the 0–24-hour curve. Local unknown sentinel 25 is never sent.

The origin is telemetry activation after current consent and completed durable
purge, not viewer launch or login. Enabling midway through a run begins hour zero.
Re-enabling or resuming after explicit queue deletion starts a new collection
epoch, as does restarting the viewer. The in-process monotonic origin is neither
stored nor sent. No session/install identifier, absolute time, pre-consent timer
read or pre-consent collection is added. Off-state preview omits elapsed hour.
Label charts **Hours since collection began**, not viewer uptime or unique sessions.

Clock time advances during logout, inactivity and worker backpressure. Before
adding any frame, the collector compares the current clock hour with the stamped
window hour. At a boundary it attempts one flush: a busy/refused post leaves the
old window unchanged and drops future metrics; an accepted post captures that
old window and clears the live window. The boundary frame is dropped in either
case. The next accepted logged-in frame stamps the new bin. Empty intervening
hours have no report; no catch-up loop fills them. Event hooks drop unknown/future
hour events. Accepted handoffs and first data rebase sampling seconds and numeric
counter baselines, so delayed first frames and dropped intervals cannot invent
observations in the new bin. Within a bin, logged-out periods contribute no frames.
An observed frame is assigned by sampling endpoint; frame intervals are not split.

Existing hourly-duration flushing remains as a second cap; windows can therefore
be partial and multiple reports can occupy one elapsed hour. Exit keeps the
partial window's hour. Checkpoints capture the original window hour; recovery
preserves it and must not relabel it with the next process's clock. The server
excludes `kind=checkpoint` from elapsed curves, because a crash between writing a
periodic report and removing its checkpoint can otherwise duplicate a prefix.
The collection is a pooled report/frame cohort, never a tracked individual curve.

`segment.index` remains an independent report-window ordinal saturated at 15.
It is not elapsed time. Old reports, including index 15, must remain elapsed unknown;
no reader may infer an hour from index, receipt day, queue age or report kind.
Legacy pre-minor-1 checkpoints retain their original minor and absent metadata.

The server must accept schema minor 0 and 1 plus this optional field **before**
new producers ship: an old strict server rejects minor 1. Legacy queued reports
continue to validate and send; consent version and durable queue/purge rules are
unchanged. Only minor-1 producers generate the new field when known.

Curves pool existing frame histograms by version and optional hardware tier.
Median and mean frame times are estimates from bin representatives, not exact
raw measurements. Mean FPS is `1000 / estimated_mean_frame_ms`; it is not the
arithmetic mean of instantaneous FPS or of per-report FPS. Median FPS uses the
inverse median frame time. Weight bins by frames, not by report count, and preserve
existing frame-class distinctions and overflow limitations. No additional exact
frame-duration sum is introduced by this change.
