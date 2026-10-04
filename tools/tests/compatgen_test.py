#!/usr/bin/env python3
"""tools/compatgen.py (ADR-0039 step c2e): from the @ChangeId fields of sample sources (a list as framework/sources.txt
is one, a folder as framework/java, and a boot jar of boot_jars.json) it writes CompatTable, whose disabledFor and
loggableFor must answer as system_server's CompatConfig does: each annotation, @EnabledAfter's boundary, a target
from Build.VERSION_CODES, libcore's VersionCodes or a literal, a target of 0, which gates nothing, an interface's
constant without written modifiers, an alias of CUR_DEVELOPMENT, the platform's SDK as the cap on a release build and
not on a development one, a duplicate declaration, and Compatibility's parameters, which the processor skips. The
table is compiled and run on the JDK with an android.os.Build of the test's. Then what it must refuse, as Android's
annotation processor does or because it cannot read it, each with the file and line."""
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "compatgen.py")
failures = 0

BUILD = """package android.os;

public class Build {
    public static class VERSION {
        public static final int SDK_INT = 35;
        public static final String CODENAME = System.getProperty("codename", "REL");
    }

    public static class VERSION_CODES {
        public static final int CUR_DEVELOPMENT = 10000;
        public static final int P = 28;
        public static final int Q = 29;
        public static final int R = 30;
        public static final int S = 31;
        public static final int BAKLAVA = CUR_DEVELOPMENT;
    }
}
"""

VERSION_CODES = """package dalvik.annotation.compat;

public class VersionCodes {
    public static final int Q = 29;
    public static final int R = 30;
}
"""

LISTED = """package sample;

import android.compat.annotation.ChangeId;
import android.compat.annotation.Disabled;
import android.compat.annotation.EnabledAfter;
import android.compat.annotation.EnabledSince;
import android.compat.annotation.LoggingOnly;
import android.compat.annotation.Overridable;
import android.os.Build;
import android.os.Build.VERSION_CODES;
import dalvik.annotation.compat.VersionCodes;

/** Names {@link ChangeId} and @ChangeId in its documentation, which declares nothing. */
public class Listed {
    // @ChangeId in a line comment
    String text = "@ChangeId in a string, with a \\" quote";
    char quote = '"';

    /** Enabled from R on. */
    @ChangeId
    @EnabledSince(targetSdkVersion = Build.VERSION_CODES.R)
    static final long SINCE_R = 100L;

    @ChangeId @EnabledAfter(targetSdkVersion = VersionCodes.Q) private static final long AFTER_Q = 200;

    @Disabled
    @ChangeId
    @Overridable
    public static final long DISABLED = 0x12C;

    @ChangeId
    @LoggingOnly
    static final long LOGGING = 4_00L;

    @android.compat.annotation.ChangeId
    static final long PLAIN = 500L;

    @ChangeId
    @EnabledAfter(targetSdkVersion=29 /* Android Q */)
    final static long AFTER_LITERAL = 600L;

    @ChangeId
    @EnabledSince(targetSdkVersion = Build.VERSION_CODES.BAKLAVA)
    static final long SINCE_DEVELOPMENT = 700L;

    @ChangeId
    @EnabledSince(targetSdkVersion = VERSION_CODES.S)
    static final long SINCE_S = 800L;

    @ChangeId
    @EnabledSince(targetSdkVersion = 36)
    static final long SINCE_36 = 1100L;

    /** A target of 0 gates nothing, as CompatibilityChangeInfo reads it. */
    @ChangeId
    @EnabledAfter(targetSdkVersion = 0)
    static final long AFTER_ZERO = 1200L;

    void method() {
    }
}
"""

OURS = """package sample.ours;

public final class Ours {
    @android.compat.annotation.ChangeId
    @android.compat.annotation.EnabledSince(targetSdkVersion = 35)
    public static final long OURS = 900L;

    /** The same change as Listed.SINCE_R, declared again alike. */
    @android.compat.annotation.ChangeId
    @android.compat.annotation.EnabledSince(targetSdkVersion = 30)
    static final long AGAIN = 100L;

    /** An interface's constant has no written modifiers: it is public, static and final all the same (JLS 9.3). */
    public interface Constants {
        @android.compat.annotation.ChangeId
        @android.compat.annotation.Disabled
        long IN_INTERFACE = 1300L;
    }
}
"""

