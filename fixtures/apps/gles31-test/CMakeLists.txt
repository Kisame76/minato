# gles31-test (ADR-0044, test 2), the Grafik chat's: four fixed GLES 3.1 scenes (a compute shader, a draw that
# reads what it wrote, flat values per vertex and a triangle fan) checked against their exact values and logged,
# then 120 frames and ANativeActivity_finish. Added by fixtures/CMakeLists.txt, which defines native_app_glue; the
# glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
add_library(gles31-test SHARED main.c)
target_compile_options(gles31-test PRIVATE -Wall -Wextra)
target_link_options(gles31-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(gles31-test PRIVATE native_app_glue EGL GLESv3)
