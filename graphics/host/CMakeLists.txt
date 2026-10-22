# graphics/host (ADR-0029, decisions 14 to 16; ADR-0030, decisions 3, 5 and 13): the providers of the front
# libs' host tables and graphics/host's functions in front of MoltenVK's and ANGLE's, the game view's layer with
# the benchmark's recorder (ADR-0037, decision 2), the windows' buffers for drawing by the CPU (ADR-0043, decision 3)
# and the guest's hardware buffers (ADR-0058 a)), for the app and the tests on the Mac. They need
# tools/frontgen's lists from tools/build-front-libs.sh and MoltenVK's headers from tools/fetch-moltenvk.sh and are
# left out while those are missing. tools/build-graphics-host.sh packs this library for the app.
set(APK_GRAPHICS_GEN_DIR "${PROJECT_SOURCE_DIR}/build/graphics/gen" CACHE PATH
  "tools/frontgen's outputs from tools/build-front-libs.sh")
if(EXISTS "${APK_GRAPHICS_GEN_DIR}/vulkan_names.inc" AND EXISTS "${APK_GRAPHICS_GEN_DIR}/gles_names.inc"
    AND EXISTS "${APK_GRAPHICS_GEN_DIR}/egl_names.inc" AND EXISTS "${APK_MOLTENVK_DIR}/include/vulkan/vulkan_metal.h")
  add_library(minato_graphics_host STATIC providers.c present_time.c vulkan.m angle.m angle_vulkan.m probe.m
    provoking.m drawable_probe.m layer.m window.m hardware_buffer.m image.m video_decoder.m
    video_parameters.c)
  # graphics/vulkan/host_protocol.h: what libvulkan and graphics/host agree on.
  target_include_directories(minato_graphics_host PUBLIC include PRIVATE "${APK_GRAPHICS_GEN_DIR}"
    "${PROJECT_SOURCE_DIR}/graphics/vulkan")
  target_include_directories(minato_graphics_host SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(minato_graphics_host PUBLIC apkrt PRIVATE apk_defaults)
  # The providers, the functions for missing names, the present times and those in front of MoltenVK's run on guest
  # threads' stacks, which AddressSanitizer does not know (as the runtime's syscall layer, runtime/CMakeLists.txt).
  # A source file's options come after apk_defaults' -fsanitize=address, so this one wins.
  set(on_guest_stacks "")
  if(APK_SANITIZE)
    set(on_guest_stacks -fno-sanitize=address)
  endif()
  # With modules, the object files of vulkan.m, angle.m and V3's check for the iPhone (probe.m, which only
  # debug builds of the app link) name their frameworks for the link: Foundation, Metal and QuartzCore.
  set(vulkan_options -fmodules ${on_guest_stacks})
  set_source_files_properties(providers.c present_time.c PROPERTIES COMPILE_OPTIONS "${on_guest_stacks}")
  set_source_files_properties(vulkan.m angle.m angle_vulkan.m PROPERTIES COMPILE_OPTIONS "${vulkan_options}")
  set_source_files_properties(probe.m PROPERTIES COMPILE_OPTIONS "-fmodules")
  # ADR-0044 a)'s probe and ADR-0041 decision 11 c)'s, which probe.m calls, with ARC: debug builds of the app get them
  # with probe.m.
  set_source_files_properties(provoking.m drawable_probe.m PROPERTIES COMPILE_OPTIONS "-fmodules;-fobjc-arc")
  # The game view's layer with the benchmark's recorder (ADR-0037, decision 2), with ARC. Its nextDrawable runs on
  # the thread that draws, a guest thread in Minato.
  set_source_files_properties(layer.m PROPERTIES COMPILE_OPTIONS "-fmodules;-fobjc-arc;${on_guest_stacks}")
  # The windows' buffers for drawing by the CPU (ADR-0043, decision 3), with ARC: their lock and post run on the
  # guest's thread inside its syscall, and name IOSurface for the link as well.
  set_source_files_properties(window.m PROPERTIES COMPILE_OPTIONS "-fmodules;-fobjc-arc;${on_guest_stacks}")
  # The guest's hardware buffers (ADR-0058 a)), IOSurfaces, with ARC: their functions run on the guest's thread inside
  # its syscall.
  set_source_files_properties(hardware_buffer.m PROPERTIES COMPILE_OPTIONS "-fmodules;-fobjc-arc;${on_guest_stacks}")
  # Their EGL images (ADR-0058 b)), with ARC: in front of ANGLE's functions on the guest's thread.
  set_source_files_properties(image.m PROPERTIES COMPILE_OPTIONS "-fmodules;-fobjc-arc;${on_guest_stacks}")
  # The guest's video decoders (ADR-0060 e2b3), VideoToolbox's, with ARC, and their parameter sets' reader: on the
  # guest's thread inside its syscall. graphics.video_decoder_asan runs both with AddressSanitizer.
  set_source_files_properties(video_decoder.m PROPERTIES COMPILE_OPTIONS
    "-fmodules;-fobjc-arc;-fobjc-arc-exceptions;${on_guest_stacks}")
  set_source_files_properties(video_parameters.c PROPERTIES COMPILE_OPTIONS "${on_guest_stacks}")
endif()
