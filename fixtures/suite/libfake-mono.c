// libfake-mono.so: stands in for Mono's embedding library by its exported names only (the Mono switch, see
// runtime/android/mono.c): mono_jit_set_aot_mode, which picks the execution engine, and mono_jit_init_version, which
// starts the runtime (the name that tells Mono from a library that only borrows the first one). It prints the mode it
// got and reports the engine through fake_mono_report and fake_mono_compile. The numbers are Mono's MonoAotMode:
// NONE 0, NORMAL 1, HYBRID 2, FULL 3, LLVMONLY 4, INTERP 5, INTERP_LLVMONLY 6, LLVMONLY_INTERP 7, INTERP_ONLY 8.
#include <stdio.h>

static int mode_set = 1;  // NORMAL: the JIT is allowed

void mono_jit_set_aot_mode(int mode) {
    printf("mono-switch-test: mono_jit_set_aot_mode(%d)\n", mode);
    mode_set = mode;
}

void *mono_jit_init_version(const char *name, const char *version) {
    (void)name;
    (void)version;
    printf("mono-switch-test: mono_jit_init_version\n");
    return &mode_set;
}

const char *fake_mono_report(void) {
    if (mode_set >= 5 && mode_set <= 7) return "interpreter";
    if (mode_set == 3 || mode_set == 4) return "AOT only";
    return "JIT allowed";
}

// A method without AOT code: the JIT would write machine code and ask for it to be executable (mono_arch_* and
// mono_jit_compile_method, which iOS refuses), the interpreter runs it, and a full-AOT runtime has no way to run it.
const char *fake_mono_compile(void) {
    if (mode_set >= 5 && mode_set <= 7) return "the interpreter ran the method";
    if (mode_set == 3 || mode_set == 4) return "the method has no AOT code";
    return "the JIT asked for executable memory";
}
