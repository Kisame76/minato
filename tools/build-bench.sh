#!/usr/bin/env bash
# Builds the parity benchmark (ADR-0037, decision 1).
#   tools/build-bench.sh        the APKs: graphics/bench/android with the NDK for arm64-v8a, its libraries into
#                               build/graphics/bench/bin, then build/graphics/bench/bench-gles.apk,
#                               bench-vulkan.apk and bench-gles31.apk, bench-gles's library under an ES 3.1
#                               manifest (ADR-0044 d2)), with tools/make-apk.sh, and the digest of their sources
#                               as sources.sha256 (graphics/tests/bench_sources.py), which graphics.bench checks
#   tools/build-bench.sh --ios  the scene built for iOS: MinatoBench.framework for the device and the Simulator
#                               (graphics/bench with the presets ios-release and ios-simulator-release, in
#                               build/native/<preset>-bench), packed into build/MinatoBench.xcframework. It links
#                               build/ANGLE.xcframework's slices (tools/build-angle.sh).
# tools/embed-benchmark.sh takes both from there.
# Optional environment:
#   ANDROID_NDK  NDK to build with (its glslc compiles the shaders for iOS too), default: r30 in
#                ~/Library/Android/sdk/ndk
set -euo pipefail
cd "$(dirname "$0")/.."

ios=no
for argument in "$@"; do
  case "$argument" in
    --ios) ios=yes ;;
    *)
      echo "usage: tools/build-bench.sh [--ios]" >&2
      exit 2
      ;;
  esac
done

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
  exit 1
fi

# Keep the Mac's build paths out of the benchmark's libraries, which ship in the app's APKs and framework
# (tools/check-no-build-paths.sh enforces it on a release). The APK's .so ships raw -- it is converted on the
# device, not pre-stripped like the guest system libraries -- so its debug info must not carry the NDK
# toolchain path either (that path lies under $HOME); map it out alongside the build root.
pfx="$(tools/prefix-map-flags.sh) -ffile-prefix-map=$NDK/="

if [[ "$ios" == yes ]]; then
  for needed in build/ANGLE.xcframework/ios-arm64/libGLESv2.framework build/moltenvk/include/vulkan/vulkan.h; do
    if [[ ! -e "$needed" ]]; then
      echo "$needed is missing: run tools/build-angle.sh and tools/fetch-moltenvk.sh first." >&2
      exit 1
    fi
  done
  arguments=()
  for preset in ios-release ios-simulator-release; do
    build="build/native/$preset-bench"
    cmake --preset "$preset" -B "$build" -D APK_ANDROID_NDK="$NDK" \
      -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" >/dev/null
    cmake --build "$build" --target MinatoBench
    arguments+=(-framework "$build/graphics/bench/MinatoBench.framework")
  done
  rm -rf build/MinatoBench.xcframework
  xcodebuild -create-xcframework "${arguments[@]}" -output build/MinatoBench.xcframework >/dev/null
  echo "Packed build/MinatoBench.xcframework"
  exit 0
fi

# API level 29 and RelWithDebInfo (-O2) like the test apps (tools/build-fixtures.sh).
out=build/graphics/bench
cmake -S graphics/bench/android -B "$out/ndk" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$PWD/$out/bin" \
  -DCMAKE_C_FLAGS="$pfx" -DCMAKE_CXX_FLAGS="$pfx" -DCMAKE_ASM_FLAGS="$pfx" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$out/ndk"

for app in bench-gles bench-vulkan bench-gles31; do
  library=$app
  [[ "$app" != bench-gles31 ]] || library=bench-gles
  tools/make-apk.sh --manifest "graphics/bench/android/$app/AndroidManifest.xml" --lib "$out/bin/lib$library.so" \
    --lib "$out/bin/libbench-scripting.so" --out "$out/$app.apk"
done
python3 graphics/tests/bench_sources.py graphics/bench > "$out/sources.sha256"
