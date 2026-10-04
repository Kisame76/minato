#!/usr/bin/env python3
"""tools/stub_supertypes.py, framework-check's guard (ADR-0039 b3.4): a class of framework.jar on a supertype that
neither framework.jar nor the boot jars hold is "unloadable" unless framework/stub-supertypes.txt lists it, and a
listed pair that no longer holds is "stale". Each entry says why it is there (b3.5a, the lead's rule): the step that
removes it ("step c: ...", "b3.5b: ..."), "stays: <reason>", the reason why the class never loads on Minato's path, or
"gap: <what>", an app's path that fails today without a step yet (step c2c3). An entry with none of them, or "stays"
or "gap" without its text, is "unexplained". Run on files in a scratch directory."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "stub_supertypes.py")
failures = 0

# javap's class lines: one class on a stub supertype each, one on a framework.jar class, and a generic one whose
# type parameter's bound names itself (step c2c3's DynamicAnimation<T extends DynamicAnimation<T>>), which stands on
# framework.jar classes only.
JAVAP = """Compiled from "A.java"
public class a.One extends s.Stub {
public class a.Two implements s.Listener {
public class a.Three extends a.One {
public abstract class a.Four<T extends a.Four<T>> extends a.Three implements a.Five<T> {
"""
FRAMEWORK = "a/One a/Two a/Three a/Four a/Five\n"
BOOT = "java/lang/Object\n"


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def run(allowlist):
    with tempfile.TemporaryDirectory() as scratch:
        paths = []
        for name, text in (("framework.txt", FRAMEWORK), ("boot.txt", BOOT), ("allowlist.txt", allowlist)):
            paths.append(os.path.join(scratch, name))
            with open(paths[-1], "w") as handle:
                handle.write(text)
        result = subprocess.run([sys.executable, TOOL, *paths], input=JAVAP, capture_output=True, text=True)
        check(result.returncode == 0, f"exit {result.returncode}: {result.stderr}")
        return result.stdout.splitlines()


HEADER = "# A comment line.\n# <class> -> <supertype>  # <the step that removes it, or stays: <reason>>\n"
ONE = "a.One -> s.Stub"
TWO = "a.Two -> s.Listener"

lines = run(HEADER + f"{ONE}  # step c: our Stub (decision 10)\n{TWO}  # b3.5b: ours overrides it\n")
check(lines == [], f"steps: {lines}")

lines = run(HEADER + f"{ONE}  # step c: our Stub\n")
check(lines == [f"unloadable {TWO}"], f"unlisted: {lines}")

lines = run(HEADER + f"{ONE}  # step c\n{TWO}  # step c\na.Gone -> s.Stub  # step c\n")
check(lines == ["stale a.Gone -> s.Stub"], f"stale step: {lines}")

lines = run(HEADER + f"{ONE}  # step c\n{TWO}  # stays: ours overrides the only method that makes it\n")
check(lines == [], f"stays with a reason: {lines}")

lines = run(HEADER + f"{ONE}  # step c\n{TWO}  # gap: an app's own loader that watches a content URI\n")
check(lines == [], f"gap with what: {lines}")

lines = run(HEADER + f"{ONE}  # step c\n{TWO}  # stays: never loads\na.Gone -> s.Stub  # stays: never loads\n")
check(lines == ["stale a.Gone -> s.Stub"], f"stale stays: {lines}")

for comment in ("  # stays", "  # stays:", "  # stays:   ", "", "  # whatever", "  # steps c", "  # gap", "  # gap:"):
    lines = run(HEADER + f"{ONE}  # step c\n{TWO}{comment}\n")
    check(lines == [f"unexplained {TWO}"], f"entry '{comment}': {lines}")

# The real list explains every entry.
with open(os.path.join(ROOT, "framework", "stub-supertypes.txt")) as handle:
    real = [line for line in handle if line.strip() and not line.startswith("#")]
check(real and all("# step " in line or "# b3." in line or "# stays: " in line or "# gap: " in line for line in real),
      "framework/stub-supertypes.txt: an entry without its step or reason")

if failures:
    print(f"stub_supertypes_test: {failures} failed")
    sys.exit(1)
print("stub_supertypes_test: fine")
