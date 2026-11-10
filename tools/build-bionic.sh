#!/usr/bin/env bash
# Builds the guest libc (ADR-0021): fetches the pinned AOSP sources if needed, then builds bionic's
# ld-android.so, libdl.so, libc.so and libm.so with the NDK into build/bionic/lib. The guest's
# /system gets bionic's time zone file, packed from the Mac's tz database (tools/make-tzdata.py).
# Usage: tools/build-bionic.sh
# Optional environment:
#   ANDROID_NDK  NDK to build with, default: r30 in ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

python3 tools/fetch-sources.py
# Keep the Mac's build paths out of the guest libraries (tools/check-no-build-paths.sh enforces it).
pfx="$(tools/prefix-map-flags.sh "$NDK")"
cmake -S runtime/bionic -B build/bionic -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=none \
  -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/bionic "$@"
python3 tools/make-tzdata.py build/bionic/system/usr/share/zoneinfo/tzdata
# The stamp tools/check-guest-fresh.py compares with the sources: this build finished.
touch build/bionic/.built
