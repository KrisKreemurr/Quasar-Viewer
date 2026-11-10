#!/bin/sh
# clodbench GPU gate. ~/.cache/render2-gpu.lock only gates the owner's viewer launches: this bench neither
# waits on it nor ever creates / removes it. Agent benches serialize with each other through flock on
# ~/.cache/agent-gpu-bench.lock (wait up to 10 minutes). clodbench runs compute + CLAS builds only (no ray
# tracing pipelines), so it may run beside the owner's viewer; runs stay under 2 minutes.
#   clb_run.sh <clodbench> [options]
exec flock -w 600 "$HOME/.cache/agent-gpu-bench.lock" timeout 120 nice -n 5 "$@"
