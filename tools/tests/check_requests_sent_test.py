#!/usr/bin/env python3
"""tools/java-start-check.sh: every request file the script writes into the check folder ("$CHECK/<name>") is one of
REQUESTS, the list send_requests copies to the iPhone. In the Simulator the app reads the folder the script writes, so
a name missing from the list works there and never reaches the iPhone: live-counts, pad-only and permission-allow did
not (iPhone, 2026-11-11: the returns round's "no live line" in a build that writes it). Names written through a
variable ("$CHECK/$name") are not read here. Then the script's own send_requests, with a devicectl_copy that keeps
what it would send: each written name, written as "yes", must arrive as "yes".
Runs on /usr/bin/python3 (3.9).
Usage: tools/tests/check_requests_sent_test.py [<java-start-check.sh>]
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def unsent(script):
    """The names written into "$CHECK/<name>" that REQUESTS lacks, sorted; and REQUESTS itself."""
    requests = set()
    for match in re.finditer(r'^REQUESTS\+?="([^"]*)"', script, re.M):
        requests |= set(match.group(1).split())
    written = set(re.findall(r'>\s*"\$CHECK/([A-Za-z0-9_.-]+)"', script))
    return sorted(written - requests), requests


def sent(script, names):
    """What the script's send_requests copies for `names`, each written as "yes" into $CHECK: {name: text or None}."""
    body = re.search(r"^send_requests\(\) \{\n.*?^\}\n", script, re.M | re.S).group(0)
    lists = "".join(line + "\n" for line in script.splitlines() if re.match(r'^REQUESTS\+?="', line))
    with tempfile.TemporaryDirectory() as work:
        check, kept = os.path.join(work, "check"), os.path.join(work, "kept")
        os.makedirs(check)
        for name in names:
            with open(os.path.join(check, name), "w") as handle:
                handle.write("yes\n")
        # devicectl_copy to --source <folder> --destination <folder on the iPhone>: kept here instead.
        # The copy path: send_requests asks console_use first (CONSOLE=yes's way, which sends no files).
        program = (lists + body + 'devicectl_copy() { cp -R "$3" "$KEPT"; }\nconsole_use() { return 1; }\n'
                   'ON_DEVICE=yes EVENTS_RESET=yes CHECK_REL=check\nsend_requests\n')
        subprocess.run(["bash", "-c", program], check=True,
                       env=dict(os.environ, WORK=work, CHECK=check, KEPT=kept, EVENTS=os.path.join(work, "events")))
        result = {}
        for name in names:
            path = os.path.join(kept, name)
            result[name] = open(path).read().strip() if os.path.exists(path) else None
        return result


def main(argv):
    path = argv[1] if len(argv) > 1 else os.path.join(ROOT, "tools", "java-start-check.sh")
    missing, requests = unsent(open(path, encoding="utf-8").read())
    failures = 0
    if not requests:
        print("FAIL  no REQUESTS list in " + path)
        failures += 1
    # The reader itself: a written name the list lacks is found, one it has is not.
    sample = 'REQUESTS="tap ping"\nREQUESTS+=" menu"\necho yes > "$CHECK/tap"\necho yes > "$CHECK/live-counts"\n'
    if unsent(sample)[0] != ["live-counts"]:
        print("FAIL  the reader should find live-counts unsent in the sample, got {}".format(unsent(sample)[0]))
        failures += 1
    if missing:
        print("FAIL  written into $CHECK but not in REQUESTS, so never sent to the iPhone: " + " ".join(missing))
        failures += 1
    script = open(path, encoding="utf-8").read()
    written = sorted(set(re.findall(r'>\s*"\$CHECK/([A-Za-z0-9_.-]+)"', script)))
    arrived = sent(script, written)
    lost = [name for name in written if arrived[name] != "yes"]
    if lost:
        print("FAIL  send_requests did not send these as written: "
              + ", ".join("{} ({})".format(name, arrived[name] or "absent") for name in lost))
        failures += 1
    if failures:
        print("check_requests_sent_test: {} FAILED".format(failures))
        return 1
    print("check_requests_sent_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
