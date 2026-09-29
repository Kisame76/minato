# Minato's graphics (ADR-0029, ADR-0030): the host side of the front libs and their tests. The guest
# front libs themselves are built with the NDK by tools/build-front-libs.sh.
# MoltenVK as released, from tools/fetch-moltenvk.sh (ADR-0030): graphics/host's probe needs its headers,
# and its tests are skipped without it.
set(APK_MOLTENVK_DIR "${PROJECT_SOURCE_DIR}/build/moltenvk" CACHE PATH "MoltenVK from tools/fetch-moltenvk.sh")
# The probe is Objective-C (a CAMetalLayer); enabled here, above graphics/host and the tests that link it.
enable_language(OBJC)
add_subdirectory(host)
# ANGLE's checkout from tools/build-angle.sh, with its builds in out/<slice>. Tests that need it are skipped
# while it is missing.
set(APK_ANGLE_DIR "${PROJECT_SOURCE_DIR}/build/angle/angle" CACHE PATH "ANGLE's checkout from tools/build-angle.sh")
if(DEFINED ENV{ANDROID_NDK})
  set(ndk_default "$ENV{ANDROID_NDK}")
else()
  set(ndk_default "$ENV{HOME}/Library/Android/sdk/ndk/30.0.16248370")
endif()
set(APK_ANDROID_NDK "${ndk_default}" CACHE PATH
  "Android NDK whose stubs name the functions of Android's libraries and whose glslc compiles the bench's shaders")
# The parity benchmark's scene (ADR-0037): for the Mac, which graphics.bench runs, and as MinatoBench.framework for
# iOS (tools/build-bench.sh --ios).
add_subdirectory(bench)
if(APK_HOST_BUILD)
  add_subdirectory(tests)
endif()
