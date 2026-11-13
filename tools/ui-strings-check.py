#!/usr/bin/env python3
"""Checks the text Minato shows: no dashes, never the word "game" (the app's wording is "app"), no decision numbers
and, with a names file, none of its names. The text is every string literal of app/Sources that SwiftUI puts on
screen (Text, Label, Button, Toggle, alerts and the other views and modifiers in VISIBLE, with the literals nested
in their interpolations), every literal of app/Sources/Library/Texts.swift (the user-facing wording), the Info.plist
usage descriptions in project.yml and any Localizable .strings or .xcstrings file under app/. Comments and other
literals (log lines, keys) are not text on screen. One line per finding, "<file>:<line>: <rule>: <text>"; exit 0
when there is none, 1 when there is one.
--names FILE: one regular expression per line (blank lines and lines starting with # are not patterns), each an
internal name that must not show; the file is kept outside the tree. Without it that rule is skipped with a note.
Usage: tools/ui-strings-check.py [--names FILE] [tree root, default: this tree]"""
import json
import os
import re
import sys

VISIBLE = re.compile(
    r"(?:\b(?:Text|Label|Button|Toggle|Section|Picker|Menu|LabeledContent|TextField|SecureField|Link|"
    r"ContentUnavailableView|Stepper|NavigationLink|ShareLink|GroupBox|DisclosureGroup|Tab)\s*\(|"
    r"\.(?:alert|confirmationDialog|navigationTitle|help|accessibilityLabel|accessibilityHint|badge|searchable)"
    r"\s*\()\s*(?:\w+:\s*)?$")
TEXTS_FILE = "app/Sources/Library/Texts.swift"
# The names of Google's products are theirs (a Play dependency in Get Info).
ALLOWED = [re.compile(r"Google Play Games")]
# A dash inside text (an em or en dash, a spaced hyphen or a double hyphen; a lone dash is the placeholder of an
# empty value, as in Apple's own lists), the word game, a decision's number.
RULES = [
    ("dash", re.compile("(?=.*\\w).*(?:[—–]| - |--)")),
    ("game", re.compile(r"\bgames?\b", re.IGNORECASE)),
    ("decision number", re.compile(r"\bADR[- ]?\d+")),
]


def names_rule(path):
    """The names file's patterns as one rule."""
    patterns = [row.strip() for row in open(path, encoding="utf-8")
                if row.strip() and not row.strip().startswith("#")]
    return ("internal name", re.compile("|".join("(?:%s)" % pattern for pattern in patterns)))


def literals(source):
    """The string literals of Swift source with the line each starts on and the code before it on that line, the
    literals inside an interpolation after their enclosing one with that one's visibility. Comments are skipped."""
    found = []
    i, n, line = 0, len(source), 1

    def read_string(start, line_no, before, parent_visible):
        # source[start] is the opening quote (one or three); returns the index after the closing quote and the line.
        triple = source.startswith('"""', start)
        j = start + (3 if triple else 1)
        text, nested, ln = [], [], line_no
        while j < n:
            c = source[j]
            if triple and source.startswith('"""', j):
                j += 3
                break
            if not triple and c == '"':
                j += 1
                break
            if c == "\n":
                ln += 1
            if c == "\\" and j + 1 < n and source[j + 1] == "(":
                # An interpolation: code up to its closing parenthesis, with literals of its own.
                depth, k = 1, j + 2
                while k < n and depth:
                    d = source[k]
                    if d == '"':
                        inner_before = source[source.rfind("\n", 0, k) + 1:k]
                        k, ln = read_string(k, ln, inner_before, None)
                        nested.append(len(found) - 1)
                        continue
                    if d == "\n":
                        ln += 1
                    depth += 1 if d == "(" else -1 if d == ")" else 0
                    k += 1
                text.append("{}")
                j = k
                continue
            if c == "\\" and j + 1 < n:
                text.append(source[j:j + 2])
                j += 2
                continue
            text.append(c)
            j += 1
        visible = parent_visible if parent_visible is not None else bool(VISIBLE.search(before))
        found.append({"line": line_no, "text": "".join(text), "visible": visible})
        for index in nested:  # the nested literals take the enclosing literal's visibility
            found[index]["visible"] = visible
        return j, ln

    while i < n:
        c = source[i]
        if source.startswith("//", i):
            end = source.find("\n", i)
            i = n if end < 0 else end
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            line += source.count("\n", i, end)
            i = end
            continue
        if c == '"':
            before = source[source.rfind("\n", 0, i) + 1:i]
            i, line = read_string(i, line, before, None)
            continue
        if c == "\n":
            line += 1
        i += 1
    return found


