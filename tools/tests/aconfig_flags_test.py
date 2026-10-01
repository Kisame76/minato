#!/usr/bin/env python3
"""tools/aconfig-flags.py (ADR-0039 decision 11, the lead's refinement): from aconfig's flag declarations and a
release's values it writes what aapt2's --feature-flags reads, one '<package>.<name>=<true|false>' per line, as
Soong's aconfig_text rule writes it (`aconfig dump-cache --format='{fully_qualified_name}={state:bool}'`). A flag
without a value is DISABLED, aconfig's default. First on files in a scratch directory, then on the tag's sources."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "aconfig-flags.py")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


def run(declarations, values):
    """The tool's exit status, its output file's lines and its error text."""
    with tempfile.TemporaryDirectory() as scratch:
        out = os.path.join(scratch, "flags.txt")
        result = subprocess.run([sys.executable, TOOL, "feature-flags", "--declarations", declarations,
                                 "--values", values, "--out", out], capture_output=True, text=True)
        lines = open(out).read().splitlines() if os.path.exists(out) else None
        return result.returncode, lines, result.stderr


DECLARATIONS = """package: "android.example"
container: "system"

# A comment, as declarations have them.
flag {
    name: "shown"
    namespace: "example"
    description: "A flag with { braces } and a \\"quote\\" in its text, "
        "and a second string, which text format joins to the first."
    bug: "1"
}

flag {
    name: "hidden"
    namespace: "example"
    description: "Without a value, so DISABLED."
    bug: "2"
    is_fixed_read_only: true
}
"""

VALUE = """flag_value {
  package: "android.example"
  name: "shown"
  state: ENABLED
  permission: READ_ONLY
}
"""

# A flag that only a value names, as camera_hsum_permission, which frameworks/av declares.
VALUE_ONLY = """flag_value {
  package: "com.android.internal.camera.flags"
  name: "camera_hsum_permission"
  state: ENABLED
  permission: READ_ONLY
}
"""


def main():
    with tempfile.TemporaryDirectory() as scratch:
        declarations = os.path.join(scratch, "declarations")
        values = os.path.join(scratch, "values")
        write(os.path.join(declarations, "core/java/android/example/flags.aconfig"), DECLARATIONS)
        write(os.path.join(values, "android.example/shown_flag_values.textproto"), VALUE)
        write(os.path.join(values, "com.android.internal.camera.flags/camera_hsum_permission_flag_values.textproto"),
              VALUE_ONLY)
        status, lines, error = run(declarations, values)
        check(status == 0, f"the tool runs: {error}")
        check(lines == ["android.example.hidden=false", "android.example.shown=true",
                        "com.android.internal.camera.flags.camera_hsum_permission=true"],
              f"every flag once, sorted, with its value or false: {lines}")

        write(os.path.join(values, "android.example/broken_flag_values.textproto"),
              'flag_value {\n  package: "android.example"\n  state: ENABLED\n}\n')
        status, lines, error = run(declarations, values)
        check(status != 0 and "broken_flag_values.textproto" in error and lines is None,
              f"a value without a name is refused with its file, and nothing is written: {status} {error}")

    # The tag's sources: the 7 flags of framework-res's manifest (core/res/AndroidManifest.xml), with ap3a's values.
    sources = os.path.join(ROOT, "build", "sources")
    status, lines, error = run(os.path.join(sources, "frameworks_base"),
                               os.path.join(sources, "build_release", "aconfig", "ap3a"))
    check(status == 0, f"the tool runs on the tag's sources: {error}")
    expected = {
        "android.provider.user_keys": "true",
        "com.android.internal.camera.flags.camera_hsum_permission": "true",
        "android.net.platform.flags.register_nsd_offload_engine": "false",
        "android.content.pm.quarantined_enabled": "false",
        "com.android.window.flags.screen_recording_callbacks": "true",
        "android.permission.flags.sensitive_notification_app_protection": "true",
        "android.app.contextualsearch.flags.enable_service": "true",
    }
    found = dict(line.split("=", 1) for line in lines or [])
    for name, value in expected.items():
        check(found.get(name) == value, f"{name}={value} in the tag's flags: {found.get(name)}")
    check(len(found) > 1000, f"the tag's flags are all there: {len(found)}")

    print("aconfig-flags: ok" if failures == 0 else f"aconfig-flags: {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
