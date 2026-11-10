#!/usr/bin/env bash
# Runs the provider contracts' test on the Mac (group 2 of the framework breadth, framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and provider-contracts-test (seconds) every time, so it never tests an old
# binary; then starts provider-contracts-test through app_process, whose Application loads each type of its
# assets/types.txt and logs its shape, and compares its log with Android 15's (fixtures/expected/
# provider-contracts-test.logcat, from tools/android-reference.sh --app provider-contracts-test): the types load with
# their API's shapes, and an app without the calendar, call log and voicemail permissions is refused as on a phone.
# The twin provider-contracts-test-granted (fixtures/expected/provider-contracts-test-granted.logcat) holds those
# permissions, which Minato grants no app: the harness grants them with MINATO_TEST_GRANTS (ManifestPackage; only here,
# tools/test-grants-check.sh pins that the iPhone never sets it), and the providers answer a holder with no rows.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/provider-contracts-check.sh
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
tools/build-java-test-app.sh provider-contracts-test >/dev/null
tools/build-java-test-app.sh provider-contracts-test-granted >/dev/null

python3 framework/tests/java_app_run.py provider-contracts-test io.github.kisame76.minato.test.providercontracts \
  io.github.kisame76.minato.test.providercontracts.ContractsApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/provider-contracts-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/provider-contracts-test/provider-contracts-test.apk \
  fixtures/expected/provider-contracts-test.logcat

GRANTS=android.permission.READ_CALENDAR,android.permission.WRITE_CALENDAR,android.permission.READ_CALL_LOG
GRANTS=$GRANTS,android.permission.WRITE_CALL_LOG,com.android.voicemail.permission.ADD_VOICEMAIL
python3 framework/tests/java_app_run.py --env "MINATO_TEST_GRANTS=$GRANTS" provider-contracts-test-granted \
  io.github.kisame76.minato.test.providercontractsgranted \
  io.github.kisame76.minato.test.providercontracts.ContractsApplication build/art "$BIONIC/lib" "$BIONIC/system" \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/provider-contracts-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/provider-contracts-test-granted/provider-contracts-test-granted.apk \
  fixtures/expected/provider-contracts-test-granted.logcat
