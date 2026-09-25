#!/usr/bin/env bash
# Checks Minato's certificate handling in the Simulator (ADR-0023): every .p12 format SideStore
# and AltStore write, the refusals, the certificate SideStore embeds, AltStore's export with its
# callback, and a certificate change. The Debug build answers through
# app/Sources/Signing/CertificateCheck.swift; the Simulator app is signed ad hoc, so the app is told
# which certificate to take as its own (-simulatedOwnCertificate).
# Usage: tools/certificate-check.sh [simulator name or UDID]
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true
if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
python3 tools/make-test-certificates.py "$WORK/files"
INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
CONTAINER=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
CHECK="$CONTAINER/tmp/certificate-check"
rm -rf "$CHECK"
mkdir -p "$CHECK"
cp "$WORK/files/"* "$CHECK/"
OWN=$(shasum -a 256 "$WORK/files/own.der" | cut -d ' ' -f 1)
failures=0

# Starts the app with the arguments and a fresh answer file name, waits for its one-line answer and
# prints it, or "no answer".
ask() {
  local answer
  answer="answer-$(uuidgen)"
  xcrun simctl launch --terminate-running-process "$SIM_UDID" "$BUNDLE_ID" "$@" "$answer" >/dev/null
  for _ in $(seq 1 150); do
    if [[ -s "$CHECK/$answer" ]]; then
      cat "$CHECK/$answer"
      return 0
    fi
    sleep 0.2
  done
  echo "no answer"
}

expect() {
  local what=$1 wanted=$2 got=$3
  if [[ "$got" == "$wanted" ]]; then
    echo "ok    $what"
  else
    echo "FAIL  $what: got \"$got\", wanted \"$wanted\""
    failures=$((failures + 1))
  fi
}

own=(-simulatedOwnCertificate own.der)
for file in legacy openssl3 empty sidestore; do
  if [[ ! -f "$CHECK/$file.p12" ]]; then
    echo "skip  $file.p12 (not made)"
    continue
  fi
  password=pw
  [[ "$file" == empty || "$file" == sidestore ]] && password=""
  expect "$file.p12 is imported and passes the self-test" "imported $OWN selftest passed" \
    "$(ask "${own[@]}" -certificateCheckImport "$file.p12" "$password")"
done
expect "another certificate than the app's own is refused" "failed notTheAppsCertificate" \
  "$(ask "${own[@]}" -certificateCheckImport other.p12 "")"
if [[ -f "$CHECK/openssl3.p12" ]]; then
  expect "a wrong password is refused" "failed wrongPassword" \
    "$(ask "${own[@]}" -certificateCheckImport openssl3.p12 wrong)"
fi
expect "a file that is no .p12 is refused" "failed unreadable" \
  "$(ask "${own[@]}" -certificateCheckImport own.der "")"

# The automation (SigningCoordinator). -certificateCheckStatus answers once the coordinator is idle:
# "<state> <source or next step> <sha256 or store and request token>".
# 1. Installed by SideStore with ALTCertificate.p12 in the bundle: imported without any tap.
sidestore=(-simulatedInstaller sideStore -simulatedReachableStores sideStore -embeddedCertificate sidestore.p12)
expect "SideStore's embedded certificate is taken without a tap" "valid sideStoreEmbedded $OWN" \
  "$(ask "${own[@]}" "${sidestore[@]}" -certificateCheckReset -certificateCheckStatus)"
# 2. A certificate change: fetched again the same way.
expect "a changed certificate is fetched again by itself" "valid sideStoreEmbedded $OWN" \
  "$(ask "${own[@]}" "${sidestore[@]}" -certificateCheckSimulateChange -certificateCheckStatus)"
