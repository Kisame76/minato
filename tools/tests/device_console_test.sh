#!/usr/bin/env bash
# Checks the device check's console transport without a device: tools/java-start-check.sh --device with CONSOLE=yes
# runs app-start-test against require-device-check.sh's fakes, where every copy into or out of the app's
# tmp/session-check hangs, as the phone's file service did (2026-11-11), and a fake Minato answers on the console that
# `devicectl device process launch --console` gives: it takes the run's requests from MINATO_CHECK_REQUESTS and
# minato://check URLs, writes its events and the guest's output as minato-check-event, minato-guest-log and
# minato-guest-out lines, ends the library's process at the tap through the restart's fallback (way none, confirm), and
# starts game mode when the runner launches it again. The round must end by the console alone: its last line seen,
# app-start-test's lines as Android's, no hung copy. The other copies (library.json, the APK) answer.
# Usage: tools/tests/device_console_test.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

# The rule over the check: the guest log is fetched in fetch_log alone, which reads the console while there is one;
# a bare fetch of it anywhere else copies under the console too.
helper=$(grep -n '^fetch_log() {' tools/java-start-check.sh | cut -d: -f1)
bare=$(grep -n 'fetch "$CHECK_REL/guest-log"' tools/java-start-check.sh |
  awk -F: -v helper="${helper:-0}" '$1 < helper || $1 > helper + 6')
if [[ -n "$bare" ]]; then
  echo "device_console_test: FAIL, a fetch of the guest log outside fetch_log:"
  sed 's/^/  | /' <<< "$bare"
  exit 1
fi

