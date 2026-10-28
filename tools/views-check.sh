#!/usr/bin/env bash
# Runs ADR-0043's test 1 (views-test) as step c3 cuts it, on the Mac (framework/tests/java_app_run.py): builds
# graphics-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and the app (seconds) every time, so it never tests an old binary. Then
# views-test starts under graphics-run in a session on the M4 plan's display, the iPhone 15 Pro Max's 1290 x 2796 px at
# scale 3 in en-US, whose host plays a start (apk-run --lifecycle start), with graphics/tests/window_host.m's fake
# buffers, which write each post of the window into build/views-check/dump (MINATO_WINDOW_BUFFERS=fake,
# MINATO_WINDOW_DUMP). Its lifecycle plays a tap on the Button's middle, from its bounds in that log, which must click
# it from the touch (views-test-device's line), as the AVD's adb shell input tap does (test 1's click). Its log is
# compared with Android 15's (fixtures/expected/views-test.logcat), and its posts with
# Android's screen by framework/tests/views_compare.py (fixtures/expected/views-test.rgba.gz and views-test.rows); all
# three come from tools/android-reference.sh --app views-test.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-front-libs.sh, tools/build-aapt2.sh
# and tools/build-framework-res.sh first.
# Usage: tools/views-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

APP=views-test
PACKAGE=io.github.kisame76.minato.test.views
for expected in fixtures/expected/$APP.logcat fixtures/expected/$APP.rgba.gz fixtures/expected/$APP.rows; do
  if [[ ! -f "$expected" ]]; then
    echo "FAIL: no $expected: run tools/android-reference.sh --app $APP"
    exit 1
  fi
done

NATIVE=build/native/macos-app
WORK=build/views-check
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
tools/build-java-test-app.sh $APP >/dev/null

# views_compare.py reads the run's posts from the dump's posts.txt, which starts empty, so that a run without a post
# leaves none of an earlier run's.
mkdir -p "$WORK/dump"
: >"$WORK/dump/posts.txt"
# The Button's middle in the window, from Android's line of its bounds.
read -r left top right bottom <<<"$(sed -n 's/.*: button bounds \([0-9 ]*\)$/\1/p' fixtures/expected/$APP.logcat)"
if [[ -z "$bottom" ]]; then
  echo "FAIL: fixtures/expected/$APP.logcat has no button bounds: run tools/android-reference.sh --app $APP"
  exit 1
fi
failed=0
MINATO_WINDOW_BUFFERS=fake MINATO_WINDOW_DUMP="$PWD/$WORK/dump" python3 framework/tests/java_app_run.py \
  --lifecycle "start,tap:$(((left + right) / 2)):$(((top + bottom) / 2))" \
  --in-order "I/views-test-device: click from a touch" $APP $PACKAGE \
  android.app.Application build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" "$WORK" build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/$APP/$APP.apk fixtures/expected/$APP.logcat 1290x2796@3,en-US || failed=1
python3 framework/tests/views_compare.py fixtures/expected/$APP.rgba.gz fixtures/expected/$APP.rows \
  fixtures/expected/$APP.logcat "$WORK/dump" || failed=1
exit "$failed"
