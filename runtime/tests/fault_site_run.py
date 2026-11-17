#!/usr/bin/env python3
"""A guest's fatal fault in its own library, as a real session reports it: apk-run runs the fixture fault-site-test,
which reads byte 0x1087b through a NULL base ("null", libfault-site.c) and through x18 = 0 ("x18",
libfault-site-x18.S), so the process ends with SIGSEGV at 0x1087b in libfault-site.so.
  - With MINATO_LOADER_LOG set, as a real session sets it: stderr holds ONE fault-site line, "E/apkrt: fault-site
    SIGSEGV lib other pc +0x<offset> lr ... far 0x1087b x18 0x<x18> zero <registers> far-in <registers>
    rewritten-words <n> form <form>", whose pc offset lies in the function that loads (for "x18" exactly its second
    word, the load), whose zero list holds the base register and far-in list the index register, and whose form names
    them: base x0 or x18 = 0, index = 0x1087b. For "x18" also x18 0x0 and at least the two rewritten words of the
    function's page (its two words on x18 branch to islands). The library is "other": never its name.
  - Without it (the fixture harness): no fault-site line.
  - Both: the exit report records "signal: 11 (Segmentation fault)" and "fault: 0x1087b" (ADR-0032 decision 8).
  - "exception", with the flag: a C++ exception thrown in libfault-site-throw.so and caught in libfault-site.so ends
    with 0 and "caught", as on Android, and writes no fault-site line: an unwind is no fault.
Prints "skipped: ..." while the fixture is not built.
Usage: fault_site_run.py <fixtures-bin> <bionic-lib> <bionic-system> <apk-convert> <apk-run> <work>
"""
import fcntl
import os
import re
import struct
import subprocess
import sys

LINE = re.compile(r"^E/apkrt: fault-site SIGSEGV lib (\S+) pc \+0x([0-9a-f]+) lr (?:\+0x[0-9a-f]+|\S+ \+0x[0-9a-f]+|"
                  r"0x[0-9a-f]+) far 0x([0-9a-f]+) x18 0x([0-9a-f]+) zero (\S+) far-in (\S+) rewritten-words "
                  r"(\d+|unknown) form (.*)$")
FORM = re.compile(r"^load byte register-offset base (x\d+) index (x\d+)$")


def convert(apk_convert, elf, container):
    """The container of `elf` in `container`, made again only when the ELF file is newer."""
    if os.path.exists(container) and os.path.getmtime(container) >= os.path.getmtime(elf):
        return
    result = subprocess.run([apk_convert, "convert", elf, container, "--sign", "-"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"apk-convert {elf} failed: {result.stderr.strip()}")


def dynamic_symbols(path):
    """{name: (value, size)} of an ELF64 library's .dynsym."""
    with open(path, "rb") as file:
        data = file.read()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize) for index in range(shnum)]
    symbols = {}
    for _, kind, _, _, offset, size, link, _, _, entsize in sections:
        if kind != 11:  # SHT_DYNSYM
            continue
        strings = sections[link][4]
        for at in range(offset, offset + size, entsize):
            name, _, _, _, value, length = struct.unpack_from("<IBBHQQ", data, at)
            end = data.index(b"\0", strings + name)
            symbols[data[strings + name:end].decode()] = (value, length)
    return symbols


