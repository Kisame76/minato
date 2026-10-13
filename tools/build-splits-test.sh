#!/usr/bin/env bash
# Builds splits-test (ADR-0048 c), the guest side of split sets) as a bundletool .apks, build/splits-test/splits-test.apks:
# a base module (fixtures/java/splits-test/base, its resources with a language variant, and a JNI library), an
# install-time feature module (fixtures/java/splits-test/feature, with an asset), put together by bundletool's
# build-bundle and split by its build-apks in the default mode, as Play serves an app, signed with make-apk's key,
# never with bundletool's default keystore. For the Mac it also takes the set's APKs out of the .apks into
# build/splits-test/set/, named as an install names them: base.apk and split_<split name>.apk (tools/splits-check.sh).
# As an app's build does: javac compiles against the boot jars' classes and framework.jar's classes with their stubs,
# never against the SDK's android.jar (M4 plan); aapt2 links the manifest and resources in protobuf's format, as
# build-bundle takes them; D8 dexes each module; the NDK's clang builds the JNI library, as
# tools/build-java-test-app.sh builds one.
# Needs bundletool (tools/fetch-bundletool.py), the JDK and D8 of tools/fetch-sources.py --set art, tools/build-art.sh's
# boot classes, tools/build-framework.sh's output, tools/build-aapt2.sh, tools/build-framework-res.sh and the Android
# NDK (ANDROID_NDK, default as tools/build-java-test-app.sh).
# Usage: tools/build-splits-test.sh [--key <folder>] [<app>]
#   --key  make-apk's key folder, default ~/.android/minato-debug-key, which tools/make-apk.sh makes on first use
#   <app>  splits-test (the default) or another set of the same shape, such as isolated-splits-test: its manifest and
#          resources in fixtures/apps/<app>, a feature module in each folder of it that holds an AndroidManifest.xml
#          (with res/ and assets/ if it has any), its code in fixtures/java/<app>/base and fixtures/java/<app>/<module>,
#          and the JNI libraries of fixtures/java/<app>/jni/*.c, which the base module holds
set -euo pipefail
cd "$(dirname "$0")/.."

