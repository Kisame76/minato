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

# ADR-0030, test 1: the generator in tools/frontgen/, against the NDK's C headers read by ndk_headers.py.
add_test(NAME graphics.frontgen
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/frontgen_test.py"
    --repo "${PROJECT_SOURCE_DIR}" --vk-xml "${APK_MOLTENVK_DIR}/vk.xml" --angle "${APK_ANGLE_DIR}"
    --ndk "${APK_ANDROID_NDK}" --work "${CMAKE_CURRENT_BINARY_DIR}/frontgen")
set_tests_properties(graphics.frontgen PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

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
  # With the Metal hook of test 6 (metal_hook.m), which does nothing unless MINATO_METAL_HOOK is set.
  add_executable(graphics-run "${PROJECT_SOURCE_DIR}/runtime/tools/apk_run.cpp" graphics_run.c metal_hook.m)
  target_link_libraries(graphics-run PRIVATE minato_graphics_host apkrt apk_defaults
    "-framework Foundation" "-framework Metal")
  add_library(fake-driver MODULE fake_driver.c)
  target_link_libraries(fake-driver PRIVATE apk_defaults)
  # Test 6's MoltenVK stand-in, with MoltenVK's Vulkan headers.
  add_library(fake-vulkan MODULE fake_vulkan.c)
  target_include_directories(fake-vulkan SYSTEM PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(fake-vulkan PRIVATE apk_defaults)
  # Their functions run on guest stacks, as graphics/host's do.
  set(on_guest_stacks "")
  if(APK_SANITIZE)
    set(on_guest_stacks -fno-sanitize=address)
  endif()
  set(hook_options -fobjc-arc ${on_guest_stacks})
  set_source_files_properties(fake_driver.c fake_vulkan.c PROPERTIES COMPILE_OPTIONS "${on_guest_stacks}")
  set_source_files_properties(metal_hook.m PROPERTIES COMPILE_OPTIONS "${hook_options}")
  set(moltenvk "${APK_MOLTENVK_DIR}/macos/libMoltenVK.dylib")
  set(angle "${APK_ANGLE_DIR}/out/macos/libGLESv2.dylib")
  # One run of a guest program from tools/build-front-libs.sh: its name, the program, its expected output
  # (<expected>.out, and <expected>.err when it exists), the system's libraries and the changes of the
  # environment that set the providers (ENVIRONMENT_MODIFICATION).
  function(add_guest_test name program expected libraries)
    set(error "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${expected}.err")
      set(error "-D EXPECTED_ERROR=${CMAKE_CURRENT_SOURCE_DIR}/${expected}.err")
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
        -D SYSTEM=${APK_GUEST_SYSTEM_DIR}
        -P ${PROJECT_SOURCE_DIR}/tools/run-fixture.cmake)
    set_tests_properties(graphics.${name} PROPERTIES ENVIRONMENT_MODIFICATION "${ARGN}"
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
  # ADR-0030, test 6 without the window: libvulkan.so among the system's libraries, against the fake
  # MoltenVK, then against MoltenVK with its environment variables set against what libvulkan pins, with
  # the Metal hook, and without the log level, which graphics/host sets before it opens MoltenVK.
  set(vulkan_system_libraries ${thunk_system_libraries} "${APK_GRAPHICS_GUEST_DIR}/libvulkan.so")
  add_guest_test(vulkan_fake vulkan-fake-test vulkan_fake_test "${vulkan_system_libraries}"
    "MINATO_MOLTENVK=set:$<TARGET_FILE:fake-vulkan>" "MINATO_ANGLE=unset:")
  if(EXISTS "${moltenvk}")
    add_guest_test(vulkan_moltenvk vulkan-moltenvk-test vulkan_moltenvk_test "${vulkan_system_libraries}"
      "MINATO_MOLTENVK=set:${moltenvk}" "MINATO_ANGLE=unset:" "MINATO_METAL_HOOK=set:1"
      "MVK_CONFIG_LOG_LEVEL=unset:" "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=set:0"
      "MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=set:1" "MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=set:3"
      "MVK_CONFIG_API_VERSION_TO_ADVERTISE=set:10")
  else()
    add_test(NAME graphics.vulkan_moltenvk COMMAND ${CMAKE_COMMAND} -E echo
      "skipped: no MoltenVK in ${APK_MOLTENVK_DIR}, see tools/fetch-moltenvk.sh")
    set_tests_properties(graphics.vulkan_moltenvk PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endif()
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
  foreach(name IN ITEMS bind bind_missing bind_no_driver vulkan_fake vulkan_moltenvk probe)
    add_test(NAME graphics.${name} COMMAND ${CMAKE_COMMAND} -E echo
      "skipped: graphics/host needs build/graphics/gen/ from tools/build-front-libs.sh; configure again after it")
    set_tests_properties(graphics.${name} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  endforeach()
endif()
