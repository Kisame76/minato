# audio-input-test (ADR-0064): AAudio input streams and OpenSL ES recorders as Minato offers them, logged and compared
# with Android 15, without and with RECORD_AUDIO's grant (the twin audio-input-test-granted, from the same main.c).
# Added by fixtures/CMakeLists.txt, which defines native_app_glue; the glue's ANativeActivity_onCreate is only
# referenced by Java, so -u keeps it in the library.
add_library(audio-input-test SHARED main.c)
target_compile_definitions(audio-input-test PRIVATE TEST_TAG="audio-input-test")
target_compile_options(audio-input-test PRIVATE -Wall -Wextra)
target_link_options(audio-input-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(audio-input-test PRIVATE native_app_glue aaudio OpenSLES)
