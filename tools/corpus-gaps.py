#!/usr/bin/env python3
"""The gap table of the corpus (docs/plans/2026-10-07-corpus-harness.md): reads the JSONs of tools/corpus-run.py's
runs and maps every finding to a gap of the compatibility plan's section 2 by fixtures/corpus/gap-patterns.txt, then
writes docs/corpus/gaps.md: per gap, the apps it stops first and the apps it hits at all, the most first; the findings
no pattern maps, by kind; and the apps with their levels. Each run rewrites it, so section 2's order follows the data.
Python 3.9.

Usage: corpus-gaps.py <out folder>... [--table <gaps.md>] [--patterns <gap-patterns.txt>]
"""
import collections
import glob
import json
import os
import re
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("MINATO_REPO") or os.path.dirname(TOOLS)
LEVELS = ["imports", "starts", "frame", "audio", "network", "lives"]


def read_patterns(path):
    """(gap, title, pattern) from lines "<gap>\\t<title>\\t<regular expression>"; # starts a comment."""
    patterns = []
    with open(path) as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            gap, title, pattern = line.split("\t", 2)
            patterns.append((gap, title, re.compile(pattern)))
    return patterns


def gap_of(patterns, kind, text):
    for gap, title, pattern in patterns:
        if pattern.search(f"{kind}: {text}"):
            return gap, title
    return None


def main():
    arguments = sys.argv[1:]
    table = os.path.join(REPO, "docs", "corpus", "gaps.md")
    patterns_path = os.path.join(REPO, "fixtures", "corpus", "gap-patterns.txt")
    folders = []
    while arguments:
        if arguments[0] == "--table":
            table, arguments = arguments[1], arguments[2:]
        elif arguments[0] == "--patterns":
            patterns_path, arguments = arguments[1], arguments[2:]
        else:
            folders.append(arguments.pop(0))
    if not folders:
        raise SystemExit(__doc__.split("Usage: ")[1].strip())
    patterns = read_patterns(patterns_path)

    apps = []
    for folder in folders:
        for path in sorted(glob.glob(os.path.join(folder, "*.json"))):
            with open(path) as handle:
                record = json.load(handle)
            if "levels" in record:
                apps.append(record)

    titles = {gap: title for gap, title, _ in patterns}
    first = collections.defaultdict(set)
    hits = collections.defaultdict(set)
    unmapped = collections.defaultdict(lambda: collections.defaultdict(set))
    for app in apps:
        name = app.get("package") or os.path.basename(app["apk"])
        for kind, texts in app.get("findings", {}).items():
            for text in texts:
                mapped = gap_of(patterns, kind, text)
                if mapped:
                    hits[mapped[0]].add(name)
                else:
                    unmapped[kind][text].add(name)
        failure = app.get("first_failure")
        if failure:
            mapped = gap_of(patterns, failure["kind"], failure["text"])
            if mapped:
                first[mapped[0]].add(name)

    lines = ["# The corpus's gaps", "",
             f"Written by tools/corpus-gaps.py from {len(apps)} apps' runs (tools/corpus-run.py); the gaps are",
             "those of the [compatibility plan](../plans/2026-10-07-compatibility-plan.md)'s section 2, found by",
             "fixtures/corpus/gap-patterns.txt. Do not edit by hand.", "",
             "## Gaps, the most apps stopped first", "",
             "| Gap | Stops first | Hits | Apps |", "|---|---|---|---|"]
    for gap in sorted(set(first) | set(hits), key=lambda g: (-len(first[g]), -len(hits[g]), g)):
        lines.append(f"| {gap}. {titles.get(gap, '')} | {len(first[gap])} | {len(hits[gap])} | "
                     f"{', '.join(sorted(hits[gap] | first[gap]))} |")
    lines += ["", "## Findings no pattern maps yet", ""]
    if not unmapped:
        lines.append("None.")
    for kind in sorted(unmapped):
        lines.append(f"- *{kind}:*")
        for text, names in sorted(unmapped[kind].items(), key=lambda item: (-len(item[1]), item[0]))[:40]:
            lines.append(f"  - `{text[:160]}`: {len(names)} ({', '.join(sorted(names))})")
    lines += ["", "## Apps", "", "| App | " + " | ".join(LEVELS) + " | First failure |",
              "|---|" + "---|" * (len(LEVELS) + 1)]
    for app in sorted(apps, key=lambda a: a.get("package") or a["apk"]):
        marks = [{True: "yes", False: "no", None: "-"}[app["levels"].get(level)] for level in LEVELS]
        failure = app.get("first_failure")
        shown = f"{failure['kind']}: `{failure['text'][:100]}`" if failure else ""
        lines.append(f"| {app.get('package') or app['apk']} | " + " | ".join(marks) + f" | {shown} |")
    os.makedirs(os.path.dirname(table), exist_ok=True)
    with open(table, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"{table}: {len(apps)} apps, {len(set(first) | set(hits))} gaps, "
          f"{sum(len(texts) for texts in unmapped.values())} findings unmapped")


if __name__ == "__main__":
    main()
