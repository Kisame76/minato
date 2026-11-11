# ART as guest code (ADR-0033 a): ART android-15.0.0_r1 and its libraries (tools/fetch-sources.py --set art), built
# with the NDK's clang as Android arm64 platform code, in ART's release and debug flavor, and dalvikvm. Which sources
# and flags come from the projects' own Android.bp files (tools/art-plan.py), with Minato's modules of
# runtime/art/modules.bp, the files of runtime/art/overlay in place of AOSP's and the patches of runtime/art/patches,
# each with the reason it exists. ART links the guest system's libraries (tools/build-bionic.sh) and the NDK's
# libc++_shared.so, which goes into its folder (ADR-0033 decision 2). Built by tools/build-art.sh; the libraries go
# to <build>/lib, dalvikvm to <build>/bin.
cmake_minimum_required(VERSION 4.0)
project(MinatoArt LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build ART with tools/build-art.sh (Android NDK, arm64-v8a).")
endif()

set(APK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(APK_SOURCES "${APK_ROOT}/build/sources" CACHE PATH "The fetched AOSP sources (tools/fetch-sources.py --set art)")
set(APK_BIONIC_LIB "${APK_ROOT}/build/bionic/lib" CACHE PATH "The guest system's libraries (tools/build-bionic.sh)")
set(APK_ART_GENERATED "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${APK_ART_GENERATED}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/archive")

# From the NDK: libc++ (its headers and libc++_shared.so), the compiler builtins and the CRT objects.
set(ndk_libcxx_include "${CMAKE_SYSROOT}/usr/include/c++/v1")
set(ndk_libs "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android")
set(ndk_libcxx_shared "${ndk_libs}/libc++_shared.so")
file(GLOB ndk_clang_libs "${ANDROID_TOOLCHAIN_ROOT}/lib/clang/*/lib/linux")
set(ndk_builtins "${ndk_clang_libs}/libclang_rt.builtins-aarch64-android.a")
set(ndk_crt "${ndk_libs}/${ANDROID_PLATFORM_LEVEL}")
foreach(file IN ITEMS "${ndk_libcxx_include}/vector" "${ndk_libcxx_shared}" "${ndk_builtins}" "${ndk_crt}/crtbegin_so.o"
    "${ndk_crt}/crtend_so.o" "${ndk_crt}/crtbegin_dynamic.o" "${ndk_crt}/crtend_android.o")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "missing from the NDK: ${file}")
  endif()
endforeach()

# The guest system's libraries that ART links, from tools/build-bionic.sh.
foreach(library IN ITEMS libc libm libdl libdl_android ld-android liblog libz)
  if(NOT EXISTS "${APK_BIONIC_LIB}/${library}.so")
    message(FATAL_ERROR "${APK_BIONIC_LIB}/${library}.so is missing: run tools/build-bionic.sh first")
  endif()
  add_library(art_lib_${library} SHARED IMPORTED)
  set_target_properties(art_lib_${library} PROPERTIES IMPORTED_LOCATION "${APK_BIONIC_LIB}/${library}.so")
endforeach()

# libc++_shared.so next to ART's libraries, which link it.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/lib/libc++_shared.so"
  COMMAND ${CMAKE_COMMAND} -E copy "${ndk_libcxx_shared}" "${CMAKE_BINARY_DIR}/lib/libc++_shared.so"
  DEPENDS "${ndk_libcxx_shared}" VERBATIM)
add_custom_target(art_libcxx_shared ALL DEPENDS "${CMAKE_BINARY_DIR}/lib/libc++_shared.so")

# Soong's global flags for Android arm64 platform code, as for bionic (runtime/bionic/CMakeLists.txt), and more:
# 16 KiB pages leave bionic's PAGE_SIZE undefined, so that no code assumes 4 KiB (ADR-0007, Soong's
# NoBionicPageSizeMacro); Soong's global C++ flags; and fmt's format strings checked at run time, since fmt 10.2's
# compile-time check fails under the NDK's clang, which no longer folds two equal string literals into one object.
# -Wno-error comes last: the projects' -Werror was meant for the clang of their time. The language standards come
# from the plan, after the module's cflags, where Soong puts them (ADR-0043 a2): one of ours before them would win
# over the plan's when they are equal, since CMake drops a repeated option.
set(art_target "--target=aarch64-linux-android10000")
set(art_flags ${art_target} -nostdlibinc -fPIC -ffunction-sections -fdata-sections -fno-short-enums -funwind-tables
  -fstack-protector-strong -fno-exceptions -fno-strict-aliasing -ffp-contract=off -ftrivial-auto-var-init=zero
  -march=armv8-a -O2 -DANDROID -DNDEBUG -D__BIONIC_NO_PAGE_SIZE_MACRO)
