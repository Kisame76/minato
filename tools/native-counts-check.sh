#!/usr/bin/env bash
# Checks the Debug line of Get Info over the converter's counts (app/Sources/Signing/DebugNativeCounts.swift): builds
# APKConvert's library for macOS into build/native/macos-app and runs app/HostChecks/NativeCounts/main.swift, compiled
# with the Debug flag as the Debug app is, against an ELF file with svc words (the NDK fixture raw-hello).
# Needs the NDK fixtures built (build/fixtures/bin/raw-hello).
# Usage: tools/native-counts-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NATIVE=build/native/macos-app
OUT=build/native-counts-check
FIXTURE=${FIXTURE:-build/fixtures/bin/raw-hello}
[[ -f "$FIXTURE" ]] || { echo "native-counts-check: no fixture at $FIXTURE (build the NDK fixtures)" >&2; exit 2; }
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apkconv >/dev/null
rm -rf "$OUT"
mkdir -p "$OUT"
# The macos preset builds the library with AddressSanitizer, as the program must be.
swiftc -sanitize=address -D DEBUG -module-cache-path "$OUT/modules" -I importer/converter/capi/APKConvert \
  -L "$NATIVE/importer/converter" -lapkconv \
  app/Sources/Signing/DebugNativeCounts.swift app/HostChecks/NativeCounts/main.swift -o "$OUT/native-counts-check"
"$OUT/native-counts-check" "$FIXTURE"
echo "native counts check: fine"