# 3. Installed by AltStore: the app opens AltStore's export link by itself (the Simulator has no
#    AltStore, so opening fails), and the callback the store would send is accepted.
altstore=(-simulatedInstaller altStore -simulatedReachableStores altStore)
first=$(ask "${own[@]}" "${altstore[@]}" -certificateCheckReset -certificateCheckStatus)
token=${first##* }
expect "without a certificate the app asks AltStore by itself" "missing requestExport altStore $token" "$first"
# The answer AltStore sends (docs/research/store-certificates.md, section 2): the base64 of the .p12,
# percent-encoded with urlQueryAllowed minus ";/?:@&=+$, ", and the password as typed, raw. The app
# opens it itself with UIApplication.open, as the store would: xcrun simctl openurl cuts URLs to
# 2047 characters and asks for confirmation first.
python3 - "$WORK/files/legacy.p12" "$token" >"$CHECK/callback.txt" <<'EOF'
import base64, sys, urllib.parse
certificate = urllib.parse.quote(base64.b64encode(open(sys.argv[1], "rb").read()).decode(), safe="")
print(f"minato://certificate?request={sys.argv[2]}&cert={certificate}&password=pw")
EOF
got=$(ask "${own[@]}" "${altstore[@]}" -certificateCheckAwaitCallback -certificateCheckOpenCallback callback.txt \
  -certificateCheckStatus)
expect "AltStore's callback is imported" "valid altStoreExport $OWN" "$got"

# 4. After a change, converted code is signed again: an app in the library with one ELF file in its
#    lib/ folder, as the import will leave it in M3, already signed with the certificate in use, so
#    that only the simulated change makes it due.
FIXTURE=build/fixtures/bin/raw-hello
if [[ -f "$FIXTURE" ]]; then
  SUPPORT="$CONTAINER/Library/Application Support"
  # The entry joins the library's own index, which stays: a launch removes the folders of apps without
  # an entry (ADR-0034). Its code lies in the folder of its version.
  VERSION="$SUPPORT/apps/org.example.resign/0123456789abcdef"
  mkdir -p "$VERSION/lib"
  cp "$FIXTURE" "$VERSION/lib/libhello.so"
  cp "$SUPPORT/library.json" "$WORK/library.json.saved" 2>/dev/null || true
  python3 - "$SUPPORT/library.json" "$OWN" <<'EOF'
import json, os, sys
path, own = sys.argv[1], sys.argv[2]
index = json.load(open(path)) if os.path.exists(path) else {"version": 1, "entries": []}
index["entries"] = [e for e in index["entries"] if e["id"] != "org.example.resign"] + [{
    "id": "org.example.resign", "displayName": "Resign Check", "state": {"ready": {}}, "splits": [], "obbs": [],
    "source": {"fileName": "resign.apk", "size": 1, "modifiedMilliseconds": 0},
    "importedAt": "2026-09-23T00:00:00Z", "signedWith": own, "folder": "0123456789abcdef"}]
with open(path, "w") as handle:
    json.dump(index, handle)
EOF
  expect "a change signs the library again" "valid sideStoreEmbedded $OWN" \
    "$(ask "${own[@]}" "${sidestore[@]}" -certificateCheckSimulateChange -certificateCheckStatus)"
  signed=$(python3 -c 'import json,sys; print(next(e for e in json.load(open(sys.argv[1]))["entries"] if e["id"] == "org.example.resign").get("signedWith"))' \
    "$SUPPORT/library.json")
  expect "the entry records the certificate that signed it" "$OWN" "$signed"
  container_made=no
  [[ -s "$VERSION/lib/libhello.so.dylib" ]] && container_made=yes
  expect "the container is written next to its ELF original" "yes" "$container_made"
  rm -rf "$SUPPORT/apps/org.example.resign"
  if [[ -f "$WORK/library.json.saved" ]]; then cp "$WORK/library.json.saved" "$SUPPORT/library.json"; else rm -f "$SUPPORT/library.json"; fi
else
  echo "skip  signing again (no $FIXTURE; tools/build-fixtures.sh builds it)"
fi

# 5. The diagnostics show what the automation did, one "key=value" line each.
diagnostics=$(ask "${own[@]}" "${sidestore[@]}" -certificateCheckDiagnostics)
value() { printf '%s\n' "$diagnostics" | sed -n "s/^$1=//p"; }
expect "diagnostics: state" "valid" "$(value state)"
expect "diagnostics: source" "sideStoreEmbedded" "$(value source)"
expect "diagnostics: installer" "sideStore" "$(value installer)"
expect "diagnostics: imported certificate" "$OWN" "$(value imported.sha256)"
expect "diagnostics: the app's certificate" "$OWN" "$(value own.sha256)"
expect "diagnostics: they match" "yes" "$(value match)"
expect "diagnostics: self-test" "passed" "$(value selftest)"
for key in imported.serial imported.expires stores embedded keychain memory.available memory.entitlement; do
  [[ -n "$(value "$key")" ]] && present=yes || present=no
  expect "diagnostics: $key is shown" "yes" "$present"
done

# 6. A self-test that ended the app leaves its marker behind; the next launch then runs nothing by
#    itself that loads code, so a broken signer cannot kill the app at every start.
touch "$CONTAINER/Library/Application Support/signing-selftest-running"
guarded=$(ask "${own[@]}" "${sidestore[@]}" -certificateCheckReset -certificateCheckStatus)
expect "after a crashed self-test the embedded file is not imported by itself" "missing requestExport sideStore" \
  "${guarded% *}"

rm -rf "$CHECK"
if [[ "$failures" -gt 0 ]]; then
  echo "$failures check(s) failed on $SIM_NAME"
  exit 1
fi
echo "All checks passed on $SIM_NAME"
