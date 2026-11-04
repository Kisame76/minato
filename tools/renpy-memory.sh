#!/usr/bin/env bash
# What grows in Minato's process while a Ren'Py game plays, on the Mac (the investigation of the memory kill, after
# tools/renpy-check.sh): builds Minato's Ren'Py memory game (fixtures/renpy/minato-renpy-memory, many distinct
# full-screen backgrounds one after the other as a visual novel shows them) into an APK with the SDK and RAPT of
# renpy-check.sh, then runs it as renpy-check.sh does (framework/tests/renpy_run.py's start) and samples the process's
# footprint with the system's footprint tool every SECONDS s (framework/tests/renpy_memory.py): the total and the dirty
# bytes of each category go to build/renpy-memory/footprint.csv. It builds nothing else, so the shared builds must be
# there (tools/renpy-check.sh makes them); without the tools it prints "skipped: ...".
# BUILD_ONLY=1 stops once the APK is built and leaves it as build/renpy-memory-test/renpy-memory-test.apk, which
# tools/java-start-check.sh's renpy-memory-test starts on the iPhone or in the Simulator.
# Usage: tools/renpy-memory.sh [scenes (240, 0.5 s each plus the dissolve)] [seconds between samples (15)]
set -euo pipefail
cd "$(dirname "$0")/.."
SCENES=${1:-240}
SECONDS_BETWEEN=${2:-15}

SDK=build/tools/renpy-sdk
if [[ ! -x $SDK/renpy.sh || ! -f $SDK/rapt/android.py ]]; then echo "skipped: no Ren'Py SDK at $SDK"; exit 0; fi
JDK=$(echo build/tools/jdk-21/*/Contents/Home)
if [[ ! -x $JDK/bin/java ]]; then echo "skipped: no JDK 21 at build/tools/jdk-21"; exit 0; fi
ANGLE="$PWD/build/angle/angle/out/macos/libGLESv2.dylib"
MOLTENVK="$PWD/build/moltenvk/macos/libMoltenVK.dylib"
NATIVE=build/native/macos-grafik
for needed in "$ANGLE" "$MOLTENVK" "$NATIVE/graphics/tests/graphics-run" build/framework/framework.jar; do
  if [[ -z ${BUILD_ONLY:-} && ! -e $needed ]]; then
    echo "skipped: $needed is not built (tools/renpy-check.sh's builds)"; exit 0
  fi
done

OUT=build/renpy-memory
PROJECT=$OUT/minatorenpymemory
if [[ ! -f $OUT/apk/done || fixtures/renpy/minato-renpy-memory/game/script.rpy -nt $OUT/apk/done ]]; then
  rm -rf "${PROJECT:?}" "${OUT:?}/apk"
  mkdir -p "$PROJECT" "$OUT/apk"
  cp -R fixtures/renpy/minato-renpy-memory/game fixtures/renpy/minato-renpy-memory/android.json "$PROJECT/"
  python3 fixtures/renpy/minato-renpy-test/tone.py "$PROJECT/game/tone.wav"
  python3 fixtures/renpy/minato-renpy-memory/images.py "$PROJECT/game" 300
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
if [[ -n ${BUILD_ONLY:-} ]]; then
  mkdir -p build/renpy-memory-test
  cp "$APK" build/renpy-memory-test/renpy-memory-test.apk
  exit 0
fi

MINATO_SESSION_LAYERS=1 MINATO_ANGLE="$ANGLE" MINATO_MOLTENVK="$MOLTENVK" python3 framework/tests/renpy_memory.py \
  "$SCENES" "$SECONDS_BETWEEN" "$OUT/footprint.csv" minato-renpy-memory io.github.kisame76.minato.test.renpymemory \
  build/art build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
  "$NATIVE/importer/converter/apk-convert" "$NATIVE/graphics/tests/graphics-run" build/renpy-memory-run \
  build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res "$APK"
