// libminato-frame-rate.so, the FPS unlock's guest half (ADR-0066): the loader preloads it only when the host set an
// unlock (apkrt_set_frame_rate_unlock), writes the rate into minato_frame_rate_target, and resolves
// il2cpp_add_internal_call and mono_add_internal_call, by import and by dlsym alike, to the two wrappers below, with
// the function they stand for in the matching *_original. Every registration goes on unchanged but that of Unity's
// targetFrameRate setter, which is registered wrapped: a rate the app sets below the unlock, or -1 (Unity's default),
// becomes the unlock, and a higher one stays.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*Setter)(int32_t);
typedef void (*Registration)(const char *name, const void *method);

int32_t minato_frame_rate_target;
Registration minato_frame_rate_il2cpp_original;
Registration minato_frame_rate_mono_original;

// The name both runtimes register the setter by, from Unity's C# reference source.
static const char setter_name[] = "UnityEngine.Application::set_targetFrameRate(System.Int32)";
// The engine's own setter; a process runs one engine.
static Setter engine_setter;

static void raised_setter(int32_t rate) {
    const int32_t target = minato_frame_rate_target;
    engine_setter(rate <= 0 || rate < target ? target : rate);
}

// What goes on to the runtime for `name`: the method, or the setter's wrapper.
static const void *registered(const char *name, const void *method) {
    if (name == NULL || method == NULL || strcmp(name, setter_name) != 0) return method;
    engine_setter = (Setter)method;
    return (const void *)raised_setter;
}

void minato_frame_rate_il2cpp_add_internal_call(const char *name, const void *method) {
    minato_frame_rate_il2cpp_original(name, registered(name, method));
}

void minato_frame_rate_mono_add_internal_call(const char *name, const void *method) {
    minato_frame_rate_mono_original(name, registered(name, method));
}
