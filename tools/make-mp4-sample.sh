#!/usr/bin/env bash
# Makes ADR-0060 e2d's ISO BMFF test files (tools/media/make_mp4_sample.m) from Minato's own H.264 streams in
# media-codec-test's assets, by AVAssetWriter without re-encoding: baseline.mp4 (baseline's frames and an AAC track of
# a sine of ours) and main.mp4 (main's frames, with composition offsets). The AAC encoder's bytes may differ between
# runs and macOS versions, so the files are committed and only remade on purpose.
# Usage: tools/make-mp4-sample.sh [<output directory>]   (default: fixtures/apps/media-extractor-test/assets)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-fixtures/apps/media-extractor-test/assets}"
mkdir -p build/make-mp4-sample "$OUT"
clang -fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation -framework AVFoundation -framework CoreMedia \
  tools/media/make_mp4_sample.m -o build/make-mp4-sample/make-mp4-sample
build/make-mp4-sample/make-mp4-sample fixtures/apps/media-codec-test/assets "$OUT"
