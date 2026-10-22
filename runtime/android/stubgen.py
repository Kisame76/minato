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
            # the asset manager (asset_manager.cpp), also for a Java AssetManager (ADR-0041, decision 7)
            "AAsset_*", "AAssetDir_*", "AAssetManager_open", "AAssetManager_openDir", "AAssetManager_fromJava",
            # the requests (native_activity.cpp)
            "ANativeActivity_*",
            # the input queue on the input channel (input_queue.cpp) and its motion events (input.cpp, ADR-0036)
            "AInputQueue_*", "AInputEvent_*", "AMotionEvent_*",
            # the choreographer on the vsync channel (choreographer.cpp, ADR-0036)
            "AChoreographer_*", "AChoreographerFrameCallbackData_*",
            # forwarded to libnativewindow (window_forwards.c)
            "ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_getWidth", "ANativeWindow_getHeight",
            "ANativeWindow_getFormat", "ANativeWindow_setBuffersGeometry", "ANativeWindow_lock",
            "ANativeWindow_unlockAndPost",
            # a Java Surface's window, through libandroid_runtime (native_window_jni.cpp, ADR-0043 c2)
            "ANativeWindow_fromSurface", "ANativeWindow_toSurface",
            # shared memory on memfd_create, as libcutils' memfd path makes it (sharedmem.cpp)
            "ASharedMemory_*",
            # forwarded to libnativewindow (hardware_buffer_forwards.cpp, ADR-0058 a)), and a Java HardwareBuffer's
            # buffer through libandroid_runtime (hardware_buffer_jni.cpp)
            "AHardwareBuffer_fromHardwareBuffer", "AHardwareBuffer_toHardwareBuffer",
            "AHardwareBuffer_acquire", "AHardwareBuffer_allocate", "AHardwareBuffer_describe", "AHardwareBuffer_getId",
            "AHardwareBuffer_isSupported", "AHardwareBuffer_lock", "AHardwareBuffer_lockAndGetInfo",
            "AHardwareBuffer_lockPlanes", "AHardwareBuffer_recvHandleFromUnixSocket", "AHardwareBuffer_release",
            "AHardwareBuffer_sendHandleToUnixSocket", "AHardwareBuffer_unlock",
            # a manager without sensors and its event queues (sensor_manager.c, the Unity test APK probe)
            "ASensorManager_getInstance", "ASensorManager_getInstanceForPackage", "ASensorManager_getSensorList",
            "ASensorManager_getDynamicSensorList", "ASensorManager_getDefaultSensor",
            "ASensorManager_getDefaultSensorEx", "ASensorManager_createEventQueue",
            "ASensorManager_destroyEventQueue", "ASensorEventQueue_registerSensor", "ASensorEventQueue_enableSensor",
            "ASensorEventQueue_disableSensor", "ASensorEventQueue_setEventRate", "ASensorEventQueue_hasEvents",
            "ASensorEventQueue_getEvents",
            # a Java SurfaceTexture's consumer through libandroid_runtime (surface_texture.cpp, ADR-0058 c))
            "ASurfaceTexture_*",
        ],
        # M3 has no resource matching. Input's Java side waits for Java (M4).
        "stubbed": ["AConfiguration_match", "AConfiguration_isBetterThan", "AInputQueue_fromJava",
                    "AInputEvent_release", "AInputEvent_toJava", "AMotionEvent_fromJava"],
        "private": ["minato_AAssetManager_create", "minato_AInputQueue_create", "minato_AInputQueue_delete",
                    "minato_AConfiguration_fromSession"],
    },
    "libnativewindow": {
        "version": "LIBNATIVEWINDOW",
        "implemented": [
            "ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_getWidth", "ANativeWindow_getHeight",
            "ANativeWindow_getFormat", "ANativeWindow_setBuffersGeometry", "ANativeWindow_setFrameRate",
            "ANativeWindow_setFrameRateWithChangeStrategy", "ANativeWindow_setBuffersTransform",
            "ANativeWindow_setBuffersDataSpace", "ANativeWindow_getBuffersDataSpace",
            "ANativeWindow_getBuffersDefaultDataSpace", "ANativeWindow_tryAllocateBuffers",
            # drawing by the CPU, on the host's buffers (ADR-0043, decision 3)
            "ANativeWindow_lock", "ANativeWindow_unlockAndPost",
            # AHardwareBuffer on the runtime's buffers (hardware_buffer.cpp, ADR-0058 a)); its parcels stay stubs
            "AHardwareBuffer_acquire", "AHardwareBuffer_allocate", "AHardwareBuffer_describe", "AHardwareBuffer_getId",
            "AHardwareBuffer_isSupported", "AHardwareBuffer_lock", "AHardwareBuffer_lockAndGetInfo",
            "AHardwareBuffer_lockPlanes", "AHardwareBuffer_recvHandleFromUnixSocket", "AHardwareBuffer_release",
            "AHardwareBuffer_sendHandleToUnixSocket", "AHardwareBuffer_unlock",
        ],
        "stubbed": [],
        "private": ["minato_ANativeWindow_create", "minato_ANativeWindow_getInfo", "minato_BufferQueue_create",
                    "minato_BufferQueue_setDefaultSize", "minato_BufferQueue_acquire", "minato_BufferQueue_releaseSlot",
                    "minato_BufferQueue_abandon", "minato_BufferQueue_isAbandoned", "minato_BufferQueue_release",
                    "minato_ANativeWindow_isQueue", "minato_ANativeWindow_dequeue", "minato_ANativeWindow_queue",
                    "minato_ANativeWindow_cancel", "minato_BufferQueue_setConsumer",
                    "minato_BufferQueue_setDefaultFormat", "minato_BufferQueue_setDefaultDataSpace",
                    "minato_BufferQueue_discardFreeBuffers",
                    "minato_BufferQueue_detach", "minato_ANativeWindow_connect", "minato_ANativeWindow_disconnect",
                    "minato_ANativeWindow_queueWith", "minato_ANativeWindow_attachAndQueue"],
    },
    # zlib from the NDK's libz.a, whole (decision 12): only the version script.
    "libz": {"version": None, "implemented": ["*"], "stubbed": [], "private": []},
    # AAudio's output streams on the runtime's (aaudio.cpp, ADR-0036, decision 6), every name written by hand, and
    # libOpenSLES's claim of a data callback's frames (ADR-0036 c2).
    "libaaudio": {"version": None, "implemented": ["*"], "stubbed": [],
                  "private": ["minato_AAudioStream_claimCallbackFrames"]},
    # OpenSL ES on libaaudio (opensles.cpp, ADR-0036, decision 7): its 3 functions and 53 interface IDs, every name
    # written by hand.
    "libOpenSLES": {"version": None, "implemented": ["*"], "stubbed": [], "private": []},
    # No camera (ADR-0042 b, gap 15), so that an app that links the library loads, as Godot's libgodot_android.so
    # does: the camera manager answers an empty ID list (camera_manager.c), every other function an error.
    "libcamera2ndk": {
        "version": None,
        "implemented": ["ACameraManager_create", "ACameraManager_delete", "ACameraManager_getCameraIdList",
                        "ACameraManager_deleteCameraIdList"],
        "stubbed": [],
        "private": [],
    },
    # AImageReader and AImage (ADR-0058 e1), G3c, runtime/android/image_reader.cpp), the key strings
    # (AMEDIAFORMAT_KEY_*, AMEDIACODEC_KEY_*, data with Android 15's values, runtime/android/media_keys.cpp,
    # ADR-0060 e2b), AMediaFormat (media_format.cpp) and AMediaCodec's decoders (media_codec.cpp, e2b3); no
    # surfaces, crypto, extractor, muxer or DRM yet (gap 15): every other function answers an error.
    "libmediandk": {"version": None,
                    "implemented": ["AImageReader_new", "AImageReader_newWithUsage", "AImageReader_newWithDataSpace",
                                    "AImageReader_delete", "AImageReader_getWindow", "AImageReader_getWidth",
                                    "AImageReader_getHeight", "AImageReader_getFormat", "AImageReader_getMaxImages",
                                    "AImageReader_acquireNextImage", "AImageReader_acquireLatestImage",
                                    "AImageReader_acquireNextImageAsync", "AImageReader_acquireLatestImageAsync",
                                    "AImageReader_setImageListener", "AImageReader_setBufferRemovedListener",
                                    "AImage_delete", "AImage_deleteAsync", "AImage_getWidth", "AImage_getHeight",
                                    "AImage_getFormat", "AImage_getCropRect", "AImage_getTimestamp",
                                    "AImage_getDataSpace", "AImage_getNumberOfPlanes", "AImage_getPlanePixelStride",
                                    "AImage_getPlaneRowStride", "AImage_getPlaneData", "AImage_getHardwareBuffer",
                                    "AMEDIAFORMAT_KEY_*", "AMEDIACODEC_KEY_*", "AMediaFormat_*",
                                    "AMediaCodec_configure", "AMediaCodec_createCodecByName",
                                    "AMediaCodec_createDecoderByType", "AMediaCodec_createEncoderByType",
                                    "AMediaCodec_delete", "AMediaCodec_dequeueInputBuffer",
                                    "AMediaCodec_dequeueOutputBuffer", "AMediaCodec_flush",
                                    "AMediaCodec_getBufferFormat",
                                    "AMediaCodec_getInputBuffer", "AMediaCodec_getInputFormat", "AMediaCodec_getName",
                                    "AMediaCodec_getOutputBuffer", "AMediaCodec_getOutputFormat",
                                    "AMediaCodec_queueInputBuffer", "AMediaCodec_releaseName",
                                    "AMediaCodec_releaseOutputBuffer", "AMediaCodec_releaseOutputBufferAtTime",
                                    "AMediaCodec_setAsyncNotifyCallback", "AMediaCodec_setOutputSurface",
                                    "AMediaCodec_setParameters", "AMediaCodec_signalEndOfInputStream",
                                    "AMediaCodec_start", "AMediaCodec_stop"],
                    "stubbed": [],
                    "private": ["minato_AMediaFormat_count", "minato_AMediaFormat_entry", "minato_media_codec_count",
                                "minato_media_codec_at"]},
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
    # Resource matching: no match, never better
    "AConfiguration_isBetterThan": "0", "AConfiguration_match": "0",
    # Fonts: no font exists
    "AFont_getAxisCount": "0", "AFont_getAxisTag": "0", "AFont_getCollectionIndex": "0", "AFont_getWeight": "0",
    "AFont_isItalic": "0",
    # Hardware buffers in parcels, which need Binder: INVALID_OPERATION (ADR-0058 a) writes the rest)
    "AHardwareBuffer_readFromParcel": "-ENOSYS", "AHardwareBuffer_writeToParcel": "-ENOSYS",  # STATUS_INVALID_OPERATION
    # Key events: M3 has none (ADR-0036, decision 3)
    "AKeyEvent_getAction": "0", "AKeyEvent_getDownTime": "0", "AKeyEvent_getEventTime": "0", "AKeyEvent_getFlags": "0",
    "AKeyEvent_getKeyCode": "0", "AKeyEvent_getMetaState": "0", "AKeyEvent_getRepeatCount": "0",
    "AKeyEvent_getScanCode": "0",
    # Windows in parcels: INVALID_OPERATION
    "ANativeWindow_readFromParcel": "-ENOSYS", "ANativeWindow_writeToParcel": "-ENOSYS",
    # OBBs
    "AObbInfo_getFlags": "0", "AObbInfo_getVersion": "0", "AStorageManager_isObbMounted": "0",
    # Performance hints: no session exists; EPIPE, "communication with the system service has failed"
    "APerformanceHint_getPreferredUpdateRateNanos": "0", "APerformanceHint_reportActualWorkDuration": "EPIPE",
    "APerformanceHint_reportActualWorkDuration2": "EPIPE", "APerformanceHint_setPreferPowerEfficiency": "EPIPE",
    "APerformanceHint_setThreads": "EPIPE", "APerformanceHint_updateTargetWorkDuration": "EPIPE",
    "APermissionManager_checkPermission": "-2",  # PERMISSION_MANAGER_STATUS_SERVICE_UNAVAILABLE
    # Sensors: none exists
    "ASensorEventQueue_requestAdditionalInfoEvents": "-EINVAL", "ASensorManager_configureDirectReport": "-EINVAL",
    "ASensorManager_createHardwareBufferDirectChannel": "-EINVAL",
    "ASensorManager_createSharedMemoryDirectChannel": "-EINVAL",
    "ASensor_getFifoMaxEventCount": "0", "ASensor_getFifoReservedEventCount": "0", "ASensor_getHandle": "0",
    "ASensor_getHighestDirectReportRateLevel": "0", "ASensor_getMinDelay": "0",
    "ASensor_getReportingMode": "-1",  # AREPORTING_MODE_INVALID
    "ASensor_getType": "-1",  # ASENSOR_TYPE_INVALID
    "ASensor_isDirectChannelTypeSupported": "0", "ASensor_isWakeUpSensor": "0",
    # Surface transactions: none exists
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
    # Cameras: none exists (ADR-0042 b, gap 15); their status results come from STATUS_VALUES
    "ACameraMetadata_isLogicalMultiCamera": "0",
    # Media beyond the decoders (gap 15): no crypto, extractor or muxer, so nothing is supported or counted
    "AMediaCodecActionCode_isRecoverable": "0", "AMediaCodecActionCode_isTransient": "0",
    "AMediaCrypto_isCryptoSchemeSupported": "0", "AMediaCrypto_requiresSecureDecoderComponent": "0",
    "AMediaDrm_isCryptoSchemeSupported": "0", "AMediaExtractor_advance": "0",
    "AMediaCodecCryptoInfo_getMode": "0",  # AMEDIACODECRYPTOINFO_MODE_CLEAR
    "AMediaCodecCryptoInfo_getNumSubSamples": "0", "AMediaExtractor_getTrackCount": "0", "AMediaMuxer_getTrackCount": "0",
    "AMediaExtractor_getSampleFlags": "0",
    "AMediaExtractor_getSampleTrackIndex": "-1", "AMediaExtractor_getSampleSize": "-1",  # no sample
    "AMediaExtractor_readSampleData": "-1", "AMediaExtractor_getSampleTime": "-1", "AMediaExtractor_getCachedDuration": "-1",
    "AMediaMuxer_addTrack": "-10002",  # AMEDIA_ERROR_UNSUPPORTED
}

