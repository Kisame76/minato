// fault-site-test: "null" and "x18" end with SIGSEGV at address 0x1087b in its own library, as an app whose table base
// is NULL does: byte 0x1087b through a NULL base (libfault-site.c) and through x18 = 0 (libfault-site-x18.S); on
// Android the shell answers 139. "exception" throws a C++ exception in one of its libraries and catches it in another
// (libfault-site-throw.cpp, libfault-site-catch.cpp), prints "caught" and ends with 0. The other modes make the same read
// through a NULL base with an app's own crash handler installed first (libfault-site.h), which ends as its mode says:
// "chained" restores the old action and returns (139), "abort" calls abort() (134), "twice" sits under a reporter that a
// second library installs over it and that calls it, and restores the old action and raises SIGSEGV (139), "recover"
// moves past the load, after which the program prints "recovered" and calls abort() (134), "crash" faults itself (139).
// runtime/tests/fault_site_run.py checks the runtime's fault-site line for each. Built with FAULT_SITE_MODE
// (fault-site-test-<mode>) it runs that mode without an argument, for runners that start a program without one
// (fixtures/tests.txt, the iPhone's fixture runner).
#include "libfault-site.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char fault_site_read(const unsigned char *base, unsigned long index);
unsigned char fault_site_read_x18(unsigned long index);
int fault_site_catch(void);

static const struct {
    const char *mode;
    int ending;  // the engine handler's, 0 for none
} modes[] = {
    {"null", 0},
    {"x18", 0},
    {"exception", 0},
    {"chained", FAULT_SITE_RETURN},
    {"abort", FAULT_SITE_ABORT},
    {"twice", FAULT_SITE_RAISE},
    {"recover", FAULT_SITE_RECOVER},
    {"crash", FAULT_SITE_CRASH},
};

/// Loads the reporter's library, as an app loads a crash-reporting SDK, and installs its handler; 0 or -1.
static int install_reporter(void) {
    void *library = dlopen("libfault-site-reporter.so", RTLD_NOW);
    int (*install)(void) = library != NULL ? (int (*)(void))dlsym(library, "fault_site_install_reporter") : NULL;
    return install != NULL ? install() : -1;
}

int main(int argc, char **argv) {
    volatile unsigned long index = 0x1087b;  // not folded into the load
#ifdef FAULT_SITE_MODE
    const char *mode = argc == 1 ? FAULT_SITE_MODE : "";
    (void)argv;
#else
    const char *mode = argc == 2 ? argv[1] : "";
#endif
    int ending = -1;
    for (size_t at = 0; at < sizeof modes / sizeof modes[0]; ++at) {
        if (strcmp(mode, modes[at].mode) == 0) ending = modes[at].ending;
    }
    if (ending < 0) {
        fprintf(stderr, "usage: fault-site-test null|x18|exception|chained|abort|twice|recover|crash\n");
        return 2;
    }
    printf("reading %s\n", mode);
    fflush(stdout);
    if (strcmp(mode, "exception") == 0) {
        const int caught = fault_site_catch() == 0;
        printf("%s\n", caught ? "caught" : "not caught");
        return caught ? 0 : 1;
    }
    if (strcmp(mode, "x18") == 0) return fault_site_read_x18(index);
    if (ending != 0 && fault_site_install(ending) != 0) {
        printf("no handler\n");
        return 1;
    }
    if (strcmp(mode, "twice") == 0 && install_reporter() != 0) {
        printf("no reporter: %s\n", dlerror());
        return 1;
    }
    const unsigned char value = fault_site_read((const unsigned char *)0, index);
    if (strcmp(mode, "recover") != 0) return value;
    printf("recovered\n");
    fflush(stdout);
    abort();
}
