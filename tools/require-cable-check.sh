#!/usr/bin/env bash
# Checks that nothing reaches the iPhone unless it is connected by cable (the user's rule of 2026-10-23: installs and
# runs on it only over USB, never over the local network), without a device: tools/require-cable.py reads what a fake
# xcrun's `devicectl list devices` reports, and each script that reaches the iPhone runs in a scratch tree whose
# find-device.py, builds, deploy.sh, hand-over-certificate.sh, xcodebuild, xcrun, swift, cmake and git are fakes.
# Over the local network each script must stop with require-cable.py's message before its first device access or
# build. tools/java-start-check.sh checks again before each devicectl call, so a cable pulled during its run stops
# it before the next one, also where that call's output goes to /dev/null.
# Usage: tools/require-cable-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
failed=0
MESSAGE="not by cable"
# Runs $2... and ends it after $1 seconds: a script that missed its stop would wait for the app's answers.
limited() { perl -e '$s = shift @ARGV; alarm $s; exec @ARGV or die "$ARGV[0]: $!\n"' "$@"; }

# The devices as devicectl's JSON v5 reports them, with TRANSPORT for the iPhone's transport: "both" with the
# properties and the deprecated fields, "deprecated" with those only, "split" with the properties' TRANSPORT and the
# deprecated fields' wired, "bare" without a transport, "none" with the Simulator alone.
devices() {
  /usr/bin/python3 - "$1" <<'EOF'
import json
import sys

layout = sys.argv[1]
phone = {"identifier": "FAKE-DEVICE",
         "hardwareProperties": {"platform": "iOS", "reality": "physical", "udid": "FAKE-UDID"},
         "deviceProperties": {"name": "Test iPhone"},
         "connectionProperties": {"transportType": "wired" if layout == "split" else "TRANSPORT"}}
if layout == "bare":
    del phone["connectionProperties"]
elif layout != "deprecated":
    phone["properties"] = {"hardware": {"platform": "iOS", "reality": "physical", "udid": "FAKE-UDID"},
                           "state": {"name": "Test iPhone"},
                           "connection": {"state": "connected", "transportType": "TRANSPORT"}}
simulator = {"identifier": "FAKE-SIMULATOR",
             "hardwareProperties": {"platform": "iOS", "reality": "simulated", "udid": "FAKE-SIMULATOR"},
             "connectionProperties": {"transportType": "wired"}}
listed = [simulator] if layout == "none" else [simulator, phone]
print(json.dumps({"info": {"jsonVersion": 5, "outcome": "success"}, "result": {"devices": listed}}))
EOF
}

