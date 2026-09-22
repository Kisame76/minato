// Compiles the app's header as plain C, the way Swift reads it, and calls it.
#include "APKRuntime/apkrt.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const char *version = apkrt_version();
    if (strcmp(version, APKRT_EXPECTED_VERSION) != 0) {
        fprintf(stderr, "apkrt_version() is \"%s\", expected \"%s\"\n", version, APKRT_EXPECTED_VERSION);
        return 1;
    }
    printf("ok   apkrt_version() = %s\n", version);
    return 0;
}
