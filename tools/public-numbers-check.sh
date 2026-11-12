#!/usr/bin/env bash
# Recomputes the numbers that the public texts state (README.md, docs/COMPATIBILITY.md, CHANGELOG.md) from the tree and
# compares each with its sentence. Prints OK, WRONG or SKIP per line; exit 1 when any line is WRONG.
#   OK     the sentence states N and the tree gives N
#   WRONG  they differ, or the sentence is not where the check looks (a moved or reworded sentence fails loudly)
#   SKIP   the file the number comes from is not in the tree (printed, never counted as OK)
# Usage: tools/public-numbers-check.sh [tree]   (the repo root, default: the current directory; read only)
# Python 3.9 (/usr/bin/python3) is enough. Run it before a release and whenever a text or a number's source changes.
root="${1:-.}"
[[ -d "$root" ]] || { echo "usage: $0 [tree]" >&2; exit 2; }
exec /usr/bin/python3 - "$root" <<'PYTHON'
import math
import os
import re
import sys

ROOT = sys.argv[1]
WORDS = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10}


class Missing(Exception):
    pass


def read(path):
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        raise Missing(path)
    with open(full, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def flat(text):
    return re.sub(r"\s+", " ", text)


def number(token):
    token = token.strip().lower().replace(",", "")
    return WORDS[token] if token in WORDS else float(token) if "." in token else int(token)


def claims(path, pattern, group=1, section=None):
    """Every number a sentence states: `pattern` is searched in the flattened text (or in a section of it)."""
    text = flat(read(path))
    if section:
        start = text.find(section)
        if start < 0:
            return []
        text = text[start:]
    return [number(m.group(group)) for m in re.finditer(pattern, text)]


results = []


def check(label, claimed, actual, source):
    """claimed: list of numbers the sentence(s) state (every one must equal `actual`); empty = sentence not found."""
    if not claimed:
        results.append(("WRONG", label, "the sentence is not in the text any more (moved or reworded)"))
    elif all(abs(c - actual) < 1e-9 for c in claimed):
        results.append(("OK", label, "text says %s, %s gives %s" % (sorted(set(claimed))[0], source, actual)))
    else:
        results.append(("WRONG", label, "text says %s, %s gives %s" % (sorted(set(claimed)), source, actual)))


def guarded(label, function):
    try:
        function()
    except Missing as error:
        results.append(("SKIP", label, "%s is not in the tree" % error))


def lines_of(path):
    return read(path).split("\n")


C = "docs/COMPATIBILITY.md"
R = "README.md"
L = "CHANGELOG.md"
unreleased = flat(read(L)) if os.path.isfile(os.path.join(ROOT, L)) else ""
if "## Unreleased" in unreleased:  # the changelog's current section only
    rest = unreleased.split("## Unreleased", 1)[1]
    unreleased = "## Unreleased" + re.split(r" ## \d", rest, 1)[0]


def changelog_claims(pattern, group=1):
    return [number(m.group(group)) for m in re.finditer(pattern, unreleased)]


# ---- The gap counts (COMPATIBILITY, "The gaps, counted").
def gaps():
    text = read(C)
    flat_text = flat(text)
    counted = text.split("## The gaps, counted", 1)[1].split("\n## ", 1)[0]
    entries = [int(n) for n in re.findall(r"\(gap (\d+)\)", counted)]
    check("gaps: the list has 22 entries, numbered 1 to 22 once each",
          claims(C, r"has (\d+) gaps that stop real apps"),
          len(entries) if sorted(entries) == list(range(1, 23)) else -1, C + " entries")
    section = text.split("## The gaps, counted", 1)[1].split("\n## ", 1)[0]

    def count(start, stop):
        part = section.split(start, 1)[1].split(stop, 1)[0] if start in section else ""
        return len([l for l in part.split("\n") if l.startswith("- ")])
    closed = count("**Closed:**", "**Closed except")
    named = count("**Closed except for a limit that is named below:**", "**Partly closed:**")
    partly = count("**Partly closed:**", "**Found after")
    check("gaps: closed in total (the Closed list and the limit-named list)",
          claims(C, r"\*\*(\d+) closed,"), closed + named, "the two lists")
    check("gaps: closed except for a named limit", claims(C, r"closed, (\d+) of them except"), named, "its list")
    check("gaps: partly closed", claims(C, r"and (\d+) partly closed"), partly, "its list")


guarded("gaps", gaps)


# ---- The 88 system services (COMPATIBILITY and CHANGELOG), from the Android 15 inventory the fixture compares with.
def services():
    names = [l for l in lines_of("fixtures/expected/service-inventory-test.logcat")
             if re.search(r"service [A-Za-z0-9_]+: ", l)]
    check("services: COMPATIBILITY, public system services that answer",
          claims(C, r"Android 15's (\d+) public system services"),
          len(names), "service-inventory-test.logcat")
    check("services: COMPATIBILITY, names the check asks for", claims(C, r"for each of the (\d+) names"), len(names),
          "service-inventory-test.logcat")
    if unreleased:
        mine = changelog_claims(r"Android 15's (\d+) public system services")
        if mine:
            check("services: CHANGELOG", mine, len(names), "service-inventory-test.logcat")


guarded("services", services)


# ---- Parity benchmark (COMPATIBILITY, from BENCHMARKS).
def benchmark():
    text = read("docs/BENCHMARKS.md")
    heavy = re.search(r"vulkan-120-heavy[^\n]*?(\d+\.\d+) %", text) or re.search(r"at (\d+\.\d+) % of", text)
    check("benchmark: Vulkan under heavy load at 120 Hz, percent of the native FPS",
          claims(C, r"Vulkan under heavy load at 120 Hz, reached (\d+) %"),
          round(float(heavy.group(1))) if heavy else -1, "BENCHMARKS")
    ratio = re.search(r"draws (\d+\.\d+) FPS against the native build's (\d+\.\d+)", text)
    check("benchmark: ES 3.1 through ANGLE's Vulkan backend, percent of the native FPS",
          claims(C, r"\((\d+) % of the native FPS in that test\)"),
          round(100 * float(ratio.group(1)) / float(ratio.group(2))) if ratio else -1, "BENCHMARKS")
    # The goal sentence of the README: 95 % of the FPS and a p99 at most 5 % longer, as BENCHMARKS' rule says.
    goal = re.search(r"at least (\d+) % of", text)
    check("benchmark: README's goal, percent of the native FPS", claims(R, r"at least (\d+) % of its"),
          int(goal.group(1)) if goal else -1, "BENCHMARKS")


guarded("benchmark", benchmark)


# ---- Versions that a text names.
def version_text(label, path, pattern, tree_pattern, tree_path, source):
    """A version string compared as text: the sentence's and the tree's."""
    def run():
        said = re.findall(pattern, flat(read(path)))
        found = re.findall(tree_pattern, read(tree_path))
        if not said:
            results.append(("WRONG", label, "the sentence is not in the text any more (moved or reworded)"))
        elif not found:
            results.append(("WRONG", label, "%s does not give a version" % source))
        elif all(s == found[0] for s in said):
            results.append(("OK", label, "text says %s, %s gives %s" % (said[0], source, found[0])))
        else:
            results.append(("WRONG", label, "text says %s, %s gives %s" % (sorted(set(said)), source, found[0])))
    guarded(label, run)


version_text("versions: MoltenVK in COMPATIBILITY", C, r"MoltenVK (\d+\.\d+\.\d+) reports",
             r"MoltenVK (\d+\.\d+\.\d+) \(tag",
             "THIRD_PARTY.md", "THIRD_PARTY.md")
version_text("versions: Godot in COMPATIBILITY", C, r"Our Godot (\d+\.\d+\.\d+) test game",
             r'"tag": "(\d+\.\d+\.\d+)-stable"', "third_party/sources.json", "third_party/sources.json")
version_text("versions: Ren'Py in COMPATIBILITY", C, r"Our Ren'Py (\d+\.\d+\.\d+) test game",
             r"Ren'Py (\d+\.\d+\.\d+)'s SDK",
             "tools/renpy-check.sh", "tools/renpy-check.sh")
version_text("versions: WorkManager in COMPATIBILITY", C, r"androidx\.work (\d+\.\d+\.\d+)",
             r"work-runtime-(\d+\.\d+\.\d+)",
             "third_party/sources.json", "third_party/sources.json")
version_text("versions: SDL in COMPATIBILITY", C, r"SDL (\d+\.\d+)'s AAudio",
             r"SDL (\d+\.\d+)'s two Android audio drivers",
             "fixtures/apps/sdl-audio-test/main.c", "sdl-audio-test's header")


# ---- Whole-number facts in the code.
def code_facts():
    deployment = re.search(r'iOS: "(\d+)\.', read("project.yml"))
    check("platform: iOS floor in README", claims(R, r"iOS (\d+) or later"),
          int(deployment.group(1)) if deployment else -1,
          "project.yml deploymentTarget")
    # The API level of the NDK stubs that the libraries are compared with: the generator's and the bionic build's.
    stubs = re.search(r"^API = (\d+)", read("runtime/android/stubgen.py"), re.M)
    platform = re.search(r"-DANDROID_PLATFORM=android-(\d+)", read("tools/build-bionic.sh"))
    for name, found, source in (("COMPATIBILITY and stubgen.py", stubs, "runtime/android/stubgen.py"),
                                ("COMPATIBILITY and the bionic build", platform, "tools/build-bionic.sh")):
        check("platform: API level of the NDK stubs (%s)" % name, claims(C, r"the API (\d+) NDK"),
              int(found.group(1)) if found else -1, source)
    ndk = re.search(r"ndk/(\d+)\.", read("tools/build-framework-native.sh"))
    check("platform: NDK in README", claims(R, r"Android NDK r(\d+)"), int(ndk.group(1)) if ndk else -1,
          "tools/build-framework-native.sh")


guarded("code facts", code_facts)


def window_of_closed_itself():
    value = re.search(r"static let window: Double = (\d+)",
                      read("importer/APKKit/Sources/APKSession/ClosedItself.swift"))
    if unreleased:
        check("changelog: an app that closed itself within N seconds",
              changelog_claims(r"closed itself within (\d+) seconds"), int(value.group(1)) if value else -1,
              "ClosedItself.window")


guarded("closed itself", window_of_closed_itself)


def alarms():
    value = re.search(r"MIN_FUTURITY = (\d+)_?(\d+)L?;", read("framework/java/android/app/LocalAlarmManager.java"))
    if unreleased:
        check("changelog: alarms at N seconds at the earliest", changelog_claims(r"at (\d+) seconds at the earliest"),
              int(value.group(1) + value.group(2)) / 1000 if value else -1, "LocalAlarmManager.MIN_FUTURITY")


guarded("alarms", alarms)


def low_memory():
    value = re.search(r"lowMemoryInterval: Duration = \.seconds\((\d+)\)",
                      read("importer/APKKit/Sources/APKSession/SessionBridge.swift"))
    if unreleased:
        check("changelog: onLowMemory at most once in N seconds", changelog_claims(r"at most once in (\d+) seconds"),
              int(value.group(1)) if value else -1, "SessionBridge.lowMemoryInterval")


guarded("low memory", low_memory)


def grid():
    value = re.search(r"static let columns = (\d+)", read("importer/APKKit/Sources/APKLibrary/HomeScreenGrid.swift"))
    if unreleased:
        check("changelog: the library shows N apps side by side", changelog_claims(r"shows (\w+) apps side by side"),
              int(value.group(1)) if value else -1, "HomeScreenGrid.columns")


guarded("grid", grid)


def background_trace():
    value = re.search(r"afterReturn: Duration = \.seconds\((\d+)\)",
                      read("importer/APKKit/Sources/APKSession/BackgroundTrace.swift"))
    if unreleased:
        check("changelog: the memory N seconds after the return",
              changelog_claims(r"memory before and (\w+) seconds after the return"),
              int(value.group(1)) if value else -1, "BackgroundTrace.afterReturn")


guarded("background trace", background_trace)


def notification_limit():
    source = read("framework/java/android/app/NotificationService.java")
    limit = re.search(r"INTENT_BYTES = (\d+);", source)
    test = read("framework/tests/java/com/android/minato/notifications/NotificationTextTest.java")
    fill = re.search(r"new byte\[(\d+)\]\)\.length\(\) == NotificationText\.INTENT_BYTES", test)
    check("notifications: the intent's text limit in COMPATIBILITY",
          claims(C, r"at most (\d+) characters with the extras"),
          int(limit.group(1)) if limit else -1, "NotificationService.INTENT_BYTES")
    kilobytes = round(int(fill.group(1)) / 1024, 1) if fill else -1
    check("notifications: the extras that fit, in KB, in COMPATIBILITY",
          claims(C, r"so about (\d+\.\d+) KB of extras"), kilobytes,
          "NotificationTextTest (the bytes whose Base64 fills the limit)")
    if unreleased:
        mine = changelog_claims(r"\(about (\d+\.\d+) KB\)")
        if mine:
            check("notifications: the extras that fit, in KB, in CHANGELOG", mine, kilobytes, "NotificationTextTest")


guarded("notifications", notification_limit)


def msaa_buffer():
    # 1290 x 2796 pixels, 4 bytes, 4 samples, in MiB: the arithmetic the sentence rests on.
    if unreleased:
        found = re.search(r"saves about (\d+) MiB for (\d+)x at (\d+) x (\d+) pixels", unreleased)
        if found:
            mib = int(found.group(3)) * int(found.group(4)) * 4 * int(found.group(2)) / 1048576
            check("changelog: the multisample buffer in MiB", [int(found.group(1))], round(mib),
                  "width x height x 4 bytes x samples")
        else:
            results.append(("WRONG", "changelog: the multisample buffer in MiB",
                            "the sentence is not in the text any more"))


msaa_buffer()


width = max(len(label) for _, label, _ in results) if results else 0
for status, label, note in results:
    print("%-5s %-*s  %s" % (status, width, label, note))
wrong = sum(1 for s, _, _ in results if s == "WRONG")
ok = sum(1 for s, _, _ in results if s == "OK")
skip = sum(1 for s, _, _ in results if s == "SKIP")
print("%d OK, %d WRONG, %d SKIP" % (ok, wrong, skip))
sys.exit(1 if wrong else 0)
PYTHON
