#!/usr/bin/env python3
"""Tests tools/ui-strings-check.py on fake trees: a clean one is OK; each rule (a dash, the word game, a decision's
number) is red in a SwiftUI text, a nested literal of an interpolation, an Info.plist usage description and a
Localizable file; text that is not on screen (a log line, a comment), the allowed product name and a lone dash as an
empty value's placeholder stay OK. The names rule reads its patterns from a --names file: made-up names here, red
with the file, skipped with a note without it."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "ui-strings-check.py")

CLEAN_VIEW = '''import SwiftUI

struct LibraryView: View {
    let count: Int
    var body: some View {
        // A comment may say game or ADR-0032 — it is not on screen.
        Text("\\(count) \\(count == 1 ? "app" : "apps")")
        Label("Get Info", systemImage: "info.circle")
        Button("Delete App", role: .destructive) {}
            .alert("Delete this app?", isPresented: .constant(false)) {}
        Text("Needs Google Play Games, which Minato reports as absent.")
        Text("Wi-Fi and x86-64 are words, not dashes.")
        Text("–")
    }
    func log() { logger.info("game mode started, see ADR-0032 — not on screen") }
}
'''

CLEAN_TEXTS = '''enum Texts {
    static let unknown = "Unknown reason"
}
'''

CLEAN_PROJECT = '''targets:
  Minato:
    info:
      properties:
        NSCameraUsageDescription: Lets apps you run in Minato use the camera, only when you allow it for that app.
        NSLocationWhenInUseUsageDescription: >-
          Apps you run in Minato can use your location
          when you allow it for that app.
'''

# Each mutant: (what it shows, file, old text, new text). Every one must make the check red.
MUTANTS = [
    ("an em dash in a Text", "app/Sources/Views/LibraryView.swift", '"Get Info"', '"Get Info — now"'),
    ("an en dash in a Button", "app/Sources/Views/LibraryView.swift", '"Delete App"', '"Delete – App"'),
    ("a spaced hyphen as a dash in an alert", "app/Sources/Views/LibraryView.swift", '"Delete this app?"',
     '"Delete - this app?"'),
    ("an ADR number", "app/Sources/Views/LibraryView.swift", '"Get Info"', '"Get Info, see ADR-0032"'),
    ("the word game in a Label", "app/Sources/Views/LibraryView.swift", '"Get Info"', '"Game Info"'),
    ("games in a nested literal of an interpolation", "app/Sources/Views/LibraryView.swift", '"apps"', '"games"'),
    ("game in the texts file", "app/Sources/Library/Texts.swift", '"Unknown reason"', '"Unknown game"'),
    ("game in a usage description", "project.yml", "Lets apps you run", "Lets games you run"),
    ("a dash in a folded usage description", "project.yml", "can use your location",
     "can use your location — always"),
    ("a dash in a Localizable file", "app/Resources/Localizable.strings", '"Open"', '"Open — now"'),
]


def tree(root):
    files = {
        "app/Sources/Views/LibraryView.swift": CLEAN_VIEW,
        "app/Sources/Library/Texts.swift": CLEAN_TEXTS,
        "project.yml": CLEAN_PROJECT,
        "app/Resources/Localizable.strings": '"open.button" = "Open";\n',
    }
    for path, text in files.items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as out:
            out.write(text)


def run(root, names=None):
    command = [sys.executable, CHECK] + (["--names", names] if names else []) + [root]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return result.returncode, result.stdout


def names_rule(scratch):
    """The names rule with made-up names: a clean tree is OK with the file; a name on screen is red with it and
    skipped, with a note, without it; a pattern file's comments and blank lines are not patterns."""
    failed = 0
    names = os.path.join(scratch, "names.txt")
    with open(names, "w") as out:
        out.write("# made-up internal names, one pattern per line\n\n\\bZebra\\b\n\\bQ7\\b\n")
    clean = os.path.join(scratch, "names-clean")
    tree(clean)
    code, out = run(clean, names)
    if code != 0:
        print("FAIL the clean tree with a names file is not OK (exit %d):\n%s" % (code, out))
        failed += 1
    for index, name in enumerate(["Zebra", "Q7"]):
        root = os.path.join(scratch, "names-%d" % index)
        tree(root)
        path = os.path.join(root, "app/Sources/Views/LibraryView.swift")
        text = open(path).read()
        with open(path, "w") as out:
            out.write(text.replace('"Get Info"', '"Get Info (%s)"' % name, 1))
        code, out = run(root, names)
        if code != 1 or "internal name" not in out:
            print("FAIL the made-up name %s with the names file: exit %d, expected 1:\n%s" % (name, code, out))
            failed += 1
        code, out = run(root)
        if code != 0 or "note: no names file" not in out:
            print("FAIL the made-up name %s without the names file: exit %d, expected 0 and the note:\n%s"
                  % (name, code, out))
            failed += 1
    return failed


def main():
    failed = 0
    with tempfile.TemporaryDirectory() as scratch:
        clean = os.path.join(scratch, "clean")
        tree(clean)
        code, out = run(clean)
        if code != 0:
            print("FAIL the clean tree is not OK (exit %d):\n%s" % (code, out))
            failed += 1
        for index, (what, path, old, new) in enumerate(MUTANTS):
            root = os.path.join(scratch, "m%d" % index)
            tree(root)
            full = os.path.join(root, path)
            text = open(full).read()
            if old not in text:
                print("FAIL the mutant '%s' finds no %r in %s" % (what, old, path))
                failed += 1
                continue
            with open(full, "w") as out:
                out.write(text.replace(old, new, 1))
            code, out = run(root)
            if code != 1 or path not in out:
                print("FAIL %s: exit %d, expected 1 and a line naming %s:\n%s" % (what, code, path, out))
                failed += 1
        failed += names_rule(scratch)
    if failed:
        return 1
    print("ui-strings-check test: fine (%d mutants red, the clean tree OK, the names rule red with its file and "
          "skipped without it)" % len(MUTANTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
