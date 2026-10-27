// libframe-cap-api: stands in for a scripting runtime's API by its names only (IL2CPP's il2cpp_add_internal_call and
// Mono's mono_add_internal_call, ADR-0066): it keeps the internal calls an engine registers, by name, for
// frame-cap-test, which calls them as an app's scripts would.
#include <stddef.h>
#include <string.h>

typedef void (*Method)(void);

static struct {
    const char *name;
    Method method;
} calls[8];
static int count;

static void add(const char *name, Method method) {
    if (count < (int)(sizeof calls / sizeof calls[0])) {
        calls[count].name = name;
        calls[count].method = method;
        ++count;
    }
}

void il2cpp_add_internal_call(const char *name, Method method) {
    add(name, method);
}

void mono_add_internal_call(const char *name, const void *method) {
    add(name, (Method)method);
}

// The internal call registered under `name`, or NULL.
Method frame_cap_internal_call(const char *name) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(calls[i].name, name) == 0) return calls[i].method;
    }
    return NULL;
}
