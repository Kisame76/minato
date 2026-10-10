#!/usr/bin/env python3
"""The compat changes' table (ADR-0039 step c2e). On Android, system_server knows every change from its @ChangeId
field, as the annotation processor of tools/platform-compat collects them, and hands an app's process its disabled
changes at the start. Minato has no system_server, so this reads the same fields at build time and writes them as
com.android.internal.compat.CompatTable, from which ActivityThread.main gives the app's process its disabled and
loggable changes.

It reads every @ChangeId field of framework.jar's sources (the AOSP files of framework/sources.txt, and
framework/java) and of the boot jars' (build/art/boot_jars.json), with the annotations that ChangeIdProcessor reads at
android-15.0.0_r1: @Disabled, @EnabledSince and @EnabledAfter with their targetSdkVersion, @LoggingOnly and
@Overridable. A target SDK is a literal or a name of android.os.Build.VERSION_CODES or of libcore's VersionCodes.
Like the processor, it refuses a field that is not a static final long (as an interface's field is without a written
modifier), and the combinations the processor forbids (ChangeIdProcessor.java:136-170 and 230-250): @Disabled with a
target, @LoggingOnly with any of the three, both targets. It also refuses a value or a target it cannot read, and
one id declared twice with different annotations.
Any refusal fails the build.

The table keeps each id with the first target SDK at which its change is enabled: @EnabledSince's, @EnabledAfter's
plus one (CompatibilityChangeInfo.java:71-78), any for neither or for a target of 0 or less, none for @Disabled.
Its disabledFor and loggableFor answer as system_server's CompatConfig.getDisabledChanges and getLoggableChanges do
through CompatChange.isEnabled (services/core/java/com/android/server/compat at the tag: CompatConfig.java:139-187,
CompatChange.java:238-258), without the overrides, of which Minato has none.

Usage: compatgen.py --sources <build/sources> --list <framework/sources.txt> [--java <folder>]...
                    [--boot-jars <boot_jars.json>] --build <Build.java> --version-codes <VersionCodes.java>
                    --out <folder>
"""
import argparse
import json
import os
import re
import sys

PACKAGE = "com.android.internal.compat"
CLASS = "CompatTable"
# The annotations' package, which a source may also write out in full.
QUALIFIER = r"(?:android\s*\.\s*compat\s*\.\s*annotation\s*\.\s*)?"
CHANGE_ID = re.compile(r"@\s*" + QUALIFIER + r"ChangeId\b")
IGNORED_METHODS = {"reportUnconditionalChange", "isChangeEnabled"}
MODIFIERS = {"public", "protected", "private", "static", "final", "transient", "volatile", "abstract", "default",
             "synchronized", "native", "strictfp"}
# The qualifiers of a target SDK's name, and which table each names.
BUILD_QUALIFIERS = {"Build.VERSION_CODES", "VERSION_CODES", "android.os.Build.VERSION_CODES"}
LIBCORE_QUALIFIERS = {"VersionCodes", "dalvik.annotation.compat.VersionCodes"}
# The table's marks: an ungated change is enabled from target 0 on, a @Disabled one from no target.
ALWAYS = 0
NEVER = 2 ** 31 - 1


class Refusal(Exception):
    pass


def blank(text):
    """The text with its comments, and the contents of its string and character literals, as spaces; newlines stay,
    so positions and line numbers do too."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif text.startswith('"""', i):
            j = text.find('"""', i + 3)
            j = n if j < 0 else j + 3
            out.append('"' + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + '"')
            i = j
            continue
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] not in (quote, "\n"):
                j += 2 if text[j] == "\\" else 1
            closed = j < n and text[j] == quote
            j = min(j + 1, n) if closed else j
            out.append(quote + " " * (j - i - 2) + quote if closed else " " * (j - i))
            i = j
            continue
        else:
            out.append(text[i])
            i += 1
            continue
        out.append(re.sub(r"[^\n]", " ", text[i:j]))
        i = j
    return "".join(out)


