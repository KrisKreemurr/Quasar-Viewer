#!/usr/bin/env bash
# Quasar Viewer: build the viewer at low CPU priority, so the PC stays usable while it compiles.
#
# Usage (from Git Bash, in the repo folder):
#   ./quasar-build.sh                 # RelWithDebInfo, every core, low priority
#   ./quasar-build.sh Release         # another configuration
#   ./quasar-build.sh Release 4       # and at most 4 compile jobs at once
#
# Windows starts every process the build launches (MSBuild, every cl.exe) at the priority of the first, so the
# compilers take whatever CPU is left over after games, the browser and the rest, rather than all of it.

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
# /nodeReuse:false: a build helper left running by an earlier build would keep its old priority, so start fresh ones.
STATUS=0
cmd //c "start \"QuasarBuild\" /B /BELOWNORMAL /WAIT cmake --build $BUILD_DIR --config $CONFIG $PARALLEL -- /nodeReuse:false" || STATUS=$?

if [ $STATUS -eq 0 ]; then
    echo "Done. The viewer is in $BUILD_DIR/newview/$CONFIG/"
else
    echo "The build failed (exit code $STATUS). Copy the error lines above and paste them to Claude."
fi
exit $STATUS
