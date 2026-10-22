#!/usr/bin/env bash
# Makes Minato's own H.264 test streams (ADR-0060 e2b) with VideoToolbox's encoder on the Mac
# (tools/media/make_h264_sample.m) into media-codec-test's assets: baseline.h264 and main.h264 with their indexes.
# The encoder's bytes may differ between macOS versions, so the streams are committed and only remade on purpose;
# the emulator's reference and Minato's run then decode the same bytes again.
# Usage: tools/make-h264-sample.sh [<output directory>]   (default: fixtures/apps/media-codec-test/assets)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-fixtures/apps/media-codec-test/assets}"
mkdir -p build/make-h264-sample "$OUT"
clang -fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox tools/media/make_h264_sample.m -o build/make-h264-sample/make-h264-sample
build/make-h264-sample/make-h264-sample "$OUT"
