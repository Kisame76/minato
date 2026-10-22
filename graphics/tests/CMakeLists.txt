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

# Tests 2, 4, 5 and rgb_configs need Core Animation and Metal objects: Objective-C++ with ARC.
# Test 2: EGL_ANGLE_window_fixed_size on Metal (patch 0001). Test 4: the commit gate (patch 0003). Test 5:
# EGL_ANDROID_presentation_time on Metal (patch 0005, ADR-0041 decision 11). rgb_configs: RGB888 configs
# without alpha on Metal, GLSurfaceView's default (patch 0006, ADR-0043 c4). external_images: samplerExternalOES and
# GL_TEXTURE_EXTERNAL_OES on Metal (patch 0010, ADR-0058 b)).
enable_language(OBJCXX)
foreach(test IN ITEMS fixed_size commit_gate presentation_time rgb_configs external_images)
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

# ADR-0030, test 5: MoltenVK as released draws on the Mac, opened with dlopen like graphics/host will.
if(EXISTS "${APK_MOLTENVK_DIR}/include/vulkan/vulkan.h")
  add_executable(moltenvk-smoke moltenvk_smoke.cpp)
  target_include_directories(moltenvk-smoke SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(moltenvk-smoke PRIVATE apk_defaults)
  add_test(NAME graphics.moltenvk_smoke COMMAND moltenvk-smoke "${APK_MOLTENVK_DIR}/macos/libMoltenVK.dylib")
else()
  add_test(NAME graphics.moltenvk_smoke
    COMMAND ${CMAKE_COMMAND} -E echo "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/fetch-moltenvk.sh")
endif()
set_tests_properties(graphics.moltenvk_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 60)

# ADR-0044 b), test 4: our MoltenVK keeps the private Metal API off and turns it on for one instance through
# VK_EXT_layer_settings; MoltenVK as released fails it.
if(EXISTS "${APK_MOLTENVK_DIR}/include/vulkan/vulkan.h")
  add_executable(moltenvk-private-api moltenvk_private_api.cpp)
  target_include_directories(moltenvk-private-api SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(moltenvk-private-api PRIVATE apk_defaults)
  add_test(NAME graphics.moltenvk_private_api
    COMMAND moltenvk-private-api "${APK_MOLTENVK_DIR}/macos/libMoltenVK.dylib")
else()
  add_test(NAME graphics.moltenvk_private_api
    COMMAND ${CMAKE_COMMAND} -E echo "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/build-moltenvk.sh")
endif()
set_tests_properties(graphics.moltenvk_private_api PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 60)

# Test 3: every GLES and EGL function of the NDK's stubs (APK_ANDROID_NDK, graphics/CMakeLists.txt) is exported
# by the three slices or on the stub list.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME graphics.angle_exports
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/angle_exports.py"
    --angle "${APK_ANGLE_DIR}" --ndk "${APK_ANDROID_NDK}"
    --stubs "${PROJECT_SOURCE_DIR}/third_party/angle/stubs.txt")
set_tests_properties(graphics.angle_exports PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

# ADR-0030, test 1: the generator in tools/frontgen/, against the NDK's C headers read by ndk_headers.py.
add_test(NAME graphics.frontgen
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/frontgen_test.py"
    --repo "${PROJECT_SOURCE_DIR}" --vk-xml "${APK_MOLTENVK_DIR}/vk.xml" --angle "${APK_ANGLE_DIR}"
    --ndk "${APK_ANDROID_NDK}" --work "${CMAKE_CURRENT_BINARY_DIR}/frontgen")
set_tests_properties(graphics.frontgen PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

# ADR-0037 b): tools/bench-summary.py on made-up run folders: the windows, nearest rank, validity with the pair rule,
# the verdict and the noise rule.
add_test(NAME graphics.bench_summary
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/bench_summary_test.py"
    --tool "${PROJECT_SOURCE_DIR}/tools/bench-summary.py" --scratch "${CMAKE_CURRENT_BINARY_DIR}/bench_summary")
set_tests_properties(graphics.bench_summary PROPERTIES TIMEOUT 300)

# ADR-0041 test 3: tools/present-check.py on made-up recordings, decision 11's tolerance at its limits and inputs it
# must refuse.
add_test(NAME graphics.present_check
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/present_check_test.py"
    --tool "${PROJECT_SOURCE_DIR}/tools/present-check.py" --scratch "${CMAKE_CURRENT_BINARY_DIR}/present_check")
set_tests_properties(graphics.present_check PROPERTIES TIMEOUT 120)

# ADR-0043 test 2: graphics/tests/canvas_compare.py, which compares canvas-test's pixels with Android's
# (tools/canvas-check.sh), at both limits of its tolerance.
add_test(NAME graphics.canvas_compare
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/canvas_compare_test.py")
set_tests_properties(graphics.canvas_compare PROPERTIES TIMEOUT 120)

# ADR-0043 a1: tools/hwui-plan.py plans hwui's host flavor on the tag's sources, tools/soong_plan.py's planner on a
# made-up set, its host side of __ANDROID__ through unifdef on made-up sources, and tools/sysprop.py's accessors.
add_test(NAME graphics.hwui_plan
  COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/hwui_plan_test.py"
    --scratch "${CMAKE_CURRENT_BINARY_DIR}/hwui_plan")
set_tests_properties(graphics.hwui_plan PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 300)
add_test(NAME graphics.soong_plan COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/soong_plan_test.py")
# make_tree keeps a real copy, newer than the build, of a file whose patch, host side or overlay file goes (Runtime).
add_test(NAME graphics.make_tree COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/make_tree_test.py")
add_test(NAME graphics.host_side COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/host_side_test.py")
set_tests_properties(graphics.host_side PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
add_test(NAME graphics.sysprop COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/sysprop_test.py")
set_tests_properties(graphics.sysprop PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)
# ADR-0043 a2: every C and C++ compile of hwui's and ART's configured builds takes the standard that its plan names
# after the module's cflags, where Soong puts it.
add_test(NAME graphics.plan_standards
  COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/plan_standards_check.py" "${APK_HWUI_DIR}"
    "${APK_ART_DIR}")
set_tests_properties(graphics.plan_standards PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
# ADR-0043 a1: libhwui.so through our loader. The fixture hwui-load opens it as /system/bin/hwui-load, with hwui's
# set and the platform's copies of ART's build in /system/lib64 and ART's folder for ICU and libnativehelper, and
# calls init_android_graphics (hwui_load_run.py).
add_test(NAME graphics.hwui_load
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/hwui_load_run.py" "${APK_FIXTURES_DIR}"
    "${APK_HWUI_DIR}/lib" "${APK_ART_DIR}/lib" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
    $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/hwui_load_runs"
    "${PROJECT_SOURCE_DIR}/fixtures/expected/hwui-load.out")
set_tests_properties(graphics.hwui_load PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 600)

# ADR-0043 test 2's classes: framework.jar has every class and native that libhwui's 42 registrations name
# (hwui_jni_check.py), with hwui's tree from tools/build-hwui.sh and framework.jar's classes from
# tools/build-framework.sh, which a clean copy can point elsewhere with APK_FRAMEWORK_DIR.
set(APK_FRAMEWORK_DIR "${PROJECT_SOURCE_DIR}/build/framework" CACHE PATH "framework.jar's build")
add_test(NAME graphics.hwui_jni
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/hwui_jni_check.py"
    "${APK_HWUI_DIR}/tree/frameworks/base/libs/hwui" "${APK_FRAMEWORK_DIR}/classes.jar"
    "${APK_ART_DIR}/java/all-classes.jar")
set_tests_properties(graphics.hwui_jni PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
# ADR-0043 test 2's fonts: the guest's font lists name exactly the files of its /system/fonts, with decision 5's
# families, as FontListParser reads them (fonts_check.py); graphics/fonts/fonts.cmake puts both into bionic's build.
add_test(NAME graphics.fonts
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/fonts_check.py" "${APK_GUEST_SYSTEM_DIR}")
set_tests_properties(graphics.fonts PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

# ADR-0037 b): graphics/host's sources compile for the iPhone and the Simulator, whose SDK has less Metal than the
# device's: this build's compile commands of them, again with -fsyntax-only against both iOS SDKs. The app gets them
# from tools/build-graphics-host.sh, which CTest does not run.
if(TARGET minato_graphics_host)
  add_test(NAME graphics.host_ios
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/host_ios_test.py"
      --commands "${CMAKE_BINARY_DIR}/compile_commands.json" --sources "${PROJECT_SOURCE_DIR}/graphics/host"
      --presets "${PROJECT_SOURCE_DIR}/CMakePresets.json")
else()
  add_test(NAME graphics.host_ios
    COMMAND ${CMAKE_COMMAND} -E echo "skipped: graphics/host is left out, see graphics/host/CMakeLists.txt")
endif()
set_tests_properties(graphics.host_ios PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

# ADR-0030, test 2: thunk-test, a guest program from tools/build-front-libs.sh, calls every stub with
# garbage; it runs as the fixtures do (tools/run-fixture.cmake), with bionic's libraries as the system's,
# and prints "skipped:" while it is not built.
set(APK_GRAPHICS_GUEST_DIR "${PROJECT_SOURCE_DIR}/build/graphics/guest/bin" CACHE PATH
  "The guest programs from tools/build-front-libs.sh")
set(thunk_system_libraries libc libm libdl ld-android liblog libnetd_client)
list(TRANSFORM thunk_system_libraries REPLACE "(.+)" "${APK_BIONIC_DIR}/\\1.so")
add_test(NAME graphics.thunk
  COMMAND ${CMAKE_COMMAND}
    -D CONVERT=$<TARGET_FILE:apk-convert>
    -D RUN=$<TARGET_FILE:apk-run>
    -D TEST=thunk-test
    -D FIXTURE=${APK_GRAPHICS_GUEST_DIR}/thunk-test
    "-D SYSTEM_LIBRARIES=${thunk_system_libraries}"
    -D WORK=${CMAKE_CURRENT_BINARY_DIR}/thunk_runs
    -D EXPECTED_OUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/thunk_test.out
    -D SYSTEM=${APK_GUEST_SYSTEM_DIR}
    -P ${PROJECT_SOURCE_DIR}/tools/run-fixture.cmake)
set_tests_properties(graphics.thunk PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

# ADR-0030, test 4: bind-test, a guest program from tools/build-front-libs.sh, has its tables bound by the
# providers of graphics/host, which graphics-run (apk-run with them) sets from MINATO_MOLTENVK and
# MINATO_ANGLE. Three runs, each with its own expected output: with ANGLE and MoltenVK, with fake-driver
# (one function per lib) as both, and without providers.
if(TARGET minato_graphics_host)
  # With test 6's Metal hook (metal_hook.m), window (window_host.m) and session layers (session_layers.m), which do
  # nothing unless MINATO_METAL_HOOK, MINATO_WINDOW and MINATO_SESSION_LAYERS are set.
  add_executable(graphics-run "${PROJECT_SOURCE_DIR}/runtime/tools/apk_run.cpp" graphics_run.c metal_hook.m
    window_host.m session_layers.m test_control.m)
  # apk_run_layers.h, the hooks of apk-run's --lifecycle that session_layers.m defines (ADR-0043 c4 (c), ADR-0041
  # gap 4 (c)).
  target_include_directories(graphics-run PRIVATE "${PROJECT_SOURCE_DIR}/runtime/tools")
  target_link_libraries(graphics-run PRIVATE minato_graphics_host apkrt apk_defaults
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")
  # ADR-0041 gap 4 (c): the session layers through apk_run_layers.h's hooks without a guest, in WindowOrderTests'
  # cases (session_layers_test.m).
  add_executable(graphics-session-layers-test session_layers_test.m session_layers.m)
  target_include_directories(graphics-session-layers-test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/tools")
  target_link_libraries(graphics-session-layers-test PRIVATE minato_graphics_host apkrt apk_defaults
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")
  add_test(NAME graphics.session_layers COMMAND graphics-session-layers-test)
  set_tests_properties(graphics.session_layers PROPERTIES TIMEOUT 60)
  # ADR-0058 a): graphics/host's hardware buffers (hardware_buffer.m), the IOSurfaces of the guest's AHardwareBuffers,
  # through their functions without the runtime.
  add_executable(graphics-hardware-buffers-test hardware_buffers_test.m)
  target_link_libraries(graphics-hardware-buffers-test PRIVATE minato_graphics_host apkrt apk_defaults
    "-framework CoreVideo" "-framework Foundation" "-framework IOSurface" "-framework Metal")
  set_source_files_properties(hardware_buffers_test.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  add_test(NAME graphics.hardware_buffers COMMAND graphics-hardware-buffers-test)
  set_tests_properties(graphics.hardware_buffers PROPERTIES TIMEOUT 60)
  # ADR-0060 e2b3: graphics/host's video decoders (video_decoder.m) on media-codec-test's streams, each frame against
  # Android 15's (fixtures/expected/media-codec-test.logcat).
  add_executable(graphics-video-decoder-test video_decoder_test.m)
  target_link_libraries(graphics-video-decoder-test PRIVATE minato_graphics_host apkrt apk_defaults z
    "-framework Foundation")
  set_source_files_properties(video_decoder_test.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  add_test(NAME graphics.video_decoder COMMAND graphics-video-decoder-test
    "${PROJECT_SOURCE_DIR}/fixtures/apps/media-codec-test/assets"
    "${PROJECT_SOURCE_DIR}/fixtures/expected/media-codec-test.logcat")
  set_tests_properties(graphics.video_decoder PROPERTIES TIMEOUT 120)
  # The reorder depth from a stream's SPS (graphics/host/video_parameters.c), against hostile bytes.
  add_executable(graphics-video-parameters-test video_parameters_test.c ../host/video_parameters.c)
  target_include_directories(graphics-video-parameters-test PRIVATE ../host)
  target_link_libraries(graphics-video-parameters-test PRIVATE apk_defaults)
  add_test(NAME graphics.video_parameters COMMAND graphics-video-parameters-test)
  set_tests_properties(graphics.video_parameters PROPERTIES TIMEOUT 120)
  add_library(fake-driver MODULE fake_driver.c)
  target_link_libraries(fake-driver PRIVATE apk_defaults)
  # Test 6's MoltenVK stand-in, with MoltenVK's Vulkan headers.
  add_library(fake-vulkan MODULE fake_vulkan.c)
  target_include_directories(fake-vulkan SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(fake-vulkan PRIVATE apk_defaults "-framework CoreFoundation")
  # Their functions run on guest stacks, as graphics/host's do.
  set(on_guest_stacks "")
  if(APK_SANITIZE)
    set(on_guest_stacks -fno-sanitize=address)
  endif()
  set(hook_options -fobjc-arc ${on_guest_stacks})
  set_source_files_properties(fake_driver.c fake_vulkan.c PROPERTIES COMPILE_OPTIONS "${on_guest_stacks}")
  set_source_files_properties(metal_hook.m window_host.m session_layers.m session_layers_test.m test_control.m
    PROPERTIES COMPILE_OPTIONS "${hook_options}")
  set(moltenvk "${APK_MOLTENVK_DIR}/macos/libMoltenVK.dylib")
  set(angle "${APK_ANGLE_DIR}/out/macos/libGLESv2.dylib")
  # One run of a guest program from tools/build-front-libs.sh: its name, the program, its expected output
  # (<expected>.out, and <expected>.err when it exists), the system's libraries and the changes of the
  # environment that set the providers (ENVIRONMENT_MODIFICATION). HOST_LINES <regex> names the lines of stderr
  # that graphics-run writes on a thread the program does not wait for (tools/run-fixture.cmake).
  function(add_guest_test name program expected libraries)
    cmake_parse_arguments(PARSE_ARGV 4 GUEST "" "HOST_LINES" "")
    set(error "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${expected}.err")
      set(error "-D EXPECTED_ERROR=${CMAKE_CURRENT_SOURCE_DIR}/${expected}.err")
    endif()
    set(host_lines "")
    if(DEFINED GUEST_HOST_LINES)
      set(host_lines "-D HOST_LINES=${GUEST_HOST_LINES}")
    endif()
    add_test(NAME graphics.${name}
      COMMAND ${CMAKE_COMMAND}
        -D CONVERT=$<TARGET_FILE:apk-convert>
        -D RUN=$<TARGET_FILE:graphics-run>
        -D TEST=${program}
        -D FIXTURE=${APK_GRAPHICS_GUEST_DIR}/${program}
        "-D SYSTEM_LIBRARIES=${libraries}"
        -D WORK=${CMAKE_CURRENT_BINARY_DIR}/${name}_runs
        -D EXPECTED_OUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/${expected}.out
        ${error}
        ${host_lines}
        -D SYSTEM=${APK_GUEST_SYSTEM_DIR}
        -P ${PROJECT_SOURCE_DIR}/tools/run-fixture.cmake)
    set_tests_properties(graphics.${name} PROPERTIES ENVIRONMENT_MODIFICATION "${GUEST_UNPARSED_ARGUMENTS}"
      SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)
  endfunction()
  function(add_bind_test name expected)
    add_guest_test(${name} bind-test ${expected} "${thunk_system_libraries}" ${ARGN})
  endfunction()
  if(EXISTS "${moltenvk}" AND EXISTS "${angle}")
    add_bind_test(bind bind_test "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=set:${angle}")
  else()
    add_test(NAME graphics.bind COMMAND ${CMAKE_COMMAND} -E echo
      "skipped: MoltenVK or ANGLE is missing, see tools/fetch-moltenvk.sh and tools/build-angle.sh")
    set_tests_properties(graphics.bind PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endif()
  add_bind_test(bind_missing bind_test_missing "MINATO_MOLTENVK=set:$<TARGET_FILE:fake-driver>"
    "MINATO_ANGLE=set:$<TARGET_FILE:fake-driver>")
  add_bind_test(bind_no_driver bind_test_no_driver "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:")
  # ADR-0030, test 6: libvulkan.so among the system's libraries with libnativewindow.so, against the fake
  # MoltenVK; then against MoltenVK with its environment variables set against what libvulkan pins, with
  # the Metal hook, and without the log level, which graphics/host sets before it opens MoltenVK; then the
  # queue's part against MoltenVK, with the Metal hook asking for "scheduled". The window's part: against
  # the fake, which plays the app's host side, and against MoltenVK, with the Metal hook reading drawables
  # back and the window host's background script. The hook prints each drawable as Metal presents it, which
  # the program does not wait for, so its lines are compared apart from the program's (HOST_LINES).
  set(vulkan_system_libraries ${thunk_system_libraries} "${APK_BIONIC_DIR}/libnativewindow.so"
    "${APK_GRAPHICS_GUEST_DIR}/libvulkan.so")
  add_guest_test(vulkan_fake vulkan-fake-test vulkan_fake_test "${vulkan_system_libraries}"
    "MINATO_MOLTENVK=set:$<TARGET_FILE:fake-vulkan>" "MINATO_ANGLE=unset:")
  add_guest_test(vulkan_window vulkan-window-test vulkan_window_test "${vulkan_system_libraries}"
    "MINATO_MOLTENVK=set:$<TARGET_FILE:fake-vulkan>" "MINATO_ANGLE=unset:" "MINATO_WINDOW=set:64x48")
  # ADR-0043 b1: libnativewindow's lock and post against the window host's fake buffers, whose first lock opens the
  # window's gate 200 ms later (window_host.m).
  add_guest_test(window_lock window-lock-test window_lock_test
    "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so" "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:"
    "MINATO_WINDOW=set:64x48" "MINATO_WINDOW_BUFFERS=set:fake")
  # ADR-0043 b2: graphics/host's buffers (window.m) behind the same calls, the Metal hook reading back each drawable.
  add_guest_test(window_buffers window-buffers-test window_buffers_test
    "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so" "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:"
    "MINATO_WINDOW=set:64x48" "MINATO_METAL_HOOK=set:present:6")
  # ADR-0058 a): libnativewindow's AHardwareBuffer on graphics/host's hardware buffers, with Android 15's answers.
  add_guest_test(ahb ahb-test ahb_test "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so"
    "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:")
  # ADR-0058 c): libnativewindow's buffer queue, a SurfaceTexture's, on graphics/host's hardware buffers.
  add_guest_test(buffer_queue buffer-queue-test buffer_queue_test
    "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so" "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:")
  # ADR-0058 e1), G3c: libmediandk's AImageReader and AImage on the same queue, with Android 15's answers.
  add_guest_test(image_ndk image-ndk-test image_ndk_test
    "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so;${APK_BIONIC_DIR}/libmediandk.so"
    "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:")
  # And the same buffers through the window host's background script, with the Metal hook counting guest commits.
  add_guest_test(window_background window-background-test window_background_test
    "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so" "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:"
    "MINATO_WINDOW=set:64x48" "MINATO_METAL_HOOK=set:count" "MINATO_WINDOW_SCRIPT=set:background:3:100")
  # ADR-0043 b3: the hide rule on a layer that is not opaque and on an opaque one, with the window host watching the
  # layer's hidden and the Metal hook reading back each drawable presented, whose lines are compared apart.
  foreach(variant IN ITEMS hide hide_opaque)
    set(opaque 0)
    if(variant STREQUAL "hide_opaque")
      set(opaque 1)
    endif()
    add_guest_test(window_${variant} window-hide-test window_${variant}_test
      "${thunk_system_libraries};${APK_BIONIC_DIR}/libnativewindow.so" "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=unset:"
      "MINATO_WINDOW=set:64x48" "MINATO_WINDOW_OPAQUE=set:${opaque}" "MINATO_WINDOW_WATCH=set:hidden"
      "MINATO_METAL_HOOK=set:present:6" HOST_LINES "^metal:")
  endforeach()
  if(EXISTS "${moltenvk}")
    add_guest_test(vulkan_moltenvk vulkan-moltenvk-test vulkan_moltenvk_test "${vulkan_system_libraries}"
      "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=unset:" "MINATO_METAL_HOOK=set:1"
      "MVK_CONFIG_LOG_LEVEL=unset:" "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=set:0"
      "MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=set:1" "MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=set:3"
      "MVK_CONFIG_API_VERSION_TO_ADVERTISE=set:10")
    add_guest_test(vulkan_queue vulkan-queue-test vulkan_queue_test "${vulkan_system_libraries}"
      "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=unset:" "MINATO_METAL_HOOK=set:scheduled")
    add_guest_test(vulkan_present vulkan-present-test vulkan_present_test "${vulkan_system_libraries}"
      "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=unset:" "MINATO_METAL_HOOK=set:present"
      "MINATO_WINDOW=set:64x48" "MINATO_WINDOW_SCRIPT=set:background:70" HOST_LINES "^metal:")
    # ADR-0041 decision 11 c): a present graphics/host does not show, at the drawable's present.
    add_guest_test(vulkan_drop vulkan-drop-test vulkan_drop_test "${vulkan_system_libraries}"
      "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=unset:" "MINATO_METAL_HOOK=set:count" "MINATO_WINDOW=set:64x48")
    # A swapchain destroyed right after its present, while MoltenVK's scheduled handler, which the Metal hook runs
    # 100 ms late, is still to present its drawable: graphics/host's destroy waits for it (vulkan.m).
    add_guest_test(vulkan_destroy vulkan-destroy-test vulkan_destroy_test "${vulkan_system_libraries}"
      "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=unset:" "MINATO_METAL_HOOK=set:late-scheduled:100"
      "MINATO_WINDOW=set:64x48")
    # ADR-0058 e1), decision 7: VK_ANDROID_external_memory_android_hardware_buffer on graphics/host's hardware
    # buffers, with Android 15's answers.
    add_guest_test(vk_ahb vk-ahb-test vk_ahb_test "${vulkan_system_libraries}" "MINATO_MOLTENVK=set:${moltenvk}"
      "MINATO_ANGLE=unset:")
  else()
    foreach(name IN ITEMS vulkan_moltenvk vulkan_queue vulkan_present vulkan_drop vulkan_destroy vk_ahb)
      add_test(NAME graphics.${name} COMMAND ${CMAKE_COMMAND} -E echo
        "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/fetch-moltenvk.sh")
      set_tests_properties(graphics.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
    endforeach()
  endif()
  # ADR-0029, tests 5 to 7: libEGL.so, libGLESv2.so and libGLESv3.so among the system's libraries with
  # libnativewindow.so and libandroid.so. Test 5 against fake-angle (fake_angle.py), which records what
  # arrives; tests 6 and 7 against ANGLE's macOS slice with the window, test_control.m playing the app and
  # the Metal hook counting the guests' commits; test 7's second part with the window host's glue script,
  # which begins a session for native_app_glue.
  enable_language(ASM)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fake_angle.S"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/fake_angle.py"
      --names "${APK_GRAPHICS_GEN_DIR}/gles_names.inc" --out "${CMAKE_CURRENT_BINARY_DIR}/fake_angle.S"
    DEPENDS fake_angle.py "${APK_GRAPHICS_GEN_DIR}/gles_names.inc"
    COMMENT "graphics/tests/fake_angle.py: test 5's host library in ANGLE's place")
  add_library(fake-angle MODULE "${CMAKE_CURRENT_BINARY_DIR}/fake_angle.S")
  # CMake's ASM has no rule to link a module on Apple platforms; C's links it.
  set_target_properties(fake-angle PROPERTIES LINKER_LANGUAGE C)
  set(gles_system_libraries ${thunk_system_libraries} "${APK_BIONIC_DIR}/libnativewindow.so" "${APK_BIONIC_DIR}/libz.so"
    "${APK_BIONIC_DIR}/libandroid.so" "${APK_GRAPHICS_GUEST_DIR}/libEGL.so" "${APK_GRAPHICS_GUEST_DIR}/libGLESv2.so"
    "${APK_GRAPHICS_GUEST_DIR}/libGLESv3.so" "${APK_GRAPHICS_GUEST_DIR}/libGLESv1_CM.so")
  add_guest_test(gles_thunk gles-thunk-test gles_thunk_test "${gles_system_libraries}"
    "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=set:$<TARGET_FILE:fake-angle>")
  if(EXISTS "${angle}")
    set(with_angle "MINATO_MOLTENVK=unset:" "MINATO_ANGLE=set:${angle}" "MINATO_METAL_HOOK=set:count"
      "MINATO_WINDOW=set:64x48")
    add_guest_test(gles_front gles-front-test gles_front_test "${gles_system_libraries}" ${with_angle})
    add_guest_test(gles_gate gles-gate-test gles_gate_test "${gles_system_libraries}" ${with_angle})
    # ADR-0044 c): the same gates on the ES 3.1 path, ANGLE's Vulkan backend on MoltenVK, whose submits wait in the
    # background in graphics/host's functions (angle_vulkan.m); its display has no Metal events.
    if(EXISTS "${moltenvk}")
      add_guest_test(gles_gate_vulkan gles-gate-test gles_gate_vulkan_test "${gles_system_libraries}"
        "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=set:${angle}" "MINATO_METAL_HOOK=set:count"
        "MINATO_WINDOW=set:64x48" "MINATO_ANGLE_BACKEND=set:vulkan")
    else()
      add_test(NAME graphics.gles_gate_vulkan COMMAND ${CMAKE_COMMAND} -E echo
        "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/build-moltenvk.sh")
      set_tests_properties(graphics.gles_gate_vulkan PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
    endif()
    add_guest_test(gles_glue gles-glue-test gles_glue_test "${gles_system_libraries}" ${with_angle}
      "MINATO_WINDOW_SCRIPT=set:glue")
    # ADR-0041 decision 11 c): a present graphics/host does not show, at the drawable's present.
    add_guest_test(gles_drop gles-drop-test gles_drop_test "${gles_system_libraries}" ${with_angle})
    # ADR-0041 a1: libGLESv1_CM on ANGLE's ES 1.x, as an app linked against the NDK's stub reaches it.
    add_guest_test(gles1_front gles1-front-test gles1_front_test "${gles_system_libraries}" ${with_angle})
    # ADR-0058 b): EGL images of AHardwareBuffers on ANGLE's Metal display, and on its Vulkan display, whose R8G8B8X8
    # samples the alpha in memory (graphics/host/image.m's named limit).
    add_guest_test(ahb_image ahb-image-test ahb_image_test "${gles_system_libraries}" ${with_angle})
    if(EXISTS "${moltenvk}")
      add_guest_test(ahb_image_vulkan ahb-image-test ahb_image_vulkan_test "${gles_system_libraries}"
        "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=set:${angle}" "MINATO_METAL_HOOK=set:count"
        "MINATO_WINDOW=set:64x48" "MINATO_ANGLE_BACKEND=set:vulkan")
    else()
      add_test(NAME graphics.ahb_image_vulkan COMMAND ${CMAKE_COMMAND} -E echo
        "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/build-moltenvk.sh")
      set_tests_properties(graphics.ahb_image_vulkan PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
    endif()
  else()
    foreach(name IN ITEMS gles_front gles_gate gles_glue gles_drop gles1_front gles_gate_vulkan ahb_image
        ahb_image_vulkan)
      add_test(NAME graphics.${name} COMMAND ${CMAKE_COMMAND} -E echo
        "skipped: no ANGLE in ${APK_ANGLE_DIR}, see tools/build-angle.sh")
      set_tests_properties(graphics.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
    endforeach()
  endif()
  # ADR-0029 B2 and ADR-0030 V5: the test apps of fixtures/apps through activity-host, with graphics-app-test
  # playing the App's lifecycle for them. Each app's lines must be Android 15's (fixtures/expected/<app>.logcat),
  # and its window must count its presents: 12 scene frames and 120 more, and for vulkan-test the 240 frames of
  # its measurements (through the copy and directly). It prints "skipped:" while something is missing.
  set(APK_TEST_APPS_DIR "${PROJECT_SOURCE_DIR}/build/fixtures" CACHE PATH
    "tools/build-fixtures.sh's build directory, with the test apps' libraries in bin/ and their APKs in apks/")
  # With the Metal hook (metal_hook.m, the counters in app_counts.c), which counts guest commits for the live
  # variants and does nothing unless MINATO_METAL_HOOK is set.
  # app_test.mm is Objective-C++ (OBJCXX, enabled above) and relies on ARC for the layers it keeps.
  add_executable(graphics-app-test app_test.mm app_counts.c metal_hook.m)
  target_compile_options(graphics-app-test PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(graphics-app-test PRIVATE minato_graphics_host apkrt apkconv apk_defaults
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")
  foreach(app_presents IN ITEMS gles:132 vulkan:372)
    string(REPLACE ":" ";" app_presents "${app_presents}")
    list(GET app_presents 0 app)
    list(GET app_presents 1 presents)
    add_test(NAME graphics.${app}_app COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs" ${app}-test
      "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
      "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" ${presents})
    set_tests_properties(graphics.${app}_app PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 180)
  endforeach()
  # ADR-0043 test 3: window-test, which draws by the CPU on graphics/host's buffers (window.m), in the same
  # lifecycle: its lines must be Android 15's, and its window must count its 18 posts.
  add_test(NAME graphics.window_app COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs" window-test
    "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
    "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" 18)
  set_tests_properties(graphics.window_app PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 180)
  # The opaque layer's part G (V5's late frames): gles-test started at ANativeActivity_onCreateTranslucent, which asks
  # for a window format with alpha, so the runtime reports WINDOW_OPAQUE 0 and the window's layer is not opaque.
  add_test(NAME graphics.gles_translucent COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs" gles-test
    "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
    "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" translucent:132)
  set_tests_properties(graphics.gles_translucent PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 180)
  # ADR-0032's test 8 and ADR-0036 a) on the Mac: the live variants (gles-live, vulkan-live), which draw a frame
  # per AChoreographer callback until onDestroy, through the start, HOME, the return with a new window and Quit,
  # with a display link that follows FRAME_RATE and the Metal hook counting guest commits in the background.
  foreach(app IN ITEMS gles vulkan)
    add_test(NAME graphics.${app}_live COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs" ${app}-live
      "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
      "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" live)
    set_tests_properties(graphics.${app}_live PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120
      ENVIRONMENT "MINATO_METAL_HOOK=count")
  endforeach()
  # ADR-0044 c), tests 2 and 3 on the Mac: the ES 3.1 path, ANGLE's Vulkan backend on MoltenVK, which the session
  # chooses after the run-time check (MINATO_ANGLE_BACKEND=vulkan). gles31-test's exact values (fixtures/expected/
  # gles31-test.logcat; Android 15's emulator stops at ES 3.0) and its 124 presents; gles-test's Android 15 lines and
  # 132 presents; each through one swapchain for its window of one size. gles-live as above, Metal's hook counting
  # MoltenVK's commits.
  foreach(case IN ITEMS gles31_app:gles31-test:124 gles_app_vulkan:gles-test:132 gles_live_vulkan:gles-live:live)
    string(REPLACE ":" ";" case "${case}")
    list(GET case 0 name)
    list(GET case 1 app)
    list(GET case 2 mode)
    add_test(NAME graphics.${name} COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs" ${app}
      "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
      "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" ${mode})
    set_tests_properties(graphics.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 180
      ENVIRONMENT "MINATO_ANGLE_BACKEND=vulkan;MINATO_METAL_HOOK=count")
  endforeach()
  # ADR-0041 test 3 on the Mac: present-gles and present-vulkan, which ask for their presents' times at 60 and 120
  # frames a second until they finish by themselves. The Mac checks the calls, the app's lines against Android 15's
  # and present.csv; the iPhone judges the timing.
  foreach(api IN ITEMS gles vulkan)
    add_test(NAME graphics.present_${api} COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs"
      present-${api} "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
      "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" present)
    set_tests_properties(graphics.present_${api} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 150)
  endforeach()
  # ADR-0058 e1), decision 7: vk-ahb-test as an app, the way it runs on the iPhone (tools/java-start-check.sh): its
  # lines must be Android 15's (fixtures/expected/vk-ahb-test.logcat); it draws nothing, so its window counts no
  # presents.
  add_test(NAME graphics.vk_ahb_app COMMAND graphics-app-test "${CMAKE_CURRENT_BINARY_DIR}/app_runs" vk-ahb-test
    "${APK_TEST_APPS_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${APK_GRAPHICS_GUEST_DIR}"
    "${PROJECT_SOURCE_DIR}/fixtures/expected" "${angle}" "${moltenvk}" 0)
  set_tests_properties(graphics.vk_ahb_app PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 180)
  # ADR-0037 a): the parity benchmark's scene built for the Mac (graphics/bench, graphics-bench-host) and as the
  # APKs of tools/build-bench.sh (graphics-app-test's bench mode), each API and load with ticks scripted in
  # lockstep (bench_test.py): both must draw the same frame 1000, and the APK must end by itself.
  set(APK_BENCH_DIR "${PROJECT_SOURCE_DIR}/build/graphics/bench" CACHE PATH
    "tools/build-bench.sh's build directory, with the bench's APKs at its top and their libraries in bin/")
  # ADR-0037 b): graphics.recorder, the same programs with graphics/host's layer recording a run of 120 frames per API
  # and build (bench_test.py --record): every drawable once and in order (recording_check.h).
  if(TARGET minato_bench)
    add_executable(graphics-bench-host bench_host.mm)
    target_compile_options(graphics-bench-host PRIVATE -fobjc-arc)
    target_link_libraries(graphics-bench-host PRIVATE minato_bench minato_graphics_host apk_defaults
      "-framework Foundation" "-framework Metal" "-framework QuartzCore")
    add_test(NAME graphics.bench
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/bench_test.py"
        --host $<TARGET_FILE:graphics-bench-host> --app-test $<TARGET_FILE:graphics-app-test>
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/bench_runs" --bench "${APK_BENCH_DIR}" --bionic "${APK_BIONIC_DIR}"
        --system "${APK_GUEST_SYSTEM_DIR}" --front "${APK_GRAPHICS_GUEST_DIR}"
        --manifests "${PROJECT_SOURCE_DIR}/graphics/bench/android" --angle "${angle}" --moltenvk "${moltenvk}")
    set_tests_properties(graphics.bench PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 600)
    add_test(NAME graphics.recorder
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/bench_test.py" --record
        --host $<TARGET_FILE:graphics-bench-host> --app-test $<TARGET_FILE:graphics-app-test>
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/bench_runs" --bench "${APK_BENCH_DIR}" --bionic "${APK_BIONIC_DIR}"
        --system "${APK_GUEST_SYSTEM_DIR}" --front "${APK_GRAPHICS_GUEST_DIR}"
        --manifests "${PROJECT_SOURCE_DIR}/graphics/bench/android" --angle "${angle}" --moltenvk "${moltenvk}")
    set_tests_properties(graphics.recorder PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 300)
    # ADR-0042 c) 2): graphics.recorder_class, the host build's recording through MinatoLayerRecorder on a layer class
    # without its own nextDrawable, as Godot's GDTMetalLayer (bench_test.py --record-class).
    add_test(NAME graphics.recorder_class
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/bench_test.py" --record-class
        --host $<TARGET_FILE:graphics-bench-host> --app-test $<TARGET_FILE:graphics-app-test>
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/bench_runs" --bench "${APK_BENCH_DIR}" --bionic "${APK_BIONIC_DIR}"
        --system "${APK_GUEST_SYSTEM_DIR}" --front "${APK_GRAPHICS_GUEST_DIR}"
        --manifests "${PROJECT_SOURCE_DIR}/graphics/bench/android" --angle "${angle}" --moltenvk "${moltenvk}")
    set_tests_properties(graphics.recorder_class PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)
  else()
    foreach(name IN ITEMS bench recorder recorder_class)
      add_test(NAME graphics.${name} COMMAND ${CMAKE_COMMAND} -E echo
        "skipped: the bench's host build needs ANGLE, MoltenVK and the NDK's glslc (graphics/bench/CMakeLists.txt)")
      set_tests_properties(graphics.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
    endforeach()
  endif()
  # The tests that time something on the GPU's way or wait for it within a limit (graphics.window_buffers' locks,
  # 1 s), and graphics.bench, whose heavy scene holds the GPU for a minute, never run at once under ctest -j: another
  # test's GPU work could stall a frame past their limits (graphics.vulkan_live once saw a frame interval of 108 ms
  # in a full run with -j8).
  set_tests_properties(graphics.angle_commit_gate graphics.angle_presentation_time graphics.gles_front
    graphics.gles1_front graphics.gles_gate graphics.gles_glue graphics.gles_drop graphics.vulkan_queue
    graphics.vulkan_present graphics.vulkan_drop graphics.gles_app
    graphics.gles_translucent graphics.vulkan_app graphics.gles_live graphics.vulkan_live graphics.present_gles
    graphics.present_vulkan graphics.bench graphics.recorder graphics.recorder_class graphics.window_buffers
    graphics.window_background graphics.window_hide graphics.window_hide_opaque graphics.window_app
    graphics.gles31_app graphics.gles_app_vulkan graphics.gles_live_vulkan graphics.gles_gate_vulkan
    PROPERTIES RESOURCE_LOCK gpu)
  # V3's check for the iPhone, first here: graphics/host's probe with MoltenVK for macOS. MoltenVK
  # logs to stderr in between, so the probe's lines are matched, not the whole output.
  if(EXISTS "${moltenvk}" AND EXISTS "${APK_MOLTENVK_DIR}/include/vulkan/vulkan_metal.h")
    add_executable(graphics-probe graphics_probe.c)
    target_link_libraries(graphics-probe PRIVATE minato_graphics_host apk_defaults)
    add_test(NAME graphics.probe COMMAND graphics-probe "${moltenvk}")
    set_tests_properties(graphics.probe PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION
      "graphics probe: MoltenVK opened from [^\n]*/libMoltenVK\\.dylib\n.*graphics probe: an instance at Vulkan 1\\.3 with VK_KHR_surface and VK_EXT_metal_surface, MoltenVK offers 1\\.4\\.357\n.*graphics probe: GPU [^\n]+, driver MoltenVK 1\\.4\\.2, device at Vulkan 1\\.3\\.357\n.*graphics probe: a Metal surface on a CAMetalLayer of 64 x 64 points at scale 3, 192 x 192 pixels, presentable from queue family 0\n.*graphics probe: surface and instance destroyed\n")
  else()
    add_test(NAME graphics.probe COMMAND ${CMAKE_COMMAND} -E echo
      "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/fetch-moltenvk.sh")
    set_tests_properties(graphics.probe PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endif()
else()
  foreach(name IN ITEMS bind bind_missing bind_no_driver vulkan_fake vulkan_window window_lock window_buffers
      window_background window_hide window_hide_opaque vulkan_moltenvk vulkan_queue vulkan_present vulkan_drop probe
      gles_thunk gles_front gles1_front gles_gate gles_glue gles_drop gles_app gles_translucent vulkan_app window_app
      gles_live vulkan_live present_gles present_vulkan bench recorder recorder_class gles31_app gles_app_vulkan
      gles_live_vulkan gles_gate_vulkan)
    add_test(NAME graphics.${name} COMMAND ${CMAKE_COMMAND} -E echo
      "skipped: graphics/host needs build/graphics/gen/ from tools/build-front-libs.sh; configure again after it")
    set_tests_properties(graphics.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endforeach()
endif()

# ADR-0044 a): graphics/host's probe of the last vertex in a pass set up as MoltenVK's private API sets it up, on the
# Mac's GPU. It passes when the last vertex provokes (the iPhone runs it in a debug build of the app).
if(TARGET minato_graphics_host)
  add_executable(graphics-provoking-probe provoking_probe.c)
  target_link_libraries(graphics-provoking-probe PRIVATE minato_graphics_host apk_defaults)
  add_test(NAME graphics.provoking_probe COMMAND graphics-provoking-probe)
  set_tests_properties(graphics.provoking_probe PROPERTIES TIMEOUT 60)
  # ADR-0044 c): the same triangle as the run-time check before the ES 3.1 path, quietly; forced, it refuses.
  add_executable(graphics-angle-vulkan-check angle_vulkan_check.c)
  target_link_libraries(graphics-angle-vulkan-check PRIVATE minato_graphics_host apk_defaults)
  add_test(NAME graphics.angle_vulkan_check COMMAND graphics-angle-vulkan-check)
  set_tests_properties(graphics.angle_vulkan_check PROPERTIES TIMEOUT 60)
else()
  add_test(NAME graphics.provoking_probe COMMAND ${CMAKE_COMMAND} -E echo
    "skipped: graphics/host is left out, see graphics/host/CMakeLists.txt")
  set_tests_properties(graphics.provoking_probe PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
endif()

# ADR-0041 decision 11 c): graphics/host's probe of the drawables' pool, for half a second per run on the Mac's offscreen
# layer, whose numbers say nothing about the pool: it passes when it prints each run as the iPhone's debug app does.
if(TARGET minato_graphics_host)
  add_executable(graphics-drawable-probe graphics_drawable_probe.c)
  target_link_libraries(graphics-drawable-probe PRIVATE minato_graphics_host apk_defaults)
  add_test(NAME graphics.drawable_probe COMMAND graphics-drawable-probe)
  set(waits "nextDrawable waited p50 [0-9.]+ ms, p99 [0-9.]+ ms, [0-9]+ longer than half a period")
  set(waits "${waits}; ticks [0-9.]+ ms apart \\(p50\\)\n")
  string(CONCAT probe_lines "drawable probe: a layer of 64 x 64 pixels, [0-9]+ drawables\n"
    "drawable probe: 60 Hz, let go at present: 30 frames, ${waits}"
    "drawable probe: 60 Hz, held 3 frames: 30 frames, ${waits}"
    "drawable probe: 120 Hz, let go at present: 60 frames, ${waits}"
    "drawable probe: 120 Hz, held 3 frames: 60 frames, ${waits}")
  set_tests_properties(graphics.drawable_probe PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "${probe_lines}")
else()
  add_test(NAME graphics.drawable_probe COMMAND ${CMAKE_COMMAND} -E echo
    "skipped: graphics/host is left out, see graphics/host/CMakeLists.txt")
  set_tests_properties(graphics.drawable_probe PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
endif()

# The graphics breadth's G3b: buffer-queue-host-test, the buffer queue itself (runtime/android/native_window.cpp),
# and G3c's AImageReader on it (image_reader.cpp), built for the Mac on a counted fake AHardwareBuffer, so that this
# preset's AddressSanitizer and UndefinedBehaviorSanitizer see their memory, which buffer-queue-test and
# image-ndk-test run as guest code. Only the NDK's android/ and media/ headers join, through a folder of their own,
# so that the Mac's system headers stay; the host windows' syscalls are the test's.
set(ndk_android_headers "${APK_ANDROID_NDK}/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include/android")
if(EXISTS "${ndk_android_headers}/hardware_buffer.h")
  set(buffer_queue_host_include "${CMAKE_CURRENT_BINARY_DIR}/buffer-queue-host-include")
  if(NOT EXISTS "${buffer_queue_host_include}/android")
    file(MAKE_DIRECTORY "${buffer_queue_host_include}")
    file(CREATE_LINK "${ndk_android_headers}" "${buffer_queue_host_include}/android" SYMBOLIC)
  endif()
  if(NOT EXISTS "${buffer_queue_host_include}/media")
    file(CREATE_LINK "${ndk_android_headers}/../media" "${buffer_queue_host_include}/media" SYMBOLIC)
  endif()
  add_executable(buffer-queue-host-test buffer_queue_host_test.cpp
    "${PROJECT_SOURCE_DIR}/runtime/android/native_window.cpp" "${PROJECT_SOURCE_DIR}/runtime/android/image_reader.cpp")
  target_include_directories(buffer-queue-host-test SYSTEM PRIVATE "${buffer_queue_host_include}")
  target_include_directories(buffer-queue-host-test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/include")
  target_compile_options(buffer-queue-host-test PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/buffer_queue_host_shim.h" -Wno-deprecated-declarations)
  # The image reader finds the queue's functions in the test's own program.
  target_compile_definitions(buffer-queue-host-test PRIVATE syscall=minato_host_syscall MINATO_QUEUE_LIBRARY=nullptr)
  target_link_libraries(buffer-queue-host-test PRIVATE apk_defaults)
  add_test(NAME graphics.buffer_queue_host COMMAND buffer-queue-host-test)
  # ADR-0058 decision 7: external-memory-host-test, libvulkan's AHardwareBuffer functions (graphics/vulkan/
  # external_memory.c with common.c) on the Mac against MoltenVK and AHardwareBuffer stand-ins of its own, for the
  # sanitizers, which vk-ahb-test runs as guest code; tools/frontgen's tables come from tools/build-front-libs.sh.
  if(EXISTS "${APK_GRAPHICS_GEN_DIR}/vulkan_structs.inc" AND EXISTS "${APK_MOLTENVK_DIR}/include/vulkan/vulkan.h")
    add_executable(external-memory-host-test external_memory_host_test.c
      "${PROJECT_SOURCE_DIR}/graphics/vulkan/external_memory.c" "${PROJECT_SOURCE_DIR}/graphics/vulkan/common.c")
    target_include_directories(external-memory-host-test SYSTEM PRIVATE "${buffer_queue_host_include}"
      "${APK_MOLTENVK_DIR}/include")
    target_include_directories(external-memory-host-test PRIVATE "${PROJECT_SOURCE_DIR}/graphics/vulkan"
      "${PROJECT_SOURCE_DIR}/graphics/guest" "${APK_GRAPHICS_GEN_DIR}" "${PROJECT_SOURCE_DIR}/runtime/include")
    target_compile_options(external-memory-host-test PRIVATE
      -include "${CMAKE_CURRENT_SOURCE_DIR}/buffer_queue_host_shim.h" -Wno-deprecated-declarations)
    target_compile_definitions(external-memory-host-test PRIVATE syscall=minato_host_syscall)
    target_link_libraries(external-memory-host-test PRIVATE apk_defaults)
    add_test(NAME graphics.external_memory_host COMMAND external-memory-host-test)
  else()
    add_test(NAME graphics.external_memory_host COMMAND ${CMAKE_COMMAND} -E echo
      "skipped: no tables of tools/frontgen in ${APK_GRAPHICS_GEN_DIR} or no MoltenVK headers")
    set_tests_properties(graphics.external_memory_host PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endif()
else()
  add_test(NAME graphics.buffer_queue_host COMMAND ${CMAKE_COMMAND} -E echo
    "skipped: no NDK headers at ${ndk_android_headers} (APK_ANDROID_NDK)")
  set_tests_properties(graphics.buffer_queue_host PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  add_test(NAME graphics.external_memory_host COMMAND ${CMAKE_COMMAND} -E echo
    "skipped: no NDK headers at ${ndk_android_headers} (APK_ANDROID_NDK)")
  set_tests_properties(graphics.external_memory_host PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
endif()

# ADR-0041 decision 11: graphics/host's time for Core Animation, the refresh asked for on the refresh grid less half a
# period, at the limits of a refresh.
if(TARGET minato_graphics_host)
  add_executable(graphics-present-time-test present_time_test.c)
  target_include_directories(graphics-present-time-test PRIVATE "${PROJECT_SOURCE_DIR}/graphics/host")
  target_link_libraries(graphics-present-time-test PRIVATE minato_graphics_host apk_defaults)
  add_test(NAME graphics.present_time COMMAND graphics-present-time-test)
else()
  add_test(NAME graphics.present_time COMMAND ${CMAKE_COMMAND} -E echo
    "skipped: graphics/host is left out, see graphics/host/CMakeLists.txt")
  set_tests_properties(graphics.present_time PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
endif()
