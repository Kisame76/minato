// libfake-jsc.so: stands in for JavaScriptCore (react-native's jsc-android) by its exported names only (the
// JavaScriptCore switch, see runtime/android/jsc.c): JSEvaluateScript, JSContextGroupCreate, JSGlobalContextCreate and
// JSGlobalContextCreateInGroup. It reads its options the way JSC does, from the environment, twice: in a constructor
// (some builds read them in a static initializer) and at the first context (JSC::initialize, a std::call_once), and
// reports what it would do: JSC_useJIT=0 or JavaScriptCoreUseJIT=0 starts the interpreter (enableAssembler,
// Options.cpp, WebKit); otherwise the JIT reserves its code pool and mprotects it executable. No JavaScript: a context
// is a pointer.
#include <stdio.h>
#include <stdlib.h>

static const char *option(const char *name) {
    const char *value = getenv(name);
    return value != NULL ? value : "unset";
}

static void report(const char *when) {
    printf("jsc-switch-test: %s: JSC_useJIT=%s JavaScriptCoreUseJIT=%s\n", when, option("JSC_useJIT"),
           option("JavaScriptCoreUseJIT"));
}

__attribute__((constructor)) static void read_in_constructor(void) {
    report("constructor");
}

static int initialized;
static int jit_on;

static void initialize(void) {
    if (initialized) return;
    initialized = 1;
    report("first context");
    const char *a = getenv("JSC_useJIT");
    const char *b = getenv("JavaScriptCoreUseJIT");
    jit_on = !((a != NULL && atoi(a) == 0) || (b != NULL && atoi(b) == 0));
}

void *JSContextGroupCreate(void) {
    initialize();
    return malloc(1);
}

void *JSGlobalContextCreateInGroup(void *group, void *global_object_class) {
    (void)group;
    (void)global_object_class;
    initialize();
    return malloc(1);
}

void *JSGlobalContextCreate(void *global_object_class) {
    (void)global_object_class;
    initialize();
    return malloc(1);
}

int JSEvaluateScript(void *context) {
    (void)context;
    return 1;
}

const char *fake_jsc_tier(void) {
    return jit_on ? "the JIT reserved a code pool and asked for it executable" : "the interpreter runs, no code pool";
}
