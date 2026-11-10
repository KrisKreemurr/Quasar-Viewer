#!/usr/bin/env bash
# vqbench (#1039): run the bench ONLY while the GPU is free -- no viewer process and no render2 GPU lock -- holding
# the lock for exactly the run. Usage: vqb_run.sh <path to vqbench> [vqbench options]
# (/proc/<pid>/comm is truncated to 15 characters: the viewer is found by its executable's path, never pgrep -f)
set -euo pipefail
LOCK="${HOME}/.cache/render2-gpu.lock"
BIN="${1:?usage: vqb_run.sh <vqbench> [options]}"
shift

viewer_running() {
    local p exe
    for p in /proc/[0-9]*; do
        exe=$(readlink "$p/exe" 2>/dev/null || true)
        case "$exe" in
            *do-not-directly-run-slipstream-bin*) echo "${p#/proc/} $exe"; return 0 ;;
        esac
    done
    return 1
}

if v=$(viewer_running); then
    echo "vqb_run: the viewer is running (pid ${v%% *}); not timing anything" >&2
    exit 3
fi
mkdir -p "$(dirname "$LOCK")"
# atomic create: fails if anyone holds it
if ! (set -C; echo vq-bench > "$LOCK") 2>/dev/null; then
    echo "vqb_run: the GPU lock is held ($LOCK: $(cat "$LOCK" 2>/dev/null))" >&2
    exit 3
fi
trap 'rm -f "$LOCK"' EXIT INT TERM
# re-check right before the run (a viewer may have started between the two tests)
if v=$(viewer_running); then
    echo "vqb_run: the viewer started (pid ${v%% *}); released the lock" >&2
    exit 3
fi
nice -n 0 "$BIN" "$@"
