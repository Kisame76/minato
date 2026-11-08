#!/usr/bin/env bash
# Checks that the app's FixtureRunner starts a test's program only once the app has finished launching (ADR-0033
# d3's finding): on the iPhone, iOS's launch watchdog (FrontBoard, 0x8BADF00D) ends an app that has not finished its
# launch after 20 s, so an ART run-test of 30 s or more died before its end. The Simulator does not enforce that
# watchdog, so this checks the order instead: FixtureRunner writes the app's state as the program starts to
# output/launch, "active" once the launch has finished. One native fixture (hello-dyn, -runFixture) and one of ART's
# (art-hello, -runART) run in the Simulator through tools/device-suite.sh, which builds and installs the app; their
# results stay in build/simulator-suite/ and build/simulator-suite-art/, the suite's output in
# build/fixture-launch-check/. Then the installed Simulator build is asked for a test of fixtures/iphone-only.txt,
# which allocates until iOS's jetsam ends it on the iPhone: its FixtureRunner must refuse it with its line and status
# 127 before anything runs, as a Simulator app runs on the Mac's memory.
# Usage: tools/fixture-launch-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

LOGS=build/fixture-launch-check
mkdir -p "$LOGS"
failed=0
# <results folder> <test>: its program started with the app active.
check() {
  local state
  state=$(cat "$1/$2/launch" 2>/dev/null || echo "unknown, no launch file")
  if [[ "$state" == active ]]; then
    echo "ok    $2 started once the app's launch had finished"
  else
    echo "FAIL  $2 started with the app $state"
    failed=1
  fi
}

# The suite's own comparison is not this check's: its exit status only says whether the tests matched Android's.
tools/device-suite.sh --simulator hello-dyn >"$LOGS/native.log" 2>&1 || true
check build/simulator-suite hello-dyn
tools/device-suite.sh --art --simulator art-hello >"$LOGS/art.log" 2>&1 || true
check build/simulator-suite-art art-hello

# The iPhone's tests in the Simulator build: refused, with the app's line in the test's stderr and status 127.
IPHONE_ONLY=$(awk '!/^#/ && NF { print $1; exit }' fixtures/iphone-only.txt)
BUNDLE_ID=io.github.kisame76.minato
read -r DEVICE_ID _ < <(python3 tools/find-simulator.py) || true
container=$(xcrun simctl get_app_container "${DEVICE_ID:-none}" "$BUNDLE_ID" data 2>/dev/null || true)
if [[ -z "$IPHONE_ONLY" || -z "$container" ]]; then
  echo "FAIL  no iPhone-only test or no installed Simulator build to ask"
  failed=1
else
  rm -rf "${container:?}/tmp/fixtures/${IPHONE_ONLY:?}"
  xcrun simctl launch --console --terminate-running-process "$DEVICE_ID" "$BUNDLE_ID" -runFixture "$IPHONE_ONLY" \
    >"$LOGS/iphone-only.log" 2>&1 || true
  output="$container/tmp/fixtures/$IPHONE_ONLY/output"
  if grep -q "FixtureRunner: $IPHONE_ONLY runs on the iPhone only" "$output/stderr" 2>/dev/null &&
      [[ "$(cat "$output/status" 2>/dev/null)" == 127 ]]; then
    echo "ok    $IPHONE_ONLY was refused in the Simulator, as fixtures/iphone-only.txt asks"
  else
    echo "FAIL  $IPHONE_ONLY was not refused in the Simulator: status $(cat "$output/status" 2>/dev/null || echo none)"
    failed=1
  fi
fi
exit "$failed"
