#!/usr/bin/env bash
# Builds packs-test (ADR-0050 test 2) as a bundletool .apks, build/packs-test/packs-test.apks: the app of
# tools/build-java-test-app.sh packs-test as the base module, and two asset packs with one asset each, fastfollow
# (fast-follow) and ondemand (on-demand), put together by bundletool's build-bundle and split by its build-apks in the
# default mode, as Play serves an app, signed with make-apk's key, never with bundletool's default keystore, as
# tools/aab-to-apk.sh signs. The library keeps the packs' slices in the version's packs/ (ADR-0049 f), which
# tools/formats-check.sh checks with this set.
# Needs bundletool (tools/fetch-bundletool.py), the JDK of tools/fetch-sources.py --set art, tools/build-aapt2.sh,
# tools/build-framework-res.sh and what tools/build-java-test-app.sh needs.
# Usage: tools/build-packs-test.sh [--key <folder>]
#   --key  make-apk's key folder, default ~/.android/minato-debug-key, which tools/make-apk.sh makes on first use
set -euo pipefail
cd "$(dirname "$0")/.."

die() {
  echo "build-packs-test: $1" >&2
  exit 1
}
key=$HOME/.android/minato-debug-key
if [[ $# -eq 2 && "$1" == --key ]]; then
  key=$2
elif [[ $# -ne 0 ]]; then
  die "usage: tools/build-packs-test.sh [--key <folder>]"
fi
JAR=$(/usr/bin/python3 tools/fetch-bundletool.py 2>/dev/null) \
  || die "bundletool differs from its pin or is missing (tools/fetch-bundletool.py)"
JAVA=$(find -L build/tools/jdk-17 -maxdepth 5 -path '*/bin/java' -type f 2>/dev/null | head -n 1)
[[ -n "$JAVA" ]] || die "no JDK in build/tools/jdk-17"
AAPT2=build/aapt2/aapt2
FRAMEWORK_RES=build/framework-res/framework-res.apk
[[ -x "$AAPT2" ]] || die "no $AAPT2 (tools/build-aapt2.sh)"
[[ -f "$FRAMEWORK_RES" ]] || die "no $FRAMEWORK_RES (tools/build-framework-res.sh)"
[[ -f "$key/key.der" && -f "$key/certificate.der" ]] \
  || die "no key.der and certificate.der in $key: tools/make-apk.sh makes them on first use"

tools/build-java-test-app.sh packs-test >/dev/null
OUT=build/packs-test
PACKAGE=io.github.kisame76.minato.test.packs

work=""
cleanup() {
  [[ -z "$work" ]] || rm -rf "${work:?}"
}
trap cleanup EXIT
work=$(mktemp -d "${TMPDIR:-/tmp}/build-packs-test.XXXXXX")
chmod 700 "$work"

# The modules in protobuf's format, as build-bundle takes them: the base with the app's dex, and each pack with its
# manifest and asset; bundletool refuses a resource table in an asset pack, which aapt2 writes anyway.
mkdir -p "$work/modules"
"$AAPT2" link --proto-format -o "$work/base-linked.apk" -I "$FRAMEWORK_RES" \
  --manifest fixtures/apps/packs-test/AndroidManifest.xml
for pack in fastfollow:fast-follow ondemand:on-demand; do
  name=${pack%%:*}
  mkdir -p "$work/$name/assets"
  cat >"$work/$name/AndroidManifest.xml" <<EOF
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:dist="http://schemas.android.com/apk/distribution" package="$PACKAGE" split="$name">
  <dist:module dist:type="asset-pack"><dist:fusing dist:include="true"/><dist:delivery><dist:${pack#*:}/></dist:delivery></dist:module>
</manifest>
EOF
  printf 'Minato packs-test, the %s pack.\n' "$name" >"$work/$name/assets/$name.txt"
  "$AAPT2" link --proto-format -o "$work/$name-linked.apk" -I "$FRAMEWORK_RES" \
    --manifest "$work/$name/AndroidManifest.xml"
done
/usr/bin/python3 - "$work" "$OUT/dex/classes.dex" <<'EOF'
import os
import sys
import zipfile

work, dex = sys.argv[1:]
for name in ("base", "fastfollow", "ondemand"):
    with zipfile.ZipFile(os.path.join(work, f"{name}-linked.apk")) as apk, \
            zipfile.ZipFile(os.path.join(work, "modules", f"{name}.zip"), "w", zipfile.ZIP_DEFLATED) as module:
        for entry in apk.infolist():
            if name != "base" and entry.filename == "resources.pb":
                continue
            target = "manifest/AndroidManifest.xml" if entry.filename == "AndroidManifest.xml" else entry.filename
            module.writestr(target, apk.read(entry))
        if name == "base":
            module.write(dex, "dex/classes.dex")
        else:
            module.write(os.path.join(work, name, "assets", f"{name}.txt"), f"assets/{name}.txt")
EOF
"$JAVA" -jar "$JAR" build-bundle --modules="$work/modules/base.zip,$work/modules/fastfollow.zip,$work/modules/ondemand.zip" \
  --output="$work/packs-test.aab" >/dev/null

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
"$JAVA" -jar "$JAR" build-apks --bundle="$work/packs-test.aab" --output="$work/packs-test.apks" --aapt2="$AAPT2" \
  --ks="$work/key.p12" --ks-key-alias=minato --ks-pass="file:$work/password" --key-pass="file:$work/password" \
  >"$work/build-apks.log" 2>&1 \
  || { sed 's/^/  /' "$work/build-apks.log" >&2; die "bundletool cannot build the set"; }

mv "$work/packs-test.apks" "$OUT/packs-test.apks"
echo "build-packs-test: wrote $OUT/packs-test.apks ($(stat -f %z "$OUT/packs-test.apks") bytes)"
