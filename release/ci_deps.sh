#!/usr/bin/env bash
# CI 依赖安装：按目标平台族分发。
# Windows ARM64 的 MSYS2 工具链由 workflow 中的 msys2/setup-msys2 安装，不走这里。
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

FAMILY="${1:-}"
case "$FAMILY" in
  linux)
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
      meson ninja-build nasm ffmpeg libavcodec-dev libavdevice-dev \
      libavformat-dev libavutil-dev libswresample-dev \
      libusb-1.0-0 libusb-1.0-0-dev libv4l-dev \
      libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev \
      libsndio-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
      libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
      libdrm-dev libva-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
      libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev \
      libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev zip
    ;;
  windows-cross)
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
      meson ninja-build nasm mingw-w64 mingw-w64-tools \
      libz-mingw-w64-dev zip
    ;;
  macos)
    brew install meson nasm libiconv zlib automake autoconf libtool
    ;;
  *)
    echo "ERROR: unknown platform family '$FAMILY' (expected linux|windows-cross|macos)" >&2
    exit 1
    ;;
esac
