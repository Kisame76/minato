#!/usr/bin/env bash
# Checks what an app gets from a PackageInstaller session (install-session-test): a broken APK is refused at once,
# a valid one (the app's own) asks the user. Runs on the Mac (framework/tests/java_app_run.py) and compares with
# fixtures/expected/install-session-test.logcat, the log of Android 15.
# It builds apk-run and apk-convert into build/native/macos-app, app_process, framework.jar and the test every time.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/install-session-check.sh
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
tools/build-java-test-app.sh install-session-test >/dev/null

output=$(python3 framework/tests/java_app_run.py --lifecycle start install-session-test \
  io.github.kisame76.minato.test.installsession io.github.kisame76.minato.test.installsession.SessionApplication build/art \
  build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/install-session-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/install-session-test/install-session-test.apk fixtures/expected/install-session-test.logcat 1290x2796@3,en-US) || { echo "$output"; exit 1; }
echo "$output"
# Minato's own lines, shown as device lines (Android has no counterpart): the session folder lies in the data folder,
# a set with a broken split is refused as one APK would be (STATUS_FAILURE_INVALID = 4), and a confirm that ends without the host's
# answer reports STATUS_FAILURE_ABORTED (3) to the sender.
for line in 'session folder inside the data folder: true' 'case 3 status: 4' 'confirm ended: status 3'; do
  grep -q "$line" <<<"$output" || { echo "FAIL: no device line \"$line\""; exit 1; }
done
