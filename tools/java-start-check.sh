#!/usr/bin/env bash
# Checks ADR-0039 c5 in the Simulator: a Java app starts in game mode from the library, as a NativeActivity app
# does. app-start-test (ADR-0039's test 1), context-test (step c1's, started twice over the same data),
# asset-ndk-test (ADR-0041 decision 7's, whose JNI library System.loadLibrary loads) and jni-lib-test (step c5.1's,
# which looks at its JNI library in nativeLibraryDir) are imported and tapped in the library, and each one's lines
# must be Android 15's (fixtures/expected/<app>.logcat) as the Mac's harness compares
# them: app-start-test's System.out lines (framework/tests/app_process_run.py), which SessionCheck's guest-log request
# sends to tmp/session-check/guest-out, and the lines of the others' own tags in their guest logs, of both of
# context-test's starts (framework/tests/java_app_run.py). An app process never ends by itself (step c2b): 2 s after
# the app's last line the script ends Minato, as tools/android-reference.sh stops the app on the AVD after that line,
# and it prints the time from the tap to that line. The Debug build answers through
# app/Sources/Session/SessionCheck.swift. The test apps leave the library before the run, which a killed run may have
# left them in, and after it; the library is then as before.
# Usage: tools/java-start-check.sh [simulator name or UDID]
# Optional environment:
#   APP   a built Minato.app (Debug, for the Simulator) to install in place of tools/sim.sh's build
#   LOGS  a folder that keeps each start's guest log and stdout, <app>-<start>.log and .out, and a screenshot
#         <app>-<start>.png of a start that failed, which shows an alert's text
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true
if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

# The test apps, "<app> <package>", and where each one's last line appears.
APPS=("app-start-test io.github.kisame76.minato.test.appstart" "context-test io.github.kisame76.minato.test.context"
      "asset-ndk-test io.github.kisame76.minato.test.assetndk" "jni-lib-test io.github.kisame76.minato.test.jnilib")
last_line() {
  case "$1" in
    app-start-test) echo "guest-out ^app-start-test: MessageDigest " ;; # app_process_run.LAST
    *) echo "guest-log ^I/$1: done$" ;;                                 # java_app_run's "<app>: done"
  esac
}
for app in "${APPS[@]}"; do
  tools/build-java-test-app.sh "${app%% *}" >/dev/null
done

if [[ -n "${APP:-}" ]]; then
  xcrun simctl boot "$SIM_UDID" 2>/dev/null || true # already booted is fine
  xcrun simctl install "$SIM_UDID" "$APP"
else
  INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
fi
CONTAINER=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
SUPPORT="$CONTAINER/Library/Application Support"
CHECK="$CONTAINER/tmp/session-check"
EVENTS="$CHECK/events"
REQUEST="$SUPPORT/launch-request.json"
WORK=$(mktemp -d)

launch() { xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null; }
terminate() { xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true; }

# The test apps go through the library, as session-check.sh removes its own, then what is left of them. Fails while
# the index still names one.
remove_test_apps() {
  terminate
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  for app in "${APPS[@]}"; do echo "${app#* }"; done > "$CHECK/end"
  launch || true
  for _ in $(seq 1 50); do [[ -d "$CHECK" ]] || break; sleep 0.2; done
  terminate
  rm -rf "$CHECK" "$REQUEST"
  for app in "${APPS[@]}"; do
    rm -rf "${SUPPORT:?}/apps/${app#* }" "${SUPPORT:?}/data/${app#* }" "${CONTAINER:?}/Library/Caches/guest/${app#* }"
  done
  python3 - "$SUPPORT/library.json" "${APPS[@]#* }" <<'EOF'
import json
import sys
try:
    ids = {entry["id"] for entry in json.load(open(sys.argv[1]))["entries"]}
except FileNotFoundError:
    sys.exit(0)
left = [package for package in sys.argv[2:] if package in ids]
if left:
    print(f"still in the library: {' '.join(left)}")
    sys.exit(1)
EOF
}