# KEEP=<new folder>: the run's tree stays there.
if [[ -n "${KEEP:-}" ]]; then WORK=$KEEP && mkdir "$WORK" || exit 1; else WORK=$(mktemp -d); fi
[[ -n "${KEEP:-}" ]] || trap 'rm -rf "${WORK:?}"' EXIT
# require-device-check.sh's limited(), devices(), make_tree() and fake_bundle(), taken as they are.
for f in limited devices make_tree fake_bundle; do
  body=$(awk -v name="$f() {" 'index($0, name) == 1 { on = 1; print; if ($0 ~ /}$/ && $0 != name) exit; next }
    on { print } on && $0 == "}" { exit }' tools/require-device-check.sh)
  [[ -n "$body" ]] || { echo "no $f() in tools/require-device-check.sh"; exit 1; }
  eval "$body"
done
tree=$WORK/tree
make_tree "$tree" "wired"
mkdir -p "$tree/tools" "$tree/framework/tests" "$tree/graphics/tests" "$tree/fixtures/expected" "$tree/phone"
cp tools/java-start-check.sh "$tree/tools/"
# app_process_run, which the round's comparison and the fake use, and the graphics checks it imports.
cp framework/tests/*.py "$tree/framework/tests/"
cp graphics/tests/*.py "$tree/graphics/tests/"
fake_bundle "$tree/Minato.app"
for app in app-start-test menu-pause-test glsurface-pause-test; do
  cp "fixtures/expected/$app.logcat" "$tree/fixtures/expected/"
  mkdir -p "$tree/build/$app"
  : > "$tree/build/$app/$app.apk"
done

# The fake Minato: one process per console launch, its state (the app's tmp/session-check and the launch request) in
# $tree/phone.
cat > "$tree/fake/minato.py" <<'EOF'
import json
import os
import shutil
import sys
import time
import urllib.parse

tree, kind = sys.argv[1], sys.argv[2]
phone = os.path.join(tree, "phone")
check = os.path.join(phone, "check")
pid_file = os.path.join(phone, "pids")
pid = 100 + (sum(1 for _ in open(pid_file)) if os.path.exists(pid_file) else 0)
open(pid_file, "a").write(f"{pid}\n")


def write(name, value):
    os.makedirs(check, exist_ok=True)
    open(os.path.join(check, name), "w").write(value + "\n" if value else "")


def read(name):
    try:
        return open(os.path.join(check, name)).read().strip()
    except OSError:
        return ""


if kind == "url":  # minato://check/<name>?<value> to the running app
    url = urllib.parse.urlsplit(sys.argv[3])
    if os.path.isdir(check) and url.netloc == "check":
        write(url.path.lstrip("/"), urllib.parse.unquote(url.query) if url.query else "yes")
    sys.exit(0)

environment = json.loads(sys.argv[3])
console = environment.get("MINATO_CHECK_CONSOLE") == "1"
for line in environment.get("MINATO_CHECK_REQUESTS", "").splitlines():
    name, _, value = line.partition("=")
    write(name, value if "=" in line else "yes")


def say(prefix, text):
    if console:
        print(prefix + text, flush=True)


def event(text):
    if not os.path.isdir(check):
        return
    line = f"{time.time():.3f} {pid} {text}"
    open(os.path.join(check, "events"), "a").write(line + "\n")
    say("minato-check-event ", line)


def expected(app):
    return [line.rstrip("\n") for line in open(os.path.join(tree, "fixtures", "expected", app + ".logcat"))]


def log(line):
    # The guest's log line into the app's file and on the console, as SessionCheck's tee writes both.
    open(os.path.join(check, "guest-log"), "a").write(line + "\n")
    say("minato-guest-log ", line)


def taken(name):
    # A one-time signal: the menu request in the app's folder (an empty file is none, as the app takes it), Home
    # (away) or the return (front) from the runner.
    path = os.path.join(check if name == "menu" else phone, name)
    if not os.path.exists(path):
        return False
    given = name != "menu" or open(path).read().strip() != ""
    os.remove(path)
    return given


def play_menu_pause():
    # menu-pause-test's MainActivity as Minato on the iPhone gives it: the menu's pause, Continue, then Home (the
    # focus's loss first, at willDeactivate) and the return.
    tag = "I/menu-pause-test: MainActivity "
    for line in expected("menu-pause-test")[:5]:
        log(line)
    end = time.time() + 60
    while time.time() < end:
        if taken("menu"):
            log(tag + "onPause")
            log(tag + "onWindowFocusChanged false")
            event("menu opened")
            time.sleep(1)
            log(tag + "onResume")
            log(tag + "onWindowFocusChanged true")
            event("menu continued")
        if taken("away"):
            for name in ("onWindowFocusChanged false", "onPause", "onStop", "onSaveInstanceState"):
                log(tag + name)
        if taken("front"):
            for name in ("onRestart", "onStart", "onResume", "onWindowFocusChanged true"):
                log(tag + name)
        time.sleep(0.1)


def play_glsurface_pause():
    # glsurface-pause-test: Android's lines up to "ready", the menu's pause and Continue, then Android's from "frames
    # after the return" on.
    lines = expected("glsurface-pause-test")
    ready = lines.index("I/glsurface-pause-test: ready")
    for line in lines[: ready + 1]:
        log(line)
    end = time.time() + 60
    while time.time() < end and not taken("menu"):
        time.sleep(0.1)
    for name in ("onPause", "GLSurfaceView.onPause returned", "onResume"):
        log("I/glsurface-pause-test: " + name)
    event("menu opened")
    for line in lines[lines.index("I/glsurface-pause-test: frames after the return"):]:
        log(line)
    time.sleep(60)


request = os.path.join(phone, "launch-request")
if os.path.exists(request):  # game mode
    package = open(request).read().strip()
    os.remove(request)
    event(f"process gameMode {package} 0.100")
    for name in ("away", "front"):
        if os.path.exists(os.path.join(phone, name)):
            os.remove(os.path.join(phone, name))
    if package.endswith((".menupause", ".glsurfacepause")) and read("guest-log"):
        open(os.path.join(check, "guest-log"), "w").close()  # the game mode truncates the log's file
        event("guest log on")
        play_menu_pause() if package.endswith(".menupause") else play_glsurface_pause()
        sys.exit(0)
    if read("guest-log"):
        event("guest log on")
        sys.path.insert(0, os.path.join(tree, "framework", "tests"))
        from app_process_run import expected_lines
        say("minato-guest-log ", "I/art: Using CollectorTypeCMS GC.")
        for line in expected_lines(os.path.join(tree, "fixtures", "expected", "app-start-test.logcat")):
            say("minato-guest-out ", line)
    time.sleep(30)
    sys.exit(0)

event("process library")
library = os.path.join(phone, "library.json")
if read("end"):
    if os.path.exists(library):
        os.remove(library)
    event("check ended")
    shutil.rmtree(check)
    sys.exit(0)
# The APKs the run copied into incoming/ are ready.
packages = ("appstart", "menupause", "glsurfacepause")
json.dump({"entries": [{"id": "io.github.kisame76.minato.test." + name, "state": {"ready": {}}} for name in packages]},
          open(library, "w"))
package = read("tap")
if package:
    os.remove(os.path.join(check, "tap"))
    event(f"decision run signer none")
    open(request, "w").write(package)
    if read("way") == "none":
        event("restartFailed none")
        event("fallback shown Restart Minato")
        if read("confirm"):
            event("fallback confirmed")
            sys.exit(0)
time.sleep(30)
EOF

# The fake xcrun in front of make_tree's: copies of tmp/session-check hang (but the guest log's in the menu round, which
# answer with the phone's file, as the iPhone's file service did in an iPhone run of 2026-11-12), library.json is
# the fake Minato's, the console launch runs the fake Minato, a payload URL reaches it, a launch of Settings is Home and
# a plain launch of Minato its return.
mv "$tree/fake/xcrun" "$tree/fake/xcrun-base"
cat > "$tree/fake/xcrun" <<EOF
#!/bin/bash
tree="$tree"
EOF
cat >> "$tree/fake/xcrun" <<'EOF'
case "$*" in
  *"device copy from"*session-check/guest-log*)
    if [[ -f "$tree/guest-log-copies-answer" ]]; then
      echo "xcrun $* (answers)" >>"$tree/access.log"
      prev=""
      for a in "$@"; do [[ "$prev" == --destination ]] && dest=$a; prev=$a; done
      mkdir -p "$(dirname "$dest")"
      cp "$tree/phone/check/guest-log" "$dest" || exit 1
      exit 0
    fi
    echo "xcrun $* (hangs)" >>"$tree/access.log"
    exec sleep 600 ;;
  *"device copy"*session-check*)
    echo "xcrun $* (hangs)" >>"$tree/access.log"
    exec sleep 600 ;;
  *"device copy from"*library.json*)
    echo "xcrun $*" >>"$tree/access.log"
    prev=""
    for a in "$@"; do [[ "$prev" == --destination ]] && dest=$a; prev=$a; done
    mkdir -p "$(dirname "$dest")"
    [[ -f "$tree/phone/library.json" ]] || exit 1
    cp "$tree/phone/library.json" "$dest"
    exit 0 ;;
  *"device process launch"*--payload-url*)
    echo "xcrun $*" >>"$tree/access.log"
    prev=""
    for a in "$@"; do [[ "$prev" == --payload-url ]] && url=$a; prev=$a; done
    exec /usr/bin/python3 "$tree/fake/minato.py" "$tree" url "$url" ;;
  *"device process launch"*--console*)
    echo "xcrun $*" >>"$tree/access.log"
    prev="" environment="{}"
    for a in "$@"; do [[ "$prev" == --environment-variables ]] && environment=$a; prev=$a; done
    exec /usr/bin/python3 "$tree/fake/minato.py" "$tree" launch "$environment" ;;
  *"device process launch"*com.apple.Preferences)
    echo "xcrun $*" >>"$tree/access.log"
    : >"$tree/phone/away"
    exit 0 ;;
  *"device process launch"*" io.github.kisame76.minato")
    echo "xcrun $*" >>"$tree/access.log"
    : >"$tree/phone/front"
    exit 0 ;;
esac
exec "$tree/fake/xcrun-base" "$@"
EOF
chmod +x "$tree/fake/xcrun"

# One round of the check over the fakes: ONLY=$1, its output in $tree/$1.out, its copies and launches in
# $tree/$1.access.
run_round() {
  rm -f "$tree/access.log"
  (cd "$tree" && CONSOLE=yes ONLY=$1 APP="$tree/Minato.app" TMPDIR="$tree" PATH="$tree/fake:$PATH" \
    DEVICECTL_COPY_TIMEOUT_S=5 limited 120 tools/java-start-check.sh --device) >"$tree/$1.out" 2>&1
  status=$?
  mv "$tree/access.log" "$tree/$1.access" 2>/dev/null || : >"$tree/$1.access"
  problems=""
  [[ $status == 0 ]] || problems+=" exit $status"
  grep -q -E "hung|no response within" "$tree/$1.out" && problems+=" a copy hung"
  grep -q -E "command not found|syntax error|unbound variable|java-start-check.sh: line [0-9]+: " "$tree/$1.out" &&
    problems+=" broken line"
}
report() {
  [[ -n "$problems" ]] || return 0
  echo "device_console_test: FAIL, $1:$problems"
  sed 's/^/  | /' "$tree/$2.out" | tail -n 40
  exit 1
}

# The console round: app-start-test by the console alone, every copy of the check's folder hangs.
run_round app-start-test
grep -q -E "^ok +its last line" "$tree/app-start-test.out" || problems+=" no last line"
grep -q "app-start-test printed what Android 15's app process shows" "$tree/app-start-test.out" ||
  problems+=" lines differ"
report "the console round" app-start-test
launches=$(grep -c 'process launch' "$tree/app-start-test.access")
hangs=$(grep -c '(hangs)' "$tree/app-start-test.access")

# The menu round under the console (an iPhone run's red, 2026-11-12): menu-pause-test's Home and back and
# glsurface-pause-test, with copies of the guest log that answer. The guest log must come from the console alone: a
# copy would put the phone's file in place of the console's lines, and the next console read would add them again,
# the Home's callbacks twice.
: >"$tree/guest-log-copies-answer"
run_round menu-pause-test
grep -q -E "^ok +menu-pause-test: the menu pauses the activity" "$tree/menu-pause-test.out" ||
  problems+=" no menu pause"
grep -q -E "^ok +menu-pause-test: Home and back give Android 15's callbacks" "$tree/menu-pause-test.out" ||
  problems+=" Home and back differ"
grep -q -E "^ok +glsurface-pause-test: the menu pauses" "$tree/menu-pause-test.out" ||
  problems+=" no GLSurfaceView pause"
copies=$(grep -c 'session-check/guest-log.*(answers)' "$tree/menu-pause-test.access")
(( copies == 0 )) || problems+=" $copies copies of the guest log under the console"
report "the menu round" menu-pause-test

echo "device_console_test: ok ($launches console launches, $hangs copies of the check's folder tried;" \
  "the menu round from the console alone)"
