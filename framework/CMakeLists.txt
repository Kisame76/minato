# The framework's guest code (ADR-0039), built with the NDK's toolchain as the fixtures are
# (tools/build-framework-native.sh): app_process.
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
