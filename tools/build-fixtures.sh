#!/usr/bin/env bash
# Builds the NDK test programs in fixtures/ for arm64-v8a into build/fixtures/bin (ADR-0016).
# Usage: tools/build-fixtures.sh
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

# API level 29 (Android 10): the first with native ELF TLS, and below 30 like the minSdk of most apps,
# so the NDK links with --no-rosegment and read-only data shares the executable segment (risk R1).
cmake -S fixtures -B build/fixtures -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build/fixtures
