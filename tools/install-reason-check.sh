#!/usr/bin/env bash
# Checks what the installer tells an app that hands it APKs which cannot be installed over it (install-reason-test): a
# file that is no APK, the app itself signed with another key, and the app in an older version, each by an intent
# (ACTION_INSTALL_PACKAGE with EXTRA_RETURN_RESULT: EXTRA_INSTALL_RESULT) and by a PackageInstaller session (the final
# status). Runs on the Mac (framework/tests/java_app_run.py) and compares with
# fixtures/expected/install-reason-test.logcat, the log of Android 15. apk-run grants the right to install apps
# (APK_RUN_INSTALL_SOURCES=allow), as the user's switch does on Android.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/install-reason-check.sh
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
tools/build-java-test-app.sh install-reason-test >/dev/null
# The app starts with its signers, as the library writes them at the import: the installer compares an update's key.
swift build --package-path importer/APKKit --product apk-inspect >/dev/null
INSPECT="$(swift build --package-path importer/APKKit --show-bin-path)/apk-inspect"

package=io.github.kisame76.minato.test.installreason
export APK_RUN_INSTALL_SOURCES=allow
# The first run is Android's: the library takes the installable package (the host answers "ready"). Then the library
# refuses it with each kind of reason (the host's a2 of APKRT_EVENT_INSTALL_RESULT; Mac only, the expected files change
# the last two lines from Android's by the codes Android's installer gives for the reason).
for answer in ready failed:4 failed:5 failed:0; do
  case "$answer" in
    ready) expected=fixtures/expected/install-reason-test.logcat ;;
    failed:4) expected=fixtures/expected/install-reason-test-host-storage.logcat ;;
    failed:5) expected=fixtures/expected/install-reason-test-host-incompatible.logcat ;;
    failed:0) expected=fixtures/expected/install-reason-test-host-failed.logcat ;;
  esac
  echo "host answer: $answer"
  APK_RUN_INSTALL_ANSWER=$answer python3 framework/tests/java_app_run.py --apk-inspect "$INSPECT" install-reason-test \
    "$package" "$package.ReasonApplication" build/art \
    build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/install-reason-check \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    build/install-reason-test/install-reason-test.apk "$expected" 1290x2796@3,en-US
done
