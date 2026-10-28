#!/usr/bin/env bash
# Runs the tests of ADR-0062 (the intents that leave the app for iOS) on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and the two apps (seconds) every time, so it never tests an old binary; then
#   - open-test, the intents that resolve and those that do not, whose log must be what Android 15 logs on apk-runner-35
#     (fixtures/expected/open-test.logcat, from tools/android-reference.sh --app open-test); and
#   - open-start-test, which starts each kind of intent for a result: apk-run answers each request with RESULT_OK
#     after 300 ms and writes what the host got ("apk-run: open <ID>: <kind>\t<decoded fields>"), which is checked in
#     order, and the app's log (fixtures/expected/open-start-test.logcat) says the results and that no share folder is
#     left. Runtime-only: on Android each intent would open an app of the emulator.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/open-check.sh
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
tools/build-java-test-app.sh open-test >/dev/null
tools/build-java-test-app.sh open-start-test >/dev/null

python3 framework/tests/java_app_run.py open-test io.github.kisame76.minato.test.open \
  io.github.kisame76.minato.test.open.OpenApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/open-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/open-test/open-test.apk fixtures/expected/open-test.logcat

# What the host got for each intent (apk-run writes a request's decoded fields on one line, tabs and newlines as \t and
# \n): a link with a space and a non-ASCII letter, a phone number without its spaces, a map point, a mail with the
# addresses of the URL and of the extras, a text message, a chooser around a text with an emoji, a share of a file,
# and of two files whose second name has a slash, each file's bytes as the host reads them from the share folder
# (--shares: the provider's "the report\n" and "PNG?"); the market link resolves to nothing, so no request.
python3 framework/tests/java_app_run.py --lifecycle start --shares \
  --in-order 'apk-run: open 1: 1\thttps://example.com/a%20b?q=ü' \
  --in-order 'apk-run: open 2: 1\ttel:+49301234' \
  --in-order 'apk-run: open 3: 1\tmaps://?ll=52.52%2C13.40&q=Berlin' \
  --in-order 'apk-run: open 4: 2\tmailto:a@example.com,b@example.com?subject=Hi%20there&body=line1%0Aline2' \
  --in-order 'apk-run: open 5: 3\tsms:123&body=Hello%20%26%20bye' \
  --in-order 'apk-run: open 6: 4\tSub\tText 😀\t0' \
  --in-order 'apk-run: open 7: 4\t\twith a file\t1\treport.txt\ttext/plain' \
  --in-order 'apk-run: open 7 file report.txt: 11 bytes the report\n' \
  --in-order 'apk-run: open 8: 4\t\t\t2\treport.txt\ta b_c.png\ttext/plain\timage/png' \
  --in-order 'apk-run: open 8 file report.txt: 11 bytes the report\n' \
  --in-order 'apk-run: open 8 file a b_c.png: 4 bytes PNG?' \
  open-start-test io.github.kisame76.minato.test.openstart \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/open-start-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/open-start-test/open-start-test.apk fixtures/expected/open-start-test.logcat \
  1290x2796@3,en-US
