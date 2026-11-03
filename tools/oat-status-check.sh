#!/usr/bin/env bash
# Runs M6 c1's test 3 on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into
# build/native/macos-app, app_process with libandroid_runtime.so, framework.jar and oat-status-test every time, then
# starts oat-status-test three times at Android 15's code path for its APK: with the verify odex our dex2oat made for
# it, with no odex, and with an odex our dex2oat made for other dex code (app-start-test's) at the same location. Its
# lines must be Android 15's on apk-runner-35 for each
# (fixtures/expected/oat-status-test.{verify,none,other-dex}.logcat:
# the public DexFile.isDexOptNeeded says false for the first, true for the others). Two cases for the compiled code of
# M6 c: "speed", the odex our dex2oat makes at --compiler-filter=speed for an app's whole boot class path (framework.jar
# included) and class loader context: DexFile.isDexOptNeeded says false and ART's -verbose:oat log says the odex is the
# best oat file, up to date and executable; and "speed-core-only", one made for the three core jars alone, whose boot
# class path is not a prefix of the app's: isDexOptNeeded says false too (it asks for the verify filter), the log says
# kOatBootImageOutOfDate and no executable odex (the stale rule of a
# framework change). Not a case: an odex made for another
# boot class path, which ART accepts for the filter verify (OatFileAssistant checks the boot class path's checksums only
# for filters that depend on the boot image).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/oat-status-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
app=oat-status-test
package=io.github.kisame76.minato.test.oatstatus
tools/build-java-test-app.sh "$app" >/dev/null
tools/build-java-test-app.sh app-start-test >/dev/null

apk="build/$app/$app.apk"
work=build/oat-status-check
rm -rf "$work"
mkdir -p "$work"
location=$(python3 -c "
import sys
sys.path.insert(0, 'framework/tests')
from app_process_run import code_path
print(code_path('$apk', '$package') + '/base.apk')")
make=(python3 runtime/tests/dex2oat_make.py build/art build/bionic/lib build/bionic/system
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert"
  "$NATIVE/runtime/apk-run" "$work/dex2oat")
"${make[@]}" "$apk" "$location" "$work/verify"
"${make[@]}" build/app-start-test/app-start-test.apk "$location" "$work/other-dex"
"${make[@]}" "$apk" "$location" "$work/speed" --filter speed --framework build/framework/framework.jar
"${make[@]}" "$apk" "$location" "$work/speed-core-only" --filter speed

failed=0
for case in verify none other-dex speed speed-core-only; do
  odex=()
  [[ "$case" == none ]] || odex=(--odex "$work/$case.odex")
  # ART's own word on the odex (-verbose:oat, which only a measure sets), from the run's lines: the speed odex is the
  # best oat file and runs executable; the one for the three core jars is out of date for the app's boot class path
  # and does not.
  proof=()
  [[ "$case" != speed && "$case" != speed-core-only ]] || proof=(--env MINATO_MEASURE_ART_OPTIONS=-verbose:oat)
  MINATO_LINE_TIMES="$PWD/$work/lines-$case.txt" python3 framework/tests/java_app_run.py \
    ${proof[@]+"${proof[@]}"} ${odex[@]+"${odex[@]}"} \
    "$app" "$package" io.github.kisame76.minato.test.oatstatus.StatusApplication build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "$work/run-$case" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "$apk" "fixtures/expected/$app.$case.logcat" 1290x2796@3,en-US || failed=1
  lines="$work/lines-$case.txt"
  case "$case" in
    speed)
      grep -q "/oat/arm64/base.odex is kOatUpToDate with filter speed" "$lines" &&
        grep -q "/oat/arm64/base.odex (executable=1)" "$lines" ||
        { echo "speed: ART does not run the odex"; failed=1; } ;;
    speed-core-only)
      grep -q "/oat/arm64/base.odex is kOatBootImageOutOfDate with filter speed" "$lines" &&
        ! grep -q "/oat/arm64/base.odex (executable=1)" "$lines" ||
        { echo "speed-core-only: ART runs it"; failed=1; } ;;
  esac
done
exit "$failed"
