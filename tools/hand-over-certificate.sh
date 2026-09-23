#!/usr/bin/env bash
# Hands the signing certificate over to APK-Runner on the connected iPhone, so that the app signs
# converted code with it (ADR-0022), without a .p12 file exported and imported by hand.
# tools/deploy.sh runs this after every install. It needs the Debug build tools/deploy.sh
# installed: it asks the app whether it has the certificate it is signed with
# (app/Sources/Signing/CertificateHandover.swift), and only when it does not:
#   1. exports that identity, certificate and private key, from the Mac's keychain, encrypted with a
#      one-time password (apk-convert export-identity); macOS asks whether to allow the export,
#   2. copies the file into the app's tmp folder, deletes its own copy, and
#   3. starts the app with the password in its environment; the app deletes the file, imports the
#      identity into its keychain, on this device only, and has the kernel check it.
# The private key never goes into the app bundle, and nothing secret stays on disk.
# Usage: tools/hand-over-certificate.sh [--force] [device name or identifier]
#   --force  hands the certificate over even when the app already has it
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.apkrunner"
APP="build/DerivedData/Build/Products/Debug-iphoneos/APKRunner.app"
APK_CONVERT="build/native/macos/importer/converter/apk-convert"
FORCE=0
if [[ "${1:-}" == "--force" ]]; then
  FORCE=1
  shift
fi

read -r DEVICE_ID _ < <(python3 tools/find-device.py "${1:-}") || true
if [[ -z "${DEVICE_ID:-}" ]]; then
  echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
  exit 1
fi
if [[ ! -f "$APP/APKRunner" ]]; then
  echo "No device build of the app at $APP; tools/deploy.sh builds and installs it." >&2
  exit 1
fi

# The exported identity and what devicectl reports go into a folder only this user can read,
# removed on exit.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Starts the app with `request`, waits until it has answered and ended, and prints its answer.
# The answer goes into a file named anew for each request, so that none is left from an earlier
# one. False, with devicectl's messages, when there is none.
ask_app() {
  local request=$1 answer
  answer=$(uuidgen)
  xcrun devicectl device process launch --device "$DEVICE_ID" --terminate-existing --console --timeout 120 \
    "$BUNDLE_ID" -- "$request" "$answer" >"$WORK/devicectl.log" 2>&1 || true
  if ! xcrun devicectl device copy from --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" --source "tmp/signing-certificate/$answer" --destination "$WORK/$answer" \
    >>"$WORK/devicectl.log" 2>&1; then
    if grep -q "could not be, unlocked" "$WORK/devicectl.log"; then
      echo "The iPhone is locked: unlock it and run tools/hand-over-certificate.sh again." >&2
    else
      sed 's/^/  /' "$WORK/devicectl.log" >&2
    fi
    return 1
  fi
  cat "$WORK/$answer"
}

if ! status=$(ask_app -signingCertificateStatus); then
  echo "The app did not say whether it has a signing certificate." >&2
  exit 1
fi
read -r state team expires <<<"$status"
case "$state" in
  current)
    if [[ "$FORCE" == 0 ]]; then
      echo "Signing certificate: in the app (team $team, valid until $expires)"
      exit 0
    fi
    echo "Signing certificate: handing it over again (--force)" ;;
  other) echo "Signing certificate: the app has another one (team $team, valid until $expires); handing over the current one" ;;
  none) echo "Signing certificate: none in the app yet; handing it over" ;;
  *)
    echo "The app answered \"$status\" about its signing certificate." >&2
    exit 1 ;;
esac

cmake --preset macos >/dev/null
cmake --build --preset macos --target apk-convert >/dev/null
password=$(/usr/bin/openssl rand -hex 24)
echo "  macOS now asks whether apk-convert may export the key from your keychain: enter your Mac's password and choose Allow."
if ! APK_P12_PASSWORD="$password" "$APK_CONVERT" export-identity "$APP/APKRunner" "$WORK/identity.p12" >/dev/null; then
  echo "Not handed over. Allow the export, or import a .p12 file in the app (More > Signing Certificate)." >&2
  exit 1
fi
copied=0
xcrun devicectl device copy to --device "$DEVICE_ID" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
  --source "$WORK/identity.p12" --destination "tmp/signing-certificate/identity.p12" >"$WORK/devicectl.log" 2>&1 &&
  copied=1
rm -f "$WORK/identity.p12"
if [[ "$copied" == 0 ]]; then
  sed 's/^/  /' "$WORK/devicectl.log" >&2
  echo "Not handed over: the file did not reach the iPhone." >&2
  exit 1
fi

# devicectl passes DEVICECTL_CHILD_<name> to the app as <name>.
export DEVICECTL_CHILD_APK_CERTIFICATE_PASSWORD="$password"
result=$(ask_app -importSigningCertificate) || result="failed the app did not answer"
unset DEVICECTL_CHILD_APK_CERTIFICATE_PASSWORD
read -r outcome details <<<"$result"
if [[ "$outcome" != imported ]]; then
  echo "Not handed over: $details" >&2
  exit 1
fi
read -r team expires <<<"$details"
echo "Signing certificate: handed over; the iPhone accepts code signed with it (team $team, valid until $expires)"