die() {
  echo "build-splits-test: $1" >&2
  exit 1
}
key=$HOME/.android/minato-debug-key
if [[ $# -ge 2 && "$1" == --key ]]; then
  key=$2
  shift 2
fi
APP=splits-test
if [[ $# -eq 1 ]]; then
  APP=$1
elif [[ $# -ne 0 ]]; then
  die "usage: tools/build-splits-test.sh [--key <folder>] [<app>]"
fi
[[ -f "fixtures/apps/$APP/AndroidManifest.xml" ]] || die "no fixtures/apps/$APP/AndroidManifest.xml"
JAR=$(/usr/bin/python3 tools/fetch-bundletool.py 2>/dev/null) \
  || die "bundletool differs from its pin or is missing (tools/fetch-bundletool.py)"
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
R8=build/sources/r8/r8.jar
BOOT_CLASSES=build/art/java/all-classes.jar
FRAMEWORK_CLASSES=build/framework/classes.jar
STUBS=build/framework/stubs.jar
AAPT2=build/aapt2/aapt2
FRAMEWORK_RES=build/framework-res/framework-res.apk
NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/30.0.16248370}"
CLANG="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang"
[[ -x "$JDK/bin/javac" && -f "$R8" && -f "$BOOT_CLASSES" ]] \
  || die "no JDK, D8 or $BOOT_CLASSES: run tools/fetch-sources.py --set art and tools/build-art.sh first"
[[ -f "$FRAMEWORK_CLASSES" && -f "$STUBS" ]] || die "no $FRAMEWORK_CLASSES or $STUBS: run tools/build-framework.sh"
[[ -x "$AAPT2" ]] || die "no $AAPT2 (tools/build-aapt2.sh)"
[[ -f "$FRAMEWORK_RES" ]] || die "no $FRAMEWORK_RES (tools/build-framework-res.sh)"
[[ -x "$CLANG" ]] || die "no Android NDK at $NDK: set ANDROID_NDK to its folder"
[[ -f "$key/key.der" && -f "$key/certificate.der" ]] \
  || die "no key.der and certificate.der in $key: tools/make-apk.sh makes them on first use"

OUT=build/$APP
MODULES=()
for manifest in fixtures/apps/$APP/*/AndroidManifest.xml; do
  MODULES+=("$(basename "$(dirname "$manifest")")")
done
work=""
cleanup() {
  [[ -z "$work" || -n "${KEEP_WORK:-}" ]] || rm -rf "${work:?}"
}
trap cleanup EXIT
work=$(mktemp -d "${TMPDIR:-/tmp}/build-splits-test.XXXXXX")
chmod 700 "$work"
mkdir -p "$work/gen" "$work/classes/base" "$work/dex/base" "$work/lib" "$work/modules"

# The base module: resources and manifest linked in protobuf's format, with the R class for its code.
"$AAPT2" compile --dir "fixtures/apps/$APP/res" -o "$work/compiled.zip"
"$AAPT2" link --proto-format -o "$work/base-linked.apk" -I "$FRAMEWORK_RES" --emit-ids "$work/ids.txt" \
  --manifest "fixtures/apps/$APP/AndroidManifest.xml" --java "$work/gen" "$work/compiled.zip"
# -Xlint:-options: the notes about source 8 on a newer JDK.
"$JDK/bin/javac" -source 8 -target 8 -Xlint:-options -bootclasspath "$BOOT_CLASSES" \
  -classpath "$FRAMEWORK_CLASSES:$STUBS" -d "$work/classes/base" \
  $(find "fixtures/java/$APP/base" "$work/gen" -name '*.java' | sort)
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" --output "$work/dex/base" \
  $(find "$work/classes/base" -name '*.class' | sort)
# Each feature module: its manifest, its code, and its resources, whose ids take their places beside the base's
# (--stable-ids), as the splits of one package share them.
earlier="$work/classes/base"  # the code a module may use: the base's and that of the modules before it
for module in "${MODULES[@]}"; do
  mkdir -p "$work/classes/$module" "$work/dex/$module"
  "$JDK/bin/javac" -source 8 -target 8 -Xlint:-options -bootclasspath "$BOOT_CLASSES" \
    -classpath "$FRAMEWORK_CLASSES:$STUBS:$earlier" -d "$work/classes/$module" \
    $(find "fixtures/java/$APP/$module" -name '*.java' | sort)
  libs=()
  for dir in ${earlier//:/ }; do libs+=(--lib "$dir"); done
  "$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
    --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" "${libs[@]}" --output "$work/dex/$module" \
    $(find "$work/classes/$module" -name '*.class' | sort)
  earlier="$earlier:$work/classes/$module"
  compiled=()
  if [[ -d "fixtures/apps/$APP/$module/res" ]]; then
    "$AAPT2" compile --dir "fixtures/apps/$APP/$module/res" -o "$work/$module-compiled.zip"
    compiled=("$work/$module-compiled.zip")
  fi
  "$AAPT2" link --proto-format -o "$work/$module-linked.apk" -I "$FRAMEWORK_RES" --stable-ids "$work/ids.txt" \
    --manifest "fixtures/apps/$APP/$module/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 35 \
    ${compiled[@]+"${compiled[@]}"}
done
# The JNI libraries, which the arm64 split holds.
LIBRARIES=()
for source in fixtures/java/$APP/jni/*.c; do
  [[ -f "$source" ]] || continue
  library=lib$(basename "$source" .c).so
  "$CLANG" --target=aarch64-linux-android29 -shared -fPIC -O2 -Wall -Werror -Wl,-z,max-page-size=16384 \
    -Wl,--no-undefined -landroid -o "$work/lib/$library" "$source"
  LIBRARIES+=("$library")
done

/usr/bin/python3 - "$work" "fixtures/apps/$APP" "${LIBRARIES[*]-}" "${MODULES[@]}" <<'PY'
import os
import sys
import zipfile

work, app, libraries = sys.argv[1:4]
modules = sys.argv[4:]
# The assets of each module (bundletool refuses one name with different content in two modules).
assets = {"base": os.path.join(app, "assets")}
assets.update({module: os.path.join(app, module, "assets") for module in modules})
for name in ["base"] + modules:
    with zipfile.ZipFile(os.path.join(work, f"{name}-linked.apk")) as apk, \
            zipfile.ZipFile(os.path.join(work, "modules", f"{name}.zip"), "w", zipfile.ZIP_DEFLATED) as module:
        for entry in apk.infolist():
            target = "manifest/AndroidManifest.xml" if entry.filename == "AndroidManifest.xml" else entry.filename
            module.writestr(target, apk.read(entry))
        module.write(os.path.join(work, "dex", name, "classes.dex"), "dex/classes.dex")
        if name == "base":
            for library in libraries.split():
                module.write(os.path.join(work, "lib", library), "lib/arm64-v8a/" + library)
        if os.path.isdir(assets[name]):
            for asset in sorted(os.listdir(assets[name])):
                module.write(os.path.join(assets[name], asset), "assets/" + asset)
PY
modules_list=$work/modules/base.zip
for module in "${MODULES[@]}"; do modules_list+=,$work/modules/$module.zip; done
"$JDK/bin/java" -jar "$JAR" build-bundle --modules="$modules_list" \
  --output="$work/$APP.aab" >/dev/null

# The keystore: make-apk's key and certificate as PEM, then a PKCS#12 under a random password, all in the private
# folder, as tools/aab-to-apk.sh makes it.
(
  umask 077
  head -c 32 /dev/urandom | base64 | tr -d '\n' >"$work/password" \
    && /usr/bin/openssl rsa -inform DER -in "$key/key.der" -out "$work/key.pem" 2>/dev/null \
    && /usr/bin/openssl x509 -inform DER -in "$key/certificate.der" -out "$work/certificate.pem" \
    && /usr/bin/openssl pkcs12 -export -inkey "$work/key.pem" -in "$work/certificate.pem" -name minato \
      -passout "file:$work/password" -out "$work/key.p12"
) || die "the key in $key cannot be made into a keystore"
"$JDK/bin/java" -jar "$JAR" build-apks --bundle="$work/$APP.aab" --output="$work/$APP.apks" --aapt2="$AAPT2" \
  --ks="$work/key.p12" --ks-key-alias=minato --ks-pass="file:$work/password" --key-pass="file:$work/password" \
  >"$work/build-apks.log" 2>&1 \
  || { sed 's/^/  /' "$work/build-apks.log" >&2; die "bundletool cannot build the set"; }

rm -rf "${OUT:?}"
mkdir -p "$OUT/set"
# The set for the Mac: what bundletool's extract-apks chooses for a device like Minato's (arm64-v8a, @3x, English, API
# 35), named as an install names them: base.apk, and for each split of the base module split_config.<config>.apk, of
# another module split_<module>.apk or split_<module>.config.<config>.apk.
cat >"$work/device.json" <<JSON
{"supportedAbis": ["arm64-v8a"], "supportedLocales": ["en"], "screenDensity": 480, "sdkVersion": 35}
JSON
"$JDK/bin/java" -jar "$JAR" extract-apks --apks="$work/$APP.apks" --output-dir="$work/chosen" \
  --device-spec="$work/device.json" >/dev/null
/usr/bin/python3 - "$work/chosen" "$OUT/set" <<'PY'
import os
import re
import shutil
import sys

source, target = sys.argv[1:]
for entry in sorted(os.listdir(source)):
    if not entry.endswith(".apk"):
        continue
    # bundletool names a split <module>-master or <module>-<config>, with _2 for the second variant of the same.
    module, _, config = re.sub(r"_\d+$", "", entry[:-4]).partition("-")
    if module == "base" and config == "master":
        name = "base.apk"
    elif config == "master":
        name = f"split_{module}.apk"
    elif module == "base":
        name = f"split_config.{config}.apk"
    else:
        name = f"split_{module}.config.{config}.apk"
    shutil.copyfile(os.path.join(source, entry), os.path.join(target, name))
PY
mv "$work/$APP.apks" "$OUT/$APP.apks"
echo "build-splits-test: wrote $OUT/$APP.apks and $(ls "$OUT/set" | wc -l | tr -d ' ') APKs in $OUT/set"
