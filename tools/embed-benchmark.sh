#!/usr/bin/env bash
# Xcode build phase (project.yml): makes a Release build with APK_EMBED_BENCHMARK=YES the benchmark
# build of ADR-0037 (decision 3): MinatoBench.framework, the scene built for iOS, in Frameworks/ (the
# mode opens it with dlopen), and the scene's APKs in Benchmark/, imported at the first start like
# any APK. Every other build removes them; the mode itself is compiled only under BENCHMARK, which
# the same setting turns on. In a Debug build the setting is an error: it would measure -O0 code.
set -euo pipefail

if [[ "${APK_EMBED_BENCHMARK:-NO}" == "YES" && "$CONFIGURATION" != "Release" ]]; then
  echo "error: APK_EMBED_BENCHMARK=YES needs a Release build, not $CONFIGURATION (ADR-0037)" >&2
  exit 1
fi

FRAMEWORK="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH/MinatoBench.framework"
OUT="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/Benchmark"
rm -rf "$FRAMEWORK" "$OUT"
[[ "${APK_EMBED_BENCHMARK:-NO}" == "YES" ]] || exit 0

case "$PLATFORM_NAME" in
  iphoneos) SLICE=ios-arm64 ;;
  iphonesimulator) SLICE=ios-arm64-simulator ;;
  *) echo "error: no benchmark for $PLATFORM_NAME" >&2; exit 1 ;;
esac
BENCH="$SRCROOT/build/MinatoBench.xcframework/$SLICE/MinatoBench.framework"
APKS=("$SRCROOT/build/graphics/bench/bench-gles.apk" "$SRCROOT/build/graphics/bench/bench-vulkan.apk")

# Decision 3: a benchmark build links the Release builds of the runtime and of graphics/host
# (tools/build-native.sh --app --release, tools/build-graphics-host.sh --release), which note their
# configuration beside each xcframework.
for xcframework in APKRuntime APKConvert MinatoGraphics; do
  configuration=$(cat "$SRCROOT/build/$xcframework.xcframework.configuration" 2>/dev/null || echo none)
  if [[ "$configuration" != Release ]]; then
    echo "error: build/$xcframework.xcframework holds the $configuration build, not Release (ADR-0037)" >&2
    exit 1
  fi
done

# MinatoBench.framework from tools/build-bench.sh --ios, the APKs from tools/build-bench.sh.
for needed in "$BENCH" "${APKS[@]}"; do
  if [[ ! -e "$needed" ]]; then
    echo "error: $needed is missing: run tools/build-bench.sh and tools/build-bench.sh --ios (ADR-0037)" >&2
    exit 1
  fi
done
mkdir -p "$(dirname "$FRAMEWORK")" "$OUT"
cp -R "$BENCH" "$FRAMEWORK"
# The headers are for the build; the mode opens the library by dlopen.
rm -rf "$FRAMEWORK/Headers"
codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" "$FRAMEWORK"
cp "${APKS[@]}" "$OUT/"