def main():
    if len(sys.argv) != 7:
        raise SystemExit(__doc__)
    fixtures, bionic_lib, bionic_system, apk_convert, apk_run, work = sys.argv[1:]
    program = os.path.join(fixtures, "fault-site-test")
    library = os.path.join(fixtures, "libfault-site.so")
    thrower = os.path.join(fixtures, "libfault-site-throw.so")
    for needed in (program, library, thrower):
        if not os.path.exists(needed):
            print(f"skipped: {needed} is not built (tools/build-fixtures.sh)")
            return
    symbols = dynamic_symbols(library)
    system = os.path.join(work, "system-lib64")
    own = os.path.join(work, "data-local-tmp")
    for folder in (system, own):
        os.makedirs(folder, exist_ok=True)
    with open(os.path.join(work, "convert.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name in sorted(os.listdir(bionic_lib)):
            if name.endswith(".so"):
                convert(apk_convert, os.path.join(bionic_lib, name), os.path.join(system, name + ".dylib"))
        convert(apk_convert, program, os.path.join(work, "fault-site-test.dylib"))
        convert(apk_convert, library, os.path.join(own, "libfault-site.so.dylib"))
        convert(apk_convert, thrower, os.path.join(own, "libfault-site-throw.so.dylib"))

    def run(mode, session):
        report = os.path.join(work, f"report-{mode}-{'session' if session else 'harness'}.txt")
        if os.path.exists(report):
            os.remove(report)
        environment = {key: value for key, value in os.environ.items() if key != "MINATO_LOADER_LOG"}
        if session:
            environment["MINATO_LOADER_LOG"] = "1"
        command = [apk_run, "--mount", f"/system={bionic_system}", "--libraries", f"/system/lib64={system}",
                   "--libraries", f"/data/local/tmp={own}", "--cwd", "/data/local/tmp", "--argv0", "./fault-site-test",
                   "--env", "LD_LIBRARY_PATH=/data/local/tmp", "--report", report,
                   os.path.join(work, "fault-site-test.dylib"), mode]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60,
                                env=environment)
        recorded = open(report).read() if os.path.exists(report) else ""
        return result, recorded

    failures = []
    for mode, function in (("null", "fault_site_read"), ("x18", "fault_site_read_x18")):
        if function not in symbols:
            failures.append(f"{mode}: libfault-site.so exports no {function}")
            continue
        start, size = symbols[function]
        result, recorded = run(mode, True)
        where = f"{mode}, session"
        if result.returncode not in (-11, 139) or result.stdout.splitlines() != [f"reading {mode}"]:
            failures.append(f"{where}: exit {result.returncode}, stdout {result.stdout.splitlines()}")
        lines = [line for line in result.stderr.splitlines() if line.startswith("E/apkrt: fault-site ")]
        if len(lines) != 1 or not LINE.match(lines[0]):
            failures.append(f"{where}: want one fault-site line, got {lines or result.stderr.splitlines()[-5:]}")
        else:
            role, pc, far, x18, zero, far_in, rewritten, form = LINE.match(lines[0]).groups()
            offset = int(pc, 16)
            shape = FORM.match(form)
            problems = []
            if role != "other" or "fault-site" in role:
                problems.append(f"lib {role}")
            if mode == "x18" and offset != start + 4:
                problems.append(f"pc +0x{offset:x}, want the load at +0x{start + 4:x}")
            if mode == "null" and not start <= offset < start + max(size, 4):
                problems.append(f"pc +0x{offset:x} outside {function} (+0x{start:x}, {size} bytes)")
            if int(far, 16) != 0x1087b:
                problems.append(f"far 0x{far}")
            if mode == "null":
                if shape is None:
                    problems.append(f"form {form}")
                else:
                    base, index = shape.groups()
                    if base not in zero.split(","):
                        problems.append(f"base {base} not in zero {zero}")
                    if index not in far_in.split(","):
                        problems.append(f"index {index} not in far-in {far_in}")
            else:
                # The converter islanded both words on x18, so the site's word is the branch to the island ("form
                # none"); x18 is the base, 0, and x0 the index (the function's argument).
                if form != "none" or "x18" not in zero.split(",") or "x0" not in far_in.split(","):
                    problems.append(f"form {form}, zero {zero}, far-in {far_in}")
                if int(x18, 16) != 0 or rewritten == "unknown" or int(rewritten) < 2:
                    problems.append(f"x18 0x{x18}, rewritten-words {rewritten}")
            if problems:
                failures.append(f"{where}: {'; '.join(problems)} in {lines[0]!r}")
        if "signal: 11 (Segmentation fault)" not in recorded or "fault: 0x1087b" not in recorded:
            failures.append(f"{where}: the exit report has no cause: {recorded.splitlines()[:4]}")
    result, recorded = run("null", False)
    if any(line.startswith("E/apkrt: fault-site ") for line in result.stderr.splitlines()):
        failures.append("harness: a fault-site line without the session's flag")
    if "signal: 11 (Segmentation fault)" not in recorded:
        failures.append(f"harness: the exit report has no cause: {recorded.splitlines()[:4]}")
    result, recorded = run("exception", True)
    if result.returncode != 0 or result.stdout.splitlines() != ["reading exception", "caught"]:
        failures.append(f"exception: exit {result.returncode}, stdout {result.stdout.splitlines()}, stderr's end "
                        f"{result.stderr.splitlines()[-5:]}")
    if any(line.startswith("E/apkrt: fault-site ") for line in result.stderr.splitlines()):
        failures.append("exception: a fault-site line for an exception that was caught")
    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        sys.exit(1)
    print("a guest's fatal fault names its site in a real session (NULL base and x18 = 0) and its cause in the report; "
          "a caught C++ exception across two libraries ends with 0 and no line")


if __name__ == "__main__":
    main()
