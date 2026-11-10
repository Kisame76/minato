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
# memory-churn-test: memory released in rounds (malloc, munmap, madvise, a dirtied private file) must not add up.
add_executable(memory-churn-test suite/memory-churn-test.c)
# memory-release-test: the other ways to give memory back (MADV_REMOVE, mremap, a GC reservation) and to hold it.
add_executable(memory-release-test suite/memory-release-test.c)
# memory-munmap-piece-test: a munmap of a 4 KiB piece (a program that hardcodes 4096, as Mono/il2cpp pools do) out of a
# 64 KiB mapping; on 16 KiB host pages it must still return 0 and leave the neighbours readable (ADR-0007).
add_executable(memory-munmap-piece-test suite/memory-munmap-piece-test.c)
# memory-munmap-fill-test: freeing a region in 4 KiB steps returns the memory on 16 KiB pages too (ADR-0007: a host
# page is unmapped once its last 4 KiB piece goes), so the resident size falls.
add_executable(memory-munmap-fill-test suite/memory-munmap-fill-test.c)
# memory-munmap-remap-test: a fresh mapping over a host page whose pieces were partly unmapped resets the per-page
# piece tracking, so unmapping the one old live piece does not drop the fresh mapping (ADR-0007).
add_executable(memory-munmap-remap-test suite/memory-munmap-remap-test.c)
# memory-munmap-keep-test / memory-dontneed-piece-test: a munmap or MADV_DONTNEED of a 4 KiB piece inside a 16 KiB host
# page must leave the live neighbours' bytes untouched (byte-precise, not MADV_ZERO which rounds to the host page).
add_executable(memory-munmap-keep-test suite/memory-munmap-keep-test.c)
add_executable(memory-dontneed-piece-test suite/memory-dontneed-piece-test.c)
# memory-mmap-reuse-test / memory-mmap-noreplace-test: a 4 KiB MAP_FIXED|ANON (or MAP_FIXED_NOREPLACE) back into a piece
# freed inside a live host page gets a fresh zero page / is seen free by the records (ADR-0007: the sub-page placement
# mirror of the munmap side).
add_executable(memory-mmap-reuse-test suite/memory-mmap-reuse-test.c)
add_executable(memory-mmap-noreplace-test suite/memory-mmap-noreplace-test.c)
# memory-mmap-shared-neighbour-test: a sub-page anon MAP_FIXED must refuse a shared host page, not memset into the
# shared object (ADR-0007); only the memfd's kept bytes are printed, so it matches Android.
add_executable(memory-mmap-shared-neighbour-test suite/memory-mmap-shared-neighbour-test.c)
# memory-mmap-file-piece-test: a MAP_PRIVATE file sub-page into a live host page reads the file's bytes (copied in),
# leaving the neighbours (ADR-0007: the sub-page file mirror of the anon placement).
add_executable(memory-mmap-file-piece-test suite/memory-mmap-file-piece-test.c)
# memory-private-file-test: a written MAP_PRIVATE file mapping advised away with MADV_DONTNEED reads the file's bytes
# again, not the written ones (ADR-0007: the loader re-maps the file; Linux drops the private copies).
add_executable(memory-private-file-test suite/memory-private-file-test.c)
# memory-private-file-rename-test: the re-map must never show a different file when the path is replaced under the
# mapping (an inode guard; ADR-0007). memory-private-file-split-test: it picks the right record when an anonymous
# MAP_FIXED has replaced the middle of the file mapping (no stale record over it).
add_executable(memory-private-file-rename-test suite/memory-private-file-rename-test.c)
add_executable(memory-private-file-split-test suite/memory-private-file-split-test.c)
# Without builtins, so that clang does not fold the math calls at compile time.
add_executable(math-test suite/math-test.c)
# scudo-region-test: one size class must hold more than the old 64 MiB region (ADR-0035 amendment, RegionSizeLog 27).
add_executable(scudo-region-test suite/scudo-region-test.c)
# ctr-el0-test: a guest read of CTR_EL0 (and compiler-rt's __clear_cache, which reads it) must not trap on XNU.
add_executable(ctr-el0-test suite/ctr-el0-test.c)
# scudo-big-region-test: one size class must hold 200 MiB, which needs a 256 MiB region (ADR-0035, two libc variants).
add_executable(scudo-big-region-test suite/scudo-big-region-test.c)
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
# page4k-test and its library, linked for 4 KiB pages as NDK builds before r28 are: code and writable
# data meet in one 16 KiB page, so the converter moves the library's whole image (ADR-0007 decision 2).
add_library(page4k-lib SHARED suite/libpage4k-lib.c)
target_link_options(page4k-lib PRIVATE -Wl,-z,max-page-size=4096)
add_executable(page4k-test suite/page4k-test.c)
target_link_libraries(page4k-test PRIVATE page4k-lib)
# runs-test and its library, linked with runs-lib.ld so that a second code segment, a read-only one and a writable one
# follow its data, as the Unity test APK v2's libil2cpp.so (ADR-0008 amendment, step 2); --rosegment keeps read-only data in
# LOAD segments of their own, as there.
add_library(runs-lib SHARED suite/libruns-lib.c)
target_link_options(runs-lib PRIVATE -Wl,--rosegment "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/suite/runs-lib.ld")
set_target_properties(runs-lib PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/suite/runs-lib.ld")
add_executable(runs-test suite/runs-test.c)
target_link_libraries(runs-test PRIVATE runs-lib)
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
# dl-own-test and libdl-own.so, which defines getenv and calls it through its PLT (-fsemantic-interposition), as an
# app's library may define a name of libc.so's (ADR-0040 test 2, ADR-0033 e1).
add_library(dl-own SHARED suite/libdl-own.c)
target_compile_options(dl-own PRIVATE -fsemantic-interposition)
add_executable(dl-own-test suite/dl-own-test.c)
target_link_libraries(dl-own-test PRIVATE dl)
add_executable(signal-test suite/signal-test.c)
# kill-self-test: SIGKILL to its own process, as Process.killProcess(Process.myPid()) sends it (ADR-0028).
add_executable(kill-self-test suite/kill-self-test.c)
# memory-limit-test: allocates until iOS's jetsam ends it, for the memory-watermark device check (ADR-0032 d8). It is
# listed in fixtures/iphone-only.txt, NOT fixtures/tests.txt, so only tools/device-suite.sh (--iphone-only) and the
# app's FixtureRunner run it, on a real iPhone; Mac ctest, the AVD and the Simulator (which read only tests.txt) never
# do, since only a device enforces the memory limit (elsewhere it would thrash the host).
add_executable(memory-limit-test suite/memory-limit-test.c)
# dl-lock-test and the library whose constructor holds the dynamic linker's lock (ADR-0025, addition d).
add_library(dl-lock SHARED suite/libdl-lock.c)
target_link_libraries(dl-lock PRIVATE dl)
add_executable(dl-lock-test suite/dl-lock-test.c)
target_link_libraries(dl-lock-test PRIVATE dl)
# frame-cap-test and the two libraries that stand in for an engine and its scripting runtime by Unity's names
# (ADR-0066): the engine imports one registration function and looks the other up with dlsym.
add_library(frame-cap-api SHARED suite/libframe-cap-api.c)
add_library(frame-cap-engine SHARED suite/libframe-cap-engine.c)
target_link_libraries(frame-cap-engine PRIVATE frame-cap-api dl)
add_executable(frame-cap-test suite/frame-cap-test.c)
target_link_libraries(frame-cap-test PRIVATE frame-cap-engine frame-cap-api)
# luajit-switch-test and the two libraries that stand in for LuaJIT and for plain Lua by their exported names (the LuaJIT
# switch, runtime/tests/luajit_switch_run.py): the app imports the names, or looks them up with dlsym.
add_library(fake-luajit SHARED suite/libfake-luajit.c)
add_library(fake-lua SHARED suite/libfake-lua.c)
add_executable(luajit-switch-test suite/luajit-switch-test.c)
target_link_libraries(luajit-switch-test PRIVATE fake-luajit dl)
# mono-switch-test and the two libraries that stand in for Mono and for a library that only borrows its mode name (the
# Mono switch, runtime/tests/mono_switch_run.py): the app imports the names, or looks them up with dlsym.
add_library(fake-mono SHARED suite/libfake-mono.c)
add_library(fake-notmono SHARED suite/libfake-notmono.c)
add_executable(mono-switch-test suite/mono-switch-test.c)
target_link_libraries(mono-switch-test PRIVATE fake-mono dl)
# ffi-closure-test and the two libraries that stand in for a statically linked libffi by its exported names and by the
# layout of its legacy closure path (libffi closures without executable memory, runtime/tests/ffi_closures_run.py):
# libfake-ffi-sym.so is linked -Bsymbolic and calls the names from inside, by direct branches.
add_library(fake-ffi SHARED suite/libfake-ffi.c)
add_library(fake-ffi-sym SHARED suite/libfake-ffi.c)
target_link_options(fake-ffi-sym PRIVATE -Wl,-Bsymbolic)
add_executable(ffi-closure-test suite/ffi-closure-test.c)
target_link_libraries(ffi-closure-test PRIVATE fake-ffi dl)
add_dependencies(ffi-closure-test fake-ffi-sym)
# jsc-switch-test and the library that stands in for JavaScriptCore by its exported names (the JavaScriptCore switch,
# runtime/tests/jsc_switch_run.py): the app opens it with dlopen, before or after its own first getenv.
add_library(fake-jsc SHARED suite/libfake-jsc.c)
add_executable(jsc-switch-test suite/jsc-switch-test.c)
target_link_libraries(jsc-switch-test PRIVATE dl)
add_dependencies(jsc-switch-test fake-jsc)
# ns-test and its libraries (ADR-0039 a, ADR-0040 decision 2, ADR-0046 a): a program outside the APEXes, and a
# library for the folders of ART and Conscrypt that needs libns-dep.so and opens libns-plugin.so, both beside it
# only; runtime.system_links places them (runtime/tests/ns_run.py).
add_library(ns-dep SHARED suite/libns-dep.c)
# The same soname as an app ships it, another value (ADR-0040 test 1, ADR-0033 e2a).
add_library(ns-dep-app SHARED suite/libns-dep-app.c)
set_target_properties(ns-dep-app PROPERTIES NO_SONAME ON)
target_link_options(ns-dep-app PRIVATE -Wl,-soname,libns-dep.so)
add_library(ns-plugin SHARED suite/libns-plugin.c)
# Whether dlsym(RTLD_DEFAULT) from a library sees ns_plugin_value through its namespace (ADR-0033 e2b2).
add_library(ns-find SHARED suite/libns-find.c)
target_link_libraries(ns-find PRIVATE dl)
add_library(ns-lib SHARED suite/libns-lib.c)
target_link_libraries(ns-lib PRIVATE ns-dep dl)
add_executable(ns-test suite/ns-test.c)
target_link_libraries(ns-test PRIVATE dl)
# hwui-load: opens libhwui.so and calls init_android_graphics (ADR-0043 a1); graphics.hwui_load places libhwui and
# what it needs (graphics/tests/hwui_load_run.py).
add_executable(hwui-load suite/hwui-load.c)
target_link_libraries(hwui-load PRIVATE dl)
# property-test: bionic's property calls on the runtime's property area (ADR-0039 decisions 4 and 9).
add_executable(property-test suite/property-test.c)
# hwcap-test: the CPU features a program learns, from getauxval, in its own ifunc resolver (ADR-0047 a) and on
# /proc/cpuinfo (c).
add_executable(hwcap-test suite/hwcap-test.c)
# el0-check: what the instructions of the CPU features that ADR-0047 reports only after a check at EL0 do on the
# device, each under a SIGILL handler (b1).
add_executable(el0-check suite/el0-check.c)
# camera-media-test: libcamera2ndk.so and libmediandk.so as Godot's library links them (ADR-0042 b, gap 15).
add_executable(camera-media-test suite/camera-media-test.c)
target_link_libraries(camera-media-test PRIVATE dl)
# media-keys-test: libmediandk.so's key strings (ADR-0060 e2b).
add_executable(media-keys-test suite/media-keys-test.c)
target_link_libraries(media-keys-test PRIVATE dl)
# env-test: prints the environment it starts with; runtime.app_environment runs it through apk-run
# (runtime/tests/environment_run.py).
add_executable(env-test suite/env-test.c)
# session-test: the session's configuration through session_configuration and libandroid; runtime.session_run starts
# it through apk-run with --configuration and without (runtime/tests/session_run.py).
add_executable(session-test suite/session-test.c)
target_include_directories(session-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
target_link_libraries(session-test PRIVATE android dl)
# libcodec-test.so: the JNI half of the Java fixture codec-test (fixtures/java/codec-test, ADR-0033 test 2), which
# runtime.art_codec runs on ART.
add_library(codec-test SHARED suite/libcodec-test.c)
# libstress-test.so: the JNI half of the Java fixture stress-test (fixtures/java/stress-test, ADR-0033 test 4), which
# runtime.art_stress runs on ART. stw-test runs its part c, Boehm's stop-the-world of suite/stw.h, without ART.
add_library(stress-test SHARED suite/libstress-test.c)
add_executable(stw-test suite/stw-test.c)
# fd-test: descriptor numbers as Linux hands them out, whatever the process holds on the host (ADR-0027).
add_executable(fd-test suite/fd-test.c)
# poll-test: eventfd, epoll, poll and select, as Android's looper uses them (ADR-0027).
add_executable(poll-test suite/poll-test.c)
# timerfd-test: timerfd_create, timerfd_settime and timerfd_gettime as event loops use them.
add_executable(timerfd-test suite/timerfd-test.c)
# sched-priority-test: sched_get_priority_max and sched_get_priority_min for every scheduling policy.
add_executable(sched-priority-test suite/sched-priority-test.c)
# socket-test: socketpairs, AF_UNIX names, TCP and UDP on loopback, flags, options and signals (ADR-0054).
# gai-offline-test: getaddrinfo without a reachable network (runtime/tests/gai_offline_run.py; on Android in airplane
# mode).
add_executable(gai-offline-test suite/gai-offline-test.c)
add_executable(socket-test suite/socket-test.c)
# memory-test: every size class of the allocator, a 2 GiB window at a 4 GiB boundary and RLIMIT_STACK
# (ADR-0035).
add_executable(memory-test suite/memory-test.c)
# mmap-offset-test: a file mapping at a 4 KiB-aligned (not 16 KiB-aligned) offset, as an engine makes for an
# uncompressed APK/OBB entry; Minato emulates it so iOS's 16 KiB pages do not force a read()-into-RAM fallback
# (ADR-0007 change).
add_executable(mmap-offset-test suite/mmap-offset-test.c)
# atexit-test: 2,000 handlers of __cxa_atexit, whose read-only array bionic grows with mremap (ADR-0021).
add_executable(atexit-test suite/atexit-test.c)
# start-test: ifunc resolvers and constructors run on the program's main thread (ADR-0028, decision 5).
add_executable(start-test suite/start-test.c)
# kernel-test: what ART's start asks of the kernel, /proc and /sys (ADR-0033, decision 8).
add_executable(kernel-test suite/kernel-test.c)
# priority-test: per-thread nice values, as Process.setThreadPriority and getThreadPriority use them.
add_executable(priority-test suite/priority-test.c)
# memfd-test: memfd_create and its seals, as libcutils' ashmem uses them once sys.use_memfd is on.
add_executable(memfd-test suite/memfd-test.c)
# sharedmem-test: the NDK's ASharedMemory, on ashmem on Android 15 and on memfd_create in Minato.
add_executable(sharedmem-test suite/sharedmem-test.c)
target_link_libraries(sharedmem-test PRIVATE android)
# ifaddrs-test: the network interfaces an app sees, loopback's lines only (ADR-0054 c).
add_executable(ifaddrs-test suite/ifaddrs-test.c)
# route-test: the routes an app reads, RTM_GETROUTE's dump, lo's lines and Android's layout (ADR-0054 c3).
add_executable(route-test suite/route-test.c)
# stdcxx-test: Android's libstdc++.so, which ANDROID_STL=system links, in place of the NDK's libc++ (gap 14).
add_executable(stdcxx-test suite/stdcxx-test.cpp)
target_compile_options(stdcxx-test PRIVATE -std=gnu++20 -fno-exceptions -fno-rtti)
target_link_options(stdcxx-test PRIVATE -nostdlib++)
target_link_libraries(stdcxx-test PRIVATE stdc++ dl)
# unbacked-libs-test: the NDK libraries with nothing behind them in Minato, which an app may name in NEEDED (gap 14).
add_executable(unbacked-libs-test suite/unbacked-libs-test.c)
target_link_libraries(unbacked-libs-test PRIVATE sync amidi binder_ndk neuralnetworks OpenMAXAL)
# cacerts-test: the trust store's CA certificates, in Conscrypt's APEX and in /system/etc/security (ADR-0046).
add_executable(cacerts-test suite/cacerts-test.c)
# sendfile-test: sendfile(2) to a file, a socket and a pipe, its offsets and its errors in the kernel's order.
add_executable(sendfile-test suite/sendfile-test.c)
# proc-links-test: the links exe, cwd and root of /proc/self and /proc/<pid>.
add_executable(proc-links-test suite/proc-links-test.c)
# proc-access-test: access() and stat()/lstat() on the /proc names the runtime serves and the directories.
add_executable(proc-access-test suite/proc-access-test.c)
# sensors-test: libandroid's sensor manager and an event queue without sensors.
add_executable(sensors-test suite/sensors-test.c)
target_link_libraries(sensors-test PRIVATE android)
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
# input-test (ADR-0036, step b): the input queue and its motion events for the gestures of gestures.txt; the
# classification and the action button came with API 33.
add_library(input-test SHARED apps/input-test/main.c)
target_compile_options(input-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(input-test PRIVATE --target=aarch64-none-linux-android35 -u ANativeActivity_onCreate)
target_link_libraries(input-test PRIVATE native_app_glue nativewindow m)
# window-test (ADR-0043 test 3): drawing by the CPU with ANativeWindow_lock and ANativeWindow_unlockAndPost.
add_library(window-test SHARED apps/window-test/main.c)
target_compile_options(window-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(window-test PRIVATE --target=aarch64-none-linux-android35 -u ANativeActivity_onCreate)
target_link_libraries(window-test PRIVATE native_app_glue nativewindow)
# window-formats-test (ADR-0043 b5, step f): its whole window by the CPU in RGBA_8888, RGBX_8888 and RGB_565, for the
# screenshots of tools/window-contents-check.sh.
add_library(window-formats-test SHARED apps/window-formats-test/main.c)
target_compile_options(window-formats-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(window-formats-test PRIVATE --target=aarch64-none-linux-android35 -u ANativeActivity_onCreate)
target_link_libraries(window-formats-test PRIVATE native_app_glue nativewindow)
# native-ime-test (ADR-0068 step 2e): a NativeActivity without native_app_glue that asks for the soft keyboard, which
# Android 15 ignores, and logs the keys of its input queue. Its source is in fixtures/java, beside the fixture input
# method that tools/build-java-test-app.sh adds to the APK for the Android reference.
add_library(native-ime-test SHARED java/native-ime-test/jni/native_ime_test.c)
target_compile_options(native-ime-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(native-ime-test PRIVATE --target=aarch64-none-linux-android35)
target_link_libraries(native-ime-test PRIVATE android log)
# gamepad-native-test (ADR-0068 decision 5, step 4): a NativeActivity without native_app_glue that logs the keys and
# the joystick motion events of a game controller in its input queue.
add_library(gamepad-native-test SHARED apps/gamepad-native-test/main.c)
target_compile_options(gamepad-native-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(gamepad-native-test PRIVATE --target=aarch64-none-linux-android35)
target_link_libraries(gamepad-native-test PRIVATE android log)

# mouse-native-test (ADR-0068 decision 5, the mouse step): a NativeActivity without native_app_glue that logs the motion
# events and keys a mouse gives its input queue.
add_library(mouse-native-test SHARED apps/mouse-native-test/main.c)
target_compile_options(mouse-native-test PRIVATE --target=aarch64-none-linux-android35)
target_link_options(mouse-native-test PRIVATE --target=aarch64-none-linux-android35)
target_link_libraries(mouse-native-test PRIVATE android log)
# x18-test: every form of x18 the converter puts into an island, written in assembly, since the
# NDK's clang reserves x18 (ADR-0020 X5).
add_executable(x18-test suite/x18-test.c suite/x18-test.S)
# x18-stress: eight threads with x18 islands under SIGUSR1 from the main thread.
add_executable(x18-stress suite/x18-stress.c suite/x18-stress.S)
# c-rename-test: a function option (c) renames whole (ADR-0020); runtime-only -- it is ABI-correct only after
# conversion, so it runs through apk-run but not natively (fixtures/tests.txt).
add_executable(c-rename-test suite/c-rename-test.c suite/c-rename-test.S)
# c-signal-test: the signal path hands a handler the guest x18 of a (c)-renamed function from the rename register
# and carries its write back (ADR-0020 must-have 6, design-A step A0); runtime-only, like c-rename-test.
add_executable(c-signal-test suite/c-signal-test.c suite/c-signal-test.S)
# island-bench: what one pass through an island costs, against a twin the converter leaves alone
# (ADR-0020 X6).
add_executable(island-bench bench/island-bench.c bench/island-bench.S)
# address-space: the address space a guest program has left, and the allocator's spill (ADR-0035).
add_executable(address-space bench/address-space.c)
# c3b-monitor-probe: does an unrelated-granule store between a load- and store-exclusive clear the
# local monitor (ADR-0020 C3b)? A livelock there would mean the island's x18 spill breaks the window.
add_executable(c3b-monitor-probe bench/c3b-monitor-probe.c)
# c3b-contention-probe: the same question under contention, 1, 4 and 8 threads, with the sums checked (ADR-0020 X7 (c1)).
add_executable(c3b-contention-probe bench/c3b-contention-probe.c)
# lld relaxes ADRP and ADD into NOP and ADR when the target lies within 1 MiB, and the converter
# knows a jump table's base only from ADRP and ADD, as the toolchains that use x18 left it.
target_link_options(x18-test PRIVATE -Wl,--no-relax)
target_link_options(x18-stress PRIVATE -Wl,--no-relax)
target_link_options(island-bench PRIVATE -Wl,--no-relax)

# The test apps (ADR-0029 B2, ADR-0030 V5), each with its own CMakeLists.txt.
add_subdirectory(apps/gles-test)
add_subdirectory(apps/gles31-test)
# The shader-bench for the shader cache plan (M6): times of fresh and repeated shader builds.
add_subdirectory(apps/shader-bench)
add_subdirectory(apps/vulkan-test)
# The vk-ahb-test, graphics/tests/vk_ahb_test.c as an app for the iPhone (ADR-0058 e1)).
add_subdirectory(apps/vk-ahb-test)
# gles-procs-test, graphics/tests/gles_procs_test.c as an app for the iPhone: which GLES, EGL and Vulkan functions an
# app finds, against Android 15's measurement.
add_subdirectory(apps/gles-procs-test)
# The present-test for Swappy's frame pacing (ADR-0041 test 3).
add_subdirectory(apps/present-test)
# The present-limit-test for the frame rate limit's hold of unpaced presents (ADR-0078 decision 4).
add_subdirectory(apps/present-limit-test)
# The audio test apps (ADR-0036, step c), each with its own CMakeLists.txt.
add_subdirectory(apps/aaudio-test)
add_subdirectory(apps/opensl-test)
# The microphone's test apps (ADR-0064), without and with RECORD_AUDIO's grant.
add_subdirectory(apps/audio-input-test)
add_subdirectory(apps/audio-input-test-granted)
# The SDL2 audio sequences (the audio speed of an SDL2 game).
add_subdirectory(apps/sdl-audio-test)
# The turn test app (ADR-0038, test 3).
add_subdirectory(apps/rotation-test)
# The live audio test app (ADR-0036, after M3).
add_subdirectory(apps/audio-live)
