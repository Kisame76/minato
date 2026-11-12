#!/usr/bin/env bash
# Checks that what README.md and docs/*.md point at exists in the tree:
#   link    a relative link `[text](target)`: the file or folder, relative to the page
#   anchor  the `#heading` of a link, on the page itself or on the md file it names (GitHub's heading slugs)
#   path    a repository path in code quotes or a code block (app/, importer/, runtime/, tools/, docs/, fixtures/,
#           framework/, graphics/, third_party/, .github/); globs, placeholders and variables are skipped, and so are
#           the paths that .gitignore lists (generated files) and a line that names an external repository (github.com)
#   script  a bare script name in code quotes (`name.sh`, `name.py`): some file of the tree has that name, or the page
#           names it under build/ (generated, not versioned)
#   ADR     an ADR-NNNN number: docs/DESIGN.md has its heading `### ADR-NNNN` (docs/DESIGN.md itself is not checked)
# External URLs are only listed (URL), never fetched. Prints WRONG lines (file:line, the kind, the target) and a total;
# exit 1 when any is WRONG, else 0. No network, read only. Python 3.9 (/usr/bin/python3) is enough.
# Usage: tools/docs-paths-check.sh [tree]   (the repo root, default: the one this script lives in)
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
[[ -d "$root" ]] || { echo "usage: $0 [tree]" >&2; exit 2; }
exec /usr/bin/python3 - "$root" <<'PYTHON'
import glob
import os
import re
import sys

ROOT = sys.argv[1]
TOP = r"(?:app|importer|runtime|tools|docs|fixtures|framework|graphics|third_party|\.github)"
PATH_RX = re.compile(r"(?<![\w/.\-~$])(" + TOP + r"/[A-Za-z0-9_./+\-]*[A-Za-z0-9_+/\-])")
SCRIPT_RX = re.compile(r"(?<![\w/.\-*>}])([A-Za-z0-9_\-]+\.(?:sh|py))(?![\w/.\-])")
LINK_RX = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
URL_RX = re.compile(r"https?://[^\s)>\]`\"']+")
ADR_RX = re.compile(r"ADR-(\d{4})")
CODE_RX = re.compile(r"`([^`\n]+)`")
SKIP = set("*<>{}$|=;,") | {"…"}

wrong, urls, checked = [], [], 0


def slug(heading):
    text = re.sub(r"[`*_]", "", heading.strip().lower())
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def headings(path):
    found, fenced = set(), False
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    except OSError:
        return found
    for line in lines:
        if line.startswith("```"):
            fenced = not fenced
        elif not fenced:
            match = re.match(r"#{1,6}\s+(.*)", line)
            if match:
                found.add(slug(re.sub(r"\s+\{#.*\}$", "", match.group(1))))
    return found


names = None


def have_name(name):
    global names
    if names is None:
        names = set()
        for folder, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in (".git", "build", "DerivedData", "node_modules")]
            names.update(files)
    return name in names


ignored = set()
if os.path.isfile(os.path.join(ROOT, ".gitignore")):
    for entry in open(os.path.join(ROOT, ".gitignore"), encoding="utf-8").read().split("\n"):
        entry = entry.strip().lstrip("/")
        if entry and not entry.startswith("#") and not any(c in entry for c in "*?["):
            ignored.add(entry.rstrip("/"))


def is_ignored(target):
    parts = target.rstrip("/").split("/")
    return any("/".join(parts[:k]) in ignored for k in range(1, len(parts) + 1))


design = os.path.join(ROOT, "docs", "DESIGN.md")
listed = None
if os.path.isfile(design):
    listed = set(re.findall(r"^### ADR-(\d{4})", open(design, encoding="utf-8").read(), re.M))
docs = sorted(os.path.relpath(f, ROOT) for f in glob.glob(os.path.join(ROOT, "docs", "*.md")))
pages = [p for p in ["README.md"] + docs if os.path.isfile(os.path.join(ROOT, p))]
for page in pages:
    here = os.path.dirname(os.path.join(ROOT, page))
    own = headings(os.path.join(ROOT, page))
    fenced = False
    text = open(os.path.join(ROOT, page), encoding="utf-8", errors="replace").read()
    generated = {os.path.basename(t) for t in re.findall(r"`(build/[^`\s]+)`", text)}  # named under build/
    for number, line in enumerate(text.split("\n"), 1):
        where = "%s:%d" % (page, number)
        if line.startswith("```"):
            fenced = not fenced
            continue
        code = [line] if fenced else CODE_RX.findall(line)
        prose = "" if fenced else CODE_RX.sub(" ", line)
        for url in URL_RX.findall(line):
            urls.append((where, url.rstrip(".,;:")))
        for target in LINK_RX.findall(prose):
            if re.match(r"(?:https?:|mailto:)", target):
                continue
            checked += 1
            file_part, _, fragment = target.partition("#")
            if file_part:
                resolved = os.path.normpath(os.path.join(here, file_part))
                if not os.path.exists(resolved):
                    wrong.append((where, "link", target))
                    continue
                if fragment and resolved.endswith(".md") and slug(fragment) not in headings(resolved):
                    wrong.append((where, "anchor", target))
            elif fragment and slug(fragment) not in own:
                wrong.append((where, "anchor", "#" + fragment))
        if page != os.path.join("docs", "DESIGN.md") and listed is not None:
            for adr in ADR_RX.findall(line):
                checked += 1
                if adr not in listed:
                    wrong.append((where, "ADR", "ADR-" + adr))
        for snippet in code:
            for match in PATH_RX.finditer(snippet):
                end = match.end()
                if end < len(snippet) and snippet[end] in SKIP:
                    continue
                target = re.sub(r":\d+$", "", match.group(1))
                if any(c in SKIP for c in target):
                    continue
                if re.search(r"(?:github|gitlab)\.com/", line) or is_ignored(target):
                    continue
                checked += 1
                if not os.path.exists(os.path.join(ROOT, target)):
                    wrong.append((where, "path", target))
            if not fenced:
                for match in SCRIPT_RX.finditer(snippet):
                    if "/" in snippet[max(0, match.start() - 1):match.start() + 1]:
                        continue
                    if match.group(1) in generated:
                        continue
                    checked += 1
                    if not have_name(match.group(1)):
                        wrong.append((where, "script", match.group(1)))

seen = set()
for where, kind, target in wrong:
    if (where, kind, target) not in seen:
        seen.add((where, kind, target))
        print("WRONG  %-28s %-6s %s" % (where, kind, target))
for where, url in urls:
    print("URL    %-28s %s" % (where, url))
print("%d references checked, %d WRONG, %d URLs listed" % (checked, len(seen), len(urls)))
sys.exit(1 if seen else 0)
PYTHON
