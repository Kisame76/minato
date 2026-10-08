#!/usr/bin/env bash
# Runs ADR-0039's test of the main thread's uncaught exception on the Mac (framework/tests/java_app_run.py): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and crash-test (seconds) every time, so it never tests an old binary; then
# starts crash-test through app_process in a session on the M4 plan's display, as tools/launch-mode-check.sh does.
# Its activity throws from a message on the main thread, and its own default handler, as a crash reporter's, logs the
# exception and hands it on to Android's. Its log must be Android 15's (fixtures/expected/crash-test.logcat, from
# tools/android-reference.sh --app crash-test), and the process's log must hold what Android 15 logs around it, in
# its order: AndroidRuntime's "Shutting down VM" once the exception leaves ActivityThread.main, RuntimeInit's FATAL
# EXCEPTION lines with the process and the exception, the app's handler, then the kill of itself, so that the run ends
# by itself with SIGKILL's status, 137 (128 + 9) through apk-run's session.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/crash-check.sh
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
tools/build-java-test-app.sh crash-test >/dev/null

python3 framework/tests/java_app_run.py --status 137 \
  --in-order "D/AndroidRuntime: Shutting down VM" \
  --in-order "E/AndroidRuntime: FATAL EXCEPTION: main" \
  --in-order "E/AndroidRuntime: Process: io.github.kisame76.minato.test.crash, PID: " \
  --in-order "E/AndroidRuntime: java.lang.IllegalStateException: thrown on the main thread" \
  --in-order "I/crash-test: uncaught on main: java.lang.IllegalStateException: thrown on the main thread" \
  --in-order "I/Process : Sending signal. PID: " \
  crash-test io.github.kisame76.minato.test.crash io.github.kisame76.minato.test.crash.CrashApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/crash-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/crash-test/crash-test.apk fixtures/expected/crash-test.logcat 1290x2796@3,en-US
