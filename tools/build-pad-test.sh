#!/usr/bin/env bash
# Builds pad-test (ADR-0050 test 3) with Google's Play libraries, asset-delivery 2.2.2 and feature-delivery 2.1.0 with
# the libraries their POMs name, as AARs from tools/fetch-play-libraries.py, into build/pad-test/: the app of
# fixtures/java/pad-test as the base module, and two asset packs with one asset each, fastfollow (fast-follow) and
# ondemand (on-demand), and an on-demand feature module, extra (ADR-0050 c2), with a class of its own and a title
# string in the base, put together by bundletool's build-bundle and split by its build-apks twice, signed with
# make-apk's key: pad-test.apks in the default mode, as a user's file brings an app, which Minato imports, and
# pad-test-local.apks in the local-testing mode, whose base names local_testing_dir and which bundletool's install-apks
# puts on the emulator with the pack slices in local_testing (tools/android-reference.sh). From pad-test.apks it also
# takes the base and the packs' slices, as the library keeps them, for the Mac (tools/packs-check.sh): base-master.apk
# and slices/<pack>-master.apk, the module's split extra-master.apk among them.
# As an app's build does: javac compiles the app against the boot jars' classes, framework.jar's classes with their
# stubs and the AARs' classes.jar, never against the SDK's android.jar (M4 plan); aapt2 links the app's manifest, into
# which the AARs' manifests are merged by hand, with the AARs' resources and writes their R classes; D8 dexes the app
# with the libraries. The libraries stay a black box: only javac, D8 and aapt2 read them (ADR-0050 c)), and they
# never go into the repository or the app.
# Needs tools/fetch-play-libraries.py --fetch (on the user's go, with the terms the user accepted), bundletool
# (tools/fetch-bundletool.py), the JDK and D8 of tools/fetch-sources.py --set art, tools/build-art.sh's boot classes,
# tools/build-framework.sh's output, tools/build-aapt2.sh and tools/build-framework-res.sh.
# Usage: tools/build-pad-test.sh [--key <folder>]
#   --key  make-apk's key folder, default ~/.android/minato-debug-key, which tools/make-apk.sh makes on first use
set -euo pipefail
cd "$(dirname "$0")/.."

