// ns-test (ADR-0039 a, ADR-0040 decision 2): a program outside the APEXes, run as /system/bin/ns-test, opens
// each library its arguments name, in order, and prints what dlopen answers, and for a library it opened what
// that library's ns_lib_check found. On Android 15 a program in /system/bin reaches ART's libraries only
// through the links of its namespace (linkerconfig's [system] section), and a library in ART's APEX finds what
// it needs and opens in its APEX's folder (runtime.system_links, runtime/tests/ns_run.py).
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    for (int index = 1; index < argc; index++) {
        void *library = dlopen(argv[index], RTLD_NOW);
        if (library == NULL) {
            printf("dlopen(%s): %s\n", argv[index], dlerror());
            continue;
        }
        const char *(*check)(void) = (const char *(*)(void))dlsym(library, "ns_lib_check");
        printf("dlopen(%s): %s\n", argv[index], check != NULL ? check() : "no ns_lib_check");
    }
    return 0;
}
