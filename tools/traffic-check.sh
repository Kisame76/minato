#!/usr/bin/env bash
# Runs the test of TrafficStats on the Mac (framework/tests/java_app_run.py; the Unity test APK probe's blocker 9): builds
# apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and traffic-test (seconds) every time, so it never tests an old binary;
# then starts traffic-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/traffic-test.logcat, from tools/android-reference.sh --app traffic-test): the thread's tag and
# uid, the socket tagger that the app process attaches at its start (StrictMode's untagged socket violation), tagging
# sockets and descriptors, the counters Android answers UNSUPPORTED for and incrementOperationCount's checks; what the
# device counts (the app's own bytes, the totals, an interface's, the mobile and loopback ones) is shown only. The
# process's log must hold the tagger's lines in the reference run's order: D/TrafficStats tagSocket at each new
# socket and each tag, I/TrafficStats untagSocket at each untag (Android 15 logs both).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/traffic-check.sh
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
tools/build-java-test-app.sh traffic-test >/dev/null

# The tagger's lines for the app's sockets: the untagged and the tagged DatagramSocket made, then a bound Socket, a
# DatagramSocket and a descriptor, each made, tagged and untagged.
tag=(--in-order "D/TrafficStats: tagSocket(")
untag=(--in-order "I/TrafficStats: untagSocket(")
python3 framework/tests/java_app_run.py "${tag[@]}" "${tag[@]}" "${tag[@]}" "${tag[@]}" "${untag[@]}" \
  "${tag[@]}" "${tag[@]}" "${untag[@]}" "${tag[@]}" "${tag[@]}" "${untag[@]}" \
  traffic-test io.github.kisame76.minato.test.traffic \
  io.github.kisame76.minato.test.traffic.TrafficApplication build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/traffic-check build/framework-native/app_process64 build/framework/framework.jar \
  build/framework-res/framework-res build/traffic-test/traffic-test.apk fixtures/expected/traffic-test.logcat
