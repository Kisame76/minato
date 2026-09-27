#!/usr/bin/env python3
"""Checks that a built Minato.app carries the license texts of everything it embeds.

THIRD_PARTY.md lists what Minato adopts, in one table or more per section. third_party/licenses.json says
for each such section when the app embeds it: always, with a framework in the bundle, or never. For every
table row of an embedded section, the bundle's Licenses/licenses.json must name a component that covers
it, and each of that component's texts must be in Licenses/ and not empty. A component of
third_party/licenses.json may instead give the reason why nothing of it is in the binary.

Usage: tools/license-check.py <path to Minato.app> [...]
"""
import json
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))


def sections(markdown):
    """{section heading: [first cell of each table row]} of THIRD_PARTY.md, without the header rows."""
    found = {}
    heading = None
    previous = ""
    for line in markdown.splitlines():
        if line.startswith("## "):
            heading = line[3:].strip()
        elif heading and line.strip().startswith("|"):
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            separator = all(set(cell) <= set("-: ") for cell in cells)
            header = not previous.strip().startswith("|")
            if not separator and not header:
                found.setdefault(heading, []).append(cells[0].replace("`", ""))
        previous = line
    return found


def check(app, config, markdown):
    """The problems of one bundle, as sentences."""
    embedding = config["sections"]
    exempt = {row for component in config["components"] if component.get("none") for row in component["rows"]}
    problems = []
    index_path = os.path.join(app, "Licenses", "licenses.json")
    if not os.path.isfile(index_path):
        return [f"{app} has no Licenses/licenses.json"]
    with open(index_path) as handle:
        components = json.load(handle)["components"]
    frameworks = os.path.join(app, "Frameworks")
    for heading, rows in sections(markdown).items():
        rule = next((value for key, value in embedding.items() if heading.startswith(key)), None)
        if rule is None:
            problems.append(f"THIRD_PARTY.md's section '{heading}' has tables but no rule in "
                            "third_party/licenses.json's 'sections'")
            continue
        if rule == "never" or (rule != "always" and not os.path.isdir(os.path.join(frameworks, rule))):
            continue
        for row in rows:
            if row in exempt:
                continue
            covering = [component for component in components if row in component["rows"]]
            if not covering:
                problems.append(f"'{row}' ({heading}) has no license text in the bundle")
            for component in covering:
                if not component["files"]:
                    problems.append(f"'{row}': {component['name']} lists no text")
                for name in component["files"]:
                    path = os.path.join(app, "Licenses", component["name"], name)
                    if not os.path.isfile(path) or os.path.getsize(path) == 0:
                        problems.append(f"'{row}': Licenses/{component['name']}/{name} is missing or empty")
    return problems


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    with open(os.path.join(ROOT, "third_party", "licenses.json")) as handle:
        config = json.load(handle)
    with open(os.path.join(ROOT, "THIRD_PARTY.md")) as handle:
        markdown = handle.read()
    failed = False
    for app in sys.argv[1:]:
        problems = check(app, config, markdown)
        for problem in problems:
            print(f"FAIL {problem}")
        embedded = sorted(os.listdir(os.path.join(app, "Frameworks"))) if os.path.isdir(os.path.join(app, "Frameworks")) else []
        print(f"{'FAIL' if problems else 'ok'}: {app} (frameworks: {', '.join(embedded) or 'none'})")
        failed = failed or bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
