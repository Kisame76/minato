# gles-test (ADR-0029, test 8; step B2), the Grafik chat's: twelve fixed GLES 3.0 scenes read back and logged,
# then 120 frames and ANativeActivity_finish. Added by fixtures/CMakeLists.txt, which defines native_app_glue;
# the glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
add_library(gles-test SHARED main.c)
target_compile_options(gles-test PRIVATE -Wall -Wextra)
target_link_options(gles-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(gles-test PRIVATE native_app_glue EGL GLESv3)
