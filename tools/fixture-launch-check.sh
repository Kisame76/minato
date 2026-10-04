#!/usr/bin/env bash
# Checks that the app's FixtureRunner starts a test's program only once the app has finished launching (ADR-0033
# d3's finding): on the iPhone, iOS's launch watchdog (FrontBoard, 0x8BADF00D) ends an app that has not finished its
# launch after 20 s, so an ART run-test of 30 s or more died before its end. The Simulator does not enforce that
# watchdog, so this checks the order instead: FixtureRunner writes the app's state as the program starts to
# output/launch, "active" once the launch has finished. One native fixture (hello-dyn, -runFixture) and one of ART's
# (art-hello, -runART) run in the Simulator through tools/device-suite.sh, which builds and installs the app; their
# results stay in build/simulator-suite/ and build/simulator-suite-art/, the suite's output in
# build/fixture-launch-check/.
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
exit "$failed"
