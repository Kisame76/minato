#!/usr/bin/env python3
"""tools/stub_references.py, framework-check's guard of run-time references (ADR-0039 step c2a): a reference from
framework.jar's code to a class that neither framework.jar nor the boot jars hold is "unlisted" unless
framework/stub-references.txt lists it for the referring top-level class or for "*"; a listed pair that no longer
holds is "stale"; a line without its why (a step, "stays: <reason>" or "gap: <what>") is "unexplained". Every kind of
reference counts: new, calls, fields, checkcast and instanceof, class constants, arrays, catch types and a lambda's
interface. Run on files in a scratch directory."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "stub_references.py")
failures = 0

# javap -c -p's output: a.One refers to each stub s.* in one way; a.One$Inner, an inner class, to s.Inner; a.Two
# only to classes that exist (framework.jar, the boot jars, its own members, arrays and primitive arrays).
JAVAP = """Compiled from "One.java"
public class a.One<T, U> extends java.lang.Object implements java.lang.Runnable {
  private s.Field field;

  public a.One();
    Code:
       0: aload_0
       1: invokespecial #1                  // Method java/lang/Object."<init>":()V
       4: return

  public void run();
    Code:
       0: new           #7                  // class s/New
       3: invokevirtual #9                  // Method s/Call.go:()V
       6: invokeinterface #11,  1           // InterfaceMethod s/Listener.heard:()V
       9: getstatic     #13                 // Field s/Constants.VALUE:I
      12: checkcast     #15                 // class s/Cast
      15: instanceof    #17                 // class s/Test
      18: ldc           #19                 // class s/Constant
      20: anewarray     #21                 // class "[Ls/Element;"
      23: multianewarray #23,  2            // class "[[Ls/Grid;"
      27: invokedynamic #25,  0             // InvokeDynamic #0:apply:()Ls/Function;
      30: invokevirtual #27                 // Method field:()V
      33: checkcast     #29                 // class "[I"
      36: return
    Exception table:
       from    to  target type
           0    36    37   Class s/Failure
           0    36    40   any

