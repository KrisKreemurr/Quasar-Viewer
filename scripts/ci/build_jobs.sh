#!/bin/sh
# Parallel compile jobs for this machine: one per core, capped by memory. GCC 14 peaks
# at ~5.5 GB in cc1plus on most viewer TUs (llcleanrenderervk.cpp more, in its own
# depth-1 ninja pool), so allow ~7 GB per job; at 3.5 GB a 24-core / 94 GB box ran 24
# jobs and OOM-thrashed (cgroup limit first: in an LXC MemTotal is the host's).
# Override with BUILD_JOBS.
[ -n "${BUILD_JOBS:-}" ] && { echo "$BUILD_JOBS"; exit 0; }
cores=$(nproc)
mem_kb=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
if [ -r /sys/fs/cgroup/memory.max ] && [ "$(cat /sys/fs/cgroup/memory.max)" != max ]; then
  cg_kb=$(( $(cat /sys/fs/cgroup/memory.max) / 1024 - $(cat /sys/fs/cgroup/memory.current) / 1024 ))
  [ "$cg_kb" -lt "$mem_kb" ] && mem_kb=$cg_kb
fi
jobs=$(( mem_kb / 7000000 ))
[ "$jobs" -gt "$cores" ] && jobs=$cores
[ "$jobs" -lt 1 ] && jobs=1
echo "$jobs"
