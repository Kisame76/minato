# The framework's guest code (ADR-0039), built with the NDK's toolchain as the fixtures are
# (tools/build-framework-native.sh): app_process, libandroidfw with the libraries it links (b3.2), and
# libandroid_runtime.so with framework.jar's natives (b3.3), which app_process registers, libhwui's among them
# (ADR-0043 test 2).
# app_process links the guest's ART libraries of tools/build-art.sh as dalvikvm does
# (runtime/art, build/art/plan.cmake): libnativehelper and libsigchain ahead of libc, liblog, and --export-dynamic.
# Its C++ library is static: a /system/bin program finds no libc++_shared.so in the app's guest system, since ART's copy
# stays in its APEX; only the Mac harnesses put one into /system/lib64, for hwui's set (ADR-0043 test 2).
# JNI's headers come from the fetched libnativehelper.
cmake_minimum_required(VERSION 3.24)
project(MinatoFramework LANGUAGES C CXX)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build the framework's guest code with tools/build-framework-native.sh (Android NDK, arm64-v8a).")
endif()

set(MINATO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ART_LIB "${MINATO_ROOT}/build/art/lib" CACHE PATH "ART's guest libraries (tools/build-art.sh)")
set(NATIVEHELPER "${MINATO_ROOT}/build/sources/libnativehelper")
foreach(library IN ITEMS libnativehelper libsigchain libandroidicu)
  if(NOT EXISTS "${ART_LIB}/${library}.so")
    message(FATAL_ERROR "${ART_LIB}/${library}.so is missing: run tools/build-art.sh first")
  endif()
  add_library(art_${library} SHARED IMPORTED)
  set_target_properties(art_${library} PROPERTIES IMPORTED_LOCATION "${ART_LIB}/${library}.so")
endforeach()

