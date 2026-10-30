#!/usr/bin/env bash
# Runs the return key's test (ADR-0068) on the Mac (framework/tests/java_app_run.py), for enter-test (target 35) and
# enter-test-15 (target 15, before Jelly Bean): it builds apk-run and apk-convert for macOS into
# build/native/macos-app, app_process with libandroid_runtime.so (incremental), framework.jar and the apps every time,
# then starts each app through app_process in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px
# at scale 3 in en-US. The host plays the calls Minato's keyboard makes from UIKit's keys for each of the app's seven
# fields (SoftKeyboard, as LatinIME makes them), one field after another as the app moves the focus: the Enter of a
# plain single-line field, of one with the Send action, of one with an action label (actionId 42, which the start
# carries), of a multi-line field (a new line, the ENTER key before Jelly Bean), a number field's digit as its key and
# its delete (the DEL key before Jelly Bean), and the Enter of an engine's view with imeOptions 0 and of a view without
# an editor (IME_ACTION_UNSPECIFIED, performed). The label field's start must carry its actionId and label, the
# engine's its imeOptions 0. Each app's log is compared with fixtures/expected/<app>.logcat, Android
# 15's with the fixture input method (tools/android-reference.sh).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/enter-check.sh [app...]   (both apps by default)
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

# The text records of each field (APKRT_TEXT_*: 0 commit, 3 delete around, 6 editor action, 7 key), in the fields'
# order: plain, send, label, multi, number, engine, null.
newline=$'\n'
apps=("$@")
[[ ${#apps[@]} -gt 0 ]] || apps=(enter-test enter-test-15)
for app in "${apps[@]}"; do
  tools/build-java-test-app.sh "$app" >/dev/null
  steps=start
  steps+=,text:0:1:0:x,text:6:5:0:
  steps+=,text:0:1:0:y,text:6:4:0:
  steps+=,text:6:42:0:
  if [[ "$app" == enter-test ]]; then
    steps+=",text:0:1:0:a,text:0:1:0:$newline,text:0:1:0:b,text:3:1:0:"
    steps+=,text:7:14:0:,text:3:1:0:
  else
    steps+=,text:0:1:0:a,text:7:66:0:,text:0:1:0:b,text:7:67:0:
    steps+=,text:7:14:0:,text:7:67:0:
  fi
  steps+=,text:0:1:0:z,text:6:0:0:
  steps+=,text:7:12:0:,text:6:0:0:,text:7:67:0:
  python3 framework/tests/java_app_run.py \
    --in-order 'apk-run: ime 4: start\t1\t201326597\t0\t0\t0\t3\t42\t1\t' \
    --in-order 'apk-run: ime 7: start\t1\t0\t0\t-1\t-1\t6\t0\t0\t' \
    --lifecycle "$steps" \
    "$app" io.github.kisame76.minato.test.enter android.app.Application \
    build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
done
