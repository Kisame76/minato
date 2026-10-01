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
# The test app is removed afterwards, the hint counts as not shown yet, and the FPS label is off.
# On the iPhone (--device), devicectl sends the requests and fetches the events, and the app must stay
# unlocked. BY_HAND=1 runs one round there in which the user taps the test app, swipes down from the top
# edge and taps Quit; the script waits for each step. TOUCH=1 runs one round in which the touches come by hand
# (ADR-0036, decision 3): a tap, two fingers down and up, then the swipe down from the top edge and Quit; it
# checks what reaches the motion sink and prints how far each new touch's time lies before now on the
# uptime clock and on CLOCK_MONOTONIC_RAW. FRAMES=1 runs one round in which game mode stays 5 s before its
# menu and Quit (ADR-0036, decision 2): the frame clock runs while the gate is open, off the main thread, and
# pauses with the menu; it prints the intervals between ticks. ENDS=1 runs the rounds of decision 8: a crash and
# an app that cannot start show once at the library's next start, and a Quit shows nothing. AUDIO=1 runs one
# round in which game mode plays a 2 s test tone at 44.1 kHz through the audio host (ADR-0036, decision 4): the
# session activates and asks for a 5 ms IO buffer, the engine runs, the render block reads about as many frames as 2 s
# hold without an xrun, and the engine stops and the session deactivates once the stream closes; a second round
# goes Home and back during the tone, and the engine starts again at the foreground. FPS=1 runs the rounds of the
# FPS label (ADR-0032, decision 9): with fake presents at 60 a second, "Show FPS" in the menu turns it on, the label
# counts 60 in the first window and again in the next one after Home and back; the setting holds at the next
# start, and "Hide FPS" turns it off. ACTIVITY=1 runs test 7 of ADR-0032 in the Simulator: activity-test
# (build/fixtures/apks/activity-test.apk) on activity-host (ADR-0028 d), through its start, Home and back, and
# the end that follows its ANativeActivity_finish; its log must be Android 15's
# (fixtures/expected/activity-test.logcat) apart from the recorded departure: the focus goes at "inactive",
# before onPause, where Android pauses first. LIVE=1 runs test 8 of ADR-0032 with the live variants of ADR-0036 a)
# (build/fixtures/apks/gles-live.apk and, on the iPhone, vulkan-live.apk, which needs MoltenVK): each draws paced
# by AChoreographer and asks for 120 and 60 Hz in turn every 5 s; game mode goes Home and back, and Quit returns
# to the library. It checks that the FPS label counts in both windows, that the app ends with "windows 2,
# failures 0" and without a GPU error in its log, that no answer waits out its timeout and, on the iPhone, that
# the touches the user makes reach the input channel and that the display link ran at 120 and at 60 Hz; it prints
# the gap between the scene's willDeactivate and didEnterBackground. LIVE_BY_HAND=1 runs vulkan-live once on the
# iPhone with Home, the menu and Quit by hand, for the gap of the real gesture. GUEST_AUDIO=1 runs ADR-0036 c)'s
# test apps in game mode (build/fixtures/apks/aaudio-test.apk and opensl-test.apk) until their
# ANativeActivity_finish: each log must be Android 15's (fixtures/expected/<app>.logcat); the lines of its -device
# tag, the rates, bursts, callbacks, xruns and the timestamp's age, and what the audio host did are printed. The
# apps play zeros, so this log, not a listen, is the check. Every other round runs game mode without a guest.
# Usage: tools/session-check.sh [simulator name or UDID]
#        tools/session-check.sh --device [device name or identifier]
# Optional environment:
#   SHOTS       a folder for screenshots of game mode, its menu and the fallback's alerts
#   SKIP_BUILD  when set, installs the app built last instead of building it first
#   BY_HAND     with --device: only the round by hand
#   TOUCH       only the round of touches by hand
#   FRAMES      only the round of the frame clock
#   ENDS        only the rounds of ends that go wrong
#   AUDIO       only the round of the test tone
#   AWAY        with AUDIO: the seconds the second round stays in Settings (2 when unset)
#   FPS         only the rounds of the FPS label
#   ACTIVITY    only test 7, with activity-test
#   LIVE        only test 8, with the live variants
#   LIVE_BY_HAND  with --device: only test 8's round by hand, with vulkan-live
#   GUEST_AUDIO only the rounds of aaudio-test and opensl-test
#   PORTRAIT    only the round of the library's orientation
set -euo pipefail
cd "$(dirname "$0")/.."

