// cxx-cross-test's throwing library: it throws std::runtime_error, its own Fault (a type with a vtable) and an int,
// each from a frame below a callback of libcxx-catcher.so, so the unwind crosses both libraries twice.
#include "cxx-cross.h"

#include <stdexcept>
#include <string>

const char *Fault::what() const noexcept { return "fault from the thrower"; }

namespace {

std::string events, taken;

struct Guard {
    explicit Guard(const char *name) : name_(name) {}
    ~Guard() { cxx_cross_note(name_); }
    const char *name_;
};

}  // namespace

extern "C" void cxx_cross_note(const char *event) {
    if (!events.empty()) events += ", ";
    events += event;
}

extern "C" const char *cxx_cross_take(void) {
    taken.swap(events);
    events.clear();
    return taken.c_str();
}

extern "C" void cxx_thrower_throw(int kind) {
    Guard guard("released the throw site (thrower)");
    if (kind == 0) throw std::runtime_error("runtime error from the thrower");
    if (kind == 1) throw Fault(7);
    throw 42;
}

extern "C" void cxx_thrower_call(void (*callback)(int), int kind) {
    Guard guard("released the call (thrower)");
    callback(kind);
}
