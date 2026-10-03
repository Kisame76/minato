#!/usr/bin/env python3
"""The guest's fonts (ADR-0043 test 2, decision 5): /system/etc/font_fallback.xml, which Android 15's SystemFonts
reads, and the legacy fonts.xml, read as FontListParser reads them, against the files in /system/fonts.
- FontListParser drops a font whose file is missing, a family left without fonts and an alias to no named family,
  each without a word, and throws on a named family with lang, variant or ignore. So every font a list names must be
  a file in /system/fonts, every family must have a font, every alias must point to a named family of its list, and
  no named family may carry those attributes.
- /system/fonts holds only what the lists name, as public.libraries.txt lists only what we ship, and
  RobotoStatic-Regular.ttf, which no list names: Typeface's class init preloads it by path, as Roboto-Regular.ttf.
- Decision 5's families: sans-serif first, sans-serif-condensed and monospace named, and a last family without a
  name that holds DroidSansFallbackFull.ttf.
Prints "skipped: ..." while the guest system is not built.

Usage: fonts_check.py <guest system>
  <guest system>: the folder mounted as the guest's /system, build/bionic/system of tools/build-bionic.sh
"""
import os
import re
import sys
import xml.etree.ElementTree as ElementTree

LISTS = ("font_fallback.xml", "fonts.xml")
NAMED = ("sans-serif", "sans-serif-condensed", "monospace")
FALLBACK = "DroidSansFallbackFull.ttf"
# What Typeface's class init preloads by path, named by a list or not (Typeface.java).
PRELOADED = ("Roboto-Regular.ttf", "RobotoStatic-Regular.ttf")
# FontListParser's FILENAME_WHITESPACE_PATTERN: leading and trailing XML whitespace.
WHITESPACE = re.compile(r"^[ \n\r\t]+|[ \n\r\t]+$")


def file_name(font):
    """A font element's file name: all of its text, between its axis children too, without the outer whitespace."""
    return WHITESPACE.sub("", (font.text or "") + "".join(child.tail or "" for child in font))


def read_list(path):
    """[(family name or None, [file names])] and [(alias name, its target)] of one font list, and its problems."""
    root = ElementTree.parse(path).getroot()
    if root.tag != "familyset":
        return [], [], [f"the root is <{root.tag}>, not <familyset>"]
    families, aliases, problems = [], [], []
    for element in root:
        name = element.get("name")
        if element.tag == "family":
            inner = [element]
            if name is not None:
                problems += [f"family {name} has {key}, which FontListParser throws on"
                             for key in ("lang", "variant", "ignore") if element.get(key) is not None]
        elif element.tag == "family-list":
            inner = element.findall("family")
            problems += [f"a family in family-list {name} has {key}, which FontListParser throws on"
                         for family in inner for key in ("name", "lang", "variant", "ignore")
                         if family.get(key) is not None]
        elif element.tag == "alias":
            aliases.append((name, element.get("to")))
            continue
        else:
            continue
        files = [file_name(font) for family in inner for font in family.findall("font")]
        if not files:
            problems.append(f"family {name or len(families) + 1} has no font, so FontListParser drops it")
        families.append((name, files))
    named = {name for name, _ in families if name}
    problems += [f"alias {name} points to {target or 'nothing'}, no named family of this list"
                 for name, target in aliases if target not in named]
    return families, aliases, problems


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 2
    system = sys.argv[1]
    if not os.path.isdir(system):
        print(f"skipped: {system} is not built (tools/build-bionic.sh)")
        return 0
    fonts_dir = os.path.join(system, "fonts")
    shipped = set(os.listdir(fonts_dir)) if os.path.isdir(fonts_dir) else set()
    failures, listed, summary = [], set(), []
    for list_name in LISTS:
        path = os.path.join(system, "etc", list_name)
        if not os.path.isfile(path):
            failures.append(f"no /system/etc/{list_name}: tools/build-bionic.sh copies it from graphics/fonts")
            continue
        families, aliases, problems = read_list(path)
        failures += [f"{list_name}: {problem}" for problem in problems]
        names = [name for name, _ in families]
        files = [file for _, family_files in families for file in family_files]
        listed.update(files)
        failures += [f"{list_name}: {file} is not in /system/fonts" for file in sorted(set(files) - shipped)]
        if not names or names[0] != "sans-serif":
            failures.append(f"{list_name}: the first family is {names[0] if names else 'missing'}, not sans-serif")
        failures += [f"{list_name}: no family {name}" for name in NAMED if name not in names]
        if not families or families[-1][0] is not None or FALLBACK not in families[-1][1]:
            failures.append(f"{list_name}: the last family is not a fallback without a name that holds {FALLBACK}")
        summary.append(f"{list_name} {len(families)} families and {len(aliases)} aliases")
    failures += [f"/system/fonts/{file} is in no list" for file in sorted(shipped - listed - set(PRELOADED))]
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        print(f"{len(failures)} failures in {system}")
        return 1
    print(f"the guest's font lists ({', '.join(summary)}) name {', '.join(sorted(listed))}, all in "
          f"/system/fonts, which holds {len(shipped)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
