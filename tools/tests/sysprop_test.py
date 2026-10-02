#!/usr/bin/env python3
"""tools/sysprop.py (ADR-0043 a): from a .sysprop file (sysprop's text format) it writes the C++ accessors that
AOSP's sysprop_cpp writes for a cc module, <name>.sysprop.h and <name>.sysprop.cpp, for read-only properties of the
types Boolean, Integer, Long and String. Each getter reads its property (bionic's __system_property_read_callback, or
libbase's GetProperty off bionic) and parses it as sysprop does: an empty value is none, a Boolean is "1" or "true"
and "0" or "false" in any case, Integer and Long go through libbase's ParseInt. Checked on the tag's two files, by
compiling the output as C++20 with -Werror against libbase's headers on the Mac and running it with a GetProperty of
the test's; and on what it must refuse: other types, other access, a property without its name, broken syntax, a
property name that a C++ string would have to escape (a quote, a backslash, a control character)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "sysprop.py")
SOURCES = os.path.join(ROOT, "build", "sources")
HWUI = os.path.join(SOURCES, "frameworks_base_hwui", "libs", "hwui", "HWUIProperties.sysprop")
INCREMENTAL = os.path.join(SOURCES, "incremental_delivery", "sysprop", "IncrementalProperties.sysprop")
LIBBASE = os.path.join(SOURCES, "libbase", "include")
failures = 0

LONG = """owner: Platform
module: "minato.test"
prop {
    api_name: "big"
    type: Long
    prop_name: "ro.minato.big"
    scope: Internal
    access: Readonly
}
"""

# The property values the generated getters see: a GetProperty of the test's in place of libbase's.
CALLER = r"""#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>

#include "HWUIProperties.sysprop.h"
#include "IncrementalProperties.sysprop.h"
#include "Long.sysprop.h"

static std::map<std::string, std::string> values;

namespace android::base {
std::string GetProperty(const std::string& key, const std::string& default_value) {
    const auto found = values.find(key);
    return found != values.end() ? found->second : default_value;
}
}  // namespace android::base

template <typename T>
static std::string text(const std::optional<T>& value) {
    if (!value) return "none";
    if constexpr (std::is_same_v<T, bool>) return *value ? "true" : "false";
    else if constexpr (std::is_same_v<T, std::string>) return "\"" + *value + "\"";
    else return std::to_string(*value);
}

