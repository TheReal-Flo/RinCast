#!/usr/bin/env bash
# Builds the native macOS streamer in mac/ and packs a versioned release zip.
#
#   scripts/build-mac.sh [--test] [--no-package] [--arch universal|arm64|x86_64]
#                        [--config Release|Debug] [--build-dir DIR] [--out-dir DIR]
#
# By default it writes RinCast-<version>-macos-universal.zip to dist/, holding one
# top-level RinCast/ folder with the streamer, its README, VERSION and the licence
# texts. "universal" means a single binary with both arm64 and x86_64 slices, so the
# same download runs on Apple Silicon and Intel Macs.
#
# No Apple Developer certificate or notarisation is needed to build: the streamer is
# a plain command-line executable, and Screen Recording / Accessibility are runtime
# TCC grants made by the user, not signing entitlements (see mac/README.md).
#
# Requirements: macOS with Xcode Command Line Tools and CMake >= 3.22.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build-mac.sh [options]

  --test               also run the shared core tests
  --no-package         build only, don't write a zip
  --arch ARCH          universal (default), arm64 or x86_64
  --config CONFIG      Release (default) or Debug
  --build-dir DIR      CMake build folder (default: build-mac/)
  --out-dir DIR        where the zip goes (default: dist/)
EOF
}

die() { echo "build-mac.sh: $*" >&2; exit 1; }

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
arch=universal
config=Release
build_dir="$root/build-mac"
out_dir="$root/dist"
package=1
tests=0

while (($#)); do
    case "$1" in
        --test)        tests=1; shift ;;
        --no-package)  package=0; shift ;;
        --arch)        arch=${2:?--arch needs a value}; shift 2 ;;
        --config)      config=${2:?--config needs a value}; shift 2 ;;
        --build-dir)   build_dir=${2:?--build-dir needs a value}; shift 2 ;;
        --out-dir)     out_dir=${2:?--out-dir needs a value}; shift 2 ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "unknown option: $1" ;;
    esac
done

case "$arch" in
    universal)         arch_flags='arm64;x86_64'; label=universal ;;
    arm64|x86_64)      arch_flags=$arch; label=$arch ;;
    *)                 die "--arch must be universal, arm64 or x86_64" ;;
esac
[[ $config == Release || $config == Debug ]] || die "--config must be Release or Debug"

version=$(head -n 1 "$root/VERSION" | tr -d '[:space:]')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be major.minor.patch, got '$version'"

echo "== RinCast $version: macOS streamer ($label, $config)"

# 12.3 is when ScreenCaptureKit arrived. The macOS 13 audio APIs are guarded at
# runtime in mac_capture.mm, so the binary still starts on Monterey (video only).
cmake -S "$root/mac" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$config" \
    "-DCMAKE_OSX_ARCHITECTURES=$arch_flags" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.3 \
    -DRINCAST_CORE_TESTS=ON
cmake --build "$build_dir" --config "$config" --parallel

bin="$build_dir/rincast-streamer-mac"
[[ -x $bin ]] || die "$bin was not built"
if [[ $arch == universal ]]; then
    slices=$(lipo -info "$bin")
    for slice in arm64 x86_64; do
        [[ $slices == *"$slice"* ]] || die "$bin has no $slice slice"
    done
    echo "== $slices"
fi

if ((tests)); then
    echo "== Core tests"
    "$build_dir/shared/rincast-core-tests"
fi

if ((!package)); then
    echo "== Done (no package)"
    exit 0
fi

# Stage a single RinCast/ folder, like the Windows zip from build-all.ps1.
stage="$build_dir/package"
rm -rf "$stage"
mkdir -p "$stage/RinCast"
cp "$bin" "$stage/RinCast/rincast-streamer-mac"
cp "$root/mac/README.md" "$stage/RinCast/README.md"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$root/VERSION" "$stage/RinCast/"

mkdir -p "$out_dir"
zip_path="$out_dir/RinCast-$version-macos-$label.zip"
rm -f "$zip_path"
(cd "$stage" && zip -q -r -X "$zip_path" RinCast)

echo "== $zip_path"
