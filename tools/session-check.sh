#!/usr/bin/env bash
# Checks game mode in the Simulator (ADR-0032, test 6). For each way of decision 2, our own URL, the
# workspace and none (the fallback), one tap in the library starts a NativeActivity test app in game mode
# in a new process, and Quit in its menu returns to the library in another; it prints the time from the
# tap and from the restart to the new process. A restart ends the old process itself, before the new one
# starts. Game mode's window: its layer covers the screen in pixels, in portrait and, held for the check,
# in landscape; the guest starts with that final size; the status bar and the home indicator are hidden,
# every edge defers the system's gestures, and the hint shows at the first start only. The test app's
# lifecycle callbacks follow decision 7. The fallback's alert comes up in both modes, also right after the
# menu, and game mode restarts once, also after Home and back; a restart whose request goes unanswered
# ends in the fallback after 3 s. The Debug build answers through app/Sources/Session/SessionCheck.swift.
# The library is restored afterwards, and the hint counts as not shown yet.
# Usage: tools/session-check.sh [simulator name or UDID]
# Optional environment:
#   SHOTS       a folder for screenshots of game mode, its menu and the fallback's alerts
#   SKIP_BUILD  when set, installs the app built last instead of building it first
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
PKG="io.github.kisame76.minato.check.session"
read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true
if [[ -z "${SIM_UDID:-}" ]]; then
  echo "No matching iPhone simulator found." >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# The test app: a NativeActivity APK around a fixture library (ADR-0031). Game mode does not run its
# code yet (ADR-0032 step c), so any ready NativeActivity app does.
cat > "$WORK/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$PKG" android:versionCode="1" android:versionName="1.0">
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
  <application android:label="Session Check" android:hasCode="false">
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <meta-data android:name="android.app.lib_name" android:value="raw-dyn"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
EOF
[[ -f build/fixtures/bin/libraw-dyn.so ]] || tools/build-fixtures.sh >/dev/null
tools/make-apk.sh --manifest "$WORK/AndroidManifest.xml" --lib build/fixtures/bin/libraw-dyn.so \
  --out "$WORK/session-check.apk" >/dev/null

if [[ -n "${SKIP_BUILD:-}" ]]; then
  xcrun simctl install "$SIM_UDID" build/DerivedData/Build/Products/Debug-iphonesimulator/Minato.app
else
  INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
fi
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
DATA=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
SUPPORT="$DATA/Library/Application Support"
CHECK="$DATA/tmp/session-check"
EVENTS="$CHECK/events"
REQUEST="$SUPPORT/launch-request.json"
mkdir -p "$SUPPORT/incoming"
cp "$SUPPORT/library.json" "$WORK/library.json.saved" 2>/dev/null || true
had_data=no
[[ -d "$SUPPORT/data" ]] && had_data=yes

restore() {
  xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  # The hint counts as not shown yet, as before the first session.
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  touch "$CHECK/forget-hint"
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  for _ in $(seq 1 50); do [[ -f "$CHECK/forget-hint" ]] || break; sleep 0.2; done
  xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  rm -rf "$CHECK" "$SUPPORT/apps/$PKG" "$SUPPORT/data/$PKG" "$REQUEST"
  if [[ "$had_data" == no ]]; then rmdir "$SUPPORT/data" 2>/dev/null || true; fi
  if [[ -f "$WORK/library.json.saved" ]]; then cp "$WORK/library.json.saved" "$SUPPORT/library.json"; else rm -f "$SUPPORT/library.json"; fi
  rm -rf "$WORK"
}
trap restore EXIT

failures=0
ok() { echo "ok    $1"; }
fail() {
  echo "FAIL  $1"
  failures=$((failures + 1))
}
# A round that cannot go on: the failure, then the round's events so far.
abort_round() {
  fail "$1"
  sed 's/^/      | /' "$EVENTS" 2>/dev/null || true
}
expect() {
  if [[ "$3" == "$2" ]]; then ok "$1"; else fail "$1: got \"$3\", wanted \"$2\""; fi
}