def closing(text, start):
    """The index after the parenthesis that closes the one at start."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    raise Refusal("an annotation's parenthesis is never closed")


def enclosing_parenthesis(code, at):
    """The index of the parenthesis that is open around `at` within its declaration (a parameter list), or None."""
    depth = 0
    for i in range(at - 1, -1, -1):
        if code[i] == ")":
            depth += 1
        elif code[i] == "(":
            if depth == 0:
                return i
            depth -= 1
        elif depth == 0 and code[i] in ";{}":
            return None
    return None


def in_interface(code, at):
    """Whether the type whose body holds `at` is an interface or an annotation type, whose fields are public, static and
    final without a written modifier (JLS 9.3), as javax.lang.model reports them to the processor."""
    depth = 0
    for i in range(at - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                header = code[max(code.rfind(";", 0, i), code.rfind("{", 0, i), code.rfind("}", 0, i)) + 1:i]
                return re.search(r"\binterface\b", header) is not None
            depth -= 1
    return False


def member(code, at):
    """The declaration around the @ChangeId at `at`: its text from after the previous member up to its own end, and
    the character that ends it (";" for a field, "{" for a body)."""
    depth, start = 0, 0
    for i in range(at - 1, -1, -1):
        if code[i] == ")":
            depth += 1
        elif code[i] == "(":
            depth -= 1
        elif depth == 0 and code[i] in ";{}":
            start = i + 1
            break
    depth = 0
    for i in range(at, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
        elif depth == 0 and code[i] in ";{":
            return code[start:i], code[i]
    raise Refusal("the declaration never ends")


def parse_member(text):
    """The annotations (simple name and arguments, None without parentheses), the modifiers, and the type, name and
    value of the field that `text` declares; raises Refusal for anything else."""
    annotations, modifiers, i = [], set(), 0
    while True:
        while i < len(text) and text[i].isspace():
            i += 1
        if text.startswith("@", i):
            match = re.compile(r"@\s*((?:[\w$]+\s*\.\s*)*[\w$]+)").match(text, i)
            name = re.sub(r"\s", "", match.group(1)).rpartition(".")[2]
            i = match.end()
            while i < len(text) and text[i].isspace():
                i += 1
            arguments = None
            if text.startswith("(", i):
                end = closing(text, i)
                arguments, i = text[i + 1:end - 1], end
            annotations.append((name, arguments))
            continue
        word = re.compile(r"[\w$]+").match(text, i)
        if word and word.group(0) in MODIFIERS:
            modifiers.add(word.group(0))
            i = word.end()
            continue
        break
    field = re.fullmatch(r"\s*([\w$.]+(?:\s*\[\s*\])*)\s+([\w$]+)\s*=\s*(.*?)\s*", text[i:], re.S)
    if not field:
        raise Refusal("@ChangeId on something other than a field with a value")
    return annotations, modifiers, re.sub(r"\s", "", field.group(1)), field.group(2), field.group(3)


def long_literal(text):
    """A Java long literal's value, or None for anything else."""
    literal = text.replace("_", "")
    match = re.fullmatch(r"(0[xX][0-9a-fA-F]+|0|[1-9][0-9]*)[lL]?", literal)
    if not match:
        return None
    value = int(match.group(1), 16) if match.group(1)[:2] in ("0x", "0X") else int(match.group(1))
    return value if value < 2 ** 63 else None


def version_codes(path, class_name):
    """The int constants of `class_name` in the source at `path` (the whole file without a class name), with names
    that alias another of them resolved."""
    code = blank(open(path, encoding="utf-8").read())
    if class_name:
        match = re.search(r"\bclass\s+" + class_name + r"\b[^{]*\{", code)
        if not match:
            raise SystemExit(f"compatgen: {path} has no class {class_name}")
        depth, start = 1, match.end()
        for end in range(start, len(code)):
            depth += {"{": 1, "}": -1}.get(code[end], 0)
            if depth == 0:
                break
        code = code[start:end]
    values, aliases = {}, {}
    for name, value in re.findall(r"\bstatic\s+final\s+int\s+(\w+)\s*=\s*([\w$]+)\s*;", code):
        if value.isdigit():
            values[name] = int(value)
        else:
            aliases[name] = value
    for name, other in aliases.items():
        if other in values:
            values[name] = values[other]
    if not values:
        raise SystemExit(f"compatgen: no version codes in {path}")
    return values


def target(arguments, build, libcore):
    """The SDK of an @EnabledSince or @EnabledAfter's arguments."""
    match = re.fullmatch(r"\s*targetSdkVersion\s*=\s*(.*?)\s*", arguments or "", re.S)
    if not match:
        raise Refusal(f"a target SDK without targetSdkVersion: ({arguments})")
    expression = re.sub(r"\s", "", match.group(1))
    if re.fullmatch(r"[0-9]+", expression):
        return int(expression)
    qualifier, _, name = expression.rpartition(".")
    # A name without a qualifier is a static import of Build.VERSION_CODES' constant (ContentCaptureSession's
    # UPSIDE_DOWN_CAKE); the processor lets javac resolve it, and a name that is not one of the table's is refused.
    table = (build if qualifier in BUILD_QUALIFIERS or not qualifier
             else libcore if qualifier in LIBCORE_QUALIFIERS else None)
    if table is None or name not in table:
        raise Refusal(f"a target SDK it cannot read: {expression}")
    return table[name]


