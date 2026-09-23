// libraw-dyn.so: the library half of raw-dyn (M2 step 5a). No libc; output goes through svc.
#include "raw_syscall.h"

// Read by the program through the GOT (R_AARCH64_GLOB_DAT). The constructor sets it; volatile keeps
// the compiler from running the constructor at compile time and dropping it.
volatile int raw_dyn_counter;

static volatile int initialised;

__attribute__((constructor)) static void raw_dyn_init(void) {
    initialised = 1;
    raw_dyn_counter = 41;
}

// The program defines this too. Its definition comes first in the global scope, so this call,
// which goes through the library's own PLT, must land in the program (symbol interposition).
const char *raw_dyn_greeting(void) {
    return "wrong: the library's own greeting\n";
}

// Called by the program through its PLT (R_AARCH64_JUMP_SLOT).
long raw_dyn_print(void) {
    if (!initialised) return -1;
    const char *text = raw_dyn_greeting();
    size_t size = 0;
    while (text[size] != '\0') ++size;
    return raw_write(1, text, size);
}
