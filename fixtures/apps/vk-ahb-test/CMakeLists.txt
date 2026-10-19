# vk-ahb-test (ADR-0058 e1), decision 7), the Grafik chat's: graphics/tests/vk_ahb_test.c as a NativeActivity app,
# whose lines go to the log; the NDK's glslc compiles its compute shader into a C initializer that it includes. Added
# by fixtures/CMakeLists.txt, which defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced by
# Java, so -u keeps it. API 29's AHardwareBuffer functions are all it calls.
set(tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../graphics/tests")
set(shader "${CMAKE_CURRENT_BINARY_DIR}/shaders/vk_ahb_test.comp.inc")
add_custom_command(OUTPUT "${shader}"
  COMMAND "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc" -mfmt=c -o "${shader}" "${tests}/vk_ahb_test.comp"
  DEPENDS "${tests}/vk_ahb_test.comp"
  COMMENT "glslc: vk-ahb-test's compute shader")
add_library(vk-ahb-test SHARED main.c "${shader}")
target_include_directories(vk-ahb-test PRIVATE "${tests}" "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set_source_files_properties(main.c PROPERTIES OBJECT_DEPENDS "${tests}/vk_ahb_test.c")
target_compile_options(vk-ahb-test PRIVATE -Wall -Wextra)
target_link_options(vk-ahb-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(vk-ahb-test PRIVATE native_app_glue vulkan nativewindow log)
