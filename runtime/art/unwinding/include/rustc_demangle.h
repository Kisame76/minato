// The Rust demangler's C interface that libunwindstack calls (ADR-0033 a). The guest's has no Rust demangler: it
// returns NULL, so Rust names stay as they are in stack dumps.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char* rustc_demangle(const char* mangled, char* out, size_t* len, int* status);

#ifdef __cplusplus
}
#endif
