# audio-input-test-granted (ADR-0064): the twin of audio-input-test, from its main.c; the harness grants RECORD_AUDIO
# before the start. Its lines carry its own name as the tag.
add_library(audio-input-test-granted SHARED ../audio-input-test/main.c)
target_compile_definitions(audio-input-test-granted PRIVATE TEST_TAG="audio-input-test-granted")
target_compile_options(audio-input-test-granted PRIVATE -Wall -Wextra)
target_link_options(audio-input-test-granted PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(audio-input-test-granted PRIVATE native_app_glue aaudio OpenSLES)
