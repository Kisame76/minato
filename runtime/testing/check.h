// Minimal test support for the C++ tests of the runtime and the converter, run by CTest.
// No external test library: TEST registers a function, CHECK and CHECK_THROWS report failures,
// and run_all() returns the exit code.
#pragma once

#include <cstdio>
#include <exception>
#include <source_location>
#include <string_view>
#include <vector>

namespace apk::testing {

struct Test {
    const char *name;
    void (*run)();
};

inline std::vector<Test> &registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int failures = 0;

struct Registration {
    Registration(const char *name, void (*run)()) { registry().push_back({name, run}); }
};

inline void fail(std::source_location location, const char *message, const char *detail = "") {
    std::fprintf(stderr, "%s:%u: %s%s\n", location.file_name(), static_cast<unsigned>(location.line()), message, detail);
    ++failures;
}

inline void check(bool condition, const char *expression, std::source_location location) {
    if (!condition) fail(location, "CHECK failed: ", expression);
}

/// Passes if `statement` throws `Exception` with `message_part` in its message.
template <typename Exception, typename Statement>
void check_throws(Statement statement, std::string_view message_part, const char *expression,
                  std::source_location location) {
    try {
        statement();
    } catch (const Exception &error) {
        if (std::string_view(error.what()).find(message_part) == std::string_view::npos) {
            fail(location, "CHECK_THROWS got an unexpected message: ", error.what());
        }
        return;
    } catch (const std::exception &error) {
        fail(location, "CHECK_THROWS got another exception: ", error.what());
        return;
    }
    fail(location, "CHECK_THROWS did not throw: ", expression);
}

inline int run_all() {
    for (const auto &test : registry()) {
        const int before = failures;
        try {
            test.run();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s: unexpected exception: %s\n", test.name, error.what());
            ++failures;
        }
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", test.name);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace apk::testing

#define TEST(name)                                                                      \
    static void name();                                                                 \
    static const ::apk::testing::Registration name##_registration(#name, name);         \
    static void name()

#define CHECK(condition) \
    ::apk::testing::check(static_cast<bool>(condition), #condition, std::source_location::current())

#define CHECK_THROWS(Exception, expression, message_part)                                         \
    ::apk::testing::check_throws<Exception>([&] { static_cast<void>(expression); }, message_part, \
                                            #expression, std::source_location::current())
