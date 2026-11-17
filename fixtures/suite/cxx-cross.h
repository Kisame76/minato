// cxx-cross-test's shared declarations: the exception type with a vtable that libcxx-thrower.so throws and
// libcxx-catcher.so catches by its own type (its key function, what(), is in libcxx-thrower.cpp, so its type_info and
// vtable live there once), and the two libraries' entry points.
#pragma once

#include <exception>

struct Fault : std::exception {
    explicit Fault(int value) : code(value) {}
    const char *what() const noexcept override;
    int code;
};

extern "C" {
/// Calls `callback(kind)` with a guard of its own on the stack; whatever the callback throws passes through it.
void cxx_thrower_call(void (*callback)(int), int kind);
/// Throws by `kind`: 0 std::runtime_error, 1 Fault(7), 2 the int 42; with a guard of its own on the stack.
void cxx_thrower_throw(int kind);
/// Records an event in order ("released <name>" from a guard's destructor, "caught ..."); kept in the thrower,
/// which the catcher links against.
void cxx_cross_note(const char *event);
/// The events since the last call, joined by ", "; clears them.
const char *cxx_cross_take(void);
}
