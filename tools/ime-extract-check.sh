#!/usr/bin/env bash
# Runs ADR-0068's test of the host's text bar on the Mac (framework/tests/java_app_run.py): builds apk-run and
# apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental),
# framework.jar and ime-extract-test (seconds) every time, so it never tests an old binary; then starts ime-extract-test
# through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at scale 3 in en-US.
# The guest asks its note field for its extracted text without GET_EXTRACTED_TEXT_MONITOR as a keyboard with a text bar
# that is not extracting does (monitoring would end a selection's action mode), and tells the host where the field is
# (field) and what it holds (extract, with where its window of the text starts); the host plays a commit, a composing
# text, its end and a selection (apk-run's text steps).
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
tools/build-java-test-app.sh ime-surrounding-test >/dev/null
tools/build-java-test-app.sh ime-prefill-test >/dev/null

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

# An engine's field (ime-surrounding-test, as SDL2's DummyEdit in Ren'Py): its connection gives no extracted text, so
# the bar is fed from getTextBeforeCursor, getSelectedText and getTextAfterCursor, as Android's input methods read
# such a field, again after each of the host's edits.
surrounding='\t1\t\t'
python3 framework/tests/java_app_run.py \
  --in-order 'apk-run: ime 1: start\t1\t' --in-order 'apk-run: ime 1: stop' \
  --in-order 'apk-run: ime 2: start\t1\t' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t0\t0\t-1\t-1'"$surrounding" \
  --in-order 'apk-run: ime 2: extract\t2\t0\t3\t3\t-1\t-1'"$surrounding"'Ren' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t6\t6\t-1\t-1'"$surrounding"'Renpy!' \
  --in-order 'apk-run: ime 2: extract\t2\t0\t5\t5\t-1\t-1'"$surrounding"'Renpy' \
  --lifecycle 'start,text:0:1:0:Ren,text:0:1:0:py!,text:3:1:0:' \
  ime-surrounding-test io.github.kisame76.minato.test.imesurrounding android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/ime-surrounding-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/ime-surrounding-test/ime-surrounding-test.apk fixtures/expected/ime-surrounding-test.txt 1290x2796@3,en-US

# A field's text before the keyboard comes (ime-prefill-test, four fields in turn): the bar is given what Android's
# keyboard reads (fixtures/expected/ime-prefill-test.logcat, LatinIME on apk-runner-35): the text set before the focus,
# the text the app sets while the field is served, an empty field with its hint, and an SDL2-style field, which hands
# a keyboard no text of the game's own.
python3 framework/tests/java_app_run.py \
  --in-order 'apk-run: ime 2: extract\t2\t0\t6\t6\t-1\t-1\t1\t\tEileen' \
  --in-order 'apk-run: ime 3: extract\t3\t0\t0\t0\t-1\t-1\t1\t\t' \
  --in-order 'apk-run: ime 3: extract\t3\t0\t4\t4\t-1\t-1\t1\t\tLucy' \
  --in-order 'apk-run: ime 4: start\t1\t469762053\t0\t0\t0\t-1\t0\t0\tYour name' \
  --in-order 'apk-run: ime 4: extract\t4\t0\t0\t0\t-1\t-1\t1\tYour name\t' \
  --in-order 'apk-run: ime 5: extract\t5\t0\t0\t0\t-1\t-1\t1\t\t' \
  --lifecycle start \
  ime-prefill-test io.github.kisame76.minato.test.imeprefill android.app.Application \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" build/ime-prefill-check \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
  build/ime-prefill-test/ime-prefill-test.apk fixtures/expected/ime-prefill-test.txt 1290x2796@3,en-US
