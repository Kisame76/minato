#!/usr/bin/env python3
"""The NDK libraries Minato writes itself (ADR-0028, decisions 7, 9 and 12): their version scripts, and a
stub for every function that is not written by hand.

A library exports every name of its NDK stub (sysroot/usr/lib/aarch64-linux-android/35/<library>.so),
since an app that imports a name the library lacks cannot link at all. The names written by hand are
listed in LIBRARIES; every other function becomes a stub that logs its name once and answers what means
"nothing" or "failed": NULL for a pointer, 0 for a float, nothing for void, and for an integer the value
in VALUES, which every such stub must have: 0 for a count or a yes-or-no answer, the API's error code
where it has one, -1 otherwise. VALUES may also give a float or a pointer another value. The return types
come from the NDK's headers, read with clang's AST dump; KINDS gives those the headers do not declare.

A stub is compiled without the NDK's headers, as `<kind> name(void)`: under the AArch64 calling
convention a function may ignore its arguments, and only the kind of the result decides where it goes
(x0 for integers and pointers, s0 or d0 for floats). No stubbed function returns a structure.

Usage:
  stubgen.py generate <library> --sysroot <dir> --clang <clang> --nm <llvm-nm> --map <out.map> [--stubs <out.c>]
      [--names <out.txt>]   (the public names, one per line, for llvm-objcopy)
  stubgen.py check [--ndk <dir>] [--built <dir>]
      checks the tables against the NDK and, with --built, that the built libraries export exactly
      the names they should, with their versions. Prints "skipped: ..." without an NDK.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile

API = 35
TRIPLE = "aarch64-linux-android"

# Per library: the version of its public names (None: none, as the NDK's stub has none), the names
# written by hand (patterns), and its private names, version APKRT_PRIVATE.
LIBRARIES = {
    "libandroid": {
        "version": "LIBANDROID",
        "implemented": [
            "ALooper_*",
            "AConfiguration_*",
            # the asset manager (asset_manager.cpp); AAssetManager_fromJava stays a stub until Java (M4)
            "AAsset_*", "AAssetDir_*", "AAssetManager_open", "AAssetManager_openDir",
            # the requests (native_activity.cpp)
            "ANativeActivity_*",
            # the input queue without events (input_queue.cpp); its events come with M3 step 6
            "AInputQueue_attachLooper", "AInputQueue_detachLooper",
            # forwarded to libnativewindow (window_forwards.c)
            "ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_getWidth", "ANativeWindow_getHeight",
            "ANativeWindow_getFormat", "ANativeWindow_setBuffersGeometry", "ANativeWindow_lock",
            "ANativeWindow_unlockAndPost",
        ],
        # M3 has no resource matching. libandroid's own AHardwareBuffer functions stay stubs; they would
        # forward once libnativewindow has buffers.
        "stubbed": ["AConfiguration_match", "AConfiguration_isBetterThan"],
        "private": ["minato_AAssetManager_create", "minato_AInputQueue_create", "minato_AInputQueue_delete"],
    },
    "libnativewindow": {
        "version": "LIBNATIVEWINDOW",
        "implemented": [
            "ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_getWidth", "ANativeWindow_getHeight",
            "ANativeWindow_getFormat", "ANativeWindow_setBuffersGeometry", "ANativeWindow_setFrameRate",
            "ANativeWindow_setFrameRateWithChangeStrategy", "ANativeWindow_setBuffersTransform",
            "ANativeWindow_setBuffersDataSpace", "ANativeWindow_getBuffersDataSpace",
            "ANativeWindow_getBuffersDefaultDataSpace", "ANativeWindow_tryAllocateBuffers",
        ],
        "stubbed": [],
        "private": ["minato_ANativeWindow_create", "minato_ANativeWindow_getInfo"],
    },
    # zlib from the NDK's libz.a, whole (decision 12): only the version script.
    "libz": {"version": None, "implemented": ["*"], "stubbed": [], "private": []},
}

# The kinds of the functions the NDK's headers do not declare.
KINDS = {
    "AHardwareBuffer_getNativeHandle": "pointer",
    "AHardwareBuffer_readFromParcel": "int",
    "AHardwareBuffer_writeToParcel": "int",
    "ANativeWindow_readFromParcel": "int",
    "ANativeWindow_writeToParcel": "int",
}

_ERRNO = "(errno = ENOSYS, -1)"  # -1 with errno set, where the API says so
VALUES = {
    # Choreographer's frame data (M3 step 6): getters of data no frame gives
    "AChoreographerFrameCallbackData_getFrameTimeNanos": "0",
    "AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos": "0",
    "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos": "0",
    "AChoreographerFrameCallbackData_getFrameTimelineVsyncId": "0",
    "AChoreographerFrameCallbackData_getFrameTimelinesLength": "0",
    "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex": "0",
    # Resource matching: no match, never better
    "AConfiguration_isBetterThan": "0", "AConfiguration_match": "0",
    # Fonts: no font exists
    "AFont_getAxisCount": "0", "AFont_getAxisTag": "0", "AFont_getCollectionIndex": "0", "AFont_getWeight": "0",
    "AFont_isItalic": "0",
    # Hardware buffers: none can be made (decision 7)
    "AHardwareBuffer_allocate": "-EINVAL", "AHardwareBuffer_getId": "-EINVAL", "AHardwareBuffer_isSupported": "0",
    "AHardwareBuffer_lock": "-EINVAL", "AHardwareBuffer_lockAndGetInfo": "-EINVAL",
    "AHardwareBuffer_lockPlanes": "-EINVAL", "AHardwareBuffer_recvHandleFromUnixSocket": "-EINVAL",
    "AHardwareBuffer_sendHandleToUnixSocket": "-EINVAL", "AHardwareBuffer_unlock": "-EINVAL",
    "AHardwareBuffer_readFromParcel": "-ENOSYS", "AHardwareBuffer_writeToParcel": "-ENOSYS",  # STATUS_INVALID_OPERATION
    # Input (M3 step 6): getters of events no queue gives
    "AInputEvent_getDeviceId": "0", "AInputEvent_getSource": "0", "AInputEvent_getType": "0",
    "AInputQueue_getEvent": "-1", "AInputQueue_hasEvents": "0", "AInputQueue_preDispatchEvent": "0",
    "AKeyEvent_getAction": "0", "AKeyEvent_getDownTime": "0", "AKeyEvent_getEventTime": "0", "AKeyEvent_getFlags": "0",
    "AKeyEvent_getKeyCode": "0", "AKeyEvent_getMetaState": "0", "AKeyEvent_getRepeatCount": "0",
    "AKeyEvent_getScanCode": "0",
    "AMotionEvent_getAction": "0", "AMotionEvent_getActionButton": "0", "AMotionEvent_getButtonState": "0",
    "AMotionEvent_getClassification": "0", "AMotionEvent_getDownTime": "0", "AMotionEvent_getEdgeFlags": "0",
    "AMotionEvent_getEventTime": "0", "AMotionEvent_getFlags": "0", "AMotionEvent_getHistoricalEventTime": "0",
    "AMotionEvent_getHistorySize": "0", "AMotionEvent_getMetaState": "0", "AMotionEvent_getPointerCount": "0",
    "AMotionEvent_getPointerId": "0", "AMotionEvent_getToolType": "0",
    # Drawing by the CPU is not in M3 (decision 7): INVALID_OPERATION
    "ANativeWindow_lock": "-ENOSYS", "ANativeWindow_unlockAndPost": "-ENOSYS",
    "ANativeWindow_readFromParcel": "-ENOSYS", "ANativeWindow_writeToParcel": "-ENOSYS",
    # OBBs
    "AObbInfo_getFlags": "0", "AObbInfo_getVersion": "0", "AStorageManager_isObbMounted": "0",
    # Performance hints: no session exists; EPIPE, "communication with the system service has failed"
    "APerformanceHint_getPreferredUpdateRateNanos": "0", "APerformanceHint_reportActualWorkDuration": "EPIPE",
    "APerformanceHint_reportActualWorkDuration2": "EPIPE", "APerformanceHint_setPreferPowerEfficiency": "EPIPE",
    "APerformanceHint_setThreads": "EPIPE", "APerformanceHint_updateTargetWorkDuration": "EPIPE",
    "APermissionManager_checkPermission": "-2",  # PERMISSION_MANAGER_STATUS_SERVICE_UNAVAILABLE
    # Sensors: none exists
    "ASensorEventQueue_disableSensor": "-EINVAL", "ASensorEventQueue_enableSensor": "-EINVAL",
    "ASensorEventQueue_getEvents": "-EINVAL", "ASensorEventQueue_hasEvents": "0",
    "ASensorEventQueue_registerSensor": "-EINVAL", "ASensorEventQueue_requestAdditionalInfoEvents": "-EINVAL",
    "ASensorEventQueue_setEventRate": "-EINVAL", "ASensorManager_configureDirectReport": "-EINVAL",
    "ASensorManager_createHardwareBufferDirectChannel": "-EINVAL",
    "ASensorManager_createSharedMemoryDirectChannel": "-EINVAL", "ASensorManager_destroyEventQueue": "-EINVAL",
    "ASensorManager_getDynamicSensorList": "0", "ASensorManager_getSensorList": "0",
    "ASensor_getFifoMaxEventCount": "0", "ASensor_getFifoReservedEventCount": "0", "ASensor_getHandle": "0",
    "ASensor_getHighestDirectReportRateLevel": "0", "ASensor_getMinDelay": "0",
    "ASensor_getReportingMode": "-1",  # AREPORTING_MODE_INVALID
    "ASensor_getType": "-1",  # ASENSOR_TYPE_INVALID
    "ASensor_isDirectChannelTypeSupported": "0", "ASensor_isWakeUpSensor": "0",
    # Shared memory
    "ASharedMemory_create": _ERRNO, "ASharedMemory_dupFromJava": "-1", "ASharedMemory_getSize": "0",
    "ASharedMemory_setProt": "(errno = EINVAL, -1)",
    # Surface textures and transactions: none exists
    "ASurfaceTexture_attachToGLContext": "-EINVAL", "ASurfaceTexture_detachFromGLContext": "-EINVAL",
    "ASurfaceTexture_getTimestamp": "0", "ASurfaceTexture_updateTexImage": "-EINVAL",
    "ASurfaceTransactionStats_getAcquireTime": "-1", "ASurfaceTransactionStats_getLatchTime": "0",
    "ASurfaceTransactionStats_getPresentFenceFd": "-1", "ASurfaceTransactionStats_getPreviousReleaseFenceFd": "-1",
    # Thermal: no thermal service
    "AThermal_getCurrentThermalStatus": "-1",  # ATHERMAL_STATUS_ERROR
    "AThermal_getThermalHeadroom": "__builtin_nanf(\"\")",
    "AThermal_getThermalHeadroomThresholds": "ENOSYS",  # "the feature is disabled by the current system"
    "AThermal_registerThermalStatusListener": "EPIPE", "AThermal_unregisterThermalStatusListener": "EPIPE",
    "ATrace_isEnabled": "0",
    # Networks: none can be chosen, not even the default one (NETWORK_UNSPECIFIED)
    "android_getaddrinfofornetwork": "EAI_FAIL", "android_getprocdns": _ERRNO, "android_getprocnetwork": _ERRNO,
    "android_res_nquery": "-ENOSYS", "android_res_nresult": "-ENOSYS", "android_res_nsend": "-ENOSYS",
    "android_setprocdns": _ERRNO, "android_setprocnetwork": _ERRNO, "android_setsocknetwork": _ERRNO,
    "android_tag_socket": "-ENOSYS", "android_tag_socket_with_uid": "-ENOSYS", "android_untag_socket": "-ENOSYS",
}

HEADERS = [
    "asset_manager", "asset_manager_jni", "choreographer", "configuration", "data_space", "font", "font_matcher",
    "hardware_buffer", "hardware_buffer_jni", "input", "input_transfer_token_jni", "keycodes", "looper",
    "multinetwork", "native_activity", "native_window", "native_window_jni", "obb", "performance_hint",
    "permission_manager", "sensor", "sharedmem", "sharedmem_jni", "storage_manager", "surface_control",
    "surface_control_input_receiver", "surface_control_jni", "surface_texture", "surface_texture_jni",
    "system_fonts", "thermal", "trace", "window",
]

INTEGERS = {"char", "signed char", "unsigned char", "short", "unsigned short", "int", "unsigned int", "long",
            "unsigned long", "long long", "unsigned long long", "_Bool", "bool"}


def fail(message):
    raise SystemExit(f"stubgen.py: {message}")


def stub_path(sysroot, library):
    return os.path.join(sysroot, "usr", "lib", TRIPLE, str(API), f"{library}.so")


def stub_names(nm, sysroot, library):
    """The names the NDK's stub exports, each with its version or None."""
    output = subprocess.run([nm, "-D", "--defined-only", stub_path(sysroot, library)], check=True,
                            capture_output=True, text=True).stdout
    names = {}
    for line in output.splitlines():
        symbol = line.split()[-1]
        name, _, version = symbol.partition("@@")
        names[name] = version or None
    return names


