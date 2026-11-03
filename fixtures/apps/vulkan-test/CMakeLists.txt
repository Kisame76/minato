# vulkan-test (ADR-0030, test 7; step V5): twelve fixed Vulkan scenes read back and logged,
# then 120 frames, the copy's measurements and ANativeActivity_finish. Added by fixtures/CMakeLists.txt, which
# defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it. The
# NDK's glslc compiles the shaders into C initializers that main.c includes.
set(glslc "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
set(shaders flat.vert flat.frag arithmetic.frag texel.frag compressed.frag instances.vert instances.frag
  compute.comp show.frag bits.frag)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(compiled "")
foreach(shader IN LISTS shaders)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.inc")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${glslc}" -mfmt=c -o "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
    DEPENDS "shaders/${shader}" shaders/push.glsl
    COMMENT "glslc: vulkan-test's ${shader}")
  list(APPEND compiled "${output}")
endforeach()
add_library(vulkan-test SHARED main.c ${compiled})
target_include_directories(vulkan-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_options(vulkan-test PRIVATE -Wall -Wextra)
target_link_options(vulkan-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(vulkan-test PRIVATE native_app_glue vulkan)
# vulkan-live (ADR-0032, test 8; ADR-0036 a): the same library under the name fixtures/apps/vulkan-live's
# manifest gives it, whose android.app.func_name starts it at ANativeActivity_onCreateLive.
add_custom_command(TARGET vulkan-test POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:vulkan-test>" "$<TARGET_FILE_DIR:vulkan-test>/libvulkan-live.so"
  COMMENT "vulkan-live: libvulkan-test.so copied")