HEADERS = [
    "asset_manager", "asset_manager_jni", "choreographer", "configuration", "data_space", "font", "font_matcher",
    "hardware_buffer", "hardware_buffer_jni", "input", "input_transfer_token_jni", "keycodes", "looper",
    "multinetwork", "native_activity", "native_window", "native_window_jni", "obb", "performance_hint",
    "permission_manager", "sensor", "sharedmem", "sharedmem_jni", "storage_manager", "surface_control",
    "surface_control_input_receiver", "surface_control_jni", "surface_texture", "surface_texture_jni",
    "system_fonts", "thermal", "trace", "window",
    # Not in android/: the camera's and the media's (ADR-0042 b, gap 15).
    "camera/NdkCameraCaptureSession", "camera/NdkCameraDevice", "camera/NdkCameraManager", "camera/NdkCameraMetadata",
    "camera/NdkCaptureRequest", "media/NdkImage", "media/NdkImageReader", "media/NdkMediaCodec",
    "media/NdkMediaCrypto", "media/NdkMediaDataSource", "media/NdkMediaDrm", "media/NdkMediaExtractor",
    "media/NdkMediaFormat", "media/NdkMediaMuxer",
]

# The value of every stub whose result is its API's status type and that VALUES does not name: the error that fits
# a call when nothing of the API exists (the NDK's NdkCameraError.h and NdkMediaError.h).
STATUS_VALUES = {
    "camera_status_t": "-10001",  # ACAMERA_ERROR_INVALID_PARAMETER: no camera ID is valid, no camera object exists
    "media_status_t": "-10002",  # AMEDIA_ERROR_UNSUPPORTED
}

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


