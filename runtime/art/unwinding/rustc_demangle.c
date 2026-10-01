// No Rust demangler for libunwindstack (rustc_demangle.h): every name stays mangled.
#include "rustc_demangle.h"

char* rustc_demangle(const char* mangled, char* out, size_t* len, int* status) {
    (void)mangled;
    (void)out;
    (void)len;
    if (status != NULL) *status = 0;
    return NULL;
}