  static {};
    Code:
       0: invokestatic  #31                 // Method s/Static.init:()V
       3: invokestatic  #33                 // Method s/Shared.go:()V
       6: return
}
Compiled from "One.java"
class a.One$Inner implements java.lang.Runnable {
  public void run();
    Code:
       0: invokestatic  #3                  // Method s/Inner.go:()V
       3: return
}
Compiled from "Two.java"
public final class a.Two {
  void use();
    Code:
       0: invokestatic  #2                  // Method a/One.make:()La/One;
       3: invokevirtual #4                  // Method "[Ljava/lang/Object;".clone:()Ljava/lang/Object;
       6: invokevirtual #5                  // Method java/lang/String.length:()I
       9: checkcast     #6                  // class "[Ljava/lang/String;"
      12: invokedynamic #7,  0              // InvokeDynamic #0:run:()Ljava/lang/Runnable;
      15: invokespecial #8                  // Method "<init>":()V
      18: invokestatic  #9                  // Method s/Shared.go:()V
      21: return
}
"""
FRAMEWORK = "a/One a/One$Inner a/Two\n"
BOOT = "java/lang/Object java/lang/Runnable java/lang/String\n"
STUBS = ["s.New", "s.Call", "s.Listener", "s.Constants", "s.Cast", "s.Test", "s.Constant", "s.Element", "s.Grid",
         "s.Function", "s.Failure", "s.Static"]


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def run(allowlist, javap=JAVAP):
    with tempfile.TemporaryDirectory() as scratch:
        paths = []
        for name, text in (("framework.txt", FRAMEWORK), ("boot.txt", BOOT), ("allowlist.txt", allowlist)):
            paths.append(os.path.join(scratch, name))
            with open(paths[-1], "w") as handle:
                handle.write(text)
        result = subprocess.run([sys.executable, TOOL, *paths], input=javap, capture_output=True, text=True)
        check(result.returncode == 0, f"exit {result.returncode}: {result.stderr}")
        return result.stdout.splitlines()


HEADER = "# A comment line.\n# <stub> <- <class>, <class>...  # <why>\n"
EVERY = ("".join(f"{stub} <- a.One  # step c2c: taken\n" for stub in STUBS) + "s.Inner <- a.One  # gap: an inner class's\n"
         + "s.Shared <- a.One  # step c2c: taken\ns.Shared <- a.Two  # gap: two lines for one stub add up\n")

lines = run(HEADER + EVERY)
check(lines == [], f"all listed: {lines}")

# Each kind of reference, one at a time: unlisted, it is reported with its method and instruction.
expected = {"s.New": "One.run: new", "s.Call": "One.run: invokevirtual", "s.Listener": "One.run: invokeinterface",
            "s.Constants": "One.run: getstatic", "s.Cast": "One.run: checkcast", "s.Test": "One.run: instanceof",
            "s.Constant": "One.run: ldc", "s.Element": "One.run: anewarray", "s.Grid": "One.run: multianewarray",
            "s.Function": "One.run: invokedynamic", "s.Failure": "One.run: catch", "s.Static": "One.<clinit>: invokestatic"}
for stub, site in expected.items():
    lines = run(HEADER + EVERY.replace(f"{stub} <- a.One  # step c2c: taken\n", ""))
    check(lines == [f"unlisted a.One -> {stub} ({site})"], f"{stub} unlisted: {lines}")

# An inner class counts as its top-level class.
lines = run(HEADER + EVERY.replace("s.Inner <- a.One  # gap: an inner class's\n", ""))
check(lines == ["unlisted a.One -> s.Inner (One$Inner.run: invokestatic)"], f"inner class: {lines}")

# "*" covers every class; a line for a class that no longer refers is stale, and "*" when nothing does.
lines = run(HEADER + EVERY.replace("s.Cast <- a.One", "s.Cast <- *"))
check(lines == [], f"wildcard: {lines}")
lines = run(HEADER + EVERY + "s.Cast <- a.Two  # stays: never runs\n")
check(lines == ["stale a.Two -> s.Cast"], f"stale class: {lines}")
lines = run(HEADER + EVERY + "s.Gone <- *  # stays: never runs\ns.Gone2 <- a.One  # gap: a gap\n")
check(lines == ["stale * -> s.Gone", "stale a.One -> s.Gone2"], f"stale stub: {lines}")

# Two lines for one stub, each with its why, add up: without a.Two's, its reference is unlisted.
lines = run(HEADER + EVERY.replace("s.Shared <- a.Two  # gap: two lines for one stub add up\n", ""))
check(lines == ["unlisted a.Two -> s.Shared (Two.use: invokestatic)"], f"two lines: {lines}")

# Every way to say why; anything else is unexplained.
for why in ("step c2c: taken", "ADR-0041 a): with input", "M4 step 10: with TextView", "stays: never runs",
            "gap: Log.wtf"):
    lines = run(HEADER + EVERY.replace("s.Cast <- a.One  # step c2c: taken", f"s.Cast <- a.One  # {why}"))
    check(lines == [], f"why '{why}': {lines}")
for why in ("", "  # stays", "  # stays:", "  # gap:  ", "  # whatever", "  # steps c2c: x", "  # ADR-41: x"):
    lines = run(HEADER + EVERY.replace("s.Cast <- a.One  # step c2c: taken", f"s.Cast <- a.One{why}"))
    check(lines == ["unexplained s.Cast <- a.One"], f"why '{why}': {lines}")

# The real list explains every line.
with open(os.path.join(ROOT, "framework", "stub-references.txt")) as handle:
    real = [line for line in handle if line.strip() and not line.startswith("#")]
reasons = ("# step ", "# ADR-", "# M4 step ", "# stays: ", "# gap: ")
check(real and all(any(reason in line for reason in reasons) for line in real),
      "framework/stub-references.txt: a line without its why")

if failures:
    print(f"stub_references_test: {failures} failed")
    sys.exit(1)
print("stub_references_test: fine")
