# aaudio-test (ADR-0036, step c): AAudio output as Minato offers it, logged and compared with Android 15. Added by
# fixtures/CMakeLists.txt, which defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced
# by Java, so -u keeps it in the library.
add_library(aaudio-test SHARED main.c)
target_compile_options(aaudio-test PRIVATE -Wall -Wextra)
target_link_options(aaudio-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(aaudio-test PRIVATE native_app_glue aaudio)
