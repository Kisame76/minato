// jsc-switch-test: an app that starts a JavaScriptCore context the way react-native does (the JavaScriptCore switch,
// runtime/android/jsc.c): it opens libfake-jsc.so with dlopen (System.loadLibrary) and looks the names up. Arguments:
//   (none)       dlopen first, then the app reads JSC_useJIT: the library is mapped before the app's first getenv.
//   getenv-first the app reads JSC_useJIT, then dlopens: the library is mapped after it.
//   app-sets     the app sets JSC_useJIT=1 itself, then dlopens: a JIT would only crash on iOS, so the switch wins.
// Expected output: fixtures/expected/jsc-switch-test.out, as Android prints it; the switch changes the values and the
// tier.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void app_reads(void) {
    const char *value = getenv("JSC_useJIT");
    printf("jsc-switch-test: app: JSC_useJIT=%s\n", value != NULL ? value : "unset");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "getenv-first") == 0) app_reads();
    if (strcmp(mode, "app-sets") == 0) setenv("JSC_useJIT", "1", 1);
    void *library = dlopen("libfake-jsc.so", RTLD_NOW);
    if (library == NULL) {
        printf("jsc-switch-test: %s\n", dlerror());
        return 1;
    }
    void *(*create_group)(void) = (void *(*)(void))dlsym(library, "JSContextGroupCreate");
    void *(*create_context)(void *, void *) = (void *(*)(void *, void *))dlsym(library, "JSGlobalContextCreateInGroup");
    const char *(*tier)(void) = (const char *(*)(void))dlsym(library, "fake_jsc_tier");
    if (create_group == NULL || create_context == NULL || tier == NULL) {
        printf("jsc-switch-test: a name is missing\n");
        return 1;
    }
    if (strcmp(mode, "getenv-first") != 0) app_reads();
    create_context(create_group(), NULL);
    printf("jsc-switch-test: %s\n", tier());
    return 0;
}
