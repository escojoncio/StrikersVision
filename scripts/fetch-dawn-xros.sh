#!/usr/bin/env bash
# Dawn for visionOS: Aurora's prebuilt iOS package (the version Aurora pins), with the platform of
# every object in libwebgpu_dawn.a rewritten to visionOS. Nobody publishes a visionOS Dawn; the iOS
# one is the same arm64 code against the same Metal, and the only thing ld objects to is the
# LC_BUILD_VERSION platform each object carries ("building for visionOS, but linking in object file
# built for iOS").
#
#   scripts/fetch-dawn-xros.sh <out dir>      -> <out dir>/{include,lib/libwebgpu_dawn.a,lib/cmake/Dawn}
set -euo pipefail
OUT="${1:?usage: fetch-dawn-xros.sh <out dir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
VERSIONS="$HERE/../smstrikers-port/extern/aurora/cmake/AuroraDependencyVersions.cmake"
VERSION=$(sed -n 's/.*AURORA_DAWN_VERSION "\(v[0-9.]*\)".*/\1/p' "$VERSIONS" | head -n 1)
[ -n "$VERSION" ] || { echo "no AURORA_DAWN_VERSION in $VERSIONS" >&2; exit 1; }
URL="https://github.com/encounter/dawn/releases/download/$VERSION/dawn-ios-arm64.tar.gz"
SDK=$(xcrun --sdk xros --show-sdk-version)

echo "Dawn $VERSION (ios-arm64) -> visionOS, SDK $SDK"
rm -rf "$OUT"
mkdir -p "$OUT"
curl -sSfL --retry 4 --retry-delay 5 "$URL" | tar xz -C "$OUT"
[ -f "$OUT/lib/libwebgpu_dawn.a" ] || { echo "unexpected package layout" >&2; ls -R "$OUT" | head -40 >&2; exit 1; }

# PLATFORM_VISIONOS = 11 (<mach-o/loader.h>). minos 26.0, the app's deployment target.
python3 -I "$HERE/macho-retarget.py" "$OUT/lib/libwebgpu_dawn.a" 11 26.0 "$SDK"
# The table of contents is rebuilt so it is ranlib's own again after the rewrite.
xcrun ranlib "$OUT/lib/libwebgpu_dawn.a"

# Proof, not hope: every member now says visionOS (platform 11 / VISIONOS).
otool -l "$OUT/lib/libwebgpu_dawn.a" | awk '/cmd LC_BUILD_VERSION/{f=1} f&&/platform/{print "platform", $2; f=0}' | sort | uniq -c || true
echo "$VERSION" > "$OUT/VERSION"