set(art_cppflags -fno-rtti -fvisibility-inlines-hidden -Wsign-promo -Wno-gnu-include-next -DFMT_CONSTEVAL=)
set(art_link_flags ${art_target} -nostdlib -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-z,noexecstack
  -Wl,-z,relro -Wl,-z,now -Wl,--build-id=md5 -Wl,--hash-style=gnu)

# One module's compile unit. libc++'s headers come before bionic's (SYSTEM_INCLUDES), whose headers they include
# with #include_next.
function(art_unit name)
  cmake_parse_arguments(PARSE_ARGV 1 unit "" ""
    "SOURCES;GENERATED_HEADERS;CFLAGS;CONLYFLAGS;CPPFLAGS;ASFLAGS;INCLUDES;SYSTEM_INCLUDES")
  add_library(${name} OBJECT ${unit_SOURCES} ${unit_GENERATED_HEADERS})
  if(unit_GENERATED_HEADERS)
    set_source_files_properties(${unit_GENERATED_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()
  target_include_directories(${name} PRIVATE ${unit_INCLUDES})
  target_include_directories(${name} SYSTEM BEFORE PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${ndk_libcxx_include}>")
  target_include_directories(${name} SYSTEM PRIVATE ${unit_SYSTEM_INCLUDES})
  target_compile_options(${name} PRIVATE ${art_flags}
    "$<$<COMPILE_LANGUAGE:C>:${unit_CFLAGS};${unit_CONLYFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${art_cppflags};${unit_CFLAGS};${unit_CPPFLAGS}>"
    "$<$<COMPILE_LANGUAGE:ASM>:-D__ASSEMBLY__;${unit_ASFLAGS}>"
    -Wno-error)
endfunction()

# A shared library or a program of UNITS, the static libraries of ARCHIVES, the libraries of NEEDED (the set's, or
# the guest system's), and libc++_shared.so where it is used.
function(art_link target)
  cmake_parse_arguments(PARSE_ARGV 1 link "" "STL;VERSION_SCRIPT" "ARCHIVES;NEEDED;LDFLAGS")
  target_link_options(${target} PRIVATE ${art_link_flags} ${link_LDFLAGS})
  if(link_VERSION_SCRIPT)
    target_link_options(${target} PRIVATE "-Wl,--version-script=${link_VERSION_SCRIPT}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${link_VERSION_SCRIPT}")
  endif()
  foreach(unit IN LISTS link_ARCHIVES)
    if(NOT TARGET ${unit}_archive)
      add_library(${unit}_archive STATIC "$<TARGET_OBJECTS:${unit}>")
    endif()
    target_link_libraries(${target} PRIVATE ${unit}_archive)
  endforeach()
  foreach(needed IN LISTS link_NEEDED)
    target_link_libraries(${target} PRIVATE art_lib_${needed})
  endforeach()
  if(link_STL MATCHES "static")
    target_link_libraries(${target} PRIVATE "-Wl,--exclude-libs,ALL" "${ndk_libs}/libc++_static.a"
      "${ndk_libs}/libc++abi.a")
  elseif(NOT link_STL STREQUAL "none")
    add_dependencies(${target} art_libcxx_shared)
    target_link_libraries(${target} PRIVATE "-Wl,--push-state,--as-needed" "${ndk_libcxx_shared}" "-Wl,--pop-state")
  endif()
  target_link_libraries(${target} PRIVATE "${ndk_builtins}")
endfunction()

function(art_library name)
  cmake_parse_arguments(PARSE_ARGV 1 library "" "STL;VERSION_SCRIPT;NOCRT" "UNITS;ARCHIVES;NEEDED;LDFLAGS")
  set(objects)
  foreach(unit IN LISTS library_UNITS)
    list(APPEND objects "$<TARGET_OBJECTS:${unit}>")
  endforeach()
  add_library(art_lib_${name} SHARED ${objects})
  set_target_properties(art_lib_${name} PROPERTIES OUTPUT_NAME ${name} PREFIX "" SUFFIX ".so" LINKER_LANGUAGE C)
  if(NOT library_NOCRT)
    target_sources(art_lib_${name} PRIVATE "${ndk_crt}/crtbegin_so.o")
  endif()
  art_link(art_lib_${name} STL "${library_STL}" VERSION_SCRIPT "${library_VERSION_SCRIPT}"
    ARCHIVES ${library_ARCHIVES} NEEDED ${library_NEEDED} LDFLAGS ${library_LDFLAGS})
  if(NOT library_NOCRT)
    target_link_libraries(art_lib_${name} PRIVATE "${ndk_crt}/crtend_so.o")
  endif()
endfunction()

# A program, a PIE with linker64 as its interpreter, like the NDK's.
function(art_program name)
  cmake_parse_arguments(PARSE_ARGV 1 program "" "STL;VERSION_SCRIPT;NOCRT" "UNITS;ARCHIVES;NEEDED;LDFLAGS")
  set(objects)
  foreach(unit IN LISTS program_UNITS)
    list(APPEND objects "$<TARGET_OBJECTS:${unit}>")
  endforeach()
  add_executable(art_program_${name} "${ndk_crt}/crtbegin_dynamic.o" ${objects})
  set_target_properties(art_program_${name} PROPERTIES OUTPUT_NAME ${name} LINKER_LANGUAGE C)
  target_link_options(art_program_${name} PRIVATE -pie -Wl,-dynamic-linker,/system/bin/linker64)
  art_link(art_program_${name} STL "${program_STL}" VERSION_SCRIPT "${program_VERSION_SCRIPT}"
    ARCHIVES ${program_ARCHIVES} NEEDED ${program_NEEDED} LDFLAGS ${program_LDFLAGS})
  target_link_libraries(art_program_${name} PRIVATE "${ndk_crt}/crtend_android.o")
endfunction()

# A generator's command, run in the tree's root (the plan's APK_ART_TREE) as Soong runs it.
function(art_generate)
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
    WORKING_DIRECTORY "${APK_ART_TREE}" VERBATIM)
endfunction()

# A generator's tool that runs on the Mac, built with the Mac's compiler.
function(art_host_tool output)
  cmake_parse_arguments(PARSE_ARGV 1 tool "" "" "SOURCES;INCLUDES;DEPENDS")
  list(TRANSFORM tool_INCLUDES PREPEND "-I")
  get_filename_component(directory "${output}" DIRECTORY)
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
    COMMAND xcrun --sdk macosx clang -w ${tool_INCLUDES} -o "${output}" ${tool_SOURCES}
    DEPENDS ${tool_SOURCES} ${tool_DEPENDS} VERBATIM)
endfunction()

# A cc_object compiled to assembly text (-S), which a generator reads: as an object library's C++ compile would
# be, with the unit's flags.
function(art_object_text output)
  cmake_parse_arguments(PARSE_ARGV 1 object "" "" "SOURCES;CFLAGS;CPPFLAGS;INCLUDES;SYSTEM_INCLUDES")
  set(flags ${object_CFLAGS} ${object_CPPFLAGS})
  list(TRANSFORM flags REPLACE "^SHELL:" "")
  list(TRANSFORM object_INCLUDES PREPEND "-I")
  list(TRANSFORM object_SYSTEM_INCLUDES PREPEND "-isystem")
  get_filename_component(directory "${output}" DIRECTORY)
  # The generated depfile makes Ninja re-run this when a header asm_defines.cc includes changes (e.g. thread.h,
  # thread.def), not only when the .cc itself does; otherwise an incremental build keeps a stale asm_defines.h.
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
    COMMAND "${CMAKE_CXX_COMPILER}" ${art_flags} ${art_cppflags} -isystem "${ndk_libcxx_include}" ${flags}
      ${object_INCLUDES} ${object_SYSTEM_INCLUDES} -Wno-error -MD -MF "${output}.d" -S -o "${output}" ${object_SOURCES}
    DEPENDS ${object_SOURCES}
    DEPFILE "${output}.d"
    VERBATIM)
