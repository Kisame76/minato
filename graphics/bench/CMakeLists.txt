# The parity benchmark's scene (ADR-0037, decision 1): the APKs' files (android/, built by tools/build-bench.sh)
# with native/native.m in place of android/main.c, drawing with ANGLE and MoltenVK directly (native/queue_path.c
# puts libvulkan's queue path in front of MoltenVK on request, ADR-0030 V5).
# - On the Mac, a static library for graphics.bench and graphics.recorder.
# - For iOS, MinatoBench.framework for the app's benchmark builds (tools/build-bench.sh --ios): the same files at -O2,
#   as the NDK builds the APKs, linking the slice of ANGLE's framework that the app embeds (build/ANGLE.xcframework
#   from tools/build-angle.sh). MoltenVK is opened with dlopen in both, as graphics/host opens it.
# Both need ANGLE's headers, MoltenVK's (tools/fetch-moltenvk.sh) and the NDK's glslc, which compiles the shaders for
# every build; without them the library is left out and the tests are skipped.
set(bench_glslc "${APK_ANDROID_NDK}/shader-tools/darwin-x86_64/glslc")
if(APK_HOST_BUILD)
  set(bench_angle "${APK_ANGLE_DIR}/out/macos/libGLESv2.dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  string(TOLOWER "${CMAKE_OSX_SYSROOT}" sysroot)
  set(bench_slice ios-arm64)
  if(sysroot MATCHES "simulator")
    set(bench_slice ios-arm64-simulator)
  endif()
  set(bench_angle "${PROJECT_SOURCE_DIR}/build/ANGLE.xcframework/${bench_slice}/libGLESv2.framework/libGLESv2")
else()
  return()
endif()
foreach(needed IN ITEMS "${bench_angle}" "${APK_ANGLE_DIR}/include/EGL/egl.h"
    "${APK_MOLTENVK_DIR}/include/vulkan/vulkan.h" "${bench_glslc}")
  if(NOT EXISTS "${needed}")
    return()
  endif()
endforeach()

include(shaders.cmake)
bench_shaders("${bench_glslc}" shaders)

if(APK_HOST_BUILD)
  # ANGLE's macOS slice is named ./libGLESv2.dylib, which dyld would look for in the working directory: the library
  # links a copy named @rpath/libGLESv2.dylib, signed again, as an imported library, whose folder CMake puts into
  # the programs' rpath whether or not the copy is there yet.
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

  set(bench minato_bench)
  add_library(minato_bench STATIC scene.c gles.c vulkan.c native/native.m native/queue_path.c ${shaders})
  add_dependencies(minato_bench bench-angle)
  target_include_directories(minato_bench PUBLIC native)
  target_link_libraries(minato_bench
    PUBLIC bench_angle "-framework Foundation" "-framework Metal" "-framework QuartzCore")
else()
  # ANGLE's framework is @rpath/libGLESv2.framework/libGLESv2, in the app's Frameworks beside this one: the only
  # rpath, without the build's folders. The -O2 comes after the configuration's -O3 and wins. Not in ALL: the app's
  # builds of these presets (tools/build-native.sh --app) leave it out, tools/build-bench.sh --ios names it.
  set(bench MinatoBench)
  add_library(MinatoBench SHARED EXCLUDE_FROM_ALL
    scene.c gles.c vulkan.c native/native.m native/queue_path.c native/minato_bench.h ${shaders})
  set_target_properties(MinatoBench PROPERTIES FRAMEWORK TRUE PUBLIC_HEADER native/minato_bench.h
    MACOSX_FRAMEWORK_IDENTIFIER io.github.kisame76.minato.bench
    MACOSX_FRAMEWORK_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/native/Info.plist.in"
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path/..")
  target_compile_options(MinatoBench PRIVATE -O2)
  target_include_directories(MinatoBench PRIVATE native)
  target_link_libraries(MinatoBench
    PRIVATE "${bench_angle}" "-framework Foundation" "-framework Metal" "-framework QuartzCore")
endif()
# MoltenVK's headers before ANGLE's, which have a vulkan/ folder of their own.
target_include_directories(${bench} PRIVATE . ../egl ../vulkan "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_include_directories(${bench} SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include" "${APK_ANGLE_DIR}/include")
target_compile_definitions(${bench} PRIVATE BENCH_ANGLE_DIRECT)
# -ffp-contract=off: no fused multiply-adds, as in the APKs (scene.h).
target_compile_options(${bench} PRIVATE -ffp-contract=off $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
target_link_libraries(${bench} PRIVATE apk_defaults)
