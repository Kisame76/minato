# rotation-test (ADR-0038, test 3): a turn as a NativeActivity that handles it itself sees it, logged and compared
# with Android 15. Added by fixtures/CMakeLists.txt, which defines native_app_glue; the glue's
# ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
add_library(rotation-test SHARED main.c)
target_compile_options(rotation-test PRIVATE -Wall -Wextra)
target_link_options(rotation-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(rotation-test PRIVATE native_app_glue)
