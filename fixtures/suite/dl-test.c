// dl-test: libdl (M2 step 5c): libraries opened at run time with their dependencies, constructors
// and dynamic TLS; dlsym with a handle, RTLD_DEFAULT and TLS symbols; dladdr; dl_iterate_phdr;
// dlerror with Android's messages. Run the way Android's shell runs it: from /data/local/tmp, with
// LD_LIBRARY_PATH=/data/local/tmp so that linker64 finds the libraries there.
// Expected output: fixtures/expected/dl-test.out
#define _GNU_SOURCE  // RTLD_DEFAULT, Dl_info
#include <android/api-level.h>
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

int dl_test_exported(void) {
    return 5;
}

static const char *error_text(void) {
    const char *error = dlerror();
    return error != NULL ? error : "NULL";
}

struct Search {
    const char *name;
    int found;
    size_t tls_module;
    void *tls_data;
};

static int search(struct dl_phdr_info *info, size_t size, void *argument) {
    struct Search *search = argument;
    (void)size;
    if (info->dlpi_name != NULL && strcmp(info->dlpi_name, search->name) == 0) {
        ++search->found;
        search->tls_module = info->dlpi_tls_modid;
        search->tls_data = info->dlpi_tls_data;
    }
    return 0;
}

static int (*plugin_add)(int);

static void *add_in_thread(void *argument) {
    (void)argument;
    return (void *)(long)plugin_add(10);
}

int main(void) {
    printf("target SDK of a shell program: %d\n", android_get_application_target_sdk_version());

    void *plugin = dlopen("libdl-plugin.so", RTLD_NOW);
    printf("dlopen: %s, dlerror: %s\n", plugin != NULL ? "a handle" : "NULL", error_text());
    printf("dlopen again: %s\n", dlopen("libdl-plugin.so", RTLD_NOW) == plugin ? "the same handle" : "another");
    printf("dlopen by path: %s\n",
           dlopen("/data/local/tmp/libdl-plugin.so", RTLD_NOW) == plugin ? "the same handle" : "another");

    plugin_add = (int (*)(int))dlsym(plugin, "plugin_add");
    int (*helper_twice)(int) = (int (*)(int))dlsym(plugin, "helper_twice");
    int *plugin_value = dlsym(plugin, "plugin_value");
    printf("dlsym through the handle: plugin_add %s, helper_twice from its dependency %s, plugin_value %d\n",
           plugin_add != NULL ? "found" : "missing", helper_twice != NULL ? "found" : "missing",
           plugin_value != NULL ? *plugin_value : -1);
    const void *local = dlsym(RTLD_DEFAULT, "plugin_add");
    printf("dlsym(RTLD_DEFAULT) of a symbol of an RTLD_LOCAL library: %s, ", local == NULL ? "NULL" : "found");
    printf("dlerror: %s\n", error_text());
    printf("dlsym(RTLD_DEFAULT, \"printf\") is printf: %s\n", dlsym(RTLD_DEFAULT, "printf") == (void *)printf ? "yes" : "no");
    const void *nope = dlsym(plugin, "nope");
    printf("dlsym of a missing symbol: %s, ", nope == NULL ? "NULL" : "found");
    printf("dlerror: %s, ", error_text());
    printf("dlerror again: %s\n", error_text());

    void *program = dlopen(NULL, RTLD_NOW);
    int (*exported)(void) = (int (*)(void))dlsym(program, "dl_test_exported");
    printf("dlopen(NULL) and dlsym of the program's own symbol: %d\n", exported != NULL ? exported() : -1);

    // Dynamic TLS: no memory for this thread until the first access.
    struct Search before = {"/data/local/tmp/libdl-plugin.so", 0, 0, NULL};
    dl_iterate_phdr(search, &before);
    printf("dl_iterate_phdr: plugin found %d time(s), TLS module %s, TLS data before first use: %s\n", before.found,
           before.tls_module > 0 ? "set" : "missing", before.tls_data == NULL ? "NULL" : "allocated");
    const int first = plugin_add(4);
    const int second = plugin_add(1);
    printf("plugin_add(4): %d, plugin_add(1): %d\n", first, second);
    struct Search after = {"/data/local/tmp/libdl-plugin.so", 0, 0, NULL};
    dl_iterate_phdr(search, &after);
    int *counter = dlsym(plugin, "plugin_counter");
    printf("TLS after first use: counter %d through dlsym, dl_iterate_phdr data is that block: %s\n",
           counter != NULL ? *counter : -1, after.tls_data == (void *)counter ? "yes" : "no");
    pthread_t thread;
    void *result;
    pthread_create(&thread, NULL, add_in_thread, NULL);
    pthread_join(thread, &result);
    printf("another thread starts with its own counter: %ld, this thread still has %d\n", (long)result, *counter);

    Dl_info info;
    memset(&info, 0, sizeof info);
    const int found = dladdr((char *)plugin_add + 4, &info);
    printf("dladdr: %d, %s, %s, symbol address is plugin_add: %s, base set: %s\n", found, info.dli_fname,
           info.dli_sname, info.dli_saddr == (void *)plugin_add ? "yes" : "no", info.dli_fbase != NULL ? "yes" : "no");
    printf("dladdr outside every library: %d\n", dladdr((void *)16, &info));

    void *global = dlopen("libdl-global.so", RTLD_NOW | RTLD_GLOBAL);
    int (*global_function)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "global_function");
    printf("RTLD_GLOBAL: %s, dlsym(RTLD_DEFAULT) finds its symbol: %d\n", global != NULL ? "opened" : "failed",
           global_function != NULL ? global_function() : -1);

    // linker64 searches with open(): a search that fails somewhere leaves ENOENT in errno.
    const char *const failing[] = {"libnope.so", "libdl-broken.so", "libdl-unresolved.so"};
    for (int i = 0; i < 3; ++i) {
        errno = 0;
        void *handle = dlopen(failing[i], RTLD_NOW);
        const int error = errno;
        printf("dlopen(\"%s\"): %s, errno %d, ", failing[i], handle == NULL ? "NULL" : "a handle", error);
        printf("dlerror: %s\n", error_text());
    }
    // RTLD_NOLOAD still looks for the file: without one the library is not found.
    void *missing = dlopen("libdl-global-not-there.so", RTLD_NOW | RTLD_NOLOAD);
    printf("RTLD_NOLOAD of a library that does not exist: %s, ", missing == NULL ? "NULL" : "a handle");
    printf("dlerror: %s\n", error_text());
    void *not_loaded = dlopen("libdl-broken.so", RTLD_NOW | RTLD_NOLOAD);
    printf("RTLD_NOLOAD of a library whose dlopen failed: %s, ", not_loaded == NULL ? "NULL" : "a handle");
    printf("dlerror: %s\n", error_text());
    printf("RTLD_NOLOAD of a loaded library: %s\n",
           dlopen("libdl-helper.so", RTLD_NOW | RTLD_NOLOAD) != NULL ? "a handle" : "NULL");
    printf("dlclose: %d\n", dlclose(global));
    return 0;
}
