# The guest libc (ADR-0021): AOSP bionic android-15.0.0_r1 (tools/fetch-sources.py), built with the
# NDK's clang as Android arm64 platform code: ld-android.so (the stub libc.so links against),
# libdl.so, libc.so and libm.so. Which sources and flags come from bionic's own Android.bp files
# (tools/bionic-plan.py). Built by tools/build-bionic.sh; the results go to <build>/lib.
cmake_minimum_required(VERSION 4.0)
project(APKRunnerBionic LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build bionic with tools/build-bionic.sh (Android NDK, arm64-v8a).")
endif()

set(APK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(APK_SOURCES "${APK_ROOT}/build/sources" CACHE PATH "The fetched AOSP sources (tools/fetch-sources.py)")
set(APK_BIONIC_GENERATED "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${APK_BIONIC_GENERATED}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/archive")

# Stand-ins for projects we do not fetch: the liblog header from the NDK (only android/log.h, the
# NDK's other headers would shadow bionic's), LLVM's libunwind and the compiler builtins.
set(ndk_sysroot_include "${CMAKE_SYSROOT}/usr/include")
file(COPY "${ndk_sysroot_include}/android/log.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_liblog/include/android")
file(GLOB ndk_clang_libs "${ANDROID_TOOLCHAIN_ROOT}/lib/clang/*/lib/linux")
set(ndk_builtins "${ndk_clang_libs}/libclang_rt.builtins-aarch64-android.a")
set(ndk_libunwind "${ndk_clang_libs}/aarch64/libunwind.a")
set(ndk_crt "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/${ANDROID_PLATFORM_LEVEL}")
foreach(file IN ITEMS "${ndk_builtins}" "${ndk_libunwind}" "${ndk_crt}/crtbegin_so.o" "${ndk_crt}/crtend_so.o")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "missing from the NDK: ${file}")
  endif()
endforeach()

# Android's "libunwind-exported": the NDK's libunwind with its hidden symbols made visible, so that
# libc.so can export _Unwind_Backtrace and friends as Android's does (LIBC_R).
set(libunwind_exported "${CMAKE_BINARY_DIR}/libunwind-exported.a")
add_custom_command(OUTPUT "${libunwind_exported}"
  COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/export-archive-symbols.py" "${CMAKE_AR}" "${ndk_libunwind}"
    "${libunwind_exported}"
  DEPENDS "${ndk_libunwind}" "${APK_ROOT}/tools/export-archive-symbols.py" VERBATIM)
add_custom_target(libunwind_exported DEPENDS "${libunwind_exported}")

# Soong's global flags for Android arm64 platform code (build/soong/cc/config), as far as they
# matter here: every API is visible (android10000), only bionic's own headers (-nostdlibinc), 16 KiB
# pages (ADR-0007). -Wno-error comes last: bionic's -Werror was meant for the clang of its time.
# PLATFORM_SDK_VERSION is a Soong product variable: 35 for Android 15.
set(apk_target "--target=aarch64-linux-android10000")
set(apk_flags ${apk_target} -nostdlibinc -fPIC -ffunction-sections -fdata-sections -fno-short-enums
  -funwind-tables -fstack-protector-strong -fno-exceptions -fno-strict-aliasing -ffp-contract=off
  -ftrivial-auto-var-init=zero -march=armv8-a -O2 -DANDROID -DNDEBUG)
set(apk_link_flags ${apk_target} -nostdlib -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-z,noexecstack
  -Wl,-z,relro -Wl,-z,now -Wl,--build-id=md5 -Wl,--hash-style=gnu)

function(apk_bionic_unit name)
  cmake_parse_arguments(PARSE_ARGV 1 unit "" "" "SOURCES;CFLAGS;CONLYFLAGS;CPPFLAGS;ASFLAGS;INCLUDES")
  add_library(${name} OBJECT ${unit_SOURCES})
  target_include_directories(${name} PRIVATE ${unit_INCLUDES} "${CMAKE_BINARY_DIR}/ndk_liblog/include")
  target_compile_options(${name} PRIVATE ${apk_flags}
    "$<$<COMPILE_LANGUAGE:C>:-std=gnu17;${unit_CFLAGS};${unit_CONLYFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;${unit_CFLAGS};${unit_CPPFLAGS}>"
    "$<$<COMPILE_LANGUAGE:ASM>:-D__ASSEMBLY__;-DPLATFORM_SDK_VERSION=35;${unit_ASFLAGS}>"
    -Wno-error)
endfunction()

function(apk_bionic_library name)
  cmake_parse_arguments(PARSE_ARGV 1 library "" "NOCRT" "UNITS;STATIC;NEEDED;SUBSTITUTES;LDFLAGS;VERSION_SCRIPTS")
  set(objects)
  foreach(unit IN LISTS library_UNITS)
    list(APPEND objects "$<TARGET_OBJECTS:${unit}>")
  endforeach()
  set(target guest_${name})
  add_library(${target} SHARED ${objects})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name} PREFIX "" SUFFIX ".so" LINKER_LANGUAGE C)
  target_link_options(${target} PRIVATE ${apk_link_flags} ${library_LDFLAGS})
  if(library_VERSION_SCRIPTS)
    foreach(script IN LISTS library_VERSION_SCRIPTS)
      target_link_options(${target} PRIVATE "-Wl,--version-script=${script}")
      set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${script}")
    endforeach()
    # Version scripts can be generated: make the link wait for them.
    add_custom_target(${target}_version_scripts DEPENDS ${library_VERSION_SCRIPTS})
    add_dependencies(${target} ${target}_version_scripts)
  endif()
  if(NOT library_NOCRT)
    target_sources(${target} PRIVATE "${ndk_crt}/crtbegin_so.o")
  endif()
  foreach(static IN LISTS library_STATIC)
    string(MAKE_C_IDENTIFIER "bionic_${static}" unit)
    if(TARGET ${unit})
      if(NOT TARGET guest_static_${static})
        add_library(guest_static_${static} STATIC "$<TARGET_OBJECTS:${unit}>")
      endif()
      target_link_libraries(${target} PRIVATE guest_static_${static})
    endif()
  endforeach()
  foreach(needed IN LISTS library_NEEDED)
    target_link_libraries(${target} PRIVATE guest_${needed})
  endforeach()
  if("ndk_libunwind" IN_LIST library_SUBSTITUTES)
    add_dependencies(${target} libunwind_exported)
    target_link_libraries(${target} PRIVATE "-Wl,--whole-archive" "${libunwind_exported}" "-Wl,--no-whole-archive")
  endif()
  target_link_libraries(${target} PRIVATE "${ndk_builtins}")
  if(NOT library_NOCRT)
    target_link_libraries(${target} PRIVATE "${ndk_crt}/crtend_so.o")
  endif()