# A tree $1 with the fakes; the transports of the listings, one per `devicectl list devices` and the last one for
# every later one, are $2 ("fail" for a devicectl that fails, "broken" for JSON cut short), the layout $3.
make_tree() {
  local tree=$1 transports=$2 layout=${3:-both} script
  mkdir -p "$tree/tools" "$tree/fake"
  [[ ! -f tools/require-cable.py ]] || cp tools/require-cable.py "$tree/tools/"
  devices "$layout" >"$tree/devices.json.in"
  echo "$transports" >"$tree/transports"
  printf 'print("FAKE-DEVICE FAKE-UDID enabled connected")\n' >"$tree/tools/find-device.py"
  printf 'print("FAKE-SIMULATOR Simulator")\n' >"$tree/tools/find-simulator.py"
  for script in gen.sh build-native.sh build-graphics-host.sh build-bench.sh build-fixtures.sh build-bionic.sh \
      build-java-test-app.sh build-splits-test.sh build-godot.sh sim.sh; do
    printf '#!/bin/sh\necho "build %s $*" >>"%s/build.log"\n' "$script" "$tree" >"$tree/tools/$script"
  done
  for script in deploy.sh hand-over-certificate.sh; do
    printf '#!/bin/sh\necho "%s $*" >>"%s/access.log"\n' "$script" "$tree" >"$tree/tools/$script"
  done
  printf '#!/bin/sh\necho "xcodebuild $*" >>"%s/access.log"\n' "$tree" >"$tree/fake/xcodebuild"
  for script in swift cmake; do
    printf '#!/bin/sh\necho "build %s $*" >>"%s/build.log"\n' "$script" "$tree" >"$tree/fake/$script"
  done
  printf '#!/bin/sh\necho 0000000\n' >"$tree/fake/git"
  cat >"$tree/fake/xcrun" <<EOF
#!/bin/bash
tree="$tree"
EOF
  cat >>"$tree/fake/xcrun" <<'EOF'
if [[ "$1 $2 $3" == "devicectl list devices" ]]; then
  count=$(( $(cat "$tree/lists" 2>/dev/null || echo 0) + 1 ))
  echo "$count" >"$tree/lists"
  read -r -a transports <"$tree/transports"
  index=$(( count <= ${#transports[@]} ? count - 1 : ${#transports[@]} - 1 ))
  transport=${transports[$index]}
  output=""
  while [[ $# -gt 0 ]]; do
    [[ "$1" != --json-output ]] || output=$2
    shift
  done
  case "$transport" in
    fail) echo "ERROR: The device list could not be read." >&2; exit 1 ;;
    broken) printf '{"result": {"devices": [' >"$output" ;;
    *) sed "s/TRANSPORT/$transport/g" "$tree/devices.json.in" >"$output" ;;
  esac
  exit 0
fi
echo "xcrun $*" >>"$tree/access.log"
if [[ "$*" == *"info processes"* ]]; then
  while [[ $# -gt 0 ]]; do
    [[ "$1" != -j ]] || echo '{"result": {"runningProcesses": []}}' >"$2"
    shift
  done
fi
exit 0
EOF
  chmod +x "$tree/tools/"*.sh "$tree/fake/"*
}

report() {
  local name=$1 problems=$2 tree=$3
  if [[ -z "$problems" ]]; then
    echo "same     $name"
  else
    echo "DIFFERS  $name:$problems"
    sed 's/^/  /' "$tree/out"
    failed=1
  fi
}

# require-cable.py with the device $3 ("" for the first iPhone) over the transports $4 in layout $5 must exit with $2.
check_tool() {
  local name=$1 expected=$2 wanted=$3 tree="$WORK/tool-$((++cases))" status=0 problems=""
  make_tree "$tree" "$4" "${5:-both}"
  (cd "$tree" && PATH="$tree/fake:$PATH" python3 tools/require-cable.py ${wanted:+"$wanted"}) >"$tree/out" 2>&1 \
    || status=$?
  [[ "$status" == "$expected" ]] || problems+=" exit $status"
  if [[ "$expected" == 0 ]]; then
    [[ ! -s "$tree/out" ]] || problems+=" printed something"
  else
    grep -qi "$MESSAGE" "$tree/out" || problems+=" no message"
  fi
  report "$name" "$problems" "$tree"
}
cases=0

check_tool "an iPhone by cable passes" 0 "" "wired"
check_tool "an iPhone over the local network stops" 1 "" "localNetwork"
check_tool "the iPhone named by its identifier, by cable" 0 FAKE-DEVICE "wired"
check_tool "the iPhone named by its UDID, by cable" 0 FAKE-UDID "wired"
check_tool "the iPhone named by its name, by cable" 0 "Test iPhone" "wired"
check_tool "the iPhone named by its name, over the network" 1 "Test iPhone" "localNetwork"
check_tool "a device that is not listed stops" 1 OTHER-DEVICE "wired"
check_tool "the Simulator, wired as devicectl says, is no iPhone" 1 FAKE-SIMULATOR "wired"
check_tool "no iPhone at all stops" 1 "" "wired" none
check_tool "the deprecated fields alone, by cable" 0 "" "wired" deprecated
check_tool "the deprecated fields alone, over the network" 1 "" "localNetwork" deprecated
check_tool "the properties over the network beat the deprecated wired" 1 "" "localNetwork" split
check_tool "an iPhone without a transport stops" 1 "" "wired" bare
check_tool "a devicectl that fails stops" 1 "" "fail"
check_tool "JSON cut short stops" 1 "" "broken"
check_tool "an unknown transport stops" 1 "" "usb"

# Script $2 with the arguments after $4 and the environment $3 ("" for none) over the transports $4: it must stop
# with the message before any device access or build.
check_stops() {
  local name=$1 script=$2 environment=$3 transports=$4 tree="$WORK/script-$((++cases))" status=0 problems=""
  shift 4
  make_tree "$tree" "$transports"
  cp "tools/$script" "$tree/tools/$script"
  (cd "$tree" && limited 60 env $environment TMPDIR="$tree" PATH="$tree/fake:$PATH" "tools/$script" "$@") \
    >"$tree/out" 2>&1 || status=$?
  [[ "$status" != 0 ]] || problems+=" exit 0"
  grep -qi "$MESSAGE" "$tree/out" || problems+=" no message"
  [[ ! -s "$tree/access.log" ]] || problems+=" reached the device: $(tr '\n' ';' <"$tree/access.log")"
  [[ ! -s "$tree/build.log" ]] || problems+=" built: $(head -1 "$tree/build.log")"
  report "$name" "$problems" "$tree"
}

check_stops "deploy.sh over the network" deploy.sh "INSTALL_ONLY=1" localNetwork
check_stops "hand-over-certificate.sh over the network" hand-over-certificate.sh "" localNetwork FAKE-DEVICE
check_stops "device-suite.sh over the network" device-suite.sh "" localNetwork
check_stops "benchmark.sh over the network" benchmark.sh "" localNetwork
check_stops "record-session.sh over the network" record-session.sh "" localNetwork --no-build \
  io.github.kisame76.minato.example
check_stops "session-check.sh over the network" session-check.sh "" localNetwork --device
check_stops "java-start-check.sh over the network" java-start-check.sh "ONLY=app-start-test" localNetwork --device

# The Simulator paths never ask for a cable and never list the devices, which the fake tree shows as far as their
# Simulator's lookup.
check_simulator() {
  local name=$1 script=$2 tree="$WORK/simulator-$((++cases))" status=0 problems=""
  shift 2
  make_tree "$tree" localNetwork
  : >"$tree/tools/find-simulator.py" # no Simulator, so that the run ends at its device's lookup
  cp "tools/$script" "$tree/tools/$script"
  (cd "$tree" && TMPDIR="$tree" PATH="$tree/fake:$PATH" limited 20 "tools/$script" "$@") >"$tree/out" 2>&1 \
    || status=$?
  ! grep -qi "$MESSAGE" "$tree/out" || problems+=" asked for a cable"
  [[ ! -e "$tree/lists" ]] || problems+=" listed the devices"
  report "$name" "$problems" "$tree"
}
check_simulator "record-session.sh in the Simulator" record-session.sh --simulator --no-build \
  io.github.kisame76.minato.example
check_simulator "device-suite.sh in the Simulator" device-suite.sh --simulator
check_simulator "benchmark.sh in the Simulator" benchmark.sh --simulator

# deploy.sh by cable goes on to its build and install.
tree="$WORK/deploy-wired"
make_tree "$tree" wired
cp tools/deploy.sh "$tree/tools/deploy.sh"
status=0
(cd "$tree" && INSTALL_ONLY=1 PATH="$tree/fake:$PATH" tools/deploy.sh) >"$tree/out" 2>&1 || status=$?
problems=""
[[ "$status" == 0 ]] || problems+=" exit $status"
grep -q "devicectl device install app" "$tree/access.log" 2>/dev/null || problems+=" not installed"
report "deploy.sh by cable installs" "$problems" "$tree"

# java-start-check.sh with APP over the transports $2: the device accesses before the stop must be $3, a ";" list
# of what access.log has, each line up to its first option.
check_pulled() {
  local name=$1 transports=$2 expected=$3 tree="$WORK/pulled-$((++cases))" status=0 problems="" seen
  make_tree "$tree" "$transports"
  cp tools/java-start-check.sh "$tree/tools/java-start-check.sh"
  mkdir -p "$tree/Minato.app"
  (cd "$tree" && ONLY=app-start-test APP="$tree/Minato.app" TMPDIR="$tree" PATH="$tree/fake:$PATH" \
    limited 60 tools/java-start-check.sh --device) >"$tree/out" 2>&1 || status=$?
  [[ "$status" != 0 ]] || problems+=" exit 0"
  grep -qi "$MESSAGE" "$tree/out" || problems+=" no message"
  seen=$( { sed 's/ --.*//' "$tree/access.log" 2>/dev/null || true; } | tr '\n' ';')
  [[ "$seen" == "$expected" ]] || problems+=" accessed \"$seen\", not \"$expected\""
  report "$name" "$problems" "$tree"
}
# The start's check, then the install's; the cable is gone at the install.
check_pulled "a cable pulled before the install stops it" "wired localNetwork" ""
# The start's, the install's, then the first call after the hand-over, info processes, whose output goes to
# /dev/null: the message must still show.
check_pulled "a cable pulled after the install stops the next call" "wired wired localNetwork" \
  "xcrun devicectl device install app;hand-over-certificate.sh FAKE-DEVICE;"
# Then the copy of the requests into the app's folder, and the launch of Minato after it.
check_pulled "a cable pulled before a copy stops it" "wired wired wired localNetwork" \
  "xcrun devicectl device install app;hand-over-certificate.sh FAKE-DEVICE;xcrun devicectl device info processes;"
check_pulled "a cable pulled before the launch stops it" "wired wired wired wired localNetwork" \
  "xcrun devicectl device install app;hand-over-certificate.sh FAKE-DEVICE;xcrun devicectl device info processes;\
xcrun devicectl device copy to;"

exit "$failed"
