// cxx-cross-test's catching library: it calls libcxx-thrower.so with a callback of its own that calls back into the
// thrower, which throws, so the exception unwinds thrower -> catcher -> thrower frames before a catch here. It catches
// std::runtime_error by type, Fault by std::exception (what() through the thrower's vtable) and by its own type, and an
// int by `...`, rethrows it once and catches it again in a third frame. One shared libc++ (libc++_shared.so), as an
// app with several C++ libraries ships it, so the types match across the libraries.
#include "cxx-cross.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

struct Guard {
    explicit Guard(const char *name) : name_(name) {}
    ~Guard() { cxx_cross_note(name_); }
    const char *name_;
};

void callback(int kind) {
    Guard guard("released the callback (catcher)");
    cxx_thrower_throw(kind);
}

void rethrower() {
    try {
        cxx_thrower_call(callback, 2);
    } catch (...) {
        cxx_cross_note("caught by ..., rethrowing");
        throw;
    }
}

void print(const char *what) { std::printf("%s: %s\n", what, cxx_cross_take()); }

}  // namespace

extern "C" int cxx_catcher_run(void) {
    try {
        cxx_thrower_call(callback, 0);
    } catch (const std::runtime_error &error) {
        cxx_cross_note((std::string("caught runtime_error \"") + error.what() + "\"").c_str());
    }
    print("runtime_error by its type");

    try {
        cxx_thrower_call(callback, 1);
    } catch (const std::exception &error) {
        cxx_cross_note((std::string("caught std::exception \"") + error.what() + "\"").c_str());
    }
    print("Fault by std::exception");

    try {
        cxx_thrower_call(callback, 1);
    } catch (const std::runtime_error &) {
        cxx_cross_note("caught as runtime_error (wrong)");
    } catch (const Fault &fault) {
        cxx_cross_note((std::string("caught Fault, code ") + std::to_string(fault.code)).c_str());
    }
    print("Fault by its own type");

    try {
        rethrower();
    } catch (int value) {
        cxx_cross_note((std::string("caught the int ") + std::to_string(value) + " in the third frame").c_str());
    }
    print("int by ..., rethrown");

    std::exception_ptr kept;
    try {
        cxx_thrower_call(callback, 0);
    } catch (...) {
        kept = std::current_exception();
    }
    try {
        std::rethrow_exception(kept);
    } catch (const std::runtime_error &error) {
        cxx_cross_note((std::string("caught it again from an exception_ptr: \"") + error.what() + "\"").c_str());
    }
    print("runtime_error kept and rethrown");
    return 0;
}