BUNDLE_ID="io.github.kisame76.minato"
PKG="io.github.kisame76.minato.check.session"
ACTIVITY_PKG="io.github.kisame76.minato.test.activity"
ACTIVITY_APK="build/fixtures/apks/activity-test.apk"
GLES_LIVE_PKG="io.github.kisame76.minato.test.gles.live"
VULKAN_LIVE_PKG="io.github.kisame76.minato.test.vulkan.live"
AAUDIO_PKG="io.github.kisame76.minato.test.aaudio"
OPENSL_PKG="io.github.kisame76.minato.test.opensl"
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
if [[ -n "${LIVE_BY_HAND:-}" && "$ON_DEVICE" == no ]]; then
  echo "LIVE_BY_HAND needs the iPhone (--device)." >&2
  exit 1
fi

# The fixture apps this run imports beside the test app, one "<package> <APK>" per line. The Simulator has no
# MoltenVK (ADR-0030, decision 8), so vulkan-live runs only on the iPhone.
GUEST_APPS=""
[[ -z "${ACTIVITY:-}" ]] || GUEST_APPS+="$ACTIVITY_PKG $ACTIVITY_APK"$'\n'
[[ -z "${LIVE:-}" ]] || GUEST_APPS+="$GLES_LIVE_PKG build/fixtures/apks/gles-live.apk"$'\n'
if [[ -n "${GUEST_AUDIO:-}" ]]; then
  GUEST_APPS+="$AAUDIO_PKG build/fixtures/apks/aaudio-test.apk"$'\n'"$OPENSL_PKG build/fixtures/apks/opensl-test.apk"$'\n'
fi
if [[ "$ON_DEVICE" == yes && -n "${LIVE:-}${LIVE_BY_HAND:-}" ]]; then
  GUEST_APPS+="$VULKAN_LIVE_PKG build/fixtures/apks/vulkan-live.apk"$'\n'
