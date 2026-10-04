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
# test 2), context-test and looper-test (step c1), provider-test (step c2c1).
# Needs the JDK and D8 of tools/fetch-sources.py --set art, the boot jars' classes of tools/build-art.sh,
# tools/build-framework.sh's output, and for res/ tools/build-aapt2.sh and tools/build-framework-res.sh.
# Usage: tools/build-java-test-app.sh <app>
set -euo pipefail
cd "$(dirname "$0")/.."

APP=${1:?usage: tools/build-java-test-app.sh <app>}
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
FRAMEWORK_CLASSES=build/framework/classes.jar
STUBS=build/framework/stubs.jar
OUT=build/$APP
if [[ ! -d "fixtures/java/$APP" || ! -f "fixtures/apps/$APP/AndroidManifest.xml" ]]; then
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
  -classpath "$FRAMEWORK_CLASSES:$STUBS" -d "$OUT/classes" $(find "fixtures/java/$APP" -name '*.java' | sort)
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class' | sort)
assets=()
[[ ! -d "fixtures/apps/$APP/assets" ]] || assets=(--assets "fixtures/apps/$APP/assets")
manifest=(--manifest "fixtures/apps/$APP/AndroidManifest.xml")
if [[ -d "fixtures/apps/$APP/res" ]]; then
  build/aapt2/aapt2 compile --dir "fixtures/apps/$APP/res" -o "$OUT/compiled.zip"
  build/aapt2/aapt2 link -o "$OUT/resources.apk" -I build/framework-res/framework-res.apk -0 txt \
    --manifest "fixtures/apps/$APP/AndroidManifest.xml" "$OUT/compiled.zip"
  manifest=(--resources "$OUT/resources.apk")
fi
tools/make-apk.sh "${manifest[@]}" --dex "$OUT/dex/classes.dex" ${assets[@]+"${assets[@]}"} --out "$OUT/$APP.apk"
