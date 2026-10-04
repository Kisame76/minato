// dl-own-test (ADR-0040 test 2, ADR-0033 e1): a library opened with dlopen that defines a name libc.so also has
// (libdl-own.so's getenv) uses its own definition for its own references, and the program, whose references were
// bound at its start, keeps libc.so's.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    void *library = dlopen("libdl-own.so", RTLD_NOW);
    if (library == NULL) {
        printf("dlopen(libdl-own.so): %s\n", dlerror());
        return 1;
    }
    const char *(*check)(void) = (const char *(*)(void))dlsym(library, "dl_own_check");
    printf("dlopen(libdl-own.so): its call reaches %s\n", check != NULL ? check() : "nothing");
    printf("the program's call reaches %s\n", getenv("DL_OWN_MARK") != NULL ? "libdl-own.so's getenv" :
                                                                               "libc.so's getenv");
    return 0;
}
