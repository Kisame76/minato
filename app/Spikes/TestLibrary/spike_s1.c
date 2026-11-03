// Test library for spike S1. tools/build-spike-dylibs.sh builds it on the Mac and
// signs it like the app; at runtime the app copies it to Application Support and loads it with dlopen.

int spike_answer(void) {
    return 42;
}

// Shows that the writable data segment works: returns 1, 2, 3, ... on successive calls.
int spike_counter(void) {
    static int counter;
    return ++counter;
}
