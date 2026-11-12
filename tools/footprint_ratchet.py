#!/usr/bin/env python3
"""The per-return footprint ratchet: given a session log of a background round, it holds the footprint flat across the
returns. It reads Minato's own lines -- BackgroundTrace's per-return line

    I/minato   : trip N back after S s: foot=F0 tex=.. depth=.. before, foot=F1 tex=.. depth=.. 10 s after; ...

and the 1 Hz breakdown

    I/apkrt   : mem foot=.. int=.. comp=.. gfx=.. media=.. purg=.. purgc=.. other=.. ext=..

and FAILS (exit 1) on any of:
  - a return that does not reclaim: foot grows by more than the tolerance from before to 10 s after (F1 - F0 > tol);
  - the per-return baseline ratchets up: a later trip's before-foot exceeds the first trip's by more than the tolerance;
  - textures or depth buffers left orphaned: a later trip's after-count exceeds the first trip's;
  - the un-ledgered GPU remainder climbs: the last `other` sample exceeds the first by more than the tolerance.
A flat run passes (exit 0). A log with no per-return line is a note, not a failure (nothing to judge), like
renpy_footprint.py's no-sample note. The footprint is the device's (iOS TASK_VM_INFO); this reads the recorded numbers,
so it runs on any host. The texture fix removes the bulk of the per-return growth; this ratchet keeps it that way.

Runs on /usr/bin/python3 (3.9).
Usage: tools/footprint_ratchet.py <session-log> [<tolerance MiB, default 32>]
"""
import re
import sys

BACK = re.compile(
    r"trip (\d+) back after [\d.]+ s: "
    r"foot=(\d+)\S* tex=(\d+)(?:/\d+)? depth=(\d+)(?:/\d+)? before, "
    r"foot=(\d+)\S* tex=(\d+)(?:/\d+)? depth=(\d+)(?:/\d+)? 10 s after")
MEM = re.compile(r"\bI[/ ]apkrt\b.*\bmem foot=(\d+)")
OTHER = re.compile(r"\bother=(\d+)")


class Trip(object):
    __slots__ = ("n", "fb", "tb", "db", "fa", "ta", "da")

    def __init__(self, n, fb, tb, db, fa, ta, da):
        self.n, self.fb, self.tb, self.db, self.fa, self.ta, self.da = n, fb, tb, db, fa, ta, da


def parse(path):
    trips, others = [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = BACK.search(line)
            if m and ("minato" in line):
                g = [int(x) for x in m.groups()]
                trips.append(Trip(g[0], g[1], g[2], g[3], g[4], g[5], g[6]))
                continue
            if MEM.search(line):
                o = OTHER.search(line)
                if o:
                    others.append(int(o.group(1)))
    trips.sort(key=lambda t: t.n)
    return trips, others


def breaches(trips, others, tol):
    out = []
    for t in trips:
        if t.fa - t.fb > tol:
            out.append("trip %d: the return did not reclaim -- foot grew %+d MiB (%d -> %d), over the %d MiB tolerance"
                       % (t.n, t.fa - t.fb, t.fb, t.fa, tol))
    if trips:
        base = trips[0]
        for t in trips[1:]:
            if t.fb - base.fb > tol:
                out.append("trip %d: the per-return baseline climbed %+d MiB since trip %d (%d -> %d) -- the ratchet"
                           % (t.n, t.fb - base.fb, base.n, base.fb, t.fb))
            if t.ta > base.ta:
                out.append("trip %d: textures left orphaned by the return -- tex %d -> %d since trip %d"
                           % (t.n, base.ta, t.ta, base.n))
            if t.da > base.da:
                out.append("trip %d: depth buffers left orphaned by the return -- depth %d -> %d since trip %d"
                           % (t.n, base.da, t.da, base.n))
    if len(others) >= 2 and others[-1] - others[0] > tol:
        out.append("the un-ledgered GPU remainder climbed %+d MiB over the run (other %d -> %d) -- the jetsam risk"
                   % (others[-1] - others[0], others[0], others[-1]))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    path = argv[1]
    tol = int(argv[2]) if len(argv) > 2 else 32
    try:
        trips, others = parse(path)
    except OSError as exc:
        print("note  footprint-ratchet: cannot read %s (%s)" % (path, exc))
        return 0
    if not trips:
        print("note  footprint-ratchet: no per-return line in the log (nothing to judge)")
        return 0
    print("footprint ratchet: %d returns, tolerance %d MiB" % (len(trips), tol))
    for t in trips:
        print("  trip %d: foot %d -> %d (%+d)   tex %d -> %d   depth %d -> %d"
              % (t.n, t.fb, t.fa, t.fa - t.fb, t.tb, t.ta, t.db, t.da))
    if others:
        print("  other (un-ledgered GPU): %d -> %d MiB" % (others[0], others[-1]))
    found = breaches(trips, others, tol)
    if found:
        print("RATCHET BREACH (%d):" % len(found))
        for b in found:
            print("  - " + b)
        return 1
    print("ratchet held: footprint flat per return (<= %d MiB), baseline steady, tex/depth flat, other not climbing"
          % tol)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
