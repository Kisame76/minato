#!/usr/bin/env bash
# Runs turn-test (what an activity hears when the display turns) on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, app_process with libandroid_runtime.so (incremental),
# framework.jar and the app every time, then starts the app through app_process on the M4 plan's display and turns the
# window with apk-run's turn steps as the App does (ADR-0038): to landscape and back for PlainActivity, which has no
# android:configChanges and is relaunched (onPause, onStop, onSaveInstanceState, onDestroy, then a new instance with
# the saved state), and again for ConfigActivity, which has them and hears onConfigurationChanged and a relayout.
# Its log is compared with Android 15's (fixtures/expected/turn-test.logcat, from tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/turn-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

app=turn-test
tools/build-java-test-app.sh "$app" >/dev/null
# The app says "ready for turn N" half a second after its focus; a turn comes a second after the step before it, and
# the pause gives the first activity time to start the second.
steps=start,turn:2796:1290,turn:1290:2796,pause:2000,turn:2796:1290,turn:1290:2796
python3 framework/tests/java_app_run.py \
  --lifecycle "$steps" --until "config 4 done" \
  "$app" io.github.kisame76.minato.test.turn android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
