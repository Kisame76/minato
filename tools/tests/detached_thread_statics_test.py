#!/usr/bin/env python3
"""The detached-thread guard: a detached or never-joined thread must never lock a namespace/function-static object that
has a destructor (std::mutex, std::condition_variable, a container, a string). At a host test's process exit the C++
runtime destroys such a static while the detached thread keeps running; the thread's next lock of a destroyed std::mutex
throws system_error (EINVAL) -> terminate -> abort (the runtime.program flake, .ips backtrace in the stall watchdog
daemon). The device never hits this -- a game process ends through process_exit.cpp's ::_exit / park, which runs no
static destructors -- so it is tests-only, but a flake is a flake. The cure is to make each such static NEVER DESTROYED
(a leaked heap object: `std::mutex &lock = *new std::mutex;`), so the detached thread always locks a live object.

This check has no false alarms because it does NOT flag every static mutex (most are touched only by the main thread
or by joined threads and are safe). It guards two things instead:
  1. the three statics a detached thread provably locks use the never-destroyed idiom (fails if one reverts to owned);
  2. the set of detached / never-joined thread sites matches the reviewed allowlist, so a NEW detached thread fails the
     check until someone has classified whether it locks a static-with-a-destructor (and, if so, leaked it).
Python 3.9 (the lead runs tools tests with /usr/bin/python3 3.9.6)."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
failures = 0


def check(condition, what):
    global failures
    if not condition:
        print("FAIL", what)
        failures += 1


def read(relpath):
    with open(os.path.join(REPO, relpath)) as f:
        return f.read()


# 1. The three statics a detached thread provably locks must be never-destroyed (leaked), not owned. Each pair is
#    (file, name, regex that must match the leaked form, regex that must NOT match the owned form).
LEAKED = [
    ("runtime/src/stall_watchdog.cpp", "stall watchdog lock",
     r"std::mutex\s*&\s*lock\s*=\s*\*\s*new\s+std::mutex", r"^\s*std::mutex\s+lock\s*;"),
    ("runtime/src/stall_watchdog.cpp", "stall watchdog surfaces",
     r"std::vector<Surface>\s*&\s*surfaces\s*=\s*\*\s*new", r"^\s*std::vector<Surface>\s+surfaces\s*;"),
    ("runtime/src/sys_events.cpp", "TimerThread singleton",
     r"static\s+TimerThread\s*&\s*\w+\s*=\s*\*\s*new\s+TimerThread", r"^\s*static\s+TimerThread\s+\w+\s*;"),
    ("runtime/src/dns_proxy.cpp", "dns_proxy services mutex",
     r"static\s+std::mutex\s*&\s*services\s*=\s*\*\s*new\s+std::mutex", r"^\s*static\s+std::mutex\s+services\s*;"),
]
for relpath, name, leaked_re, owned_re in LEAKED:
    src = read(relpath)
    check(re.search(leaked_re, src) is not None,
          "%s (%s) must use the never-destroyed idiom (`&NAME = *new ...`): see runtime/src/stall_watchdog.cpp"
          % (name, relpath))
    check(re.search(owned_re, src, re.M) is None,
          "%s (%s) must NOT be an owned static with a destructor (it is locked by a detached thread)"
          % (name, relpath))

# 2. Every detached / never-joined thread site in runtime/src and graphics/host, with its classification. A new site
#    (or one in a new file, or an extra one in a listed file) makes the count differ -> fail, so it gets reviewed:
#    does it lock a namespace/function-static object with a destructor? If so, leak that object (add it to list 1).
#    Classifications (why each listed site is safe today):
#      stall_watchdog.cpp     the daemon locks `lock` and reads `surfaces` -> both leaked (list 1).
#      sys_events.cpp         TimerThread::loop() locks `mutex_` and reads `owners_` -> the singleton is leaked.
#      dns_proxy.cpp          a converse thread locks `services` in getaddrinfo -> leaked (list 1).
#      memory_watermark.cpp   the 1 Hz sampler touches only atomics and stack char[] buffers -> no static-with-dtor.
#      sys_threads.cpp        guest threads are parked by process_exit stop() before any exit -> not running at exit.
#      program.cpp            the guest's main thread, parked by stop() the same way.
DETACH_RE = re.compile(r"\.detach\s*\(\s*\)|PTHREAD_CREATE_DETACHED")
EXPECTED = {
    "runtime/src/stall_watchdog.cpp": 1,
    "runtime/src/sys_events.cpp": 1,
    "runtime/src/dns_proxy.cpp": 1,
    "runtime/src/memory_watermark.cpp": 1,
    "runtime/src/sys_threads.cpp": 1,
    "runtime/src/program.cpp": 1,
}
found = {}
for base in ("runtime/src", "graphics/host"):
    root = os.path.join(REPO, base)
    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            if not fname.endswith((".cpp", ".cc", ".mm", ".m", ".h", ".hpp")):
                continue
            full = os.path.join(dirpath, fname)
            rel = os.path.relpath(full, REPO)
            count = len(DETACH_RE.findall(open(full).read()))
            if count:
                found[rel] = count

for rel, count in sorted(found.items()):
    expected = EXPECTED.get(rel)
    if expected is None:
        check(False, "new detached/never-joined thread in %s: classify whether it locks a namespace/function-static "
                     "object with a destructor; if so make that object never-destroyed, then add it here" % rel)
    else:
        check(count == expected, "%s has %d detached-thread site(s), expected %d: a new site needs that review"
              % (rel, count, expected))
for rel in sorted(EXPECTED):
    check(rel in found, "expected a detached-thread site in %s but found none: update this allowlist if it was removed"
          % rel)

print("detached_thread_statics_test:", "FAILED %d" % failures if failures else "ok")
sys.exit(1 if failures else 0)
