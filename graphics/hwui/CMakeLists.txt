# hwui as guest code (ADR-0043 a1): hwui's host flavor with Skia, minikin, HarfBuzz, FreeType and the image codecs,
# and the NDK's libjnigraphics over it (ADR-0041 a5)
# (tools/fetch-sources.py --set graphics), built with the NDK's clang as Android arm64 platform code. Which sources and
# flags come from the projects' own Android.bp files (tools/hwui-plan.py), with Minato's modules of
# graphics/hwui/modules.bp, the files of graphics/hwui/overlay in place of AOSP's and the patches of
# graphics/hwui/patches, each with the reason it exists. hwui links the guest system's libraries: bionic's
# (tools/build-bionic.sh), those of ART's build it shares (tools/build-art.sh), and the NDK's libc++_shared.so, which
# ART's folder holds. Built by tools/build-hwui.sh; the libraries go to <build>/lib, and the digests of the patches
# they carry to <build>/patches.sha256.
cmake_minimum_required(VERSION 4.0)
project(MinatoHwui LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build hwui with tools/build-hwui.sh (Android NDK, arm64-v8a).")
endif()

set(APK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(APK_SOURCES "${APK_ROOT}/build/sources" CACHE PATH "The fetched AOSP sources (tools/fetch-sources.py --set graphics)")
set(APK_BIONIC_LIB "${APK_ROOT}/build/bionic/lib" CACHE PATH "The guest system's libraries (tools/build-bionic.sh)")
set(APK_ART_LIB "${APK_ROOT}/build/art/lib" CACHE PATH "ART's libraries (tools/build-art.sh), some of which hwui links")
set(APK_HWUI_GENERATED "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${APK_HWUI_GENERATED}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/archive")

# From the NDK: libc++ (its headers and libc++_shared.so), the compiler builtins and the CRT objects.
set(ndk_libcxx_include "${CMAKE_SYSROOT}/usr/include/c++/v1")
set(ndk_libs "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android")
set(ndk_libcxx_shared "${ndk_libs}/libc++_shared.so")
file(GLOB ndk_clang_libs "${ANDROID_TOOLCHAIN_ROOT}/lib/clang/*/lib/linux")
set(ndk_builtins "${ndk_clang_libs}/libclang_rt.builtins-aarch64-android.a")
set(ndk_crt "${ndk_libs}/${ANDROID_PLATFORM_LEVEL}")
foreach(file IN ITEMS "${ndk_libcxx_include}/vector" "${ndk_libcxx_shared}" "${ndk_builtins}" "${ndk_crt}/crtbegin_so.o"
    "${ndk_crt}/crtend_so.o")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "missing from the NDK: ${file}")
  endif()
endforeach()

# The guest system's libraries that hwui links: bionic's, with Minato's libandroid for libjnigraphics, and those of
# ART's build it shares (tools/hwui-plan.py's GUEST_SYSTEM).
foreach(library IN ITEMS libc libm libdl libdl_android ld-android liblog libz libandroid)
  if(NOT EXISTS "${APK_BIONIC_LIB}/${library}.so")
    message(FATAL_ERROR "${APK_BIONIC_LIB}/${library}.so is missing: run tools/build-bionic.sh first")
  endif()
  add_library(hwui_lib_${library} SHARED IMPORTED)
  set_target_properties(hwui_lib_${library} PROPERTIES IMPORTED_LOCATION "${APK_BIONIC_LIB}/${library}.so")
endforeach()
foreach(library IN ITEMS libbase libexpat libicu libicui18n libicuuc libnativehelper libziparchive)
  if(NOT EXISTS "${APK_ART_LIB}/${library}.so")
    message(FATAL_ERROR "${APK_ART_LIB}/${library}.so is missing: run tools/build-art.sh first")
  endif()
  add_library(hwui_lib_${library} SHARED IMPORTED)
  set_target_properties(hwui_lib_${library} PROPERTIES IMPORTED_LOCATION "${APK_ART_LIB}/${library}.so")
endforeach()

# Soong's global flags for Android arm64 platform code, as for ART (runtime/art/CMakeLists.txt): 16 KiB pages leave
# bionic's PAGE_SIZE undefined (ADR-0007), Soong's global C++ flags, fmt's format strings checked at run time; the
# NDK's libc++, newer than Android 15's, refusing specializations of its traits that the projects make (math/half.h's
# is_floating_point) unless told; and -Wno-error last, since the projects' -Werror was meant for the clang of their
# time. The language standards come from the plan, after the module's cflags, where Soong puts them (ADR-0043 a2): one
# of ours before them would win over the plan's when they are equal, since CMake drops a repeated option.
set(hwui_target "--target=aarch64-linux-android10000")
# The -ffile-prefix-map flags that keep the Mac's build paths out of libhwui (tools/prefix-map-flags.sh,
# tools/check-no-build-paths.sh). They must ride on the per-unit flags below: the NDK toolchain file's
# CMAKE_CXX_FLAGS_INIT wins over a -DCMAKE_CXX_FLAGS, so flags passed that way never reach hwui_unit's compiles.
separate_arguments(apk_prefix_map NATIVE_COMMAND "${APK_PREFIX_MAP}")
set(hwui_flags ${hwui_target} -nostdlibinc -fPIC -ffunction-sections -fdata-sections -fno-short-enums -funwind-tables
  -fstack-protector-strong -fno-exceptions -fno-strict-aliasing -ffp-contract=off -ftrivial-auto-var-init=zero
  -march=armv8-a -O2 -DANDROID -DNDEBUG -D__BIONIC_NO_PAGE_SIZE_MACRO ${apk_prefix_map})
set(hwui_cppflags -fno-rtti -fvisibility-inlines-hidden -Wsign-promo -Wno-gnu-include-next -DFMT_CONSTEVAL=
  -Wno-invalid-specialization)
set(hwui_link_flags ${hwui_target} -nostdlib -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-z,noexecstack
  -Wl,-z,relro -Wl,-z,now -Wl,--build-id=md5 -Wl,--hash-style=gnu)

# One module's compile unit. libc++'s headers come before bionic's (SYSTEM_INCLUDES), whose headers they include
# with #include_next.
function(hwui_unit name)
  cmake_parse_arguments(PARSE_ARGV 1 unit "" ""
    "SOURCES;GENERATED_HEADERS;CFLAGS;CONLYFLAGS;CPPFLAGS;ASFLAGS;INCLUDES;SYSTEM_INCLUDES")
  add_library(${name} OBJECT ${unit_SOURCES} ${unit_GENERATED_HEADERS})
  if(unit_GENERATED_HEADERS)
    set_source_files_properties(${unit_GENERATED_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()
  target_include_directories(${name} PRIVATE ${unit_INCLUDES})
  target_include_directories(${name} SYSTEM BEFORE PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${ndk_libcxx_include}>")
  target_include_directories(${name} SYSTEM PRIVATE ${unit_SYSTEM_INCLUDES})
  target_compile_options(${name} PRIVATE ${hwui_flags}
    "$<$<COMPILE_LANGUAGE:C>:${unit_CFLAGS};${unit_CONLYFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${hwui_cppflags};${unit_CFLAGS};${unit_CPPFLAGS}>"
    "$<$<COMPILE_LANGUAGE:ASM>:-D__ASSEMBLY__;${unit_ASFLAGS}>"
    -Wno-error)
endfunction()

# A shared library of UNITS, the static libraries of ARCHIVES, the libraries of NEEDED (the set's, or the guest
# system's), and libc++_shared.so where it is used.
function(hwui_library name)
  cmake_parse_arguments(PARSE_ARGV 1 library "" "STL;VERSION_SCRIPT;NOCRT" "UNITS;ARCHIVES;NEEDED;LDFLAGS")
  set(objects)
  foreach(unit IN LISTS library_UNITS)
    list(APPEND objects "$<TARGET_OBJECTS:${unit}>")
  endforeach()
  set(target hwui_lib_${name})
  add_library(${target} SHARED ${objects})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name} PREFIX "" SUFFIX ".so" LINKER_LANGUAGE C)
  if(NOT library_NOCRT)
    target_sources(${target} PRIVATE "${ndk_crt}/crtbegin_so.o")
  endif()
  target_link_options(${target} PRIVATE ${hwui_link_flags} ${library_LDFLAGS})
  if(library_VERSION_SCRIPT)
    target_link_options(${target} PRIVATE "-Wl,--version-script=${library_VERSION_SCRIPT}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${library_VERSION_SCRIPT}")
  endif()
  foreach(unit IN LISTS library_ARCHIVES)
    if(NOT TARGET ${unit}_archive)
      add_library(${unit}_archive STATIC "$<TARGET_OBJECTS:${unit}>")
    endif()
    target_link_libraries(${target} PRIVATE ${unit}_archive)
  endforeach()
  foreach(needed IN LISTS library_NEEDED)
    target_link_libraries(${target} PRIVATE hwui_lib_${needed})
  endforeach()
  if(library_STL MATCHES "static")
    target_link_libraries(${target} PRIVATE "-Wl,--exclude-libs,ALL" "${ndk_libs}/libc++_static.a"
      "${ndk_libs}/libc++abi.a")
  elseif(NOT library_STL STREQUAL "none")
    target_link_libraries(${target} PRIVATE "-Wl,--push-state,--as-needed" "${ndk_libcxx_shared}" "-Wl,--pop-state")
  endif()
  target_link_libraries(${target} PRIVATE "${ndk_builtins}")
  if(NOT library_NOCRT)
    target_link_libraries(${target} PRIVATE "${ndk_crt}/crtend_so.o")
  endif()
endfunction()

# A generator's command, run in the tree's root (the plan's APK_HWUI_TREE) as Soong runs it.
function(hwui_generate)
  cmake_parse_arguments(PARSE_ARGV 0 generate "" "COMMAND" "DEPENDS")
  set(outputs ${generate_UNPARSED_ARGUMENTS})
  set(directories)
  foreach(output IN LISTS outputs)
    get_filename_component(directory "${output}" DIRECTORY)
    list(APPEND directories "${directory}")
  endforeach()
  list(REMOVE_DUPLICATES directories)
  add_custom_command(OUTPUT ${outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${directories}
    COMMAND sh -c "${generate_COMMAND}"
    DEPENDS ${generate_DEPENDS}
    WORKING_DIRECTORY "${APK_HWUI_TREE}" VERBATIM)
endfunction()

execute_process(COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/hwui-plan.py" "${APK_SOURCES}"
  "${CMAKE_BINARY_DIR}/plan.cmake" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "tools/hwui-plan.py failed")
endif()
# The plan also lays out the tree, with the overlay's files and the patches, and generates the flags and the system
# properties: a new file, a changed patch or a changed generator plans again.
file(GLOB_RECURSE hwui_changes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overlay/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APK_ROOT}/tools/hwui-plan.py"
  "${APK_ROOT}/tools/soong_plan.py" "${APK_ROOT}/tools/blueprint.py" "${APK_ROOT}/tools/aconfig-flags.py"
  "${APK_ROOT}/tools/sysprop.py" "${CMAKE_CURRENT_SOURCE_DIR}/modules.bp" ${hwui_changes})
include("${CMAKE_BINARY_DIR}/plan.cmake")

# The patches this build carries, as shasum -a 256 lists them, in patches.sha256 once every library of the set is
# linked: app_process loads hwui at every start (ADR-0043 test 2), and a build of older patches crashes there (patch
# 0007), so graphics.hwui_load, and graphics/tests/hwui_ready.py before every app process, compare it with
# graphics/hwui/patches.
set(patch_digests "")
file(GLOB hwui_patches RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
foreach(patch IN LISTS hwui_patches)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${patch}" digest)
  string(APPEND patch_digests "${digest}  ${patch}\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/patches.sha256.next" CONTENT "${patch_digests}" @ONLY)
get_property(hwui_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(hwui_libraries)
foreach(target IN LISTS hwui_targets)
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "SHARED_LIBRARY")
    list(APPEND hwui_libraries ${target})
  endif()
endforeach()
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/patches.sha256"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/patches.sha256.next" "${CMAKE_BINARY_DIR}/patches.sha256"
  DEPENDS "${CMAKE_BINARY_DIR}/patches.sha256.next" ${hwui_libraries} VERBATIM)
add_custom_target(hwui_patches_stamp ALL DEPENDS "${CMAKE_BINARY_DIR}/patches.sha256")