# A killed run leaves its apps in the library. Saved with them, the index would bring them back at the end without
# their files, and an import of the same APK keeps such an entry, whose tap fails.
mkdir -p "$SUPPORT/incoming"
remove_test_apps
cp "$SUPPORT/library.json" "$WORK/library.json.saved" 2>/dev/null || true
had_data=no
[[ -d "$SUPPORT/data" ]] && had_data=yes

restore() {
  remove_test_apps || true
  if [[ "$had_data" == no ]]; then rmdir "$SUPPORT/data" 2>/dev/null || true; fi
  if [[ -f "$WORK/library.json.saved" ]]; then cp "$WORK/library.json.saved" "$SUPPORT/library.json"; else rm -f "$SUPPORT/library.json"; fi
  if [[ -n "${LOGS:-}" ]]; then
    mkdir -p "$LOGS"
    cp "$WORK"/*.log "$WORK"/*.out "$LOGS/" 2>/dev/null || true
  fi
  rm -rf "${WORK:?}"
}
trap restore EXIT

failures=0
ok() { echo "ok    $1"; }
fail() {
  echo "FAIL  $1"
  failures=$((failures + 1))
}

# The events the app appended, "<seconds> <pid> <event>"; `seen` counts the lines already taken. Waits up to $2
# seconds for the next event that matches the extended regex $1 and puts it into EVENT, as session-check.sh does.
seen=0
EVENT=""
await_event() {
  local pattern=$1 end=$((SECONDS + ${2:-20})) found
  while true; do
    found=$(tail -n +$((seen + 1)) "$EVENTS" 2>/dev/null | grep -n -E -m 1 "^[0-9.]+ [0-9]+ ($pattern)" || true)
    if [[ -n "$found" ]]; then
      seen=$((seen + ${found%%:*}))
      EVENT=${found#*:}
      return 0
    fi
    (( SECONDS < end )) || break
    sleep 0.2
  done
  EVENT=""
  return 1
}
event_of() { local rest=${EVENT#* }; echo "${rest#* }"; }
# A screenshot of the Simulator into LOGS as $1.png, for a start that failed.
screenshot() {
  [[ -n "${LOGS:-}" ]] || return 0
  mkdir -p "$LOGS"
  xcrun simctl io "$SIM_UDID" screenshot "$LOGS/$1.png" >/dev/null 2>&1 || true
}
# Field $1 of app $2's entry.
field() {
  python3 -c 'import json,sys; e={e["id"]:e for e in json.load(open(sys.argv[1]))["entries"]}.get(sys.argv[2]); print(None if e is None else e.get(sys.argv[3]))' \
    "$SUPPORT/library.json" "$2" "$1" 2>/dev/null || echo None
}

# One start of app $1 with package $2, the $3rd: the tap, game mode, the app's lines up to its last one, and Minato's
# end 2 s later. Its guest log and stdout are kept as $WORK/$1-$3.log and .out, empty when it did not start.
run_start() {
  local app=$1 package=$2 round=$3 file pattern tap now end reached=""
  read -r file pattern <<< "$(last_line "$app")"
  echo "== $app, start $round"
  terminate
  rm -f "$REQUEST" "$CHECK/guest-log" "$CHECK/guest-out"
  : > "$WORK/$app-$round.log"
  : > "$WORK/$app-$round.out"
  : > "$EVENTS"
  seen=0
  echo "$package" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  launch
  if ! await_event "decision .*" 60; then
    fail "  no tap in the library"
    return
  fi
  tap=${EVENT%% *}
  if ! await_event "process gameMode $package.*" 60; then
    fail "  no game mode after the tap"
    sed 's/^/      | /' "$EVENTS"
    screenshot "$app-$round"
    return
  fi
  end=$((SECONDS + 300))
  while (( SECONDS < end )); do
    if grep -q -E "$pattern" "$CHECK/$file" 2>/dev/null; then
      reached=yes
      break
    fi
    if tail -n +$((seen + 1)) "$EVENTS" | grep -q -E '^[0-9.]+ [0-9]+ (could not start|program ended) '; then break; fi
    sleep 0.2
  done
  now=$(python3 -c 'import time; print(time.time())')
  if [[ -n "$reached" ]]; then
    ok "  its last line, $(python3 -c 'import sys; print(f"{float(sys.argv[2]) - float(sys.argv[1]):.1f}")' "$tap" "$now") s after the tap"
    sleep 2
  else
    fail "  no last line"
    screenshot "$app-$round"
    sed 's/^/      | /' "$EVENTS"
    tail -n 40 "$CHECK/guest-log" 2>/dev/null | sed 's/^/      | /' || true
  fi
  terminate
  cp "$CHECK/guest-log" "$WORK/$app-$round.log" 2>/dev/null || : > "$WORK/$app-$round.log"
  cp "$CHECK/guest-out" "$WORK/$app-$round.out" 2>/dev/null || : > "$WORK/$app-$round.out"
}

# Import the apps; the Simulator signs a ready app's code ad hoc at its first tap.
terminate
rm -f "$REQUEST"
rm -rf "$CHECK"
mkdir -p "$CHECK"
: > "$EVENTS"
for app in "${APPS[@]}"; do
  cp "build/${app%% *}/${app%% *}.apk" "$SUPPORT/incoming/"
done
launch
for app in "${APPS[@]}"; do
  package=${app#* }
  for _ in $(seq 1 150); do
    [[ "$(field state "$package")" == "{'ready': {}}" ]] && break
    sleep 0.2
  done
  state=$(field state "$package")
  if [[ "$state" == "{'ready': {}}" ]]; then ok "$package is ready"; else fail "$package is not ready: $state"; fi
done

run_start app-start-test io.github.kisame76.minato.test.appstart 1
python3 - "$WORK/app-start-test-1.out" "$WORK/app-start-test-1.log" fixtures/expected/app-start-test.logcat <<'EOF' \
  || failures=$((failures + 1))
import sys
sys.path.insert(0, "framework/tests")
from app_process_run import expected_lines, our_lines

out, log, expected = sys.argv[1:]
got, wanted = our_lines(open(out, encoding="utf-8", errors="replace").read()), expected_lines(expected)
if got == wanted:
    print(f"ok      app-start-test printed what Android 15's app process shows, {len(got)} lines")
    sys.exit(0)
print("FAIL    app-start-test's lines differ from Android 15's:")
for line in wanted:
    if line not in got:
        print(f"      | only Android's: {line}")
for line in got:
    if line not in wanted:
        print(f"      | only ours:      {line}")
print("      | its guest log, last 40 lines:")
for line in open(log, encoding="utf-8", errors="replace").read().splitlines()[-40:]:
    print(f"      | {line}")
sys.exit(1)
EOF

run_start context-test io.github.kisame76.minato.test.context 1
run_start context-test io.github.kisame76.minato.test.context 2
python3 - "$WORK/context-test-1.log" "$WORK/context-test-2.log" fixtures/expected/context-test.logcat <<'EOF' \
  || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected = sys.argv[1:]
# One log of the two starts, as java_app_run.py --restart counts them.
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
compare_log("context-test", subprocess.CompletedProcess([], 0, "", log), expected, {})
EOF

# Their Applications load their JNI libraries; jni-lib-test also looks at its own in nativeLibraryDir, the code path's
# lib/arm64, where the library puts clones of the version's originals.
run_one() {
  run_start "$1" "$2" 1
  python3 - "$1" "$WORK/$1-1.log" "fixtures/expected/$1.logcat" <<'EOF' || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

app, log, expected = sys.argv[1:]
compare_log(app, subprocess.CompletedProcess([], 0, "", open(log, encoding="utf-8", errors="replace").read()), expected,
            {})
EOF
}
run_one asset-ndk-test io.github.kisame76.minato.test.assetndk
run_one jni-lib-test io.github.kisame76.minato.test.jnilib

echo "java start check: $failures failed"
exit $((failures > 0))
