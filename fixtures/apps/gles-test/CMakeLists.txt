# gles-test (ADR-0029, test 8; step B2), the Grafik chat's: twelve fixed GLES 3.0 scenes read back and logged,
# then 120 frames and ANativeActivity_finish. Added by fixtures/CMakeLists.txt, which defines native_app_glue;
# the glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
add_library(gles-test SHARED main.c)
target_compile_options(gles-test PRIVATE -Wall -Wextra)
target_link_options(gles-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(gles-test PRIVATE native_app_glue EGL GLESv3)
# gles-live (ADR-0032, test 8; ADR-0036 a): the same library under the name fixtures/apps/gles-live's manifest
# gives it, whose android.app.func_name starts it at ANativeActivity_onCreateLive.
add_custom_command(TARGET gles-test POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:gles-test>" "$<TARGET_FILE_DIR:gles-test>/libgles-live.so"
  COMMENT "gles-live: libgles-test.so copied")
