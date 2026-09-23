// Compiles the app's header as plain C, the way Swift reads it, and calls it.
#include "APKRuntime/apkrt.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int condition, const char *what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

int main(void) {
    const char *version = apkrt_version();
    if (strcmp(version, APKRT_EXPECTED_VERSION) != 0) {
        fprintf(stderr, "apkrt_version() is \"%s\", expected \"%s\"\n", version, APKRT_EXPECTED_VERSION);
        return 1;
    }
    printf("ok   apkrt_version() = %s\n", version);

    char error[256] = "";
    expect(apkrt_mount("/data/local/tmp", "/tmp", error, sizeof error) == 0, "apkrt_mount of a host directory");
    error[0] = '\0';
    expect(apkrt_mount("data/local/tmp", "/tmp", error, sizeof error) == -1 && strstr(error, "absolute") != NULL,
           "apkrt_mount refuses a relative guest path");
    error[0] = '\0';
    expect(apkrt_mount("/data", "/nonexistent/apkrt", error, sizeof error) == -1 &&
               strstr(error, "/nonexistent/apkrt") != NULL,
           "apkrt_mount refuses a host path that does not exist");
    expect(apkrt_set_working_directory("/data/local/tmp", error, sizeof error) == 0, "apkrt_set_working_directory");
    error[0] = '\0';
    expect(apkrt_set_working_directory("tmp", error, sizeof error) == -1 && strstr(error, "absolute") != NULL,
           "apkrt_set_working_directory refuses a relative path");
    expect(apkrt_add_library_directory("/system/lib64", "/tmp", error, sizeof error) == 0,
           "apkrt_add_library_directory of a host directory");
    error[0] = '\0';
    expect(apkrt_add_library_directory("system/lib64", "/tmp", error, sizeof error) == -1 &&
               strstr(error, "absolute") != NULL,
           "apkrt_add_library_directory refuses a relative guest path");
    error[0] = '\0';
    expect(apkrt_add_library_directory("/system/lib64", "/nonexistent/apkrt", error, sizeof error) == -1 &&
               strstr(error, "/nonexistent/apkrt") != NULL,
           "apkrt_add_library_directory refuses a host directory that does not exist");
    return failures == 0 ? 0 : 1;
}