add_executable(app_process app_process/app_process.cpp)
set_target_properties(app_process PROPERTIES OUTPUT_NAME app_process64)
target_include_directories(app_process PRIVATE "${NATIVEHELPER}/include_jni" "${NATIVEHELPER}/include_platform")
target_compile_options(app_process PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
# --exclude-libs,ALL keeps the static C++ library's symbols out of what --export-dynamic exports: Android's
# app_process has libc++.so and exports none of them. Exported, they bind an app library's own C++ runtime to ours
# (its personality routine then reads its unwind context with our unwinder: Unity's self-quit crash, retest 12),
# cxx-binding-test.
target_link_options(app_process PRIVATE -Wl,--export-dynamic -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
# In this order, as dalvikvm's DT_NEEDED: libsigchain before libc, which the toolchain adds last.
target_link_libraries(app_process PRIVATE art_libnativehelper art_libsigchain log)

# --- libandroidfw and what it links (ADR-0039 b3.2), as static archives for libandroid_runtime.so (b3.3). Each
# library takes its Android.bp's sources and flags for the android target, with Soong's global flags as for the host
# aapt2 (tools/aapt2/soong.cmake) on top of the NDK's own; liblog and libz are the guest system's. Left out, as no
# caller needs them before their Java comes: libandroidfw's BackupData and BackupHelpers, which need libbinder.
# CursorWindow, which SQLite's queries fill (the compatibility plan's gap 7), takes its parcel from native/include's
# binder/Parcel.h, Minato's parcel. libincfs is our two-function stand-in (native/incfs), and libziparchive is the
# default one, with incfs support off, rather than libziparchive_for_incfs: Minato installs no app on incfs.
set(SOURCES "${MINATO_ROOT}/build/sources")
foreach(source frameworks_base system_core_framework incremental_delivery libbase fmtlib libziparchive logging libpng
        sqlite)
  if(NOT EXISTS "${SOURCES}/${source}")
    message(FATAL_ERROR "No ${SOURCES}/${source}: run tools/fetch-sources.py --set framework and --set art")
  endif()
endforeach()
include("${MINATO_ROOT}/tools/aapt2/soong.cmake")
find_library(guest_z z REQUIRED)

function(sources_in variable directory)
  set(result "")
  foreach(name ${ARGN})
    list(APPEND result "${directory}/${name}")
  endforeach()
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()

# liblog is the guest system's (tools/build-bionic.sh), with liblog's own headers, which the NDK's lack (log/log.h):
# libandroidfw calls __android_log_error_write, which the NDK's liblog stub does not export.
set(GUEST_LOG "${MINATO_ROOT}/build/bionic/lib/liblog.so")
if(NOT EXISTS "${GUEST_LOG}")
  message(FATAL_ERROR "${GUEST_LOG} is missing: run tools/build-bionic.sh first")
endif()
add_library(guest_log SHARED IMPORTED)
set_target_properties(guest_log PROPERTIES IMPORTED_LOCATION "${GUEST_LOG}" IMPORTED_NO_SONAME FALSE
                      INTERFACE_INCLUDE_DIRECTORIES "${SOURCES}/logging/liblog/include")

# libbase with fmtlib.
sources_in(base_sources "${SOURCES}/libbase"
  abi_compatibility.cpp chrono_utils.cpp cmsg.cpp errors_unix.cpp file.cpp hex.cpp logging.cpp mapped_file.cpp
  parsebool.cpp parsenetaddress.cpp posix_strerror_r.cpp process.cpp properties.cpp result.cpp stringprintf.cpp
  strings.cpp threads.cpp)
add_library(base STATIC ${base_sources} "${SOURCES}/fmtlib/src/format.cc")
target_include_directories(base PUBLIC "${SOURCES}/libbase/include" "${SOURCES}/fmtlib/include")
soong_module(base FLAGS -Wall -Werror -Wextra -Wexit-time-destructors)
# fmt's format strings are checked at run time, as in the host aapt2 and runtime/art: fmt 10.2's compile-time check
# fails under this clang too.
target_compile_definitions(base PUBLIC _FILE_OFFSET_BITS=64 FMT_CONSTEVAL=)
target_link_libraries(base PUBLIC guest_log)

# libcutils with libcutils_sockets.
set(core "${SOURCES}/system_core_framework")
sources_in(cutils_sources "${core}/libcutils"
  config_utils.cpp iosched_policy.cpp load_file.cpp native_handle.cpp properties.cpp record_stream.cpp strlcpy.c
  fs.cpp hashmap.cpp multiuser.cpp str_parms.cpp android_reboot.cpp ashmem-dev.cpp klog.cpp partition_utils.cpp
  qtaguid.cpp trace-dev.cpp uevent.cpp sockets.cpp android_get_control_file.cpp socket_inaddr_any_server_unix.cpp
  socket_local_client_unix.cpp socket_local_server_unix.cpp socket_network_client_unix.cpp sockets_unix.cpp)
add_library(cutils STATIC ${cutils_sources})
target_include_directories(cutils PUBLIC "${core}/libcutils/include" "${core}/libsystem/include"
                           PRIVATE "${core}/libutils/include")
soong_module(cutils FLAGS -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(cutils PUBLIC base guest_log)

# libutils with libutils_binder.
sources_in(utils_sources "${core}/libutils"
  FileMap.cpp JenkinsHash.cpp LightRefBase.cpp NativeHandle.cpp Printer.cpp StopWatch.cpp SystemClock.cpp Threads.cpp
  Timers.cpp Tokenizer.cpp misc.cpp Trace.cpp Looper.cpp)
sources_in(utils_binder_sources "${core}/libutils/binder"
  Errors.cpp RefBase.cpp SharedBuffer.cpp String16.cpp String8.cpp StrongPointer.cpp Unicode.cpp VectorImpl.cpp)
# libvndksupport and libprocessgroup's headers are our stand-ins (native/include): misc.cpp loads libraries through
# libvndksupport, which native/vndksupport answers as a device without a vendor namespace, and Threads.cpp includes
# libprocessgroup without calling it.
add_library(utils STATIC ${utils_sources} ${utils_binder_sources} native/vndksupport/linker.cpp)
target_include_directories(utils PUBLIC "${core}/libutils/include" "${core}/libutils/binder/include"
                           PRIVATE native/include)
soong_module(utils FLAGS -Wall -Werror -Wno-exit-time-destructors -fvisibility=protected)
target_compile_definitions(utils PRIVATE ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION)
target_link_libraries(utils PUBLIC cutils base guest_log)

# libziparchive.
sources_in(ziparchive_sources "${SOURCES}/libziparchive"
  zip_archive.cc zip_archive_stream_entry.cc zip_cd_entry_map.cc zip_error.cpp zip_writer.cc)
add_library(ziparchive STATIC ${ziparchive_sources})
# tools/aapt2/gtest holds gtest_prod.h, which zip_writer.h includes for FRIEND_TEST.
target_include_directories(ziparchive PUBLIC "${SOURCES}/libziparchive/include" "${MINATO_ROOT}/tools/aapt2"
                           PRIVATE "${SOURCES}/libziparchive/incfs_support/include")
set_target_properties(ziparchive PROPERTIES CXX_STANDARD 20)
soong_module(ziparchive FLAGS -Werror $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers -Wconversion
  -Wno-sign-conversion -Wold-style-cast>)
target_compile_definitions(ziparchive PRIVATE ZLIB_CONST INCFS_SUPPORT_DISABLED=1)
target_link_libraries(ziparchive PUBLIC base guest_log ${guest_z})

# libincfs-utils, and our libincfs part.
set(incfs "${SOURCES}/incremental_delivery/incfs")
add_library(incfs_utils STATIC "${incfs}/util/map_ptr.cpp" native/incfs/incfs_minato.cpp)
target_include_directories(incfs_utils PUBLIC "${incfs}/util/include" "${incfs}/include" "${incfs}/kernel-headers")
soong_module(incfs_utils)
target_link_libraries(incfs_utils PUBLIC base utils)

# libpng, with its arm64 NEON code but not filter_neon.S, as for arm64 in its Android.bp.
file(GLOB png_sources "${SOURCES}/libpng/*.c")
list(REMOVE_ITEM png_sources "${SOURCES}/libpng/example.c" "${SOURCES}/libpng/pngtest.c")
sources_in(png_arm "${SOURCES}/libpng/arm" arm_init.c filter_neon_intrinsics.c palette_neon_intrinsics.c)
add_library(png STATIC ${png_sources} ${png_arm})
target_include_directories(png PUBLIC "${SOURCES}/libpng")
soong_module(png EXTERNAL FLAGS -std=gnu89 -Wall -Werror -Wno-unused-parameter -O3)
target_link_libraries(png PUBLIC ${guest_z})

# libandroidfw with libandroidfw_pathutils. AssetsProvider.cpp is the copy with Minato's patch
# (native/patches/androidfw-directory-listing.patch), which tools/build-framework-native.sh writes.
set(androidfw "${SOURCES}/frameworks_base/libs/androidfw")
set(androidfw_patched "${CMAKE_BINARY_DIR}/patched/libs/androidfw/AssetsProvider.cpp")
if(NOT EXISTS "${androidfw_patched}")
  message(FATAL_ERROR "${androidfw_patched} is missing: build with tools/build-framework-native.sh")
endif()
sources_in(androidfw_sources "${androidfw}"
  ApkAssets.cpp ApkParsing.cpp Asset.cpp AssetDir.cpp AssetManager.cpp AssetManager2.cpp
  AttributeResolution.cpp BigBuffer.cpp BigBufferStream.cpp ChunkIterator.cpp ConfigDescription.cpp CursorWindow.cpp
  FileStream.cpp
  Idmap.cpp LoadedArsc.cpp Locale.cpp LocaleData.cpp misc.cpp NinePatch.cpp ObbFile.cpp PosixUtils.cpp Png.cpp
  PngChunkFilter.cpp PngCrunch.cpp ResourceTimer.cpp ResourceTypes.cpp ResourceUtils.cpp StreamingZipInflater.cpp
  StringPool.cpp TypeWrappers.cpp Util.cpp ZipFileRO.cpp ZipUtils.cpp PathUtils.cpp)
add_library(androidfw STATIC ${androidfw_sources} "${androidfw_patched}")
# native/include stands in for libbinder's TextOutput.h, which ResourceTypes.cpp includes without calling.
target_include_directories(androidfw PUBLIC "${androidfw}/include" "${androidfw}/include_pathutils"
                           PRIVATE native/include)
# libandroidfw_defaults: cpp_std gnu++2b.
set_target_properties(androidfw PROPERTIES CXX_STANDARD 23)
soong_module(androidfw FLAGS -Werror -Wunreachable-code)
target_link_libraries(androidfw PUBLIC utils cutils ziparchive base guest_log incfs_utils png ${guest_z})

# ADR-0039 b3.2's test (tools/androidfw-check.sh).
add_executable(androidfw-test tests/native/androidfw_test.cpp)
set_target_properties(androidfw-test PROPERTIES CXX_STANDARD 23)
target_compile_options(androidfw-test PRIVATE -Wall -Wextra -Werror)
target_link_options(androidfw-test PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(androidfw-test PRIVATE androidfw)

# The guest's libsqlite.so test (compatibility plan, gap 7; tools/sqlite-native-check.sh): a program that opens the
# library with dlopen, as an app with a targetSdk below 24 may, so that it runs on Android 15's own library too.
add_executable(sqlite-native-test tests/native/sqlite_native_test.c)
target_include_directories(sqlite-native-test PRIVATE "${SOURCES}/sqlite/dist/sqlite-autoconf-3440300")
target_compile_options(sqlite-native-test PRIVATE -Wall -Wextra -Werror)
target_link_options(sqlite-native-test PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(sqlite-native-test PRIVATE dl)

# --- libsqlite.so (compatibility plan, gap 7): SQLite 3.44.3 of external/sqlite's dist at android-15.0.0_r1, the
# version that the tag's release flag RELEASE_PACKAGE_LIBSQLITE3 picks, with Android's functions of its android folder
# (libsqlite3_android: the phone numbers, the LOCALIZED, UNICODE and PHONEBOOK collators and _TOKENIZE) linked in
# whole, as Android.bp builds libsqlite for the android target: sqlite-defaults' flags and SQLITE_ENABLE_ICU. It lies
# in the guest's /system/lib64, as on Android 15, where the database natives of libandroid_runtime and apps with a
# targetSdk below 24 (the linker's exempt list) find it. ICU is libandroidicu, as on Android: ICU's C functions under
# the names of libandroidicu's headers, beside ART's ICU, where the default namespace reaches it as linkerconfig's link
# to com_android_i18n gives it.
set(sqlite "${SOURCES}/sqlite/dist/sqlite-autoconf-3440300")
set(icu_include "${SOURCES}/icu/libandroidicu/include")
add_library(sqlite3_android OBJECT "${SOURCES}/sqlite/android/PhoneNumberUtils.cpp"
            "${SOURCES}/sqlite/android/OldPhoneNumberUtils.cpp" "${SOURCES}/sqlite/android/sqlite3_android.cpp")
target_include_directories(sqlite3_android PRIVATE "${sqlite}" ${icu_include})
set_target_properties(sqlite3_android PROPERTIES POSITION_INDEPENDENT_CODE ON)
soong_module(sqlite3_android EXTERNAL FLAGS -Wall -Werror -Wno-unused-function -Wno-unused-variable -DSQLITE_ENABLE_ICU
  -DSQLITE_DEFAULT_LEGACY_ALTER_TABLE)
target_link_libraries(sqlite3_android PRIVATE guest_log)
add_library(sqlite SHARED "${sqlite}/sqlite3.c" $<TARGET_OBJECTS:sqlite3_android>)
target_include_directories(sqlite PUBLIC "${sqlite}" "${SOURCES}/sqlite/android" PRIVATE ${icu_include})
soong_module(sqlite EXTERNAL FLAGS -DNDEBUG=1 -DHAVE_USLEEP=1 -DSQLITE_HAVE_ISNAN
  -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576 -DSQLITE_THREADSAFE=2 -DSQLITE_TEMP_STORE=3
  -DSQLITE_POWERSAFE_OVERWRITE=1 -DSQLITE_DEFAULT_FILE_FORMAT=4 -DSQLITE_DEFAULT_AUTOVACUUM=1
  -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_BACKWARDS -DSQLITE_ENABLE_FTS4
  -DSQLITE_OMIT_BUILTIN_TEST -DSQLITE_OMIT_COMPILEOPTION_DIAGS -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_DEFAULT_FILE_PERMISSIONS=0600 -DSQLITE_SECURE_DELETE -DSQLITE_ENABLE_BATCH_ATOMIC_WRITE
  -DBIONIC_IOCTL_NO_SIGNEDNESS_OVERLOAD -DSQLITE_DEFAULT_LEGACY_ALTER_TABLE -DSQLITE_ALLOW_ROWID_IN_VIEW
  -DSQLITE_ENABLE_BYTECODE_VTAB -Wno-unused-parameter -Werror -ftrivial-auto-var-init=pattern
  -DUSE_PREAD64 -Dfdatasync=fdatasync -DHAVE_MALLOC_H=1 -DHAVE_MALLOC_USABLE_SIZE -DSQLITE_ENABLE_DBSTAT_VTAB
  -DSQLITE_ENABLE_ICU)
target_link_options(sqlite PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(sqlite PRIVATE dl guest_log art_libandroidicu)

# --- libhwui.so (ADR-0043 test 2): the natives of framework.jar's android.graphics classes, from the build of
# tools/build-hwui.sh, which a clean copy can point elsewhere with HWUI_LIB. libandroid_runtime calls the two C
# functions of hwui's jni_runtime.h, as AOSP's does: only they and a JNIEnv cross between its static libc++ and the
# libc++_shared of hwui's set.
set(HWUI_LIB "${MINATO_ROOT}/build/hwui/lib" CACHE PATH "libhwui.so and its set (tools/build-hwui.sh)")
if(NOT EXISTS "${HWUI_LIB}/libhwui.so")
  message(FATAL_ERROR "${HWUI_LIB}/libhwui.so is missing: run tools/build-hwui.sh first")
endif()
set(hwui_include "${SOURCES}/frameworks_base_hwui/libs/hwui/apex/include")
if(NOT EXISTS "${hwui_include}/android/graphics/jni_runtime.h")
  message(FATAL_ERROR "No ${hwui_include}/android/graphics/jni_runtime.h: run tools/fetch-sources.py --set graphics")
endif()
add_library(hwui SHARED IMPORTED)
set_target_properties(hwui PROPERTIES IMPORTED_LOCATION "${HWUI_LIB}/libhwui.so"
                      INTERFACE_INCLUDE_DIRECTORIES "${hwui_include}")

# --- libinput's MotionEvent and VelocityTracker (ADR-0045 step 7b1), for MotionEvent's and VelocityTracker's natives:
# libinput's Input.cpp and VelocityTracker.cpp at android-15.0.0_r1 as they stand (tools/fetch-sources.py --set
# framework), with ours for InputEventLabels (native/android_runtime/input_event_labels.cpp), and in native/include
# for the two AIDL headers, InputDevice.h's reserved device ids, an empty DisplayViewport.h and binder/Parcel.h onto
# minato_parcel.h; and libui's Transform with the Region and Rect it names. Soong's flags carry -ffp-contract=off, so
# the velocities' floats come out as Android's. Since step 7c1 also libinput's KeyCharacterMap.cpp as it stands, for
# KeyCharacterMap's natives, with ours for the meta-state functions of libinput's Keyboard.cpp
# (native/android_runtime/keyboard_meta_state.cpp) and a stand-in input/Keyboard.h in native/include.
set(input_sources "${SOURCES}/frameworks_native_input")
set(keys_sources "${SOURCES}/frameworks_native_keys")
set(ui_sources "${SOURCES}/frameworks_native_graphics/libs/ui")
foreach(source frameworks_native_keys frameworks_base_keyboards)
  if(NOT EXISTS "${SOURCES}/${source}")
    message(FATAL_ERROR "No ${SOURCES}/${source}: run tools/fetch-sources.py --set framework")
  endif()
endforeach()
add_library(input STATIC "${input_sources}/libs/input/Input.cpp" "${input_sources}/libs/input/VelocityTracker.cpp"
            "${keys_sources}/libs/input/KeyCharacterMap.cpp" native/android_runtime/input_event_labels.cpp
            native/android_runtime/keyboard_meta_state.cpp "${ui_sources}/Transform.cpp" "${ui_sources}/Region.cpp"
            "${ui_sources}/Rect.cpp")
target_include_directories(input PUBLIC "${input_sources}/include" "${keys_sources}/include" native/include
                           "${ui_sources}/include"
                           "${SOURCES}/frameworks_native_graphics/libs/math/include"
                           "${SOURCES}/frameworks_native_graphics/libs/arect/include"
                           "${SOURCES}/frameworks_native_headers/include" PRIVATE "${ui_sources}/include_private"
                           native/android_runtime)
set_target_properties(input PROPERTIES CXX_STANDARD 20)
soong_module(input FLAGS -Wall -Wextra -Werror -Wno-unused-parameter)
# libmath's half.h specializes std::is_floating_point, which this clang refuses unless told; its users need it too.
target_compile_options(input PUBLIC -Wno-invalid-specialization)
target_link_libraries(input PUBLIC utils cutils base guest_log PRIVATE art_libnativehelper)
# AOSP's core/jni files for MotionEvent's and VelocityTracker's natives, and since step 7c1 for KeyEvent's and
# KeyCharacterMap's.
sources_in(input_jni_sources "${SOURCES}/frameworks_base/core/jni"
  android_view_MotionEvent.cpp android_view_VelocityTracker.cpp android_view_KeyEvent.cpp
  android_view_KeyCharacterMap.cpp)
# The virtual keyboard's key character map (ADR-0045 step 7c1): Virtual.kcm's text as a C array with its NUL, which the
# input service's native parses under Android's path (native/android_runtime/android_hardware_input_InputService.cpp),
# rewritten only when the file changes.
# Generic.kcm is built in the same way (ADR-0068 step 7c2), for the hardware keyboards the host adds.
foreach(kcm Virtual Generic)
  string(TOLOWER "${kcm}" kcm_lower)
  set(kcm_file "${SOURCES}/frameworks_base_keyboards/data/keyboards/${kcm}.kcm")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kcm_file}")
  file(READ "${kcm_file}" kcm_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," kcm_bytes "${kcm_hex}")
  file(WRITE "${CMAKE_BINARY_DIR}/generated/${kcm_lower}_kcm.cpp.new"
       "// Made by framework/CMakeLists.txt from ${kcm}.kcm (ADR-0045 step 7c1, ADR-0068 step 7c2).\n"
       "extern const char k${kcm}Kcm[];\nconst char k${kcm}Kcm[] = {${kcm_bytes}0};\n")
  configure_file("${CMAKE_BINARY_DIR}/generated/${kcm_lower}_kcm.cpp.new"
                 "${CMAKE_BINARY_DIR}/generated/${kcm_lower}_kcm.cpp" COPYONLY)
endforeach()

# --- libandroid_runtime.so (ADR-0039 decision 10, b3.3): the natives of framework.jar's classes. AOSP's core/jni
# files as they stand where they need only libandroidfw, liblog and Bionic's properties; ours (native/android_runtime)
# for AndroidRuntime's statics, Trace without Perfetto, the one function of android_util_Binder.cpp they call, and
# MessageQueue on M3's ALooper (step c1), and Parcel's natives (ADR-0043 d2) over minato_parcel, libbinder's
# Parcel for the parcels of one process in its wire format.
# libandroidfw and the libraries under it are linked in statically with --exclude-libs,ALL: ART's APEX has its own
# libbase.so and libziparchive.so, which our copies must not interpose (Android keeps the two in separate
# namespaces). Their state is ours too: our libbase's LOG goes straight to the guest's liblog. Flags: the
# libandroid_runtime module's for the android target, with cpp_std gnu++20, without GCC's -Wno-maybe-uninitialized,
# which this clang refuses.
set(jni "${SOURCES}/frameworks_base/core/jni")
sources_in(android_runtime_sources "${jni}"
  android_util_Log.cpp android_util_AssetManager.cpp android_util_StringBlock.cpp android_util_XmlBlock.cpp
  android_content_res_ApkAssets.cpp android_os_SystemProperties.cpp android_os_SystemClock.cpp core_jni_helpers.cpp
  android_animation_PropertyValuesHolder.cpp com_android_internal_util_VirtualRefBasePtr.cpp)
# SQLite's natives (the compatibility plan's gap 7): AOSP's core/jni files as they stand, over libsqlite.so and
# libandroidfw's CursorWindow.
sources_in(database_jni_sources "${jni}"
  android_database_CursorWindow.cpp android_database_SQLiteCommon.cpp android_database_SQLiteConnection.cpp
  android_database_SQLiteDebug.cpp android_database_SQLiteGlobal.cpp android_database_SQLiteRawStatement.cpp)
# ActivityThread's native (b3.4c) reads the session's configuration through libandroid, which it opens with dlopen
# rather than linking: on Android libandroid needs libandroid_runtime, not the other way. DisplayEventReceiver's
# natives (ADR-0041 a)) take libandroid's AChoreographer the same way.
# InputChannel's and InputEventReceiver's (ADR-0045 step 7b2), ours on the session's input channel, take its
# ALooper the same way.
# The GLES bindings (ADR-0041 a), decision 4): AOSP's core/jni files for android.opengl, javax.microedition.khronos and
# com.google.android.gles_jni, five of them in tools/build-framework-native.sh's patched copies
# (native/patches/opengl-without-libgui.patch: no libgui, libui or libEGL's private API, and no ETC1), with ours for a
# Surface's window (android_view_Surface.cpp) and Unwritten's line (minato_gles.cpp). They call EGL and GLES
# directly, so the library links the NDK's libEGL, libGLESv1_CM, libGLESv2 and libGLESv3, as an app does; Minato's
# front libs of the same names answer at run time.
# GLUtils' bitmap natives take a Bitmap through hwui's ABitmap C API (a3). Surface's natives (ADR-0043 c2) lock and
# post a window through libnativewindow and draw on it through hwui's ACanvas C API.
sources_in(gles_sources "${jni}"
  android_opengl_GLES10.cpp android_opengl_GLES10Ext.cpp android_opengl_GLES11.cpp android_opengl_GLES11Ext.cpp
  android_opengl_GLES20.cpp android_opengl_GLES30.cpp android_opengl_GLES31.cpp android_opengl_GLES31Ext.cpp
  android_opengl_GLES32.cpp com_google_android_gles_jni_GLImpl.cpp android/opengl/poly_clip.cpp)
set(gles_patched "${CMAKE_BINARY_DIR}/patched/core/jni")
foreach(file android_opengl_EGL14.cpp android_opengl_EGL15.cpp android_opengl_EGLExt.cpp
    com_google_android_gles_jni_EGLImpl.cpp android/opengl/util.cpp)
  if(NOT EXISTS "${gles_patched}/${file}")
    message(FATAL_ERROR "${gles_patched}/${file} is missing: build with tools/build-framework-native.sh")
  endif()
  list(APPEND gles_sources "${gles_patched}/${file}")
endforeach()
add_library(android_runtime SHARED ${android_runtime_sources} ${gles_sources} ${input_jni_sources}
            ${database_jni_sources} native/android_runtime/AndroidRuntime.cpp
            native/android_runtime/android_os_MessageQueue.cpp native/android_runtime/android_os_Trace.cpp
            native/android_runtime/android_os_Debug.cpp
            native/android_runtime/android_util_Process.cpp
            native/android_runtime/android_util_Binder.cpp native/android_runtime/android_app_ActivityThread.cpp
            native/android_runtime/android_view_DisplayEventReceiver.cpp native/android_runtime/android_view_Surface.cpp
            native/android_runtime/android_view_TextureView.cpp
            native/android_runtime/com_android_internal_minato_webkit_WebViewChannel.cpp
            native/android_runtime/android_hardware_display_DisplayService.cpp
            native/android_runtime/android_hardware_SensorManager.cpp
            native/android_runtime/android_hardware_HardwareBuffer.cpp
            native/android_runtime/android_hardware_SyncFence.cpp
            native/android_runtime/android_media_ImageReader.cpp
            native/android_runtime/android_media_PublicFormatUtils.cpp
            native/android_runtime/android_media_ImageWriter.cpp
            native/android_runtime/android_media_MediaCodec.cpp
            native/android_runtime/android_media_MediaExtractor.cpp
            native/android_runtime/android_media_MediaPlayer.cpp native/android_runtime/ogg_wav.cpp
            native/android_runtime/opus_wav.cpp native/android_runtime/vorbis_wav.cpp
            native/android_runtime/android_hardware_camera2_utils_SurfaceUtils.cpp
            "${SOURCES}/frameworks_native_graphics/libs/ui/PublicFormat.cpp"
            "${SOURCES}/frameworks_native_etc1/opengl/libs/ETC1/etc1.cpp"
            native/android_runtime/android_hardware_Camera.cpp
            native/android_runtime/android_media_AudioTrack.cpp
            native/android_runtime/android_media_SoundPool.cpp "${SOURCES}/stb_vorbis/stb_vorbis.c"
            native/android_runtime/android_graphics_HardwareRenderer.cpp
            native/android_runtime/android_graphics_SurfaceTexture.cpp
            native/android_runtime/android_util_EventLog.cpp native/android_runtime/android_app_Activity.cpp
            native/android_runtime/android_app_NativeActivity.cpp native/android_runtime/android_view_InputQueue.cpp
            native/android_runtime/minato_gles.cpp native/android_runtime/android_hardware_input_InputService.cpp
            "${CMAKE_BINARY_DIR}/generated/virtual_kcm.cpp" "${CMAKE_BINARY_DIR}/generated/generic_kcm.cpp"
            native/android_runtime/android_view_InputChannel.cpp
            native/android_runtime/android_view_InputEventReceiver.cpp
            native/android_runtime/android_os_Parcel.cpp native/android_runtime/minato_parcel.cpp)
target_include_directories(android_runtime PUBLIC native/android_runtime "${jni}/include" "${NATIVEHELPER}/include_jni"
                           PRIVATE "${jni}" "${jni}/android/opengl" native/include "${NATIVEHELPER}/include"
                           "${NATIVEHELPER}/include_platform" "${NATIVEHELPER}/header_only_include" ../runtime/include)
# ETC1's natives in core/jni's android/opengl/util.cpp and libETC1's etc1.cpp (the graphics breadth's G1) include
# <ETC1/etc1.h>, from frameworks/native's opengl/include, whose EGL and GLES headers stay the NDK's: only ETC1's folder.
file(COPY "${SOURCES}/frameworks_native_headers/opengl/include/ETC1" DESTINATION "${CMAKE_BINARY_DIR}/etc1-include")
target_include_directories(android_runtime PRIVATE "${CMAKE_BINARY_DIR}/etc1-include")
set_target_properties(android_runtime PROPERTIES CXX_STANDARD 20)
# stb_vorbis (Sean Barrett, MIT or public domain; third_party/sources.json): the Ogg Vorbis decoder of SoundPool, whole
# files from memory, and of MediaPlayer (vorbis_wav.cpp), a mapped file decoded where it is read. Third-party C, so no
# warnings.
set_source_files_properties("${SOURCES}/stb_vorbis/stb_vorbis.c" PROPERTIES COMPILE_OPTIONS "-w"
  COMPILE_DEFINITIONS "STB_VORBIS_NO_STDIO;STB_VORBIS_NO_PUSHDATA_API")
soong_module(android_runtime FLAGS -Wno-unused-parameter -Wno-non-virtual-dtor -Wno-parentheses
  -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES -DU_USING_ICU_NAMESPACE=0 -Wall -Werror -Wextra
  -Wthread-safety -Wno-error=deprecated-declarations -Wunused -Wunreachable-code
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion-null>)
target_link_options(android_runtime PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
target_link_libraries(android_runtime PUBLIC utils PRIVATE androidfw art_libnativehelper guest_log hwui EGL GLESv1_CM
                      GLESv2 GLESv3 nativewindow input sqlite aaudio mediandk)

# app_process registers libandroid_runtime's natives with register_framework_natives, so it links the library, as
# Android's app_process does, and sets liblog's default tag as zygote's SetThreadName does.
target_link_libraries(app_process PRIVATE android_runtime)
