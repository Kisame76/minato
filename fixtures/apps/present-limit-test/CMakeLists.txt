# present-limit-test (ADR-0078 decision 4): one library that presents without pacing of its own, with EGL at a swap
# interval of 0 or with a FIFO swapchain in a loop, and logs its presents a second. It goes into two apps under their
# own names: fixtures/apps/present-limit-gles's manifest starts it at ANativeActivity_onCreateGLES,
# fixtures/apps/present-limit-vulkan's at ANativeActivity_onCreateVulkan. Added by fixtures/CMakeLists.txt, which
# defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it.
add_library(present-limit-test SHARED main.c)
target_compile_options(present-limit-test PRIVATE -Wall -Wextra)
target_link_options(present-limit-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(present-limit-test PRIVATE native_app_glue EGL GLESv3 vulkan log)
add_custom_command(TARGET present-limit-test POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:present-limit-test>"
    "$<TARGET_FILE_DIR:present-limit-test>/libpresent-limit-gles.so"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:present-limit-test>"
    "$<TARGET_FILE_DIR:present-limit-test>/libpresent-limit-vulkan.so"
  COMMENT "present-limit-gles and present-limit-vulkan: libpresent-limit-test.so copied")
