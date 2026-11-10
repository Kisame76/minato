#!/usr/bin/env bash
# Ren'Py on the Mac, an engine Minato tests itself (ADR-0055's start path from the APK): builds Minato's Ren'Py test
# game (fixtures/renpy/minato-renpy-test) into a universal APK with Ren'Py's SDK and RAPT, then runs it through the
# Java path under graphics-run with the session's layers and the Mac's ANGLE (framework/tests/renpy_run.py): its first
# frame, its tone playing at the right rate, and its lines as Android 15's (fixtures/expected/minato-renpy-test.logcat,
# from this script's --reference on the AVD). Nothing is downloaded here; without the tools it prints "skipped: ...".
#
# The tools, all under build/tools (gitignored), fetched once by hand:
#   renpy-sdk     Ren'Py 8.5.3's SDK, https://www.renpy.org/dl/8.5.3/renpy-8.5.3-sdk.tar.bz2, SHA-256
#                 eb0a9be7f0fb13632fe25ceade9a8bed5a1b4d6b6e83bd19eeeb29e1a1bb4a45, unpacked as renpy-sdk/, with RAPT,
#                 https://www.renpy.org/dl/8.5.3/renpy-8.5.3-rapt.zip, SHA-256
#                 8a12be34a2f5238d125ff6dd76a56772fd8e44838af864f81bca82c1059b00e6, unpacked into it (renpy-sdk/rapt;
#                 both listed in https://www.renpy.org/dl/8.5.3/checksums.txt, which Ren'Py signs). RAPT's Android SDK
#                 is renpy-sdk/rapt/Sdk: commandlinetools-mac-11076708_latest.zip, SHA-256
#                 7bc5c72ba0275c80a8f19684fb92793b83a6b5c94d4d179fc5988930282d7e64, then `sdkmanager "platform-tools"
#                 "platforms;android-36"` after `sdkmanager --licenses` (the Android SDK's terms, which the user
#                 accepted on 2026-10-30).
#   jdk-21        Temurin 21.0.13+9 for macOS on arm64, OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.13_9.tar.gz, SHA-256
#                 bf036b35b071bfb2f54f2c0af8ee8d88afa95ee6eb3dce02c0f94cfc2524d04a: RAPT needs JDK 21.
#   renpy-gradle  Gradle's cache (GRADLE_USER_HOME) of RAPT's first build, which Gradle fills by itself.
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh, tools/build-framework-res.sh,
# tools/build-front-libs.sh, tools/build-angle.sh and tools/fetch-moltenvk.sh first.
# With --reference it builds the APK, then runs it on the AVD apk-runner-35-iphone (tools/android-reference.sh's,
# landscape there, as the game asks) instead of the Mac, and writes the game's lines as logcat -v tag shows them, the
# tag unpadded, to build/android-reference/minato-renpy-test.logcat; two runs must agree before that file becomes
# fixtures/expected/minato-renpy-test.logcat.
# Usage: tools/renpy-check.sh [--reference]
set -euo pipefail
cd "$(dirname "$0")/.."

