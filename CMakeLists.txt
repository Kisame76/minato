# Native code of Minato: the runtime (runtime/) and the converter (importer/converter/).
# Configure with the presets in CMakePresets.json, usually through tools/build-native.sh (ADR-0016).
# The NDK fixtures are a separate project (fixtures/), because they need the Android toolchain.
cmake_minimum_required(VERSION 4.0)
project(MinatoNative VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(APK_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Tests and host tools run on the Mac only; the iOS builds produce the libraries for the app.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(APK_HOST_BUILD ON)
  enable_testing()
else()
  set(APK_HOST_BUILD OFF)
endif()

# Settings for the project's own targets. Vendored code (AOSP) will not get them.
add_library(apk_defaults INTERFACE)
target_compile_options(apk_defaults INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
if(APK_SANITIZE)
  target_compile_options(apk_defaults INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(apk_defaults INTERFACE -fsanitize=address,undefined)
endif()

if(APK_HOST_BUILD)
  # The NDK fixtures from tools/build-fixtures.sh. Tests that need them are skipped while they are missing.
  set(APK_FIXTURES_DIR "${PROJECT_SOURCE_DIR}/build/fixtures/bin" CACHE PATH "Directory of the built NDK fixtures")
  # The guest libc from tools/build-bionic.sh (ADR-0021); tests that need it are skipped without it.
  set(APK_BIONIC_DIR "${PROJECT_SOURCE_DIR}/build/bionic/lib" CACHE PATH "Directory of the built bionic libraries")
  # The guest's /system from the same script: bionic's time zone file.
  set(APK_GUEST_SYSTEM_DIR "${PROJECT_SOURCE_DIR}/build/bionic/system" CACHE PATH "Directory mounted as the guest's /system")
endif()

add_subdirectory(runtime)
add_subdirectory(importer/converter)
add_subdirectory(graphics)

if(APK_HOST_BUILD)
  # End to end (M2 steps 4 and 5, ADR-0019, ADR-0021): each fixture is converted, signed ad hoc and
  # run by apk-run in a process of its own, the way Android's shell runs it:
  #   cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./<program>
  # Its own libraries lie next to it, the system's in /system/lib64. It must print exactly
  # fixtures/expected/<name>.out, on stderr exactly <name>.err or nothing, and exit with STATUS (0).
  # PROGRAM runs another fixture than <name>; NO_LIBRARY_PATH leaves LD_LIBRARY_PATH out; MEASURES
  # marks a benchmark, whose stdout is not compared.
  # tools/android-reference.sh runs the same on Android, tools/device-suite.sh on the iPhone.
  function(apk_fixture_test name)
    cmake_parse_arguments(PARSE_ARGV 1 FIXTURE "NO_LIBRARY_PATH;MEASURES" "PROGRAM;STATUS;SCUDO_REGIONS" "LIBRARIES;SYSTEM_LIBRARIES")
    if(NOT FIXTURE_PROGRAM)
      set(FIXTURE_PROGRAM ${name})
    endif()
    if(NOT DEFINED FIXTURE_STATUS)
      set(FIXTURE_STATUS 0)
    endif()
    set(expected "${PROJECT_SOURCE_DIR}/fixtures/expected/${name}")
    set(expected_error "")
    if(EXISTS "${expected}.err")
      set(expected_error "-D EXPECTED_ERROR=${expected}.err")
    endif()
    add_test(NAME fixture.${name}
      COMMAND ${CMAKE_COMMAND}
        -D CONVERT=$<TARGET_FILE:apk-convert>
        -D RUN=$<TARGET_FILE:apk-run>
        -D TEST=${name}
        -D FIXTURE=${APK_FIXTURES_DIR}/${FIXTURE_PROGRAM}
        "-D LIBRARIES=${FIXTURE_LIBRARIES}"
        "-D SYSTEM_LIBRARIES=${FIXTURE_SYSTEM_LIBRARIES}"
        -D NO_LIBRARY_PATH=${FIXTURE_NO_LIBRARY_PATH}
        -D MEASURES=${FIXTURE_MEASURES}
        -D SCUDO_REGIONS=${FIXTURE_SCUDO_REGIONS}
        -D WORK=${CMAKE_CURRENT_BINARY_DIR}/fixture_runs
        -D EXPECTED_OUTPUT=${expected}.out
        ${expected_error}
        -D EXPECTED_STATUS=${FIXTURE_STATUS}
        -D SYSTEM=${APK_GUEST_SYSTEM_DIR}
        -P ${PROJECT_SOURCE_DIR}/tools/run-fixture.cmake)
    set_tests_properties(fixture.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endfunction()

  # bionic's libraries, built from AOSP sources (M2 step 5b), as the system's: libc.so, libm.so,
  # libdl.so, ld-android.so, liblog.so, libnetd_client.so, which libc.so opens at start, and libstdc++.so (gap 14);
  # and the NDK libraries Minato writes itself (ADR-0028): libandroid.so, libnativewindow.so and libz.so, and the
  # stubs libcamera2ndk.so and libmediandk.so (ADR-0042 b, gap 15) and libsync.so, libamidi.so, libbinder_ndk.so,
  # libneuralnetworks.so and libOpenMAXAL.so (gap 14).
  set(bionic_libraries libc libc-bigregions libm libdl ld-android liblog libnetd_client libstdc++ libandroid
    libnativewindow libz libcamera2ndk libmediandk libsync libamidi libbinder_ndk libneuralnetworks libOpenMAXAL)
  list(TRANSFORM bionic_libraries REPLACE "(.+)" "${APK_BIONIC_DIR}/\\1.so")

  # The tests of fixtures/tests.txt, which says what each line means.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/fixtures/tests.txt")
  file(STRINGS "${PROJECT_SOURCE_DIR}/fixtures/tests.txt" fixture_tests REGEX "^[^#]")
  foreach(line IN LISTS fixture_tests)
    separate_arguments(fields UNIX_COMMAND "${line}")
    list(POP_FRONT fields name)
    set(arguments "")
    foreach(field IN LISTS fields)
      if(field STREQUAL "system")
        list(APPEND arguments SYSTEM_LIBRARIES ${bionic_libraries})
      elseif(field STREQUAL "no-library-path")
        list(APPEND arguments NO_LIBRARY_PATH)
      elseif(field STREQUAL "runtime-only")
        # Only Android leaves it out.
      elseif(field STREQUAL "measures")
        list(APPEND arguments MEASURES)
      elseif(field MATCHES "^program=(.+)$")
        list(APPEND arguments PROGRAM ${CMAKE_MATCH_1})
      elseif(field MATCHES "^status=(.+)$")
        list(APPEND arguments STATUS ${CMAKE_MATCH_1})
      elseif(field MATCHES "^scudo-regions=(.+)$")
        list(APPEND arguments SCUDO_REGIONS ${CMAKE_MATCH_1})
      elseif(field MATCHES "^libraries=(.+)$")
        string(REPLACE "," ";" own "${CMAKE_MATCH_1}")
        list(TRANSFORM own PREPEND "${APK_FIXTURES_DIR}/")
        list(APPEND arguments LIBRARIES ${own})
      else()
        message(FATAL_ERROR "fixtures/tests.txt: unknown field \"${field}\" of ${name}")
      endif()
    endforeach()
    apk_fixture_test(${name} ${arguments})
  endforeach()

  # Test 2 with Godot (ADR-0042 a and c): the guards of tools/fetch-godot.py, without the network; the APKs of
  # tools/build-godot.sh; the project run headless on the Mac; the twin's pack of its --pack, run the same way; and the
  # twin's framework of its --twin. The last four say "skipped:" without the editor, and share build/godot/ through
  # their lock.
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME godot.fetch COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/fetch-godot.py" --self-test)
  foreach(check export script pack twin)
    add_test(NAME godot.${check} COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/godot-check.py" ${check})
    set_tests_properties(godot.${check} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" RESOURCE_LOCK godot TIMEOUT 600)
  endforeach()
  # The bundle's license rule for the twin: Godot's texts go into a build with MinatoGodot.framework (c) 2).
  add_test(NAME godot.licenses COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/license-check-test.py")
  # The source archive of the parts under the GPL-2.0 with the Classpath Exception and its SOURCE.txt (ADR-0033).
  add_test(NAME release.source_archive
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/source-archive-test.py")
  # The release guards: the guest outputs are fresh against files the builds read, and the guard runs before the
  # release commit (tools/check-guest-fresh.py, tools/release.sh).
  add_test(NAME release.guest_fresh
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/guest-fresh-test.py")
  # The prepared mode of the check builds (MINATO_PREPARED=1): the stamp tool, and that each group of inputs the
  # framework build steps declare changes the digest (tools/prepared-stamp.py).
  add_test(NAME tools.prepared_stamp
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/prepared_stamp_test.py")
endif()
