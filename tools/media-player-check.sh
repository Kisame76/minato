#!/usr/bin/env bash
# Runs ADR-0060 e2e1's test of android.media.MediaPlayer on the Mac (framework/tests/java_app_run.py): builds apk-run,
# apk-convert and graphics-run for macOS into build/native/macos-grafik, the guest's libraries (tools/build-bionic.sh,
# incremental), app_process with libandroid_runtime.so, framework.jar and the apps every time, so it never tests an
# old binary; then media-player-test and its twin without the INTERNET permission, media-player-test-offline, against
# Android 15's logs (fixtures/expected/<app>.logcat, from tools/android-reference.sh --app <app>): a player's answers
# in each state, the sources, seeks, a playback to its end into an ImageReader, the http refusals and DRM. A session
# (a configuration), whose frame clock lets the main Looper run the players' callbacks past the first traversal.
# Needs tools/build-art.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/media-player-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi

NATIVE=build/native/macos-grafik
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert graphics-run >/dev/null
tools/build-bionic.sh >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
for app in media-player-test media-player-test-offline; do
  tools/build-java-test-app.sh "$app" >/dev/null
  package=io.github.kisame76.minato.test.mediaplayer
  [[ "$app" != *-offline ]] || package=${package}offline
  python3 framework/tests/java_app_run.py "$app" "$package" \
    io.github.kisame76.minato.test.mediaplayer.MediaPlayerApplication build/art build/bionic/lib \
    build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" "build/$app-check" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" 1290x2796@3,en-US
done
