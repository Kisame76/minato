// The one piece of googletest that the host aidl takes outside its tests (ADR-0045 decision 1):
// aidl_checkapi.cpp compares two dumps of a type for --checkapi with testing::internal::EqHelper and
// prints its failure message. This stands in for it, so googletest stays out of the build; its message
// shows both dumps, without gtest's line diff. Only --lang=java runs in Minato's build.
#pragma once

#include <string>

namespace testing {

class AssertionResult {
 public:
  AssertionResult(bool success, std::string message) : success_(success), message_(std::move(message)) {}
  operator bool() const { return success_; }
  const char* failure_message() const { return message_.c_str(); }

 private:
  bool success_;
  std::string message_;
};

namespace internal {

class EqHelper {
 public:
  static AssertionResult Compare(const char* lhs_expression, const char* rhs_expression, const std::string& lhs,
                                 const std::string& rhs) {
    if (lhs == rhs) return AssertionResult(true, "");
    return AssertionResult(false, std::string("Expected equality of these values:\n  ") + lhs_expression + "\n    " +
                                      lhs + "\n  " + rhs_expression + "\n    " + rhs + "\n");
  }
};

}  // namespace internal
}  // namespace testing
