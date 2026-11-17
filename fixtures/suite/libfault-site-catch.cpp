// fault-site-test's catcher: calls the thrower in libfault-site-throw.so and catches what it throws, so the unwinder
// walks from one converted library into another. catch (...) matches whatever type each library's own C++ runtime
// names it by (the NDK links one into each).

extern "C" void fault_site_throw(int value);

extern "C" int fault_site_catch(void) {
    try {
        fault_site_throw(42);
    } catch (...) {
        return 0;
    }
    return 1;
}
