# NDK test programs for the loader (M2), built for arm64-v8a by tools/build-fixtures.sh.
# Static programs first (M2 step 4), then dynamically linked ones (step 5).
cmake_minimum_required(VERSION 4.0)
project(MinatoFixtures LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build the fixtures with tools/build-fixtures.sh (Android NDK, arm64-v8a).")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Static PIEs without an interpreter: the loader places them itself. A fixed address, as -static
# produces, is no option, because below 4 GiB nothing can be mapped on iOS (ADR-0012).

# raw-hello: Linux system calls only (svc #0). No libc, no TLS, no relocations.
add_executable(raw-hello static/raw-hello.S)
target_link_options(raw-hello PRIVATE -static-pie -nostdlib)

# hello: printf from the static NDK libc, with its start-up code: TLS set-up (msr TPIDR_EL0),
# stack protector (mrs TPIDR_EL0) and many system calls. Android itself cannot run it: bionic's
# static start code applies no relocations, so a static PIE crashes there (SIGSEGV).
add_executable(hello static/hello.c)
target_link_options(hello PRIVATE -static-pie)

# raw-dyn and libraw-dyn.so: dynamic linking without a libc (M2 step 5a). No stack protector and no
# builtins, because nothing provides __stack_chk_guard, strlen or memcpy; --as-needed drops the libm
# that the toolchain file links to every target. The library keeps semantic interposition, so its
# call of raw_dyn_greeting goes through its PLT and the program can override it.
set(raw_options -ffreestanding -fno-builtin -fno-stack-protector)
add_library(raw-dyn-lib SHARED dynamic/raw-dyn-lib.c)
set_target_properties(raw-dyn-lib PROPERTIES OUTPUT_NAME raw-dyn)
target_compile_options(raw-dyn-lib PRIVATE ${raw_options} -fsemantic-interposition)
target_link_options(raw-dyn-lib PRIVATE -nostdlib -Wl,--as-needed)
add_executable(raw-dyn dynamic/raw-dyn.c)
target_compile_options(raw-dyn PRIVATE ${raw_options})
target_link_libraries(raw-dyn PRIVATE raw-dyn-lib)
target_link_options(raw-dyn PRIVATE -nostdlib -Wl,--as-needed -Wl,--export-dynamic-symbol=raw_dyn_greeting)

# The same library with only a SysV hash table (DT_HASH), as older NDKs built for API < 23. The
# linker's symbol lookup is tested on both kinds of table (runtime.linker).
add_library(raw-dyn-sysv SHARED dynamic/raw-dyn-lib.c)
target_compile_options(raw-dyn-sysv PRIVATE ${raw_options} -fsemantic-interposition)
target_link_options(raw-dyn-sysv PRIVATE -nostdlib -Wl,--as-needed -Wl,--hash-style=sysv)

# hello-dyn: printf from libc.so, as every NDK program uses it (M2 step 5b). At run time the runtime
# gives it bionic's libc.so, libm.so and libdl.so, built by tools/build-bionic.sh.
add_executable(hello-dyn dynamic/hello-dyn.c)

# The test suite of M2 step 5c: one program per area, each printing lines that C, POSIX and bionic's
# sources fix exactly, so that Android prints the same (fixtures/expected/<name>.out).
add_executable(libc-test suite/libc-test.c)
add_executable(files-test suite/files-test.c)
# Without builtins, so that clang does not fold the math calls at compile time.
add_executable(math-test suite/math-test.c)
target_compile_options(math-test PRIVATE -fno-builtin)
target_link_libraries(math-test PRIVATE m)
# The logger functions of liblog came with API 30.
add_executable(log-test suite/log-test.c)
target_compile_options(log-test PRIVATE --target=aarch64-none-linux-android30)
target_link_options(log-test PRIVATE --target=aarch64-none-linux-android30)
target_link_libraries(log-test PRIVATE log)
add_executable(thread-test suite/thread-test.c)
add_library(tls-lib SHARED suite/libtls-lib.c)
add_executable(tls-test suite/tls-test.c)
target_link_libraries(tls-test PRIVATE tls-lib)
# dl-test and the libraries it opens at run time; libdl-missing.so is built for libdl-broken.so to
# link against, but never installed next to dl-test.
add_library(dl-helper SHARED suite/libdl-helper.c)
add_library(dl-plugin SHARED suite/libdl-plugin.c)
target_link_libraries(dl-plugin PRIVATE dl-helper)
add_library(dl-global SHARED suite/libdl-global.c)
add_library(dl-missing SHARED suite/libdl-missing.c)
add_library(dl-broken SHARED suite/libdl-broken.c)
target_link_libraries(dl-broken PRIVATE dl-missing)
add_library(dl-unresolved SHARED suite/libdl-unresolved.c)
target_link_options(dl-unresolved PRIVATE -Wl,-z,undefs)
add_executable(dl-test suite/dl-test.c)
target_link_libraries(dl-test PRIVATE dl)
target_link_options(dl-test PRIVATE -Wl,--export-dynamic-symbol=dl_test_exported)
add_executable(signal-test suite/signal-test.c)
# dl-lock-test and the library whose constructor holds the dynamic linker's lock (ADR-0025, addition d).
add_library(dl-lock SHARED suite/libdl-lock.c)
target_link_libraries(dl-lock PRIVATE dl)
add_executable(dl-lock-test suite/dl-lock-test.c)
target_link_libraries(dl-lock-test PRIVATE dl)
# fd-test: descriptor numbers as Linux hands them out, whatever the process holds on the host (ADR-0027).
add_executable(fd-test suite/fd-test.c)
# poll-test: eventfd, epoll, poll and select, as Android's looper uses them (ADR-0027).
add_executable(poll-test suite/poll-test.c)
# memory-test: every size class of the allocator, a 2 GiB window at a 4 GiB boundary and RLIMIT_STACK
# (ADR-0035).
add_executable(memory-test suite/memory-test.c)
# start-test: ifunc resolvers and constructors run on the program's main thread (ADR-0028, decision 5).
add_executable(start-test suite/start-test.c)
# looper-test and configuration-test: ALooper and AConfiguration of libandroid.so (ADR-0028, decision 9).
# The configuration's newest getters came with API 34.
add_executable(looper-test suite/looper-test.c)
target_link_libraries(looper-test PRIVATE android)
add_executable(configuration-test suite/configuration-test.c)
target_compile_options(configuration-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(configuration-test PRIVATE --target=aarch64-none-linux-android35)
target_link_libraries(configuration-test PRIVATE android)
# libandroid-test: the window of libnativewindow.so as activity-host makes it, libandroid's forwards and
# its generated stubs (ADR-0028, decisions 7 and 9). libnativewindow comes first, so that the program's
# ANativeWindow and AHardwareBuffer functions are its own.
add_executable(libandroid-test suite/libandroid-test.c)
target_include_directories(libandroid-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
target_compile_options(libandroid-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(libandroid-test PRIVATE --target=aarch64-none-linux-android35)
target_link_libraries(libandroid-test PRIVATE nativewindow android dl)
# asset-test: libandroid's asset manager on archives it writes with zlib, and AConfiguration_fromAssetManager
# (ADR-0028, decisions 9 and 10).
add_executable(asset-test suite/asset-test.c)
target_include_directories(asset-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
target_compile_options(asset-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(asset-test PRIVATE --target=aarch64-none-linux-android35)
target_link_libraries(asset-test PRIVATE android z dl)
# jni-test: activity-host's JNI stub, from its own source (ADR-0028, decision 8), without the C++ library,
# which activity-host does not have either.
add_executable(jni-test suite/jni-test.c ../runtime/activity-host/jni_stub.cpp)
target_include_directories(jni-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/activity-host")
target_compile_options(jni-test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20 -fno-exceptions -fno-rtti>)
target_link_options(jni-test PRIVATE -nostdlib++)
target_link_libraries(jni-test PRIVATE log)
# jni-fatal-test: the stub's FatalError, which ends the program.
add_executable(jni-fatal-test suite/jni-fatal-test.c ../runtime/activity-host/jni_stub.cpp)
target_include_directories(jni-fatal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/activity-host")
target_compile_options(jni-fatal-test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20 -fno-exceptions -fno-rtti>)
target_link_options(jni-fatal-test PRIVATE -nostdlib++)
target_link_libraries(jni-fatal-test PRIVATE log)
# The test apps of M3 (ADR-0028, decision 15): NativeActivity libraries on native_app_glue from the NDK's
# sources, which tools/build-fixtures.sh packs into build/fixtures/apks/<app>.apk with tools/make-apk.sh.
# The glue's ANativeActivity_onCreate is only referenced by Java, so -u keeps it in the library.
set(app_glue "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(native_app_glue STATIC "${app_glue}/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${app_glue}")
target_link_libraries(native_app_glue PUBLIC android log)
# activity-test (test 3): its lifecycle, the JNI stub, assets and the configuration; the newest
# configuration getters came with API 34.
add_library(activity-test SHARED apps/activity-test/main.c)
target_include_directories(activity-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
target_compile_options(activity-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(activity-test PRIVATE --target=aarch64-none-linux-android35 -u ANativeActivity_onCreate)
target_link_libraries(activity-test PRIVATE native_app_glue)
# choreographer-test (ADR-0036, step a): AChoreographer with every way to post, the timelines (API 33) and
# the refresh-rate callback; the 32-bit posts are deprecated since API 29 and still in use.
add_library(choreographer-test SHARED apps/choreographer-test/main.c)
target_compile_options(choreographer-test PRIVATE --target=aarch64-none-linux-android35 -Wno-deprecated-declarations)
target_link_options(choreographer-test PRIVATE --target=aarch64-none-linux-android35 -u ANativeActivity_onCreate)
target_link_libraries(choreographer-test PRIVATE native_app_glue nativewindow)
# x18-test: every form of x18 the converter puts into an island, written in assembly, since the
# NDK's clang reserves x18 (ADR-0020 X5).
add_executable(x18-test suite/x18-test.c suite/x18-test.S)
# x18-stress: eight threads with x18 islands under SIGUSR1 from the main thread.
add_executable(x18-stress suite/x18-stress.c suite/x18-stress.S)
# island-bench: what one pass through an island costs, against a twin the converter leaves alone
# (ADR-0020 X6).
add_executable(island-bench bench/island-bench.c bench/island-bench.S)
# address-space: the address space a guest program has left, and the allocator's spill (ADR-0035).
add_executable(address-space bench/address-space.c)
# lld relaxes ADRP and ADD into NOP and ADR when the target lies within 1 MiB, and the converter
# knows a jump table's base only from ADRP and ADD, as the toolchains that use x18 left it.
target_link_options(x18-test PRIVATE -Wl,--no-relax)
target_link_options(x18-stress PRIVATE -Wl,--no-relax)
target_link_options(island-bench PRIVATE -Wl,--no-relax)
