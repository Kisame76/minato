# Minato's graphics (ADR-0029, ADR-0030): the host side of the front libs and their tests. The guest
# front libs themselves are built with the NDK by tools/build-front-libs.sh.
# MoltenVK as released, from tools/fetch-moltenvk.sh (ADR-0030): graphics/host's probe needs its headers,
# and its tests are skipped without it.
set(APK_MOLTENVK_DIR "${PROJECT_SOURCE_DIR}/build/moltenvk" CACHE PATH "MoltenVK from tools/fetch-moltenvk.sh")
# The probe is Objective-C (a CAMetalLayer); enabled here, above graphics/host and the tests that link it.
enable_language(OBJC)
add_subdirectory(host)
if(APK_HOST_BUILD)
  # ANGLE's checkout from tools/build-angle.sh, with its builds in out/<slice>. Tests that need it are
  # skipped while it is missing.
  set(APK_ANGLE_DIR "${PROJECT_SOURCE_DIR}/build/angle/angle" CACHE PATH "ANGLE's checkout from tools/build-angle.sh")
  add_subdirectory(tests)
endif()
