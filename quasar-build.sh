#!/usr/bin/env bash
# Quasar Viewer: build the viewer at low CPU priority, so the PC stays usable while it compiles.
#
# Usage (from Git Bash, in the repo folder):
#   ./quasar-build.sh                 # RelWithDebInfo, every core, low priority
#   ./quasar-build.sh Release         # another configuration
#   ./quasar-build.sh Release 4       # and at most 4 compile jobs at once
#
# Windows starts every process the build launches (MSBuild, every cl.exe) at the priority of the process that
# launched it, so the compilers take whatever CPU is left over after games, the browser and the rest.

set -euo pipefail

CONFIG="${1:-RelWithDebInfo}"
JOBS="${2:-}"
BUILD_DIR="build-Windows-vs2026-os"

cd "$(dirname "$0")"

if [ ! -d "$BUILD_DIR" ]; then
    echo "No $BUILD_DIR folder yet. Run the configure step first:"
    echo "  cmake -S indra --preset vs2026-os"
    exit 1
fi

PARALLEL="--parallel"
if [ -n "$JOBS" ]; then
    PARALLEL="--parallel $JOBS"
fi

echo "Building $CONFIG at low priority..."
STATUS=0
(
    # Lower this subshell's own priority; Windows gives every process it starts (cmake, MSBuild, each cl.exe) the same.
    WINPID="$(cat /proc/$BASHPID/winpid 2>/dev/null || true)"
    if [ -n "$WINPID" ]; then
        powershell.exe -NoProfile -Command "(Get-Process -Id $WINPID).PriorityClass = 'BelowNormal'" >/dev/null 2>&1 \
            || echo "(Could not lower the priority; building at normal priority.)"
    fi
    # /nodeReuse:false: a build helper left running by an earlier build would keep its old priority, so start fresh ones.
    cmake --build "$BUILD_DIR" --config "$CONFIG" $PARALLEL -- /nodeReuse:false
) || STATUS=$?

if [ $STATUS -eq 0 ]; then
    echo "Done. The viewer is in $BUILD_DIR/newview/$CONFIG/"
else
    echo "The build failed (exit code $STATUS). Copy the error lines above and paste them to Claude."
fi
exit $STATUS
