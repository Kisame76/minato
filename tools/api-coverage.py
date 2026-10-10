#!/usr/bin/env python3
"""The framework's breadth (the compatibility plan's entry 22, docs/plans/2026-10-07-framework-breadth.md): every
public type of Android 15's API (frameworks/base's core/api/current.txt) that framework.jar lacks, by package and by
what it needs, and the batches in which the pure-Java ones go into framework/sources.txt.

A missing top-level type's group comes from its AOSP source and from the hidden types it pulls in; the worst wins, in
this order:
  pure Java    its source is fetched and plain Java;
  service      it reaches a system service: ServiceManager, a Stub's asInterface, getSystemService of a service
               that our ContextImpl does not answer (--context-impl: the names and classes it answers, as
               Context.SENSOR_SERVICE.equals(name) and serviceClass == SensorManager.class name them, and
               --service-registry: those our SystemServiceRegistry registers, as cached(Context.POWER_SERVICE,
               PowerManager.class, ...) names them, are local; a name in a variable is not), or an AIDL interface
               (a .aidl file that declares an interface, not one that declares a parcelable, or a name like
               IDumpstate that its package lacks, as another project's .aidl makes it);
  native       it declares native methods (JNI);
  module       it imports an android or com.android type that nothing here defines: framework.jar, the APIs the build
               makes compile-only stubs of (--stub-api: libcore's, ICU's and the modules' current.txt files), the
               classes the build compiles against (--known: its stub classes and the boot jars' java.base.jmod), and
               the fetched sources; a module's or a HAL's, so a batch cannot compile it alone;
  not fetched  build/sources lacks its source.
A type uses what it imports and the types of its own package that it names. A structured parcelable's .aidl file
(parcelable X { ... }), which aidl turns into plain Java, is a source like a .java file. A used type outside the API
that framework.jar lacks has no compile-only stub, so it comes with the type, as do the hidden types it uses in turn:
they are pulled in, and their groups count. A used API type that framework.jar lacks is a stub at compile time and stays
one unless a batch takes it: a stub reference, which framework/stub-references.txt must list. A nested type that
framework.jar lacks while it has its top-level type counts apart: for our own types (--ours), our code has to add it.
The pure-Java types go into batches of up to --batch types (60), whole packages where they fit: first the packages of
--first's types (fixtures/corpus/api-first.txt: one qualified name a line, # starts a comment), then the packages whose
types the most missing types of other packages use. Within a package a type comes after the ones it uses. A type that
another step takes (fixtures/corpus/api-taken.txt: a package, which is its own types only, or a type, then a tab and the
step) goes into no batch; the table lists it with its step. The APIs that Android 15 builds apart from the core one,
each a library of its own (--module-api <name>=<current.txt>: the connectivity modules', Wi-Fi's and location's), are
counted apart, by package: which of their top-level types framework.jar has, and which another step takes. Their
sources lie in other projects, so no batch takes them. Writes the table (docs/corpus/api-coverage.md) and the JSON
for the batches (build/api-coverage.json). It reads framework.jar's classes in build/framework/classes, so
tools/build-framework.sh comes first. Python 3.9.

Usage: api-coverage.py [--api <current.txt>] [--stub-api <current.txt>]... [--known <folder or .jmod>]...
                       [--classes <folder>] [--sources <folder>]... [--ours <folder>] [--first <file>] [--taken <file>]
                       [--batch <n>] [--context-impl <file>] [--service-registry <file>]
                       [--module-api <name>=<current.txt>]...
                       [--table <file>] [--json <file>]
"""
import collections
import json
import os
import re
import sys
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("MINATO_REPO") or os.path.dirname(TOOLS)
GROUPS = ["pure Java", "service", "native", "module", "not fetched"]
# Annotations may have qualified names and arguments: @android.widget.RemoteViews.RemoteView, @FlaggedApi("a.b").
TYPE = re.compile(r'^  (?:@[\w.]+(?:\([^)]*\))? )*(?:public|protected) (?:\w+ )*?(?:class|interface|enum|@interface) '
                  r'([\w.]+)')
