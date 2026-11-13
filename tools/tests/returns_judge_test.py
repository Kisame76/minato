#!/usr/bin/env python3
"""Tests the judge of tools/java-start-check.sh's SDL returns round (the Python block after its RETURNS_RACE_DEPTH
line, run as the script runs it) on made rounds of footprints, one race per trip: a device round with ANGLE's buffer
release (foot up 77 MiB at one return, then down below the first return's) and a Simulator round with it (+17 MiB
per race) are ok; a build without the release (+65 MiB per race, as in the Simulator) is red by its third return;
one spike of 77 MiB at the last return after one race is ok. Each return's first swap is printed, and one later than
RETURNS_FIRST_SWAP after its return is red; a return without a measured swap is named n/a. The swap's measure,
first_swap()'s awk program, is run on made session events."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "java-start-check.sh")


def judge_source():
    """The judge's Python block: the heredoc after the line that passes RETURNS_RACE_DEPTH."""
    lines = open(SCRIPT, encoding="utf-8").read().split("\n")
    start = next(i for i, row in enumerate(lines) if "RETURNS_RACE_DEPTH:-4" in row)
    while "<<'PY'" not in lines[start]:
        start += 1
    end = next(i for i in range(start + 1, len(lines)) if lines[i] == "PY")
    return "\n".join(lines[start + 1:end])


def first_swap_program():
    """first_swap()'s awk program, as the script runs it."""
    text = open(SCRIPT, encoding="utf-8").read()
    body = text[text.index("\nfirst_swap() {"):]
    body = body[body.index("awk '") + len("awk '"):]
    return body[:body.index("' \"$EVENTS\"")]


def first_swap(events):
    """first_swap() on made events (epoch seconds, a pid, the event)."""
    result = subprocess.run(["awk", first_swap_program()], input="\n".join(events) + "\n", stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True)
    return result.stdout.strip()


def run(feet, races, contexts=None, depths=None, knobs=None, swaps=None):
    """The judge on a round: footprints (start, return 1, ...), races per trip (index 0 the start), each race with one
    more EGL context and two more depth textures than the start had, and each return's first swap in seconds after
    it ("-" for none measured), 4 s each unless given."""
    trips = len(feet) - 1
    contexts = contexts or [1 + sum(races[:n + 1]) for n in range(trips + 1)]
    depths = depths or [3 + 2 * sum(races[:n + 1]) for n in range(trips + 1)]
    lines = ["live windows 1 surfaces 1 layers 1 of %d gpu 0.0 foot %.1f" % (n + 1, foot)
             for n, foot in enumerate(feet)]
    gls = ["gl tex=1/0 color=600/0 depth=%d/0 msaa=0/0 mtl=0 ctx=%d" % (depths[n], contexts[n])
           for n in range(trips + 1)]
    knobs = knobs or {}
    args = [str(knobs.get("growth", 32)), str(trips), str(knobs.get("race_depth", 4)),
            str(knobs.get("race_growth", 96)), str(knobs.get("race_foot", 20)), str(knobs.get("race_spike", 48)),
            str(knobs.get("first_swap", 8))]
    swaps = swaps or ["4.0"] * trips
    command = [sys.executable, "-", *args, *[str(r) for r in races], "--", *lines, "--", *gls, "--", *swaps]
    result = subprocess.run(command, input=judge_source(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True)
    return result.returncode, result.stdout


CASES = [
    # (what, footprints from the start, races per trip, expected exit, a text the output must hold)
    ("a device round with the buffer release: +77 at return 2, then down", [4062, 4239, 4316, 4260, 4120],
     [0, 1, 1, 1, 1], 0, "ok "),
    ("a Simulator round with the release: +17 per race, races on half the trips",
     [231, 238, 255, 255, 272, 272, 289, 289, 306], [0, 1, 1, 0, 1, 0, 1, 0, 1], 0, "ok "),
    ("a build without the release: +65 per race", [231, 238, 303, 368, 433], [0, 1, 1, 1, 1], 1,
     "return 3: the footprint 130 MiB above the first return's with 2 race(s)"),
    ("one spike of 77 at the last return, after one race", [231, 238, 315], [0, 1, 1], 0, "ok "),
    ("no race: the footprint holds to RETURNS_GROWTH", [231, 238, 280], [0, 0, 0], 1,
     "return 2: the footprint 42 MiB above the first return's"),
]


# (what, each return's first swap, expected exit, a text the output must hold), on the device round above.
SWAPS = [
    ("each return's first swap is printed", ["4.0", "3.1", "6.2", "5.0"], 0, "return 2  first swap 3.1 s"),
    ("a return whose first swap comes late, as before the SDL fix", ["4.0", "25.5", "5.0", "4.0"], 1,
     "return 2: first swap 25.5 s after the return, over 8 s"),
    ("a swap just at the bound", ["8.0", "4.0", "4.0", "4.0"], 0, "return 1  first swap 8.0 s"),
    ("a return without a measured swap", ["4.0", "-", "4.0", "4.0"], 0, "return 2  first swap n/a"),
]


PONG = "%s 7 pong presents %d scene active surfaces 1 surface frames %d"
# (what, made events, the expected measure)
EVENTS = [
    ("the first pong with a surface frame after the return",
     ["100.000 7 scene willEnterForeground state background", "100.002 7 scene willEnterForeground",
      PONG % ("101.000", 0, 0), PONG % ("104.200", 3, 0), PONG % ("106.300", 6, 3), PONG % ("109.000", 60, 50)],
     "6.3"),
    ("the last return counts, not an earlier one",
     ["50.000 7 scene willEnterForeground", PONG % ("51.000", 9, 4), "90.000 7 scene willEnterForeground",
      PONG % ("92.000", 0, 0), PONG % ("98.150", 4, 1)], "8.2"),
    ("no frame after the return yet", ["90.000 7 scene willEnterForeground", PONG % ("92.000", 3, 0)], "-"),
    ("no return", [PONG % ("92.000", 3, 2)], "-"),
]


def main():
    failed = 0
    for what, events, expected in EVENTS:
        got = first_swap(events)
        if got != expected:
            print("FAIL first_swap, %s: %r (expected %r)" % (what, got, expected))
            failed += 1
    for what, feet, races, expected, text in CASES:
        code, out = run(feet, races)
        if code != expected or text not in out:
            print("FAIL %s: exit %d (expected %d), %r not in:\n%s" % (what, code, expected, text, out))
            failed += 1
    for what, swaps, expected, text in SWAPS:
        code, out = run(CASES[0][1], CASES[0][2], swaps=swaps)
        if code != expected or text not in out:
            print("FAIL %s: exit %d (expected %d), %r not in:\n%s" % (what, code, expected, text, out))
            failed += 1
    if failed:
        return 1
    print("returns judge test: fine (%d rounds, %d event sets)" % (len(CASES) + len(SWAPS), len(EVENTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
