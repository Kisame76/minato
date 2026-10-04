// ns-test (ADR-0039 a, ADR-0040 decision 2): a program outside the APEXes, run as /system/bin/ns-test, opens
// each library its arguments name, in order, and prints what dlopen answers, and for a library it opened what
// that library's ns_lib_check found. On Android 15 a program in /system/bin reaches ART's libraries only
// through the links of its namespace (linkerconfig's [system] section), and a library in ART's APEX finds what
// it needs and opens in its APEX's folder (runtime.system_links, runtime/tests/ns_run.py).
// An argument <namespace>:<library> opens the library with android_dlopen_ext in that exported namespace, as
// libnativeloader opens the JNI library of a boot jar in an APEX, conscrypt.jar's libjavacrypto.so for one, in
// the APEX's namespace (FindApexNamespace, ADR-0046 a).
// An argument @<folder>:<library> creates a namespace on <folder> as libnativeloader creates a class loader's,
// isolated with the app's library folder as its search path, and opens the library in it with android_dlopen_ext,
// by its full path, as ART opens an app's JNI library (ADR-0033 e0).
#include <android/dlext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// libdl_android.so's function `symbol`, which the NDK leaves out.
static void *dl_android_function(const char *symbol) {
    void *dl_android = dlopen("libdl_android.so", RTLD_NOW);
    return dl_android != NULL ? dlsym(dl_android, symbol) : NULL;
}

// The exported namespace `name`, through android_get_exported_namespace.
static struct android_namespace_t *exported_namespace(const char *name) {
    struct android_namespace_t *(*get)(const char *) =
        (struct android_namespace_t *(*)(const char *))dl_android_function("android_get_exported_namespace");
    return get != NULL ? get(name) : NULL;
}

// A class loader's namespace on `folder`, as libnativeloader's CreateClassLoaderNamespace asks for one: no
// LD_LIBRARY_PATH part, the folder as the default library path and as the permitted path, isolated.
static struct android_namespace_t *class_loader_namespace(const char *folder) {
    struct android_namespace_t *(*create)(const char *, const char *, const char *, uint64_t, const char *,
                                          struct android_namespace_t *) =
        (struct android_namespace_t *(*)(const char *, const char *, const char *, uint64_t, const char *,
                                         struct android_namespace_t *))dl_android_function("android_create_namespace");
    return create != NULL ? create("clns-ns-test", NULL, folder, 1 /* ANDROID_NAMESPACE_TYPE_ISOLATED */, folder, NULL)
                          : NULL;
}

int main(int argc, char **argv) {
    for (int index = 1; index < argc; index++) {
        char shown[512];
        void *library;
        const char *colon = strchr(argv[index], ':');
        if (colon == NULL) {
            snprintf(shown, sizeof shown, "dlopen(%s)", argv[index]);
            library = dlopen(argv[index], RTLD_NOW);
        } else {
            char name[256];
            snprintf(name, sizeof name, "%.*s", (int)(colon - argv[index]), argv[index]);
            const int created = name[0] == '@';
            struct android_namespace_t *space = created ? class_loader_namespace(name + 1) : exported_namespace(name);
            if (space == NULL) {
                printf("%s(%s): null\n", created ? "android_create_namespace" : "android_get_exported_namespace",
                       created ? name + 1 : name);
                continue;
            }
            snprintf(shown, sizeof shown, "android_dlopen_ext(%s, %s)", colon + 1, name);
            const android_dlextinfo info = {.flags = ANDROID_DLEXT_USE_NAMESPACE, .library_namespace = space};
            library = android_dlopen_ext(colon + 1, RTLD_NOW, &info);
        }
        if (library == NULL) {
            printf("%s: %s\n", shown, dlerror());
            continue;
        }
        const char *(*check)(void) = (const char *(*)(void))dlsym(library, "ns_lib_check");
        printf("%s: %s\n", shown, check != NULL ? check() : "no ns_lib_check");
    }
    return 0;
}