def contains_kind(node, kind):
    """Whether an AST node of clang's JSON dump has a node of `kind` below it."""
    stack = list(node.get("inner", []) or [])
    while stack:
        inner = stack.pop()
        if inner.get("kind") == kind:
            return True
        stack.extend(inner.get("inner", []) or [])
    return False


def return_kinds(clang, sysroot, names):
    """The kind of each name's result: void, int, float, double or pointer; and for each name whose result is a
    status type of STATUS_VALUES, that type."""
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "all.h")
        with open(source, "w") as handle:
            handle.writelines(f"#include <{header if '/' in header else 'android/' + header}.h>\n" for header in HEADERS)
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
            target = node["type"].get("desugaredQualType", node["type"]["qualType"])
            if strip(target) == node["name"] and contains_kind(node, "EnumType"):
                target = f"enum {node['name']}"  # typedef enum { ... } name, the camera's and the media's status
            typedefs.setdefault(node["name"], target)
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

    kinds, statuses = {}, {}
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
        if strip(function_type[:parameters]) in STATUS_VALUES:
            statuses[name] = strip(function_type[:parameters])
    return kinds, statuses


def present_names(nm, sysroot, library):
    """The names of the NDK's stub that the library exports, each with its version or None: all but those that
    LIBRARIES gives as absent; and the problems of those patterns."""
    absent = LIBRARIES[library].get("absent", [])
    names = stub_names(nm, sysroot, library)
    problems = [f"{library}: the absent pattern {pattern} matches no name of the NDK's stub" for pattern in absent
                if not any(fnmatch.fnmatchcase(name, pattern) for name in names)]
    return {name: version for name, version in names.items() if not matches(name, absent)}, problems


