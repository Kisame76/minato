// cxx-cross-test: C++ exceptions thrown and caught across two libraries (libcxx-thrower.so, libcxx-catcher.so) that
// share one libc++ (libc++_shared.so), as an app with several C++ libraries ships them: by type, by a base type through
// a vtable, by `...` with a rethrow, and through an exception_ptr, each unwinding through frames of both libraries
// whose destructors run in order. An engine throws C++ exceptions early in its start (IL2CPP for managed ones).
// Expected output: fixtures/expected/cxx-cross-test.out (Android 15's).
#include <stdio.h>

int cxx_catcher_run(void);

int main(void) {
    const int result = cxx_catcher_run();
    printf("done: %d\n", result);
    return result;
}
