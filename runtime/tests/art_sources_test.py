#!/usr/bin/env python3
"""tools/art_sources.py: the stamp of the sources ART is built from. A stamp that matches the tree passes the check, a
changed patch or source fails it, a missing stamp fails it, and a file of runtime/art/tests does not count."""
import pathlib
import subprocess
import sys
import tempfile

TOOL = pathlib.Path(__file__).resolve().parents[2] / "tools" / "art_sources.py"


def run(*arguments):
    return subprocess.run([sys.executable, str(TOOL), *arguments], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    sources = root / "art"
    (sources / "patches").mkdir(parents=True)
    (sources / "tests").mkdir()
    (sources / "patches" / "0001.patch").write_text("one\n")
    (sources / "CMakeLists.txt").write_text("project\n")
    build = root / "build"
    build.mkdir()

    assert run("--check", str(sources), str(build)).returncode == 1, "no stamp must fail"
    (build / "sources.sha256").write_text(run(str(sources)).stdout)
    assert run("--check", str(sources), str(build)).returncode == 0, "a matching stamp must pass"
    (sources / "tests" / "extra.py").write_text("test only\n")
    assert run("--check", str(sources), str(build)).returncode == 0, "a test file must not count"
    (sources / "patches" / "0001.patch").write_text("two\n")
    stale = run("--check", str(sources), str(build))
    assert stale.returncode == 1 and "tools/build-art.sh" in stale.stdout + stale.stderr, "a changed patch must fail"
print("art_sources: ok")
