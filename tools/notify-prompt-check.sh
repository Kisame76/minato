#!/usr/bin/env bash
# Runs the test of the notification prompt for an app below targetSdk 33 on the Mac (N1b;
# framework/tests/java_app_run.py):
# notify-prompt-test creates a notification channel while its activity is in front, and on Android 13 and later the
# system then asks the user for POST_NOTIFICATIONS in a dialog over the activity (the activity pauses; the permission,
# the enabled state and the importance stay denied, false and 0 until the user answers). It builds apk-run and
# apk-convert for macOS into build/native/macos-app (or $NATIVE), and app_process, framework.jar and the app every
# time, and compares the log with Android 15's (fixtures/expected/notify-prompt-test.logcat, from
# tools/android-reference.sh --app notify-prompt-test). The run has a display configuration, so it has a session.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/notify-prompt-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=${NATIVE:-build/native/macos-app}
BIONIC=${BIONIC:-build/bionic}
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh notify-prompt-test >/dev/null

python3 framework/tests/java_app_run.py --lifecycle start \
  notify-prompt-test io.github.kisame76.minato.test.notifyprompt \
  android.app.Application build/art "$BIONIC/lib" "$BIONIC/system" build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/notify-prompt-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/notify-prompt-test/notify-prompt-test.apk fixtures/expected/notify-prompt-test.logcat 1290x2796@3,en-US
