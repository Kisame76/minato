#!/usr/bin/env bash
# Builds one of ADR-0039's Java test apps, fixtures/java/<app> with fixtures/apps/<app>/AndroidManifest.xml and its
# assets/ and res/, if any, into build/<app>/<app>.apk: javac compiles the app against the boot jars' classes and
# framework.jar's classes with b3.1's stubs (build/framework/classes.jar and stubs.jar of tools/build-framework.sh;
# compile only: on Android the system's framework serves them, in Minato our framework.jar), D8 dexes the app's
# classes alone, and make-apk packs them. An app with res/ goes through the host aapt2 first, linked against our
# framework-res as an app is against android.jar's, and make-apk takes aapt2's APK (--resources); .txt files stay
# stored, so a raw text resource opens as a file descriptor (openRawResourceFd). The apps: app-start-test (test 1),
# asset-manager-test (b3.3), resources-test (b3.4a), config-test (b3.4c, test 3), styled-text-test (b3.4b),
# package-manager-test (b3.5a), package-manager-legacy-test (b3.5b1), graphics-start-test and canvas-test (ADR-0043
# test 2), context-test and looper-test (step c1), provider-test (step c2c1), view-tree-test (step c2c2), app-calls-test
# (step c2d), animator-test and gles-java-test (ADR-0041 a)), compat-test and compat-test-35 (step c2e),
# activity-start-test (step c2c3), services-test (ADR-0045, the start's local services), input-service-test (ADR-0045,
# the input service), start-stops-test (ADR-0042 b, Godot's start stops) and settings-test (ADR-0045, gap 13). An app
# without its own fixtures/java folder, a twin such as compat-test-35, takes the sources of the app it is named after.
# jnigraphics-test (ADR-0041 a5) is one more, whose JNI library links libjnigraphics, and touch-test (ADR-0045 step
# 7b), whose events are made up.
# binder-test (ADR-0043 step d2b)) is one more, whose binders are its own, and strictmode-test (ADR-0039 step c2d3).
# drawables-test (ADR-0043 step d)) is one more, with an XML drawable of every tag in res/drawable, and checkbox-test
# (step e1)), with a layout of CheckBoxes in res/layout. pointer-insets-test (ADR-0042 b) is one more, with a
# <pointer-icon> in res/xml and its bitmap in res/drawable.
# palette-test (ADR-0043 step f)) is one more, and so is packs-test (ADR-0050 test 2), which tools/build-packs-test.sh
# also packs as a bundletool set with a fast-follow and an on-demand pack; its twin packs-test-own, from its sources,
# names its own local_testing_dir.
# activity-flow-test (ADR-0039 step b3.5b2-1) is one more, whose activities start one another, and so are
# gesture-flow-test (step b3.5b2-1b), whose touch goes on while its second activity starts, result-flow-test
# (step b3.5b2-2a), whose activities return results to the ones that started them, and launch-mode-test
# (step b3.5b2-2b), whose starts follow the launch modes and the task's flags.
# key-test (ADR-0045 step 7c) is one more, whose keys it injects into its own window.
# broadcast-test (ADR-0045 decision 5) is one more, whose activity sends its own broadcasts to its own receivers.
# api-breadth-test (the compatibility plan's entry 22) is one more, whose assets/types.txt names the public types it
# loads, and so is widgets-test (gap 5), with layouts of widgets in res/layout and a shape in res/drawable.
# breadth-test, the framework breadth's behavior test, is one too.
# sqlite-test (the compatibility plan's gap 7) is one more, whose activity makes, changes and deletes its own SQLite
# database.
# timezone-test (the compatibility plan's gap 21) is one more, whose activity reads the time zone data.
# package-installer-test (the Unity test APK resolve list, batch E) is one more, whose activity asks the package installer
# for sessions and a new one, and reads the exit reasons.
# media-absent-test (the Unity test APK gap sort, category 2) is one more, whose activity asks android.media's DRM, codec,
# retriever and extractor classes.
# resolve-log-test (the Unity test APK probe's log of failed resolutions) is one more, whose compile-only/ classes javac
# compiles the app against and D8 leaves out, so that the app meets missing classes and members at run time.
# system-services-test (gap 1 of the compatibility plan, batch 3) is one more, whose activity asks the system services
# as libraries do; its twin system-services-test-unpermitted requests none of their permissions.
# battery-test (gap 1 of the compatibility plan) is one more, whose activity reads the battery's sticky broadcast,
# its changes and BatteryManager.
# Needs the JDK and D8 of tools/fetch-sources.py --set art, the boot jars' classes of tools/build-art.sh,
# tools/build-framework.sh's output, for res/ tools/build-aapt2.sh and tools/build-framework-res.sh, and for jni/ the
# Android NDK (ANDROID_NDK, default: r30 in ~/Library/Android/sdk/ndk).
# Usage: tools/build-java-test-app.sh <app>
set -euo pipefail
cd "$(dirname "$0")/.."

