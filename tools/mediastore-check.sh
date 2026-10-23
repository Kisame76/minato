#!/usr/bin/env bash
# Runs the test of ADR-0057 step e, mediastore-test, on the Mac (framework/tests/java_app_run.py):
# builds apk-run and apk-convert for macOS into build/native/macos-app, and app_process with libandroid_runtime.so
# (incremental), framework.jar (about 10 s) and mediastore-test (seconds) every time, so it never tests an old binary;
# then starts mediastore-test through app_process and compares its log with Android 15's on apk-runner-35
# (fixtures/expected/mediastore-test.logcat, from tools/android-reference.sh --app mediastore-test): an app without
# storage permissions inserts media into MediaStore (pending, written, finished, a duplicate name, audio, folders the
# collection refuses, insertImage), reads its rows and the bytes back and deletes them. The harness answers the
# provider's exports to Photos like a host that has the permission (apk-run --export-dir, see below).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first.
# Usage: tools/mediastore-check.sh
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
tools/build-java-test-app.sh mediastore-test >/dev/null

# What the host adds to Photos is copied here by apk-run: images and videos that were finished, not audio.
EXPORTED=$(mktemp -d "${TMPDIR:-/tmp}/mediastore-export.XXXXXX")
trap 'rm -rf "$EXPORTED"' EXIT
python3 framework/tests/java_app_run.py --lifecycle start --export-dir "$EXPORTED" \
  mediastore-test io.github.kisame76.minato.test.mediastore \
  android.app.Application build/art build/bionic/lib build/bionic/system \
  build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/runtime/apk-run" build/mediastore-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res \
  build/mediastore-test/mediastore-test.apk fixtures/expected/mediastore-test.logcat \
  1290x2796@3,en-US

# The three finished images were exported with their sizes (shot.jpg 2000, the second 500, insertImage's 759); the
# audio file and the item that was never written were not.
listing=$(cd "$EXPORTED" && for f in *; do echo "$f $(wc -c < "$f" | tr -d ' ')"; done | sort)
expected=$'legacy.jpg 759\nshot (1).jpg 500\nshot.jpg 2000'
if [[ "$listing" != "$expected" ]]; then
  echo "mediastore-test: the host exported:"
  echo "$listing"
  exit 1
fi
echo "mediastore-test: the host exported the three finished images"
