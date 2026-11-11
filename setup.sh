#!/bin/bash
# Quasar on Slipstream: one-time WSL setup and first Windows build.
# Run in Ubuntu with:  curl -L https://raw.githubusercontent.com/KrisKreemurr/Quasar-Viewer/quasar/slipstream/setup.sh | bash
set -eo pipefail
cd ~
# Slipstream's own Windows builds use Ubuntu 24.04 with clang/LLD 22 (their release.yml);
# Ubuntu ships 18, so LLVM 22 comes from apt.llvm.org. Only the first run installs
# anything, so later runs never ask for the password.
LLVM=/usr/lib/llvm-22/bin
if [ ! -x "$LLVM/clang-cl" ] || ! command -v glslc >/dev/null || ! command -v 7z >/dev/null; then
  sudo apt update
  sudo apt install -y build-essential cmake ninja-build git python3 python3-venv python3-pip p7zip-full nsis pkg-config libvulkan-dev curl wget lsb-release software-properties-common gnupg glslc glslang-tools
  wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
  sudo bash /tmp/llvm.sh 22
  sudo apt install -y lld-22 llvm-22
fi
# A current CMake from Kitware's apt repository (Ubuntu's 3.28 is older than Slipstream builds with)
if ! cmake --version 2>/dev/null | grep -q "version [4-9]"; then
  wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
  echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main" | sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
  sudo apt update
  sudo apt install -y cmake
  rm -rf ~/quasar-slipstream/build-windows-x86_64/CMakeCache.txt ~/quasar-slipstream/build-windows-x86_64/CMakeFiles
fi
# clang-cl, lld-link, llvm-rc, llvm-mt, llvm-lib, llvm-dlltool under their plain names
export PATH="$LLVM:$PATH"
if ! command -v xwin >/dev/null; then
  curl -L https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz | tar xz
  sudo mv xwin-0.6.5-x86_64-unknown-linux-musl/xwin /usr/local/bin/
  rm -rf xwin-0.6.5-x86_64-unknown-linux-musl
fi
[ -d ~/.xwin ] || xwin --accept-license splat --output ~/.xwin
[ -d ~/quasar-slipstream ] || git clone --branch quasar/slipstream --single-branch https://github.com/KrisKreemurr/Quasar-Viewer.git ~/quasar-slipstream
cd ~/quasar-slipstream
# Follow the branch exactly (it may be rebuilt on a newer Slipstream, which a plain pull
# refuses), then run the NEW copy of this script: bash reads a script as it goes, so one
# that the update rewrote under it would run half old, half new.
if [ "${1:-}" != "--updated" ]; then
  git fetch origin quasar/slipstream
  git reset --hard FETCH_HEAD
  exec bash ./setup.sh --updated
fi
# A different Slipstream version under the same build folder starts its configure fresh
# (downloaded libraries and compiled files that still match are kept)
VER=$(cat indra/newview/VIEWER_VERSION.txt)
if [ -f build-windows-x86_64/.quasar-version ] && [ "$(cat build-windows-x86_64/.quasar-version)" != "$VER" ]; then
  rm -rf build-windows-x86_64/CMakeCache.txt build-windows-x86_64/CMakeFiles build-windows-x86_64/vk
