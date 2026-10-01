#!/usr/bin/env python3
"""The managers' generator (tools/managergen, ADR-0045 decision 3) on framework/tests/managergen's samples.

- The parser reads metalava's signature format 2.0: classes, interfaces, enums and abstract classes with their
  generics, nested types as Outer.Inner, ctors, methods with annotations (arguments with braces, commas and
  quoted parentheses), varargs and throws, fields with their constant values (strings with "//" and ";" in them),
  and enum constants.
- The writer puts each nested type into its outer class, writes every method's body as one log line and its
  type's default, keeps abstract and interface methods without a body, leaves equals, hashCode and toString to
  Object, and gives a class without a ctor a package-private one.
- The hand file's members replace the generated ones by name and erased parameter types; members marked @hide
  and private ones are added; any other member the API lacks is an error, as are unknown classes and lines the
  parser cannot read.
- Two runs write the same files.
tools/framework-host-check.sh runs it, then compiles the samples' output with GeneratedSampleTest on the JVM.
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "managergen"))

import managergen  # noqa: E402

SAMPLES = os.path.join(HERE, "managergen")
API = os.path.join(SAMPLES, "sample-current.txt")
HAND = os.path.join(SAMPLES, "hand")
PACKAGE = "com.android.minato.sample"
CLASSES = [f"{PACKAGE}.SampleManager", f"{PACKAGE}.SampleDevice", f"{PACKAGE}.SampleFormat",
           f"{PACKAGE}.SampleCallback", f"{PACKAGE}.SampleListener", f"{PACKAGE}.SampleBase",
           f"{PACKAGE}.SampleChild", f"{PACKAGE}.SampleOther", f"{PACKAGE}.SampleShape", f"{PACKAGE}.SampleSquare"]


def read_api():
    with open(API, encoding="utf-8") as api:
        return managergen.parse(api.read(), API)


def generate(hand=HAND, classes=None):
    return managergen.generate(read_api(), classes or CLASSES, hand)


class ParserTest(unittest.TestCase):
    def setUp(self):
        self.api = read_api()

    def test_types_and_nesting(self):
        manager = self.api.types[f"{PACKAGE}.SampleManager"]
        self.assertEqual(manager.kind, "class")
        self.assertEqual([t.name for t in manager.nested], ["OnChangeListener", "Settings"])
        self.assertEqual(manager.nested[0].kind, "interface")
        self.assertEqual(manager.nested[0].modifiers, ["public", "static"])
        self.assertEqual(self.api.types[f"{PACKAGE}.SampleFormat"].kind, "enum")
        callback = self.api.types[f"{PACKAGE}.SampleCallback"]
        self.assertEqual(callback.type_parameters, "<T>")
        self.assertEqual(callback.implements, f"java.lang.Runnable, java.lang.Comparable<{PACKAGE}.SampleCallback<T>>")
        self.assertEqual(self.api.types[f"{PACKAGE}.SampleListener"].extends,
                         "java.util.EventListener, java.lang.Runnable")

    def test_methods(self):
        methods = self.api.types[f"{PACKAGE}.SampleManager"].methods
        self.assertEqual(len(methods), 21)
        by_name = {m.name: m for m in methods}
        self.assertEqual(by_name["adjustVolume"].parameters, ["int", "int"])
        self.assertEqual(by_name["getDevices"].returns, f"java.util.List<{PACKAGE}.SampleDevice>")
        self.assertEqual(by_name["getDevices"].parameters, ["int"])
        self.assertEqual(by_name["first"].type_parameters, "<T extends java.lang.Number>")
        self.assertEqual(by_name["register"].parameters,
                         [f"{PACKAGE}.SampleManager.OnChangeListener", "java.lang.Object..."])
        self.assertEqual(by_name["load"].throws, "java.io.IOException")
        self.assertIn("static", by_name["isSupported"].modifiers)
        self.assertEqual([m.annotations for m in methods if m.name == "requestFocus"], [["@Deprecated"], []])

    def test_fields(self):
        fields = {f.name: f for f in self.api.types[f"{PACKAGE}.SampleManager"].fields}
        self.assertEqual(fields["URL"].value, '"http://example.org/a;b // c \\"q\\""')
        self.assertEqual(fields["MODE_RINGTONE"].value, "-1")
        self.assertEqual(fields["BIG"].value, "9223372036854775807L")
        self.assertEqual(fields["HALF"].value, "0.5f")
        self.assertEqual(fields["LETTER"].value, "65")
        self.assertIsNone(fields["RUNTIME"].value)
        self.assertEqual(fields["NONE"].type, f"java.util.function.Supplier<{PACKAGE}.SampleDevice>")

    def test_enum_constants(self):
        self.assertEqual(self.api.types[f"{PACKAGE}.SampleFormat"].constants, ["PCM_16BIT", "PCM_FLOAT"])

    def test_second_file(self):
        # A second file into the same API links its nested types to the first file's, and nothing twice.
        text = "// Signature format: 2.0\npackage com.android.minato.sample {\n\n" \
               "  public static class SampleManager.Extra {\n  }\n\n}\n"
        managergen.parse(text, "second.txt", self.api)
        self.assertEqual([t.name for t in self.api.types[f"{PACKAGE}.SampleManager"].nested],
                         ["OnChangeListener", "Settings", "Extra"])

    def test_unreadable_line(self):
        text = "// Signature format: 2.0\npackage a {\n  public class B {\n    method public int;\n  }\n}\n"
        with self.assertRaisesRegex(managergen.GeneratorError, r"x\.txt:4: "):
            managergen.parse(text, "x.txt")

    def test_unknown_format(self):
        with self.assertRaisesRegex(managergen.GeneratorError, "format"):
            managergen.parse("// Signature format: 3.0\n", "x.txt")


class WriterTest(unittest.TestCase):
    def setUp(self):
        self.files = generate()
        self.manager = self.files["com/android/minato/sample/SampleManager.java"]

    def test_one_file_per_top_level_class(self):
        self.assertEqual(sorted(self.files), sorted(c.replace(".", "/") + ".java" for c in CLASSES))
        self.assertTrue(self.manager.startswith("// Generated by tools/managergen"))
        self.assertIn(f"package {PACKAGE};", self.manager)
        self.assertIn("import java.util.ArrayList;", self.manager)

    def test_generated_body(self):
        self.assertIn(
            "    public void adjustVolume(int p0, int p1) {\n"
            f"        Unwritten.log(\"{PACKAGE}.SampleManager.adjustVolume(int,int)\", null);\n"
            "    }\n", self.manager)
        self.assertIn(
            "    public java.util.List<com.android.minato.sample.SampleDevice> getDevices(int p0) {\n"
            f"        Unwritten.log(\"{PACKAGE}.SampleManager.getDevices(int)\", \"null\");\n"
            "        return null;\n"
            "    }\n", self.manager)
        self.assertIn("public <T extends java.lang.Number> T first(java.util.List<T> p0) {", self.manager)
        self.assertIn(f"Unwritten.log(\"{PACKAGE}.SampleManager.first(java.util.List)\", \"null\");", self.manager)
        self.assertIn("public void load(String p0) throws java.io.IOException {", self.manager)
        self.assertIn("    @Deprecated\n    public boolean isMusicActive() {", self.manager)
        self.assertNotIn("@NonNull", self.manager)
        self.assertNotIn("@RequiresPermission", self.manager)

    def test_object_methods_stay_objects(self):
        self.assertIn("    public boolean equals(Object p0) {\n        return super.equals(p0);\n    }\n", self.manager)
        self.assertIn("    public int hashCode() {\n        return super.hashCode();\n    }\n", self.manager)

    def test_nested_types(self):
        self.assertIn("    public static interface OnChangeListener {\n", self.manager)
        self.assertIn("        public void onChange(int p0);\n", self.manager)
        self.assertIn("        public default boolean wants(int p0) {\n", self.manager)
        self.assertIn("    public static final class Settings {\n", self.manager)
        self.assertIn("        public Settings(int p0) {\n", self.manager)

    def test_fields(self):
        self.assertIn('    public static final String URL = "http://example.org/a;b // c \\"q\\"";\n', self.manager)
        self.assertIn("    public static final int RUNTIME;\n", self.manager)
        self.assertIn("        RUNTIME = 0;\n", self.manager)
        self.assertIn("        public final int initial;\n", self.manager)

    def test_ctors(self):
        device = self.files["com/android/minato/sample/SampleDevice.java"]
        self.assertIn("    SampleDevice() {\n    }\n", device)
        # SampleManager's hidden ctor comes from the hand file, so no package-private one is added.
        self.assertNotIn("    SampleManager() {", self.manager)

    def test_object_methods_under_an_abstract_one(self):
        # A superclass in the API declares equals and hashCode abstract, so super cannot be called: identity.
        square = self.files["com/android/minato/sample/SampleSquare.java"]
        self.assertIn("    public boolean equals(Object p0) {\n        return this == p0;\n    }\n", square)
        self.assertIn("    public int hashCode() {\n        return System.identityHashCode(this);\n    }\n", square)
        self.assertIn("    public String toString() {\n        return super.toString();\n    }\n", square)

    def test_super_without_a_plain_ctor(self):
        # The superclass's ctor with the fewest parameters that throws nothing, given its types' defaults.
        child = self.files["com/android/minato/sample/SampleChild.java"]
        self.assertIn("    public SampleChild() {\n        super((String) null, 0);\n", child)
        other = self.files["com/android/minato/sample/SampleOther.java"]
        self.assertIn("    SampleOther() {\n        super((String) null, 0);\n    }\n", other)

    def test_enum_and_abstract(self):
        sample_format = self.files["com/android/minato/sample/SampleFormat.java"]
        self.assertIn("public enum SampleFormat {\n    PCM_16BIT,\n    PCM_FLOAT;\n", sample_format)
        callback = self.files["com/android/minato/sample/SampleCallback.java"]
        self.assertIn("public abstract class SampleCallback<T> implements java.lang.Runnable, "
                      f"java.lang.Comparable<{PACKAGE}.SampleCallback<T>> {{", callback)
        listener = self.files["com/android/minato/sample/SampleListener.java"]
        self.assertIn("public interface SampleListener extends java.util.EventListener, java.lang.Runnable {", listener)
        self.assertIn("    public abstract void onResult(T p0);\n", callback)

    def test_hand_members(self):
        self.assertIn("        return 2; // a '}' and \"}\" in a comment and a string\n", self.manager)
        self.assertEqual(self.manager.count(" getMode("), 1)
        self.assertEqual(self.manager.count(" requestFocus("), 2)
        self.assertIn("listener.onChange(hint);", self.manager)
        self.assertIn("public SampleManager(String name) {", self.manager)
        self.assertIn("public List<String> calls() {", self.manager)
        self.assertIn("private final List<String> calls = new ArrayList<>();", self.manager)
        self.assertIn("import static java.util.Objects.requireNonNull;", self.manager)
        self.assertIn("    static {\n        READY = true;\n    }\n", self.manager)

    def test_same_output_twice(self):
        self.assertEqual(self.files, generate())


class ReferencesTest(unittest.TestCase):
    """The types a set of classes reaches through its signatures: the compile-only stubs beside them. A type in no
    API file is an error in a class to write, and drops the member that names it in a stub."""

    LIBRARY = "// Signature format: 2.0\npackage org.sample.lib {\n\n  public class Lib {\n  }\n\n}\n"

    def api(self, extra=""):
        api = managergen.parse(self.LIBRARY, "lib.txt", library=True)
        with open(API, encoding="utf-8") as text:
            managergen.parse(text.read(), API, api)
        if extra:
            managergen.parse(extra, "extra.txt", api)
        return api

    def test_closure(self):
        api = self.api()
        self.assertEqual(managergen.references(api, [f"{PACKAGE}.SampleManager"]),
                         [f"{PACKAGE}.SampleDevice", f"{PACKAGE}.SampleFormat"])
        self.assertEqual(managergen.references(api, [f"{PACKAGE}.SampleChild"]), [f"{PACKAGE}.SampleBase"])
        # The roots themselves are not repeated.
        self.assertEqual(managergen.references(api, [f"{PACKAGE}.SampleChild", f"{PACKAGE}.SampleBase"]), [])

    def test_library_types_stay_out(self):
        extra = ("// Signature format: 2.0\npackage com.android.minato.other {\n\n  public class UsesLib {\n"
                 "    method public org.sample.lib.Lib lib(java.util.Map<java.lang.String, org.sample.lib.Lib>);\n"
                 "  }\n\n}\n")
        api = self.api(extra)
        self.assertEqual(managergen.references(api, ["com.android.minato.other.UsesLib"]), [])
        self.assertTrue(api.types["org.sample.lib.Lib"].library)
        with self.assertRaisesRegex(managergen.GeneratorError, "library"):
            managergen.generate(api, ["org.sample.lib.Lib"])

    def test_unknown_reference(self):
        extra = ("// Signature format: 2.0\npackage com.android.minato.other {\n\n  public class Lost {\n"
                 "    method public android.bluetooth.BluetoothAdapter adapter();\n  }\n\n}\n")
        with self.assertRaisesRegex(managergen.GeneratorError, "android.bluetooth.BluetoothAdapter"):
            managergen.references(self.api(extra), ["com.android.minato.other.Lost"])


    def test_stubs_drop_members_with_unknown_types(self):
        # A stub only has to compile: a member naming a type in no API file goes, where a class to write fails.
        extra = ("// Signature format: 2.0\npackage com.android.minato.other {\n\n  public class UsesStub {\n"
                 "    method public com.android.minato.other.Stubbed stubbed();\n  }\n\n"
                 "  public class Stubbed {\n    method public android.media.Session2Token token();\n"
                 "    method public java.util.List<android.media.Session2Token> tokens();\n"
                 "    method public int count();\n  }\n\n}\n")
        api = self.api(extra)
        self.assertEqual(managergen.references(api, ["com.android.minato.other.UsesStub"]),
                         ["com.android.minato.other.Stubbed"])
        stub = managergen.generate(api, ["com.android.minato.other.Stubbed"], stubs=True)[
            "com/android/minato/other/Stubbed.java"]
        self.assertNotIn("token", stub)
        self.assertIn("public int count() {", stub)


class ErrorTest(unittest.TestCase):
    def hand(self, body):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        folder = temporary.name
        os.makedirs(os.path.join(folder, "com/android/minato/sample"))
        with open(os.path.join(folder, "com/android/minato/sample/SampleManager.java"), "w") as hand:
            hand.write("class SampleManager {\n" + body + "\n}\n")
        return folder

    def test_member_outside_the_api(self):
        folder = self.hand("    public int getModes() {\n        return 1;\n    }")
        with self.assertRaisesRegex(managergen.GeneratorError, r"getModes\(\).*not in the API"):
            generate(folder)

    def test_overload_outside_the_api(self):
        folder = self.hand("    public int getMode(int which) {\n        return 1;\n    }")
        with self.assertRaisesRegex(managergen.GeneratorError, r"getMode\(int\).*not in the API"):
            generate(folder)

    def test_hand_file_for_another_class(self):
        folder = self.hand("")
        os.rename(os.path.join(folder, "com/android/minato/sample/SampleManager.java"),
                  os.path.join(folder, "com/android/minato/sample/SampleDevice.java"))
        with self.assertRaisesRegex(managergen.GeneratorError, "class SampleDevice"):
            generate(folder)

    def test_unknown_class(self):
        with self.assertRaisesRegex(managergen.GeneratorError, "android.media.Nothing"):
            generate(classes=["android.media.Nothing"])

    def test_nested_class_alone(self):
        with self.assertRaisesRegex(managergen.GeneratorError, "nested"):
            generate(classes=[f"{PACKAGE}.SampleManager.Settings"])


if __name__ == "__main__":
    unittest.main()
