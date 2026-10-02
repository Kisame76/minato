# The framework's guest code (ADR-0039), built with the NDK's toolchain as the fixtures are
# (tools/build-framework-native.sh): app_process, libandroidfw with the libraries it links (b3.2), and
# libandroid_runtime.so with framework.jar's natives (b3.3), which app_process registers.
# app_process links the guest's ART libraries of tools/build-art.sh as dalvikvm does
# (runtime/art, build/art/plan.cmake): libnativehelper and libsigchain ahead of libc, liblog, and --export-dynamic.
# Its C++ library is static: a /system/bin program finds no libc++_shared.so, since ART's copy stays in its APEX.
# JNI's headers come from the fetched libnativehelper.
cmake_minimum_required(VERSION 3.24)
project(MinatoFramework LANGUAGES C CXX)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build the framework's guest code with tools/build-framework-native.sh (Android NDK, arm64-v8a).")
endif()

set(MINATO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ART_LIB "${MINATO_ROOT}/build/art/lib" CACHE PATH "ART's guest libraries (tools/build-art.sh)")
set(NATIVEHELPER "${MINATO_ROOT}/build/sources/libnativehelper")
foreach(library IN ITEMS libnativehelper libsigchain)
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
target_link_options(app_process PRIVATE -Wl,--export-dynamic -Wl,-z,max-page-size=16384)
# In this order, as dalvikvm's DT_NEEDED: libsigchain before libc, which the toolchain adds last.
target_link_libraries(app_process PRIVATE art_libnativehelper art_libsigchain log)

# --- libandroidfw and what it links (ADR-0039 b3.2), as static archives for libandroid_runtime.so (b3.3). Each
# library takes its Android.bp's sources and flags for the android target, with Soong's global flags as for the host
# aapt2 (tools/aapt2/soong.cmake) on top of the NDK's own; liblog and libz are the guest system's. Left out, as no
# caller needs them before their Java comes: libandroidfw's BackupData, BackupHelpers and CursorWindow, which need
# libbinder. libincfs is our two-function stand-in (native/incfs), and libziparchive is the default one, with
# incfs support off, rather than libziparchive_for_incfs: Minato installs no app on incfs.
set(SOURCES "${MINATO_ROOT}/build/sources")
foreach(source frameworks_base system_core_framework incremental_delivery libbase fmtlib libziparchive logging libpng)
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
  AttributeResolution.cpp BigBuffer.cpp BigBufferStream.cpp ChunkIterator.cpp ConfigDescription.cpp FileStream.cpp
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

# --- libandroid_runtime.so (ADR-0039 decision 10, b3.3): the natives of framework.jar's classes. AOSP's core/jni
# files as they stand where they need only libandroidfw, liblog and Bionic's properties; ours (native/android_runtime)
# for AndroidRuntime's statics, Trace without Perfetto and the one function of android_util_Binder.cpp they call.
# libandroidfw and the libraries under it are linked in statically with --exclude-libs,ALL: ART's APEX has its own
# libbase.so and libziparchive.so, which our copies must not interpose (Android keeps the two in separate
# namespaces). Their state is ours too: our libbase's LOG goes straight to the guest's liblog. Flags: the
# libandroid_runtime module's for the android target, with cpp_std gnu++20, without GCC's -Wno-maybe-uninitialized,
# which this clang refuses.
set(jni "${SOURCES}/frameworks_base/core/jni")
sources_in(android_runtime_sources "${jni}"
  android_util_Log.cpp android_util_AssetManager.cpp android_util_StringBlock.cpp android_util_XmlBlock.cpp
  android_content_res_ApkAssets.cpp android_os_SystemProperties.cpp core_jni_helpers.cpp)
add_library(android_runtime SHARED ${android_runtime_sources} native/android_runtime/AndroidRuntime.cpp
            native/android_runtime/android_os_Trace.cpp native/android_runtime/android_util_Binder.cpp)
target_include_directories(android_runtime PUBLIC native/android_runtime "${jni}/include" "${NATIVEHELPER}/include_jni"
                           PRIVATE "${jni}" native/include "${NATIVEHELPER}/include" "${NATIVEHELPER}/include_platform"
                           "${NATIVEHELPER}/header_only_include")
set_target_properties(android_runtime PROPERTIES CXX_STANDARD 20)
soong_module(android_runtime FLAGS -Wno-unused-parameter -Wno-non-virtual-dtor -Wno-parentheses
  -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES -DU_USING_ICU_NAMESPACE=0 -Wall -Werror -Wextra
  -Wthread-safety -Wno-error=deprecated-declarations -Wunused -Wunreachable-code
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion-null>)
target_link_options(android_runtime PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
target_link_libraries(android_runtime PUBLIC utils PRIVATE androidfw art_libnativehelper guest_log)

# app_process registers libandroid_runtime's natives with register_framework_natives, so it links the library, as
# Android's app_process does, and sets liblog's default tag as zygote's SetThreadName does.
target_link_libraries(app_process PRIVATE android_runtime)
