# apkrt: loader and Linux syscall layer for guest code (ARCHITECTURE.md 4.3–4.6).
# So far only the C interface that the app calls; the loader follows in M2 step 4.
add_library(apkrt STATIC src/apkrt.cpp)
target_include_directories(apkrt PUBLIC include)
target_compile_definitions(apkrt PRIVATE APKRT_VERSION="${PROJECT_VERSION}")
target_link_libraries(apkrt PRIVATE apk_defaults)

if(APK_HOST_BUILD)
  # Test support shared with the converter's tests.
  add_library(apk_testing INTERFACE)
  target_include_directories(apk_testing INTERFACE testing)

  add_executable(c_api_test tests/c_api_test.c)
  target_compile_definitions(c_api_test PRIVATE APKRT_EXPECTED_VERSION="${PROJECT_VERSION}")
  target_link_libraries(c_api_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.c_api COMMAND c_api_test)
endif()
