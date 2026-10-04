#!/usr/bin/env bash
# Runs ADR-0039 step c1's tests on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS
# into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s)
# and the two apps (seconds) every time, so it never tests an old binary; then compares each app's log with Android
# 15's (fixtures/expected/<app>.logcat, from tools/android-reference.sh --app <app>):
#   - context-test, started twice over the same data: the Context's folders, files, SharedPreferences, a string, an
#     asset and permission checks, from Application.attachBaseContext;
#   - looper-test: a HandlerThread's Looper on MessageQueue's natives.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/context-check.sh
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
# <app> <package> <application class> [java_app_run.py's options]
check() {
  local app=$1 package=$2 application=$3
  shift 3
  tools/build-java-test-app.sh "$app" >/dev/null
  python3 framework/tests/java_app_run.py "$@" "$app" "$package" "$application" build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" || failed=1
}
check context-test io.github.kisame76.minato.test.context io.github.kisame76.minato.test.context.ContextApplication \
  --restart
check looper-test io.github.kisame76.minato.test.looper io.github.kisame76.minato.test.looper.LooperApplication
exit "$failed"
