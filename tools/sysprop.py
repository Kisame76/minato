#!/usr/bin/env python3
"""Writes the C++ accessors of a .sysprop file (sysprop's text format) as AOSP's sysprop_cpp writes them for a cc
module (ADR-0043 a): <name>.sysprop.h with a getter per property in the module's namespace, and <name>.sysprop.cpp,
which reads the property (bionic's __system_property_read_callback, or libbase's GetProperty off bionic) and parses
it as sysprop does: an empty value is none, a Boolean is "1" or "true" and "0" or "false" in any case, Integer and
Long go through libbase's ParseInt. Read-only properties of the types Boolean, Integer, Long and String; anything
else is refused, so that a new use shows, and so is a property name that the getter's C++ string would have to escape.

  sysprop.py cc --out <directory> <file.sysprop>
"""
import argparse
import os
import re
import sys

TOKEN = re.compile(r'(?P<space>\s+)|(?P<comment>#[^\n]*)|(?P<string>"(?:[^"\\]|\\.)*")|'
                   r'(?P<name>[A-Za-z_][A-Za-z0-9_.]*)|(?P<number>-?\d+)|(?P<symbol>[{}:])')
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")

# sysprop's types and what a getter returns for them; and the fields of sysprop.proto this tool reads.
TYPES = {"Boolean": "std::optional<bool>", "Integer": "std::optional<std::int32_t>",
         "Long": "std::optional<std::int64_t>", "String": "std::optional<std::string>"}
UNSUPPORTED_TYPES = {"Double", "Enum", "BooleanList", "IntegerList", "LongList", "DoubleList", "StringList",
                     "EnumList", "UIntList", "UInt"}
FILE_FIELDS = {"owner", "module", "prop"}
PROP_FIELDS = {"api_name", "type", "prop_name", "scope", "access"}
UNSUPPORTED_FIELDS = {"enum_values", "integer_as_bool", "deprecated", "legacy_prop_name"}

PARSERS = {
    "std::optional<bool>": """\
template <>
std::optional<bool> DoParse(const char* str) {
    static constexpr const char* kYes[] = {"1", "true"};
    static constexpr const char* kNo[] = {"0", "false"};
    for (const char* yes : kYes) {
        if (strcasecmp(yes, str) == 0) return std::make_optional(true);
    }
    for (const char* no : kNo) {
        if (strcasecmp(no, str) == 0) return std::make_optional(false);
    }
    return std::nullopt;
}
""",
    "std::optional<std::int32_t>": """\
template <>
std::optional<std::int32_t> DoParse(const char* str) {
    std::int32_t ret;
    return android::base::ParseInt(str, &ret) ? std::make_optional(ret) : std::nullopt;
}
""",
    "std::optional<std::int64_t>": """\
template <>
std::optional<std::int64_t> DoParse(const char* str) {
    std::int64_t ret;
    return android::base::ParseInt(str, &ret) ? std::make_optional(ret) : std::nullopt;
}
""",
    "std::optional<std::string>": """\
template <>
std::optional<std::string> DoParse(const char* str) {
    return std::make_optional(std::string(str));
}
""",
}


class Refused(Exception):
    pass


def tokens(text):
    """(kind, value, line) of the text's tokens, without space and comments."""
    result, position, line = [], 0, 1
    while position < len(text):
        match = TOKEN.match(text, position)
        if match is None:
            raise Refused(f"line {line}: unexpected {text[position:position + 10]!r}")
        kind, value = match.lastgroup, match.group()
        if kind == "string":
            value = bytes(value[1:-1], "utf-8").decode("unicode_escape")
        if kind not in ("space", "comment"):
            result.append((kind, value, line))
        line += match.group().count("\n")
        position = match.end()
    result.append(("end", None, line))
    return result


def message(items, index, closing):
    """The fields of a message up to `closing` ("}" or the end) as (name, value) pairs; a nested message's value is
    its own list. Returns (fields, the index after it)."""
    fields = []
    while True:
        kind, value, line = items[index]
        if (closing == "}" and (kind, value) == ("symbol", "}")) or (closing == "end" and kind == "end"):
            return fields, index + 1
        if kind != "name":
            raise Refused(f"line {line}: expected a field or '{closing}'" if closing == "}" else
                          f"line {line}: expected a field")
        index += 1
        if items[index][:2] == ("symbol", ":"):
            index += 1
        if items[index][:2] == ("symbol", "{"):
            inner, index = message(items, index + 1, "}")
            fields.append((value, inner))
            continue
        kind, field_value, line = items[index]
        if kind not in ("string", "name", "number"):
            raise Refused(f"line {line}: {value} has no value")
        fields.append((value, field_value))
        index += 1