endfunction()

# The boot jars into <build>/javalib (runtime/art/boot_jars.py), with the JDK and D8 that tools/fetch-sources.py
# --set art fetches.
file(GLOB APK_JDK_HOME LIST_DIRECTORIES true "${APK_ROOT}/build/tools/jdk-17/*/Contents/Home")
set(APK_R8_JAR "${APK_SOURCES}/r8/r8.jar")
if(NOT EXISTS "${APK_JDK_HOME}/bin/javac" OR NOT EXISTS "${APK_R8_JAR}")
  message(FATAL_ERROR "No JDK 17 in build/tools/jdk-17 or no D8 in ${APK_R8_JAR}: run tools/fetch-sources.py --set art")
endif()
# Android 15's hidden API lists, which the boot jars' dex files carry as flags (tools/hiddenapi-encode.py): fetched once
# after the user's OK, outside the repository. Without the file the build says so in one line and writes no flags.
if(DEFINED ENV{MINATO_HIDDENAPI_FLAGS})
  set(APK_HIDDENAPI_FLAGS "$ENV{MINATO_HIDDENAPI_FLAGS}")
else()
  set(APK_HIDDENAPI_FLAGS "$ENV{HOME}/.android/minato-hiddenapi/hiddenapi-flags.csv")
endif()
set(apk_hiddenapi_inputs "${APK_ROOT}/tools/hiddenapi-encode.py" "${APK_ROOT}/third_party/sources.json")
if(EXISTS "${APK_HIDDENAPI_FLAGS}")
  list(APPEND apk_hiddenapi_inputs "${APK_HIDDENAPI_FLAGS}")
