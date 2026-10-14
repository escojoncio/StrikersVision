#!/usr/bin/env bash
# The game side for visionOS: configures smstrikers-port for xros (arm64, visionOS 26.0) and builds
# strikers_xros, which leaves libstrikers.a and the headers the app needs in <out dir>.
#
#   scripts/build-game-xros.sh <dawn dir> <build dir> <out dir>
set -euo pipefail
DAWN="$(cd "${1:?usage: build-game-xros.sh <dawn dir> <build dir> <out dir>}" && pwd)"
BUILD="${2:?}"
OUT="${3:?}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="$HERE/../smstrikers-port"
mkdir -p "$BUILD" "$OUT"
BUILD="$(cd "$BUILD" && pwd)"
OUT="$(cd "$OUT" && pwd)"

# Hermetic like tools/configure.sh: Aurora asks find_package for these before fetching its own, and in
# a cross build a hit in the SDK (sqlite3) or on the host is worse than a miss. ffmpeg (THP movies)
# is off for now. CMAKE_POLICY_VERSION_MINIMUM lets CMake 4 configure fetched projects that still
# declare an ancient minimum.
cmake -S "$PORT" -B "$BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=visionOS \
    -DCMAKE_OSX_SYSROOT=xros \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DSTRIKERS_AURORA=ON \
    -DSTRIKERS_FFMPEG=OFF \
    -DSTRIKERS_DAWN_XROS="$DAWN" \
    -DSTRIKERS_XROS_OUT="$OUT" \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_absl=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON \
    -DAURORA_CACHE_USE_ZSTD=OFF

# -k 0: every compile error in one run, not the first one per job.
cmake --build "$BUILD" --target strikers_xros -- -k 0
ls -la "$OUT" "$OUT/include"
for A in "$OUT/libstrikers.a" "$OUT/libstrikers_deps.a"; do
    echo "$A:"
    otool -l "$A" | awk '/cmd LC_BUILD_VERSION/{f=1} f&&/platform/{print "  platform", $2; f=0}' | sort | uniq -c || true
done