def check_tables(library, names, kinds, statuses):
    """The problems of the tables for one library."""
    problems = []
    table = LIBRARIES[library]
    for pattern in table["implemented"] + table["stubbed"]:
        if not any(fnmatch.fnmatchcase(name, pattern) for name in names):
            problems.append(f"{library}: {pattern} matches no name of the NDK's stub")
    for name, kind in kinds.items():
        if kind == "int" and name not in VALUES and name not in statuses:
            problems.append(f"{library}: the stub {name} returns an integer and has no value in VALUES")
        if kind in ("void", "pointer") and name in VALUES and VALUES[name] != "NULL":
            problems.append(f"{library}: the stub {name} returns {kind}, VALUES cannot give it {VALUES[name]}")
    return problems


C_TYPES = {"void": "void", "int": "long", "float": "float", "double": "double", "pointer": "void *"}
DEFAULTS = {"int": None, "float": "0.0f", "double": "0.0", "pointer": "NULL"}


def write_stubs(path, library, kinds, statuses):
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
        value = VALUES.get(name, STATUS_VALUES[statuses[name]] if name in statuses else DEFAULTS.get(kind))
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
    if not version:  # an unnamed version node allows no other: the private names are unversioned too
        lines += [f"    {name};" for name in table["private"]]
    lines += ["  local:", "    *;", "};"]
    if table["private"] and version:
        lines += ["APKRT_PRIVATE {", "  global:"] + [f"    {name};" for name in table["private"]]
        lines += [f"}} {version};"]
    with open(path, "w") as handle:
        handle.write("\n".join(lines) + "\n")