SDK=build/tools/renpy-sdk
if [[ ! -x $SDK/renpy.sh || ! -f $SDK/rapt/android.py ]]; then echo "skipped: no Ren'Py SDK at $SDK"; exit 0; fi
JDK=$(echo build/tools/jdk-21/*/Contents/Home)
if [[ ! -x $JDK/bin/java ]]; then echo "skipped: no JDK 21 at build/tools/jdk-21"; exit 0; fi
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
if [[ ! -f "$ANGLE" ]]; then echo "skipped: $ANGLE is not built (tools/build-angle.sh)"; exit 0; fi
MOLTENVK="$PWD/build/moltenvk/macos/libMoltenVK.dylib"
if [[ ! -f "$MOLTENVK" ]]; then echo "skipped: $MOLTENVK is not fetched (tools/fetch-moltenvk.sh)"; exit 0; fi

# The game's project, as Ren'Py's launcher keeps one: its game/ with the tone, android.json, and a key made here.
OUT=build/renpy-test
PROJECT=$OUT/minatorenpytest
rm -rf "${PROJECT:?}" "${OUT:?}/apk"
mkdir -p "$PROJECT" "$OUT/apk"
cp -R fixtures/renpy/minato-renpy-test/game fixtures/renpy/minato-renpy-test/android.json "$PROJECT/"
python3 fixtures/renpy/minato-renpy-test/tone.py "$PROJECT/game/tone.wav"
if [[ ! -f $OUT/android.keystore ]]; then
  "$JDK/bin/keytool" -genkey -keystore "$OUT/android.keystore" -alias android -keyalg RSA -keysize 2048 \
    -keypass android -storepass android -dname "CN=Minato Test" -validity 20000 >/dev/null 2>&1
fi
cp "$OUT/android.keystore" "$PROJECT/android.keystore"
cp "$OUT/android.keystore" "$PROJECT/bundle.keystore"
if ! JAVA_HOME="$PWD/$JDK" RAPT_NO_TERMS=1 GRADLE_USER_HOME="$PWD/build/tools/renpy-gradle" \
    "$SDK/renpy.sh" "$PWD/$SDK/launcher" android_build "$PWD/$PROJECT" --destination "$PWD/$OUT/apk" \
    >"$OUT/build.log" 2>&1 </dev/null
then
  echo "FAIL: Ren'Py's Android build failed; $OUT/build.log:"; tail -20 "$OUT/build.log"; exit 1
fi
APK=$(echo "$OUT"/apk/io.github.kisame76.minato.test.renpy-*-release.apk)
if [[ ! -f $APK ]]; then echo "FAIL: no APK in $OUT/apk"; exit 1; fi

if [[ ${1:-} == --reference ]]; then
  ANDROID=${ANDROID_HOME:-$HOME/Library/Android/sdk}
  ADB=$ANDROID/platform-tools/adb
  PACKAGE=io.github.kisame76.minato.test.renpy
  RESULT=build/android-reference/minato-renpy-test.logcat
  mkdir -p build/android-reference
  if pgrep -q qemu-system; then echo "An emulator runs already: stop it first."; exit 1; fi
  "$ANDROID/emulator/emulator" -avd apk-runner-35-iphone -port 5554 -no-window -no-boot-anim -no-snapshot \
    -gpu swiftshader_indirect >build/android-reference/emulator.log 2>&1 &
  started=$!
  trap '"$ADB" -s emulator-5554 emu kill >/dev/null 2>&1 || kill "$started" 2>/dev/null || true' EXIT
  for _ in $(seq 1 180); do
    [[ "$("$ADB" -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]] && break
    sleep 2
  done
  device() { "$ADB" -s emulator-5554 "$@" </dev/null; }
  device install -r "$APK" >/dev/null
  device logcat -c
  device shell am start -W -n "$PACKAGE/org.renpy.android.PythonSDLActivity" >/dev/null
  for _ in $(seq 1 60); do
    device logcat -d -v tag | grep -q 'minato-renpy-test: done' && break
    sleep 1
  done
  sleep 2
  device logcat -d -v tag | sed -E 's|^([VDIWEF])/python *: |\1/python: |' \
    | grep -E '^[VDIWEF]/python: minato-renpy-test: ' >"$RESULT" || true
  device uninstall "$PACKAGE" >/dev/null
  echo "Android 15 logged $(wc -l <"$RESULT" | tr -d ' ') lines into $RESULT:"
  sed 's/^/    /' "$RESULT"
  exit 0
fi

NATIVE=build/native/macos-checks
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target graphics-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
tools/build-framework.sh >/dev/null
MINATO_SESSION_LAYERS=1 MINATO_ANGLE="$ANGLE" MINATO_MOLTENVK="$MOLTENVK" python3 framework/tests/renpy_run.py \
  minato-renpy-test io.github.kisame76.minato.test.renpy build/art build/bionic/lib build/bionic/system build/hwui/lib \
  build/sources/icu/icu4c/source/stubdata/icudt75l.dat "$NATIVE/importer/converter/apk-convert" \
  "$NATIVE/graphics/tests/graphics-run" build/renpy-check build/framework-native/app_process64 \
  build/framework/framework.jar build/framework-res/framework-res "$APK" fixtures/expected/minato-renpy-test.logcat
