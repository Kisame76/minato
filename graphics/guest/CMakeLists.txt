# The guest side of Minato's graphics (ADR-0029, ADR-0030), built with the Android NDK for arm64-v8a by
# tools/build-front-libs.sh: tools/frontgen's tables, stubs, constructors and export lists in
# build/graphics/gen/, the binding the front libs share, libvulkan.so, libEGL.so, libGLESv1_CM.so, libGLESv2.so
# and libGLESv3.so, and the guest programs of ADR-0030's tests 2, 4 and 6, ADR-0029's tests 5 to 7 and ADR-0041
# a1's.
cmake_minimum_required(VERSION 4.0)
project(MinatoGraphicsGuest LANGUAGES C ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build the guest side with tools/build-front-libs.sh (Android NDK, arm64-v8a).")
endif()

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH graphics)
cmake_path(GET graphics PARENT_PATH repo)
set(APK_GRAPHICS_GEN_DIR "${repo}/build/graphics/gen" CACHE PATH "tools/frontgen's outputs")
set(APK_MOLTENVK_DIR "${repo}/build/moltenvk" CACHE PATH "MoltenVK and vk.xml from tools/fetch-moltenvk.sh")
set(APK_ANGLE_DIR "${repo}/build/angle/angle" CACHE PATH "ANGLE's checkout from tools/build-angle.sh")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(gen "${APK_GRAPHICS_GEN_DIR}")
set(registries "${APK_MOLTENVK_DIR}/vk.xml"
  "${APK_ANGLE_DIR}/third_party/OpenGL-Registry/src/xml/gl.xml" "${APK_ANGLE_DIR}/scripts/gl_angle_ext.xml"
  "${APK_ANGLE_DIR}/third_party/EGL-Registry/src/api/egl.xml" "${APK_ANGLE_DIR}/scripts/egl_angle_ext.xml")
foreach(registry IN LISTS registries)
  if(NOT EXISTS "${registry}")
    message(FATAL_ERROR "${registry} is missing; see tools/fetch-moltenvk.sh and tools/build-angle.sh")
  endif()
endforeach()

# tools/frontgen: per front lib its stubs and table, its layout, the provider's names, the export lists
# and the report (ADR-0030, decision 7).
set(generated "${gen}/vulkan_procs.inc" "${gen}/vulkan_extensions.inc" "${gen}/vulkan_structs.inc")
foreach(lib IN ITEMS vulkan gles egl)
  list(APPEND generated "${gen}/${lib}_stubs.S" "${gen}/${lib}_bind.c" "${gen}/${lib}_table.h"
    "${gen}/${lib}_names.inc" "${gen}/${lib}_report.json")
endforeach()
set(stubs "")
foreach(library IN ITEMS libvulkan libGLESv1_CM libGLESv2 libGLESv3 libEGL)
  list(APPEND generated "${gen}/${library}.map")
  list(APPEND stubs "${ANDROID_TOOLCHAIN_ROOT}/sysroot/usr/lib/aarch64-linux-android/35/${library}.so")
endforeach()
file(GLOB frontgen "${repo}/tools/frontgen/*.py")
add_custom_command(OUTPUT ${generated}
  COMMAND Python3::Interpreter "${repo}/tools/frontgen/frontgen.py" --out "${gen}"
    --vk-xml "${APK_MOLTENVK_DIR}/vk.xml" --angle "${APK_ANGLE_DIR}" --ndk "${ANDROID_NDK}"
  DEPENDS ${frontgen} ${registries} ${stubs}
    "${graphics}/vulkan/frontgen.txt" "${graphics}/gles/frontgen.txt" "${graphics}/egl/frontgen.txt"
  COMMENT "tools/frontgen: the front libs' tables, stubs and export lists")