BOOT = """package libcore.sample;

public final class Boot {
    @ChangeId
    @EnabledSince(targetSdkVersion = dalvik.annotation.compat.VersionCodes.R)
    public static final long BOOT = 1000;
}
"""

COMPATIBILITY = """package android.compat;

public final class Compatibility {
    public static void reportUnconditionalChange(@ChangeId long changeId) {
    }

    public static boolean isChangeEnabled(@ChangeId long changeId) {
        return true;
    }
}
"""

MAIN = """import com.android.internal.compat.CompatTable;
import java.util.Arrays;

public final class Main {
    public static void main(String[] args) {
        for (int target : new int[] {28, 29, 30, 35, 36}) {
            System.out.println("disabled " + target + " " + Arrays.toString(CompatTable.disabledFor(target)));
            System.out.println("loggable " + target + " " + Arrays.toString(CompatTable.loggableFor(target)));
        }
    }
}
"""

# What disabledFor and loggableFor give on a release build of SDK 35 (the cap at 35: SINCE_36 stays disabled for
# target 36), and on a development build, without the cap.
RELEASE = """disabled 28 [100, 200, 300, 600, 700, 800, 900, 1000, 1100, 1300]
loggable 28 []
disabled 29 [100, 200, 300, 600, 700, 800, 900, 1000, 1100, 1300]
loggable 29 []
disabled 30 [300, 700, 800, 900, 1100, 1300]
loggable 30 [100, 200, 600, 1000]
disabled 35 [300, 700, 1100, 1300]
loggable 35 [900]
disabled 36 [300, 700, 1100, 1300]
loggable 36 []
"""
DEVELOPMENT = RELEASE.replace("disabled 36 [300, 700, 1100, 1300]\nloggable 36 []",
                              "disabled 36 [300, 700, 1300]\nloggable 36 [1100]")


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def generate(scratch, listed, java=None, boot=None):
    """Runs compatgen over the sample tree with `listed` as the list's one file; its result."""
    sources = os.path.join(scratch, "sources")
    write(os.path.join(sources, "sample", "Listed.java"), listed)
    write(os.path.join(scratch, "sources.txt"), "# The list's comment.\nsample/Listed.java  # a taken file\n")
    command = [sys.executable, TOOL, "--sources", sources, "--list", os.path.join(scratch, "sources.txt"),
               "--build", os.path.join(scratch, "Build.java"),
               "--version-codes", os.path.join(scratch, "VersionCodes.java"), "--out", os.path.join(scratch, "out")]
    if java:
        command += ["--java", java]
    if boot:
        command += ["--boot-jars", boot]
    return subprocess.run(command, capture_output=True, text=True)


def refused(scratch, field, message, line):
    """compatgen must refuse a class with `field` among its members, naming the file, the line and `message`."""
    listed = ("package sample;\n\nimport android.compat.annotation.*;\nimport android.os.Build;\n\n"
              "public class Listed {\n" + field + "\n}\n")
    result = generate(scratch, listed)
    where = os.path.join(scratch, "sources", "sample", f"Listed.java:{line}")
    check(result.returncode != 0 and where in result.stderr and message in result.stderr,
          f"refuses {field!r} at line {line} with {message!r}: {result.returncode} {result.stderr.strip()}")


