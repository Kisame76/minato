#!/usr/bin/env bash
# Runs ADR-0039's test 2 (java-test) as ADR-0043 step c1 cuts it, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and the app (seconds) every time, so it never tests an old binary. Then
# the app starts twice over the same data, in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x
# 2796 px at scale 3 in en-US, whose host plays a start, a background and a return (apk-run --lifecycle) and quits after
# the app's finish. Its log is compared with Android 15's on that display (fixtures/expected/java-test.logcat, from
# tools/android-reference.sh --app java-test, which sends HOME, starts the app again, lets it finish, then
# force-stops and starts it once more). Then java-test starts without a configuration, so without a session, where
# the launcher activity starts by itself as before ADR-0043 step c1, up to its onResume, against the reference's
# lines up to there (java_app_run.py --until).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/java-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

failed=0
tools/build-java-test-app.sh java-test >/dev/null
python3 framework/tests/java_app_run.py --restart --lifecycle start,background,foreground java-test \
  io.github.kisame76.minato.test.javatest android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/java-test-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/java-test/java-test.apk fixtures/expected/java-test.logcat \
  1290x2796@3,en-US || failed=1
python3 framework/tests/java_app_run.py --until onResume java-test io.github.kisame76.minato.test.javatest \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/java-test-nosession-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res build/java-test/java-test.apk \
  fixtures/expected/java-test.logcat || failed=1
exit "$failed"
