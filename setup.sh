#!/bin/bash
# Quasar on Slipstream: one-time WSL setup and first Windows build.
# Run in Ubuntu with:  curl -L https://raw.githubusercontent.com/KrisKreemurr/Quasar-Viewer/quasar/slipstream/setup.sh | bash
set -e
cd ~
# Only the first run installs packages, so later runs never ask for the password
if ! command -v clang-cl >/dev/null || ! command -v 7z >/dev/null || ! command -v ninja >/dev/null; then
  sudo apt update
  sudo apt install -y build-essential clang lld llvm cmake ninja-build git python3 python3-venv python3-pip p7zip-full nsis pkg-config libvulkan-dev curl
fi
if ! command -v xwin >/dev/null; then
  curl -L https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz | tar xz
  sudo mv xwin-0.6.5-x86_64-unknown-linux-musl/xwin /usr/local/bin/
  rm -rf xwin-0.6.5-x86_64-unknown-linux-musl
fi
[ -d ~/.xwin ] || xwin --accept-license splat --output ~/.xwin
[ -d ~/quasar-slipstream ] || git clone --branch quasar/slipstream --single-branch https://github.com/KrisKreemurr/Quasar-Viewer.git ~/quasar-slipstream
cd ~/quasar-slipstream
git pull
make windows
rm -rf /mnt/c/dev/QuasarSlipstream
7z x -o/mnt/c/dev/QuasarSlipstream build-windows-x86_64/package/*_win64.zip >/dev/null
# A double-click rebuild for Windows, so Linux never has to be opened again
printf '@echo off\r\nwsl -d Ubuntu-24.04 -- bash -lc "~/quasar-slipstream/setup.sh"\r\npause\r\n' > /mnt/c/dev/Build-Quasar.bat
echo "Done: C:\\dev\\QuasarSlipstream  (next time, double-click C:\\dev\\Build-Quasar.bat)"