# The events the app appended, "<seconds> <pid> <event>"; `seen` counts the lines already taken.
seen=0
EVENT=""
# Waits up to $2 seconds (default 20) for the next event that matches the extended regex $1 and puts it
# into EVENT; events before it are passed over.
await_event() {
  local pattern=$1 limit=${2:-20} found
  for _ in $(seq 1 $((limit * 5))); do
    found=$(tail -n +$((seen + 1)) "$EVENTS" 2>/dev/null | grep -n -E -m 1 "^[0-9.]+ [0-9]+ ($pattern)" || true)
    if [[ -n "$found" ]]; then
      seen=$((seen + ${found%%:*}))
      EVENT=${found#*:}
      return 0
    fi
    sleep 0.2
  done
  EVENT=""
  return 1
}
time_of() { echo "${1%% *}"; }
pid_of() { local rest=${1#* }; echo "${rest%% *}"; }
event_of() { local rest=${1#* }; echo "${rest#* }"; }
seconds() { python3 -c 'import sys; print(f"{float(sys.argv[2]) - float(sys.argv[1]):.2f}")' "$1" "$2"; }
# The callbacks of process $1 among the events taken so far, on one line.
callbacks_of() {
  head -n "$seen" "$EVENTS" | awk -v pid="$1" '$2 == pid && $3 == "callback" { printf "%s%s", sep, $4; sep = " " }'
}
shot() {
  [[ -n "${SHOTS:-}" ]] || return 0
  mkdir -p "$SHOTS"
  sleep "${2:-0.8}"
  xcrun simctl io "$SIM_UDID" screenshot --type=png "$SHOTS/$1.png" >/dev/null 2>&1 || true
}
field() {
  python3 -c 'import json,sys; e={e["id"]:e for e in json.load(open(sys.argv[1]))["entries"]}.get(sys.argv[2]); print(None if e is None else e.get(sys.argv[3]))' \
    "$SUPPORT/library.json" "$PKG" "$1" 2>/dev/null || echo None
}

# Checks game mode's window from its "window ..." event $1: $2 is the orientation it holds, portrait or
# landscape, and $3 the size in pixels the guest started with.
check_window() {
  local line=$1 orientation=$2 start=$3 pattern='window layer ([0-9]+)x([0-9]+) covers ([a-z]+) screen ([0-9]+)x([0-9]+) scale ([0-9.]+)/([0-9.]+) orientation ([0-9]+) statusBarHidden ([a-z]+) homeIndicatorHidden ([a-z]+) deferredEdges ([0-9]+)'
  if [[ ! "$line" =~ $pattern ]]; then
    fail "window: \"$line\""
    return
  fi
  local w=${BASH_REMATCH[1]} h=${BASH_REMATCH[2]} sw=${BASH_REMATCH[4]} sh=${BASH_REMATCH[5]} turned=${BASH_REMATCH[8]}
  local sorted_layer sorted_screen
  sorted_layer=$(printf '%s\n%s\n' "$w" "$h" | sort -n | tr '\n' ' ')
  sorted_screen=$(printf '%s\n%s\n' "$sw" "$sh" | sort -n | tr '\n' ' ')
  expect "  the layer has the screen's pixels (${w}x${h})" "$sorted_screen" "$sorted_layer"
  expect "  the layer covers the window" "true" "${BASH_REMATCH[3]}"
  expect "  the layer's scale is the screen's native scale" "${BASH_REMATCH[7]}" "${BASH_REMATCH[6]}"
  expect "  the status bar is hidden" "true" "${BASH_REMATCH[9]}"
  expect "  the home indicator is hidden" "true" "${BASH_REMATCH[10]}"
  expect "  every edge defers the system's gestures" "15" "${BASH_REMATCH[11]}"
  # UIInterfaceOrientation: 1 portrait, 3 and 4 the landscape sides.
  if [[ "$orientation" == landscape ]]; then
    [[ "$turned" == 3 || "$turned" == 4 ]] && (( w > h )) && ok "  landscape, as held" \
      || fail "  landscape, as held: orientation $turned, ${w}x${h}"
  else
    expect "  portrait, the orientation at start (no screenOrientation)" "1" "$turned"
  fi
  expect "  the guest starts with the window's final size" "${w}x${h}" "$start"
}

START="onCreate onStart onResume onNativeWindowCreated onWindowFocusChanged(true)"
MENU="onWindowFocusChanged(false)"
QUIT="onPause onNativeWindowDestroyed onStop onDestroy"

# Game mode from its start to Quit, in process $2 for way $1; $3 is "shown" when the hint is due, $4 the
# orientation it holds.
check_game_mode() {
  local way=$1 pid=$2 hint=$3 orientation=$4 start=none
  if await_event "hint (not )?shown"; then
    expect "  the hint $hint" "hint $hint" "$(event_of "$EVENT")"
  else
    fail "  no hint event"
  fi
  if await_event "start window [0-9]+x[0-9]+"; then
    start=${EVENT##* }
  else
    fail "  no start event"
  fi
  if await_event "window .*"; then
    check_window "$(event_of "$EVENT")" "$orientation" "$start"
    shot "$way-1-game-mode" 0.2
  else
    fail "  no window event"
  fi
  if await_event "menu opened"; then
    ok "  the menu opens"
    shot "$way-2-menu"
  else
    fail "  the menu did not open"
  fi
  if await_event "quit"; then
    QUIT_TIME=$(time_of "$EVENT")
    ok "  Quit"
  else
    fail "  no Quit"
  fi
  await_event "restart $way" || fail "  game mode did not restart through $way"
  expect "  the callbacks in game mode" "$START $MENU $QUIT" "$(callbacks_of "$pid")"
}

# Ends the library the last round restarted into before the new round's requests are written: while it
# still starts up, it would take the tap meant for the new round.
begin_round() {
  echo "== $1"
  xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  : > "$EVENTS"
  seen=0
}

# Waits for the event "exit" and checks that process $1 wrote it: a restart ends the old process itself.
# A crash would pass without it, since the restart's second request starts Minato anyway.
check_exit() {
  if await_event "exit"; then
    expect "  $2 ends itself for the restart" "$1" "$(pid_of "$EVENT")"
  else
    fail "  $2 did not end itself for the restart"
  fi
}

# One round for way $1: tap, game mode, Quit, library. $2 is "shown" when the hint is due, $3 the
# orientation game mode holds: portrait, the test app's orientation at start, or landscape by request.
run_way() {
  local way=$1 hint=$2 orientation=$3 library_pid game_pid tap_time restart_time
  begin_round "$way, $orientation"
  echo "$way" > "$CHECK/way"
  echo "$PKG" > "$CHECK/tap"
  touch "$CHECK/quit"
  rm -f "$CHECK/confirm" "$CHECK/orientation"
  if [[ "$orientation" == landscape ]]; then echo landscape > "$CHECK/orientation"; fi
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  await_event "process library" || { abort_round "the library did not start"; return; }
  library_pid=$(pid_of "$EVENT")
  await_event "tap $PKG" || { abort_round "the library did not tap $PKG"; return; }
  tap_time=$(time_of "$EVENT")
  if await_event "decision .*" 60; then
    ok "decision: $(event_of "$EVENT")"
  fi
  await_event "restart $way" 60 || { abort_round "the library did not restart through $way"; return; }
  restart_time=$(time_of "$EVENT")
  check_exit "$library_pid" "the library"
  if ! await_event "process gameMode $PKG .*"; then
    abort_round "no game mode after the restart through $way"
    return
  fi
  game_pid=$(pid_of "$EVENT")
  [[ "$game_pid" != "$library_pid" ]] && ok "game mode is a new process" || fail "game mode runs in the library's process"
  echo "      tap to game mode: $(seconds "$tap_time" "$(time_of "$EVENT")") s, restart to game mode: $(seconds "$restart_time" "$(time_of "$EVENT")") s"
  # Game mode reads the request's time, written just before the restart: it says the same to 0.1 s.
  local logged=${EVENT##* }
  python3 -c 'import sys; sys.exit(abs(float(sys.argv[1]) - (float(sys.argv[3]) - float(sys.argv[2]))) > 0.1)' \
    "$logged" "$restart_time" "$(time_of "$EVENT")" \
    && ok "game mode's time since the request ($logged s)" || fail "game mode's time since the request: $logged s"
  check_game_mode "$way" "$game_pid" "$hint" "$orientation"
  check_exit "$game_pid" "game mode"
  if await_event "process library"; then
    [[ "$(pid_of "$EVENT")" != "$game_pid" ]] && ok "the library is a new process" || fail "the library runs in game mode's process"
    echo "      Quit to library: $(seconds "$QUIT_TIME" "$(time_of "$EVENT")") s"
  else
    fail "no library after Quit through $way"
  fi
  [[ -f "$REQUEST" ]] && fail "a launch request is left" || ok "no launch request left"
  rm -f "$CHECK/orientation"
}

# The alert of the fallback: $1 is its title.
check_alert() {
  if await_event "fallback (not )?shown.*"; then
    expect "  the alert shows" "fallback shown $1" "$(event_of "$EVENT")"
  else
    fail "  no alert event"
  fi
}

# The fallback: neither way restarts, the alert asks the user to open Minato again, and its OK ends the
# process; opening Minato then (here: simctl launch) goes on.
run_fallback() {
  local library_pid game_pid restarts
  begin_round "none (the fallback)"
  echo none > "$CHECK/way"
  echo "$PKG" > "$CHECK/tap"
  echo 1 > "$CHECK/confirm"
  touch "$CHECK/quit"
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  await_event "process library" || { abort_round "the library did not start"; return; }
  library_pid=$(pid_of "$EVENT")
  await_event "restartFailed none" 60 || { abort_round "the library's restart did not fail"; return; }
  ok "the tap's restart fails"
  check_alert "Open Minato again to start “Session Check”."
  shot "none-1-alert-start" 0.2
  await_event "fallback confirmed" || fail "the alert was not confirmed"
  sleep 1
  [[ -f "$REQUEST" ]] && ok "the launch request stays" || fail "the launch request is gone"
  # Long enough for Home and back below.
  echo 8 > "$CHECK/confirm"
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  if ! await_event "process gameMode $PKG .*"; then
    abort_round "opening Minato again did not start game mode"
    return
  fi
  game_pid=$(pid_of "$EVENT")
  [[ "$game_pid" != "$library_pid" ]] && ok "opening Minato again starts game mode" || fail "game mode runs in the library's process"
  check_game_mode none "$game_pid" "not shown" portrait
  await_event "restartFailed none" || fail "game mode's restart did not fail"
  # Right after the menu closed.
  check_alert "Open Minato again to return to the library."
  shot "none-3-alert-library" 0.2
  # Home and back while the alert waits: every change of the scene reaches the ended session.
  xcrun simctl launch "$SIM_UDID" com.apple.Preferences >/dev/null
  sleep 1.5
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  await_event "fallback confirmed" || fail "the alert was not confirmed"
  restarts=$(head -n "$seen" "$EVENTS" | awk -v pid="$game_pid" '$2 == pid && $3 == "restart"' | wc -l | tr -d ' ')
  expect "  game mode restarts once, also after Home and back" "1" "$restarts"
  sleep 1
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  await_event "process library" && ok "opening Minato again shows the library" || fail "no library after the fallback"
  [[ -f "$REQUEST" ]] && fail "a launch request is left" || ok "no launch request left"
}

# A restart whose first request goes unanswered: after 3 s the alert asks the user to open Minato again,
# as when neither way works, and the request stays.
run_timeout() {
  local library_pid game_pid restart_time waited
  begin_round "url, unanswered (the timeout)"
  echo url > "$CHECK/way"
  echo "$PKG" > "$CHECK/tap"
  echo 1 > "$CHECK/confirm"
  touch "$CHECK/quit" "$CHECK/unanswered"
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  await_event "process library" || { abort_round "the library did not start"; return; }
  library_pid=$(pid_of "$EVENT")
  await_event "restart url" 60 || { abort_round "the library did not restart through url"; return; }
  restart_time=$(time_of "$EVENT")
  await_event "answer dropped" || fail "the restart's answer was not dropped"
  if ! await_event "restartFailed url" 10; then
    abort_round "the unanswered restart did not end in the fallback"
    return
  fi
  waited=$(seconds "$restart_time" "$(time_of "$EVENT")")
  python3 -c 'import sys; sys.exit(not 2.9 <= float(sys.argv[1]) <= 4.0)' "$waited" \
    && ok "the restart gives up after $waited s" || fail "the restart gave up after $waited s, wanted 3 s"
  check_alert "Open Minato again to start “Session Check”."
  await_event "fallback confirmed" || fail "the alert was not confirmed"
  sleep 1
  [[ -f "$REQUEST" ]] && ok "the launch request stays" || fail "the launch request is gone"
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
  if ! await_event "process gameMode $PKG .*"; then
    abort_round "opening Minato again did not start game mode"
    return
  fi
  game_pid=$(pid_of "$EVENT")
  [[ "$game_pid" != "$library_pid" ]] && ok "opening Minato again starts game mode" || fail "game mode runs in the library's process"
  check_game_mode url "$game_pid" "not shown" portrait
  check_exit "$game_pid" "game mode"
  await_event "process library" && ok "Quit returns to the library" || fail "no library after Quit"
  [[ -f "$REQUEST" ]] && fail "a launch request is left" || ok "no launch request left"
}

# Import the test app; the first session shows the hint.
rm -rf "$CHECK" "$REQUEST"
mkdir -p "$CHECK"
touch "$CHECK/forget-hint"
: > "$EVENTS"
cp "$WORK/session-check.apk" "$SUPPORT/incoming/"
xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID" >/dev/null
for _ in $(seq 1 100); do
  [[ "$(field state)" == "{'ready': {}}" ]] && break
  sleep 0.2
done
expect "the test app is ready" "{'ready': {}}" "$(field state)"
expect "not signed yet" "None" "$(field signedWith)"

run_way url shown portrait
expect "signed ad hoc at the first tap" "ad hoc" "$(field signedWith)"
folder=$(field folder)
[[ -f "$SUPPORT/apps/$PKG/$folder/lib/libraw-dyn.so.dylib" ]] && ok "its container is in lib/" || fail "no container in lib/"
run_way workspace "not shown" landscape
run_fallback
run_timeout

echo "session check: $failures failed"
exit $((failures > 0))