def usage_descriptions(text):
    """The NS...UsageDescription values of project.yml, a folded block (>-) joined, with their lines."""
    out, lines = [], text.split("\n")
    for index, row in enumerate(lines):
        m = re.match(r"^(\s*)NS\w+UsageDescription:\s*(.*)$", row)
        if not m:
            continue
        value = m.group(2).strip()
        if value in (">-", ">", "|", "|-"):
            parts, k = [], index + 1
            while k < len(lines) and (not lines[k].strip() or len(lines[k]) - len(lines[k].lstrip()) > len(m.group(1))):
                parts.append(lines[k].strip())
                k += 1
            value = " ".join(p for p in parts if p)
        out.append((index + 1, value.strip("'\"")))
    return out


def localizable(path):
    text = open(path, encoding="utf-8").read()
    if path.endswith(".xcstrings"):
        values = []

        def walk(node):
            if isinstance(node, dict):
                if isinstance(node.get("value"), str):
                    values.append((1, node["value"]))
                for child in node.values():
                    walk(child)
            elif isinstance(node, list):
                for child in node:
                    walk(child)

        walk(json.loads(text))
        return values
    return [(text.count("\n", 0, m.start()) + 1, m.group(1))
            for m in re.finditer(r'^\s*"(?:[^"\\]|\\.)*"\s*=\s*"((?:[^"\\]|\\.)*)"\s*;', text, re.MULTILINE)]


def findings(root, rules):
    out = []

    def judge(rel, line, text):
        shown = text
        for allowed in ALLOWED:
            shown = allowed.sub("", shown)
        for name, rule in rules:
            if rule.search(shown):
                out.append("%s:%d: %s: %s" % (rel, line, name, text))

    sources = os.path.join(root, "app", "Sources")
    for folder, _, files in os.walk(sources):
        for name in sorted(files):
            if not name.endswith(".swift"):
                continue
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, root)
            whole = rel == TEXTS_FILE
            for literal in literals(open(path, encoding="utf-8").read()):
                if whole or literal["visible"]:
                    judge(rel, literal["line"], literal["text"])
    project = os.path.join(root, "project.yml")
    if os.path.exists(project):
        for line, value in usage_descriptions(open(project, encoding="utf-8").read()):
            judge("project.yml", line, value)
    for folder, _, files in os.walk(os.path.join(root, "app")):
        for name in sorted(files):
            if name.endswith(".strings") or name.endswith(".xcstrings"):
                path = os.path.join(folder, name)
                for line, value in localizable(path):
                    judge(os.path.relpath(path, root), line, value)
    return out


def main():
    args, rules = sys.argv[1:], list(RULES)
    if args[:1] == ["--names"]:
        if len(args) < 2 or not os.path.isfile(args[1]):
            print("ui-strings-check: --names needs a file")
            return 2
        rules.append(names_rule(args[1]))
        args = args[2:]
    else:
        print("ui-strings-check: note: no names file, the internal-name rule is skipped")
    root = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    found = findings(os.path.abspath(root), rules)
    for row in found:
        print(row)
    print("ui-strings-check: %s" % ("%d finding%s" % (len(found), "" if len(found) == 1 else "s") if found else "OK"))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
