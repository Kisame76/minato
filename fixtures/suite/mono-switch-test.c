// mono-switch-test: an app that starts Mono the way a .NET host does (the Mono switch, runtime/android/mono.c): it
// picks the execution engine with mono_jit_set_aot_mode(<mode>), starts the runtime, then asks which engine runs a
// method.
// Without an argument the library is linked in and its names are imported; "dlsym-mono" and "dlsym-notmono" open
// libfake-mono.so or libfake-notmono.so and look the names up. The first argument is a MonoAotMode (default 1,
// NORMAL). Expected output: fixtures/expected/mono-switch-test.out (the default, as Android prints it).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mono_jit_set_aot_mode(int mode);
void *mono_jit_init_version(const char *name, const char *version);
const char *fake_mono_report(void);
const char *fake_mono_compile(void);

int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 1;
    void (*set_mode)(int) = mono_jit_set_aot_mode;
    void *(*init)(const char *, const char *) = mono_jit_init_version;
    const char *(*report)(void) = fake_mono_report;
    const char *(*compile)(void) = fake_mono_compile;
    if (argc > 2) {
        void *library = dlopen(strcmp(argv[2], "dlsym-notmono") == 0 ? "libfake-notmono.so" : "libfake-mono.so",
                               RTLD_NOW);
        if (library == NULL) {
            printf("mono-switch-test: %s\n", dlerror());
            return 1;
        }
        set_mode = (void (*)(int))dlsym(library, "mono_jit_set_aot_mode");
        init = (void *(*)(const char *, const char *))dlsym(library, "mono_jit_init_version");
        report = (const char *(*)(void))dlsym(library, "fake_mono_report");
        compile = (const char *(*)(void))dlsym(library, "fake_mono_compile");
        if (set_mode == NULL || report == NULL || compile == NULL) {
            printf("mono-switch-test: a name is missing\n");
            return 1;
        }
    }
    set_mode(mode);
    if (init != NULL) init("minato", "v4.0.30319");
    printf("mono-switch-test: %s\n", report());
    printf("mono-switch-test: %s\n", compile());
    return 0;
}
