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

# Until ADR-0037 a) and b) (Grafik) deliver the framework and the APKs, a missing one is only a
# warning, so the mode and the CSVs can be tested; from then on it is an error. The paths above
# follow Grafik's a) plan.
missing() {
  echo "warning: $1 is missing; it comes with ADR-0037 a) and b)" >&2
}

if [[ -d "$BENCH" ]]; then
  mkdir -p "$(dirname "$FRAMEWORK")"
  cp -R "$BENCH" "$FRAMEWORK"
  codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" "$FRAMEWORK"
else
  missing "$BENCH"
fi
for apk in "${APKS[@]}"; do
  if [[ -f "$apk" ]]; then
    mkdir -p "$OUT"
    cp "$apk" "$OUT/"
  else
    missing "$apk"
  fi
done
