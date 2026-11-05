#!/usr/bin/env python3
"""tools/api-coverage.py, the framework's breadth (the compatibility plan's entry 22): on an API file, a classes folder
and a source folder this test writes, the missing public types by group, with the closure over the missing types a
source uses, the nested types our own top-level types lack, the batches and the table, and a module API's types
apart. Python 3.9."""
import json
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "api-coverage.py")
failures = 0

API = """// Signature format: 2.0
package a.b {

  public class Present {
    ctor public Present();
  }

  public static class Present.Nested {
  }

  public final class Plain {
    method public void run();
  }

  @Deprecated public class UsesPlain {
  }

  @FlaggedApi("a.b.flag") public interface Native {
  }

  public class UsesNative {
  }

  public class Served {
  }

  public class Aidl {
  }

  public class UsesHidden {
  }

  public class Cycle1 {
  }

  public class Cycle2 {
  }

  public class Elsewhere {
  }

  public class Parceled {
  }

  public class UsesModule {
  }

  public class UsesStub {
  }

  public class Dumper {
  }

  public class UsesElsewhere {
  }

  public class Archived {
  }

}

package c.d {

  public abstract class Other {
  }

  public static interface Other.Callback {
  }

}

package e.f {

  @android.widget.RemoteViews.RemoteView public class Remote {
  }

  @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME) public @interface Marked {
  }

}

package g.h {

  public class Inflating {
  }

  public class Typing {
  }

  public class Wifi {
  }

  public class Named {
  }

  public class Registered {
  }

}

"""

SOURCES = {
    "a/b/Plain.java": "package a.b;\n/** Native. */\npublic final class Plain { String s = \"native x(\"; }\n",
    "a/b/UsesPlain.java": "package a.b;\npublic class UsesPlain { Plain plain; }\n",
    "a/b/Native.java": ("package a.b;\npublic interface Native { }\n"
                        "class Impl { private static native int open(String path); }\n"),
    "a/b/UsesNative.java": "package a.b;\nimport c.d.Other;\npublic class UsesNative { Native n; Other o; }\n",
    "a/b/Served.java": "package a.b;\nimport android.os.ServiceManager;\npublic class Served { }\n",
    "a/b/Aidl.java": "package a.b;\npublic class Aidl { IThing thing; }\n",
    "a/b/IThing.aidl": "package a.b;\ninterface IThing { }\n",
    "a/b/Box.aidl": "package a.b;\n/** Not an interface. */\nparcelable Box;\n",
    "a/b/Parceled.java": ("package a.b;\nimport android.os.IBinder;\n"
                          "public class Parceled { Box box; IBinder token; }\n"),
    "a/b/Box.java": "package a.b;\nclass Box { }\n",
    "a/b/UsesModule.java": ("package a.b;\nimport java.util.List;\nimport android.hardware.thing.V1_0.Strength;\n"
                           "public class UsesModule { }\n"),
    "a/b/Dumper.java": ("package a.b;\nimport java.io.IOException;\n"
                       "public class Dumper { IRemote remote; IOException e; Integer i; }\n"),
    "a/b/UsesStub.java": ("package a.b;\nimport x.y.Known;\nimport com.android.internal.R;\n"
                         "import android.compat.annotation.UnsupportedAppUsage;\npublic class UsesStub { }\n"),
    "a/b/UsesHidden.java": "package a.b;\nimport a.b.internal.Helper;\npublic class UsesHidden { Helper h; }\n",
    "a/b/internal/Helper.java": "package a.b.internal;\npublic class Helper { native void x(); }\n",
    "a/b/Cycle1.java": "package a.b;\npublic class Cycle1 { Cycle2 c; }\n",
    "a/b/Cycle2.java": "package a.b;\npublic class Cycle2 { Cycle1 c; }\n",
    "a/b/UsesElsewhere.java": "package a.b;\npublic class UsesElsewhere { Elsewhere e; }\n",
    "a/b/Archived.java": "package a.b;\npublic class Archived { ArchivedParcel parcel; }\n",
    "a/b/ArchivedParcel.aidl": ("package a.b;\nimport a.b.internal.Details;\n"
                                "parcelable ArchivedParcel {\n    Details details;\n}\n"),
    "a/b/internal/Details.java": "package a.b.internal;\npublic class Details { }\n",
    "c/d/Other.java": "package c.d;\npublic abstract class Other { public interface Callback { } }\n",
    "e/f/Remote.java": "package e.f;\npublic class Remote { }\n",
    "e/f/Marked.java": "package e.f;\npublic @interface Marked { }\n",
    "g/h/Inflating.java": ("package g.h;\npublic class Inflating {\n    Object of(Context c) {\n"
                           "        return c.getSystemService(Context.LAYOUT_INFLATER_SERVICE);\n    }\n}\n"),
    "g/h/Typing.java": ("package g.h;\npublic class Typing {\n"
                        "    Object of(Context c) { return c.getSystemService(InputMethodManager.class); }\n}\n"),
    "g/h/Wifi.java": ("package g.h;\npublic class Wifi {\n"
                      "    Object of(Context c) { return c.getSystemService(Context.WIFI_SERVICE); }\n}\n"),
    "g/h/Named.java": ("package g.h;\npublic class Named {\n"
                       "    Object of(Context c, String name) { return c.getSystemService(name); }\n}\n"),
    "g/h/Registered.java": ("package g.h;\npublic class Registered {\n"
                            "    Object of(Context c) { return c.getSystemService(Context.POWER_SERVICE); }\n"
                            "    Object by(Context c) { return c.getSystemService(DisplayManager.class); }\n}\n"),
}

