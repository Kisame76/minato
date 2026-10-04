#!/usr/bin/env python3
"""The event log tags generator (tools/event-log-tags.py, ADR-0039 step c2c3) on made-up tag files.

- A tag gives a constant with its number and, with fields, a write method that hands them to EventLog.writeEvent in
  their order: Java's types for 1 to 5, the field's name in camel case ("Component Name" gives componentName, a Java
  keyword gets "_"), the tag's name in camel case after "write".
- A tag without fields gives only its constant; comments and blank lines are skipped.
- The class is named after the file and lies in the folder of its java_package.
- A file without java_package, a line that is no tag and a field of an unknown type fail.
tools/framework-host-check.sh runs it.
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "event-log-tags.py")

SAMPLE = """# A comment.
option java_package android.app

# The activity's onPause has been called.
30021 wm_on_paused_called (Token|1|5),(Component Name|3),(Reason|3),(time|2|3)

30062 wm_on_activity_result_called (Token|1|5),(Component Name|3),(Reason|3)
40000 one_float (Value|5)
40001 a_list (Values|4)
40002 no_fields
40003 keyword (int|1)
"""


class EventLogTagsTest(unittest.TestCase):
    def generate(self, text, name="EventLogTags.logtags"):
        """Runs the tool on `text`; returns its exit status, stderr and the files it wrote, by relative path."""
        with tempfile.TemporaryDirectory() as folder:
            source = os.path.join(folder, name)
            with open(source, "w", encoding="utf-8") as handle:
                handle.write(text)
            out = os.path.join(folder, "out")
            run = subprocess.run([sys.executable, TOOL, source, "--out", out], capture_output=True, text=True)
            files = {}
            for directory, _, names in os.walk(out):
                for file_name in names:
                    path = os.path.join(directory, file_name)
                    with open(path, encoding="utf-8") as handle:
                        files[os.path.relpath(path, out)] = handle.read()
            return run.returncode, run.stderr, files

    def test_class_in_its_package(self):
        status, _, files = self.generate(SAMPLE)
        self.assertEqual(status, 0)
        self.assertEqual(list(files), ["android/app/EventLogTags.java"])
        java = files["android/app/EventLogTags.java"]
        self.assertIn("package android.app;\n", java)
        self.assertIn("public class EventLogTags {\n    private EventLogTags() {\n    }\n", java)

    def test_constants(self):
        java = self.generate(SAMPLE)[2]["android/app/EventLogTags.java"]
        for constant in ("WM_ON_PAUSED_CALLED = 30021;", "WM_ON_ACTIVITY_RESULT_CALLED = 30062;", "ONE_FLOAT = 40000;",
                         "A_LIST = 40001;", "NO_FIELDS = 40002;", "KEYWORD = 40003;"):
            self.assertIn("    public static final int " + constant + "\n", java)

    def test_write_methods(self):
        java = self.generate(SAMPLE)[2]["android/app/EventLogTags.java"]
        self.assertIn(
            "    public static void writeWmOnPausedCalled(int token, String componentName, String reason, long time) {\n"
            "        android.util.EventLog.writeEvent(WM_ON_PAUSED_CALLED, token, componentName, reason, time);\n"
            "    }\n", java)
        self.assertIn(
            "    public static void writeWmOnActivityResultCalled(int token, String componentName, String reason) {\n"
            "        android.util.EventLog.writeEvent(WM_ON_ACTIVITY_RESULT_CALLED, token, componentName, reason);\n",
            java)
        self.assertIn("    public static void writeOneFloat(float value) {\n"
                      "        android.util.EventLog.writeEvent(ONE_FLOAT, value);\n", java)
        self.assertIn("    public static void writeAList(Object[] values) {\n", java)
        self.assertIn("    public static void writeKeyword(int int_) {\n"
                      "        android.util.EventLog.writeEvent(KEYWORD, int_);\n", java)

    def test_tag_without_fields_has_no_method(self):
        java = self.generate(SAMPLE)[2]["android/app/EventLogTags.java"]
        self.assertNotIn("writeNoFields", java)

    def test_class_named_after_the_file(self):
        files = self.generate("option java_package com.example\n1 x (a|1)\n", name="OtherTags.logtags")[2]
        self.assertIn("public class OtherTags {", files["com/example/OtherTags.java"])

    def test_failures(self):
        for text, message in (("1 x (a|1)\n", "no option java_package"),
                              ("option java_package a\nnot a tag\n", "not a tag"),
                              ("option java_package a\n1 x (a|9)\n", "bad field (a|9)")):
            status, stderr, files = self.generate(text)
            self.assertNotEqual(status, 0, text)
            self.assertIn(message, stderr)
            self.assertEqual(files, {})


if __name__ == "__main__":
    unittest.main()
