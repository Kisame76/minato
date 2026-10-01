// The one googletest header that libziparchive's zip_writer.h includes outside its tests: FRIEND_TEST names a test
// class as a friend. Without the tests the friend is never defined, so this declares it and leaves googletest out
// of the host aapt2 (ADR-0039 b1).
#pragma once

#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name##_##test_name##_Test
