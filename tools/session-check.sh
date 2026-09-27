#!/usr/bin/env bash
# Checks game mode in the Simulator or on the iPhone (ADR-0032, test 6). For each way of decision 2, our own URL, the
# workspace and none (the fallback), one tap in the library starts a NativeActivity test app in game mode
# in a new process, and Quit in its menu returns to the library in another; it prints the time from the
# tap and from the restart to the new process. A restart ends the old process itself, before the new one
# starts. Game mode's window: its layer covers the screen in pixels, in portrait and, held for the check,
# in landscape; the guest starts with that final size; the status bar and the home indicator are hidden,
# every edge defers the system's gestures, and the hint shows at the first start only. The test app's
# lifecycle callbacks follow decision 7. The fallback's alert comes up in both modes, also right after the
# menu, and game mode restarts once, also after Home and back; a restart whose request goes unanswered
# ends in the fallback after 3 s. The Debug build answers through app/Sources/Session/SessionCheck.swift.
# The test app is removed afterwards, and the hint counts as not shown yet.
# On the iPhone (--device), devicectl sends the requests and fetches the events, and the app must stay
# unlocked. BY_HAND=1 runs one round there in which the user taps the test app, swipes down from the top
# edge and taps Quit; the script waits for each step. TOUCH=1 runs one round in which the touches come by hand
# (ADR-0036, decision 3): a tap, two fingers down and up, then the swipe down from the top edge and Quit; it
# checks what reaches the motion sink and prints how far each new touch's time lies before now on the
# uptime clock and on CLOCK_MONOTONIC_RAW. FRAMES=1 runs one round in which game mode stays 5 s before its
# menu and Quit (ADR-0036, decision 2): the frame clock runs while the gate is open, off the main thread, and
# pauses with the menu; it prints the intervals between ticks. ENDS=1 runs the rounds of decision 8: a crash and
# an app that cannot start show once at the library's next start, and a Quit shows nothing.
# Usage: tools/session-check.sh [simulator name or UDID]
#        tools/session-check.sh --device [device name or identifier]
# Optional environment:
#   SHOTS       a folder for screenshots of game mode, its menu and the fallback's alerts
#   SKIP_BUILD  when set, installs the app built last instead of building it first
#   BY_HAND     with --device: only the round by hand
#   TOUCH       only the round of touches by hand
#   FRAMES      only the round of the frame clock
#   ENDS        only the rounds of ends that go wrong
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
PKG="io.github.kisame76.minato.check.session"
ON_DEVICE=no
if [[ "${1:-}" == "--device" ]]; then
  ON_DEVICE=yes
  shift
  read -r DEVICE_ID _ < <(python3 tools/find-device.py "${1:-}") || true
  if [[ -z "${DEVICE_ID:-}" ]]; then
    echo "No paired iPhone found. Connect it by cable and trust this Mac." >&2
    exit 1
  fi
else
  read -r SIM_UDID SIM_NAME < <(python3 tools/find-simulator.py "${1:-}") || true
  if [[ -z "${SIM_UDID:-}" ]]; then
    echo "No matching iPhone simulator found." >&2
    exit 1
  fi
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

# The app's container: in the Simulator the folder itself, on the iPhone a copy of the files the check
# uses, which the helpers below fetch and send.
SUPPORT_REL="Library/Application Support"
CHECK_REL="tmp/session-check"
REQUEST_REL="$SUPPORT_REL/launch-request.json"
if [[ "$ON_DEVICE" == yes ]]; then
  if [[ -n "${SKIP_BUILD:-}" ]]; then
    xcrun devicectl device install app --device "$DEVICE_ID" -q build/DerivedData/Build/Products/Debug-iphoneos/Minato.app >/dev/null
  else
    INSTALL_ONLY=1 tools/deploy.sh "$DEVICE_ID" >/dev/null
  fi
  CONTAINER="$WORK/container"
else
  if [[ -n "${SKIP_BUILD:-}" ]]; then
    xcrun simctl install "$SIM_UDID" build/DerivedData/Build/Products/Debug-iphonesimulator/Minato.app
  else
    INSTALL_ONLY=1 tools/sim.sh --headless "$SIM_UDID" >/dev/null
  fi
  CONTAINER=$(xcrun simctl get_app_container "$SIM_UDID" "$BUNDLE_ID" data)
