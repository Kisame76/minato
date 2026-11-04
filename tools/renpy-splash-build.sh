#!/usr/bin/env bash
# Builds Minato's Ren'Py test game (fixtures/renpy/minato-renpy-test) with its splashscreen that calls a screen at once
# (splash.rpy) into build/renpy-splash-test/renpy-splash-test.apk, which tools/java-start-check.sh starts in game mode
# as its case renpy-splash-test: with the Interface start that early, SDL must already have its window, or set_mode
# fails ("Could not fetch native window") and the game exits with status 1. Same tools as tools/renpy-check.sh (its
# header says where they come from); without them this prints "skipped: ..." and builds nothing.
# Usage: tools/renpy-splash-build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SDK=build/tools/renpy-sdk
if [[ ! -x $SDK/renpy.sh || ! -f $SDK/rapt/android.py ]]; then echo "skipped: no Ren'Py SDK at $SDK"; exit 0; fi
JDK=$(echo build/tools/jdk-21/*/Contents/Home)
if [[ ! -x $JDK/bin/java ]]; then echo "skipped: no JDK 21 at build/tools/jdk-21"; exit 0; fi

OUT=build/renpy-splash-test
PROJECT=$OUT/minatorenpytest
KEY=build/renpy-test/android.keystore
rm -rf "${PROJECT:?}" "${OUT:?}/apk"
mkdir -p "$PROJECT" "$OUT/apk" "$(dirname "$KEY")"
cp -R fixtures/renpy/minato-renpy-test/game fixtures/renpy/minato-renpy-test/android.json "$PROJECT/"
cp fixtures/renpy/minato-renpy-test/splash.rpy "$PROJECT/game/splash.rpy"
python3 fixtures/renpy/minato-renpy-test/tone.py "$PROJECT/game/tone.wav"
if [[ ! -f $KEY ]]; then
  "$JDK/bin/keytool" -genkey -keystore "$KEY" -alias android -keyalg RSA -keysize 2048 \
    -keypass android -storepass android -dname "CN=Minato Test" -validity 20000 >/dev/null 2>&1
fi
cp "$KEY" "$PROJECT/android.keystore"
cp "$KEY" "$PROJECT/bundle.keystore"
if ! JAVA_HOME="$PWD/$JDK" RAPT_NO_TERMS=1 GRADLE_USER_HOME="$PWD/build/tools/renpy-gradle" \
    "$SDK/renpy.sh" "$PWD/$SDK/launcher" android_build "$PWD/$PROJECT" --destination "$PWD/$OUT/apk" \
    >"$OUT/build.log" 2>&1 </dev/null
then
  echo "FAIL: Ren'Py's Android build failed; $OUT/build.log:"; tail -20 "$OUT/build.log"; exit 1
fi
APK=$(echo "$OUT"/apk/io.github.kisame76.minato.test.renpy-*-release.apk)
if [[ ! -f $APK ]]; then echo "FAIL: no APK in $OUT/apk"; exit 1; fi
cp "$APK" "$OUT/renpy-splash-test.apk"
echo "$OUT/renpy-splash-test.apk"