def matches(name, patterns):
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def stubbed_names(library, names):
    table = LIBRARIES[library]
    return sorted(name for name in names
                  if not matches(name, table["implemented"]) or name in table["stubbed"])


def strip(type_name):
    type_name = re.sub(r"\b(_Nonnull|_Nullable|_Null_unspecified|const|volatile|restrict)\b", " ", type_name)
    return " ".join(type_name.split()).replace(" *", "*")


def return_kinds(clang, sysroot, names):
    """The kind of each name's result: void, int, float, double or pointer."""
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "all.h")
        with open(source, "w") as handle:
            handle.writelines(f"#include <android/{header}.h>\n" for header in HEADERS)
        dump = subprocess.run([clang, f"--target={TRIPLE}{API}", f"--sysroot={sysroot}", "-x", "c",
                               "-fsyntax-only", "-Xclang", "-ast-dump=json", source],
                              check=True, capture_output=True, text=True).stdout
    functions, typedefs = {}, {}
    stack = [json.loads(dump)]
    while stack:
        node = stack.pop()
        kind = node.get("kind")
        if kind == "FunctionDecl" and "name" in node:
            functions.setdefault(node["name"], node["type"]["qualType"])
        elif kind == "TypedefDecl" and "name" in node:
            typedefs.setdefault(node["name"], node["type"].get("desugaredQualType", node["type"]["qualType"]))
        stack.extend(node.get("inner", []) or [])

    def classify(type_name, depth=0):
        type_name = strip(type_name)
        if depth > 16:
            fail(f"typedefs of {type_name} go too deep")
        if type_name.endswith("*"):
            return "pointer"
        if type_name == "void":
            return "void"
        if type_name in ("float", "double"):
            return type_name
        if type_name in INTEGERS or type_name.startswith("enum "):
            return "int"
        if type_name in typedefs:
            return classify(typedefs[type_name], depth + 1)
        fail(f"cannot tell the kind of {type_name}")

    kinds = {}
    for name in names:
        if name in KINDS:
            kinds[name] = KINDS[name]
            continue
        if name not in functions:
            fail(f"{name} is in no header of HEADERS, and not in KINDS")
        function_type = functions[name]
        parameters = function_type.find("(")
        if parameters <= 0 or function_type[parameters + 1:parameters + 2] == "*":
            fail(f"{name} has a type stubgen.py does not know: {function_type}")
        kinds[name] = classify(function_type[:parameters])
    return kinds


