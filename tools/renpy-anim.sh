#!/usr/bin/env bash
# Builds Minato's Ren'Py anim game (fixtures/renpy/minato-renpy-anim: a title animation of 600 distinct full-screen
# frames, with the preload gate's log lines) into an APK with the SDK and RAPT of tools/renpy-check.sh:
# build/renpy-anim/apk/io.github.kisame76.minato.test.renpymemory-*-release.apk, which tools/java-start-check.sh's
# renpy-returns-test starts when RENPY_RETURNS_APK names no other. The frames (30 PNGs and anim.rpy, frames.py), the
# tone and the music tracks are written at the build; the repository holds no image. The game has the memory game's
# package, signed with build/renpy-anim/android.keystore (made here when missing), so it replaces an install of
# either. It builds again when a file of the fixture is newer than the last build; without the SDK or the JDK it prints
# "skipped: ...".
# Usage: tools/renpy-anim.sh
set -euo pipefail
cd "$(dirname "$0")/.."
FIXTURE=fixtures/renpy/minato-renpy-anim

SDK=build/tools/renpy-sdk
if [[ ! -x $SDK/renpy.sh || ! -f $SDK/rapt/android.py ]]; then echo "skipped: no Ren'Py SDK at $SDK"; exit 0; fi
JDK=$(echo build/tools/jdk-21/*/Contents/Home)
if [[ ! -x $JDK/bin/java ]]; then echo "skipped: no JDK 21 at build/tools/jdk-21"; exit 0; fi

OUT=build/renpy-anim
PROJECT=$OUT/minatorenpymemory
if [[ ! -f $OUT/apk/done || -n "$(find "$FIXTURE" -type f -newer "$OUT/apk/done" | head -n 1)" ]]; then
  rm -rf "${PROJECT:?}" "${OUT:?}/apk"
  mkdir -p "$PROJECT" "$OUT/apk"
  cp -R "$FIXTURE/game" "$FIXTURE/android.json" "$PROJECT/"
  python3 fixtures/renpy/minato-renpy-test/tone.py "$PROJECT/game/tone.wav"
  python3 "$FIXTURE/frames.py" "$PROJECT/game" 30
  python3 fixtures/renpy/minato-renpy-memory/tracks.py "$PROJECT/game" 60
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
  touch "$OUT/apk/done"
fi
APK=$(echo "$OUT"/apk/io.github.kisame76.minato.test.renpymemory-*-release.apk)
if [[ ! -f $APK ]]; then echo "FAIL: no APK in $OUT/apk"; exit 1; fi
echo "renpy-anim: $APK"
