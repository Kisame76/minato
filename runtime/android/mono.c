// libminato-mono.so, the Mono switch's guest half (ADR-0071 decision 3): the loader preloads it when it is among the
// system's libraries and resolves mono_jit_set_aot_mode, by import and by dlsym alike, to the wrapper below wherever
// the library that defines it also defines mono_jit_init_version (Mono's embedding library: Unity's MonoBleedingEdge,
// dotnet/android's, Xamarin's), writing the function it stands for into minato_mono_original. The host picks the
// engine with this call before it starts the runtime; the wrapper hands Mono the interpreter's mode instead of one
// that lets the JIT run, so that Mono never writes machine code and asks for it to be executable, which iOS refuses.
// MonoAotMode (mono/mini/jit.h): NONE 0, NORMAL 1, HYBRID 2, FULL 3, LLVMONLY 4, INTERP 5, INTERP_LLVMONLY 6,
// LLVMONLY_INTERP 7, INTERP_ONLY 8. INTERP is "--full-aot --interpreter": no JIT, also none for the wrappers, which
// then come from the app's AOT images (the "interp" AOT option); INTERP_ONLY would still JIT its wrappers. So the JIT
// modes (NONE, NORMAL, HYBRID, INTERP_ONLY) and FULL become INTERP, LLVMONLY becomes INTERP_LLVMONLY, and the
// interpreter modes 5 to 7 stay as the host chose them. A mode above them (the sentinel's range) is passed through.
// An app built without AOT images still emits its interp-to-native trampolines at run time (ADR-0071's named limit).
// A library that defines the mode name without mono_jit_init_version is not Mono and is left alone.
enum {
    MONO_AOT_MODE_NONE = 0,
    MONO_AOT_MODE_FULL = 3,
    MONO_AOT_MODE_LLVMONLY = 4,
    MONO_AOT_MODE_INTERP = 5,
    MONO_AOT_MODE_INTERP_LLVMONLY = 6,
    MONO_AOT_MODE_LLVMONLY_INTERP = 7,
    MONO_AOT_MODE_INTERP_ONLY = 8,
};

typedef void (*SetAotMode)(int mode);

SetAotMode minato_mono_original;

void minato_mono_set_aot_mode(int mode) {
    if (mode == MONO_AOT_MODE_LLVMONLY) {
        mode = MONO_AOT_MODE_INTERP_LLVMONLY;
    } else if (mode >= MONO_AOT_MODE_NONE && mode <= MONO_AOT_MODE_FULL) {
        mode = MONO_AOT_MODE_INTERP;
    } else if (mode == MONO_AOT_MODE_INTERP_ONLY) {
        mode = MONO_AOT_MODE_INTERP;
    }
    minato_mono_original(mode);
}