def check_tables(library, names, kinds):
    """The problems of the tables for one library."""
    problems = []
    table = LIBRARIES[library]
    for pattern in table["implemented"] + table["stubbed"]:
        if not any(fnmatch.fnmatchcase(name, pattern) for name in names):
            problems.append(f"{library}: {pattern} matches no name of the NDK's stub")
    for name, kind in kinds.items():
        if kind == "int" and name not in VALUES:
            problems.append(f"{library}: the stub {name} returns an integer and has no value in VALUES")
        if kind in ("void", "pointer") and name in VALUES and VALUES[name] != "NULL":
            problems.append(f"{library}: the stub {name} returns {kind}, VALUES cannot give it {VALUES[name]}")
    return problems


C_TYPES = {"void": "void", "int": "long", "float": "float", "double": "double", "pointer": "void *"}
DEFAULTS = {"int": None, "float": "0.0f", "double": "0.0", "pointer": "NULL"}


def write_stubs(path, library, kinds):
    lines = [
        f"// Generated by runtime/android/stubgen.py from the NDK's {library}.so and headers: do not edit.",
        "// Each stub logs its name once and answers what means \"nothing\" or \"failed\" (ADR-0028, decision 9).",
        "#include <android/log.h>",
        "#include <errno.h>",
        "#include <netdb.h>",
        "#include <stddef.h>",
        "",
        "static void log_once(char *logged, const char *name) {",
        "  if (!__atomic_exchange_n(logged, 1, __ATOMIC_RELAXED)) {",
        f"    __android_log_print(ANDROID_LOG_WARN, \"{library}\", \"%s: not implemented\", name);",
        "  }",
        "}",
        "",
    ]
    for name in sorted(kinds):
        kind = kinds[name]
        value = VALUES.get(name, DEFAULTS.get(kind))
        body = f"static char logged; log_once(&logged, \"{name}\");"
        if kind != "void":
            body += f" return {value};"
        lines.append(f"__attribute__((visibility(\"default\"))) {C_TYPES[kind]} {name}(void) {{ {body} }}")
    with open(path, "w") as handle:
        handle.write("\n".join(lines) + "\n")


