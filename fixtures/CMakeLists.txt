# NDK test programs for the loader (M2), built for arm64-v8a by tools/build-fixtures.sh.
# Static programs first (M2 step 4), dynamically linked ones later (step 5).
cmake_minimum_required(VERSION 4.0)
project(APKRunnerFixtures LANGUAGES C ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build the fixtures with tools/build-fixtures.sh (Android NDK, arm64-v8a).")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Static PIEs without an interpreter: the loader places them itself. A fixed address, as -static
# produces, is no option, because below 4 GiB nothing can be mapped on iOS (ADR-0012).

# raw-hello: Linux system calls only (svc #0). No libc, no TLS, no relocations.
add_executable(raw-hello static/raw-hello.S)
target_link_options(raw-hello PRIVATE -static-pie -nostdlib)

# hello: printf from the static NDK libc, with its start-up code: TLS set-up (msr TPIDR_EL0),
# stack protector (mrs TPIDR_EL0) and many system calls.
add_executable(hello static/hello.c)
target_link_options(hello PRIVATE -static-pie)
