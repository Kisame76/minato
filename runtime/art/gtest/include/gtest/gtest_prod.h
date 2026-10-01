// googletest's FRIEND_TEST, which libziparchive's zip_writer.h uses to let a test reach its internals (ADR-0033 a).
// The guest builds no tests; the friend declaration names the class googletest would generate for the test.
#pragma once

#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name##_##test_name##_Test
