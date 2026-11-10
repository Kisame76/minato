#!/usr/bin/env bash
# Builds workmanager-test with androidx.work 2.12.0 and its closure (tools/fetch-workmanager-libraries.py, fetched on
# request after the user's OK; Apache-2.0, never in the repository or the app) into build/workmanager-test/: the app of
# fixtures/java/workmanager-test, whose manifest merges the libraries' declarations by hand, as an app's build does.
# javac compiles the app against the boot jars' classes, framework.jar's classes with their stubs and the libraries'
# classes.jar, never against the SDK's android.jar (M4 plan); aapt2 links the app's manifest with the AARs' resources
# and writes their R classes; D8 dexes the app with the libraries, into as many dexes as it needs; make-apk packs them.
# The libraries stay a black box: only javac, D8 and aapt2 read them.
# Needs tools/fetch-workmanager-libraries.py --fetch, the JDK and D8 of tools/fetch-sources.py --set art,
# tools/build-art.sh's boot classes, tools/build-framework.sh's output, tools/build-aapt2.sh and
# tools/build-framework-res.sh.
# Usage: tools/build-workmanager-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

die() {
  echo "build-workmanager-test: $1" >&2
  exit 1
}
LIBS=()
LIST=(/usr/bin/python3 tools/fetch-workmanager-libraries.py --list)
while IFS= read -r file; do LIBS+=("$file"); done < <("${LIST[@]}" 2>/dev/null) || true
[[ ${#LIBS[@]} -gt 0 ]] \
  || die "no WorkManager libraries, or not the pinned ones: tools/fetch-workmanager-libraries.py says what to do"
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
FRAMEWORK_CLASSES=build/framework/classes.jar
STUBS=build/framework/stubs.jar
AAPT2=build/aapt2/aapt2
FRAMEWORK_RES=build/framework-res/framework-res.apk
[[ -x "$JDK/bin/javac" && -f "$R8" && -f "$BOOT_CLASSES" ]] \
  || die "no JDK, D8 or $BOOT_CLASSES: run tools/fetch-sources.py --set art and tools/build-art.sh first"
[[ -f "$FRAMEWORK_CLASSES" && -f "$STUBS" ]] || die "no $FRAMEWORK_CLASSES or $STUBS: run tools/build-framework.sh"
[[ -x "$AAPT2" ]] || die "no $AAPT2 (tools/build-aapt2.sh)"
[[ -f "$FRAMEWORK_RES" ]] || die "no $FRAMEWORK_RES (tools/build-framework-res.sh)"

APP=workmanager-test
OUT=build/$APP
work=""
cleanup() {
  [[ -z "$work" ]] || rm -rf "${work:?}"
}
trap cleanup EXIT
work=$(mktemp -d "${TMPDIR:-/tmp}/build-workmanager-test.XXXXXX")
chmod 700 "$work"

# Each AAR unpacked: its classes.jar for javac and D8, its res/ for aapt2 and the package of its R class; each JAR
# as it is.
classpath=()
resources=()
packages=()
for lib in "${LIBS[@]}"; do
  name=$(basename "$lib")
  name=${name%.*}
  if [[ "$lib" == *.jar ]]; then
    classpath+=("$lib")
    continue
  fi
  mkdir -p "$work/aar/$name"
  unzip -q "$lib" -d "$work/aar/$name"
  [[ ! -f "$work/aar/$name/classes.jar" ]] || classpath+=("$work/aar/$name/classes.jar")
  if [[ -d "$work/aar/$name/res" && -n "$(find "$work/aar/$name/res" -type f | head -n 1)" ]]; then
    "$AAPT2" compile --dir "$work/aar/$name/res" -o "$work/$name-res.zip"
    resources+=("$work/$name-res.zip")
    packages+=("$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$work/aar/$name/AndroidManifest.xml" | head -n 1)")
  fi
done
libraries_classpath=$(IFS=:; echo "${classpath[*]}")

mkdir -p "$work/gen" "$work/classes" "$work/dex" "$OUT"
extra=()
if [[ ${#packages[@]} -gt 0 ]]; then
  extra=(--extra-packages "$(IFS=:; echo "${packages[*]}")")
fi
"$AAPT2" link -o "$work/resources.apk" -I "$FRAMEWORK_RES" --manifest "fixtures/apps/$APP/AndroidManifest.xml" \
  --java "$work/gen" ${extra[@]+"${extra[@]}"} ${resources[@]+"${resources[@]}"}
# -Xlint:-options: the notes about source 8 on a newer JDK.
"$JDK/bin/javac" -source 8 -target 8 -Xlint:-options -bootclasspath "$BOOT_CLASSES" \
  -classpath "$FRAMEWORK_CLASSES:$STUBS:$libraries_classpath" -d "$work/classes" \
  $(find "fixtures/java/$APP" "$work/gen" -name '*.java' | sort)
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" --output "$work/dex" \
  $(find "$work/classes" -name '*.class' | sort) "${classpath[@]}" \
  2> >(grep -v 'kotlin' >&2)
dexes=()
for dex in "$work"/dex/classes*.dex; do dexes+=(--dex "$dex"); done
tools/make-apk.sh --resources "$work/resources.apk" "${dexes[@]}" --out "$OUT/$APP.apk"
echo "build-workmanager-test: wrote $OUT/$APP.apk, $(( ${#dexes[@]} / 2 )) dex file(s)"