fi
guest_packages() { printf '%s' "$GUEST_APPS" | awk '{ print $1 }'; }

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
REQUESTS="way tap quit confirm orientation unanswered forget-hint crash start-failed tone fps no-guest guest-log extra-session turn-library end events"
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
  # The hint counts as not shown yet and the FPS label is off, as before the first session, and the test apps go.
  rm -rf "$CHECK"
  mkdir -p "$CHECK"
  echo yes > "$CHECK/forget-hint"
  { echo "$PKG"; guest_packages; } > "$CHECK/end"
  send_requests || true
  launch || true
  for _ in $(seq 1 50); do fetch_requests || break; sleep 0.2; done
  terminate
  if [[ "$ON_DEVICE" == no ]]; then
    rm -rf "$CHECK" "$SUPPORT/apps/$PKG" "$SUPPORT/data/$PKG" "$REQUEST"
    for package in $(guest_packages); do rm -rf "$SUPPORT/apps/$package" "$SUPPORT/data/$package"; done
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
# What the user is to do or check now, on the iPhone.
say() { [[ "$ON_DEVICE" == no ]] || echo ">>>   $1"; }

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
# Field $1 of the test app's entry, or of app $2's.
field() {
  has "$SUPPORT_REL/library.json" || true
  python3 -c 'import json,sys; e={e["id"]:e for e in json.load(open(sys.argv[1]))["entries"]}.get(sys.argv[2]); print(None if e is None else e.get(sys.argv[3]))' \
    "$SUPPORT/library.json" "${2:-$PKG}" "$1" 2>/dev/null || echo None
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
  # Only test 7 runs a guest: the test app's library is no NativeActivity.
  echo yes > "$CHECK/no-guest"
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
  echo "      the events: action, history, failed posts, pressure/touch major in pixels per pointer"
  head -n "$seen" "$EVENTS" | awk '$3 == "motion" { print "        " $4, $8, $12, $14 }' | head -n 60
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

# ADR-0038, decision 1.1: the library stays in portrait when it is asked to turn, as a turned device asks.
run_portrait() {
  begin_round "the library stays in portrait"
  echo yes > "$CHECK/turn-library"
  send_requests
  launch
  await_event "library orientation .*" 30 || { abort_round "the library did not say its orientation"; return; }
  expect "  the library stays in portrait" "library orientation portrait" "$(event_of "$EVENT")"
  grep -q " library turn refused " "$EVENTS" && ok "  iOS refuses the turn" || fail "  iOS did not refuse the turn"
}

# ADR-0036, decision 4: a test tone through the audio host's session, engine and render block.
run_audio() {
  local line frames expected xruns
  begin_round "the test tone"
  echo "$PKG" > "$CHECK/tap"
  echo 2 > "$CHECK/tone"
  echo 3 > "$CHECK/quit"
  send_requests
  launch
  await_event "process gameMode $PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  await_event "audio activate .*" && ok "  the session activates" || fail "  the session did not activate"
  await_event "audio startEngine .*" && ok "  the engine starts" || fail "  the engine did not start"
  await_event "audio tone opened .*" && ok "  the stream opens" || fail "  the stream did not open"
  say "Listen now: a tone should play for 2 s."
  await_event "audio tone frames .*" 10 || { abort_round "the tone did not end"; return; }
  line=$(head -n "$seen" "$EVENTS" | grep " audio tone frames " | tail -n 1)
  read -r frames expected xruns < <(echo "$line" | awk '{ print $6, $8, $10 }')
  head -n "$seen" "$EVENTS" | awk '$3 == "audio" { $1 = $2 = ""; sub(/^ +/, ""); print }' | sed 's/^/      /'
  (( frames * 10 >= expected * 9 && frames * 10 <= expected * 11 )) \
    && ok "  the render block read $frames frames, 2 s hold $expected" || fail "  the render block read $frames frames, 2 s hold $expected"
  [[ "$xruns" == 0 ]] && ok "  no xrun" || fail "  $xruns xruns"
  # The Simulator keeps the Mac's IO buffer; the lines above show the one the session gave.
  head -n "$seen" "$EVENTS" | grep -q " audio preferIOBuffer(0.005) " \
    && ok "  the session asks for a 5 ms IO buffer for LOW_LATENCY" || fail "  the session did not ask for 5 ms"
  head -n "$seen" "$EVENTS" | grep -q " audio deactivate " \
    && ok "  the engine stops and the session deactivates" || fail "  the session stayed active"
  head -n "$seen" "$EVENTS" | grep -q " audio .* failed" && fail "  an audio step failed" || ok "  no audio step failed"
  await_event "quit" && ok "  Quit" || fail "  no Quit"

  # iOS stops the engine in the background: .ambient has no background audio.
  # The tone must outlast the time away: on the iPhone, devicectl's fetch and launch take up to about 8 s more
  # before Settings is in front, and a tone that ends in the background has no engine to start again.
  local away=${AWAY:-2} slack=6
  [[ "$ON_DEVICE" == yes ]] && slack=16
  begin_round "the test tone across Home and back, $away s away"
  echo "$PKG" > "$CHECK/tap"
  echo $((away + slack)) > "$CHECK/tone"
  echo $((away + slack + 2)) > "$CHECK/quit"
  send_requests
  launch
  await_event "process gameMode $PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  await_event "audio tone opened .*" || { abort_round "the stream did not open"; return; }
  say "Listen now: a tone should play."
  sleep 1
  launch com.apple.Preferences
  say "Minato is in the background for $away s now (Settings is in front): listen whether the tone goes on."
  sleep "$away"
  launch
  say "Minato is back: listen whether the tone plays again."
  await_event "audio foreground .*" && ok "  the host sees the foreground ($EVENT)" || fail "  the host did not see the foreground"
  await_event "audio startEngine .*" 5 && ok "  the engine starts again" || fail "  the engine did not start again"
  await_event "audio tone frames .*" 15 || { abort_round "the tone did not end"; return; }
  say "The tone has ended."
  head -n "$seen" "$EVENTS" | grep -q " audio .* failed" && fail "  an audio step failed" || ok "  no audio step failed"
  await_event "quit" 10 && ok "  Quit" || fail "  no Quit"
}

# ADR-0032, decision 9: the FPS label, from the presents of the guest's window sampled once a second. Nothing draws
# without a guest, so the Debug build counts 60 fake presents a second for every window.
run_fps() {
  begin_round "the FPS label"
  echo "$PKG" > "$CHECK/tap"
  echo 60 > "$CHECK/fps"
  echo 9 > "$CHECK/quit"
  send_requests
  launch
  await_event "process gameMode $PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  await_event "fps at start .*" && expect "  off by default" "fps at start hidden" "$(event_of "$EVENT")" \
    || fail "  no FPS request at the start"
  await_event "fps shown" && ok "  \"Show FPS\" turns it on" || fail "  \"Show FPS\" did not turn it on"
  await_event "fps window 1 presents [0-9]+ value (59|60|61)" 5 && ok "  it counts 60 in window 1 ($(event_of "$EVENT"))" \
    || fail "  it did not count 60 in window 1"
  shot "fps-label"
  launch com.apple.Preferences
  sleep 2
  launch
  await_event "fps window 2 presents [0-9]+ value none" 5 && ok "  window 2 starts its own count" \
    || fail "  window 2 did not start its own count"
  await_event "fps window 2 presents [0-9]+ value (59|60|61)" 5 && ok "  it counts 60 in window 2 ($(event_of "$EVENT"))" \
    || fail "  it did not count 60 in window 2"
  await_event "quit" 15 && ok "  Quit" || fail "  no Quit"

  begin_round "the FPS label at the next start"
  echo "$PKG" > "$CHECK/tap"
  echo hide > "$CHECK/fps"
  echo 3 > "$CHECK/quit"
  send_requests
  launch
  await_event "process gameMode $PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  await_event "fps at start .*" && expect "  the setting holds" "fps at start shown" "$(event_of "$EVENT")" \
    || fail "  no FPS request at the start"
  await_event "fps hidden" && ok "  \"Hide FPS\" turns it off" || fail "  \"Hide FPS\" did not turn it off"
  await_event "quit" 10 || fail "  no Quit"
  head -n "$seen" "$EVENTS" | sed -n '/ fps hidden$/,$p' | grep -q " fps window " \
    && fail "  it still counts after \"Hide FPS\"" || ok "  no count after \"Hide FPS\""
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

# Test 7: activity-test on activity-host, from the tap through Home and back to the end it asks for itself.
run_activity() {
  begin_round "test 7: activity-test on activity-host"
  rm -f "$CHECK/no-guest"
  echo "$ACTIVITY_PKG" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  send_requests
  launch
  await_event "process gameMode $ACTIVITY_PKG .*" 60 || { abort_round "no game mode after the tap"; return; }
  local pid start
  pid=$(pid_of "$EVENT")
  await_event "start window [0-9]+x[0-9]+" && start=${EVENT##* } || start=none
  await_event 'callback onWindowFocusChanged\(true\)' 20 || { abort_round "activity-test did not gain the focus"; return; }
  shot "activity-1-started"
  sleep 1
  launch com.apple.Preferences
  await_event "callback onSaveInstanceState" 10 || fail "  no onSaveInstanceState in the background"
  sleep 1
  launch
  await_event "request finish" 20 && ok "  activity-test finishes itself after its return" || fail "  no FINISH"
  await_event "program ended .*" 10 && expect "  activity-host ends with status 0" "program ended 0" "$(event_of "$EVENT")" \
    || fail "  activity-host did not end"
  await_event "process library.*" 30 && expect "  back in the library, without a notice" "process library" "$(event_of "$EVENT")" \
    || fail "  no library after the end"
  expect "  the callbacks" "$START onWindowFocusChanged(false) onPause onNativeWindowDestroyed onStop onSaveInstanceState onStart onResume onNativeWindowCreated onWindowFocusChanged(true) onWindowFocusChanged(false) $QUIT" \
    "$(callbacks_of "$pid")"
  expect "  the requests" "keepScreenOn(true) finish" \
    "$(head -n "$seen" "$EVENTS" | awk -v pid="$pid" '$2 == pid && $3 == "request" { printf "%s%s", sep, $4; sep = " " }')"

  # The app's lines against Android 15's, with test 7's departure: LOST_FOCUS before PAUSE.
  has "$CHECK_REL/guest-log" || { fail "  no guest log"; return; }
  grep '^I/activity-test: ' "$CHECK/guest-log" > "$WORK/activity-app.log" || true
  python3 - fixtures/expected/activity-test.logcat > "$WORK/activity-expected.log" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
pause, lost = "I/activity-test: APP_CMD_PAUSE", "I/activity-test: APP_CMD_LOST_FOCUS"
for i in range(len(lines) - 1):
    if lines[i] == pause and lines[i + 1] == lost:
        lines[i], lines[i + 1] = lost, pause
print("\n".join(lines))
PY
  if diff -u "$WORK/activity-expected.log" "$WORK/activity-app.log" > "$WORK/activity.diff"; then
    ok "  its log is Android 15's but for the departure ($(wc -l < "$WORK/activity-app.log" | tr -d ' ') lines)"
  else
    fail "  its log differs from Android 15's:"
    sed 's/^/      | /' "$WORK/activity.diff"
  fi
  local device
  device=$(grep '^I/activity-test-device: ' "$CHECK/guest-log" || true)
  sed 's/^/      /' <<< "$device"
  grep -q "^I/activity-test-device: window 1: ${start/x/ x } *$" <<< "$device" \
    && ok "  window 1 has the window's size ($start)" || fail "  window 1 does not have the window's size ($start)"
  grep -q "^I/activity-test-device: window 2: ${start/x/ x } *$" <<< "$device" \
    && ok "  window 2 has it too" || fail "  window 2 does not have the window's size ($start)"
  if [[ "$ON_DEVICE" == no ]]; then
    local folder=$SUPPORT/data/$ACTIVITY_PKG
    [[ -d "$folder/data/files" && -d "$folder/external/files" && -d "$folder/obb" ]] \
      && ok "  the app's three folders exist" || fail "  the app's folders are missing"
  fi
}

# The lines of tag $1 in the guest's log against Android 15's (fixtures/expected/$1.logcat), then the lines of
# its -device tag, which differ by device.
compare_guest_log() {
  local tag=$1 device
  grep "^I/$tag: " "$CHECK/guest-log" > "$WORK/$tag-app.log" || true
  if diff -u "fixtures/expected/$tag.logcat" "$WORK/$tag-app.log" > "$WORK/$tag.diff"; then
    ok "  its log is Android 15's ($(wc -l < "$WORK/$tag-app.log" | tr -d ' ') lines)"
  else
    fail "  its log differs from Android 15's:"
    sed 's/^/      | /' "$WORK/$tag.diff"
  fi
  device=$(grep "^I/$tag-device: " "$CHECK/guest-log" || true)
  if [[ -n "$device" ]]; then sed 's/^/      /' <<< "$device"; else fail "  no $tag-device lines"; fi
}

# ADR-0036 c): test app $1, whose log tag is $2, runs in game mode until its ANativeActivity_finish. It plays
# zeros; what the audio host did goes with its log.
run_guest_audio() {
  local package=$1 tag=$2 pid audio
  begin_round "$tag in game mode"
  rm -f "$CHECK/no-guest"
  echo "$package" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  send_requests
  launch
  await_event "process gameMode $package .*" 60 || { abort_round "no game mode after the tap"; return; }
  pid=$(pid_of "$EVENT")
  say "$tag runs now; it plays silence and ends by itself."
  await_event "request finish" 60 && ok "  $tag finishes itself" || fail "  no FINISH"
  await_event "program ended .*" 10 && expect "  it ends with status 0" "program ended 0" "$(event_of "$EVENT")" \
    || fail "  $tag did not end"
  await_event "process library.*" 30 && expect "  back in the library, without a notice" "process library" "$(event_of "$EVENT")" \
    || fail "  no library after the end"
  audio=$(head -n "$seen" "$EVENTS" | awk -v pid="$pid" '$2 == pid && $3 == "audio" { $1 = $2 = ""; sub(/^ +/, ""); print }')
  sed 's/^/      /' <<< "$audio"
  grep -q "^audio publishOutput " <<< "$audio" && ok "  the output reaches the runtime" || fail "  no publishOutput"
  grep -q "^audio stream closed " <<< "$audio" && ok "  its streams close through the host" || fail "  no stream closed"
  grep -q "^audio disconnected" <<< "$audio" && fail "  a stream was disconnected" || ok "  no stream disconnected"
  grep -q " failed" <<< "$audio" && fail "  an audio step failed" || ok "  no audio step failed"
  has "$CHECK_REL/guest-log" || { fail "  no guest log"; return; }
  compare_guest_log "$tag"
}

# Test 8 (ADR-0032) with a live variant (ADR-0036 a): $1 is its package, $2 its log tag, and $3 "by-hand" when
# the user goes Home and back and quits. It draws paced by AChoreographer, a frame per callback, and asks for 120
# and 60 Hz in turn every 5 s.
run_live() {
  local pkg=$1 tag=$2 by_hand=${3:-} pid lines
  begin_round "test 8: $tag${by_hand:+, Home and Quit by hand}"
  # The folder keeps the last round's requests.
  rm -f "$CHECK/no-guest" "$CHECK/quit" "$CHECK/tone"
  echo "$pkg" > "$CHECK/tap"
  echo yes > "$CHECK/guest-log"
  # With a guest, "show" only turns the label on: its count is the window's real presents.
  echo show > "$CHECK/fps"
  # A second GameSession, as SwiftUI may build one, must change nothing: one sampler, the requests to this one.
  echo yes > "$CHECK/extra-session"
  [[ -n "$by_hand" ]] || echo 28 > "$CHECK/quit"
  send_requests
  launch
  await_event "process gameMode $pkg .*" 60 || { abort_round "no game mode after the tap"; return; }
  pid=$(pid_of "$EVENT")
  await_event 'callback onWindowFocusChanged\(true\)' 30 || { abort_round "$tag did not gain the focus"; return; }
  say "Touch now: tap and swipe on the picture for about 10 s, with one finger and with two."
  sleep 12
  if [[ -n "$by_hand" ]]; then
    say "Swipe up now to the Home Screen, wait about 5 s, then tap Minato."
    await_event "scene didEnterBackground" 180 || { abort_round "no background"; return; }
    await_event "scene didActivate" 180 || { abort_round "Minato did not come back"; return; }
  else
    launch com.apple.Preferences
    await_event "scene didEnterBackground" 10 || { abort_round "no background"; return; }
    sleep 3
    launch
    await_event "scene didActivate" 10 || { abort_round "Minato did not come back"; return; }
  fi
  await_event 'callback onWindowFocusChanged\(true\)' 20 && ok "  back with the focus" || fail "  no focus after the return"
  say "Touch now again for a few seconds."
  if [[ -n "$by_hand" ]]; then
    sleep 8
    say "Swipe down from the top edge now, then tap Quit."
    await_event "quit" 180 && ok "  Quit" || fail "  no Quit"
  else
    await_event "quit" 40 && ok "  Quit" || fail "  no Quit"
  fi
  say "Done with $tag: nothing more to do in this round."
  await_event "program ended .*" 20 && expect "  $tag ends with status 0" "program ended 0" "$(event_of "$EVENT")" \
    || fail "  $tag did not end"
  await_event "process library.*" 30 && expect "  back in the library, without a notice" "process library" "$(event_of "$EVENT")" \
    || fail "  no library after Quit"
  lines=$(head -n "$seen" "$EVENTS")
  echo "      callbacks: $(callbacks_of "$pid")"

  # The gap Grafik's finding depends on: from the focus's loss to the background, with the callbacks in it.
  awk -v pid="$pid" '
    $2 != pid { next }
    $3 == "scene" && $4 == "willDeactivate" { start = $1 }
    start != "" && ($3 == "scene" || $3 == "callback" || $3 == "answer") {
      printf "      +%4.0f ms  %s\n", ($1 - start) * 1000, substr($0, index($0, $3))
    }
    $3 == "scene" && $4 == "didActivate" { start = "" }' <<< "$lines"
  local gap
  gap=$(awk -v pid="$pid" '$2 == pid && $3 == "scene" && $4 == "willDeactivate" { start = $1 }
    $2 == pid && $3 == "scene" && $4 == "didEnterBackground" && start != "" { printf "%.0f", ($1 - start) * 1000; exit }' <<< "$lines")
  [[ -n "$gap" ]] && ok "  willDeactivate to didEnterBackground: $gap ms" || fail "  no gap from willDeactivate to didEnterBackground"
  local answers
  answers=$(awk -v pid="$pid" '$2 == pid && $3 == "answer"' <<< "$lines")
  [[ -n "$answers" ]] && ok "  answers timed: $(wc -l <<< "$answers" | tr -d ' ')" || fail "  no answer was timed"
  grep -q " timedOut " <<< "$answers" && fail "  a wait timed out: $(grep " timedOut " <<< "$answers" | awk '{ print $4 }' | tr '\n' ' ')" \
    || ok "  no wait timed out"

  # The FPS label counts the real presents of both windows, before Home and after the return.
  local counted
  counted=$(awk -v pid="$pid" '$2 == pid && $3 == "fps" && $4 == "window" && $9 != "none" && $9 > 0 { print $5 }' <<< "$lines" | sort -u)
  echo "      FPS values: $(awk -v pid="$pid" '$2 == pid && $3 == "fps" && $4 == "window" { printf "%s:%s ", $5, $9 }' <<< "$lines")"
  [[ "$(wc -l <<< "$counted" | tr -d ' ')" == 2 && -n "$counted" ]] && ok "  the FPS label counts in both windows" \
    || fail "  the FPS label did not count in two windows: $(echo $counted)"
  grep -q " extra session made$" <<< "$lines" && ok "  a second GameSession was built" || fail "  no second GameSession"
  local samples span
  read -r samples span < <(awk -v pid="$pid" '$2 == pid && $3 == "fps" && $4 == "window" { if (n++ == 0) first = $1; last = $1 }
    END { printf "%d %.0f\n", n, last - first }' <<< "$lines")
  (( samples <= span + 2 )) && ok "  one FPS sampler ($samples samples in $span s)" \
    || fail "  more than one FPS sampler: $samples samples in $span s"

  # The display link: its summaries of 120 intervals, beside what the app asked for and measured.
  awk -v pid="$pid" '$2 == pid && ($3 == "frames" || ($3 == "request" && $4 ~ /^frameRate/)) { print "      " substr($0, index($0, $3)) }' <<< "$lines"
  if [[ "$ON_DEVICE" == yes ]]; then
    grep -q -E " frames [0-9]+ interval [0-9.]+/8\.[23][0-9]/" <<< "$lines" && grep -q -E " frames [0-9]+ interval [0-9.]+/16\.[67][0-9]/" <<< "$lines" \
      && ok "  the display link ran at 120 and at 60 Hz" || fail "  the display link did not run at both 120 and 60 Hz"
    # The touches reached the guest's input channel.
    local motions last
    motions=$(awk -v pid="$pid" '$2 == pid && $3 == "motion"' <<< "$lines")
    last=$(tail -n 1 <<< "$motions")
    echo "      motion events: $(grep -c . <<< "$motions" || true), the last: ${last#* * }"
    [[ -n "$motions" ]] && ok "  touches reached the sink" || fail "  no touch reached the sink"
    [[ -n "$last" && "$(awk '{ print $10 }' <<< "$last")" != 0 ]] && ok "  the input channel exists" || fail "  no input channel"
    [[ -n "$last" && "$(awk '{ print $12 }' <<< "$last")" == 0 ]] && ok "  no post failed" || fail "  posts failed: $(awk '{ print $12 }' <<< "$last")"
  fi

  # The app's own count, and no GPU error from it, ANGLE or MoltenVK, which all write to its log.
  has "$CHECK_REL/guest-log" || { fail "  no guest log"; return; }
  grep -E "^I/$tag-device: (rate|ANativeWindow_setFrameRate|window)" "$CHECK/guest-log" | sed 's/^/      /' || true
  local end
  end=$(grep -E "^I/$tag: onDestroy: " "$CHECK/guest-log" || true)
  if [[ "$end" =~ windows\ 2,\ failures\ 0, ]]; then
    ok "  $tag: ${end#*onDestroy: }"
  else
    fail "  $tag's end: \"${end:-none}\"; its log:"
    sed 's/^/      | /' "$CHECK/guest-log" | head -n 60
  fi
  local errors
  errors=$(grep -i -E "mvk-error|VK_ERROR|DEVICE_LOST|command buffer|GL error|eglSwapBuffers failed|no EGL|no surface|release failed" "$CHECK/guest-log" || true)
  [[ -z "$errors" ]] && ok "  no GPU error in the log" || { fail "  GPU errors in the log:"; sed 's/^/      | /' <<< "$errors" | head -n 20; }
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
while read -r package apk; do
  [[ -n "$package" ]] || continue
  [[ -f "$apk" ]] || { echo "$apk is missing: tools/build-fixtures.sh builds it" >&2; exit 1; }
  if [[ "$ON_DEVICE" == yes ]]; then
    devicectl_copy to --source "$apk" --destination "$SUPPORT_REL/incoming/${apk##*/}"
  else
    cp "$apk" "$SUPPORT/incoming/"
  fi
done <<< "$GUEST_APPS"
launch
for _ in $(seq 1 100); do
  [[ "$(field state)" == "{'ready': {}}" ]] && break
  sleep 0.2
done
expect "the test app is ready" "{'ready': {}}" "$(field state)"
for package in $(guest_packages); do
  for _ in $(seq 1 100); do
    [[ "$(field state "$package")" == "{'ready': {}}" ]] && break
    sleep 0.2
  done
  expect "$package is ready" "{'ready': {}}" "$(field state "$package")"
done
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

if [[ -n "${TOUCH:-}${FRAMES:-}${ENDS:-}${AUDIO:-}${FPS:-}${ACTIVITY:-}${LIVE:-}${LIVE_BY_HAND:-}${GUEST_AUDIO:-}${PORTRAIT:-}" ]]; then
  [[ -n "${TOUCH:-}" ]] && run_touch
  [[ -n "${FRAMES:-}" ]] && run_frames
  [[ -n "${ENDS:-}" ]] && run_ends
  [[ -n "${AUDIO:-}" ]] && run_audio
  [[ -n "${PORTRAIT:-}" ]] && run_portrait
  [[ -n "${FPS:-}" ]] && run_fps
  [[ -n "${ACTIVITY:-}" ]] && run_activity
  if [[ -n "${LIVE:-}" ]]; then
    run_live "$GLES_LIVE_PKG" gles-live
    [[ "$ON_DEVICE" == no ]] || run_live "$VULKAN_LIVE_PKG" vulkan-live
  fi
  [[ -n "${LIVE_BY_HAND:-}" ]] && run_live "$VULKAN_LIVE_PKG" vulkan-live by-hand
  if [[ -n "${GUEST_AUDIO:-}" ]]; then
    run_guest_audio "$AAUDIO_PKG" aaudio-test
    run_guest_audio "$OPENSL_PKG" opensl-test
  fi
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
run_portrait
run_fallback
run_timeout

echo "session check: $failures failed"
exit $((failures > 0))