die() {
  echo "build-pad-test: $1" >&2
  exit 1
}
key=$HOME/.android/minato-debug-key
if [[ $# -eq 2 && "$1" == --key ]]; then
  key=$2
elif [[ $# -ne 0 ]]; then
  die "usage: tools/build-pad-test.sh [--key <folder>]"
fi
LIBRARIES=$(/usr/bin/python3 tools/fetch-play-libraries.py 2>/dev/null) \
  || die "no Play libraries, or not the pinned ones: tools/fetch-play-libraries.py says what to do"
JAR=$(/usr/bin/python3 tools/fetch-bundletool.py 2>/dev/null) \
  || die "bundletool differs from its pin or is missing (tools/fetch-bundletool.py)"
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
[[ -f "$key/key.der" && -f "$key/certificate.der" ]] \
  || die "no key.der and certificate.der in $key: tools/make-apk.sh makes them on first use"

APP=pad-test
OUT=build/$APP
PACKAGE=io.github.kisame76.minato.test.pad
work=""
cleanup() {
  [[ -z "$work" ]] || rm -rf "${work:?}"
}
trap cleanup EXIT
work=$(mktemp -d "${TMPDIR:-/tmp}/build-pad-test.XXXXXX")
chmod 700 "$work"

# The AARs, unpacked: each one's classes.jar for javac and D8, its res/ for aapt2, and its manifest's package, the
# package of its R class.
classpath=()
resources=()
packages=()
for aar in "$LIBRARIES"/*.aar; do
  name=$(basename "$aar" .aar)
  mkdir -p "$work/aar/$name"
  unzip -q "$aar" -d "$work/aar/$name"
  [[ ! -f "$work/aar/$name/classes.jar" ]] || classpath+=("$work/aar/$name/classes.jar")
  if [[ -d "$work/aar/$name/res" && -n "$(ls -A "$work/aar/$name/res")" ]]; then
    "$AAPT2" compile --dir "$work/aar/$name/res" -o "$work/$name-res.zip"
    resources+=("$work/$name-res.zip")
    packages+=("$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$work/aar/$name/AndroidManifest.xml" | head -n 1)")
  fi
done
[[ ${#classpath[@]} -gt 0 ]] || die "the AARs in $LIBRARIES hold no classes.jar"
libraries_classpath=$(IFS=:; echo "${classpath[*]}")

# The base in protobuf's format, as build-bundle takes it, and the R classes of the app and of each AAR with
# resources.
mkdir -p "$work/gen" "$work/classes" "$work/dex" "$work/modules"
extra=()
if [[ ${#packages[@]} -gt 0 ]]; then
  extra=(--extra-packages "$(IFS=:; echo "${packages[*]}")")
fi
"$AAPT2" compile --dir "fixtures/apps/$APP/res" -o "$work/app-res.zip"
resources+=("$work/app-res.zip")
"$AAPT2" link --proto-format -o "$work/base-linked.apk" -I "$FRAMEWORK_RES" --emit-ids "$work/ids.txt" \
  --manifest "fixtures/apps/$APP/AndroidManifest.xml" --java "$work/gen" ${extra[@]+"${extra[@]}"} \
  ${resources[@]+"${resources[@]}"}
# -Xlint:-options: the notes about source 8 on a newer JDK.
"$JDK/bin/javac" -source 8 -target 8 -Xlint:-options -bootclasspath "$BOOT_CLASSES" \
  -classpath "$FRAMEWORK_CLASSES:$STUBS:$libraries_classpath" -d "$work/classes" \
  $(find "fixtures/java/$APP" "$work/gen" -name '*.java' | sort)
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --lib "$FRAMEWORK_CLASSES" --lib "$STUBS" --output "$work/dex" \
  $(find "$work/classes" -name '*.class' | sort) "${classpath[@]}"
[[ -f "$work/dex/classes.dex" && ! -f "$work/dex/classes2.dex" ]] || die "D8 wrote other than one classes.dex"

# The feature module's code, a class of its own that the base does not hold, in a dex of its own.
mkdir -p "$work/classes-extra" "$work/dex-extra"
"$JDK/bin/javac" -source 8 -target 8 -Xlint:-options -bootclasspath "$BOOT_CLASSES" -d "$work/classes-extra" \
  $(find "fixtures/java/$APP-extra" -name '*.java' | sort)
"$JDK/bin/java" -cp "$R8" com.android.tools.r8.D8 --release --min-api 29 --lib "$BOOT_CLASSES" \
  --output "$work/dex-extra" $(find "$work/classes-extra" -name '*.class' | sort)

# The packs, each with its manifest and one asset; bundletool refuses a resource table in an asset pack, which aapt2
# writes anyway.
for pack in fastfollow:fast-follow ondemand:on-demand; do
  name=${pack%%:*}
  mkdir -p "$work/$name/assets"
  cat >"$work/$name/AndroidManifest.xml" <<EOF
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:dist="http://schemas.android.com/apk/distribution" package="$PACKAGE" split="$name">
  <dist:module dist:type="asset-pack"><dist:fusing dist:include="true"/><dist:delivery><dist:${pack#*:}/></dist:delivery></dist:module>
</manifest>
EOF
  printf 'Minato pad-test, the %s pack.\n' "$name" >"$work/$name/assets/$name.txt"
  "$AAPT2" link --proto-format -o "$work/$name-linked.apk" -I "$FRAMEWORK_RES" \
    --manifest "$work/$name/AndroidManifest.xml"
done
# The module's title is the base's string, with the id the base gave it, which bundletool looks up in the base.
"$AAPT2" link --proto-format -o "$work/extra-linked.apk" -I "$FRAMEWORK_RES" --auto-add-overlay \
  --stable-ids "$work/ids.txt" -R "$work/app-res.zip" \
  --manifest "fixtures/apps/$APP/extra/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 35
/usr/bin/python3 - "$work" <<'EOF'
import os
import sys
import zipfile

work = sys.argv[1]
for name in ("base", "fastfollow", "ondemand", "extra"):
    with zipfile.ZipFile(os.path.join(work, f"{name}-linked.apk")) as apk, \
            zipfile.ZipFile(os.path.join(work, "modules", f"{name}.zip"), "w", zipfile.ZIP_DEFLATED) as module:
        for entry in apk.infolist():
            if name != "base" and entry.filename == "resources.pb":
                continue
            target = "manifest/AndroidManifest.xml" if entry.filename == "AndroidManifest.xml" else entry.filename
            module.writestr(target, apk.read(entry))
        if name == "base":
            module.write(os.path.join(work, "dex", "classes.dex"), "dex/classes.dex")
        elif name == "extra":
            module.write(os.path.join(work, "dex-extra", "classes.dex"), "dex/classes.dex")
        else:
            module.write(os.path.join(work, name, "assets", f"{name}.txt"), f"assets/{name}.txt")
EOF
"$JDK/bin/java" -jar "$JAR" build-bundle \
  --modules="$work/modules/base.zip,$work/modules/fastfollow.zip,$work/modules/ondemand.zip,$work/modules/extra.zip" \
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
for mode in default local; do
  options=()
  output=$APP.apks
  if [[ "$mode" == local ]]; then
    options=(--local-testing)
    output=$APP-local.apks
  fi
  "$JDK/bin/java" -jar "$JAR" build-apks --bundle="$work/$APP.aab" --output="$work/$output" --aapt2="$AAPT2" \
    --ks="$work/key.p12" --ks-key-alias=minato --ks-pass="file:$work/password" --key-pass="file:$work/password" \
    ${options[@]+"${options[@]}"} >"$work/build-apks-$mode.log" 2>&1 \
    || { sed 's/^/  /' "$work/build-apks-$mode.log" >&2; die "bundletool cannot build the $mode set"; }
done

rm -rf "${OUT:?}"
mkdir -p "$OUT/slices"
mv "$work/$APP.apks" "$work/$APP-local.apks" "$OUT/"
unzip -q -o -j "$OUT/$APP.apks" splits/base-master.apk -d "$OUT"
unzip -q -o -j "$OUT/$APP.apks" 'asset-slices/*' -d "$OUT/slices"
# The on-demand feature module's split, which the local-testing mode puts in local_testing beside the packs'.
unzip -q -o -j "$OUT/$APP.apks" splits/extra-master.apk -d "$OUT/slices"
# bundletool's entries carry the mode 0666 and its fixed time, which unzip restores.
chmod 0644 "$OUT/base-master.apk" "$OUT"/slices/*.apk
touch "$OUT/base-master.apk" "$OUT"/slices/*.apk
echo "build-pad-test: wrote $OUT/$APP.apks, $OUT/$APP-local.apks, $OUT/base-master.apk and $(ls "$OUT/slices" | wc -l | tr -d ' ') slices"
