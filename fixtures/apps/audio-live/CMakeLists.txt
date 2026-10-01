# audio-live (ADR-0036, after M3): a guest stream that plays until the user quits, and its error callback, logged
# and compared with Android 15. Added by fixtures/CMakeLists.txt, which defines native_app_glue; the glue's
# ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
add_library(audio-live SHARED main.c)
target_compile_options(audio-live PRIVATE -Wall -Wextra)
target_link_options(audio-live PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(audio-live PRIVATE native_app_glue aaudio m)
