#!/usr/bin/env bash
# Runs ADR-0068's test of the host's text bar on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar and ime-extract-test (seconds) every time, so it never tests an old binary; then starts ime-extract-test
# through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US.
# The guest asks its note field for its extracted text with GET_EXTRACTED_TEXT_MONITOR as a keyboard with a text bar
# does, and tells the host where the field is (field) and what it holds (extract, with where its window of the text
# starts); the host plays a commit, a composing text, its end and a selection (apk-run's text steps).
# The app's log is compared with
# fixtures/expected/ime-extract-test.logcat, which is the Android 15 emulator's with the fixture input method making the
# same calls (tools/android-reference.sh), and the host must have heard of the field and of each extract in order, whose
# selections and composing regions are those the fixture method's updates carried (ime-extract-test-device's lines);
# the password field's text goes as bullets only, and its own never reaches the host's lines.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/ime-extract-check.sh
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
tools/build-java-test-app.sh ime-extract-test >/dev/null

head='\t5\tnote\t'
steps='start,text:0:1:0:!,text:1:1:0:ab,text:2:0:0:,text:4:2:4:'
python3 framework/tests/java_app_run.py \
  --in-order 'apk-run: ime 1: start\t131073\t' \
  --in-order 'apk-run: ime 1: field\t1\t0\t0\t1290\t' \
  --in-order 'apk-run: ime 1: stop' --in-order 'apk-run: ime 2: start\t131073\t' \
  --in-order 'apk-run: ime 2: field\t2\t0\t0\t1290\t' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t8\t8\t-1\t-1'"$head"'héllo 😀' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t9\t9\t-1\t-1'"$head"'héllo 😀!' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t11\t11\t9\t11'"$head"'héllo 😀!ab' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t11\t11\t-1\t-1'"$head"'héllo 😀!ab' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t2\t4\t-1\t-1'"$head"'héllo 😀!ab' \
  --in-order 'apk-run: ime 2: stop' --in-order 'apk-run: ime 3: start\t129\t' \
  --in-order 'apk-run: ime 3: extract\t3\t0\t6\t6\t-1\t-1\t3\t\t••••••' \
  --never 'secret' \
  --lifecycle "$steps" \
  ime-extract-test io.github.kisame76.minato.test.imeextract android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/ime-extract-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/ime-extract-test/ime-extract-test.apk fixtures/expected/ime-extract-test.logcat 1290x2796@3,en-US