APP=${1:?usage: tools/build-java-test-app.sh <app>}
JAVA=fixtures/java/$APP
[[ -d "$JAVA" ]] || JAVA=fixtures/java/${APP%-*}
# A twin of a twin, edge-back-test-immersive-landscape say, takes the first app's sources.
[[ -d "$JAVA" ]] || JAVA=fixtures/java/${APP%-*-*}
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
FRAMEWORK_CLASSES=build/framework/classes.jar
STUBS=build/framework/stubs.jar
OUT=build/$APP
if [[ ! -d "$JAVA" || ! -f "fixtures/apps/$APP/AndroidManifest.xml" ]]; then
  echo "No fixtures/java/$APP or fixtures/apps/$APP/AndroidManifest.xml." >&2
  exit 1
fi
if [[ ! -x "$JDK/bin/javac" || ! -f "$R8" || ! -f "$BOOT_CLASSES" ]]; then
  echo "No JDK, D8 or $BOOT_CLASSES: run tools/fetch-sources.py --set art and tools/build-art.sh first." >&2
  exit 1
fi
if [[ ! -f "$FRAMEWORK_CLASSES" || ! -f "$STUBS" ]]; then
  echo "No $FRAMEWORK_CLASSES or $STUBS: run tools/build-framework.sh first." >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"
# -Xlint:-options: the notes about source 8 on a newer JDK.
"$JDK/bin/javac" -source 8 -target 8 -Xlint:-options -bootclasspath "$BOOT_CLASSES" \
  -classpath "$FRAMEWORK_CLASSES:$STUBS" -d "$OUT/classes" $(find "$JAVA" -name '*.java' | sort)
if [[ -d "$JAVA/compile-only" ]]; then
  (cd "$JAVA/compile-only" && find . -name '*.java') | while read -r source; do
    rm -f "$OUT/classes/${source%.java}.class" "$OUT/classes/${source%.java}"\$*.class
  done
fi
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class' | sort)
assets=()
[[ ! -d "fixtures/apps/$APP/assets" ]] || assets=(--assets "fixtures/apps/$APP/assets")
# An app with a jni folder gets its C and C++ files as its JNI library, lib<app>.so in lib/arm64-v8a, built with the
# NDK's clang at API 29 (D8's --min-api); jni/link.txt names the NDK libraries it links, one per line, each kept as
# NEEDED.
libs=()
if [[ -d "$JAVA/jni" ]]; then
  NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
  if [[ ! -x "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" ]]; then
    echo "No Android NDK at $NDK. Set ANDROID_NDK to its folder." >&2
    exit 1
  fi
  link=()
  if [[ -f "$JAVA/jni/link.txt" ]]; then
    while read -r name; do link+=("-l$name"); done < "$JAVA/jni/link.txt"
  fi
  # C++ files make it a C++ library with the NDK's static C++ library (c++_static), as Unity's libil2cpp.so is.
  compiler=clang
  cxx=()
  if [[ -n "$(find "$JAVA/jni" -name '*.cpp')" ]]; then compiler=clang++ cxx=(-std=c++20 -static-libstdc++); fi
  mkdir -p "$OUT/lib"
  "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/$compiler" --target=aarch64-linux-android29 -shared -fPIC -O2 \
    -Wall -Werror ${cxx[@]+"${cxx[@]}"} -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    -o "$OUT/lib/lib$(basename "$JAVA").so" $(find "$JAVA/jni" -name '*.c' -o -name '*.cpp' | sort) \
    -Wl,--no-as-needed ${link[@]+"${link[@]}"} -Wl,--as-needed
  libs=(--lib "$OUT/lib/lib$(basename "$JAVA").so")
fi
manifest=(--manifest "fixtures/apps/$APP/AndroidManifest.xml")
if [[ -d "fixtures/apps/$APP/res" ]]; then
  build/aapt2/aapt2 compile --dir "fixtures/apps/$APP/res" -o "$OUT/compiled.zip"
  build/aapt2/aapt2 link -o "$OUT/resources.apk" -I build/framework-res/framework-res.apk -0 txt \
    --manifest "fixtures/apps/$APP/AndroidManifest.xml" "$OUT/compiled.zip"
  manifest=(--resources "$OUT/resources.apk")
fi
tools/make-apk.sh "${manifest[@]}" ${libs[@]+"${libs[@]}"} --dex "$OUT/dex/classes.dex" ${assets[@]+"${assets[@]}"} \
  --out "$OUT/$APP.apk"
