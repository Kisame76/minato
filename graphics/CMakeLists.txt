# Minato's graphics (ADR-0029, ADR-0030): the host side of the front libs and their tests. The guest
# front libs themselves are built with the NDK by tools/build-front-libs.sh.
if(APK_HOST_BUILD)
  # ANGLE's checkout from tools/build-angle.sh, with its builds in out/<slice>. Tests that need it are
  # skipped while it is missing.
  set(APK_ANGLE_DIR "${PROJECT_SOURCE_DIR}/build/angle/angle" CACHE PATH "ANGLE's checkout from tools/build-angle.sh")
  add_subdirectory(tests)
endif()
