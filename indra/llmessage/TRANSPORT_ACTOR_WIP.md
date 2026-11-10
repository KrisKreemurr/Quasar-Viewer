# Transport actor integration constraints — WIP

This child is disabled and not a complete transport implementation. Circuit
state, the socket loop, callback delivery and lifecycle are not yet integrated.

Every accepted outgoing operation must reserve one terminal-completion credit
before publication. Credits cover in-flight operations and completion events
already queued; a worker enqueue does not release a credit. Only main consuming
its completion releases the credit. On stop, the worker must settle every
accepted operation, including circuit-gone outcomes, using its existing credits
without waiting for main to consume events. The main thread must be able to
join while its event pump is paused. Reservations rejected before acceptance
must be released without a callback. Completion and timeout/control/fatal
capacity must not depend on a free application-data event or arena record.

Application data that has been ACKed may stay in the bounded arena and metadata
queue after worker join. Main must dispatch or dispose of it according to the
shutdown contract, clear every borrowing message reader, and drain/release FIFO
before destroying the retained schemas, queue or arena. No socket or worker may
survive its dependencies. The stopped-worker state must remain owned and alive
until accepted terminal events are delivered or explicitly cancelled on main.

The compact ingress event and arena publication helper alone do not establish
these properties. Required tests include a paused main with all terminal credits
occupied across queued and in-flight operations, stop/join without a consumer,
then main drain delivering each accepted completion exactly once; separately
fill application data and prove bounded timeout/control/fatal progress. Cover
partial startup, callback reentry, duplicate stop and reader-clear ordering.

Circuit close and timeout events carry the last published application sequence
for their exact incarnation. Main may drain control/completion events promptly,
but must defer callbacks and region removal until it has consumed every
accepted application event for that incarnation through the barrier. The only
exception is an explicit controlled session teardown policy that accounts for
every retained payload. Worker ACK/ping/control progress does not wait for
main to satisfy the barrier. Sequence space must not wrap or be reused within
an incarnation; failure to allocate an ordering slot refuses admission before
ACK. This value contract is not yet wired to a publisher or dispatcher.

Integration tests must queue an accepted reliable inventory or agent update,
then a close or timeout through the separate control queue, deliver control
first, and verify the application event is handled before callback/removal.
Cover same-host new incarnation, stalled main, a full application queue,
barrier advancement after handler/reader clear, and explicit teardown.

## Protected viewer and off-circuit handshake

`LLMessageSystem` defaults `mbProtected` to true. In the protected viewer,
`findCircuit` does not create or revive an unknown/dead sender. After template
validation `checkMessages` rejects off-circuit templates other than
`UseCircuitCode` (Low 3); the owner must not add an off-circuit ping reply.
Unprotected library/server-tool behavior is a separate compatibility path.

The protected-viewer `UseCircuitCode` exception remains an implementation
requirement: retain its decoded payload and bounded main-handshake event before
any ACK, invoke the existing handler only on main, and preserve ordering of the
resulting owner Enable and ACK transaction. An ACK for the off-circuit reliable
packet must not be committed until the matching handler-created circuit and
incarnation exist. The handshake event needs independent bounded capacity,
login/endpoint identity, terminal cleanup on stop and no borrowed wire/scratch
storage. This path must preserve application-event ordering and cannot use the
normal ingress helper's nonzero-existing-incarnation precondition. The current
receiver rejects this case and is not ready for viewer use.

## Slow or stalled main and crossings

Transport ACKs, pings and keepalive must progress with a full application
queue and while main is paced to 1 Hz or actually blocked, subject to bounded
memory and protocol retry backpressure. Viewer handlers remain on main; no
fixture may claim application callbacks ran while main was blocked. For paced
main, preserve event dependency order across CrossedRegion (HTTP event queue),
CompleteAgentMovement, AgentMovementComplete and EnableSimulator / crossing
destination setup. Do not prioritize CrossedRegion by overtaking prerequisite
application messages, and preserve Crossing568 / PRE-DISCONNECT diagnostics.
Tests must distinguish actual blocked-main transport progress from paced-main
ordered handler/command progress and include crossing, teardown and cache load.

HTTP event polling is main-pump dependent: receiving UDP on a worker alone
does not promptly dispatch CrossedRegion when the shared main event/asset queue
is backlogged or the frame pacer sleeps. A bounded ordered main control service
and a no-lost-wake primitive are required. `LLTransportMainWake` defines the
steady-deadline/observe/signal/wait value boundary; it has no implementation
yet. Signals wake the main loop but never execute handlers on producer threads.
The main lane must preserve event dependencies and have an explicit budget.
