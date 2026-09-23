# apkrt: loader and Linux syscall layer for guest code (ARCHITECTURE.md 4.3–4.6).
enable_language(ASM)
# M2 step 4: static programs from container dylibs; step 5: the linker and libdl for dynamic programs
# (ADR-0021), and the Linux personality their test suite needs: files, threads, signals.
set(apkrt_syscall_sources src/syscalls.cpp src/sys_files.cpp src/sys_memory.cpp src/sys_signals.cpp
  src/sys_threads.cpp src/threads.cpp src/guest_context.cpp src/signals.cpp src/process_exit.cpp)
add_library(apkrt STATIC src/apkrt.cpp src/guest_tls.c src/loader.cpp src/linker.cpp src/dl.cpp src/program.cpp
  src/vfs.cpp ${apkrt_syscall_sources} src/syscall_entry.S)
target_include_directories(apkrt PUBLIC include)
target_compile_definitions(apkrt PRIVATE APKRT_VERSION="${PROJECT_VERSION}")
target_link_libraries(apkrt PRIVATE apk_defaults)
# The syscall layer runs on the guest's stack, which AddressSanitizer does not know as a thread
# stack; instrumented, every exit_group would print a false "stack switching" warning.
# So does libdl's work (dl.cpp and the linker and loader under it), which guest code calls on its
# own stack: when an exception unwinds such a stack, AddressSanitizer reports its own redzones. The
# unit tests of the linker and the loader build instrumented copies (tests/checked_*.cpp).
if(APK_SANITIZE)
  set_source_files_properties(${apkrt_syscall_sources} src/dl.cpp src/linker.cpp src/loader.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
endif()

if(APK_HOST_BUILD)
  # Test support shared with the converter's tests.
  add_library(apk_testing INTERFACE)
  target_include_directories(apk_testing INTERFACE testing)

  add_executable(c_api_test tests/c_api_test.c)
  target_compile_definitions(c_api_test PRIVATE APKRT_EXPECTED_VERSION="${PROJECT_VERSION}")
  target_link_libraries(c_api_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.c_api COMMAND c_api_test)

  add_executable(guest_tls_test tests/guest_tls_test.c)
  target_link_libraries(guest_tls_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.guest_tls COMMAND guest_tls_test)

  add_executable(loader_test tests/loader_test.cpp tests/checked_loader.cpp)
  target_include_directories(loader_test PRIVATE src)
  target_link_libraries(loader_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.loader COMMAND loader_test)

  # On the NDK fixtures raw-dyn and libraw-dyn.so, laid out in memory; skipped while they are missing.
  add_executable(linker_test tests/linker_test.cpp tests/checked_linker.cpp tests/checked_loader.cpp)
  target_include_directories(linker_test PRIVATE src)
  target_link_libraries(linker_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.linker COMMAND linker_test "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}")
  set_tests_properties(runtime.linker PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(vfs_test tests/vfs_test.cpp)
  target_include_directories(vfs_test PRIVATE src)
  target_link_libraries(vfs_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.vfs COMMAND vfs_test)

  add_executable(exit_test tests/exit_test.cpp)
  target_include_directories(exit_test PRIVATE src)
  target_link_libraries(exit_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.exit COMMAND exit_test)

  add_executable(syscalls_test tests/syscalls_test.cpp)
  target_include_directories(syscalls_test PRIVATE src)
  target_link_libraries(syscalls_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.syscalls COMMAND syscalls_test)

  add_executable(apk-run tools/apk_run.cpp)
  target_link_libraries(apk-run PRIVATE apkrt apk_defaults)

  add_executable(threads_test tests/threads_test.cpp tests/threads_probe.S)
  target_include_directories(threads_test PRIVATE src)
  target_link_libraries(threads_test PRIVATE apkrt apk_defaults apk_testing)
  # The children run the test's functions on stacks the test maps itself, like guest threads.
  if(APK_SANITIZE)
    set_source_files_properties(tests/threads_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.threads COMMAND threads_test)
  set_tests_properties(runtime.threads PROPERTIES TIMEOUT 60)

  add_executable(syscall_entry_test tests/syscall_entry_test.c tests/syscall_entry_probe.S)
  target_link_libraries(syscall_entry_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.syscall_entry COMMAND syscall_entry_test)
endif()
