# graphics/host (ADR-0030, decision 3): the providers of the front libs' host tables, for the app and the
# tests on the Mac. They need tools/frontgen's lists from tools/build-front-libs.sh and are left out
# while those are missing. tools/build-graphics-host.sh packs this library for the app.
set(APK_GRAPHICS_GEN_DIR "${PROJECT_SOURCE_DIR}/build/graphics/gen" CACHE PATH
  "tools/frontgen's outputs from tools/build-front-libs.sh")
if(EXISTS "${APK_GRAPHICS_GEN_DIR}/vulkan_names.inc" AND EXISTS "${APK_GRAPHICS_GEN_DIR}/gles_names.inc"
    AND EXISTS "${APK_GRAPHICS_GEN_DIR}/egl_names.inc")
  add_library(minato_graphics_host STATIC providers.c)
  target_include_directories(minato_graphics_host PUBLIC include PRIVATE "${APK_GRAPHICS_GEN_DIR}")
  target_link_libraries(minato_graphics_host PUBLIC apkrt PRIVATE apk_defaults)
  # The providers and the functions for missing names run on guest threads' stacks, which
  # AddressSanitizer does not know (as the runtime's syscall layer, runtime/CMakeLists.txt).
  # A source file's options come after apk_defaults' -fsanitize=address, so this one wins.
  if(APK_SANITIZE)
    set_source_files_properties(providers.c PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  # V3's check for the iPhone (minato_graphics.h), in an object file of its own, which only debug
  # builds of the app link. With modules, the object file names Foundation and QuartzCore for the link.
  if(EXISTS "${APK_MOLTENVK_DIR}/include/vulkan/vulkan_metal.h")
    target_sources(minato_graphics_host PRIVATE probe.m)
    target_include_directories(minato_graphics_host SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include")
    set_source_files_properties(probe.m PROPERTIES COMPILE_OPTIONS "-fmodules")
  endif()
endif()