fi
SUPPORT="$CONTAINER/$SUPPORT_REL"
CHECK="$CONTAINER/$CHECK_REL"
EVENTS="$CHECK/events"
REQUEST="$CONTAINER/$REQUEST_REL"

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
# Every request of app/Sources/Session/SessionCheck.swift, and the events.
REQUESTS="way tap quit confirm orientation unanswered forget-hint crash start-failed end events"
# On the iPhone: the requests as the script has them, events included, go into the app's folder. A request
# the script does not have goes as an empty file, which the app takes for none: devicectl cannot delete a
# file, and its --remove-existing-content empties the app's whole container, not just the folder.
send_requests() {
  [[ "$ON_DEVICE" == yes ]] || return 0
  rm -rf "$WORK/sent"
  mkdir "$WORK/sent"
  for name in $REQUESTS; do
    if [[ -f "$CHECK/$name" ]]; then cp "$CHECK/$name" "$WORK/sent/"; else : > "$WORK/sent/$name"; fi
  done
  devicectl_copy to --source "$WORK/sent" --destination "$CHECK_REL"
}
# On the iPhone: the folder as the app has it, without the requests it took or emptied. False when there
# is none.
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
has() {
  if [[ "$ON_DEVICE" == yes ]]; then
    mkdir -p "$(dirname "$CONTAINER/$1")"
    devicectl_copy from --source "$1" --destination "$CONTAINER/$1" || { rm -f "$CONTAINER/$1"; return 1; }
  else
    [[ -f "$CONTAINER/$1" ]]
  fi
}

mkdir -p "$SUPPORT/incoming"
if [[ "$ON_DEVICE" == no ]]; then
  cp "$SUPPORT/library.json" "$WORK/library.json.saved" 2>/dev/null || true
  had_data=no
  [[ -d "$SUPPORT/data" ]] && had_data=yes
fi

restore() {
  terminate
  # The hint counts as not shown yet, as before the first session, and the test app goes.
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  echo yes > "$CHECK/forget-hint"
  echo "$PKG" > "$CHECK/end"
  send_requests || true
  launch || true
  for _ in $(seq 1 50); do fetch_requests || break; sleep 0.2; done
  terminate
  if [[ "$ON_DEVICE" == no ]]; then
    rm -rf "$CHECK" "$SUPPORT/apps/$PKG" "$SUPPORT/data/$PKG" "$REQUEST"
    if [[ "$had_data" == no ]]; then rmdir "$SUPPORT/data" 2>/dev/null || true; fi
    if [[ -f "$WORK/library.json.saved" ]]; then cp "$WORK/library.json.saved" "$SUPPORT/library.json"; else rm -f "$SUPPORT/library.json"; fi
  fi
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
# Seconds to wait for the menu and for Quit: longer when the user opens it.
PATIENCE=20
# Waits up to $2 seconds (default 20) for the next event that matches the extended regex $1 and puts it
# into EVENT; events before it are passed over.
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
  if [[ "$ON_DEVICE" == yes ]]; then
    xcrun devicectl device capture screenshot --device "$DEVICE_ID" -q --destination "$SHOTS/$1.png" >/dev/null 2>&1 || true
  else
    xcrun simctl io "$SIM_UDID" screenshot --type=png "$SHOTS/$1.png" >/dev/null 2>&1 || true
  fi
}
field() {
  has "$SUPPORT_REL/library.json" || true
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
  elif [[ "$ON_DEVICE" == yes ]]; then
    # The iPhone's orientation at start is the one it has, which lying flat stays the last app's.
    { [[ "$turned" == 1 ]] && (( h > w )) || { [[ "$turned" == 3 || "$turned" == 4 ]] && (( w > h )); }; } \
      && ok "  the orientation at start (no screenOrientation): $turned" \
      || fail "  the orientation at start (no screenOrientation): orientation $turned, ${w}x${h}"
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
  if await_event "menu opened" "$PATIENCE"; then
    ok "  the menu opens"
    shot "$way-2-menu"
  else
    fail "  the menu did not open"
  fi
  if await_event "quit" "$PATIENCE"; then
    QUIT_TIME=$(time_of "$EVENT")
    ok "  Quit"
  else
    fail "  no Quit"
  fi
  await_event "restart $way" || fail "  game mode did not restart through $way"
  local callbacks
  callbacks=$(callbacks_of "$pid")
  if (( PATIENCE > 20 )); then
    # By hand, iOS may take the focus or the foreground in between, each with its own callbacks.
    [[ "$callbacks" == "$START "* && "$callbacks" == *" $MENU $QUIT" ]] \
      && ok "  the callbacks in game mode start and end as decision 7 says" \
      || fail "  the callbacks in game mode: \"$callbacks\""
    echo "      $callbacks"
  else
    expect "  the callbacks in game mode" "$START $MENU $QUIT" "$callbacks"
  fi
}