def read(path):
    """(module, [(api_name, C++ type, prop_name)]) of a .sysprop file."""
    with open(path) as handle:
        fields, _ = message(tokens(handle.read()), 0, "end")
    module, props = None, []
    for name, value in fields:
        if name not in FILE_FIELDS:
            raise Refused(f"unknown field {name}")
        if name == "module":
            module = value
        elif name == "prop":
            props.append(dict(value))
            for field, _ in value:
                if field in UNSUPPORTED_FIELDS:
                    raise Refused(f"field {field} is not supported")
                if field not in PROP_FIELDS:
                    raise Refused(f"unknown field {field}")
    if not module or not all(IDENTIFIER.match(part) for part in module.split(".")):
        raise Refused(f"the module {module!r} is not a dotted name")
    getters, names = [], set()
    for prop in props:
        api_name = prop.get("api_name", "")
        if not IDENTIFIER.match(api_name) or api_name in names:
            raise Refused(f"the api_name {api_name!r} is not a new identifier")
        names.add(api_name)
        kind = prop.get("type")
        if kind in UNSUPPORTED_TYPES or kind not in TYPES:
            raise Refused(f"type {kind} is not supported")
        if prop.get("access") != "Readonly":
            raise Refused(f"access {prop.get('access')} is not supported")
        prop_name = prop.get("prop_name")
        if not prop_name:
            raise Refused(f"{api_name} has no prop_name")
        if any(character in '"\\' or ord(character) < 0x20 or ord(character) == 0x7f for character in prop_name):
            raise Refused(f"the prop_name {prop_name!r} of {api_name} holds a quote, a backslash or a control "
                          "character, which the C++ string of its getter would have to escape")
        getters.append((api_name, TYPES[kind], prop_name))
    return module, getters


def cc(path, out):
    module, getters = read(path)
    name = os.path.basename(path)
    namespace = module.replace(".", "::")
    comment = f"// Generated by tools/sysprop.py from {name}. Do not edit.\n"
    header = comment + "#pragma once\n\n#include <cstdint>\n#include <optional>\n#include <string>\n\n"
    header += f"namespace {namespace} {{\n\n"
    header += "".join(f"{kind} {api_name}();\n\n" for api_name, kind, _ in getters)
    header += f"}}  // namespace {namespace}\n"
    kinds = [kind for kind in TYPES.values() if any(kind == used for _, used, _ in getters)]
    source = comment + f'#include "{name}.h"\n\n#include <strings.h>\n\n#include <cstdint>\n#include <optional>\n'
    source += "#include <string>\n\n#ifdef __BIONIC__\n#include <sys/system_properties.h>\n#else\n"
    source += "#include <android-base/properties.h>\n#endif\n"
    if any(kind.endswith("int32_t>") or kind.endswith("int64_t>") for kind in kinds):
        source += "#include <android-base/parseint.h>\n"
    source += "\nnamespace {\n\ntemplate <typename T>\nT DoParse(const char* str);\n\n"
    source += "\n".join(PARSERS[kind] for kind in kinds)
    source += """
template <typename T>
T GetProp(const char* key) {
    std::string value;
#ifdef __BIONIC__
    auto pi = __system_property_find(key);
    if (pi != nullptr) {
        __system_property_read_callback(
            pi,
            [](void* cookie, const char*, const char* value, std::uint32_t) {
                *static_cast<std::string*>(cookie) = value;
            },
            &value);
    }
#else
    value = android::base::GetProperty(key, "");
#endif
    return value.empty() ? T{} : DoParse<T>(value.c_str());
}

}  // namespace

"""
    source += f"namespace {namespace} {{\n\n"
    source += "".join(f'{kind} {api_name}() {{\n    return GetProp<{kind}>("{prop_name}");\n}}\n\n'
                      for api_name, kind, prop_name in getters)
    source += f"}}  // namespace {namespace}\n"
    os.makedirs(out, exist_ok=True)
    for suffix, text in ((".h", header), (".cpp", source)):
        with open(os.path.join(out, name + suffix), "w") as handle:
            handle.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    modes = parser.add_subparsers(dest="mode", required=True)
    cc_mode = modes.add_parser("cc", help="the C++ accessors, <name>.sysprop.h and <name>.sysprop.cpp")
    cc_mode.add_argument("--out", required=True)
    cc_mode.add_argument("sysprop")
    arguments = parser.parse_args()
    try:
        cc(arguments.sysprop, arguments.out)
    except (OSError, Refused) as error:
        print(f"sysprop: {arguments.sysprop}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
