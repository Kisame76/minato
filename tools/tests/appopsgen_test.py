#!/usr/bin/env python3
"""tools/appopsgen.py (ADR-0045 decision 6, the storage and app ops step): from a sample AppOpsManager.java it writes
AppOpsTable by op number: names through OPSTR_ constants and literals, simple names, permissions and default modes as
the tag's expressions with MODE_DEFAULT where none is set, getSystemAlertWindowDefault() as MODE_DEFAULT, switch ops
in the table's order, and the runtime permissions' ops without the list's comments. Then the tag's own file, with our
hand file: its 149 ops, CAMERA at 26, and the hand file's 87, 92 and 99. Then what it must refuse: a table that
_NUM_OP does not count, a switch op outside the table, a mode it cannot read, and a hand file's constant that the table
contradicts."""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "appopsgen.py")
TAG = os.path.join(ROOT, "build", "sources", "frameworks_base", "core", "java", "android", "app",
                   "AppOpsManager.java")
HAND = os.path.join(ROOT, "framework", "managers", "android", "app", "AppOpsManager.java")
failures = 0

SAMPLE = """public class AppOpsManager {
    public static final String OPSTR_COARSE_LOCATION = "android:coarse_location";
    public static final String OPSTR_FINE_LOCATION = "android:fine_location";
    public static final String OPSTR_CAMERA = "android:camera";
    public static final int _NUM_OP = 5;

    private static final int[] RUNTIME_PERMISSION_OPS = {
            // Location
            OP_COARSE_LOCATION,
            OP_FINE_LOCATION,
            // Camera
            OP_CAMERA,
    };

    static final AppOpInfo[] sAppOpInfos = new AppOpInfo[]{
        new AppOpInfo.Builder(OP_COARSE_LOCATION, OPSTR_COARSE_LOCATION, "COARSE_LOCATION")
            .setPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION)
            .setAllowSystemRestrictionBypass(new RestrictionBypass(true, false, false))
            .setDefaultMode(AppOpsManager.MODE_ALLOWED).build(),
        new AppOpInfo.Builder(OP_FINE_LOCATION, OPSTR_FINE_LOCATION, "FINE_LOCATION")
            .setPermission(Manifest.permission.ACCESS_FINE_LOCATION)
            .setSwitchCode(OP_COARSE_LOCATION).setDefaultMode(MODE_ALLOWED).build(),
        new AppOpInfo.Builder(OP_NONE, "", "").setDefaultMode(AppOpsManager.MODE_IGNORED).build(),
        new AppOpInfo.Builder(OP_CAMERA, OPSTR_CAMERA, "CAMERA")
            .setPermission(android.Manifest.permission.CAMERA).build(),
        new AppOpInfo.Builder(OP_SYSTEM_ALERT_WINDOW, "android:system_alert_window",
                "SYSTEM_ALERT_WINDOW").setDefaultMode(getSystemAlertWindowDefault()).build(),
    };
}
"""


def check(name, condition, detail=""):
    global failures
    if condition:
        print(f"ok    {name}")
    else:
        failures += 1
        print(f"FAIL  {name}{': ' + detail if detail else ''}")


def generate(folder, source, hand="class AppOpsManager {\n}\n"):
    """Runs the generator on the source and hand file texts; (exit status, stdout and stderr, the table's text)."""
    with open(os.path.join(folder, "AppOpsManager.java"), "w") as out:
        out.write(source)
    with open(os.path.join(folder, "Hand.java"), "w") as out:
        out.write(hand)
    return run(folder, os.path.join(folder, "AppOpsManager.java"), os.path.join(folder, "Hand.java"))


def run(folder, source, hand):
    out = os.path.join(folder, "out")
    result = subprocess.run([sys.executable, TOOL, "--source", source, "--hand", hand, "--out", out],
                            capture_output=True, text=True)
    table = os.path.join(out, "android", "app", "AppOpsTable.java")
    text = open(table).read() if result.returncode == 0 and os.path.exists(table) else ""
    return result.returncode, result.stdout + result.stderr, text


def array(text, name):
    """The entries of one of the table's arrays."""
    match = re.search(r"static final \w+\[\] " + name + r" = \{\n(.*?)\n    \};", text, re.S)
    return [line.strip().rstrip(",") for line in match.group(1).split("\n")] if match else None


