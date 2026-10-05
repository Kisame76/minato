#!/usr/bin/env bash
# Checks ADR-0039 c5 in the Simulator or, with --device, on the iPhone (step c6): a Java app starts in game mode from
# the library, as a NativeActivity app does. app-start-test (ADR-0039's test 1), context-test (step c1's, started
# twice over the same data), asset-ndk-test (ADR-0041 decision 7's, whose JNI library System.loadLibrary loads) and
# jni-lib-test (step c5.1's, which looks at its JNI library in nativeLibraryDir) are imported and tapped in the
# library, and each one's lines must be Android 15's (fixtures/expected/<app>.logcat) as the Mac's harness compares
# them: app-start-test's System.out lines (framework/tests/app_process_run.py), which SessionCheck's guest-log request
# sends to tmp/session-check/guest-out, and the lines of the others' own tags in their guest logs, of both of
# context-test's starts (framework/tests/java_app_run.py). An app process never ends by itself (step c2b): 2 s after
# the app's last line the script ends Minato, as tools/android-reference.sh stops the app on the AVD after that line,
# and it prints the time from the tap to that line. On the iPhone, java-test (ADR-0043 step c1) follows: twice the
# tap, its window's focus, Home (Settings comes to the front), the return to Minato and the app's own finish, as on
# the AVD, whose display is the iPhone 15 Pro Max's; it prints the time from the tap to its first onResume, ADR-0039
# decision 1's start time, from the app's own clock when it logs it. The Debug build answers through
# app/Sources/Session/SessionCheck.swift; on the iPhone devicectl sends the requests and fetches the events and files,
# as tools/session-check.sh does, the app must stay unlocked, and the times count the fetches' delay too. The test
# apps leave the library before the run, which a killed run may have left them in, and after it; in the Simulator the
# library is then as before.
# Usage: tools/java-start-check.sh [--device] [simulator or iPhone name or identifier]
# Optional environment:
#   APP        a built Minato.app (Debug) to install in place of tools/sim.sh's or, with --device, tools/deploy.sh's
#              build; on the iPhone it goes over the installed Minato, whose data stays
#   LOGS       a folder that keeps each start's guest log and stdout, <app>-<start>.log and .out, and a screenshot
#              <app>-<start>.png of a start that failed, which shows an alert's text
#   JAVA_TEST  set: java-test's round runs in the Simulator too, where its layout line shows the Simulator's screen
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
ON_DEVICE=no
if [[ "${1:-}" == --device ]]; then
  ON_DEVICE=yes
  shift
fi
if [[ "$ON_DEVICE" == yes ]]; then
  read -r DEVICE_ID _ _ _ < <(python3 tools/find-device.py "${1:-}") || true
  if [[ -z "${DEVICE_ID:-}" ]]; then
    echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
    exit 1
  fi
else
  read -r SIM_UDID _ < <(python3 tools/find-simulator.py "${1:-}") || true
  if [[ -z "${SIM_UDID:-}" ]]; then
    echo "No matching iPhone simulator found." >&2
    exit 1
  fi
fi

# The test apps, "<app> <package>", and where each one's last line appears.
APPS=("app-start-test io.github.kisame76.minato.test.appstart" "context-test io.github.kisame76.minato.test.context"
      "asset-ndk-test io.github.kisame76.minato.test.assetndk" "jni-lib-test io.github.kisame76.minato.test.jnilib")
JAVA_TEST=${JAVA_TEST:-}
[[ "$ON_DEVICE" == no ]] || JAVA_TEST=yes
[[ -z "$JAVA_TEST" ]] || APPS+=("java-test io.github.kisame76.minato.test.javatest")
last_line() {
  case "$1" in
    app-start-test) echo "guest-out ^app-start-test: MessageDigest " ;; # app_process_run.LAST
    *) echo "guest-log ^I/$1: done$" ;;                                 # java_app_run's "<app>: done"
  esac
}
for app in "${APPS[@]}"; do
  tools/build-java-test-app.sh "${app%% *}" >/dev/null
done

WORK=$(mktemp -d)
if [[ "$ON_DEVICE" == yes ]]; then
  if [[ -n "${APP:-}" ]]; then
    # Over the installed Minato: never with --remove-existing-content, which would empty its container.
    xcrun devicectl device install app --device "$DEVICE_ID" -q "$APP" >/dev/null
    # As tools/deploy.sh does after an install; only when the app does not have the certificate yet.
    tools/hand-over-certificate.sh "$DEVICE_ID" >/dev/null 2>&1 \
      || echo "note  the signing certificate was not handed over" >&2
  else
    INSTALL_ONLY=1 tools/deploy.sh "$DEVICE_ID" >/dev/null
  fi
  # On the iPhone a copy of the files the check uses, which the helpers below fetch and send.
  CONTAINER="$WORK/container"