# Comments, strings and characters, which name nothing.
NOISE = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
NATIVE = re.compile(r"\bnative\s+[\w<>\[\],.? ]+?\s+\w+\s*\(")
BINDER = re.compile(r"\bServiceManager\b|\.Stub\.asInterface\b")
SYSTEM_SERVICE = re.compile(r"\bgetSystemService\s*\(")
# A getSystemService argument that names a service: Context.SENSOR_SERVICE or SensorManager.class.
SERVICE_NAME = re.compile(r"(?:[\w.]+\.)?([A-Z][A-Z0-9_]*_SERVICE)")
SERVICE_CLASS = re.compile(r"(?:[\w.]+\.)?(\w+)\.class")
# A parameter, as in a declaration of getSystemService itself: String name, @NonNull Class<T> serviceClass.
PARAMETER = re.compile(r"(?:final\s+)?(?:@[\w.]+\s+)*[\w.]+(?:<[^<>]*>)?\s+\w+")
AIDL_INTERFACE = re.compile(r"^\s*(?:oneway\s+)?interface\s+\w+", re.M)
STRUCTURED_PARCELABLE = re.compile(r"^\s*(?:@\w+(?:\([^)]*\))?\s*)*parcelable\s+\w+\s*(?:<[^>]*>)?\s*\{", re.M)
IMPORT = re.compile(r"^\s*import\s+(static\s+)?([\w.]+?)(\.\*)?\s*;", re.M)
NAME = re.compile(r"\b[A-Z]\w*\b")
AIDL_NAME = re.compile(r"^I[A-Z][a-z]\w*$")
# The APIs that Android 15 builds apart from frameworks/base's core one, each a library of its own, in build/sources
# as third_party/sources.json fetches them.
MODULE_APIS = [("connectivity", "connectivity_api/framework/api/current.txt"),
               ("connectivity_t", "connectivity_t_api/framework-t/api/current.txt"),
               ("location", "location_api/location/api/current.txt"), ("wifi", "wifi_api/framework/api/current.txt")]


