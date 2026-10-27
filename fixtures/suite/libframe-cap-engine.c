// libframe-cap-engine: stands in for an engine that caps its own frame rate (ADR-0066), by Unity's names only: it
// registers its frame rate setter through il2cpp_add_internal_call, which it imports, and its getter through
// mono_add_internal_call, which it looks up with dlsym, as an engine may reach its scripting runtime either way.
// Without a rate it runs at 30 fps, as Unity does on Android with targetFrameRate -1.
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

// Registers the two internal calls; 0, or -1 when the dlsym fails.
int frame_cap_engine_start(void) {
    il2cpp_add_internal_call("UnityEngine.Application::set_targetFrameRate(System.Int32)", (Method)set_target);
    void *api = dlopen("libframe-cap-api.so", RTLD_NOW);
    void (*add)(const char *, const void *) =
        api == NULL ? NULL : (void (*)(const char *, const void *))dlsym(api, "mono_add_internal_call");
    if (add == NULL) return -1;
    add("UnityEngine.Application::get_targetFrameRate()", (const void *)get_target);
    return 0;
}

// The rate the engine runs at.
int frame_cap_engine_rate(void) {
    return target <= 0 ? 30 : target;
}