def write_map(path, library, names):
    table = LIBRARIES[library]
    version = table["version"]
    lines = [f"# Generated by runtime/android/stubgen.py from the NDK's {library}.so: do not edit.",
             f"{version} {{" if version else "{", "  global:"]
    lines += [f"    {name};" for name in sorted(names)]
    lines += ["  local:", "    *;", "};"]
    if table["private"]:
        lines += ["APKRT_PRIVATE {", "  global:"] + [f"    {name};" for name in table["private"]]
        lines += [f"}} {version};"]
    with open(path, "w") as handle:
        handle.write("\n".join(lines) + "\n")


def generate(arguments):
    names = stub_names(arguments.nm, arguments.sysroot, arguments.library)
    wrong = [f"{name}@{version}" for name, version in names.items() if version != LIBRARIES[arguments.library]["version"]
             and version is not None]
    if wrong:
        fail(f"{arguments.library}: versions stubgen.py does not expect: {', '.join(wrong)}")
    stubbed = stubbed_names(arguments.library, names)
    if arguments.stubs:
        kinds = return_kinds(arguments.clang, arguments.sysroot, stubbed)
        problems = check_tables(arguments.library, names, kinds)
        if problems:
            fail("\n".join(problems))
        write_stubs(arguments.stubs, arguments.library, kinds)
    elif stubbed:
        fail(f"{arguments.library} has stubs, give --stubs")
    write_map(arguments.map, arguments.library, names)
    if arguments.names:
        with open(arguments.names, "w") as handle:
            handle.writelines(f"{name}\n" for name in sorted(names))


