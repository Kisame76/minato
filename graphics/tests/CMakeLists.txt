# Tests of ADR-0029, part A: ANGLE's own build, before any front lib. They print "skipped:" while the
# checkout or the built library is missing (tools/build-angle.sh), so CTest stays green for everyone
# else while ANGLE is being built.

# Test 1: a host program without guest code opens the macOS slice and draws with ES 3.0 on Metal.
if(EXISTS "${APK_ANGLE_DIR}/include/EGL/egl.h")
  add_executable(angle-smoke angle_smoke.cpp)
  target_include_directories(angle-smoke SYSTEM PRIVATE "${APK_ANGLE_DIR}/include")
  target_link_libraries(angle-smoke PRIVATE apk_defaults)
  add_test(NAME graphics.angle_smoke COMMAND angle-smoke "${APK_ANGLE_DIR}/out/macos/libGLESv2.dylib")
else()
  add_test(NAME graphics.angle_smoke
    COMMAND ${CMAKE_COMMAND} -E echo "skipped: no ANGLE checkout in ${APK_ANGLE_DIR}, see tools/build-angle.sh")
endif()
set_tests_properties(graphics.angle_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

# Tests 2 and 4 need Core Animation and Metal objects: Objective-C++ with ARC.
# Test 2: EGL_ANGLE_window_fixed_size on Metal (patch 0001). Test 4: the commit gate (patch 0003).
enable_language(OBJCXX)
foreach(test IN ITEMS fixed_size commit_gate)
  string(REPLACE "_" "-" program "angle-${test}")
  if(EXISTS "${APK_ANGLE_DIR}/include/EGL/egl.h")
    add_executable(${program} angle_${test}.mm)
    target_include_directories(${program} SYSTEM PRIVATE "${APK_ANGLE_DIR}/include")
    target_compile_options(${program} PRIVATE -fobjc-arc)
    target_link_libraries(${program} PRIVATE apk_defaults
      "-framework Foundation" "-framework QuartzCore" "-framework Metal")
    add_test(NAME graphics.angle_${test} COMMAND ${program} "${APK_ANGLE_DIR}/out/macos/libGLESv2.dylib")
  else()
    add_test(NAME graphics.angle_${test}
      COMMAND ${CMAKE_COMMAND} -E echo "skipped: no ANGLE checkout in ${APK_ANGLE_DIR}, see tools/build-angle.sh")
  endif()
  set_tests_properties(graphics.angle_${test} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 60)
endforeach()

# Test 3: every GLES and EGL function of the NDK's stubs is exported by the three slices or on the stub list.
if(DEFINED ENV{ANDROID_NDK})
  set(ndk_default "$ENV{ANDROID_NDK}")
else()
  set(ndk_default "$ENV{HOME}/Library/Android/sdk/ndk/30.0.16248370")
endif()
set(APK_ANDROID_NDK "${ndk_default}" CACHE PATH "Android NDK whose stubs name the functions of Android's libraries")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME graphics.angle_exports
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/angle_exports.py"
    --angle "${APK_ANGLE_DIR}" --ndk "${APK_ANDROID_NDK}"
    --stubs "${PROJECT_SOURCE_DIR}/third_party/angle/stubs.txt")
set_tests_properties(graphics.angle_exports PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
