#!/bin/bash
# Copies NVIDIA Streamline's runtime DLLs (from the SDK setup.sh unpacked into the build
# tree) beside the built viewer in C:\dev\QuasarSlipstream. Finds them wherever the SDK
# keeps them (the production ones, not the development/debug builds).
set -euo pipefail
cd "$(dirname "$0")/../.."
SDK=build-windows-x86_64/_deps/streamline
[ -d "$SDK" ] || { echo "no Streamline SDK at $SDK"; exit 1; }
SRC=$(find "$SDK" -iname 'sl.interposer.dll' | grep -iv -E 'debug|develop' | head -1)
[ -n "$SRC" ] || { echo "sl.interposer.dll not found in the SDK"; find "$SDK" -maxdepth 3 -type d; exit 1; }
SRC=$(dirname "$SRC")
EXE=$(ls /mnt/c/dev/QuasarSlipstream/*/QuasarViewer.exe 2>/dev/null | head -1)
[ -n "$EXE" ] || { echo "no viewer in C:\\dev\\QuasarSlipstream"; exit 1; }
DST=$(dirname "$EXE")
echo "Streamline DLLs from $SRC"
for f in sl.interposer.dll sl.common.dll sl.pcl.dll sl.reflex.dll sl.dlss_g.dll nvngx_dlssg.dll; do
  if [ -f "$SRC/$f" ]; then cp "$SRC/$f" "$DST/" && echo "  copied $f"; else echo "  MISSING $f"; fi
done
