// libfake-notmono.so: a library that defines mono_jit_set_aot_mode but is not Mono (it lacks mono_jit_init_version):
// the Mono switch leaves such a definition alone.
#include <stdio.h>

static int mode_set = 1;

void mono_jit_set_aot_mode(int mode) {
    printf("mono-switch-test: mono_jit_set_aot_mode(%d)\n", mode);
    mode_set = mode;
}

const char *fake_mono_report(void) {
    return "not Mono";
}

const char *fake_mono_compile(void) {
    return mode_set == 1 ? "no engine" : "no engine, other mode";
}