def defined_symbols(nm, path):
    output = subprocess.run([nm, "-D", "--defined-only", path], check=True, capture_output=True, text=True).stdout
    return {line.split()[-1] for line in output.splitlines()}


def check(arguments):
    ndk = arguments.ndk or os.environ.get("ANDROID_NDK") or os.path.expanduser(
        "~/Library/Android/sdk/ndk/30.0.16248370")
    toolchain = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "darwin-x86_64")
    sysroot = os.path.join(toolchain, "sysroot")
    nm, clang = os.path.join(toolchain, "bin", "llvm-nm"), os.path.join(toolchain, "bin", "clang")
    if not os.path.exists(nm) or not os.path.exists(stub_path(sysroot, "libandroid")):
        print(f"skipped: no NDK at {ndk}")
        return
    problems, checked, missing, all_stubbed = [], [], [], set()
    for library, table in LIBRARIES.items():
        names = stub_names(nm, sysroot, library)
        stubbed = stubbed_names(library, names)
        all_stubbed.update(stubbed)
        if stubbed:
            problems += check_tables(library, names, return_kinds(clang, sysroot, stubbed))
        built = os.path.join(arguments.built, f"{library}.so") if arguments.built else None
        if built is None or not os.path.exists(os.path.join(arguments.built, "libc.so")):
            continue
        if not os.path.exists(built):
            missing.append(f"{library}.so")
            continue
        expected = {f"{name}@@{table['version']}" if table["version"] else name for name in names}
        expected |= {f"{name}@@APKRT_PRIVATE" for name in table["private"]}
        exported = defined_symbols(nm, built)
        for symbol in sorted(expected - exported):
            problems.append(f"{library}.so does not export {symbol}")
        for symbol in sorted(exported - expected):
            problems.append(f"{library}.so exports {symbol}, which it should not")
        checked.append(f"{library}.so ({len(exported & expected)} names)")
    problems += [f"VALUES has {name}, which no library stubs" for name in sorted(set(VALUES) - all_stubbed)]
    if problems:
        print("\n".join(problems))
        raise SystemExit(1)
    if missing:
        print(f"skipped: tables complete, but {arguments.built} lacks {', '.join(missing)} (tools/build-bionic.sh)")
        return
    print("tables complete" + (", exports checked: " + ", ".join(checked) if checked else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    generator = commands.add_parser("generate")
    generator.add_argument("library", choices=sorted(LIBRARIES))
    for option in ("--sysroot", "--clang", "--nm", "--map"):
        generator.add_argument(option, required=True)
    generator.add_argument("--stubs")
    generator.add_argument("--names")
    checker = commands.add_parser("check")
    checker.add_argument("--ndk")
    checker.add_argument("--built")
    arguments = parser.parse_args()
    generate(arguments) if arguments.command == "generate" else check(arguments)


if __name__ == "__main__":
    main()
