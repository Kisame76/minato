// fault-site-test's thrower: a C++ exception thrown in this converted library and caught in another one
// (libfault-site-catch.cpp), as an engine's start code throws and catches across its libraries. No signal is involved,
// so the runtime writes no fault-site line.

extern "C" __attribute__((noinline)) void fault_site_throw(int value) {
    throw value;
}
