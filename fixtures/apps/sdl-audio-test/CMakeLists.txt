# sdl-audio-test: the call sequences of SDL 2.30's AAudio and OpenSL ES drivers, played with silence. Added by
# fixtures/CMakeLists.txt, which defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced
# by Java, so -u keeps it in the library.
add_library(sdl-audio-test SHARED main.c)
target_compile_options(sdl-audio-test PRIVATE -Wall -Wextra)
target_link_options(sdl-audio-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(sdl-audio-test PRIVATE native_app_glue aaudio OpenSLES)
