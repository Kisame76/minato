# The Vulkan scene's shaders (ADR-0037), compiled by the NDK's glslc into C initializers that vulkan.c includes: the
# same compiler for the APKs (android/CMakeLists.txt) and the host build (CMakeLists.txt), so both give MoltenVK the
# same SPIR-V. bench_shaders(<glslc> <out>) sets <out> to the generated files, in ${CMAKE_CURRENT_BINARY_DIR}/shaders.
function(bench_shaders glslc out)
  set(sources "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders")
  set(compiled "")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  foreach(shader IN ITEMS fullscreen.vert background.frag fill.frag mesh.vert mesh.frag particle.vert particle.frag
                          post.frag)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.inc")
    add_custom_command(OUTPUT "${output}"
      COMMAND "${glslc}" -O -mfmt=c -o "${output}" "${sources}/${shader}"
      DEPENDS "${sources}/${shader}"
      COMMENT "glslc: the bench's ${shader}")
    list(APPEND compiled "${output}")
  endforeach()
  set(${out} "${compiled}" PARENT_SCOPE)
endfunction()
