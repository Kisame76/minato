# The guest side of Minato's graphics (ADR-0029, ADR-0030), built with the Android NDK for arm64-v8a by
# tools/build-front-libs.sh: tools/frontgen's tables, stubs, constructors and export lists in
# build/graphics/gen/, the binding the front libs share, libvulkan.so, and the guest programs of ADR-0030's
# tests 2, 4 and 6. The other front libs follow (ADR-0029 B1).
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
foreach(library IN ITEMS libvulkan libGLESv2 libGLESv3 libEGL)
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
  "${vulkan}/queue.c" "${vulkan}/later.c" "${gen}/vulkan_stubs.S" "${gen}/vulkan_bind.c")
target_include_directories(vulkan PRIVATE "${APK_MOLTENVK_DIR}/include" "${gen}")
target_link_libraries(vulkan PRIVATE minato_bind)
target_link_options(vulkan PRIVATE "-Wl,--version-script=${gen}/libvulkan.map" -Wl,-Bsymbolic)
set_target_properties(vulkan PROPERTIES LINK_DEPENDS "${gen}/libvulkan.map"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_options(vulkan PRIVATE -Wall -Wextra)

# ADR-0030, test 6 without the window: vulkan-fake-test against graphics/tests/fake_vulkan.c in MoltenVK's
# place, vulkan-moltenvk-test against MoltenVK itself.
foreach(test IN ITEMS vulkan_fake_test vulkan_moltenvk_test vulkan_queue_test)
  string(REPLACE "_" "-" program ${test})
  add_executable(${program} "${tests}/${test}.c")
  target_include_directories(${program} PRIVATE "${APK_MOLTENVK_DIR}/include")
  target_link_libraries(${program} PRIVATE vulkan dl)
  target_compile_options(${program} PRIVATE -Wall -Wextra)
endforeach()
