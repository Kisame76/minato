#!/usr/bin/env python3
"""tools/signature_types.py, framework-check's guard of member signatures (retest 8's batch B3): a field's type, a
method's parameter or its return type that names a class neither framework.jar nor the boot jars hold, a compile-only
stub or an absent type, makes ART's getDeclaredMethods, getDeclaredFields and getDeclaredConstructors of that class throw
NoClassDefFoundError, and with them every library that reflects over a Context, an Activity or a View. Such a pair is
"unlisted" unless framework/signature-gaps.txt lists it for the top-level class or for "*"; a listed pair that no
longer holds is "stale"; a line without its why (a step, "stays: <reason>" or "gap: <what>") is "unexplained". Run on
files in a scratch directory."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "signature_types.py")
failures = 0

# javap -p -s's output: a.One names the types s.* in one member each, an array and a nested type's name among them;
# a.One$Inner, an inner class, names s.Inner; a.Two only classes that exist (framework.jar, the boot jars, primitives,
# arrays of both).
JAVAP = """Compiled from "One.java"
public class a.One<T, U> extends java.lang.Object implements java.lang.Runnable {
  private s.Field field;
    descriptor: Ls/Field;
  private final s.Fields[][] grid;
    descriptor: [[Ls/Fields;
  public a.One(s.Constructed);
    descriptor: (Ls/Constructed;)V
  public void run();
    descriptor: ()V
  public s.Returned make();
    descriptor: ()Ls/Returned;
  public static void take(int, s.Parameter, java.lang.String[], s.Parameters[]);
    descriptor: (ILs/Parameter;[Ljava/lang/String;[Ls/Parameters;)V
  void nested(s.Outer$Nested);
    descriptor: (Ls/Outer$Nested;)V
}
Compiled from "One.java"
class a.One$Inner implements java.lang.Runnable {
  public void run(s.Inner);
    descriptor: (Ls/Inner;)V
}
Compiled from "Two.java"
public final class a.Two {
  a.One one;
    descriptor: La/One;
  void use(java.lang.String, long[], a.One, a.Two$Part);
    descriptor: (Ljava/lang/String;[JLa/One;La/Two$Part;)Ljava/lang/Runnable;
}
"""
FRAMEWORK = "a/One a/One$Inner a/Two a/Two$Part\n"
BOOT = "java/lang/Object java/lang/Runnable java/lang/String\n"
STUBS = ["s.Field", "s.Fields", "s.Constructed", "s.Returned", "s.Parameter", "s.Parameters", "s.Outer$Nested"]


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def run(allowlist, javap=JAVAP, extra=()):
    with tempfile.TemporaryDirectory() as scratch:
        paths = []
        for name, text in (("framework.txt", FRAMEWORK), ("boot.txt", BOOT), ("allowlist.txt", allowlist)):
            paths.append(os.path.join(scratch, name))
            with open(paths[-1], "w") as handle:
                handle.write(text)
        result = subprocess.run([sys.executable, TOOL, *paths, *extra], input=javap, capture_output=True, text=True)
        check(result.returncode == 0, f"exit {result.returncode}: {result.stderr}")
        return result.stdout.splitlines()


HEADER = "# A comment line.\n# <type> <- <class>, <class>...  # <why>\n"
EVERY = ("".join(f"{stub} <- a.One  # step b3: taken\n" for stub in STUBS) + "s.Inner <- a.One  # gap: an inner class's\n")

lines = run(HEADER + EVERY)
check(lines == [], f"all listed: {lines}")

# Each kind of member, one at a time: unlisted, it is reported with its member.
expected = {"s.Field": "field", "s.Fields": "grid", "s.Constructed": "One", "s.Returned": "make",
            "s.Parameter": "take", "s.Parameters": "take", "s.Outer$Nested": "nested"}
for stub, member in expected.items():
    lines = run(HEADER + EVERY.replace(f"{stub} <- a.One  # step b3: taken\n", ""))
    check(lines == [f"unlisted a.One -> {stub} ({member})"], f"{stub} unlisted: {lines}")

# An inner class counts as its top-level class.
lines = run(HEADER + EVERY.replace("s.Inner <- a.One  # gap: an inner class's\n", ""))
check(lines == ["unlisted a.One -> s.Inner (run)"], f"inner class: {lines}")

# "*" covers every class; a line for a class that no longer names the type is stale, and "*" when nothing does.
lines = run(HEADER + EVERY.replace("s.Field <- a.One", "s.Field <- *"))
check(lines == [], f"wildcard: {lines}")
lines = run(HEADER + EVERY + "s.Field <- a.Two  # stays: never reflected\n")
check(lines == ["stale a.Two -> s.Field"], f"stale class: {lines}")
lines = run(HEADER + EVERY + "s.Gone <- *  # stays: never reflected\ns.Gone2 <- a.One  # gap: a gap\n")
check(lines == ["stale * -> s.Gone", "stale a.One -> s.Gone2"], f"stale type: {lines}")

# Every way to say why; anything else is unexplained.
for why in ("step b3: taken", "ADR-0041 a): with input", "M4 step 10: with TextView", "stays: never reflected",
            "gap: Log.wtf"):
    lines = run(HEADER + EVERY.replace("s.Field <- a.One  # step b3: taken", f"s.Field <- a.One  # {why}"))
    check(lines == [], f"why '{why}': {lines}")
for why in ("", "  # stays", "  # stays:", "  # gap:  ", "  # whatever", "  # steps b3: x", "  # ADR-41: x"):
    lines = run(HEADER + EVERY.replace("s.Field <- a.One  # step b3: taken", f"s.Field <- a.One{why}"))
    check(lines == ["unexplained s.Field <- a.One"], f"why '{why}': {lines}")

# --lines: the unlisted pairs as allowlist lines, grouped by type, with a why that is still to be written.
lines = run(HEADER + EVERY.replace("s.Field <- a.One  # step b3: taken\n", "").replace("s.Fields <- a.One  # step b3: taken\n", ""),
            extra=["--lines"])
check(lines == ["s.Field <- a.One  # FILL IN WHY", "s.Fields <- a.One  # FILL IN WHY"], f"--lines: {lines}")
lines = run(HEADER + EVERY, extra=["--lines"])
check(lines == [], f"--lines, all listed: {lines}")

print("signature_types_test: " + ("fine" if not failures else f"{failures} failed"))
sys.exit(1 if failures else 0)
