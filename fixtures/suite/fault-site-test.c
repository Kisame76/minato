// fault-site-test: "null" and "x18" end with SIGSEGV at address 0x1087b in its own library, as an app whose table base
// is NULL does: byte 0x1087b through a NULL base (libfault-site.c) and through x18 = 0 (libfault-site-x18.S); on
// Android the shell answers 139. "exception" throws a C++ exception in one of its libraries and catches it in another
// (libfault-site-throw.cpp, libfault-site-catch.cpp), prints "caught" and ends with 0. runtime/tests/fault_site_run.py
// checks the runtime's fault-site line for each. Built with FAULT_SITE_MODE (fault-site-test-null, -x18 and
// -exception) it runs that mode without an argument, for runners that start a program without one (fixtures/tests.txt,
// the iPhone's fixture runner).
#include <stdio.h>
#include <string.h>

unsigned char fault_site_read(const unsigned char *base, unsigned long index);
unsigned char fault_site_read_x18(unsigned long index);
int fault_site_catch(void);

int main(int argc, char **argv) {
    volatile unsigned long index = 0x1087b;  // not folded into the load
#ifdef FAULT_SITE_MODE
    const char *mode = argc == 1 ? FAULT_SITE_MODE : "";
    (void)argv;
#else
    const char *mode = argc == 2 ? argv[1] : "";
#endif
    if (strcmp(mode, "null") != 0 && strcmp(mode, "x18") != 0 && strcmp(mode, "exception") != 0) {
        fprintf(stderr, "usage: fault-site-test null|x18|exception\n");
        return 2;
    }
    printf("reading %s\n", mode);
    fflush(stdout);
    if (strcmp(mode, "exception") == 0) {
        const int caught = fault_site_catch() == 0;
        printf("%s\n", caught ? "caught" : "not caught");
        return caught ? 0 : 1;
    }
    if (strcmp(mode, "null") == 0) return fault_site_read((const unsigned char *)0, index);
    return fault_site_read_x18(index);
}
