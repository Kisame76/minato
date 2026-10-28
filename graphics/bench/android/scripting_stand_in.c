// libbench-scripting (ADR-0066 step 4): test code of the bench APKs only. It stands in for an engine's scripting
// runtime, as libil2cpp is Unity's: the engine registers its internal calls by name with il2cpp_add_internal_call, and
// the script side finds what was registered under a name with bench_scripting_method. With an FPS unlock, Minato's
// loader hands the engine its wrapper for il2cpp_add_internal_call, so the targetFrameRate setter registered here is
// the wrapper's (runtime/android/frame_rate.c).
#include <string.h>

typedef const void *Method;

void il2cpp_add_internal_call(const char *name, Method method);
Method bench_scripting_method(const char *name);

static struct {
    char name[128];
    Method method;
} calls[8];
static int count;

void il2cpp_add_internal_call(const char *name, Method method) {
    if (count == (int)(sizeof calls / sizeof calls[0]) || strlen(name) >= sizeof calls[0].name) return;
    strcpy(calls[count].name, name);
    calls[count++].method = method;
}

Method bench_scripting_method(const char *name) {
    for (int index = 0; index < count; ++index) {
        if (strcmp(calls[index].name, name) == 0) return calls[index].method;
    }
    return NULL;
}