def main():
    jdks = glob.glob(os.path.join(ROOT, "build", "tools", "jdk-17", "*", "Contents", "Home"))
    if not jdks:
        print("skipped: no JDK, see tools/fetch-sources.py --set art")
        return 0
    jdk = jdks[0]
    with tempfile.TemporaryDirectory() as scratch:
        write(os.path.join(scratch, "Build.java"), BUILD)
        write(os.path.join(scratch, "VersionCodes.java"), VERSION_CODES)
        write(os.path.join(scratch, "java", "sample", "ours", "Ours.java"), OURS)
        boot = os.path.join(scratch, "boot", "Boot.java")
        compatibility = os.path.join(scratch, "boot", "Compatibility.java")
        write(boot, BOOT)
        write(compatibility, COMPATIBILITY)
        boot_jars = os.path.join(scratch, "boot_jars.json")
        write(boot_jars, '{"jars": {"core-sample": {"sources": ["%s", "%s"], "resources": []}}, "compile_only": []}'
              % (boot, compatibility))
        result = generate(scratch, LISTED, os.path.join(scratch, "java"), boot_jars)
        check(result.returncode == 0 and "13 changes" in result.stdout,
              f"writes the sample's 13 changes: {result.returncode} {result.stdout.strip()} {result.stderr.strip()}")
        table = os.path.join(scratch, "out", "com", "android", "internal", "compat", "CompatTable.java")
        if result.returncode == 0:
            text = open(table, encoding="utf-8").read()
            check("// Listed.AFTER_Q @EnabledAfter(29)" in text and "// Listed.DISABLED @Disabled @Overridable" in text,
                  "names each change with its annotations")
            classes = os.path.join(scratch, "classes")
            write(os.path.join(scratch, "android", "os", "Build.java"), BUILD)
            write(os.path.join(scratch, "Main.java"), MAIN)
            compiled = subprocess.run([os.path.join(jdk, "bin", "javac"), "-Xlint:all", "-Werror", "-d", classes,
                                       table, os.path.join(scratch, "android", "os", "Build.java"),
                                       os.path.join(scratch, "Main.java")], capture_output=True, text=True)
            check(compiled.returncode == 0, f"the table compiles: {compiled.stderr.strip()}")
            if compiled.returncode == 0:
                for codename, expected in (("REL", RELEASE), ("Baklava", DEVELOPMENT)):
                    ran = subprocess.run([os.path.join(jdk, "bin", "java"), f"-Dcodename={codename}", "-cp", classes,
                                          "Main"], capture_output=True, text=True)
                    check(ran.stdout == expected, f"answers on a {codename} build:\n{ran.stdout}{ran.stderr}")

        refused(scratch, "    @ChangeId @Disabled @EnabledSince(targetSdkVersion = 30) static final long X = 1L;",
                "@Disabled with @EnabledAfter or @EnabledSince", 7)
        refused(scratch, "    @ChangeId @LoggingOnly @EnabledAfter(targetSdkVersion = 30) static final long X = 1L;",
                "@LoggingOnly with", 7)
        refused(scratch, "    @ChangeId @LoggingOnly @Disabled static final long X = 1L;", "@LoggingOnly with",
                7)
        refused(scratch, "\n    @ChangeId\n    @EnabledAfter(targetSdkVersion = 29)\n"
                "    @EnabledSince(targetSdkVersion = 30)\n    static final long X = 1L;",
                "both @EnabledAfter and @EnabledSince", 8)
        refused(scratch, "    @ChangeId public final long X = 1L;", "must be static and final", 7)
        refused(scratch, "    @ChangeId static long X = 1L;", "must be static and final", 7)
        refused(scratch, "    interface Outer {\n        class Inner {\n            @ChangeId final long X = 1L;\n"
                "        }\n    }", "must be static and final", 9)
        refused(scratch, "    @ChangeId static final int X = 1;", "must be a long", 7)
        refused(scratch, "    @ChangeId static final long X = 1L << 3;", "a value it cannot read", 7)
        refused(scratch, "    @ChangeId static final long X = Y;", "a value it cannot read", 7)
        refused(scratch, "    @ChangeId @EnabledSince(targetSdkVersion = Build.VERSION_CODES.NOPE) "
                "static final long X = 1L;", "a target SDK it cannot read", 7)
        refused(scratch, "    @ChangeId @EnabledSince(targetSdkVersion = Other.R) static final long X = 1L;",
                "a target SDK it cannot read", 7)
        refused(scratch, "    @ChangeId static final long X;", "something other than a field with a value", 7)
        refused(scratch, "    @ChangeId static long x() { return 1L; }", "something other than a field", 7)
        refused(scratch, "    static boolean x(@ChangeId long changeId) { return true; }",
                "@ChangeId on a parameter", 7)
        refused(scratch, "    @ChangeId @EnabledSince(targetSdkVersion = 30) static final long X = 1L;\n"
                "    @ChangeId @EnabledSince(targetSdkVersion = 31) static final long Y = 1L;", "is also", 8)
    print(f"compatgen: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
