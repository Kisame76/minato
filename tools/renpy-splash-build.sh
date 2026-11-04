#!/usr/bin/env bash
# Builds Minato's Ren'Py test game (fixtures/renpy/minato-renpy-test) with its splashscreen that calls a screen at once
# (splash.rpy) into build/renpy-splash-test/renpy-splash-test.apk, which tools/java-start-check.sh starts in game mode
# as its case renpy-splash-test: with the Interface start that early, SDL must already have its window, or set_mode
# fails ("Could not fetch native window") and the game exits with status 1. Same tools as tools/renpy-check.sh (its
# header says where they come from); without them this prints "skipped: ..." and builds nothing.
# With the argument "stay" it builds build/renpy-splash-test/renpy-stay-test.apk instead: the same game without its
# quit and with a screen that moves for good after its lines (stay.rpy), which keeps running for the resume round of
# tools/java-start-check.sh (the splash game ends by itself a second after it starts).
# Usage: tools/renpy-splash-build.sh [stay]
set -euo pipefail
cd "$(dirname "$0")/.."

SDK=build/tools/renpy-sdk
if [[ ! -x $SDK/renpy.sh || ! -f $SDK/rapt/android.py ]]; then echo "skipped: no Ren'Py SDK at $SDK"; exit 0; fi
JDK=$(echo build/tools/jdk-21/*/Contents/Home)
if [[ ! -x $JDK/bin/java ]]; then echo "skipped: no JDK 21 at build/tools/jdk-21"; exit 0; fi

OUT=build/renpy-splash-test
NAME=renpy-splash-test
[[ "${1:-}" != stay ]] || NAME=renpy-stay-test
PROJECT=$OUT/$NAME-work/minatorenpytest
KEY=build/renpy-test/android.keystore
rm -rf "${OUT:?}/$NAME-work" "${OUT:?}/$NAME-apk"
mkdir -p "$PROJECT" "$OUT/$NAME-apk" "$(dirname "$KEY")"
cp -R fixtures/renpy/minato-renpy-test/game fixtures/renpy/minato-renpy-test/android.json "$PROJECT/"
cp fixtures/renpy/minato-renpy-test/splash.rpy "$PROJECT/game/splash.rpy"
if [[ $NAME == renpy-stay-test ]]; then
  cp fixtures/renpy/minato-renpy-test/stay.rpy "$PROJECT/game/stay.rpy"
  sed -i '' -e 's/^    \$ renpy.quit()$/    call after_game/' "$PROJECT/game/script.rpy"
  grep -q "call after_game" "$PROJECT/game/script.rpy" || { echo "FAIL: script.rpy has no quit to take out"; exit 1; }
fi
python3 fixtures/renpy/minato-renpy-test/tone.py "$PROJECT/game/tone.wav"
if [[ ! -f $KEY ]]; then
  "$JDK/bin/keytool" -genkey -keystore "$KEY" -alias android -keyalg RSA -keysize 2048 \
    -keypass android -storepass android -dname "CN=Minato Test" -validity 20000 >/dev/null 2>&1
fi
cp "$KEY" "$PROJECT/android.keystore"
cp "$KEY" "$PROJECT/bundle.keystore"
if ! JAVA_HOME="$PWD/$JDK" RAPT_NO_TERMS=1 GRADLE_USER_HOME="$PWD/build/tools/renpy-gradle" \
    "$SDK/renpy.sh" "$PWD/$SDK/launcher" android_build "$PWD/$PROJECT" --destination "$PWD/$OUT/$NAME-apk" \
    >"$OUT/$NAME-build.log" 2>&1 </dev/null
then
  echo "FAIL: Ren'Py's Android build failed; $OUT/$NAME-build.log:"; tail -20 "$OUT/$NAME-build.log"; exit 1
fi
APK=$(echo "$OUT/$NAME-apk"/io.github.kisame76.minato.test.renpy-*-release.apk)
if [[ ! -f $APK ]]; then echo "FAIL: no APK in $OUT/$NAME-apk"; exit 1; fi
cp "$APK" "$OUT/$NAME.apk"
echo "$OUT/$NAME.apk"