# Ends the library the last round restarted into before the new round's requests are written: while it
# still starts up, it would take the tap meant for the new round.
begin_round() {
  echo "== $1"
  terminate
  fetch_requests || mkdir -p "$CHECK"
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
  echo yes > "$CHECK/quit"
  rm -f "$CHECK/confirm" "$CHECK/orientation"
  if [[ "$orientation" == landscape ]]; then echo landscape > "$CHECK/orientation"; fi
  send_requests
  launch
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
  has "$REQUEST_REL" && fail "a launch request is left" || ok "no launch request left"
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
# process; opening Minato then (here: launch) goes on.
run_fallback() {
  local library_pid game_pid restarts
  begin_round "none (the fallback)"
  echo none > "$CHECK/way"
  echo "$PKG" > "$CHECK/tap"
  echo 1 > "$CHECK/confirm"
  echo yes > "$CHECK/quit"
  send_requests
  launch
  await_event "process library" || { abort_round "the library did not start"; return; }
  library_pid=$(pid_of "$EVENT")
  await_event "restartFailed none" 60 || { abort_round "the library's restart did not fail"; return; }
  ok "the tap's restart fails"
  check_alert "Open Minato again to start “Session Check”."
  shot "none-1-alert-start" 0.2
  await_event "fallback confirmed" || fail "the alert was not confirmed"
  sleep 1
  has "$REQUEST_REL" && ok "the launch request stays" || fail "the launch request is gone"
  # Long enough for Home and back below; the process has ended, so the requests can be replaced.
  fetch_requests
  echo 8 > "$CHECK/confirm"
  send_requests
  launch
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
  launch com.apple.Preferences
  sleep 1.5
  launch
  await_event "fallback confirmed" || fail "the alert was not confirmed"
  restarts=$(head -n "$seen" "$EVENTS" | awk -v pid="$game_pid" '$2 == pid && $3 == "restart"' | wc -l | tr -d ' ')
  expect "  game mode restarts once, also after Home and back" "1" "$restarts"
  sleep 1
  launch
  await_event "process library" && ok "opening Minato again shows the library" || fail "no library after the fallback"
  has "$REQUEST_REL" && fail "a launch request is left" || ok "no launch request left"
}

# A restart whose first request goes unanswered: after 3 s the alert asks the user to open Minato again,
# as when neither way works, and the request stays.
run_timeout() {
  local library_pid game_pid restart_time waited
  begin_round "url, unanswered (the timeout)"
  echo url > "$CHECK/way"
  echo "$PKG" > "$CHECK/tap"
  echo 1 > "$CHECK/confirm"
  echo yes > "$CHECK/quit"
  echo yes > "$CHECK/unanswered"
  send_requests
  launch
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
  has "$REQUEST_REL" && ok "the launch request stays" || fail "the launch request is gone"
  launch
  if ! await_event "process gameMode $PKG .*"; then
    abort_round "opening Minato again did not start game mode"
    return
  fi
  game_pid=$(pid_of "$EVENT")
  [[ "$game_pid" != "$library_pid" ]] && ok "opening Minato again starts game mode" || fail "game mode runs in the library's process"
  check_game_mode url "$game_pid" "not shown" portrait
  check_exit "$game_pid" "game mode"
  await_event "process library" && ok "Quit returns to the library" || fail "no library after Quit"
  has "$REQUEST_REL" && fail "a launch request is left" || ok "no launch request left"
}

# By hand, on the iPhone: the user taps the test app, swipes down from the top edge and taps Quit. Only the
# hint is requested; the restarts take the default way, our own URL.
run_by_hand() {
  local library_pid game_pid
  PATIENCE=180
  begin_round "by hand: tap “Session Check”, then swipe down from the top edge and tap Quit"
  echo yes > "$CHECK/forget-hint"
  send_requests
  launch
  await_event "process library" || { abort_round "the library did not start"; return; }
  library_pid=$(pid_of "$EVENT")
  echo "      waiting for the tap on “Session Check”"
  await_event "decision .*" 180 || { abort_round "no tap on “Session Check”"; return; }
  ok "decision: $(event_of "$EVENT")"
  await_event "restart url" 60 || { abort_round "the library did not restart through url"; return; }
  check_exit "$library_pid" "the library"
  if ! await_event "process gameMode $PKG .*"; then
    abort_round "no game mode after the restart"
    return
  fi
  game_pid=$(pid_of "$EVENT")
  echo "      waiting for the swipe down from the top edge, then Quit"
  check_game_mode url "$game_pid" shown portrait
  check_exit "$game_pid" "game mode"
  await_event "process library" && ok "Quit returns to the library" || fail "no library after Quit"
  has "$REQUEST_REL" && fail "a launch request is left" || ok "no launch request left"
}

# By hand: the touches in game mode reach the motion sink as ADR-0036's decision 3 says, and each new
# touch's time counts on the uptime clock.
run_touch() {
  local clocks
  PATIENCE=180
  begin_round "touches by hand: tap once, touch with two fingers and lift them, then swipe down from the top edge and tap Quit"
  echo "$PKG" > "$CHECK/tap"
  send_requests
  launch
  await_event "process gameMode $PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  await_event "start window .*" || { abort_round "the guest did not start"; return; }
  echo "      waiting for a tap"
  await_event "motion down ids 0 history 0" 180 && ok "  a tap: DOWN with pointer 0" || fail "  no DOWN"
  await_event "motion up ids 0 history 0" && ok "  and UP with pointer 0" || fail "  no UP"
  echo "      waiting for two fingers"
  await_event "motion pointerDown\\(1\\) ids 0,1 history 0" 180 && ok "  a second finger: POINTER_DOWN(1) with pointers 0,1" \
    || fail "  no POINTER_DOWN(1)"
  await_event "motion pointerUp\\([01]\\) ids 0,1 history 0" && ok "  $(event_of "$EVENT")" || fail "  no POINTER_UP"
  await_event "motion up ids [01] history 0" && ok "  $(event_of "$EVENT")" || fail "  no UP after POINTER_UP"
  echo "      waiting for the swipe down from the top edge, then Quit"
  await_event "motion down ids 0 history 0" 180 && ok "  the swipe: DOWN" || fail "  the swipe: no DOWN"
  await_event "motion cancel ids 0 history 0" && ok "  the swipe: CANCEL once it is the menu's" || fail "  the swipe: no CANCEL"
  await_event "menu opened" && ok "  the menu opens" || fail "  the menu did not open"
  await_event "quit" "$PATIENCE" && ok "  Quit" || fail "  no Quit"
  # UITouch.timestamp: a few ms before now on the uptime clock; CLOCK_MONOTONIC_RAW also counts sleep.
  clocks=$(head -n "$seen" "$EVENTS" | awk '$3 == "touch" && $4 == "clock" { printf "%s%s/%s", sep, $6, $8; sep = " " }')
  echo "      touch clock, ms before now (uptime/monotonic): $clocks"
  [[ -n "$clocks" ]] && awk -v c="$clocks" 'BEGIN { n = split(c, a, " "); for (i = 1; i <= n; i++) { split(a[i], t, "/"); if (t[1] < 0 || t[1] > 100) exit 1 } }' \
    && ok "  every touch's time lies 0 to 100 ms before now on the uptime clock" \
    || fail "  a touch's time is not on the uptime clock: $clocks"
  echo "      moves with history: $(head -n "$seen" "$EVENTS" | grep -c -E 'motion move .* history [1-9]' || true)"
}

# The frame clock ticks while the gate is open, off the main thread, and pauses when the menu takes the focus.
run_frames() {
  local summaries
  begin_round "the frame clock"
  echo "$PKG" > "$CHECK/tap"
  echo 5 > "$CHECK/quit"
  send_requests
  launch
  await_event "process gameMode $PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  await_event "frame clock running" && ok "  the clock runs once the gate opens" || fail "  the clock did not run"
  await_event "menu opened" 20 || { abort_round "the menu did not open"; return; }
  summaries=$(head -n "$seen" "$EVENTS" | awk '$3 == "frames" { $1 = $2 = ""; sub(/^ +/, ""); print }')
  echo "$summaries" | sed 's/^/      /'
  [[ -n "$summaries" ]] && ok "  the ticks reached the sink" || fail "  no ticks reached the sink"
  echo "$summaries" | grep -q "main true" && fail "  a tick came on the main thread" || ok "  no tick came on the main thread"
  await_event "frame clock paused" && ok "  the clock pauses with the menu" || fail "  the clock did not pause"
  await_event "quit" && ok "  Quit" || fail "  no Quit"
  tail -n +$((seen + 1)) "$EVENTS" | grep -q " frames " && fail "  ticks after the pause" || ok "  no ticks after the pause"
}

# Decision 8: game mode ends as the runtime ends a crashed program, or its app cannot start, or it quits; the
# library's next start shows the first two once, and the Quit not at all.
run_ends() {
  begin_round "a crash (SIGSEGV)"
  echo "$PKG" > "$CHECK/tap"
  echo 139 > "$CHECK/crash"
  send_requests
  launch
  await_event "crash 139" 60 || { abort_round "game mode did not crash"; return; }
  sleep 1
  launch
  await_event "process library.*" && expect "  the library shows the signal" \
    "process library quitUnexpectedly Session Check signal 11" "$(event_of "$EVENT")" || fail "  no library after the crash"
  shot "ends-1-crash"
  terminate
  launch
  await_event "process library.*" && expect "  and only once" "process library" "$(event_of "$EVENT")" \
    || fail "  no library at the second start"

  begin_round "an app that cannot start"
  echo "$PKG" > "$CHECK/tap"
  echo "libmain.so is missing" > "$CHECK/start-failed"
  echo yes > "$CHECK/quit"
  send_requests
  launch
  await_event "start failed .*" 60 || { abort_round "no start failure"; return; }
  await_event "process library.*" 30 && expect "  the library shows the reason" \
    "process library quitUnexpectedly Session Check failure libmain.so is missing" "$(event_of "$EVENT")" \
    || fail "  no library after Quit"
  shot "ends-2-start-failed"

  begin_round "a Quit"
  echo "$PKG" > "$CHECK/tap"
  echo yes > "$CHECK/quit"
  send_requests
  launch
  await_event "quit" 60 || { abort_round "no Quit"; return; }
  await_event "process library.*" 30 && expect "  the library shows nothing" "process library" "$(event_of "$EVENT")" \
    || fail "  no library after Quit"
}

# Import the test app; the first session shows the hint.
if [[ "$ON_DEVICE" == yes ]]; then
  terminate
  if has "$REQUEST_REL"; then
    echo "A launch request is left in Minato on the iPhone: open Minato once, then run this again." >&2
    exit 1
  fi
else
  rm -f "$REQUEST"
fi
rm -rf "$CHECK"
mkdir -p "$CHECK"
echo yes > "$CHECK/forget-hint"
: > "$EVENTS"
send_requests
if [[ "$ON_DEVICE" == yes ]]; then
  devicectl_copy to --source "$WORK/session-check.apk" --destination "$SUPPORT_REL/incoming/session-check.apk"
else
  cp "$WORK/session-check.apk" "$SUPPORT/incoming/"
fi
launch
for _ in $(seq 1 100); do
  [[ "$(field state)" == "{'ready': {}}" ]] && break
  sleep 0.2
done
expect "the test app is ready" "{'ready': {}}" "$(field state)"
# The Simulator signs ad hoc at the first tap; the iPhone with the certificate Minato has, at the import.
signer=$(field signedWith)
if [[ "$ON_DEVICE" == yes ]]; then
  [[ "$signer" != None && "$signer" != "ad hoc" ]] && ok "signed with the certificate at the import" \
    || fail "signed with the certificate at the import: $signer"
else
  expect "not signed yet" "None" "$signer"
fi

if [[ -n "${BY_HAND:-}" && "$ON_DEVICE" == yes ]]; then
  run_by_hand
  echo "session check: $failures failed"
  exit $((failures > 0))
fi

if [[ -n "${TOUCH:-}" || -n "${FRAMES:-}" || -n "${ENDS:-}" ]]; then
  [[ -n "${TOUCH:-}" ]] && run_touch
  [[ -n "${FRAMES:-}" ]] && run_frames
  [[ -n "${ENDS:-}" ]] && run_ends
  echo "session check: $failures failed"
  exit $((failures > 0))
fi

run_way url shown portrait
if [[ "$ON_DEVICE" == yes ]]; then
  expect "still signed with the certificate after the tap" "$signer" "$(field signedWith)"
else
  expect "signed ad hoc at the first tap" "ad hoc" "$(field signedWith)"
fi
folder=$(field folder)
has "$SUPPORT_REL/apps/$PKG/$folder/lib/libraw-dyn.so.dylib" && ok "its container is in lib/" || fail "no container in lib/"
run_way workspace "not shown" landscape
run_fallback
run_timeout

echo "session check: $failures failed"
exit $((failures > 0))