# ADR-0030, test 2: thunk-test calls every stub, with a call list generated from the C headers.
set(tests "${graphics}/tests")
file(GLOB headers "${CMAKE_SYSROOT}/usr/include/GLES*/*.h" "${CMAKE_SYSROOT}/usr/include/EGL/*.h"
  "${CMAKE_SYSROOT}/usr/include/KHR/*.h" "${CMAKE_SYSROOT}/usr/include/vulkan/*.h"
  "${APK_ANGLE_DIR}/include/GLES*/*.h" "${APK_ANGLE_DIR}/include/EGL/*.h" "${APK_ANGLE_DIR}/include/KHR/*.h"
  "${APK_MOLTENVK_DIR}/include/vulkan/*.h")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/thunk_calls.c"
  COMMAND Python3::Interpreter "${tests}/thunk_calls.py" --gen "${gen}" --out "${CMAKE_BINARY_DIR}/thunk_calls.c"
    --ndk "${ANDROID_NDK}" --angle "${APK_ANGLE_DIR}" --moltenvk "${APK_MOLTENVK_DIR}"
  DEPENDS "${tests}/thunk_calls.py" "${tests}/ndk_headers.py" ${generated} ${headers}
  COMMENT "graphics/tests/thunk_calls.py: test 2's calls from the C headers")
add_executable(thunk-test "${tests}/thunk_test.c" "${tests}/thunk_test.S" "${CMAKE_BINARY_DIR}/thunk_calls.c"
  "${gen}/vulkan_stubs.S" "${gen}/gles_stubs.S" "${gen}/egl_stubs.S")
target_include_directories(thunk-test PRIVATE "${tests}" "${gen}")
target_compile_options(thunk-test PRIVATE -Wall -Wextra)

