// libdl-own (ADR-0040 test 2, ADR-0033 e1): a library that defines getenv, a name libc.so also has, and calls it.
// Opened with dlopen, its own references find its own definition, as linker64 relocates a library against the
// global group (the program and DF_1_GLOBAL libraries), then its own local group, itself first.
#include <stddef.h>

char *getenv(const char *name) {
    (void)name;
    return (char *)"own";
}

// Which getenv the library's own call reaches: its own answers "own"; libc.so's finds no DL_OWN_MARK.
const char *dl_own_check(void) {
    const char *value = getenv("DL_OWN_MARK");
    return value != NULL ? "its own getenv" : "libc.so's getenv";
}
