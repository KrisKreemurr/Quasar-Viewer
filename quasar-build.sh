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
# cmd's start launches cmake below normal priority, and Windows gives everything cmake starts (MSBuild, each cl.exe)
# the same. The whole command goes to cmd as one argument that does not begin with a slash, so Git Bash passes it on
# as written, and no window title is quoted.
# /nodeReuse:false: a build helper left running by an earlier build would keep its old priority, so start fresh ones.
# The output is also kept in a log: cmd's start does not always hand the build's exit code back, so the log's error
# lines are what tell a failed build from a good one.
LOG="$BUILD_DIR/quasar-build.log"
cmd //c "start /b /belownormal /wait cmake --build $BUILD_DIR --config $CONFIG $PARALLEL -- /nodeReuse:false" </dev/null 2>&1 | tee "$LOG" || STATUS=$?

ERRORS="$(grep -E "error (C|LNK|MSB)[0-9]+|fatal error" "$LOG" | sort -u || true)"
if [ -n "$ERRORS" ] && [ $STATUS -eq 0 ]; then
    STATUS=1
fi

if [ $STATUS -eq 0 ]; then
    echo "Done. The viewer is in $BUILD_DIR/newview/$CONFIG/"
else
    echo
    echo "The build FAILED. Errors:"
    echo "$ERRORS"
    echo
    echo "Copy the error lines above and paste them to Claude (the whole output is in $LOG)."
fi
exit $STATUS