else
  if [[ -n "${APP:-}" ]]; then
    xcrun simctl boot "$SIM_UDID" 2>/dev/null || true # already booted is fine
    xcrun simctl install "$SIM_UDID" "$APP"
  else
    INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
  fi
  CONTAINER=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
fi
SUPPORT_REL="Library/Application Support"
CHECK_REL="tmp/session-check"
SUPPORT="$CONTAINER/$SUPPORT_REL"
CHECK="$CONTAINER/$CHECK_REL"
EVENTS="$CHECK/events"
REQUEST="$SUPPORT/launch-request.json"
# The app that comes to the front for java-test's Home.
AWAY=com.apple.Preferences

devicectl_copy() {
  local direction=$1
  shift
  xcrun devicectl device copy "$direction" --device "$DEVICE_ID" --domain-type appDataContainer \
    --domain-identifier "$BUNDLE_ID" -q "$@" >/dev/null 2>&1
}
# Starts app $1, Minato when none, or brings it to the front when it runs.
launch() {
  if [[ "$ON_DEVICE" == yes ]]; then
    xcrun devicectl device process launch --device "$DEVICE_ID" -q "${1:-$BUNDLE_ID}" >/dev/null
  else
    xcrun simctl launch "$SIM_UDID" "${1:-$BUNDLE_ID}" >/dev/null
  fi
}
terminate() {
  if [[ "$ON_DEVICE" == yes ]]; then
    xcrun devicectl device info processes --device "$DEVICE_ID" -q -j "$WORK/processes.json" >/dev/null 2>&1 || return 0
    for pid in $(python3 -c 'import json,sys; print(" ".join(str(p["processIdentifier"]) for p in json.load(open(sys.argv[1]))["result"]["runningProcesses"] if p.get("executable", "").endswith("/Minato.app/Minato")))' "$WORK/processes.json"); do
      xcrun devicectl device process terminate --device "$DEVICE_ID" -q --kill --pid "$pid" >/dev/null 2>&1 || true
    done
  else
    xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  fi
}
# Every request of app/Sources/Session/SessionCheck.swift, and the events, as tools/session-check.sh lists them.
REQUESTS="way tap quit confirm orientation unanswered forget-hint crash start-failed tone fps no-guest guest-log extra-session turn-library turn-session audio-disconnect status-bar end events"
# On the iPhone: the requests as the script has them, events included, go into the app's folder. A request the script
# does not have goes as an empty file, which the app takes for none: devicectl cannot delete a file.
send_requests() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  rm -rf "$WORK/sent"
  mkdir "$WORK/sent"
  for name in $REQUESTS; do
    if [[ -f "$CHECK/$name" ]]; then cp "$CHECK/$name" "$WORK/sent/"; else : > "$WORK/sent/$name"; fi
  done
  devicectl_copy to --source "$WORK/sent" --destination "$CHECK_REL"
}
# On the iPhone: the folder as the app has it, without the requests it took or emptied. False when there is none.
fetch_requests() {
  [[ "$ON_DEVICE" == yes ]] || { [[ -d "$CHECK" ]]; return; }
  rm -rf "$WORK/fetched"
  mkdir "$WORK/fetched"
  devicectl_copy from --source "$CHECK_REL" --destination "$WORK/fetched" || return 1
  find "$WORK/fetched" -type f -empty ! -name events -delete
  rm -rf "$CHECK"
  mv "$WORK/fetched" "$CHECK"
}
fetch_events() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  if devicectl_copy from --source "$CHECK_REL/events" --destination "$WORK/events"; then mv "$WORK/events" "$EVENTS"; fi
}
# Whether the container has the file $1, a path in it; on the iPhone it is fetched.
fetch() {
  if [[ "$ON_DEVICE" == yes ]]; then
    mkdir -p "$(dirname "$CONTAINER/$1")"
    devicectl_copy from --source "$1" --destination "$CONTAINER/$1" || { rm -f "$CONTAINER/$1"; return 1; }
  else
    [[ -f "$CONTAINER/$1" ]]
  fi
}
# A screenshot into LOGS as $1.png, for a start that failed.
screenshot() {
  [[ -n "${LOGS:-}" ]] || return 0
  mkdir -p "$LOGS"
  if [[ "$ON_DEVICE" == yes ]]; then
    xcrun devicectl device capture screenshot --device "$DEVICE_ID" -q --destination "$LOGS/$1.png" >/dev/null 2>&1 || true
  else
    xcrun simctl io "$SIM_UDID" screenshot "$LOGS/$1.png" >/dev/null 2>&1 || true
  fi
}

