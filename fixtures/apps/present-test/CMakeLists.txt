# present-test (ADR-0041 decision 11, test 3; step d): one library that asks for its presents'
# times with EGL_ANDROID_presentation_time and with VK_GOOGLE_display_timing, then ANativeActivity_finish. It goes
# into two apps under their own names: fixtures/apps/present-gles's manifest starts it at
# ANativeActivity_onCreateGLES, fixtures/apps/present-vulkan's at ANativeActivity_onCreateVulkan. Added by
# fixtures/CMakeLists.txt, which defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced by
# Java, so -u keeps it in the library.
add_library(present-test SHARED main.c)
target_compile_options(present-test PRIVATE -Wall -Wextra)
target_link_options(present-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(present-test PRIVATE native_app_glue EGL GLESv3 vulkan)
add_custom_command(TARGET present-test POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:present-test>" "$<TARGET_FILE_DIR:present-test>/libpresent-gles.so"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:present-test>"
    "$<TARGET_FILE_DIR:present-test>/libpresent-vulkan.so"
  COMMENT "present-gles and present-vulkan: libpresent-test.so copied")
