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
cp fixtures/expected/app-start-test.logcat "$tree/fixtures/expected/"
fake_bundle "$tree/Minato.app"
mkdir -p "$tree/build/app-start-test"
: > "$tree/build/app-start-test/app-start-test.apk"

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


request = os.path.join(phone, "launch-request")
if os.path.exists(request):  # game mode
    package = open(request).read().strip()
    os.remove(request)
    event(f"process gameMode {package} 0.100")
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
# The APK the run copied into incoming/ is ready.
json.dump({"entries": [{"id": "io.github.kisame76.minato.test.appstart", "state": {"ready": {}}}]}, open(library, "w"))
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

# The fake xcrun in front of make_tree's: copies of tmp/session-check hang, library.json is the fake Minato's, the
# console launch runs the fake Minato, a payload URL reaches it.
mv "$tree/fake/xcrun" "$tree/fake/xcrun-base"
cat > "$tree/fake/xcrun" <<EOF
#!/bin/bash
tree="$tree"
EOF
cat >> "$tree/fake/xcrun" <<'EOF'
case "$*" in
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
esac
exec "$tree/fake/xcrun-base" "$@"
EOF
chmod +x "$tree/fake/xcrun"

(cd "$tree" && CONSOLE=yes ONLY=app-start-test APP="$tree/Minato.app" TMPDIR="$tree" PATH="$tree/fake:$PATH" \
  DEVICECTL_COPY_TIMEOUT_S=5 limited 120 tools/java-start-check.sh --device) >"$tree/out" 2>&1
status=$?
problems=""
[[ $status == 0 ]] || problems+=" exit $status"
grep -q -E "^ok +its last line" "$tree/out" || problems+=" no last line"
grep -q "app-start-test printed what Android 15's app process shows" "$tree/out" || problems+=" lines differ"
grep -q -E "hung|no response within" "$tree/out" && problems+=" a copy hung"
grep -q -E "command not found|syntax error|unbound variable|java-start-check.sh: line [0-9]+: " "$tree/out" &&
  problems+=" broken line"
if [[ -n "$problems" ]]; then
  echo "device_console_test: FAIL, the console round:$problems"
  sed 's/^/  | /' "$tree/out" | tail -n 40
  exit 1
fi
echo "device_console_test: ok ($(grep -c 'process launch' "$tree/access.log") console launches," \
  "$(grep -c '(hangs)' "$tree/access.log") copies of the check's folder tried)"
