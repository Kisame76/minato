# Runs one NDK fixture through the whole chain (ADR-0019, ADR-0021): apk-convert writes the
# containers of the program, of its own libraries and of the system's and signs them ad hoc with
# its own signer (ADR-0022), and apk-run runs the program in a process of its own, the way
# Android's shell runs it:
#   cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./<program>
# The program and its own libraries lie in /data/local/tmp for the linker (<name>.dylib next to
# the program's container), the system's libraries in /system/lib64. For files, /data/local/tmp is
# an empty host directory for each run, and SYSTEM, when it exists, is /system.
# Passes when the program prints exactly the contents of EXPECTED_OUTPUT on stdout and of
# EXPECTED_ERROR on stderr (nothing when not given), and exits with EXPECTED_STATUS (0 if not given).
# NO_LIBRARY_PATH leaves LD_LIBRARY_PATH out; TEST names the run when it is not the program's name.
# MEASURES marks a benchmark: its stdout is not compared, and EXPECTED_OUTPUT is not read.
#   cmake -D CONVERT=<apk-convert> -D RUN=<apk-run> -D FIXTURE=<elf> -D WORK=<dir>
#         -D EXPECTED_OUTPUT=<file> [-D EXPECTED_ERROR=<file>] [-D EXPECTED_STATUS=<status>]
#         [-D LIBRARIES=<elf>;<elf>...] [-D SYSTEM_LIBRARIES=<elf>;<elf>...] [-D SYSTEM=<dir>]
#         [-D NO_LIBRARY_PATH=TRUE] [-D MEASURES=TRUE] [-D TEST=<name>]
#         -P tools/run-fixture.cmake
foreach(elf IN LISTS FIXTURE LIBRARIES SYSTEM_LIBRARIES)
  if(NOT EXISTS "${elf}")
    message("skipped: ${elf} is not built, see tools/build-fixtures.sh or tools/build-bionic.sh")
    return()
  endif()
endforeach()

get_filename_component(name "${FIXTURE}" NAME)
if(NOT DEFINED TEST)
  set(TEST "${name}")
endif()
if(NOT DEFINED EXPECTED_STATUS)
  set(EXPECTED_STATUS 0)
endif()
# A fresh directory for each run: a container left over from another set-up would be found first.
set(work "${WORK}/${TEST}")
set(system_libraries "${work}/system-lib64")
set(tmp "${work}/data-local-tmp")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}" "${system_libraries}" "${tmp}")
set(container "${work}/${name}.dylib")

function(convert elf directory)
  get_filename_component(file "${elf}" NAME)
  execute_process(COMMAND "${CONVERT}" convert "${elf}" "${directory}/${file}.dylib" --sign -
    RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "apk-convert ${file} failed (${status}): ${error}")
  endif()
  message("${report}")
endfunction()

foreach(elf IN LISTS FIXTURE LIBRARIES)
  convert("${elf}" "${work}")
endforeach()
foreach(elf IN LISTS SYSTEM_LIBRARIES)
  convert("${elf}" "${system_libraries}")
endforeach()

if(NOT MEASURES)
  file(READ "${EXPECTED_OUTPUT}" expected_output)
endif()
set(expected_error "")
if(DEFINED EXPECTED_ERROR)
  file(READ "${EXPECTED_ERROR}" expected_error)
endif()

set(options --mount "/data/local/tmp=${tmp}" --libraries "/system/lib64=${system_libraries}"
  --cwd /data/local/tmp --argv0 "./${name}")
if(NOT NO_LIBRARY_PATH)
  list(APPEND options --env LD_LIBRARY_PATH=/data/local/tmp)
endif()
if(DEFINED SYSTEM AND IS_DIRECTORY "${SYSTEM}")
  list(APPEND options --mount "/system=${SYSTEM}")
endif()
execute_process(COMMAND "${RUN}" ${options} "${container}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
message("exit status: ${status}\nstdout: [${output}]\nstderr: [${error}]")
if(NOT status EQUAL EXPECTED_STATUS)
  message(FATAL_ERROR "${TEST} exited with ${status}, expected ${EXPECTED_STATUS}")
endif()
if(NOT MEASURES AND NOT output STREQUAL expected_output)
  message(FATAL_ERROR "${TEST} printed [${output}], expected [${expected_output}]")
endif()
if(NOT error STREQUAL expected_error)
  message(FATAL_ERROR "${TEST} wrote [${error}] to stderr, expected [${expected_error}]")
endif()
