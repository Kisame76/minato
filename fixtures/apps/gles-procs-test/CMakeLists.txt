# gles-procs-test as an app: graphics/tests/gles_procs_test.c as a NativeActivity, whose lines go to the log, for the
# iPhone (tools/java-start-check.sh --device). Its names come from gles_procs.py, as for the shell program. Added by
# fixtures/CMakeLists.txt, which defines native_app_glue; the glue's ANativeActivity_onCreate is only referenced by
# Java, so -u keeps it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../graphics/tests")
set(procs "${CMAKE_CURRENT_BINARY_DIR}/gles_procs")
add_custom_command(OUTPUT "${procs}/gles_procs_candidates.inc"
  COMMAND Python3::Interpreter "${tests}/gles_procs.py" --ndk "${ANDROID_NDK}" --candidates
    --out "${procs}/gles_procs_candidates.inc"
  DEPENDS "${tests}/gles_procs.py"
  COMMENT "graphics/tests/gles_procs.py: the NDK's GLES, EGL and Vulkan functions")
add_custom_command(OUTPUT "${procs}/gles_procs_required.inc"
  COMMAND Python3::Interpreter "${tests}/gles_procs.py" --ndk "${ANDROID_NDK}"
    --required "${tests}/gles_procs_android.txt" --out "${procs}/gles_procs_required.inc"
  DEPENDS "${tests}/gles_procs.py" "${tests}/gles_procs_android.txt"
  COMMENT "graphics/tests/gles_procs.py: Android 15's measurement")
add_library(gles-procs-test SHARED main.c "${procs}/gles_procs_candidates.inc" "${procs}/gles_procs_required.inc")
target_include_directories(gles-procs-test PRIVATE "${tests}" "${procs}")
set_source_files_properties(main.c PROPERTIES OBJECT_DEPENDS "${tests}/gles_procs_test.c")
target_compile_options(gles-procs-test PRIVATE -Wall -Wextra)
target_link_options(gles-procs-test PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(gles-procs-test PRIVATE native_app_glue EGL dl log)