fi
mkdir -p build-windows-x86_64 && echo "$VER" > build-windows-x86_64/.quasar-version
# WSL gets half the PC's memory, and Slipstream's biggest files need several GB each to
# compile, so one job per ~3 GB (a crash with no error message was the compiler running out)
MEM_GB=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1048576 ))
JOBS=$(( MEM_GB / 3 )); [ "$JOBS" -lt 2 ] && JOBS=2; [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
export NINJA_JOBS=${NINJA_JOBS:-$JOBS}
echo "Building with $NINJA_JOBS jobs ($MEM_GB GB for Linux)"
# NVIDIA Streamline SDK (Reflex, DLSS Frame Generation): its headers for the build, its
# signed DLLs beside the viewer. Fetched once from NVIDIA's GitHub release; a fresh unpack
# makes CMake configure again so it is picked up.
SL_VER=2.14.16
SL_DIR=build-windows-x86_64/_deps/streamline
if [ ! -f "$SL_DIR/include/sl.h" ]; then
  mkdir -p build-windows-x86_64/_deps
  echo ">>> Fetching NVIDIA Streamline $SL_VER (about 270 MB, once)"
  if curl -fL -o /tmp/streamline.zip "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$SL_VER/streamline-sdk-v$SL_VER.zip" \
     && echo "32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b  /tmp/streamline.zip" | sha256sum -c - ; then
    rm -rf "$SL_DIR" && mkdir -p "$SL_DIR" && 7z x -o"$SL_DIR" /tmp/streamline.zip >/dev/null
    rm -f build-windows-x86_64/CMakeCache.txt
  else
    echo ">>> Streamline not fetched: building without Reflex and DLSS Frame Generation"
  fi
  rm -f /tmp/streamline.zip
fi
# The whole log is kept (also on C:), and a failure prints only the real errors
# Local builds: no Windows installer, and a quick zip (both only slow the build down here)
export QUASAR_NO_INSTALLER=1 QUASAR_ZIP_LEVEL=1
if ! make windows 2>&1 | tee ~/quasar-build.log; then
  cp ~/quasar-build.log /mnt/c/dev/quasar-build.log 2>/dev/null || true
  echo; echo "===== BUILD FAILED: the errors (full log in C:\\dev\\quasar-build.log) ====="
  grep -E -A3 " error:|Killed|signal|CMake Error" ~/quasar-build.log | grep -v -E "warning:|^/usr/lib/llvm" | head -60 || echo "(no error message: the compiler was most likely out of memory; run: NINJA_JOBS=2 bash setup.sh)"
  exit 1
fi
# Windows won't let a running viewer's files be replaced: wait for it to close
if tasklist.exe 2>/dev/null | grep -qi -E "QuasarViewer.exe|SlipstreamViewer.exe|SLPlugin.exe|SLVoice.exe"; then
  echo ">>> The viewer is running: close it and the new build will be copied in."
  while tasklist.exe 2>/dev/null | grep -qi -E "QuasarViewer.exe|SlipstreamViewer.exe|SLPlugin.exe|SLVoice.exe"; do sleep 2; done
  sleep 2
fi
rm -rf /mnt/c/dev/QuasarSlipstream
7z x -o/mnt/c/dev/QuasarSlipstream build-windows-x86_64/package/*_win64.zip >/dev/null
# Streamline's DLLs beside the viewer (NVIDIA's licences for them are in the SDK)
if [ -f "$SL_DIR/include/sl.h" ]; then bash scripts/quasar/copy_streamline.sh || true; fi
# A desktop shortcut straight to the exe, wherever the zip put it
EXE=$(find /mnt/c/dev/QuasarSlipstream -maxdepth 3 -iname "QuasarViewer.exe" | head -1)
if [ -n "$EXE" ]; then
  WEXE=$(wslpath -w "$EXE")
  # (the old "Quasar (Slipstream build)" shortcut pointed at SlipstreamViewer.exe, gone since the rename)
  powershell.exe -NoProfile -Command "\$d=[Environment]::GetFolderPath('Desktop'); Remove-Item -ErrorAction SilentlyContinue (\$d+'\\Quasar (Slipstream build).lnk'); \$s=(New-Object -ComObject WScript.Shell).CreateShortcut(\$d+'\\Quasar Viewer.lnk'); \$s.TargetPath='$WEXE'; \$s.WorkingDirectory=(Split-Path '$WEXE'); \$s.Save()" >/dev/null 2>&1 || true
  echo "Viewer: $WEXE  (shortcut on your desktop: Quasar Viewer)"
fi
# A "Quasar" shortcut on the Windows desktop to the built viewer (best effort)
cat > /mnt/c/dev/make-quasar-shortcut.ps1 <<'PS1'
$exe = Get-ChildItem 'C:\dev\QuasarSlipstream' -Recurse -Filter 'QuasarViewer.exe' | Select-Object -First 1
if ($exe) {
  $s = (New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop') + '\Quasar.lnk')
  $s.TargetPath = $exe.FullName
  $s.WorkingDirectory = $exe.DirectoryName
  $s.Save()
}
PS1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\dev\make-quasar-shortcut.ps1' >/dev/null 2>&1 || true
# A double-click rebuild for Windows, so Linux never has to be opened again
# It closes a running viewer first (taskkill), so its files can be replaced.
printf '@echo off\r\nrem Close any running viewer so its files can be replaced\r\ntaskkill /F /T /IM QuasarViewer.exe >nul 2>&1\r\ntaskkill /F /T /IM SLVoice.exe >nul 2>&1\r\ntimeout /t 2 /nobreak >nul\r\n\r\nwsl -d Ubuntu-24.04 -- bash -lc "cd ~/quasar-slipstream && git fetch -q origin quasar/slipstream && git reset -q --hard FETCH_HEAD && bash ./setup.sh --updated"\r\npause\r\n' > /mnt/c/dev/Build-Quasar.bat
echo "Done: C:\\dev\\QuasarSlipstream  (next time, double-click C:\\dev\\Build-Quasar.bat)"
