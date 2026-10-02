#!/usr/bin/env bash
# Builds ADR-0039's test 1, app-start-test, into build/app-start-test/app-start-test.apk: javac compiles
# fixtures/java/app-start-test against the boot jars' classes and framework.jar's classes with b3.1's stubs
# (build/framework/classes.jar and stubs.jar of tools/build-framework.sh; compile only: on Android the system's
# framework serves them, in Minato our framework.jar), D8 dexes the app's classes alone, and make-apk packs them
# with fixtures/apps/app-start-test/AndroidManifest.xml.
# Needs the JDK and D8 of tools/fetch-sources.py --set art, the boot jars' classes of tools/build-art.sh and
# tools/build-framework.sh's output.
# Usage: tools/build-app-start-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
FRAMEWORK_CLASSES=build/framework/classes.jar
STUBS=build/framework/stubs.jar
OUT=build/app-start-test
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
  -classpath "$FRAMEWORK_CLASSES:$STUBS" -d "$OUT/classes" $(find fixtures/java/app-start-test -name '*.java' | sort)
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class' | sort)
tools/make-apk.sh --manifest fixtures/apps/app-start-test/AndroidManifest.xml --dex "$OUT/dex/classes.dex" \
  --out "$OUT/app-start-test.apk"
