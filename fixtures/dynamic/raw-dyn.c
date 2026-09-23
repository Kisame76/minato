// raw-dyn: dynamic linking without a libc (M2 step 5a). Calls into libraw-dyn.so through the PLT,
// reads its data through the GOT, keeps a pointer to one of its functions in data (ABS64), overrides
// one of its functions for the library itself and refers to a weak symbol nobody defines. Exits
// with 0 when everything held.
// Expected output: "hello from raw-dyn"
#include "raw_syscall.h"

extern volatile int raw_dyn_counter;
long raw_dyn_print(void);

// Defined nowhere: a weak reference resolves to 0 (R_AARCH64_GLOB_DAT against an undefined weak symbol).
extern long raw_dyn_optional(void) __attribute__((weak));

// A function pointer in data: R_AARCH64_ABS64. volatile keeps the compiler from calling directly.
static long (*volatile print)(void) = raw_dyn_print;

// Exported (see CMakeLists.txt), so it interposes the library's definition.
const char *raw_dyn_greeting(void) {
    return "hello from raw-dyn\n";
}

__attribute__((noreturn)) void _start(void) {
    const long written = print();
    raw_exit_group(raw_dyn_counter == 41 && written == 19 && raw_dyn_optional == 0 ? 0 : 1);
}