def generate(arguments):
    names, problems = present_names(arguments.nm, arguments.sysroot, arguments.library)
    if problems:
        fail("\n".join(problems))
    wrong = [f"{name}@{version}" for name, version in names.items() if version != LIBRARIES[arguments.library]["version"]
             and version is not None]
    if wrong:
        fail(f"{arguments.library}: versions stubgen.py does not expect: {', '.join(wrong)}")
    stubbed = stubbed_names(arguments.library, names)
    if arguments.stubs:
        kinds, statuses = return_kinds(arguments.clang, arguments.sysroot, stubbed)
        problems = check_tables(arguments.library, names, kinds, statuses)
        if problems:
            fail("\n".join(problems))
        write_stubs(arguments.stubs, arguments.library, kinds, statuses)
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
        names, absent_problems = present_names(nm, sysroot, library)
        problems += absent_problems
        stubbed = stubbed_names(library, names)
        all_stubbed.update(stubbed)
        if stubbed:
            problems += check_tables(library, names, *return_kinds(clang, sysroot, stubbed))
        built = os.path.join(arguments.built, f"{library}.so") if arguments.built else None
        if built is None or not os.path.exists(os.path.join(arguments.built, "libc.so")):
            continue
        if not os.path.exists(built):
            missing.append(f"{library}.so")
            continue
        expected = {f"{name}@@{table['version']}" if table["version"] else name for name in names}
        expected |= {f"{name}@@APKRT_PRIVATE" if table["version"] else name for name in table["private"]}
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
