#!/usr/bin/env bash
# Runs ADR-0050's test 2 on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert for macOS into
# build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar (about 10 s),
# packs-test and its twin packs-test-own (seconds) every time, so it never tests an old binary; then starts each app
# through app_process twice. Without packs its log must be Android 15's (fixtures/expected/<app>.logcat, from
# tools/android-reference.sh --app <app>). With two pack slices read-only in the folder local_testing of its external
# files folder, as the library lays out an app's kept packs, and ActivityThread's --local-testing-dir local_testing, as
# GuestStart passes it, its log must be fixtures/expected/<app>-packs.logcat: packs-test finds local_testing_dir from
# Minato, packs-test-own keeps its own value. The slices are made up, as only their names and sizes count here; test 3
# gives real ones to Google's library.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-aapt2.sh and
# tools/build-framework-res.sh first.
# Usage: tools/packs-check.sh
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
tools/build-java-test-app.sh packs-test >/dev/null
tools/build-java-test-app.sh packs-test-own >/dev/null

# A fast-follow pack's slice and an on-demand pack's, under the names bundletool gives them (ADR-0049 f).
SLICES=build/packs-check/slices
rm -rf "${SLICES:?}"
mkdir -p "$SLICES"
printf 'Minato packs-test, a fast-follow pack.\n' >"$SLICES/fastfollow-master.apk"
printf 'Minato packs-test, an on-demand pack.\n' >"$SLICES/ondemand-master.apk"

status=0
for app in packs-test packs-test-own; do
  package=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "fixtures/apps/$app/AndroidManifest.xml" | head -n 1)
  for run in plain packs; do
    expected=fixtures/expected/$app.logcat
    options=()
    if [[ "$run" == packs ]]; then
      expected=fixtures/expected/$app-packs.logcat
      options=(--packs "$SLICES/fastfollow-master.apk" --packs "$SLICES/ondemand-master.apk")
    fi
    echo "$app, $run:"
    python3 framework/tests/java_app_run.py ${options[@]+"${options[@]}"} "$app" "$package" \
      io.github.kisame76.minato.test.packs.PacksApplication build/art build/bionic/lib build/bionic/system \
      build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
      "$NATIVE/runtime/apk-run" "build/packs-check/$app-$run" build/framework-native/app_process64 \
      build/framework/framework.jar build/framework-res/framework-res "build/$app/$app.apk" "$expected" || status=1
  done
done
exit "$status"
