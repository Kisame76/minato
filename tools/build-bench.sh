#!/usr/bin/env bash
# Builds the parity benchmark's APKs (ADR-0037, decision 1): graphics/bench/android with the NDK for arm64-v8a,
# its libraries into build/graphics/bench/bin, then build/graphics/bench/bench-gles.apk and bench-vulkan.apk with
# tools/make-apk.sh, where tools/embed-benchmark.sh takes them from.
# Usage: tools/build-bench.sh
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

# API level 29 and RelWithDebInfo (-O2) like the test apps (tools/build-fixtures.sh).
out=build/graphics/bench
cmake -S graphics/bench/android -B "$out/ndk" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$PWD/$out/bin" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$out/ndk"

for app in bench-gles bench-vulkan; do
  tools/make-apk.sh --manifest "graphics/bench/android/$app/AndroidManifest.xml" --lib "$out/bin/lib$app.so" \
    --out "$out/$app.apk"
done
