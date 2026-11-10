#!/usr/bin/env bash
# Runs the starting window's test (ADR-0076) on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar and starting-window-test every time, so it never tests an old binary; then cold-starts each activity of
# starting-window-test in a session (a notification's MINATO_START_INTENT picks the activity) and checks that the
# framework sends the host the splash colour that Android 15 shows for it (APKRT_REQUEST_STARTING_WINDOW, which
# apk-run logs once), and that the app logs what Android 15 logs up to the activity's first frame.
# The colours are Android 15's on apk-runner-35 in day mode, read from the screen 1.2 s after a cold start (2026-11-10):
# windowSplashScreenBackground's, windowBackground's, Theme.Material.Light's #FAFAFA and the day default #FFFFFF.
# The launcher runs last: its setSplashScreenTheme is kept for the app's later starts, as on Android, and each
# activity starts in a work folder of its own, so no start sees another's kept theme.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# HWUI (default build/hwui) is the hwui build to run with.
# Usage: tools/starting-window-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py "${HWUI:-build/hwui}/lib" build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh starting-window-test >/dev/null

PACKAGE=io.github.kisame76.minato.test.startingwindow
WORK=build/starting-window-check
mkdir -p "$WORK"
failed=0
while read -r activity colour; do
  expected="$WORK/$activity.logcat"
  : >"$expected"
  if [[ $activity == SplashColorActivity ]]; then
    printf 'I/starting-window-test: %s\n' "getSplashScreen: true" "setOnExitAnimationListener: returned" \
      "clearOnExitAnimationListener: returned" "setSplashScreenTheme: returned" >>"$expected"
  fi
  printf 'I/starting-window-test: %s: created\nI/starting-window-test: %s: drawn\n' "$activity" "$activity" \
    >>"$expected"
  start=()
  [[ $activity == SplashColorActivity ]] ||
    start=(--env "MINATO_START_INTENT=intent:#Intent;component=$PACKAGE/.$activity;end")
  rm -rf "${WORK:?}/$activity"
  if ! python3 framework/tests/java_app_run.py ${start[@]+"${start[@]}"} --until "$activity: drawn" \
      --once "apk-run: starting window request: $colour icon none" \
      --never "The session refused STARTING_WINDOW" starting-window-test "$PACKAGE" android.app.Application \
      build/art build/bionic/lib build/bionic/system "${HWUI:-build/hwui}/lib" \
      build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
      "$NATIVE/runtime/apk-run" "$WORK/$activity" build/framework-native/app_process64 build/framework/framework.jar \
      build/framework-res/framework-res build/starting-window-test/starting-window-test.apk "$expected" \
      1290x2796@3,en-US; then
    echo "FAIL: $activity (Android 15's splash: $colour)"
    failed=1
  fi
done <<'EOF'
PlainActivity #FAFAFA
WindowBackgroundActivity #1565C0
NoBackgroundActivity #FFFFFF
SplashColorActivity #2E7D32
EOF
exit $failed
