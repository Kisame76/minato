// src/linker.cpp once more, instrumented with AddressSanitizer for the unit tests. The runtime's copy
// is built without it, because it runs on guest stacks, which AddressSanitizer cannot follow when
// an exception unwinds them (dlopen).
#include "../src/linker.cpp"
