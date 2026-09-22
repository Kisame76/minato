# apkconv: turns the ELF files of an APK into container dylibs (ADR-0008, ADR-0010).
# So far the ELF reader; the rewriter and the Mach-O writer follow in M2 step 4.
add_library(apkconv STATIC src/elf.cpp)
target_include_directories(apkconv PUBLIC include)
target_link_libraries(apkconv PRIVATE apk_defaults)

if(APK_HOST_BUILD)
  add_executable(apk-convert src/apk_convert.cpp)
  target_link_libraries(apk-convert PRIVATE apkconv apk_defaults)

  add_executable(elf_test tests/elf_test.cpp)
  target_link_libraries(elf_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.elf COMMAND elf_test)

  # The NDK fixtures from tools/build-fixtures.sh; both tests are skipped while they are missing.
  set(APK_FIXTURES_DIR "${PROJECT_SOURCE_DIR}/build/fixtures/bin" CACHE PATH "Directory of the built NDK fixtures")
  add_executable(fixtures_test tests/fixtures_test.cpp)
  target_link_libraries(fixtures_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.fixtures COMMAND fixtures_test "${APK_FIXTURES_DIR}")
  set_tests_properties(converter.fixtures PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME converter.inspect_fixtures
    COMMAND apk-convert inspect "${APK_FIXTURES_DIR}/raw-hello" "${APK_FIXTURES_DIR}/hello")
  set_tests_properties(converter.inspect_fixtures PROPERTIES
    PASS_REGULAR_EXPRESSION "interpreter: none.*16 KiB pages: yes.*interpreter: none.*16 KiB pages: yes"
    SKIP_REGULAR_EXPRESSION "cannot open")
endif()