def read_api(path):
    """The API's public types as (package, name), a nested one's name as Outer.Inner."""
    types, package = [], None
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^package (\S+) \{", line)
            if match:
                package = match.group(1)
                continue
            match = TYPE.match(line)
            if match and package:
                types.append((package, match.group(1)))
    return types


def class_names(folder):
    """The binary names of the classes in `folder`, as a/b/Outer$Inner, or in a .jmod's classes/."""
    names = set()
    if folder.endswith(".jmod"):
        with zipfile.ZipFile(folder) as archive:
            return {name[len("classes/"):-len(".class")] for name in archive.namelist()
                    if name.startswith("classes/") and name.endswith(".class")}
    for directory, _, files in os.walk(folder):
        for name in files:
            if name.endswith(".class"):
                names.add(os.path.relpath(os.path.join(directory, name), folder)[:-len(".class")])
    return names


def local_services(*paths):
    """The system services our ContextImpl answers itself and our SystemServiceRegistry registers (cached or register
    of a name and a class): their names (LAYOUT_INFLATER_SERVICE) and classes (LayoutInflater)."""
    names, classes = set(), set()
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            code = NOISE.sub(" ", handle.read())
        names |= set(re.findall(r"\b(?:Context\.)?([A-Z][A-Z0-9_]*_SERVICE)\.equals\(\s*name\s*\)", code))
        classes |= {name.rsplit(".", 1)[-1]
                    for name in re.findall(r"\bserviceClass\s*==\s*([\w.]+)\.class\b", code)}
        for name, cls in re.findall(r"\b(?:cached|register)\(\s*(?:Context\.)?([A-Z][A-Z0-9_]*_SERVICE)\s*,\s*"
                                    r"([\w.]+)\.class\b", code):
            names.add(name)
            classes.add(cls.rsplit(".", 1)[-1])
    return names, classes


def source_index(roots):
    """The fetched sources: {qualified name: path} of the .java files and of the structured parcelables' .aidl
    files, and the qualified names of the AIDL interfaces."""
    java, aidl, parcelables = {}, set(), {}
    for root in roots:
        for directory, _, files in os.walk(root):
            for name in files:
                stem, extension = os.path.splitext(name)
                qualified = os.path.relpath(os.path.join(directory, stem), root).replace(os.sep, ".")
                if extension == ".java":
                    java.setdefault(qualified, os.path.join(directory, name))
                elif extension == ".aidl":
                    with open(os.path.join(directory, name), encoding="utf-8", errors="replace") as handle:
                        code = NOISE.sub(" ", handle.read())
                    if AIDL_INTERFACE.search(code):
                        aidl.add(qualified)
                    elif STRUCTURED_PARCELABLE.search(code):
                        parcelables.setdefault(qualified, os.path.join(directory, name))
    for qualified, path in parcelables.items():
        java.setdefault(qualified, path)
    return java, aidl


class Coverage:
    def __init__(self, api, classes, roots, ours, stub_apis=(), known=(), local=(set(), set())):
        self.types = read_api(api)
        self.local_names, self.local_classes = local
        # Every type the build makes a compile-only stub of, nested ones too, and every class it compiles against.
        self.stubbed = {f"{package}.{name}" for path in [api, *stub_apis] for package, name in read_api(path)}
        self.compiled = set().union(*(class_names(path) for path in known if os.path.exists(path)))
        self.needs = {}
        self.classes = class_names(classes)
        self.java, self.aidl = source_index(roots)
        self.ours = ours
        self.present = lambda qualified: qualified.replace(".", "/") in self.classes
        tops = sorted({(package, name) for package, name in self.types if "." not in name})
        self.tops = tops
        self.missing = [f"{package}.{name}" for package, name in tops if not self.present(f"{package}.{name}")]
        missing = set(self.missing)
        # A nested type framework.jar lacks under a top-level type it has.
        self.nested = []
        for package, name in self.types:
            top = f"{package}.{name.split('.')[0]}"
            binary = f"{package.replace('.', '/')}/{name.replace('.', '$')}"
            if "." in name and top not in missing and binary not in self.classes:
                self.nested.append((package, name))
        self.by_package = collections.defaultdict(set)  # the missing and hidden types' simple names, per package
        for qualified in missing:
            self.by_package[qualified.rsplit(".", 1)[0]].add(qualified.rsplit(".", 1)[1])
        for qualified in self.java:
            if not self.present(qualified):
                self.by_package[qualified.rsplit(".", 1)[0]].add(qualified.rsplit(".", 1)[1])
        self.own, self.uses, self.hidden = {}, {}, set()
        pending = list(self.missing)
        while pending:
            qualified = pending.pop()
            if qualified in self.own:
                continue
            self.own[qualified], self.uses[qualified] = self.read(qualified)
            for used in self.uses[qualified]:
                if used not in self.own:
                    if used not in missing:
                        self.hidden.add(used)
                    pending.append(used)
        self.group = {q: GROUPS[max([GROUPS.index(self.own[q])] + [GROUPS.index(self.own[h]) for h in self.pulls(q)])]
                      for q in self.own}

    def resolve(self, qualified):
        """The missing type, API or hidden, that a qualified name names or lies in; None for one framework.jar has or
        that no source defines."""
        parts = qualified.split(".")
        for end in range(len(parts), 1, -1):
            candidate = ".".join(parts[:end])
            if self.present(candidate):
                return None
            if candidate in self.java or candidate in self.missing_set():
                return candidate
        return None

    def known(self, qualified):
        """Whether a name, or a type it lies in, is framework.jar's, a stub's, a fetched source's or an AIDL
        interface's."""
        parts = qualified.split(".")
        for end in range(len(parts), 1, -1):
            candidate = ".".join(parts[:end])
            if (self.present(candidate) or candidate in self.stubbed or candidate in self.java
                    or candidate in self.aidl or candidate.replace(".", "/") in self.compiled):
                return True
        return False

    def missing_set(self):
        if not hasattr(self, "_missing_set"):
            self._missing_set = set(self.missing)
        return self._missing_set

    def read(self, qualified):
        """A type's own group and the missing types it uses."""
        path = self.java.get(qualified)
        if path is None:
            return "not fetched", set()
        with open(path, encoding="utf-8", errors="replace") as handle:
            code = NOISE.sub(" ", handle.read())
        package = qualified.rsplit(".", 1)[0]
        used, aidl = set(), False
        packages = [package]
        for match in IMPORT.finditer(code):
            name = match.group(2)
            if match.group(3):
                packages.append(name)
                continue
            if match.group(1):
                name = name.rsplit(".", 1)[0]
            if name in self.aidl:
                aidl = True
            found = self.resolve(name)
            if found:
                used.add(found)
            elif name.startswith(("android.", "com.android.")) and not self.known(name):
                self.needs.setdefault(qualified, set()).add(name)
        names = set(NAME.findall(code))
        for each in packages:
            for name in names & self.by_package.get(each, set()):
                used.add(f"{each}.{name}")
            aidl = aidl or any(f"{each}.{name}" in self.aidl for name in names)
        imported = {match.group(2).rsplit(".", 1)[-1] for match in IMPORT.finditer(code) if not match.group(3)}
        aidl = aidl or any(AIDL_NAME.match(name) and name not in imported
                           and name not in self.by_package.get(package, set())
                           and not self.known(f"{package}.{name}") for name in names)
        used.discard(qualified)
        if qualified in self.needs:
            return "module", used
        if NATIVE.search(code):
            return "native", used
        if aidl or self.reaches_service(code):
            return "service", used
        return "pure Java", used

    def reaches_service(self, code):
        """Whether code reaches a system service: through Binder, or getSystemService of one that ContextImpl does
        not answer. A declaration of getSystemService, whose argument is a parameter, reaches none."""
        if BINDER.search(code):
            return True
        for match in SYSTEM_SERVICE.finditer(code):
            depth, end = 1, match.end()
            while end < len(code) and depth:
                depth += {"(": 1, ")": -1}.get(code[end], 0)
                end += 1
            argument = " ".join(code[match.end():end - 1].split())
            name, cls = SERVICE_NAME.fullmatch(argument), SERVICE_CLASS.fullmatch(argument)
            if name and name.group(1) in self.local_names or cls and cls.group(1) in self.local_classes:
                continue
            if PARAMETER.fullmatch(argument):
                continue
            return True
        return False

    def pulls(self, qualified):
        """The hidden types a type pulls in: those it reaches through hidden ones only."""
        seen, pending = set(), [u for u in self.uses.get(qualified, ()) if u in self.hidden]
        while pending:
            hidden = pending.pop()
            if hidden in seen:
                continue
            seen.add(hidden)
            pending.extend(u for u in self.uses.get(hidden, ()) if u in self.hidden)
        return sorted(seen)

    def batches(self, first, size, taken=()):
        """The pure-Java types that no other step takes, in batches of up to `size`, whole packages where they fit."""
        pure = [q for q in self.missing if self.group[q] == "pure Java" and q not in taken]
        packages = collections.defaultdict(list)
        for qualified in pure:
            packages[qualified.rsplit(".", 1)[0]].append(qualified)
        demand = collections.Counter()
        for qualified in self.missing:
            for used in self.uses.get(qualified, ()):
                package = used.rsplit(".", 1)[0]
                if used in packages.get(package, ()) and package != qualified.rsplit(".", 1)[0]:
                    demand[package] += 1
        order = []
        for qualified in first:
            package = qualified.rsplit(".", 1)[0]
            if package in packages and package not in order:
                order.append(package)
        order += sorted((p for p in packages if p not in order), key=lambda p: (-demand[p], p))
        batches, current = [], []
        for package in order:
            ordered, seen = [], set()

            def visit(qualified):
                if qualified in seen:
                    return
                seen.add(qualified)
                for used in sorted(self.uses.get(qualified, ())):
                    if used in packages[package]:
                        visit(used)
                ordered.append(qualified)

            for qualified in sorted(packages[package]):
                visit(qualified)
            if current and len(current) + len(ordered) > size:
                batches.append(sorted(current))
                current = []
            while len(ordered) > size:
                batches.append(sorted(ordered[:size]))
                ordered = ordered[size:]
            current += ordered
        if current:
            batches.append(sorted(current))
        return batches


def main():
    arguments = sys.argv[1:]
    options = {"--api": os.path.join(REPO, "build", "sources", "frameworks_base_api", "core", "api", "current.txt"),
               "--classes": os.path.join(REPO, "build", "framework", "classes"),
               "--ours": os.path.join(REPO, "framework", "java"),
               "--first": os.path.join(REPO, "fixtures", "corpus", "api-first.txt"),
               "--taken": os.path.join(REPO, "fixtures", "corpus", "api-taken.txt"), "--batch": "60",
               "--context-impl": os.path.join(REPO, "framework", "managers", "android", "app", "ContextImpl.java"),
               "--service-registry": os.path.join(REPO, "framework", "java", "android", "app",
                                                  "SystemServiceRegistry.java"),
               "--table": os.path.join(REPO, "docs", "corpus", "api-coverage.md"),
               "--json": os.path.join(REPO, "build", "api-coverage.json")}
    roots, stub_apis, known, modules = [], [], [], []
    while arguments:
        repeated = ["--sources", "--stub-api", "--known", "--module-api"]
        if (len(arguments) < 2 or arguments[0] not in list(options) + repeated
                or arguments[0] == "--module-api" and not all(arguments[1].partition("=")[::2])):
            raise SystemExit(__doc__.split("Usage: ")[1].strip())
        if arguments[0] == "--sources":
            roots.append(arguments[1])
        elif arguments[0] == "--stub-api":
            stub_apis.append(arguments[1])
        elif arguments[0] == "--known":
            known.append(arguments[1])
        elif arguments[0] == "--module-api":
            modules.append(tuple(arguments[1].partition("=")[::2]))
        else:
            options[arguments[0]] = arguments[1]
        arguments = arguments[2:]
    if not stub_apis:
        # The API files tools/build-framework.sh makes the compile-only stubs of, besides frameworks/base's.
        sources = os.path.join(REPO, "build", "sources")
        stub_apis = [os.path.join(sources, *path.split("/")) for path in (
            "libcore/api/current.txt", "icu/android_icu4j/api/public/current.txt", *(p for _, p in MODULE_APIS))]
        stub_apis = [path for path in stub_apis if os.path.exists(path)]
    if not modules:
        sources = os.path.join(REPO, "build", "sources")
        modules = [(name, os.path.join(sources, *path.split("/"))) for name, path in MODULE_APIS]
        modules = [(name, path) for name, path in modules if os.path.exists(path)]
    if not known:
        framework = os.path.dirname(os.path.abspath(options["--classes"]))
        known = [os.path.join(framework, "stub-classes"), os.path.join(framework, "jmod", "java.base.jmod")]
    if not roots:
        sources = os.path.join(REPO, "build", "sources")
        roots = [os.path.join(sources, "frameworks_base", "core", "java"),
                 os.path.join(sources, "frameworks_base_graphics", "graphics", "java"),
                 os.path.join(sources, "frameworks_base", "opengl", "java")]
        # frameworks/base's other java folders, fetched for this (third_party/sources.json's frameworks_base_types):
        # media/java, telephony/java, apex/jobscheduler/framework/java and the rest, each a root of its own.
        types = os.path.join(sources, "frameworks_base_types")
        for directory, folders, _ in sorted(os.walk(types)):
            if os.path.basename(directory) == "java":
                roots.append(directory)
                folders[:] = []
    first, steps = [], []
    if os.path.exists(options["--first"]):
        with open(options["--first"]) as handle:
            first = [line.split("#")[0].strip() for line in handle if line.split("#")[0].strip()]
    if os.path.exists(options["--taken"]):
        with open(options["--taken"]) as handle:
            steps = [tuple(line.rstrip("\n").split("\t", 1)) for line in handle
                     if line.strip() and not line.startswith("#")]
    coverage = Coverage(options["--api"], options["--classes"], roots, options["--ours"], stub_apis, known,
                        local_services(options["--context-impl"], options["--service-registry"]))
    # Each module API's count of public types and its top-level ones, and the top-level ones framework.jar lacks.
    apart = {}
    for name, path in modules:
        each = read_api(path)
        apart[name] = (len(each), sorted({f"{package}.{simple}" for package, simple in each if "." not in simple}))
    apart_missing = [q for _, tops in apart.values() for q in tops if not coverage.present(q)]
    taken = {}
    for qualified in coverage.missing + apart_missing:
        for name, step in steps:
            if qualified == name or qualified.rsplit(".", 1)[0] == name:
                taken[qualified] = step
                break
    batches = coverage.batches(first, int(options["--batch"]), taken)
    ours = sorted(f"{package}.{name}" for package, name in coverage.nested
                  if os.path.exists(os.path.join(options["--ours"], *package.split("."), name.split(".")[0] + ".java")))
    missing = set(coverage.missing)
    types = {q: {"group": coverage.group[q], "uses": sorted(coverage.uses.get(q, ())), "pulls": coverage.pulls(q),
                 "stubs": sorted(u for u in coverage.uses.get(q, ()) if u in missing),
                 "source": coverage.java.get(q), "taken": taken.get(q),
                 "needs": sorted({n for h in [q] + coverage.pulls(q) for n in coverage.needs.get(h, ())})}
             for q in coverage.missing}
    os.makedirs(os.path.dirname(os.path.abspath(options["--json"])), exist_ok=True)
    with open(options["--json"], "w") as handle:
        json.dump({"types": types, "batches": batches, "ours": ours,
                   "nested": sorted({f"{p}.{n}" for p, n in coverage.nested} - set(ours)),
                   "modules": {name: {"api": path, "present": [q for q in apart[name][1] if coverage.present(q)],
                                      "missing": {q: {"taken": taken.get(q)} for q in apart[name][1]
                                                  if not coverage.present(q)}}
                               for name, path in modules}}, handle, indent=1)
        handle.write("\n")

    counts = collections.Counter(coverage.group[q] for q in coverage.missing)
    rows = collections.defaultdict(lambda: collections.Counter())
    for package, name in coverage.tops:
        qualified = f"{package}.{name}"
        rows[package]["present" if qualified not in types else coverage.group[qualified]] += 1
        rows[package]["missing"] += qualified in types
    lines = ["# The framework's API coverage", "",
             "Written by tools/api-coverage.py from Android 15's API (frameworks/base's core/api/current.txt), its",
             "module APIs and framework.jar's classes, for the",
             "[framework breadth plan](../plans/2026-10-07-framework-breadth.md).",
             "Do not edit by hand.", "",
             f"The API has {len(coverage.types)} public types, {len(coverage.tops)} of them top-level. framework.jar "
             f"has {len(coverage.tops) - len(coverage.missing)} of these; {len(coverage.missing)} are missing: "
             + ", ".join(f"{counts[g]} {g}" for g in GROUPS) + f". Our own types lack {len(ours)} nested ones.", "",
             "## Packages", "",
             "| Package | In framework.jar | Missing | " + " | ".join(g[0].upper() + g[1:] for g in GROUPS) + " |",
             "|---|---|---|" + "---|" * len(GROUPS)]
    for package in sorted(rows, key=lambda p: (-rows[p]["missing"], p)):
        row = rows[package]
        if row["missing"]:
            lines.append(f"| {package} | {row['present']} | {row['missing']} | "
                         + " | ".join(str(row[g]) for g in GROUPS) + " |")
    tops = sum(len(module_tops) for _, module_tops in apart.values())
    lines += ["", "## Module APIs", "",
              "The APIs that Android 15 builds apart from frameworks/base's core one, each a library of its own: "
              + (", ".join(apart) or "none") + ". The build makes compile-only stubs of their types. No batch takes "
              "them, since their sources lie in other projects: our own code does, or a later fetch.", ""]
    if apart:
        lines += [f"The module APIs have {sum(count for count, _ in apart.values())} public types, {tops} of them "
                  f"top-level. framework.jar has {tops - len(apart_missing)} of these; {len(apart_missing)} are "
                  "missing.", "", "| Module | Package | In framework.jar | Missing | Missing types |",
                  "|---|---|---|---|---|"]
    for name, (_, module_tops) in apart.items():
        present, absent = collections.Counter(), collections.defaultdict(list)
        for qualified in module_tops:
            package, simple = qualified.rsplit(".", 1)
            if coverage.present(qualified):
                present[package] += 1
            else:
                absent[package].append(simple + (f" ({taken[qualified]})" if qualified in taken else ""))
        for package in sorted(absent, key=lambda p: (-len(absent[p]), p)):
            lines.append(f"| {name} | {package} | {present[package]} | {len(absent[package])} | "
                         + ", ".join(absent[package]) + " |")
    lines += ["", "## Batches of the pure-Java types", ""]
    for number, batch in enumerate(batches, 1):
        pulled = sorted({p for q in batch for p in coverage.pulls(q)})
        packages = sorted({q.rsplit(".", 1)[0] for q in batch})
        lines.append(f"{number}. {', '.join(packages)} ({len(batch)}"
                     + (f", pulling in {len(pulled)} hidden" if pulled else "") + "): "
                     + ", ".join(q.rsplit(".", 1)[1] for q in batch))
    lines += ["", "## Needs a module", "",
              "What each imports, itself or through a hidden type it pulls in, that nothing here defines.", ""]
    lines += [f"- {q}: {', '.join(types[q]['needs'][:6])}" + (", …" if len(types[q]["needs"]) > 6 else "")
              for q in sorted(types) if types[q]["group"] == "module"] or ["None."]
    lines += ["", "## Not fetched", "", "Their sources lie beyond build/sources: a later fetch, with the user's go.",
              ""]
    unfetched = collections.defaultdict(list)
    for qualified in coverage.missing:
        if coverage.java.get(qualified) is None:
            unfetched[qualified.rsplit(".", 1)[0]].append(qualified.rsplit(".", 1)[1])
    lines += [f"- {package}: {', '.join(sorted(names))}" for package, names in sorted(unfetched.items())] or ["None."]
    lines += ["", "## Taken by other steps", ""]
    by_step = collections.defaultdict(list)
    for qualified, step in taken.items():
        by_step[step].append(qualified)
    for step in sorted(by_step):
        lines.append(f"- {step}: {len(by_step[step])} ("
                     + ", ".join(sorted(q.rsplit('.', 1)[1] for q in by_step[step])[:12])
                     + (", …" if len(by_step[step]) > 12 else "") + ")")
    if not by_step:
        lines.append("None.")
    lines += ["", "## Nested types our own classes lack", ""]
    lines += [f"- {name}" for name in ours] or ["None."]
    with open(options["--table"], "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"{options['--table']}: {len(coverage.missing)} missing, " + ", ".join(f"{counts[g]} {g}" for g in GROUPS)
          + f"; {len(batches)} batches; {len(ours)} nested types of ours; module APIs: {len(apart_missing)} of {tops} "
          "top-level types missing")


if __name__ == "__main__":
    main()
