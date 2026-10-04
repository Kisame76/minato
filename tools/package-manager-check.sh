#!/usr/bin/env bash
# Runs ADR-0039 b3.5a's and b3.5b1's tests on the Mac (framework/tests/java_app_run.py): builds apk-run and apk-convert
# for macOS into build/native/macos-app, and app_process with libandroid_runtime.so (incremental), framework.jar
# (about 10 s), package-manager-test and package-manager-legacy-test (seconds) every time, so it never tests an old
# binary; then starts each through app_process and compares its log with Android 15's
# (fixtures/expected/<app>.logcat, from tools/android-reference.sh --app <app>). Before them,
# framework/tests/package-manager's PackageSigningTest checks on the host's JVM, with framework.jar's classes, what
# PackageManager gives of the signers, a rotated key's included, which no test app can show: no signer on the Mac makes
# a rotated APK (b3.5b1).
# Needs tools/build-art.sh, tools/build-bionic.sh, tools/build-hwui.sh and tools/build-framework-res.sh first, and
# platform.xml from tools/fetch-sources.py --set framework.
# Usage: tools/package-manager-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# PackageSigningTest needs framework.jar's classes alone, on the host's JVM: it runs before the app processes' part.
tools/build-framework.sh >/dev/null
failed=0
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
# org.json for SignersFile, as tools/framework-host-check.sh compiles it from libcore's sources.
[[ -d build/framework-host/json/org/json ]] || tools/framework-host-check.sh >/dev/null
HOST=build/package-manager-check/host
CLASSES=build/framework/classes:build/framework/stub-classes:build/framework-host/json
rm -rf "$HOST"
"$JDK/bin/javac" -source 17 -target 17 -encoding UTF-8 -Xlint:all -Werror -cp "$CLASSES" -d "$HOST" \
  framework/tests/package-manager/java/android/app/PackageSigningTest.java
"$JDK/bin/java" -cp "$HOST:$CLASSES" android.app.PackageSigningTest || failed=1

# Every app process registers hwui's natives and loads the fonts of bionic's /system (ADR-0043 test 2): shared
# builds, which this check does not make, so it stops while graphics/tests/hwui_ready.py says one is not ready.
skipped=$(python3 graphics/tests/hwui_ready.py build/hwui/lib build/bionic/system)
if [[ -n "$skipped" ]]; then echo "$skipped"; exit "$failed"; fi

NATIVE=build/native/macos-app
cmake --preset macos -B "$NATIVE" >/dev/null
cmake --build "$NATIVE" --target apk-run apk-convert >/dev/null
tools/build-framework-native.sh >/dev/null
# apk-inspect reads the APK's own certificate, which make-apk's per-machine key gives, for the harness's mark.
swift build --package-path importer/APKKit --product apk-inspect >/dev/null
INSPECT="$(swift build --package-path importer/APKKit --show-bin-path)/apk-inspect"

check() {
  local app=$1 package=$2 application=$3
  tools/build-java-test-app.sh "$app" >/dev/null
  python3 framework/tests/java_app_run.py --apk-inspect "$INSPECT" "$app" "$package" "$application" build/art \
    build/bionic/lib build/bionic/system build/hwui/lib build/sources/icu/icu4c/source/stubdata/icudt75l.dat \
    "$NATIVE/importer/converter/apk-convert" "$NATIVE/runtime/apk-run" "build/package-manager-check/$app" \
    build/framework-native/app_process64 build/framework/framework.jar build/framework-res/framework-res \
    "build/$app/$app.apk" "fixtures/expected/$app.logcat" || failed=1
}
check package-manager-test io.github.kisame76.minato.test.packagemanager \
  io.github.kisame76.minato.test.packagemanager.PackageManagerApplication
check package-manager-legacy-test io.github.kisame76.minato.test.packagemanager.legacy \
  io.github.kisame76.minato.test.packagemanager.legacy.LegacyApplication
exit $failed