# The test apps go through the library, as session-check.sh removes its own, then what is left of them. Fails while
# the index still names one.
remove_test_apps() {
  terminate
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  for app in "${APPS[@]}"; do echo "${app#* }"; done > "$CHECK/end"
  send_requests
  launch || true
  for _ in $(seq 1 50); do fetch_requests || break; sleep 0.2; done
  terminate
  rm -rf "$CHECK"
  if [[ "$ON_DEVICE" == no ]]; then
    rm -f "$REQUEST"
    for app in "${APPS[@]}"; do
      rm -rf "${SUPPORT:?}/apps/${app#* }" "${SUPPORT:?}/data/${app#* }" "${CONTAINER:?}/Library/Caches/guest/${app#* }"
    done
  fi
  fetch "$SUPPORT_REL/library.json" || true
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
had_data=no
if [[ "$ON_DEVICE" == no ]]; then
  cp "$SUPPORT/library.json" "$WORK/library.json.saved" 2>/dev/null || true
  [[ -d "$SUPPORT/data" ]] && had_data=yes
fi

restore() {
  remove_test_apps || true
  if [[ "$ON_DEVICE" == no ]]; then
    if [[ "$had_data" == no ]]; then rmdir "$SUPPORT/data" 2>/dev/null || true; fi
    if [[ -f "$WORK/library.json.saved" ]]; then cp "$WORK/library.json.saved" "$SUPPORT/library.json"; else rm -f "$SUPPORT/library.json"; fi
  fi
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
    fetch_events
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
# Field $1 of app $2's entry.
field() {
  fetch "$SUPPORT_REL/library.json" || true
  python3 -c 'import json,sys; e={e["id"]:e for e in json.load(open(sys.argv[1]))["entries"]}.get(sys.argv[2]); print(None if e is None else e.get(sys.argv[3]))' \
    "$SUPPORT/library.json" "$2" "$1" 2>/dev/null || echo None
}
now() { python3 -c 'import time; print(time.time())'; }
seconds() { python3 -c 'import sys; print(f"{float(sys.argv[2]) - float(sys.argv[1]):.1f}")' "$1" "$2"; }

# Waits up to $3 seconds for a line of the start's file $1, guest-log or guest-out, that matches the extended regex
# $2, fetched on the iPhone, and sets SEEN to the time it was first seen. Fails at once when the start failed or the
# program ended.
SEEN=""
await_line() {
  local file=$1 pattern=$2 end=$((SECONDS + $3))
  while (( SECONDS < end )); do
    fetch "$CHECK_REL/$file" || true
    if grep -q -E "$pattern" "$CHECK/$file" 2>/dev/null; then
      SEEN=$(now)
      return 0
    fi
    fetch_events
    if tail -n +$((seen + 1)) "$EVENTS" | grep -q -E '^[0-9.]+ [0-9]+ (could not start|program ended) '; then return 1; fi
    sleep 0.2
  done
  return 1
}

# The tap on package $2, the $3rd start of app $1: up to game mode with the guest log on, whose files the start then
# has; TAP is the tap's time, on the app's clock. False when it did not get there.
TAP=""
begin_start() {
  local app=$1 package=$2 round=$3
  echo "== $app, start $round"
  terminate
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  [[ "$ON_DEVICE" == yes ]] || rm -f "$REQUEST"
  : > "$WORK/$app-$round.log"
  : > "$WORK/$app-$round.out"
  : > "$EVENTS"
  seen=0
  echo "$package" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  send_requests
  launch
  if ! await_event "decision .*" 60; then
    fail "  no tap in the library"
    screenshot "$app-$round"
    return 1
  fi
  TAP=${EVENT%% *}
  # The game mode process truncates the guest log's files when it takes the request.
  if ! await_event "process gameMode $package.*" 60 || ! await_event "guest log on" 60; then
    fail "  no game mode after the tap"
    sed 's/^/      | /' "$EVENTS"
    screenshot "$app-$round"
    return 1
  fi
}
# The start's files, kept as $WORK/$1-$2.log and .out, after Minato's end.
end_start() {
  terminate
  for file in guest-log guest-out; do fetch "$CHECK_REL/$file" || true; done
  cp "$CHECK/guest-log" "$WORK/$1-$2.log" 2>/dev/null || : > "$WORK/$1-$2.log"
  cp "$CHECK/guest-out" "$WORK/$1-$2.out" 2>/dev/null || : > "$WORK/$1-$2.out"
}
failed_line() {
  fail "  $1"
  screenshot "$2"
  sed 's/^/      | /' "$EVENTS"
  tail -n 40 "$CHECK/guest-log" 2>/dev/null | sed 's/^/      | /' || true
}

# One start of app $1 with package $2, the $3rd: the tap, game mode, the app's lines up to its last one, and Minato's
# end 2 s later.
run_start() {
  local app=$1 package=$2 round=$3 file pattern
  read -r file pattern <<< "$(last_line "$app")"
  if begin_start "$app" "$package" "$round"; then
    if await_line "$file" "$pattern" 300; then
      ok "  its last line, $(seconds "$TAP" "$SEEN") s after the tap"
      sleep 2
    else
      failed_line "no last line" "$app-$round"
    fi
  fi
  end_start "$app" "$round"
}

# java-test's $1st start, as on the AVD (ADR-0043 step c1): the tap, its window's focus, Home, the return and its own
# finish. The time to its first onResume comes from the app's own "onResume at" line when it has one.
run_lifecycle() {
  local round=$1 package=${APPS[${#APPS[@]} - 1]#* } resumed at
  if begin_start java-test "$package" "$round"; then
    if ! await_line guest-log "^I/java-test: onResume$" 300; then
      failed_line "no onResume" "java-test-$round"
    else
      resumed=$SEEN
      # The app's clock, which is the tap's too, in the line it logs right after.
      at=""
      await_line guest-log "^I/java-test-device: onResume at " 10 \
        && at=$(sed -n -E 's/^I\/java-test-device: onResume at ([0-9]+)$/\1/p' "$CHECK/guest-log" | head -n 1)
      if [[ -n "$at" ]]; then
        ok "  onResume $(python3 -c 'import sys; print(f"{float(sys.argv[2]) / 1000 - float(sys.argv[1]):.2f}")' "$TAP" "$at") s after the tap, by the app's clock"
      else
        ok "  onResume seen $(seconds "$TAP" "$resumed") s after the tap"
      fi
      if ! await_line guest-log "^I/java-test: focus true$" 60; then
        failed_line "no focus" "java-test-$round"
      else
        launch "$AWAY"
        if ! await_line guest-log "^I/java-test: onSaveInstanceState$" 60; then
          failed_line "no onStop after Home" "java-test-$round"
        else
          launch
          if await_line guest-log "^I/java-test: done$" 60; then
            ok "  done, after Home and the return, seen $(seconds "$TAP" "$SEEN") s after the tap"
            sleep 2
          else
            failed_line "no done after the return" "java-test-$round"
          fi
        fi
      fi
    fi
  fi
  end_start java-test "$round"
}

# Import the apps; the Simulator signs a ready app's code ad hoc at its first tap.
terminate
rm -rf "$CHECK"
mkdir -p "$CHECK"
[[ "$ON_DEVICE" == yes ]] || rm -f "$REQUEST"
: > "$EVENTS"
for app in "${APPS[@]}"; do
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy to --source "build/${app%% *}/${app%% *}.apk" --destination "$SUPPORT_REL/incoming/${app%% *}.apk"
  else
    cp "build/${app%% *}/${app%% *}.apk" "$SUPPORT/incoming/"
  fi
done
send_requests
launch
for app in "${APPS[@]}"; do
  package=${app#* }
  end=$((SECONDS + 60))
  while (( SECONDS < end )); do
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

if [[ -n "$JAVA_TEST" ]]; then
  run_lifecycle 1
  run_lifecycle 2
  python3 - "$WORK/java-test-1.log" "$WORK/java-test-2.log" fixtures/expected/java-test.logcat <<'EOF' \
    || failures=$((failures + 1))
import subprocess
import sys
sys.path.insert(0, "framework/tests")
from java_app_run import compare_log

first, second, expected = sys.argv[1:]
log = "".join(open(path, encoding="utf-8", errors="replace").read() for path in (first, second))
compare_log("java-test", subprocess.CompletedProcess([], 0, "", log), expected, {})
EOF
fi

echo "java start check: $failures failed"
exit $((failures > 0))
