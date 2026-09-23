# apkconv: turns the ELF files of an APK into container dylibs (ADR-0008, ADR-0010, ADR-0019):
# ELF reader, rewriter, Mach-O writer and the converter that puts them together.
add_library(apkconv STATIC src/elf.cpp src/macho.cpp src/a64.cpp src/rewrite.cpp src/container.cpp src/der.cpp
  src/certificate.cpp src/codesign.cpp src/security_identity.cpp src/c_api.cpp)
# The container format is shared with the loader: runtime/include/APKRuntime/container_format.h.
# capi/ holds the C interface for the app (APKConvert/apkconv.h), packed into build/APKConvert.xcframework.
target_include_directories(apkconv PUBLIC include capi "${PROJECT_SOURCE_DIR}/runtime/include")
target_link_libraries(apkconv PRIVATE apk_defaults)
# The signer asks Security.framework to sign with the private key (security_identity.cpp, ADR-0022).
target_link_libraries(apkconv PUBLIC "-framework Security" "-framework CoreFoundation")

if(APK_HOST_BUILD)
  add_executable(apk-convert src/apk_convert.cpp)
  target_link_libraries(apk-convert PRIVATE apkconv apk_defaults)

  # A container that codesign signed with the Apple Development certificate, from the Xcode device
  # build with the fixtures (tools/device-suite.sh): the signer and the C interface check against it
  # what a real Apple signature holds. Without that build these checks are skipped.
  set(APK_APPLE_SIGNED_CONTAINER
    "${PROJECT_SOURCE_DIR}/build/DerivedData/Build/Products/Debug-iphoneos/APKRunner.app/Fixtures/system-lib64/libdl.so.dylib"
    CACHE FILEPATH "A container codesign signed with an Apple Development certificate")

  # The app's C interface, compiled as plain C.
  add_executable(converter_c_api_test tests/c_api_test.c)
  target_link_libraries(converter_c_api_test PRIVATE apkconv apk_defaults)
  add_test(NAME converter.c_api
    COMMAND converter_c_api_test "${CMAKE_CURRENT_BINARY_DIR}/c_api_test_scratch" "${APK_FIXTURES_DIR}"
      "${APK_APPLE_SIGNED_CONTAINER}")

  add_executable(der_test tests/der_test.cpp)
  target_link_libraries(der_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.der COMMAND der_test)

  # Compares the signer with codesign, also against Apple's real chain; needs a directory of its own
  # and uses the fixtures when built.
  add_executable(codesign_test tests/codesign_test.cpp)
  target_link_libraries(codesign_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.codesign
    COMMAND codesign_test "${CMAKE_CURRENT_BINARY_DIR}/codesign_test_scratch" "${APK_FIXTURES_DIR}"
      "${APK_APPLE_SIGNED_CONTAINER}")

  add_executable(elf_test tests/elf_test.cpp)
  target_link_libraries(elf_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.elf COMMAND elf_test)

  # The reference code is assembled by the host toolchain and serves as the oracle for the stubs.
  enable_language(ASM)
  add_executable(rewrite_test tests/rewrite_test.cpp tests/rewrite_reference.S)
  target_link_libraries(rewrite_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.rewrite COMMAND rewrite_test)

  # The register-use decoder: reference cases from the host assembler, and llvm-objdump as the
  # oracle for hundreds of thousands of instruction words (skipped without llvm-objdump).
  add_executable(a64_test tests/a64_test.cpp tests/a64_reference.S)
  target_link_libraries(a64_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.a64 COMMAND a64_test)
  execute_process(COMMAND xcrun --find llvm-objdump OUTPUT_VARIABLE APK_LLVM_OBJDUMP
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  add_test(NAME converter.a64_objdump
    COMMAND a64_test "${APK_LLVM_OBJDUMP}" "${CMAKE_CURRENT_BINARY_DIR}/a64_test_scratch" "${APK_FIXTURES_DIR}")
  set_tests_properties(converter.a64_objdump PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(container_test tests/container_test.cpp)
  target_link_libraries(container_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.container COMMAND container_test "${CMAKE_CURRENT_BINARY_DIR}/container_test_scratch")

  # Writes, signs and loads container dylibs, so it needs a directory of its own.
  add_executable(macho_test tests/macho_test.cpp)
  target_link_libraries(macho_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.macho COMMAND macho_test "${CMAKE_CURRENT_BINARY_DIR}/macho_test_scratch")

  # The NDK fixtures (APK_FIXTURES_DIR, root CMakeLists.txt); both tests are skipped while they are missing.
  add_executable(fixtures_test tests/fixtures_test.cpp)
  target_link_libraries(fixtures_test PRIVATE apkconv apk_defaults apk_testing)
  add_test(NAME converter.fixtures COMMAND fixtures_test "${APK_FIXTURES_DIR}")
  set_tests_properties(converter.fixtures PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME converter.inspect_fixtures
    COMMAND apk-convert inspect "${APK_FIXTURES_DIR}/raw-hello" "${APK_FIXTURES_DIR}/hello")
  set_tests_properties(converter.inspect_fixtures PROPERTIES
    PASS_REGULAR_EXPRESSION "interpreter: none.*16 KiB pages: yes.*svc #0: 2.*x18: 0 reads, 0 writes, 0 saved in register dumps. islands: 0, refused: 0.*interpreter: none.*16 KiB pages: yes.*svc #0: 229.*x18: 0 reads, 1 write, 0 saved in register dumps. islands: 1, refused: 0"
    SKIP_REGULAR_EXPRESSION "cannot open")
endif()
