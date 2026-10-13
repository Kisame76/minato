#!/usr/bin/env bash
# Builds activity-test (ADR-0028 test 3, a NativeActivity with assets) as a split set (ADR-0048 c3 and c4): the
# NativeActivity's library lives in a config split, as Play serves an app built from a bundle. The bundle is
# formats-check's (ADR-0051, tests 3 and 4): one module with the manifest, the assets and lib/arm64-v8a/, put together
# by bundletool's build-bundle and split by its build-apks in the default mode, signed with make-apk's key, never with
# bundletool's default keystore. Result: build/activity-splits/activity-splits.apks and, for the Mac, the set's APKs
# that bundletool's extract-apks chooses for a device like Minato's, in build/activity-splits/set/, named as an
# install names them: base.apk and split_config.<config>.apk (tools/activity-splits-check.sh).
# Needs bundletool (tools/fetch-bundletool.py), the JDK of tools/fetch-sources.py --set art, tools/build-aapt2.sh,
# tools/build-framework-res.sh and tools/build-fixtures.sh (libactivity-test.so).
# Usage: tools/build-activity-splits.sh [--key <folder>]
#   --key  make-apk's key folder, default ~/.android/minato-debug-key, which tools/make-apk.sh makes on first use
set -euo pipefail
cd "$(dirname "$0")/.."

die() {
  echo "build-activity-splits: $1" >&2
  exit 1
}
key=$HOME/.android/minato-debug-key
if [[ $# -ge 2 && "$1" == --key ]]; then
  key=$2
  shift 2
fi
[[ $# -eq 0 ]] || die "usage: tools/build-activity-splits.sh [--key <folder>]"
APP=fixtures/apps/activity-test
LIBRARY=build/fixtures/bin/libactivity-test.so
JAR=$(/usr/bin/python3 tools/fetch-bundletool.py 2>/dev/null) \
  || die "bundletool differs from its pin or is missing (tools/fetch-bundletool.py)"
JDK=$(echo build/tools/jdk-17/*/Contents/Home)
AAPT2=build/aapt2/aapt2
FRAMEWORK_RES=build/framework-res/framework-res.apk
[[ -x "$JDK/bin/java" ]] || die "no JDK: run tools/fetch-sources.py --set art first"
[[ -x "$AAPT2" ]] || die "no $AAPT2 (tools/build-aapt2.sh)"
[[ -f "$FRAMEWORK_RES" ]] || die "no $FRAMEWORK_RES (tools/build-framework-res.sh)"
[[ -f "$LIBRARY" ]] || die "no $LIBRARY (tools/build-fixtures.sh)"
[[ -f "$key/key.der" && -f "$key/certificate.der" ]] \
  || die "no key.der and certificate.der in $key: tools/make-apk.sh makes them on first use"

OUT=build/activity-splits
work=""
cleanup() {
  [[ -z "$work" || -n "${KEEP_WORK:-}" ]] || rm -rf "${work:?}"
}
trap cleanup EXIT
work=$(mktemp -d "${TMPDIR:-/tmp}/build-activity-splits.XXXXXX")
chmod 700 "$work"

"$AAPT2" link --proto-format -o "$work/linked.apk" -I "$FRAMEWORK_RES" --manifest "$APP/AndroidManifest.xml"
/usr/bin/python3 - "$work/linked.apk" "$APP/assets" "$LIBRARY" "$work/base.zip" <<'PY'
import os
import sys
import zipfile

linked, assets, library, module = sys.argv[1:]
with zipfile.ZipFile(linked) as apk, zipfile.ZipFile(module, "w", zipfile.ZIP_DEFLATED) as out:
    for entry in apk.infolist():
        name = "manifest/AndroidManifest.xml" if entry.filename == "AndroidManifest.xml" else entry.filename
        out.writestr(name, apk.read(entry))
    out.write(library, "lib/arm64-v8a/" + os.path.basename(library))
    for directory, _, files in sorted(os.walk(assets)):
        for file in sorted(files):
            path = os.path.join(directory, file)
            out.write(path, os.path.join("assets", os.path.relpath(path, assets)))
PY
# stored.ogg stays stored, as a developer's noCompress keeps it, since the app reads it through a descriptor.
echo '{"compression": {"uncompressedGlob": ["assets/stored.ogg"]}}' >"$work/config.json"
"$JDK/bin/java" -jar "$JAR" build-bundle --modules="$work/base.zip" --config="$work/config.json" \
  --output="$work/activity-splits.aab" >/dev/null

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
"$JDK/bin/java" -jar "$JAR" build-apks --bundle="$work/activity-splits.aab" --output="$work/activity-splits.apks" \
  --aapt2="$AAPT2" --ks="$work/key.p12" --ks-key-alias=minato --ks-pass="file:$work/password" \
  --key-pass="file:$work/password" >"$work/build-apks.log" 2>&1 \
  || { sed 's/^/  /' "$work/build-apks.log" >&2; die "bundletool cannot build the set"; }

rm -rf "${OUT:?}"
mkdir -p "$OUT/set"
cat >"$work/device.json" <<JSON
{"supportedAbis": ["arm64-v8a"], "supportedLocales": ["en"], "screenDensity": 480, "sdkVersion": 35}
JSON
"$JDK/bin/java" -jar "$JAR" extract-apks --apks="$work/activity-splits.apks" --output-dir="$work/chosen" \
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
mv "$work/activity-splits.apks" "$OUT/activity-splits.apks"
echo "build-activity-splits: wrote $OUT/activity-splits.apks and $(ls "$OUT/set" | wc -l | tr -d ' ') APKs in $OUT/set"
