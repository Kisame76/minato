# The parity benchmark's APKs (ADR-0037, decision 1), built with the Android NDK for arm64-v8a by
# tools/build-bench.sh: libbench-gles.so and libbench-vulkan.so, NativeActivity libraries on the NDK's
# native_app_glue, from main.c and the scene's files with the API each picks.
cmake_minimum_required(VERSION 4.0)
project(MinatoBench LANGUAGES C)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build the bench's APKs with tools/build-bench.sh (Android NDK, arm64-v8a).")
endif()

# tools/build-bench.sh puts the libraries beside the APKs' folder.
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

set(app_glue "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(native_app_glue STATIC "${app_glue}/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${app_glue}")
target_link_libraries(native_app_glue PUBLIC android log)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH bench)
include("${bench}/shaders.cmake")
bench_shaders("${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc" shaders)

add_library(bench-gles SHARED main.c "${bench}/scene.c" "${bench}/gles.c")
target_link_libraries(bench-gles PRIVATE native_app_glue EGL GLESv3)
add_library(bench-vulkan SHARED main.c "${bench}/scene.c" "${bench}/vulkan.c" ${shaders})
target_compile_definitions(bench-vulkan PRIVATE BENCH_APK_VULKAN)
target_include_directories(bench-vulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_link_libraries(bench-vulkan PRIVATE native_app_glue vulkan)
# -ffp-contract=off: no fused multiply-adds, as in the host build (scene.h). The glue's ANativeActivity_onCreate is
# only referenced by Java, so -u keeps it.
foreach(api IN ITEMS gles vulkan)
  target_include_directories(bench-${api} PRIVATE "${bench}")
  target_compile_options(bench-${api} PRIVATE -ffp-contract=off -Wall -Wextra -Wshadow -Wconversion
    -Wsign-conversion -Werror)
  target_link_options(bench-${api} PRIVATE -u ANativeActivity_onCreate)
endforeach()