# framework.jar has a.b.Present, our own, without its nested type, and nothing else of the API; the hidden R; and two of
# the module API's types.
CLASSES = ["a/b/Present.class", "x/y/Ours.class", "com/android/internal/R.class", "android/os/IBinder.class",
           "android/os/ServiceManager.class", "g/h/Counted.class", "p/q/Whole.class"]
# A module's API, of which the build makes compile-only stubs too.
STUB_API = "package x.y {\n\n  public class Known {\n  }\n\n}\n"
# A module's API (--module-api), counted apart from the core one: framework.jar has its Counted, in a package the core
# API has too, and all of p.q, and lacks Uncounted and Far, which another step takes.
MODULE_API = """// Signature format: 2.0
package g.h {

  public class Counted {
  }

  public static class Counted.Inner {
  }

  public class Uncounted {
  }

}

package m.n {

  public final class Far {
  }

}

package p.q {

  public class Whole {
  }

}

"""


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {what}")


with tempfile.TemporaryDirectory() as work:
    api = os.path.join(work, "current.txt")
    with open(api, "w") as handle:
        handle.write(API)
    classes = os.path.join(work, "classes")
    for path in CLASSES:
        os.makedirs(os.path.dirname(os.path.join(classes, path)), exist_ok=True)
        open(os.path.join(classes, path), "w").close()
    sources = os.path.join(work, "java")
    for path, text in SOURCES.items():
        os.makedirs(os.path.dirname(os.path.join(sources, path)), exist_ok=True)
        with open(os.path.join(sources, path), "w") as handle:
            handle.write(text)
    ours = os.path.join(work, "ours")
    os.makedirs(os.path.join(ours, "a", "b"))
    open(os.path.join(ours, "a", "b", "Present.java"), "w").close()
    taken = os.path.join(work, "taken.txt")
    with open(taken, "w") as handle:
        handle.write("# Other steps\na.b.Cycle2\tgap 7 (SQLite)\na\tno package of this name has types\n"
                     "m.n.Far\t(9), a module step\n")
    context_impl = os.path.join(work, "ContextImpl.java")
    with open(context_impl, "w") as handle:
        handle.write("class ContextImpl {\n    Object getSystemService(String name) {\n"
                     "        if (Context.LAYOUT_INFLATER_SERVICE.equals(name)) {\n            return null;\n"
                     "        }\n        return null;\n    }\n"
                     "    String getSystemServiceName(Class<?> serviceClass) {\n"
                     "        if (serviceClass == InputMethodManager.class) {\n            return null;\n"
                     "        }\n        return null;\n    }\n}\n")
    registry = os.path.join(work, "SystemServiceRegistry.java")
    with open(registry, "w") as handle:
        handle.write("class SystemServiceRegistry {\n    static {\n"
                     "        cached(Context.POWER_SERVICE, PowerManager.class, ctx -> new PowerManager(ctx));\n"
                     "        register(Context.DISPLAY_SERVICE, DisplayManager.class, false,\n"
                     "                ctx -> new DisplayManager(ctx));\n"
                     "        // cached(Context.WIFI_SERVICE, WifiManager.class, ctx -> null);\n    }\n}\n")
    jmod = os.path.join(work, "java.base.jmod")
    with zipfile.ZipFile(jmod, "w") as archive:
        archive.writestr("classes/android/compat/annotation/UnsupportedAppUsage.class", b"")
    stub_api = os.path.join(work, "module-current.txt")
    with open(stub_api, "w") as handle:
        handle.write(STUB_API)
    module_api = os.path.join(work, "module-api.txt")
    with open(module_api, "w") as handle:
        handle.write(MODULE_API)
    first = os.path.join(work, "first.txt")
    with open(first, "w") as handle:
        handle.write("# the Unity test APK's blockers\nc.d.Other\n")
    table = os.path.join(work, "api-coverage.md")
    data = os.path.join(work, "api-coverage.json")
    result = subprocess.run([sys.executable, TOOL, "--api", api, "--classes", classes, "--sources", sources,
                             "--stub-api", stub_api, "--known", jmod, "--ours", ours, "--first", first,
                             "--taken", taken, "--batch", "3", "--context-impl", context_impl,
                             "--service-registry", registry,
                             "--module-api", f"mod={module_api}",
                             "--table", table, "--json", data],
                            capture_output=True, text=True)
    check(result.returncode == 0, f"exit status {result.returncode}: {result.stderr.strip()[-400:]}")
    report = json.load(open(data)) if os.path.exists(data) else {"types": {}, "batches": [], "ours": []}
    groups = {name: entry["group"] for name, entry in report["types"].items()}
    check(groups.get("a.b.Plain") == "pure Java", f"Plain is pure Java, not {groups.get('a.b.Plain')}")
    check(groups.get("a.b.UsesPlain") == "pure Java", "UsesPlain, which only uses Plain, is pure Java")
    check(groups.get("a.b.Native") == "native", "Native declares a native method in its file")
    check(groups.get("a.b.UsesNative") == "pure Java", "UsesNative is pure Java: Native stays a stub until batched")
    check(report["types"].get("a.b.UsesNative", {}).get("stubs") == ["a.b.Native", "c.d.Other"],
          "UsesNative's stub references are Native and Other")
    check(groups.get("a.b.Served") == "service", "Served reaches ServiceManager")
    check(groups.get("a.b.Aidl") == "service", "Aidl uses an AIDL interface of its package")
    check(groups.get("a.b.Parceled") == "pure Java", "a parcelable's .aidl and an IBinder are no service")
    check(groups.get("a.b.UsesModule") == "module", "UsesModule imports an android type nothing defines")
    check(groups.get("a.b.Dumper") == "service", "Dumper names an AIDL interface its package lacks")
    check(report["types"].get("a.b.UsesModule", {}).get("needs") == ["android.hardware.thing.V1_0.Strength"],
          "UsesModule needs the HAL's type")
    check(groups.get("a.b.UsesStub") == "pure Java",
          "a module API's stub, framework.jar's R and a boot class are known")
    check(groups.get("a.b.UsesHidden") == "native", "UsesHidden pulls in a hidden type with a native method")
    check(report["types"].get("a.b.UsesHidden", {}).get("pulls") == ["a.b.internal.Helper"],
          "UsesHidden pulls in a.b.internal.Helper")
    check(groups.get("a.b.Cycle1") == "pure Java" and groups.get("a.b.Cycle2") == "pure Java",
          "a cycle of plain types stays pure Java")
    check(groups.get("a.b.Elsewhere") == "not fetched", "Elsewhere has no source")
    check(groups.get("a.b.UsesElsewhere") == "pure Java", "UsesElsewhere's own source is plain; Elsewhere is a stub")
    check(groups.get("c.d.Other") == "pure Java", "Other is pure Java; its nested type comes with it")
    check(groups.get("e.f.Remote") == "pure Java" and groups.get("e.f.Marked") == "pure Java",
          "a type whose annotation has a qualified name, with or without arguments, is in the API")
    check(groups.get("g.h.Inflating") == "pure Java" and groups.get("g.h.Typing") == "pure Java",
          "a system service that ContextImpl answers, by its name or its class, is no service: "
          f"{groups.get('g.h.Inflating')}, {groups.get('g.h.Typing')}")
    check(groups.get("g.h.Registered") == "pure Java",
          "a service the service registry registers, by its name or its class, is no service: "
          f"{groups.get('g.h.Registered')}")
    check(groups.get("g.h.Wifi") == "service" and groups.get("g.h.Named") == "service",
          "one that ContextImpl does not answer, or a name in a variable, is a service: "
          f"{groups.get('g.h.Wifi')}, {groups.get('g.h.Named')}")
    check("a.b.Present" not in groups and "c.d.Other.Callback" not in groups,
          "only missing top-level types are listed")
    check(report["ours"] == ["a.b.Present.Nested"], f"our Present lacks its nested type: {report['ours']}")
    batches = report["batches"]
    check(batches[:1] == [["c.d.Other"]], f"the first batch is --first's package: {batches[:1]}")
    check(batches[1:] == [["a.b.Archived", "a.b.Cycle1", "a.b.Parceled"], ["a.b.Plain", "a.b.UsesElsewhere",
                                                                           "a.b.UsesNative"],
                          ["a.b.UsesPlain", "a.b.UsesStub"], ["e.f.Marked", "e.f.Remote"],
                          ["g.h.Inflating", "g.h.Registered", "g.h.Typing"]],
          f"then a.b's pure-Java types but the taken Cycle2, up to 3 a batch, each after what it uses, then e.f's "
          f"and g.h's: {batches[1:]}")
    check(report["types"].get("a.b.Cycle2", {}).get("taken") == "gap 7 (SQLite)", "Cycle2 is taken by gap 7")
    check(report["types"].get("a.b.Parceled", {}).get("pulls") == ["a.b.Box"], "Parceled pulls in the hidden Box")
    check(groups.get("a.b.Archived") == "pure Java", "a structured parcelable's .aidl is plain Java")
    check(report["types"].get("a.b.Archived", {}).get("pulls") == ["a.b.ArchivedParcel", "a.b.internal.Details"],
          "Archived pulls in the structured parcelable aidl makes and what it imports")
    text = open(table).read() if os.path.exists(table) else ""
    check("| a.b | 1 | 16 | 9 | 3 | 2 | 1 | 1 |" in text, "the table's row for a.b")
    check("- a.b.UsesModule: android.hardware.thing.V1_0.Strength" in text, "the table lists what a type needs")
    check("Do not edit by hand." in text, "the table says it is generated")
    check("- gap 7 (SQLite): 1 (Cycle2)" in text, "the table lists the taken type with its step")
    check("- a.b: Elsewhere" in text, "the table lists the type without a source")
    module = report.get("modules", {}).get("mod", {})
    check(module.get("present") == ["g.h.Counted", "p.q.Whole"], f"framework.jar has Counted and Whole: {module}")
    check(module.get("missing") == {"g.h.Uncounted": {"taken": None}, "m.n.Far": {"taken": "(9), a module step"}},
          f"the module lacks Uncounted, and Far, which (9) takes: {module}")
    check("g.h.Uncounted" not in groups and "m.n.Far" not in groups, "the module's types are not the core API's")
    check("The module APIs have 5 public types, 4 of them top-level. framework.jar has 2 of these; 2 are missing."
          in text, "the table counts the module API's types")
    check("| mod | g.h | 1 | 1 | Uncounted |" in text and "| mod | m.n | 0 | 1 | Far ((9), a module step) |" in text,
          "the module table's rows name the missing types, with the step that takes one")
    check("| mod | p.q |" not in text, "a module package that framework.jar has whole has no row")
    check("| g.h | 0 | 5 | 3 | 2 | 0 | 0 | 0 |" in text, "the core table's row for g.h leaves the module's types out")
    check("- (9), a module step: 1 (Far)" in text, "the table lists the taken module type with its step")

if failures:
    print(f"{failures} failed")
    sys.exit(1)
print("api-coverage: groups, closure, nested types, batches, table and module APIs as expected")
