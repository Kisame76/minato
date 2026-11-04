#!/usr/bin/env bash
# Android 15's behaviour for a Ren'Py name dialog (renpy.input), on the AVD apk-runner-35-iphone (landscape there, as
# the game asks): builds Minato's Ren'Py input game (fixtures/renpy/minato-renpy-input) with Ren'Py's SDK and RAPT as
# tools/renpy-check.sh builds its test game, builds the fixture keyboard ime-probe-test, selects it, starts the game,
# waits for its dialog, and plays the cases while the keyboard logs (tag ime-probe) what the field tells it:
#   (a) the editor info SDL gives the input method, (d) the extracted text after the keyboard typed "abc";
#   (b) Back with the keyboard up, then the dialog left alone;
#   (c) taps on the prompt text, on the background, on the field and in a corner, with the keyboard up (it comes back
#       by itself after Back); build/renpy-input/timed.log keeps the lines with their times, which differ per run.
# It writes every phase's lines to build/android-reference/renpy-input.logcat; run it twice and compare, the two must
# agree. Nothing is downloaded; without the tools it prints "skipped: ...". Needs the tools of tools/renpy-check.sh.
# Usage: tools/renpy-input-reference.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SDK=build/tools/renpy-sdk
if [[ ! -x $SDK/renpy.sh || ! -f $SDK/rapt/android.py ]]; then echo "skipped: no Ren'Py SDK at $SDK"; exit 0; fi
JDK=$(echo build/tools/jdk-21/*/Contents/Home)
if [[ ! -x $JDK/bin/java ]]; then echo "skipped: no JDK 21 at build/tools/jdk-21"; exit 0; fi

OUT=build/renpy-input
PROJECT=$OUT/minatorenpyinput
rm -rf "${PROJECT:?}" "${OUT:?}/apk"
mkdir -p "$PROJECT" "$OUT/apk"
cp -R fixtures/renpy/minato-renpy-input/game fixtures/renpy/minato-renpy-input/android.json "$PROJECT/"
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
APK=$(echo "$OUT"/apk/io.github.kisame76.minato.test.renpyinput-*-release.apk)
if [[ ! -f $APK ]]; then echo "FAIL: no APK in $OUT/apk"; exit 1; fi
tools/build-java-test-app.sh ime-probe-test >/dev/null
IME_APK=build/ime-probe-test/ime-probe-test.apk

ANDROID=${ANDROID_HOME:-$HOME/Library/Android/sdk}
ADB=$ANDROID/platform-tools/adb
PACKAGE=io.github.kisame76.minato.test.renpyinput
IME=io.github.kisame76.minato.test.imeprobe/.ProbeInputMethod
RESULT=build/android-reference/renpy-input.logcat
mkdir -p build/android-reference
if pgrep -q qemu-system; then echo "An emulator runs already: stop it first."; exit 1; fi
"$ANDROID/emulator/emulator" -avd apk-runner-35-iphone -port 5554 -no-window -no-boot-anim -no-snapshot \
  -gpu swiftshader_indirect >build/android-reference/emulator.log 2>&1 &
started=$!
device() { "$ADB" -s emulator-5554 "$@" </dev/null; }
trap 'device shell ime reset >/dev/null 2>&1 || true; sleep 35; "$ADB" -s emulator-5554 emu kill >/dev/null 2>&1 || kill "$started" 2>/dev/null || true' EXIT
for _ in $(seq 1 180); do
  [[ "$("$ADB" -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]] && break
  sleep 2
done
device install -r "$APK" >/dev/null
device install -r "$IME_APK" >/dev/null
device shell ime enable "$IME" >/dev/null
device shell ime set "$IME" >/dev/null
device shell settings put secure immersive_mode_confirmations confirmed
device shell input keyevent KEYCODE_WAKEUP
device shell wm dismiss-keyguard
device logcat -c

# phase <name> [seconds] [first line]: waits (4 s), prints the lines since the last phase (from the first line on, to
# leave out the start-up's) and keeps their times aside
phase() {
  sleep "${2:-4}"
  echo "== $1"
  device logcat -d -v tag | sed -n "/${3:-.}/,\$p" | sed -E 's|^([VDIWEF])/python *: |\1/python: |' \
    | grep -E '^[VDIWEF]/(python|ime-probe) *: (minato-renpy-input|on|get)' | sed -E 's/ +: /: /' || true
  echo "== $1" >>"$OUT/timed.log"
  device logcat -d -v time | grep -E '(python|ime-probe) *\([ 0-9]+\): (minato-renpy-input|on|get)' >>"$OUT/timed.log" || true
  device logcat -c
}
: >"$RESULT.tmp"
: >"$OUT/timed.log"
{
  device shell am start -W -n "$PACKAGE/org.renpy.android.PythonSDLActivity" >/dev/null
  for _ in $(seq 1 60); do
    device logcat -d -v tag | grep -q 'minato-renpy-input: asking' && break
    sleep 1
  done
  phase "the dialog is up" 4 "minato-renpy-input: asking"
  device exec-out screencap -p >"$OUT/dialog.png" || true
  device shell input keyevent KEYCODE_BACK
  phase "after Back" 5
  device shell input tap 680 1010
  phase "after a tap on the prompt text"
  device shell input tap 1400 500
  phase "after a tap on the background"
  device shell input tap 600 1067
  phase "after a tap on the field"
  device shell input tap 200 200
  phase "after a tap in the corner"
  device shell dumpsys input_method | grep -E 'mShowRequested|mInputShown|isInputShown|mCurFocusedWindow' | head -5
} | tee "$RESULT"
device uninstall "$PACKAGE" >/dev/null || true
device uninstall io.github.kisame76.minato.test.imeprobe >/dev/null || true
rm -f "$RESULT.tmp"
