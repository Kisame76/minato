# The parity benchmark's scene built for the Mac (ADR-0037, decision 1): the APKs' files (android/, built by
# tools/build-bench.sh) with native/native.m in place of android/main.c, drawing with ANGLE and MoltenVK
# directly, as a static library for graphics.bench. MinatoBench.framework, the same for iOS, comes with step b).
# It needs ANGLE's macOS slice (tools/build-angle.sh), MoltenVK's headers (tools/fetch-moltenvk.sh) and the NDK's
# glslc, which compiles the shaders for both builds; without them graphics.bench is skipped.
set(bench_angle "${APK_ANGLE_DIR}/out/macos/libGLESv2.dylib")
set(bench_glslc "${APK_ANDROID_NDK}/shader-tools/darwin-x86_64/glslc")
foreach(needed IN ITEMS "${bench_angle}" "${APK_ANGLE_DIR}/include/EGL/egl.h"
    "${APK_MOLTENVK_DIR}/include/vulkan/vulkan.h" "${bench_glslc}")
  if(NOT EXISTS "${needed}")
    return()
  endif()
endforeach()

# ANGLE's macOS slice is named ./libGLESv2.dylib, which dyld would look for in the working directory: the library
# links a copy named @rpath/libGLESv2.dylib, signed again, as an imported library, whose folder CMake puts into the
# programs' rpath whether or not the copy is there yet.
set(angle_copy "${CMAKE_CURRENT_BINARY_DIR}/angle/libGLESv2.dylib")
add_custom_command(OUTPUT "${angle_copy}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/angle"
  COMMAND "${CMAKE_COMMAND}" -E copy "${bench_angle}" "${angle_copy}"
  COMMAND install_name_tool -id @rpath/libGLESv2.dylib "${angle_copy}"
  COMMAND codesign --force --sign - "${angle_copy}"
  DEPENDS "${bench_angle}"
  COMMENT "ANGLE's libGLESv2.dylib as @rpath/libGLESv2.dylib for the bench's host build")
add_custom_target(bench-angle DEPENDS "${angle_copy}")
add_library(bench_angle SHARED IMPORTED GLOBAL)
set_target_properties(bench_angle PROPERTIES IMPORTED_LOCATION "${angle_copy}"
  IMPORTED_SONAME "@rpath/libGLESv2.dylib")

include(shaders.cmake)
bench_shaders("${bench_glslc}" shaders)

add_library(minato_bench STATIC scene.c gles.c vulkan.c native/native.m ${shaders})
add_dependencies(minato_bench bench-angle)
target_include_directories(minato_bench PUBLIC native)
# MoltenVK's headers before ANGLE's, which have a vulkan/ folder of their own.
target_include_directories(minato_bench PRIVATE . ../egl ../vulkan "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_include_directories(minato_bench SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include" "${APK_ANGLE_DIR}/include")
target_compile_definitions(minato_bench PRIVATE BENCH_ANGLE_DIRECT)
# -ffp-contract=off: no fused multiply-adds, as in the APKs (scene.h).
target_compile_options(minato_bench PRIVATE -ffp-contract=off $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
target_link_libraries(minato_bench PRIVATE apk_defaults
  PUBLIC bench_angle "-framework Foundation" "-framework Metal" "-framework QuartzCore")
