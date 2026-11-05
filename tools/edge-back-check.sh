#!/usr/bin/env bash
# Runs ADR-0041 decision 10 step b2's edge back gesture on the Mac (framework/tests/java_app_run.py): edge-back-test
# and its landscape twin start through app_process in a session at the M4 plan's display, turned for the twin, and
# apk-run's lifecycle plays the swipes that tools/android-reference.sh plays on apk-runner-35 in gesture navigation,
# 1.5 s apart: from the left edge at stepped distances, a long way from 2 dp in stepped lengths, one short and fast,
# the same from the right edge, one in the excluded band, one near the top, one mostly down, then a tap in the middle.
# Each app's lines must be Android 15's (fixtures/expected/edge-back-test*.logcat). With the keyboard up, the edge back
# is the BACK key's on Android 15 (measured on apk-runner-35 in gesture navigation with LatinIME): ime-hide-test and its
# twin ime-hide-test-cb, with an edge swipe where tools/ime-hide-check.sh plays the user's hide, must log what Android
# logs for the key (fixtures/expected/ime-hide-test*.logcat): the keyboard goes, a view that has not opted in sees BACK
# in onKeyPreIme, and the app's back is not invoked. Builds apk-run and apk-convert for
# macOS into build/native/macos-app, app_process, framework.jar and the apps every time, as tools/permission-check.sh
# does. Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/edge-back-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null

# The steps for a display of $1 x $2 px at 3 px per dp, as android-reference.sh's for edge-back-test.
steps() {
  local w=$1 h=$2 y=$(($2 * 7 / 10)) x length list="start,pause:2000"
  swipe() { list+=",swipe:$1:$2:$3:$4:$5,pause:1500"; }
  for x in 2 6 10 14 18 22 26 30 36 44; do swipe $((x * 3)) $y $(((x + 250) * 3)) $y 300; done
  for length in 10 20 30 40 50 60 80 100 140; do swipe 6 $y $(((2 + length) * 3)) $y 600; done
  swipe 6 $y 96 $y 60
  for x in 2 10 18 26 36; do swipe $((w - x * 3)) $y $((w - (x + 250) * 3)) $y 300; done
  swipe 6 $((h * 45 / 100)) 750 $((h * 45 / 100)) 300
  swipe 6 120 750 120 300
  swipe 6 $((h / 2 + 150)) 120 $((h - 300)) 300
  echo "$list,tap:$((w / 2)):$((h / 2))"
}

failed=0
for app in edge-back-test edge-back-test-landscape; do
  tools/build-java-test-app.sh "$app" >/dev/null
  if [[ "$app" == *landscape ]]; then size=(2796 1290); else size=(1290 2796); fi
  python3 framework/tests/java_app_run.py --lifecycle "$(steps "${size[@]}")" \
    "$app" io.github.kisame76.minato.test.edgeback android.app.Application build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/edge-back-check/$app" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" "${size[0]}x${size[1]}@3,en-US" || failed=$((failed + 1))
done
back=swipe:6:1000:756:1000:300
for app in ime-hide-test ime-hide-test-cb; do
  steps=start,tap:645:250,$back,tap:645:250,$back,tap:645:550,$back,tap:645:550
  # As in ime-hide-check.sh: the opted-in twin's later backs, with no keyboard, end its activity.
  [[ "$app" == ime-hide-test-cb ]] && steps=start,tap:645:250,$back,tap:645:250
  tools/build-java-test-app.sh "$app" >/dev/null
  python3 framework/tests/java_app_run.py --lifecycle "$steps" \
    "$app" io.github.kisame76.minato.test.imehide android.app.Application build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/edge-back-check/$app" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US || failed=$((failed + 1))
done
if ((failed)); then
  echo "edge back check: $failed of 4 apps failed"
  exit 1
fi
echo "edge back check: fine"
