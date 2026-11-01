#!/usr/bin/env python3
"""The Mac harness gives the run-tests a longer finalizer timeout than Android's 10 s (a loaded Mac is slower than a
phone, and the FinalizerWatchdog aborts a run whose daemon starves), and keeps a test's own value
(1336-short-finalizer-timeout runs with 500 ms). The device script (runtime/art/run_tests.py) does not change.
Prints "skipped: ..." while AOSP's art/test is missing.

Usage: art_run_tests_options_test.py <root>
"""
import os
import sys

root = sys.argv[1]
tests = os.path.join(root, "build", "sources", "art", "test")
if not os.path.isdir(tests):
    print(f"skipped: {tests} is not there (tools/fetch-sources.py --set art)")
    sys.exit(0)
sys.path.insert(0, os.path.join(root, "runtime", "tests"))
sys.path.insert(0, os.path.join(root, "runtime", "art"))
import art_run_tests  # noqa: E402
import run_tests  # noqa: E402

OPTION = "-XX:FinalizerTimeoutMs="


def timeout_options(arguments):
    return [argument for argument in arguments if argument.startswith(OPTION)]


def effective(arguments):
    """The value ART uses: the last one given."""
    return timeout_options(arguments)[-1]


failures = []
plain = art_run_tests.guest_arguments(tests, "080-oom-throw", "/boot.jar")
if timeout_options(plain) != [OPTION + "60000"]:
    failures.append(f"080-oom-throw: {timeout_options(plain)}")
own = art_run_tests.guest_arguments(tests, "1336-short-finalizer-timeout", "/boot.jar")
if effective(own) != OPTION + "500":
    failures.append(f"1336-short-finalizer-timeout: {timeout_options(own)}")
device = run_tests.dalvikvm_arguments(tests, "080-oom-throw", "/boot.jar")
if timeout_options(device):
    failures.append(f"the device arguments changed: {timeout_options(device)}")
if failures:
    print("\n".join(failures))
    sys.exit(1)
print("the harness raises the finalizer timeout to 60 s and keeps a test's own value")
