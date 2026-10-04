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
// by its full path, as ART opens an app's JNI library (ADR-0033 e0). Its permitted path is /data:/mnt/expand, as
// libnativeloader gives a class loader's namespace that it creates for System.load, and below target SDK 24 it has
// the exempt list (ADR-0033 e2b1). With @@<folder> the namespace is also the anonymous one, as libnativeloader asks for
// the namespace of an app's main class loader (ADR-0033 e2b2). It is linked to the APEXes' namespaces that
// /linkerconfig/apex.libraries.config.txt names, for their public libraries, as libnativeloader links it (ADR-0033
// e2b3).
// An argument sdk=<version> sets the app's target SDK version, as ART does for an app, through
// android_set_application_target_sdk_version. An argument +<library> opens the library with RTLD_GLOBAL.
#include <android/dlext.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

// An APEX's namespace and its public libraries, separated by colons, as a "public" line of
// /linkerconfig/apex.libraries.config.txt gives them.
struct apex_libraries {
    char space[64];
    char libraries[512];
};
#define MAX_APEXES 8

// The "public" lines of /linkerconfig/apex.libraries.config.txt, as libnativeloader reads them
// (apex_public_libraries): how many it wrote to `apexes`, none without the file, which is optional.
static int apex_public_libraries(struct apex_libraries apexes[MAX_APEXES]) {
    FILE *file = fopen("/linkerconfig/apex.libraries.config.txt", "r");
    if (file == NULL) return 0;
    int count = 0;
    char line[1024], tag[16];
    while (count < MAX_APEXES && fgets(line, sizeof line, file) != NULL) {
        struct apex_libraries *apex = &apexes[count];
        if (sscanf(line, "%15s %63s %511s", tag, apex->space, apex->libraries) == 3 && strcmp(tag, "public") == 0) {
            count++;
        }
    }
    fclose(file);
    return count;
}

// Whether `name` is one of the libraries of `list`, separated by colons.
static bool listed(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *item = list;; item++) {
        const char *end = strchr(item, ':');
        const size_t item_length = end != NULL ? (size_t)(end - item) : strlen(item);
        if (item_length == length && strncmp(item, name, length) == 0) return true;
        if (end == NULL) return false;
        item = end;
    }
}

// The public libraries of /system/etc/public.libraries.txt, separated by colons, as libnativeloader links a
// class loader's namespace to the system's for them: without those an APEX's namespace serves
// (InitDefaultPublicLibraries).
static void public_libraries(char *list, size_t size, const struct apex_libraries *apexes, int apex_count) {
    list[0] = '\0';
    FILE *file = fopen("/system/etc/public.libraries.txt", "r");
    if (file == NULL) return;
    char line[256];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, " \r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        bool from_apex = false;
        for (int index = 0; index < apex_count && !from_apex; index++) {
            from_apex = listed(apexes[index].libraries, line);
        }
        if (from_apex) continue;
        if (list[0] != '\0') strncat(list, ":", size - strlen(list) - 1);
        strncat(list, line, size - strlen(list) - 1);
    }
    fclose(file);
}

// The app's target SDK version, as libnativeloader reads it.
static int target_sdk_version(void) {
    int (*get)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "android_get_application_target_sdk_version");
    return get != NULL ? get() : 0;
}

// A class loader's namespace on `folder`, as libnativeloader's CreateClassLoaderNamespace asks for one when
// System.load comes from a class loader no namespace was made for: no LD_LIBRARY_PATH part, the folder as the default
// library path, /data:/mnt/expand as the permitted path (kAlwaysPermittedDirectories), isolated, the exempt list
// below target SDK 24, and linked to the exported "default" namespace for the public libraries and to each APEX's
// namespace of /linkerconfig/apex.libraries.config.txt for its own. With `anonymous` it is also the anonymous
// namespace (ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS), as for an app's main class loader.
static struct android_namespace_t *class_loader_namespace(const char *folder, bool anonymous) {
    struct android_namespace_t *(*create)(const char *, const char *, const char *, uint64_t, const char *,
                                          struct android_namespace_t *) =
        (struct android_namespace_t *(*)(const char *, const char *, const char *, uint64_t, const char *,
                                         struct android_namespace_t *))dl_android_function("android_create_namespace");
    bool (*link)(struct android_namespace_t *, struct android_namespace_t *, const char *) =
        (bool (*)(struct android_namespace_t *, struct android_namespace_t *, const char *))dl_android_function(
            "android_link_namespaces");
    if (create == NULL || link == NULL) return NULL;
    uint64_t type = 1;  // ANDROID_NAMESPACE_TYPE_ISOLATED
    if (target_sdk_version() < 24) type |= 0x08000000;  // ANDROID_NAMESPACE_TYPE_EXEMPT_LIST_ENABLED
    if (anonymous) type |= 0x10000000;                   // ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS
    struct android_namespace_t *space = create("clns-ns-test", NULL, folder, type, "/data:/mnt/expand", NULL);
    struct apex_libraries apexes[MAX_APEXES];
    const int apex_count = apex_public_libraries(apexes);
    char libraries[2048];
    public_libraries(libraries, sizeof libraries, apexes, apex_count);
    if (space == NULL || libraries[0] == '\0' || !link(space, exported_namespace("default"), libraries)) return NULL;
    for (int index = 0; index < apex_count; index++) {
        struct android_namespace_t *apex = exported_namespace(apexes[index].space);
        if (apex != NULL && !link(space, apex, apexes[index].libraries)) return NULL;
    }
    return space;
}

int main(int argc, char **argv) {
    for (int index = 1; index < argc; index++) {
        char shown[512];
        void *library;
        if (strncmp(argv[index], "sdk=", 4) == 0) {
            const int version = atoi(argv[index] + 4);
            void (*set)(int) = (void (*)(int))dl_android_function("android_set_application_target_sdk_version");
            if (set != NULL) set(version);
            printf("android_set_application_target_sdk_version(%d)%s\n", version, set != NULL ? "" : ": null");
            continue;
        }
        const char *colon = strchr(argv[index], ':');
        if (argv[index][0] == '+') {
            snprintf(shown, sizeof shown, "dlopen(%s, RTLD_GLOBAL)", argv[index] + 1);
            library = dlopen(argv[index] + 1, RTLD_NOW | RTLD_GLOBAL);
        } else if (colon == NULL) {
            snprintf(shown, sizeof shown, "dlopen(%s)", argv[index]);
            library = dlopen(argv[index], RTLD_NOW);
        } else {
            char name[256];
            snprintf(name, sizeof name, "%.*s", (int)(colon - argv[index]), argv[index]);
            const int created = name[0] == '@';
            const int anonymous = created && name[1] == '@';
            const char *folder = name + (anonymous ? 2 : 1);
            struct android_namespace_t *space =
                created ? class_loader_namespace(folder, anonymous) : exported_namespace(name);
            if (space == NULL && created) {
                const char *why = dlerror();
                printf("android_create_namespace(%s): null: %s\n", folder, why != NULL ? why : "no dlerror");
                continue;
            }
            if (space == NULL) {
                printf("android_get_exported_namespace(%s): null\n", name);
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