with tempfile.TemporaryDirectory() as folder:
    status, output, text = generate(folder, SAMPLE)
    check("the sample is read", status == 0, output.strip())
    check("names through constants and literals", array(text, "NAMES") == [
        '"android:coarse_location"', '"android:fine_location"', '""', '"android:camera"',
        '"android:system_alert_window"'], str(array(text, "NAMES")))
    check("simple names", array(text, "SIMPLE_NAMES") == [
        '"COARSE_LOCATION"', '"FINE_LOCATION"', '""', '"CAMERA"', '"SYSTEM_ALERT_WINDOW"'])
    check("permissions as the tag's expressions", array(text, "PERMISSIONS") == [
        "android.Manifest.permission.ACCESS_COARSE_LOCATION", "android.Manifest.permission.ACCESS_FINE_LOCATION",
        "null", "android.Manifest.permission.CAMERA", "null"], str(array(text, "PERMISSIONS")))
    check("default modes, MODE_DEFAULT where none is set and for the alert window", array(text, "DEFAULT_MODES") == [
        "AppOpsManager.MODE_ALLOWED", "AppOpsManager.MODE_ALLOWED", "AppOpsManager.MODE_IGNORED",
        "AppOpsManager.MODE_DEFAULT", "AppOpsManager.MODE_DEFAULT"], str(array(text, "DEFAULT_MODES")))
    check("switch ops by the table's order", array(text, "SWITCH_OPS") == ["0", "0", "2", "3", "4"],
          str(array(text, "SWITCH_OPS")))
    check("the runtime permissions' ops", array(text, "RUNTIME_PERMISSION_OPS") == ["0", "1", "3"],
          str(array(text, "RUNTIME_PERMISSION_OPS")))

    if os.path.exists(TAG):
        status, output, text = run(folder, TAG, HAND)
        names, modes = array(text, "NAMES") or [], array(text, "DEFAULT_MODES") or []
        check("the tag's table", status == 0 and len(names) == 149, output.strip())
        check("the tag's CAMERA at 26", len(names) == 149 and names[26] == '"android:camera"'
              and array(text, "PERMISSIONS")[26] == "android.Manifest.permission.CAMERA"
              and modes[26] == "AppOpsManager.MODE_ALLOWED")
        check("the tag's storage ops as the hand file numbers them", len(names) == 149
              and names[87] == '"android:legacy_storage"' and modes[87] == "AppOpsManager.MODE_DEFAULT"
              and names[92] == '"android:manage_external_storage"' and modes[92] == "AppOpsManager.MODE_DEFAULT"
              and names[99] == '"android:no_isolated_storage"' and modes[99] == "AppOpsManager.MODE_ERRORED")
        check("the tag's 40 runtime permission ops", len(array(text, "RUNTIME_PERMISSION_OPS") or []) == 40)
    else:
        check("the tag's AppOpsManager.java (tools/fetch-sources.py --set framework)", False, TAG)

    for name, source, hand, message in (
            ("a table _NUM_OP does not count", SAMPLE.replace("_NUM_OP = 5", "_NUM_OP = 6"), None,
             "5 entries, but _NUM_OP is 6"),
            ("a switch op outside the table", SAMPLE.replace("setSwitchCode(OP_COARSE_LOCATION)",
                                                             "setSwitchCode(OP_GPS)"), None,
             "switch op OP_GPS is not in the table"),
            ("a mode it cannot read", SAMPLE.replace("setDefaultMode(MODE_ALLOWED)", "setDefaultMode(mode())"),
             None, "default mode 'mode()' cannot be read"),
            ("a hand file's constant the table contradicts", SAMPLE,
             "class AppOpsManager {\n    public static final int OP_CAMERA = 4;\n}\n",
             "the hand file's OP_CAMERA is 4, the table's 3")):
        status, output, _ = generate(folder, source, hand or "class AppOpsManager {\n}\n")
        check(f"refuses {name}", status != 0 and message in output, output.strip())

print(f"appopsgen test: {'fine' if failures == 0 else f'{failures} failed'}")
sys.exit(1 if failures else 0)
