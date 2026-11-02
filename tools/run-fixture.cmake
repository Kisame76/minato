# Runs one NDK fixture through the whole chain (ADR-0019, ADR-0021): apk-convert writes the
# containers of the program, of its own libraries and of the system's and signs them ad hoc with
# its own signer (ADR-0022), and apk-run runs the program in a process of its own, the way
# Android's shell runs it:
#   cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./<program>
# The program and its own libraries lie in /data/local/tmp for the linker (<name>.dylib next to
# the program's container), the system's libraries in /system/lib64. For files, /data/local/tmp is
# an empty host directory for each run, SYSTEM, when it exists, is /system, and the CA certificates
# of build/sources/ca-certificates, when fetched, are Conscrypt's /apex/com.android.conscrypt/cacerts,
# where the runtime's link /system/etc/security/cacerts leads (ADR-0046 decision 4).
# Passes when the program prints exactly the contents of EXPECTED_OUTPUT on stdout and of
# EXPECTED_ERROR on stderr (nothing when not given), and exits with EXPECTED_STATUS (0 if not given).
# NO_LIBRARY_PATH leaves LD_LIBRARY_PATH out; TEST names the run when it is not the program's name.
# MEASURES marks a benchmark: its stdout is not compared, and EXPECTED_OUTPUT is not read.
# HOST_LINES is a regular expression for the lines of stderr that RUN writes on a thread of its own,
# which the program does not wait for (graphics-run's Metal hook, as Metal presents drawables): they
# race with the program's lines, so the lines that match it and the others are compared apart, each
# kind in its order.
#   cmake -D CONVERT=<apk-convert> -D RUN=<apk-run> -D FIXTURE=<elf> -D WORK=<dir>
#         -D EXPECTED_OUTPUT=<file> [-D EXPECTED_ERROR=<file>] [-D EXPECTED_STATUS=<status>]
#         [-D LIBRARIES=<elf>;<elf>...] [-D SYSTEM_LIBRARIES=<elf>;<elf>...] [-D SYSTEM=<dir>]
#         [-D NO_LIBRARY_PATH=TRUE] [-D MEASURES=TRUE] [-D TEST=<name>] [-D HOST_LINES=<regex>]
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
set(certificates "${CMAKE_CURRENT_LIST_DIR}/../build/sources/ca-certificates/files")
if(IS_DIRECTORY "${certificates}")
  list(APPEND options --mount "/apex/com.android.conscrypt/cacerts=${certificates}")
endif()
# apk-run logs the program's end as Android's Zygote does ("I/Zygote  : Process <pid> exited ..."), a line
# that on Android comes from the zygote process, not the app: leave it out of the stderr we compare, the way
# the expected output leaves out other processes' lines. The line itself stays; only the comparison drops it.
function(without_zygote text out)
  set(kept "")
  while(NOT text STREQUAL "")
    string(FIND "${text}" "\n" end)
    if(end EQUAL -1)
      string(LENGTH "${text}" end)
    else()
      math(EXPR end "${end} + 1")
    endif()
    string(SUBSTRING "${text}" 0 ${end} line)
    string(SUBSTRING "${text}" ${end} -1 text)
    if(NOT line MATCHES "^I/Zygote  : Process ")
      string(APPEND kept "${line}")
    endif()
  endwhile()
  set(${out} "${kept}" PARENT_SCOPE)
endfunction()

execute_process(COMMAND "${RUN}" ${options} "${container}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
without_zygote("${error}" error)
# A program that abort() ends: CMake names the signal, a shell shows 128 + SIGABRT (6), as Android's does; one that
# SIGKILL ends, 128 + SIGKILL (9).
if(status STREQUAL "Subprocess aborted")
  set(status 134)
elseif(status STREQUAL "Subprocess killed")
  set(status 137)
endif()
message("exit status: ${status}\nstdout: [${output}]\nstderr: [${error}]")
if(NOT status EQUAL EXPECTED_STATUS)
  message(FATAL_ERROR "${TEST} exited with ${status}, expected ${EXPECTED_STATUS}")
endif()
if(NOT MEASURES AND NOT output STREQUAL expected_output)
  message(FATAL_ERROR "${TEST} printed [${output}], expected [${expected_output}]")
endif()

# The lines of `text` that match HOST_LINES into `host`, the others into `program`, in their order.
function(split_lines text host program)
  set(matching "")
  set(others "")
  while(NOT text STREQUAL "")
    string(FIND "${text}" "\n" end)
    if(end EQUAL -1)
      string(LENGTH "${text}" end)
    else()
      math(EXPR end "${end} + 1")
    endif()
    string(SUBSTRING "${text}" 0 ${end} line)
    string(SUBSTRING "${text}" ${end} -1 text)
    if(line MATCHES "${HOST_LINES}")
      string(APPEND matching "${line}")
    else()
      string(APPEND others "${line}")
    endif()
  endwhile()
  set(${host} "${matching}" PARENT_SCOPE)
  set(${program} "${others}" PARENT_SCOPE)
endfunction()

# With HOST_LINES, each kind in its order: whether the program or RUN's thread writes first is a race.
if(HOST_LINES)
  split_lines("${error}" host_error program_error)
  split_lines("${expected_error}" expected_host_error expected_program_error)
  if(NOT program_error STREQUAL expected_program_error)
    message(FATAL_ERROR "${TEST} wrote [${program_error}] to stderr besides the lines of ${HOST_LINES}, "
      "expected [${expected_program_error}]")
  endif()
  if(NOT host_error STREQUAL expected_host_error)
    message(FATAL_ERROR "${TEST}'s lines of ${HOST_LINES} on stderr were [${host_error}], "
      "expected [${expected_host_error}]")
  endif()
elseif(NOT error STREQUAL expected_error)
  message(FATAL_ERROR "${TEST} wrote [${error}] to stderr, expected [${expected_error}]")
endif()
