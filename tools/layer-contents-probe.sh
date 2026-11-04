#!/usr/bin/env bash
# Spike S5 (app/Spikes/LayerContentsProbe.m): can graphics/host's window show its CPU buffers' IOSurfaces as its
# layer's contents, without the layer's drawables? Runs the probe on the Mac, where it opens a small window for about
# 30 s (its on-screen part needs the display on and the screen unlocked), or with --iphone in a Debug build of the
# app on the connected iPhone (tools/deploy.sh with -runLayerProbe: the build and install, about 5 to 10 min, then
# about 40 s on the key window before the app exits). The report goes to build/layer-contents-probe/<mac|iphone>.txt.
#   tools/layer-contents-probe.sh [--iphone [--device <name or identifier>]] [--dry-run]
# --dry-run prints what would run and touches no device.
set -euo pipefail
cd "$(dirname "$0")/.."

IPHONE=0
DEVICE=""
DRY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --iphone) IPHONE=1; shift ;;
    --device) DEVICE="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    *) echo "Unknown argument $1" >&2; exit 2 ;;
  esac
done
OUT=build/layer-contents-probe
mkdir -p "$OUT"

if [[ "$IPHONE" == 1 ]]; then
  if [[ "$DRY" == 1 ]]; then
    echo "LAUNCH_ARGS=-runLayerProbe tools/deploy.sh ${DEVICE:-(the connected iPhone)}: a Debug build, its install"
    echo "and the probe on the key window; about 6 to 11 min. Report: $OUT/iphone.txt"
    exit 0
  fi
  LAUNCH_ARGS="-runLayerProbe" tools/deploy.sh ${DEVICE:+"$DEVICE"} 2>&1 | tee "$OUT/iphone.txt"
  grep -q "LAYER PROBE done" "$OUT/iphone.txt"
  exit
fi

if [[ "$DRY" == 1 ]]; then
  echo "the probe on the Mac, with a window of 300 x 650 points for about 30 s; report: $OUT/mac.txt"
  exit 0
fi
xcrun clang -fobjc-arc -O2 -Wall -Wextra -Werror -framework AppKit -framework Metal -framework QuartzCore \
  -framework IOSurface app/Spikes/LayerContentsProbe.m -o "$OUT/probe"
"$OUT/probe" | tee "$OUT/mac.txt"