# The binding every front lib shares (ADR-0030, decision 3): minato_bind.h and bind.c, with the runtime's
# guest interface for the syscall's number.
add_library(minato_bind OBJECT bind.c)
target_include_directories(minato_bind PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${repo}/runtime/include")
target_compile_options(minato_bind PRIVATE -Wall -Wextra)

# ADR-0030, test 4: bind-test has its tables bound by <lib>_bind.c's constructors.
add_executable(bind-test "${tests}/bind_test.c" "${gen}/vulkan_stubs.S" "${gen}/gles_stubs.S" "${gen}/egl_stubs.S"
  "${gen}/vulkan_bind.c" "${gen}/gles_bind.c" "${gen}/egl_bind.c")
target_include_directories(bind-test PRIVATE "${gen}")
target_link_libraries(bind-test PRIVATE minato_bind)
target_compile_options(bind-test PRIVATE -Wall -Wextra)

# ADR-0030, decision 10: libvulkan.so, the Vulkan front lib: tools/frontgen's stubs and binding with the
# functions written by hand in graphics/vulkan/. Those compile against MoltenVK's Vulkan headers, the ones
# of the vk.xml the table comes from, which define every structure its names take. It exports exactly the
# NDK's names and binds its own references to itself.
set(vulkan "${graphics}/vulkan")
add_library(vulkan SHARED "${vulkan}/common.c" "${vulkan}/instance.c" "${vulkan}/device.c" "${vulkan}/procs.c"
  "${vulkan}/queue.c" "${vulkan}/surface.c" "${vulkan}/swapchain.c" "${gen}/vulkan_stubs.S" "${gen}/vulkan_bind.c")
target_include_directories(vulkan PRIVATE "${APK_MOLTENVK_DIR}/include" "${gen}" "${repo}/runtime/include")
# The window (ADR-0028): ANativeWindow's references from the NDK's stub, its private getInfo by dlsym.
target_link_libraries(vulkan PRIVATE minato_bind nativewindow dl log)
target_link_options(vulkan PRIVATE "-Wl,--version-script=${gen}/libvulkan.map" -Wl,-Bsymbolic)
set_target_properties(vulkan PROPERTIES LINK_DEPENDS "${gen}/libvulkan.map"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_options(vulkan PRIVATE -Wall -Wextra)

# ADR-0030, test 6: vulkan-fake-test and vulkan-window-test against graphics/tests/fake_vulkan.c in
# MoltenVK's place, the others against MoltenVK itself.
foreach(test IN ITEMS vulkan_fake_test vulkan_moltenvk_test vulkan_queue_test vulkan_window_test vulkan_present_test
    vulkan_drop_test vulkan_destroy_test)
  string(REPLACE "_" "-" program ${test})
  add_executable(${program} "${tests}/${test}.c")
  target_include_directories(${program} PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(${program} PRIVATE vulkan nativewindow dl)
  target_compile_options(${program} PRIVATE -Wall -Wextra)
endforeach()
# vulkan-window-test sets the refresh grid for VK_GOOGLE_display_timing through graphics-run's test control, and
# vulkan-drop-test (ADR-0041 decision 11 c)) also the present not to show; vulkan-destroy-test takes its window there.
foreach(program IN ITEMS vulkan-window-test vulkan-drop-test vulkan-destroy-test)
  target_sources(${program} PRIVATE "${tests}/test_control_guest.c")
  target_include_directories(${program} PRIVATE "${tests}")
  target_link_libraries(${program} PRIVATE minato_bind)
endforeach()

# ADR-0043 b1 and b2: window-lock-test, libnativewindow's lock and post against graphics-run's fake buffers, and
# window-buffers-test, window-background-test and window-hide-test (b3) against graphics/host's.
foreach(test IN ITEMS window_lock_test window_buffers_test window_background_test window_hide_test)
  string(REPLACE "_" "-" program ${test})
  add_executable(${program} "${tests}/${test}.c")
  target_link_libraries(${program} PRIVATE nativewindow dl)
  target_compile_options(${program} PRIVATE -Wall -Wextra)
endforeach()
# ADR-0058 a): ahb-test, libnativewindow's AHardwareBuffer against graphics/host's hardware buffers, and as a shell
# program on the emulator for Android 15's answers.
add_executable(ahb-test "${tests}/ahb_test.c")
# getId is API 31's, above this build's 29: a weak reference, which Android 15 and Minato both resolve. Its calls with
# NULL ask for Android's answers to them.
target_link_libraries(ahb-test PRIVATE nativewindow)
target_compile_definitions(ahb-test PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_compile_options(ahb-test PRIVATE -Wall -Wextra -Wno-nonnull -Wno-unguarded-availability)

# ADR-0029, decisions 12 to 14: libEGL.so, libGLESv2.so and libGLESv3.so, tools/frontgen's stubs and binding
# with the functions written by hand in graphics/egl/ and graphics/gles/, against the NDK's headers. libGLESv1_CM
# (ADR-0041 a1), libGLESv2 and libGLESv3 are one set of objects with three export lists; each exports exactly the
# NDK's names, and the GLES libs minato_gles_proc in version APKRT_PRIVATE, which libEGL's eglGetProcAddress
# finds. libGLESv1_CM adds Android's 7 *Bounds functions (bounds.c).
set(egl "${graphics}/egl")
set(gles "${graphics}/gles")
add_library(EGL SHARED "${egl}/display.c" "${egl}/surface.c" "${egl}/image.c" "${egl}/procs.c"
  "${gen}/egl_stubs.S" "${gen}/egl_bind.c")
# AHardwareBuffer_getId is API 31's, above this build's 29: a weak reference, which Android 15 and Minato resolve.
set_source_files_properties("${egl}/image.c" PROPERTIES COMPILE_DEFINITIONS __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
  COMPILE_OPTIONS -Wno-unguarded-availability)
target_include_directories(EGL PRIVATE "${egl}" "${gen}" "${repo}/runtime/include")
# The window (ADR-0028): ANativeWindow's references from the NDK's stub, its private getInfo by dlsym.
target_link_libraries(EGL PRIVATE minato_bind nativewindow dl)
target_link_options(EGL PRIVATE "-Wl,--version-script=${gen}/libEGL.map" -Wl,-Bsymbolic)
set_target_properties(EGL PROPERTIES LINK_DEPENDS "${gen}/libEGL.map"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_options(EGL PRIVATE -Wall -Wextra)
add_library(gles_objects OBJECT "${gles}/gles.c" "${gles}/procs.c" "${gen}/gles_stubs.S" "${gen}/gles_bind.c")
target_include_directories(gles_objects PRIVATE "${gles}" "${gen}" "${repo}/runtime/include")
target_link_libraries(gles_objects PRIVATE minato_bind)
set_target_properties(gles_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(gles_objects PRIVATE -Wall -Wextra)
foreach(library IN ITEMS GLESv1_CM GLESv2 GLESv3)
  add_library(${library} SHARED $<TARGET_OBJECTS:gles_objects> $<TARGET_OBJECTS:minato_bind>)
  target_link_options(${library} PRIVATE "-Wl,--version-script=${gen}/lib${library}.map" -Wl,-Bsymbolic)
  set_target_properties(${library} PROPERTIES LINK_DEPENDS "${gen}/lib${library}.map"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endforeach()
target_sources(GLESv1_CM PRIVATE "${gles}/bounds.c")
target_compile_options(GLESv1_CM PRIVATE -Wall -Wextra)

# ADR-0029, tests 5 to 7, run by graphics-run: gles-thunk-test calls the stubs that extend or repack through
# the built libs, with test 2's caller and a list from the C headers; the others reach graphics-run's test
# control (test_control_guest.c), and gles-glue-test runs the NDK's native_app_glue as an app does.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/gles_thunk_calls.c"
  COMMAND Python3::Interpreter "${tests}/thunk_calls.py" --gles-thunks --gen "${gen}"
    --out "${CMAKE_BINARY_DIR}/gles_thunk_calls.c" --ndk "${ANDROID_NDK}" --angle "${APK_ANGLE_DIR}"
  DEPENDS "${tests}/thunk_calls.py" "${tests}/ndk_headers.py" ${generated} ${headers}
  COMMENT "graphics/tests/thunk_calls.py: test 5's calls from the C headers")
add_executable(gles-thunk-test "${tests}/gles_thunk_test.c" "${tests}/thunk_test.S"
  "${CMAKE_BINARY_DIR}/gles_thunk_calls.c")
target_include_directories(gles-thunk-test PRIVATE "${tests}")
target_link_libraries(gles-thunk-test PRIVATE EGL dl)
target_compile_options(gles-thunk-test PRIVATE -Wall -Wextra)
set(glue "${ANDROID_NDK}/sources/android/native_app_glue")
foreach(test IN ITEMS gles_front_test gles_gate_test gles_glue_test gles_drop_test)
  string(REPLACE "_" "-" program ${test})
  add_executable(${program} "${tests}/${test}.c" "${tests}/test_control_guest.c")
  target_include_directories(${program} PRIVATE "${tests}" "${gen}" "${repo}/runtime/include")
  target_link_libraries(${program} PRIVATE EGL GLESv3 minato_bind nativewindow dl)
  target_compile_options(${program} PRIVATE -Wall -Wextra)
endforeach()
# ADR-0058 c): buffer-queue-test, libnativewindow's buffer queue through its private functions.
add_executable(buffer-queue-test "${tests}/buffer_queue_test.c")
target_include_directories(buffer-queue-test PRIVATE "${repo}/runtime/include")
target_link_libraries(buffer-queue-test PRIVATE nativewindow dl)
# ANativeWindow_setFrameRate is API 30's, above this build's 29: a weak reference, which Minato resolves.
target_compile_definitions(buffer-queue-test PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_compile_options(buffer-queue-test PRIVATE -Wall -Wextra -Wno-unguarded-availability)
# ADR-0058 b): ahb-image-test, EGL images of AHardwareBuffers, linked as an app links libEGL and libGLESv3.
add_executable(ahb-image-test "${tests}/ahb_image_test.c")
target_link_libraries(ahb-image-test PRIVATE EGL GLESv3 nativewindow)
target_compile_options(ahb-image-test PRIVATE -Wall -Wextra)
# ADR-0041 a1: gles1-front-test, linked as an app links libGLESv1_CM.
add_executable(gles1-front-test "${tests}/gles1_front_test.c")
target_link_libraries(gles1-front-test PRIVATE EGL GLESv1_CM)
target_compile_options(gles1-front-test PRIVATE -Wall -Wextra)
target_sources(gles-glue-test PRIVATE "${glue}/android_native_app_glue.c")
target_include_directories(gles-glue-test PRIVATE "${glue}")
target_link_libraries(gles-glue-test PRIVATE android log)
set_source_files_properties("${glue}/android_native_app_glue.c" PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
