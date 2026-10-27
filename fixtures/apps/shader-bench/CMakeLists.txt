# shader-bench (M6, the shader cache plan): builds 40 fixed and 20 fresh GLSL ES 3.0 programs, logs the time of their
# links and first draws, then ANativeActivity_finish. Added by fixtures/CMakeLists.txt, which defines native_app_glue;
# the glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
add_library(shader-bench SHARED main.c)
target_compile_options(shader-bench PRIVATE -Wall -Wextra)
target_link_options(shader-bench PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(shader-bench PRIVATE native_app_glue EGL GLESv3)
