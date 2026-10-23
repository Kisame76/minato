#!/usr/bin/env bash
# Makes Minato's own H.264 test streams (ADR-0060 e2b) with VideoToolbox's encoder on the Mac
# (tools/media/make_h264_sample.m) into media-codec-test's assets: baseline.h264 and main.h264 with their indexes.
# The encoder's bytes may differ between macOS versions, so the streams are committed and only remade on purpose;
# the emulator's reference and Minato's run then decode the same bytes again.
# With --bench, media-bench-test's streams instead (ADR-0060 e2c's iPhone measurement): hd.h264 at 1920 x 1080 and
# uhd.h264 at 3840 x 2160 with their indexes.
# Usage: tools/make-h264-sample.sh [--bench] [<output directory>]
#        (default: fixtures/apps/media-codec-test/assets, with --bench fixtures/apps/media-bench-test/assets)
set -euo pipefail
cd "$(dirname "$0")/.."
BENCH=()
DEFAULT=fixtures/apps/media-codec-test/assets
if [[ "${1:-}" == --bench ]]; then
  BENCH=(--bench)
  DEFAULT=fixtures/apps/media-bench-test/assets
  shift
fi
OUT="${1:-$DEFAULT}"
mkdir -p build/make-h264-sample "$OUT"
clang -fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox tools/media/make_h264_sample.m -o build/make-h264-sample/make-h264-sample
build/make-h264-sample/make-h264-sample ${BENCH[@]+"${BENCH[@]}"} "$OUT"