def changes_in(path, build, libcore):
    """Each @ChangeId field of the source at `path`, as a dict."""
    source = open(path, encoding="utf-8", errors="replace").read()
    code = blank(source)
    package = re.search(r"\bpackage\s+([\w.]+)\s*;", code)
    owner = os.path.splitext(os.path.basename(path))[0]
    found = []
    for at in CHANGE_ID.finditer(code):
        line = code.count("\n", 0, at.start()) + 1
        try:
            parenthesis = enclosing_parenthesis(code, at.start())
            if parenthesis is not None:
                # The processor's one exception (ChangeIdProcessor.java:107-122): the parameters of
                # android.compat.Compatibility's reportUnconditionalChange and isChangeEnabled.
                method = re.search(r"([\w$]+)\s*$", code[:parenthesis])
                if (package and package.group(1) == "android.compat" and owner == "Compatibility" and method
                        and method.group(1) in IGNORED_METHODS):
                    continue
                raise Refusal("@ChangeId on a parameter")
            text, end = member(code, at.start())
            if end != ";":
                raise Refusal("@ChangeId on something other than a field")
            annotations, modifiers, kind, name, value = parse_member(text)
            if in_interface(code, at.start()):
                modifiers |= {"static", "final"}
            if kind != "long":
                raise Refusal(f"{name}: a @ChangeId field must be a long, not {kind}")
            if "static" not in modifiers or "final" not in modifiers:
                raise Refusal(f"{name}: a @ChangeId field must be static and final")
            change_id = long_literal(value)
            if change_id is None:
                raise Refusal(f"{name}: a value it cannot read: {value}")
            names = [annotation for annotation, _ in annotations]
            disabled, logging_only = "Disabled" in names, "LoggingOnly" in names
            after = [target(arguments, build, libcore) for annotation, arguments in annotations
                     if annotation == "EnabledAfter"]
            since = [target(arguments, build, libcore) for annotation, arguments in annotations
                     if annotation == "EnabledSince"]
            if disabled and (after or since):
                raise Refusal(f"{name}: @Disabled with @EnabledAfter or @EnabledSince")
            if logging_only and (disabled or after or since):
                raise Refusal(f"{name}: @LoggingOnly with @EnabledAfter, @EnabledSince or @Disabled")
            if after and since:
                raise Refusal(f"{name}: both @EnabledAfter and @EnabledSince")
            if len(after) > 1 or len(since) > 1:
                raise Refusal(f"{name}: a target annotation twice")
            # CompatibilityChangeInfo.java:71-78: a target of 0 or less gates nothing.
            enabled_since = (NEVER if disabled else after[0] + 1 if after and after[0] > 0
                             else since[0] if since and since[0] > 0 else ALWAYS)
        except Refusal as refusal:
            raise SystemExit(f"compatgen: {path}:{line}: {refusal}")
        marks = ["@Disabled"] * disabled + [f"@EnabledAfter({sdk})" for sdk in after] \
            + [f"@EnabledSince({sdk})" for sdk in since] + ["@LoggingOnly"] * logging_only \
            + ["@Overridable"] * ("Overridable" in names)
        found.append({"id": change_id, "name": f"{owner}.{name}", "since": enabled_since, "where": f"{path}:{line}",
                      "marks": " ".join(marks)})
    return found


def sources(args):
    """The paths of every source to read, each once, in order."""
    paths = []
    with open(args.list, encoding="utf-8") as listing:
        for line in listing:
            line = line.split("#", 1)[0].strip()
            if line.endswith(".java"):
                paths.append(os.path.join(args.sources, line))
    for folder in args.java:
        for directory, _, names in sorted(os.walk(folder)):
            paths.extend(os.path.join(directory, name) for name in sorted(names) if name.endswith(".java"))
    if args.boot_jars:
        with open(args.boot_jars, encoding="utf-8") as handle:
            for jar in json.load(handle)["jars"].values():
                paths.extend(path for path in jar["sources"] if path.endswith(".java"))
    seen, unique = set(), []
    for path in paths:
        if path not in seen:
            seen.add(path)
            unique.append(path)
    return unique


def describe(since):
    return "never (@Disabled)" if since == NEVER else "every target" if since == ALWAYS else f"target {since}"


