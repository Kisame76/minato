#!/usr/bin/env python3
"""Puts the folder `new` in place of the folder `final` in one rename, so that `final` is never missing or half written
for a program that reads it meanwhile (tools/build-framework.sh builds beside it and swaps it in). On macOS it is
renamex_np with RENAME_SWAP, which exchanges the two names atomically, and the old content is then at `new`, which this
removes; where there is no `final` yet, or no renamex_np, a plain rename (the swap fails only on another file system).
Usage: tools/swap-dir.py <new> <final>"""
import ctypes
import ctypes.util
import os
import shutil
import sys

RENAME_SWAP = 0x2


def swap(new, final):
    if not os.path.isdir(final):
        os.rename(new, final)
        return
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    exchange = getattr(libc, "renamex_np", None)
    if exchange is None or exchange(new.encode(), final.encode(), RENAME_SWAP) != 0:
        # Without the atomic exchange: two renames, with a gap of microseconds in which `final` is not there.
        old = new + ".old"
        os.rename(final, old)
        os.rename(new, final)
        shutil.rmtree(old, ignore_errors=True)
        return
    shutil.rmtree(new, ignore_errors=True)  # the old content, now under the new name


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    swap(sys.argv[1], sys.argv[2])
