#!/bin/sh
# Builds RinCast for the Steam Frame (Linux ARM64) in Steam's Sniper SDK, the runtime the Frame runs
# native games in. From the repository root, on any PC with Docker (an x86 PC emulates ARM64):
#
#   docker run --rm --platform linux/arm64 -v "$PWD:/src" -w /src \
#     registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64:latest sh headset/linux/build.sh
#
# The result is build/frame/rincast-frame with its assets/ folder next to it.
set -e
cmake -S headset/linux -B build/frame -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14
cmake --build build/frame
