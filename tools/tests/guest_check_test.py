#!/usr/bin/env python3
"""tools/guest-check.py's link check (ADR-0052): the runtime resolves every guest path below its mount's folder with
O_RESOLVE_BENEATH and answers EACCES for a link whose walk leaves it, so the folders of Guest/ that the guest's file
system mounts, system/ and apex/, may hold no such link. In a Guest/ tree made for the test, as Darwin's kernel walks
them [verified: openat with O_RESOLVE_BENEATH on macOS 27, 2026-10-05]:
  - pass: a link that stays in its folder, one to the folder itself, one whose ".." after a folder's link goes to that
    folder's parent and so stays in, one through a link in its own folder, and a mounted folder that is itself a link
    (apkrt_mount takes its realpath);
  - named: an absolute text, also one to a file inside; a ".." above the folder, also when the text comes back; a
    link into the other mounted folder or into a sibling whose name begins with the folder's; a folder's link out;
    an in-folder link to a link that leads out; and two links that point at each other (ELOOP).
Runs on /usr/bin/python3 (3.9).
Usage: tools/tests/guest_check_test.py
"""
import importlib.util
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location("guest_check", os.path.join(ROOT, "tools", "guest-check.py"))
guest_check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guest_check)

# Each link: where it lies under Guest/, its text ({guest} for Guest/'s absolute path), and whether the check names it.
LINKS = [
    ("system/etc/fonts", "../fonts", False),
    ("system/fonts/Roboto.ttf", "Roboto-Regular.ttf", False),
    ("apex/com.android.art/javalib/core.jar", "core-oj.jar", False),
    ("system/etc/top", "..", False),
    ("system/jump", "deep/x/y", False),
    ("system/etc/back", "../jump/../..", False),
    ("system/etc/viafonts", "fonts/Roboto-Regular.ttf", False),
    ("system/etc/hosts", "/etc/hosts", True),
    ("system/etc/absolute", "{guest}/system/fonts", True),
    ("system/etc/outback", "../../system/fonts", True),
    ("system/etc/up", "../..", True),
    ("system/etc/sibling", "../../system2", True),
    ("apex/com.android.art/javalib/system", "../../../system/etc", True),
    ("system/lib", "../lib64", True),
    ("system/etc/chain", "inner", True),
    ("system/etc/inner", "../../lib64", True),
    ("system/etc/loop1", "loop2", True),
    ("system/etc/loop2", "loop1", True),
]
FILES = ["system/fonts/Roboto-Regular.ttf", "system/deep/x/y/file", "apex/com.android.art/javalib/core-oj.jar",
         "lib64/libc.so", "system2/etc/hosts"]


def write(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as file:
        file.write("x")


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as temp:
        guest = os.path.join(temp, "Guest")
        for path in FILES:
            write(os.path.join(guest, path))
        for path, text, _ in LINKS:
            os.makedirs(os.path.dirname(os.path.join(guest, path)), exist_ok=True)
            os.symlink(text.replace("{guest}", guest), os.path.join(guest, path))
        problems = guest_check.link_problems(guest)
        for path, text, named in LINKS:
            lines = [line for line in problems if f" Guest/{path} " in line]
            if bool(lines) != named:
                print(f"FAIL: Guest/{path} -> {text} {'not named' if named else 'named: ' + lines[0]}")
                failures += 1
        if len(problems) != sum(1 for _, _, named in LINKS if named):
            print(f"FAIL: {len(problems)} problems: {problems}")
            failures += 1

        # The whole check names them too, beside the paths this tree lacks, and ends with 1.
        result = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "guest-check.py"), temp],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        hosts = "link: Guest/system/etc/hosts -> /etc/hosts: an absolute text leaves the folder (Guest/system)\n"
        if result.returncode != 1 or hosts not in result.stdout:
            print(f"FAIL: guest-check.py's run, status {result.returncode}:\n{result.stdout}")
            failures += 1

        # A mounted folder that is itself a link, to a folder outside Guest/ whose links stay in it; then one that
        # leads out of it.
        outside = os.path.join(temp, "elsewhere")
        write(os.path.join(outside, "etc", "public.libraries.txt"))
        os.symlink("../etc", os.path.join(outside, "etc", "self"))
        linked = os.path.join(temp, "Linked")
        os.makedirs(linked)
        os.symlink(outside, os.path.join(linked, "system"))
        if guest_check.link_problems(linked):
            print(f"FAIL: a mounted folder that is a link: {guest_check.link_problems(linked)}")
            failures += 1
        os.symlink("../../Guest", os.path.join(outside, "etc", "guest"))
        if len(guest_check.link_problems(linked)) != 1:
            print(f"FAIL: a link out of a linked mounted folder: {guest_check.link_problems(linked)}")
            failures += 1
    if failures:
        print(f"guest_check_test: {failures} failures")
        sys.exit(1)
    print(f"guest_check_test: {len(LINKS)} links and two in a linked mount, as ADR-0052 takes them")


if __name__ == "__main__":
    main()
