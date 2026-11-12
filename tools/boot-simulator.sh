#!/usr/bin/env bash
# Boots the Simulator <udid> for the tools, once and loudly, and is the only script that runs `simctl boot`
# (tools/tests/boot_simulator_test.sh holds the others to it). On iOS 27.2 a headless boot can leave SpringBoard in a
# crash loop: `simctl boot` fails with EINVAL, or it "succeeds" and SpringBoard then crashes every few seconds
# (FBSDisplayMonitor, no main display), each crash a dialog on the Mac's screen. So:
#   - a device that is booted already is left as it is;
#   - one `simctl boot`, never a second, and never `simctl bootstatus` (its -b form started such a loop);
#   - a failed boot prints FAIL with simctl's own words, shuts the device down and exits 1;
#   - after a boot that worked, BOOT_SIM_WATCH seconds (20 by default) of watching for a new SpringBoard-*.ips in
#     BOOT_SIM_REPORTS (~/Library/Logs/DiagnosticReports): one is a FAIL too, with the device shut down at once.
# The caller stops on exit 1; it does not try again.
# Usage: tools/boot-simulator.sh <udid>
set -uo pipefail
UDID=${1:?usage: tools/boot-simulator.sh <udid>}
REPORTS=${BOOT_SIM_REPORTS:-$HOME/Library/Logs/DiagnosticReports}
WATCH=${BOOT_SIM_WATCH:-20}

if xcrun simctl list devices | grep -q -F "($UDID) (Booted)"; then
  exit 0
fi

# The crash reports there already, so that only one written after the boot counts.
before=$(ls "$REPORTS" 2>/dev/null | grep '^SpringBoard-' | sort)
if ! words=$(xcrun simctl boot "$UDID" 2>&1); then
  echo "FAIL  the Simulator $UDID did not boot: $words" >&2
  echo "      Not tried again (a second headless boot can crash-loop SpringBoard); shut down." \
    "Boot it from the Simulator app instead." >&2
  xcrun simctl shutdown "$UDID" > /dev/null 2>&1
  exit 1
fi

second=0
while [[ $second -lt $WATCH ]]; do
  after=$(ls "$REPORTS" 2>/dev/null | grep '^SpringBoard-' | sort)
  new=$(comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") | grep -v '^$' | head -n 1)
  if [[ -n "$new" ]]; then
    xcrun simctl shutdown "$UDID" > /dev/null 2>&1
    echo "FAIL  SpringBoard crashed after the Simulator $UDID booted ($new); shut down." \
      "Boot it from the Simulator app instead." >&2
    exit 1
  fi
  sleep 1
  second=$((second + 1))
done
exit 0
