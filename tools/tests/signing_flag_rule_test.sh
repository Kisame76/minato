#!/usr/bin/env bash
# The signing flag's rule over tools/*.sh: Xcode talks to Apple, and needs the signed-in account (which a command-line
# build with the flag can drop), only when tools/provisioning-updates.sh says a profile is about to run out. So:
#   1. -allowProvisioningUpdates and -allowProvisioningDeviceRegistration appear, outside comments, only in
#      provisioning-updates.sh (which prints the first) and in deploy.sh's one line that adds the device registration
#      after the tool asked for the flag; every other script takes the tool's output as its words;
#   2. a script whose xcodebuild has a device destination (generic/platform=iOS without " Simulator", or a device's id)
#      takes its signing from provisioning-updates.sh or builds unsigned (CODE_SIGNING_ALLOWED=NO);
#   3. a script that builds unsigned carries no flag.
# The rule runs on the repo's scripts and, first, on planted violations in a temp copy, each of which must fail it.
# Usage: tools/tests/signing_flag_rule_test.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
WORK=$(mktemp -d)
trap 'rm -rf "${WORK:?}"' EXIT

# The rule over the scripts of folder $1: one line per violation, nothing when it holds.
violations() {
  /usr/bin/python3 - "$1" <<'PY'
import glob
import os
import re
import sys

folder = sys.argv[1]
FLAG = re.compile(r"-allowProvisioning(Updates|DeviceRegistration)")
DEVICE = re.compile(r"generic/platform=iOS(?! Simulator)|-destination\s+\"?id=\$\{?DEVICE")
for path in sorted(glob.glob(os.path.join(folder, "*.sh"))):
    name = os.path.basename(path)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    code = [(n, l) for n, l in enumerate(lines, 1) if not l.lstrip().startswith("#")]
    text = "\n".join(l for _, l in code)
    unsigned = "CODE_SIGNING_ALLOWED=NO" in text
    for n, line in code:
        if not FLAG.search(line) or name == "provisioning-updates.sh":
            continue
        before = lines[n - 2] if n >= 2 else ""
        if (name == "deploy.sh" and "provisioning-updates.sh" in before
                and "-allowProvisioningDeviceRegistration" in line):
            continue
        print(f"{name}:{n}: the signing flag written out; take provisioning-updates.sh's output instead")
        if unsigned:
            print(f"{name}:{n}: the signing flag in a script that builds unsigned (CODE_SIGNING_ALLOWED=NO)")
    if "xcodebuild" in text and DEVICE.search(text) and "provisioning-updates.sh" not in text and not unsigned:
        print(f"{name}: an xcodebuild for a device that neither asks provisioning-updates.sh nor builds unsigned")
PY
}

failed=0
# Planted violations, each in a copy of the scripts: the rule must catch every one.
plant() {  # name, file, text
  rm -rf "$WORK/tools" && cp -R tools "$WORK/tools"
  printf '%s\n' "$3" >> "$WORK/tools/$2"
  if [[ -z "$(violations "$WORK/tools" | grep -F "$2" || true)" ]]; then
    echo "FAIL  the rule misses $1"
    failed=1
  fi
}
plant "a bare flag in a script" planted.sh \
  'xcodebuild -scheme Minato -destination generic/platform=iOS -allowProvisioningUpdates build'
plant "the flag in an unsigned build" planted.sh \
  'xcodebuild -destination generic/platform=iOS CODE_SIGNING_ALLOWED=NO -allowProvisioningUpdates'
plant "a device build without the tool" planted.sh 'xcodebuild -scheme Minato -destination generic/platform=iOS build'
plant "a device id build without the tool" planted.sh 'xcodebuild -scheme Minato -destination "id=$DEVICE_UDID" build'
plant "deploy.sh's registration without the tool's question" deploy.sh 'SIGNING=(-allowProvisioningDeviceRegistration)'
# And the rule must not fire on a Simulator build or an unsigned one.
rm -rf "$WORK/tools" && mkdir "$WORK/tools"
printf '%s\n' 'xcodebuild -destination "generic/platform=iOS Simulator" build' \
  'xcodebuild -destination generic/platform=iOS CODE_SIGNING_ALLOWED=NO build' > "$WORK/tools/fine.sh"
if [[ -n "$(violations "$WORK/tools")" ]]; then
  echo "FAIL  the rule fires on a Simulator or unsigned build"
  failed=1
fi

found=$(violations tools)
if [[ -n "$found" ]]; then
  echo "FAIL  the repo's scripts break the signing flag's rule:"
  sed 's/^/      /' <<< "$found"
  failed=1
fi
[[ $failed == 1 ]] || echo "signing_flag_rule_test: ok"
exit $failed
