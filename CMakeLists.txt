# Native code of APK-Runner: the runtime (runtime/) and the converter (importer/converter/).
# Configure with the presets in CMakePresets.json, usually through tools/build-native.sh (ADR-0016).
# The NDK fixtures are a separate project (fixtures/), because they need the Android toolchain.
cmake_minimum_required(VERSION 4.0)
project(APKRunnerNative VERSION 0.1.0 LANGUAGES C CXX)

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

# Settings for the project's own targets. Vendored code (ARTBox, AOSP) will not get them.
add_library(apk_defaults INTERFACE)
target_compile_options(apk_defaults INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
if(APK_SANITIZE)
  target_compile_options(apk_defaults INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(apk_defaults INTERFACE -fsanitize=address,undefined)
endif()

add_subdirectory(runtime)
add_subdirectory(importer/converter)