endfunction()

execute_process(COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/bionic-plan.py" "${APK_SOURCES}" "${CMAKE_BINARY_DIR}/plan.cmake"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "tools/bionic-plan.py failed")
endif()
include("${CMAKE_BINARY_DIR}/plan.cmake")

# liblog.so: APK-Runner's own (runtime/bionic/src/liblog.cpp), because Android's needs the logd
# service. The NDK's API without symbol versions, like the NDK's stub library.
set(bionic_include "${APK_SOURCES}/bionic/libc/include" "${APK_SOURCES}/bionic/libc/kernel/uapi"
  "${APK_SOURCES}/bionic/libc/kernel/android/uapi" "${APK_SOURCES}/bionic/libc/kernel/uapi/asm-arm64")
apk_bionic_unit(apk_liblog SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/liblog.cpp" INCLUDES ${bionic_include}
  CFLAGS -Wall -Wextra -Werror)
apk_bionic_library(liblog UNITS apk_liblog NEEDED libc)

# libnetd_client.so: APK-Runner's own (runtime/bionic/src/libnetd_client.cpp). bionic's libc.so opens
# it at start; without netd its functions leave the socket functions as they are.
apk_bionic_unit(apk_netd_client SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/libnetd_client.cpp" INCLUDES ${bionic_include}
  CFLAGS -Wall -Wextra -Werror)
apk_bionic_library(libnetd_client UNITS apk_netd_client NEEDED libc)
