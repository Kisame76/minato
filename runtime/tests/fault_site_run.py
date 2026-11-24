#!/usr/bin/env python3
"""A guest's fatal fault in its own library, as a real session reports it: apk-run runs the fixture fault-site-test,
which reads byte 0x1087b through a NULL base ("null", libfault-site.c) and through x18 = 0 ("x18",
libfault-site-x18.S), so the process ends with SIGSEGV at 0x1087b in libfault-site.so.
  - With MINATO_LOADER_LOG set, as a real session sets it: stderr holds ONE fault-site line, "E/apkrt: fault-site
    SIGSEGV lib other pc +0x<offset> lr ... far 0x1087b x18 0x<x18> zero <registers> far-in <registers>
    rewritten-words <n> form <form> base-is <class> heap-window <base> va=<MiB> vahole=<MiB> regs x0=0x.. ... x30=0x..
    sp=0x..", whose pc offset lies in the function that loads (for "x18" exactly its second word, the load), whose zero
    list holds the base register and far-in list the index register, and whose form names them with their values:
    base x0=0x0, index x1=0x1087b, the base classed "null" ("-" for "x18", whose word is a branch). No ART runs here,
    so heap-window is "-". regs lists x0..x30 and sp, x0 and x1 as the form has them (for "x18": x0 = 0x1087b). For
    "x18" also x18 0x0 and at least the two rewritten words of the function's page (its two words on x18 branch to
    islands). va and vahole, the address space at the fault, are not 0. The library is "other": never its name.
  - The line's x18 is the hardware register, gx18 the guest's x18 as the signal path resolves it, with where it
    came from (slot11, or rename in a (c) range): "x18" gives gx18 0, "gx18" (the guest's x18 set to 0x5a5a0000, a
    read through a NULL base in x0 with index x1) gives gx18 0x5a5a0000 from slot11, whatever the hardware x18 is.
  - Without it (the fixture harness): no fault-site line.
  - Both: the exit report records "signal: 11 (Segmentation fault)" and "fault: 0x1087b" (ADR-0032 decision 8).
  - "exception", with the flag: a C++ exception thrown in libfault-site-throw.so and caught in libfault-site.so ends
    with 0 and "caught", as on Android, and writes no fault-site line: an unwind is no fault.
  - With the flag, the NULL-base read under an app's own SIGSEGV handler (libfault-site.h) still writes ONE line for
    the read, as "null" checks it, however the handler ends: "chained" (it restores the old action and returns: the read
    faults again, 139), "abort" (abort() in the handler, 134), "twice" (a reporter from a second library calls it, and
    it restores the old action and raises SIGSEGV, which arrives when the reporter returns, 139) and "crash" (it faults
    itself at 0x2a, 139). "recover" (it moves past the read, then the program calls abort(), 134) writes none: a fault
    a handler recovered from is no cause. Each prints its handlers' lines, and the report records its signal.
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
                  r"0x[0-9a-f]+) far 0x([0-9a-f]+) x18 0x([0-9a-f]+) gx18 0x([0-9a-f]+) (slot11|rename) "
                  r"zero (\S+) far-in (\S+) rewritten-words "
                  r"(\d+|unknown) form (.*) base-is (\S+) heap-window (\S+) va=(\d+) vahole=(\d+) regs (.*)$")
REGISTERS = [f"x{number}" for number in range(31)] + ["sp"]
FORM = re.compile(r"^load byte register-offset base (x\d+)=0x([0-9a-f]+) index (x\d+)=0x([0-9a-f]+)$")


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
    reporter = os.path.join(fixtures, "libfault-site-reporter.so")
    for needed in (program, library, thrower, reporter):
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
        convert(apk_convert, reporter, os.path.join(own, "libfault-site-reporter.so.dylib"))

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
    segv, abrt = ((-11, 139), "11 (Segmentation fault)"), ((-6, 134), "6 (Aborted)")
    engine = "engine handler: signal 11 at 0x1087b"
    # mode, the reading function, (exit statuses, the report's signal), the handlers' lines after "reading <mode>"
    for mode, function, (statuses, cause), handlers in (
            ("null", "fault_site_read", segv, []), ("x18", "fault_site_read_x18", segv, []),
            ("chained", "fault_site_read", segv, [engine]), ("abort", "fault_site_read", abrt, [engine]),
            ("twice", "fault_site_read", segv, ["reporter handler: signal 11", engine]),
            ("crash", "fault_site_read", segv, [engine]), ("gx18", "fault_site_read_gx18", segv, [])):
        if function not in symbols:
            failures.append(f"{mode}: libfault-site.so exports no {function}")
            continue
        start, size = symbols[function]
        result, recorded = run(mode, True)
        where = f"{mode}, session"
        if result.returncode not in statuses or result.stdout.splitlines() != [f"reading {mode}"] + handlers:
            failures.append(f"{where}: exit {result.returncode}, stdout {result.stdout.splitlines()}")
        lines = [line for line in result.stderr.splitlines() if line.startswith("E/apkrt: fault-site ")]
        if len(lines) != 1 or not LINE.match(lines[0]):
            failures.append(f"{where}: want one fault-site line, got {lines or result.stderr.splitlines()[-5:]}")
        else:
            (role, pc, far, x18, gx18, source, zero, far_in, rewritten, form, base_is, window, va, vahole,
             registers) = LINE.match(lines[0]).groups()
            values = dict(pair.split("=0x", 1) for pair in registers.split(" ") if "=0x" in pair)
            offset = int(pc, 16)
            shape = FORM.match(form)
            problems = []
            if role != "other" or "fault-site" in role:
                problems.append(f"lib {role}")
            if mode == "x18" and offset != start + 4:
                problems.append(f"pc +0x{offset:x}, want the load at +0x{start + 4:x}")
            if mode != "x18" and not start <= offset < start + max(size, 4):
                problems.append(f"pc +0x{offset:x} outside {function} (+0x{start:x}, {size} bytes)")
            if int(far, 16) != 0x1087b:
                problems.append(f"far 0x{far}")
            if int(va) == 0 or int(vahole) == 0:
                problems.append(f"va={va} vahole={vahole}: the address space was not sampled")
            if list(values) != REGISTERS or len(registers.split(" ")) != len(REGISTERS):
                problems.append(f"regs {registers}")
            if window != "-":
                problems.append(f"heap-window {window} without ART")
            want_x0, want_class = (0x1087b, "-") if mode == "x18" else (0, "null")
            if base_is != want_class or int(values.get("x0", "-1"), 16) != want_x0:
                problems.append(f"base-is {base_is}, x0 0x{values.get('x0')}: want {want_class}, 0x{want_x0:x}")
            if mode != "x18" and int(values.get("x1", "-1"), 16) != 0x1087b:
                problems.append(f"x1 0x{values.get('x1')}, want the index 0x1087b")
            # The guest's x18 as the signal path resolves it (TSD slot 11 here: no (c) range): what the library set.
            want_gx18 = {"x18": 0, "gx18": 0x5a5a0000}.get(mode)
            if want_gx18 is not None and (int(gx18, 16) != want_gx18 or source != "slot11"):
                problems.append(f"gx18 0x{gx18} {source}, want 0x{want_gx18:x} slot11")
            if mode != "x18":
                if shape is None:
                    problems.append(f"form {form}")
                else:
                    base, base_value, index, index_value = shape.groups()
                    if base not in zero.split(",") or int(base_value, 16) != 0:
                        problems.append(f"base {base}=0x{base_value} not 0 or not in zero {zero}")
                    if index not in far_in.split(",") or int(index_value, 16) != 0x1087b:
                        problems.append(f"index {index}=0x{index_value} not 0x1087b or not in far-in {far_in}")
            else:
                # The converter islanded both words on x18, so the site's word is the branch to the island ("form
                # none"); x18 is the base, 0, and x0 the index (the function's argument).
                if form != "none" or "x18" not in zero.split(",") or "x0" not in far_in.split(","):
                    problems.append(f"form {form}, zero {zero}, far-in {far_in}")
                if int(x18, 16) != 0 or rewritten == "unknown" or int(rewritten) < 2:
                    problems.append(f"x18 0x{x18}, rewritten-words {rewritten}")
            if problems:
                failures.append(f"{where}: {'; '.join(problems)} in {lines[0]!r}")
        # The death's own signal; the fault address where the read's fault itself ends the process.
        if cause not in recorded or (mode in ("null", "x18", "chained", "gx18") and "fault: 0x1087b" not in recorded):
            failures.append(f"{where}: the exit report has no cause: {recorded.splitlines()[:4]}")
    result, recorded = run("recover", True)
    if result.returncode not in abrt[0] or result.stdout.splitlines() != ["reading recover", engine, "recovered"]:
        failures.append(f"recover: exit {result.returncode}, stdout {result.stdout.splitlines()}")
    if any(line.startswith("E/apkrt: fault-site ") for line in result.stderr.splitlines()):
        failures.append("recover: a fault-site line for a fault the handler recovered from")
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
    print("a guest's fatal fault names its site in a real session (NULL base and x18 = 0, and under the app's own "
          "handler however it ends) and its cause in the report; a fault a handler recovered from and a caught C++ "
          "exception across two libraries write no line")


if __name__ == "__main__":
    main()
