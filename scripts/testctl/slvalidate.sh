#!/bin/bash
# Release-candidate validation on the test box, driven from the dev machine.
#
#   slvalidate.sh PACKAGE_DIR NAME core|sync [PLAN.json]
#
# PACKAGE_DIR is a TESTCTL package (make TESTCTL=1). Copies it to the box as
# ~/slipstream/NAME, starts it under the Vulkan validation layers (sync adds synchronization
# validation) with the release-candidate settings file, logs in, runs slval.py, quits, then
# fetches the viewer log and prints slval_report.py's summary. Results land in
# ./slval-NAME-MODE/ (log, steps.json, snapshots, report.txt).
#
# Box: ssh alias "garbage" (override with SLVAL_HOST); scripts expected in ~/bin there
# (this directory's slctl.py, slval.py, slrun.sh are copied on every run).
set -euo pipefail

PKG=$1; NAME=$2; MODE=$3; PLAN=${4:-}
HOST=${SLVAL_HOST:-garbage}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$PWD/slval-$NAME-$MODE
mkdir -p "$OUT"

case "$MODE" in
  core) SYNC= ;;
  sync) SYNC="VK_LAYER_VALIDATE_SYNC=1" ;;
  *) echo "mode must be core or sync" >&2; exit 2 ;;
esac

rsync -a --delete "$PKG"/ "$HOST:slipstream/$NAME/"
rsync -a "$HERE/slctl.py" "$HERE/slval.py" "$HERE/slrun.sh" "$HOST:bin/"
[ -n "$PLAN" ] && rsync -a "$PLAN" "$HOST:slipstream/slval-plan.json"

ssh "$HOST" bash -s <<EOF
set -eu
rm -f ~/.slipstreamviewer/logs/Slipstream.log
export VK_LOADER_LAYERS_ENABLE='*validation' VK_LAYER_REPORT_FLAGS=error,warn VK_LAYER_DUPLICATE_MESSAGE_LIMIT=20 $SYNC
~/bin/slrun.sh ~/slipstream/$NAME --settings settings_rc.xml
~/bin/slctl.py wait STATE_STARTED 600
sleep 30
rm -rf ~/slipstream/slval-out
~/bin/slval.py ${PLAN:+~/slipstream/slval-plan.json} ~/slipstream/slval-out || echo "slval.py failed: \$?"
~/bin/slctl.py quit || true
for i in \$(seq 1 90); do pgrep -x slipstream > /dev/null || break; sleep 1; done
if pgrep -x slipstream > /dev/null; then echo "viewer did not exit; forcequit"; ~/bin/slctl.py forcequit || true; fi
true
EOF

rsync -a "$HOST:.slipstreamviewer/logs/Slipstream.log" "$OUT/Slipstream.log"
# run.log is the viewer's stdout/stderr: the log lines (with the SLVAL markers) AND the
# validation layer's messages, in order. Slipstream.log alone has no validation messages.
rsync -a "$HOST:slipstream/run.log" "$OUT/run.log"
rsync -a "$HOST:slipstream/slval-out/" "$OUT/" || true
set +e
python3 -I "$HERE/slval_report.py" "$OUT/run.log" "$OUT/steps.json" | tee "$OUT/report.txt"

# SEALED-ROOM BENCH (core runs; SLVAL_NO_SEALED=1 skips it): a closed room with lights outside it,
# replayed twice (lights / no lights, no login) -- any light the outside lights bring in is a leak
# through the walls (scripts/bench/sealed_room.py; the GPU half of the hit-nee check). Its own
# SLIPSTREAM_HOME on the box: the account's settings are not touched. No validation layers.
if [ "$MODE" = core ] && [ -z "${SLVAL_NO_SEALED:-}" ]; then
    rsync -a "$HERE/../bench/sealed_room.py" "$HERE/../bench/sl_capture_core.py" "$HOST:bin/"
    ssh "$HOST" bash -s <<EOF
uid=\$(id -u)
export XDG_RUNTIME_DIR=/run/user/\$uid DISPLAY=\${DISPLAY:-:0} DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\$uid/bus
for s in /run/user/\$uid/wayland-[0-9]; do [ -S "\$s" ] && export WAYLAND_DISPLAY=\$(basename "\$s"); done
rm -rf ~/slipstream/sealed-room
python3 -I ~/bin/sealed_room.py run --viewer ~/slipstream/$NAME --out ~/slipstream/sealed-room \
    > ~/slipstream/sealed-room.txt 2>&1
echo "sealed-room rc=\$?" >> ~/slipstream/sealed-room.txt
EOF
    rsync -a "$HOST:slipstream/sealed-room.txt" "$OUT/sealed-room.txt" || true
    grep -E '^sealed-room' "$OUT/sealed-room.txt" | tee -a "$OUT/report.txt" \
        || echo "sealed-room: NOT RUN (see $OUT/sealed-room.txt)" | tee -a "$OUT/report.txt"
fi
echo "results: $OUT"
