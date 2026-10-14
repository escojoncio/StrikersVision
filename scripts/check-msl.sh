#!/usr/bin/env bash
# Compiles the MSL source the headset layer builds at run time (kSvShadersMsl in
# visionos/xr/sv_shaders.h) with the visionOS Metal compiler, so a shader error fails the build
# instead of showing up only in the log on the headset: check-msl.sh [log file]
set -uo pipefail
LOG="${1:-msl.log}"
OUT=$(mktemp -d)
# The raw string between R"MSL( and )MSL".
python3 - "$OUT/sv_shaders.metal" <<'PY'
import re, sys
src = open("visionos/xr/sv_shaders.h", encoding="utf-8").read()
m = re.search(r'R"MSL\((.*?)\)MSL"', src, re.S)
if not m:
    sys.exit("check-msl: kSvShadersMsl not found in sv_shaders.h")
open(sys.argv[1], "w", encoding="utf-8").write(m.group(1))
PY
[ -s "$OUT/sv_shaders.metal" ] || { echo "check-msl: nothing extracted" | tee "$LOG"; exit 1; }
# Xcode 26 ships the Metal toolchain as a separate component: fetched when missing.
if ! xcrun -sdk xros --find metal >/dev/null 2>&1 || ! xcrun -sdk xros metal -v >/dev/null 2>&1; then
  echo "check-msl: Metal toolchain missing, downloading it" | tee "$LOG.dl"
  xcodebuild -downloadComponent MetalToolchain 2>&1 | tail -5 | tee -a "$LOG.dl"
fi
xcrun -sdk xros metal -c "$OUT/sv_shaders.metal" -o "$OUT/sv_shaders.air" > "$LOG" 2>&1
RC=$?
[ -f "$LOG.dl" ] && cat "$LOG.dl" >> "$LOG"
cat "$LOG"
if [ $RC -ne 0 ]; then echo "check-msl: sv_shaders.h does not compile (error: see above)" | tee -a "$LOG"; exit 1; fi
echo "check-msl: sv_shaders.h compiles" | tee -a "$LOG"
