// libframe-cap-engine: stands in for an engine that caps its own frame rate (ADR-0066), by Unity's names only: it
// registers its frame rate setter through il2cpp_add_internal_call, which it imports, and its getter through
// mono_add_internal_call, which it looks up with dlsym, as an engine may reach its scripting runtime either way;
// swapped, the setter goes through mono_add_internal_call and the getter through il2cpp_add_internal_call. Without a
// rate it runs at 30 fps, as Unity does on Android with targetFrameRate -1.
#include <dlfcn.h>
#include <stddef.h>

typedef void (*Method)(void);
void il2cpp_add_internal_call(const char *name, Method method);

static int target = -1;

static void set_target(int rate) {
    target = rate;
}

static int get_target(void) {
    return target;
}

static const char setter[] = "UnityEngine.Application::set_targetFrameRate(System.Int32)";
static const char getter[] = "UnityEngine.Application::get_targetFrameRate()";

// Registers the two internal calls, the setter by import or, `swapped`, by dlsym; 0, or -1 when the dlsym fails.
int frame_cap_engine_start(int swapped) {
    void *api = dlopen("libframe-cap-api.so", RTLD_NOW);
    void (*add)(const char *, const void *) =
        api == NULL ? NULL : (void (*)(const char *, const void *))dlsym(api, "mono_add_internal_call");
    if (add == NULL) return -1;
    il2cpp_add_internal_call(swapped ? getter : setter, swapped ? (Method)get_target : (Method)set_target);
    add(swapped ? setter : getter, swapped ? (const void *)set_target : (const void *)get_target);
    return 0;
}

// The rate the engine runs at.
int frame_cap_engine_rate(void) {
    return target <= 0 ? 30 : target;
}
