#!/usr/bin/env bash
# Turns an Android App Bundle into one universal APK on the Mac (ADR-0051): bundletool's build-apks --mode=universal
# with our JDK and our aapt2, which takes the base and every feature module and install-time asset pack that fuses
# (<dist:fusing dist:include="true"/>), merges their manifests and resource tables and keeps every lib/<abi>/. The APK
# is always signed with make-apk's key, never with bundletool's default keystore: the key folder's key.der (PKCS#1, RSA)
# and certificate.der (ADR-0031) become a PKCS#12 in a private temporary folder, with a random password in a file of
# its own that bundletool reads (--ks-pass=file:), so the password never shows in a process list; the folder goes on
# every exit, so neither the keystore nor its password lands in build/ or the repo. The APK is written as
# <out>.partial and renamed, so no half-written APK exists; on success one line, as make-apk prints it.
# Usage: tools/aab-to-apk.sh <bundle.aab> --out <file.apk> [--key <folder>]
#   --key  make-apk's key folder, default ~/.android/minato-debug-key, which tools/make-apk.sh makes on first use
# Exit status: 0 written, 1 an error, 64 wrong arguments.
# Optional environment:
#   JDK    the JDK folder, default build/tools/jdk-17
#   AAPT2  aapt2, default build/aapt2/aapt2 (tools/build-aapt2.sh)
set -euo pipefail

usage() {
  echo "Usage: tools/aab-to-apk.sh <bundle.aab> --out <file.apk> [--key <folder>]" >&2
  exit 64
}
die() {
  echo "aab-to-apk: $1" >&2
  exit 1
}

bundle="" out="" key="$HOME/.android/minato-debug-key"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      [[ $# -ge 2 ]] || usage
      out=$2
      shift 2
      ;;
    --key)
      [[ $# -ge 2 ]] || usage
      key=$2
      shift 2
      ;;
    -*) usage ;;
    *)
      [[ -z "$bundle" ]] || usage
      bundle=$1
      shift
      ;;
  esac
done
[[ -n "$bundle" && -n "$out" ]] || usage

root="$(cd "$(dirname "$0")/.." && pwd)"
JAR=$(/usr/bin/python3 "$root/tools/fetch-bundletool.py" 2>/dev/null) \
  || die "bundletool is missing or differs from its pin: run tools/fetch-bundletool.py --fetch"
jdk=${JDK:-$root/build/tools/jdk-17}
JAVA=$(find -L "$jdk" -maxdepth 5 -path '*/bin/java' -type f 2>/dev/null | head -n 1 || true)
[[ -n "$JAVA" ]] || die "no JDK in $jdk"
AAPT2=${AAPT2:-$root/build/aapt2/aapt2}
[[ -x "$AAPT2" ]] || die "no $AAPT2: run tools/build-aapt2.sh"
[[ -f "$bundle" ]] || die "no bundle $bundle"
[[ -d "$(dirname "$out")" ]] || die "no folder for $out"
[[ -f "$key/key.der" && -f "$key/certificate.der" ]] \
  || die "no key.der and certificate.der in $key: tools/make-apk.sh makes them on first use"

work=""
cleanup() {
  [[ -z "$work" ]] || rm -rf "${work:?}"
  rm -f "${out:?}.partial"
}
trap cleanup EXIT
work=$(mktemp -d "${TMPDIR:-/tmp}/aab-to-apk.XXXXXX")
chmod 700 "$work"

# The keystore: the key and its certificate as PEM, then a PKCS#12 under a random password, all in the private folder.
(
  umask 077
  head -c 32 /dev/urandom | base64 | tr -d '\n' >"$work/password" \
    && /usr/bin/openssl rsa -inform DER -in "$key/key.der" -out "$work/key.pem" 2>/dev/null \
    && /usr/bin/openssl x509 -inform DER -in "$key/certificate.der" -out "$work/certificate.pem" \
    && /usr/bin/openssl pkcs12 -export -inkey "$work/key.pem" -in "$work/certificate.pem" -name minato \
      -passout "file:$work/password" -out "$work/key.p12"
) || die "the key in $key cannot be made into a keystore"

"$JAVA" -jar "$JAR" build-apks --bundle="$bundle" --output="$work/universal.apks" --mode=universal \
  --aapt2="$AAPT2" --ks="$work/key.p12" --ks-key-alias=minato --ks-pass="file:$work/password" \
  --key-pass="file:$work/password" >"$work/build-apks.log" 2>&1 \
  || { sed 's/^/  /' "$work/build-apks.log" >&2; die "bundletool cannot build a universal APK from $bundle"; }
unzip -p "$work/universal.apks" universal.apk >"$out.partial" 2>/dev/null && [[ -s "$out.partial" ]] \
  || die "bundletool's output holds no universal.apk"
mv "$out.partial" "$out"
echo "aab-to-apk: wrote $out ($(stat -f %z "$out") bytes, SHA-256 $(shasum -a 256 "$out" | cut -d ' ' -f 1))"