int main() {
    std::printf("unset: %s %s %s %s\n", text(android::uirenderer::use_vulkan()).c_str(),
                text(android::uirenderer::render_ahead()).c_str(),
                text(android::sysprop::IncrementalProperties::enable()).c_str(), text(minato::test::big()).c_str());
    for (const char* value : {"true", "TRUE", "1", "0", "False", "yes", ""}) {
        values["ro.hwui.use_vulkan"] = value;
        std::printf("use_vulkan \"%s\": %s\n", value, text(android::uirenderer::use_vulkan()).c_str());
    }
    for (const char* value : {"2", "-1", "0x10", " 3", "3 ", "abc", "2147483648"}) {
        values["ro.hwui.render_ahead"] = value;
        std::printf("render_ahead \"%s\": %s\n", value, text(android::uirenderer::render_ahead()).c_str());
    }
    for (const char* value : {"1", ""}) {
        values["ro.incremental.enable"] = value;
        std::printf("enable \"%s\": %s\n", value, text(android::sysprop::IncrementalProperties::enable()).c_str());
    }
    values["ro.minato.big"] = "9000000000";
    std::printf("big \"9000000000\": %s\n", text(minato::test::big()).c_str());
    return 0;
}
"""

EXPECTED = """unset: none none none none
use_vulkan "true": true
use_vulkan "TRUE": true
use_vulkan "1": true
use_vulkan "0": false
use_vulkan "False": false
use_vulkan "yes": none
use_vulkan "": none
render_ahead "2": 2
render_ahead "-1": -1
render_ahead "0x10": 16
render_ahead " 3": 3
render_ahead "3 ": none
render_ahead "abc": none
render_ahead "2147483648": none
enable "1": "1"
enable "": none
big "9000000000": 9000000000
"""


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL {what}")


def run(*arguments):
    return subprocess.run([sys.executable, TOOL] + list(arguments), capture_output=True, text=True)


def refused(scratch, name, text, message):
    path = os.path.join(scratch, name + ".sysprop")
    with open(path, "w") as handle:
        handle.write(text)
    result = run("cc", "--out", os.path.join(scratch, "refused"), path)
    output = result.stdout + result.stderr
    check(result.returncode == 1 and message in output and "Traceback" not in output,
          f"{name}: exit {result.returncode}, wanted 1 with {message!r}:\n{output}")


def main():
    for path, listed in ((HWUI, "graphics"), (INCREMENTAL, "framework"), (LIBBASE, "art")):
        if not os.path.exists(path):
            print(f"skipped: {path} is missing, see tools/fetch-sources.py --set {listed}")
            return 0
    with tempfile.TemporaryDirectory() as scratch:
        out = os.path.join(scratch, "out")
        long_path = os.path.join(scratch, "Long.sysprop")
        with open(long_path, "w") as handle:
            handle.write(LONG)
        for path in (HWUI, INCREMENTAL, long_path):
            result = run("cc", "--out", out, path)
            check(result.returncode == 0, f"{os.path.basename(path)}: exit {result.returncode}:\n{result.stderr}")
        header = os.path.join(out, "HWUIProperties.sysprop.h")
        text = open(header).read() if os.path.exists(header) else ""
        check("namespace android::uirenderer {" in text, "the header's namespace is the module's")
        check("std::optional<bool> use_vulkan();" in text and "std::optional<std::int32_t> render_ahead();" in text,
              f"the header declares both getters:\n{text}")
        if failures == 0:
            caller = os.path.join(scratch, "caller.cpp")
            with open(caller, "w") as handle:
                handle.write(CALLER)
            program = os.path.join(scratch, "caller")
            sources = [caller] + [os.path.join(out, name + ".sysprop.cpp")
                                  for name in ("HWUIProperties", "IncrementalProperties", "Long")]
            compiled = subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", out, "-I", LIBBASE,
                                       "-o", program] + sources, capture_output=True, text=True)
            check(compiled.returncode == 0, f"the output compiles with -Werror:\n{compiled.stderr}")
            if compiled.returncode == 0:
                output = subprocess.run([program], capture_output=True, text=True).stdout
                check(output == EXPECTED, f"the getters parse as sysprop does:\n{output}")

        prop = 'prop {\n    api_name: "x"\n    type: %s\n    prop_name: "ro.x"\n    scope: Internal\n    access: %s\n}\n'
        head = 'owner: Platform\nmodule: "minato.test"\n'
        refused(scratch, "Double", head + prop % ("Double", "Readonly"), "type Double is not supported")
        refused(scratch, "Enum", head + prop % ("Enum", "Readonly"), "type Enum is not supported")
        refused(scratch, "ReadWrite", head + prop % ("Boolean", "ReadWrite"), "access ReadWrite is not supported")
        refused(scratch, "Nameless", head + 'prop {\n    api_name: "x"\n    type: Boolean\n    access: Readonly\n}\n',
                "x has no prop_name")
        refused(scratch, "Broken", head + 'prop {\n    api_name: "x"\n', "expected a field or '}'")
        refused(scratch, "Unknown", head + prop.replace("scope", "colour") % ("Boolean", "Readonly"),
                "unknown field colour")
        for name, prop_name in (("Quote", '"ro.\\"x"'), ("Backslash", '"ro.\\\\x"'), ("Newline", '"ro.\\nx"')):
            refused(scratch, name, head + prop.replace('"ro.x"', prop_name) % ("Boolean", "Readonly"),
                    "holds a quote, a backslash or a control character")
    print(f"sysprop: {'all checks passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
