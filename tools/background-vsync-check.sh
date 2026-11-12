#!/usr/bin/env bash
# Checks the vsync in the background (apkrt_vsync_background) on the Mac with java-test: apk-run's host holds its frame
# clock in the background, as the App's display link does not fire there, and answers the guest's vsync requests
# instead, as the App does for a session on the Java path. Android delivers a requested vsync to a stopped app
# (SurfaceFlinger's EventThread has no gate for it), and ViewRootImpl's sync barrier goes only with a delivered vsync:
# without the answer, SAVE_INSTANCE_STATE after STOP waits behind a scheduled traversal until the return. The check
# starts java-test twice over the same data, as tools/java-check.sh does, sends it to the background and back,
# compares its log with Android 15's (fixtures/expected/java-test.logcat) and asks that apk-run saw
# SAVE_INSTANCE_STATE answered within 100 ms.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/background-vsync-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh java-test >/dev/null

log=build/background-vsync-check.log
rm -f "$log"
failed=0
python3 framework/tests/java_app_run.py --log "$log" --restart --lifecycle start,background,foreground --drawn \
  java-test io.github.kisame76.minato.test.javatest android.app.Application build/art build/bionic/lib \
  build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/background-vsync-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/java-test/java-test.apk \
  fixtures/expected/java-test.logcat 1290x2796@3,en-US || failed=1
answered=$(sed -n 's/^apk-run: SAVE_INSTANCE_STATE was answered after \([0-9]*\) ms$/\1/p' "$log" | head -n 1)
if [[ -z "$answered" ]]; then
  echo "FAIL: SAVE_INSTANCE_STATE in the background was not answered"
  failed=1
elif (( answered >= 100 )); then
  echo "FAIL: SAVE_INSTANCE_STATE in the background was answered after $answered ms (at most 100)"
  failed=1
else
  echo "ok: SAVE_INSTANCE_STATE in the background was answered after $answered ms"
fi
exit "$failed"
