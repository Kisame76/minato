# The host aidl (ADR-0045 decision 1): AOSP's aidl binary from build/sources/aidl, as its Android.bp
# builds it (libaidl-common and main.cpp), with libbase from build/sources/libbase, fmtlib, and
# tools/aidl/liblog_host.cpp in place of liblog. Its tests, fuzzers, analyzer and gtest stay out; only
# --lang=java runs. tools/build-aidl.sh configures and builds it into build/aidl.
cmake_minimum_required(VERSION 3.24)
project(minato_aidl LANGUAGES CXX)

get_filename_component(MINATO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SOURCES "${MINATO_ROOT}/build/sources")
set(AIDL "${SOURCES}/aidl")
if(NOT EXISTS "${AIDL}/aidl_language_y.yy")
  message(FATAL_ERROR "No ${AIDL}: run tools/fetch-sources.py --set framework")
endif()

# aidl's grammar is GLR with `%define parse.error`, which needs Bison 3; macOS ships 2.3 in /usr/bin.
# Find it without assuming this Mac: an explicit -DBISON_EXECUTABLE or the BISON environment variable wins;
# otherwise Homebrew's bison (its own prefix from `brew --prefix`, or the Apple Silicon and Intel defaults)
# comes before the PATH, so the search does not settle on macOS's 2.3. find_package(BISON 3.0) then refuses
# anything older with a clear message.
if(NOT BISON_EXECUTABLE AND DEFINED ENV{BISON})
  set(BISON_EXECUTABLE "$ENV{BISON}" CACHE FILEPATH "GNU Bison 3 for aidl's parser" FORCE)
endif()
if(NOT BISON_EXECUTABLE)
  set(_bison_hints /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin)
  find_program(_brew brew)
  if(_brew)
    execute_process(COMMAND "${_brew}" --prefix bison
                    OUTPUT_VARIABLE _brew_bison OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _brew_rc)
    if(_brew_rc EQUAL 0 AND IS_DIRECTORY "${_brew_bison}/bin")
      list(PREPEND _bison_hints "${_brew_bison}/bin")
    endif()
  endif()
  find_program(BISON_EXECUTABLE bison HINTS ${_bison_hints})
endif()
find_package(BISON 3.0)
if(NOT BISON_FOUND)
  message(FATAL_ERROR
    "aidl's parser needs GNU Bison 3 (macOS ships 2.3 in /usr/bin), but found "
    "'${BISON_EXECUTABLE}' version '${BISON_VERSION}'. Install a newer Bison "
    "(HOMEBREW_NO_AUTO_UPDATE=1 brew install bison) or point to it with "
    "-DBISON_EXECUTABLE=/path/to/bison or the BISON environment variable.")
endif()
find_package(FLEX REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
BISON_TARGET(aidl_parser "${AIDL}/aidl_language_y.yy" "${generated}/aidl_language_y.cpp"
             DEFINES_FILE "${generated}/aidl_language_y.h")
FLEX_TARGET(aidl_lexer "${AIDL}/aidl_language_l.ll" "${generated}/aidl_language_l.cpp")
ADD_FLEX_BISON_DEPENDENCY(aidl_lexer aidl_parser)

# libbase_defaults' sources for darwin, without test_utils.cpp, which only tests use.
set(base_sources
  abi_compatibility.cpp chrono_utils.cpp cmsg.cpp errors_unix.cpp file.cpp hex.cpp logging.cpp mapped_file.cpp
  parsebool.cpp parsenetaddress.cpp posix_strerror_r.cpp process.cpp properties.cpp result.cpp stringprintf.cpp
  strings.cpp threads.cpp)
list(TRANSFORM base_sources PREPEND "${SOURCES}/libbase/")
# fmtlib's format.cc, which Soong links into libbase as the static fmtlib.
add_library(base STATIC ${base_sources} "${SOURCES}/fmtlib/src/format.cc")
target_include_directories(base PUBLIC "${SOURCES}/libbase/include" "${SOURCES}/fmtlib/include"
                           "${SOURCES}/logging/liblog/include")
# libbase_cflags_defaults. The Mac's clang warns of the variable length arrays in cmsg.cpp and
# logging_splitters.h, which AOSP's own clang build accepts.
target_compile_options(base PRIVATE -Wall -Werror -Wextra -Wexit-time-destructors -Wno-vla-cxx-extension)
# fmt's format strings are checked at run time, as in runtime/art/CMakeLists.txt: fmt 10.2's compile-time
# check fails under the Mac's clang, which no longer folds two equal string literals into one object.
target_compile_definitions(base PUBLIC _FILE_OFFSET_BITS=64 FMT_CONSTEVAL=)

add_library(log STATIC liblog_host.cpp)
target_include_directories(log PRIVATE "${SOURCES}/logging/liblog/include")
target_compile_options(log PRIVATE -Wall -Werror -Wextra)

# libaidl-common's sources, then main.cpp.
set(aidl_sources
  aidl_checkapi.cpp aidl_const_expressions.cpp aidl_dumpapi.cpp aidl_language.cpp aidl_to_common.cpp
  aidl_to_cpp_common.cpp aidl_to_cpp.cpp aidl_to_java.cpp aidl_to_ndk.cpp aidl_to_rust.cpp aidl_typenames.cpp aidl.cpp
  ast_java.cpp check_valid.cpp code_writer.cpp comments.cpp diagnostics.cpp generate_aidl_mappings.cpp generate_cpp.cpp
  generate_cpp_analyzer.cpp generate_java_binder.cpp generate_java.cpp generate_ndk.cpp generate_rust.cpp
  import_resolver.cpp io_delegate.cpp location.cpp logging.cpp options.cpp parser.cpp permission.cpp preprocess.cpp
  main.cpp)
list(TRANSFORM aidl_sources PREPEND "${AIDL}/")
add_executable(aidl ${aidl_sources} ${BISON_aidl_parser_OUTPUTS} ${FLEX_aidl_lexer_OUTPUTS})
# tools/aidl/gtest stands in for the one gtest helper aidl_checkapi.cpp includes.
target_include_directories(aidl PRIVATE "${AIDL}" "${generated}" "${CMAKE_CURRENT_SOURCE_DIR}")
# aidl_defaults. The Mac's clang also warns of a variable length array in options.cpp and of a copying
# range loop in generate_cpp.cpp (the C++ backend), which AOSP's own clang build accepts.
target_compile_options(aidl PRIVATE -Wall -Werror -Wextra -Wno-vla-cxx-extension -Wno-range-loop-construct)
target_link_libraries(aidl PRIVATE base log)