endif()
function(art_boot_jars description)
  cmake_parse_arguments(PARSE_ARGV 1 boot "" "" "JARS;DEPENDS")
  set(outputs)
  foreach(jar IN LISTS boot_JARS)
    list(APPEND outputs "${CMAKE_BINARY_DIR}/javalib/${jar}.jar")
  endforeach()
  add_custom_command(OUTPUT ${outputs}
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/boot_jars.py" "${description}" "${APK_JDK_HOME}"
      "${APK_R8_JAR}" "${CMAKE_BINARY_DIR}/javalib" "${CMAKE_BINARY_DIR}/java"
    COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/hiddenapi-encode.py" --csv "${APK_HIDDENAPI_FLAGS}"
      --pin "${APK_ROOT}/third_party/sources.json" ${outputs}
    DEPENDS ${boot_DEPENDS} "${CMAKE_CURRENT_SOURCE_DIR}/boot_jars.py" "${description}" ${apk_hiddenapi_inputs}
    VERBATIM)
  add_custom_target(art_boot_jars ALL DEPENDS ${outputs})
endfunction()

execute_process(COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/art-plan.py" "${APK_SOURCES}" "${CMAKE_BINARY_DIR}/plan.cmake"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "tools/art-plan.py failed")
endif()
# The plan also lays out the tree, with the overlay's files and the patches: a new file or a changed patch plans
# again.
file(GLOB_RECURSE art_changes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overlay/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APK_ROOT}/tools/art-plan.py"
  "${APK_ROOT}/tools/soong_plan.py" "${APK_ROOT}/tools/blueprint.py" "${CMAKE_CURRENT_SOURCE_DIR}/modules.bp"
  ${art_changes})
include("${CMAKE_BINARY_DIR}/plan.cmake")

# The Java test programs of fixtures/java, one jar each in <build>/test (test_jars.py), compiled against the classes
# of the boot jars that boot_jars.py leaves in <build>/java. Only folders with a Main.java of their own hold one, as
# dalvikvm runs it; App's test apps there compile against the framework (tools/build-java-test-app.sh).
file(GLOB art_test_programs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${APK_ROOT}/fixtures/java/*")
foreach(program IN LISTS art_test_programs)
  if(NOT IS_DIRECTORY "${program}" OR NOT EXISTS "${program}/Main.java")
    continue()
  endif()
  get_filename_component(name "${program}" NAME)
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${program}/*.java")
  set(jar "${CMAKE_BINARY_DIR}/test/${name}.jar")
  add_custom_command(OUTPUT "${jar}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_jars.py" "${APK_JDK_HOME}" "${APK_R8_JAR}"
      "${CMAKE_BINARY_DIR}/java" "${jar}" ${sources}
    DEPENDS ${sources} "${CMAKE_CURRENT_SOURCE_DIR}/test_jars.py" art_boot_jars VERBATIM)
  add_custom_target(art_test_${name} ALL DEPENDS "${jar}")
endforeach()