def write(changes, out):
    rows = sorted(changes.values(), key=lambda change: change["id"])
    folder = os.path.join(out, *PACKAGE.split("."))
    os.makedirs(folder, exist_ok=True)
    width = max(len(f"{row['id']}L,") for row in rows)
    ids = "".join(f"            {str(row['id']) + 'L,':<{width}}  // {row['name']}\n" for row in rows)
    since = "".join(f"            {('NEVER' if row['since'] == NEVER else str(row['since'])) + ',':<{width}}  "
                    f"// {row['name']}{' ' + row['marks'] if row['marks'] else ''}\n" for row in rows)
    text = f"""/*
 * Generated by tools/compatgen.py (ADR-0039 step c2e) from the @ChangeId fields of framework.jar's and the boot jars'
 * sources: {len(rows)} changes. Do not edit.
 */
package {PACKAGE};

import android.os.Build;

import java.util.Arrays;

/**
 * The compat changes that code in an app's process asks (libcore's Compatibility, CompatChanges, ART), each with the
 * first target SDK at which it is enabled, as system_server's compat config holds them. ActivityThread.main gives the
 * app's process its disabled and loggable changes from here, as bindApplication brings them on Android.
 */
public final class {CLASS} {{
    /** The mark of a @Disabled change: enabled at no target SDK. */
    private static final int NEVER = Integer.MAX_VALUE;

    /** The changes' ids, sorted. */
    private static final long[] IDS = {{
{ids}    }};

    /**
     * At each id's index, the first target SDK with the change enabled: @EnabledSince's, or @EnabledAfter's plus one;
     * 0 for an ungated change, NEVER for a @Disabled one.
     */
    private static final int[] ENABLED_SINCE = {{
{since}    }};

    private {CLASS}() {{
    }}

    /**
     * The changes disabled for an app of {{@code targetSdkVersion}}, sorted, as CompatConfig.getDisabledChanges gives
     * them: CompatChange.isEnabled compares the lower of the app's and the platform's target SDK with the change's.
     */
    public static long[] disabledFor(int targetSdkVersion) {{
        int compareSdk = Math.min(targetSdkVersion, platformTargetSdk());
        long[] disabled = new long[IDS.length];
        int count = 0;
        for (int i = 0; i < IDS.length; i++) {{
            if (compareSdk < ENABLED_SINCE[i]) {{
                disabled[count++] = IDS[i];
            }}
        }}
        return Arrays.copyOf(disabled, count);
    }}

    /**
     * The changes whose reports an app of {{@code targetSdkVersion}} logs, sorted, as CompatConfig.getLoggableChanges
     * gives them: those enabled for it whose gate is its own target SDK or the development one.
     */
    public static long[] loggableFor(int targetSdkVersion) {{
        int compareSdk = Math.min(targetSdkVersion, platformTargetSdk());
        long[] loggable = new long[IDS.length];
        int count = 0;
        for (int i = 0; i < IDS.length; i++) {{
            int since = ENABLED_SINCE[i];
            if (since > 0 && since != NEVER && compareSdk >= since
                    && (since == Build.VERSION_CODES.CUR_DEVELOPMENT || since == targetSdkVersion)) {{
                loggable[count++] = IDS[i];
            }}
        }}
        return Arrays.copyOf(loggable, count);
    }}

    /** AndroidBuildClassifier.platformTargetSdk: the platform's SDK on a release build, else the development one. */
    private static int platformTargetSdk() {{
        return "REL".equals(Build.VERSION.CODENAME) ? Build.VERSION.SDK_INT : Build.VERSION_CODES.CUR_DEVELOPMENT;
    }}
}}
"""
    path = os.path.join(folder, CLASS + ".java")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--sources", required=True)
    parser.add_argument("--list", required=True)
    parser.add_argument("--java", action="append", default=[])
    parser.add_argument("--boot-jars")
    parser.add_argument("--build", required=True)
    parser.add_argument("--version-codes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    build = version_codes(args.build, "VERSION_CODES")
    libcore = version_codes(args.version_codes, None)
    changes = {}
    for path in sources(args):
        if not os.path.exists(path):
            raise SystemExit(f"compatgen: {path} does not exist")
        for change in changes_in(path, build, libcore):
            known = changes.get(change["id"])
            if known and known["since"] != change["since"]:
                raise SystemExit(f"compatgen: {change['where']}: id {change['id']} is also {known['name']} "
                                 f"({known['where']}), enabled from {describe(known['since'])}, not "
                                 f"{describe(change['since'])}")
            changes.setdefault(change["id"], change)
    if not changes:
        raise SystemExit("compatgen: no @ChangeId field found")
    path = write(changes, args.out)
    print(f"compatgen: {path}, {len(changes)} changes")


if __name__ == "__main__":
    sys.exit(main())
