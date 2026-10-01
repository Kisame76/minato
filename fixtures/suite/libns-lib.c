// libns-lib.so for ns-test: it needs libns-dep.so and opens libns-plugin.so, which both lie only in its own
// folder, an APEX's lib64.
#include <dlfcn.h>
#include <stdio.h>

int ns_dep_value(void);

const char *ns_lib_check(void) {
    static char text[256];
    void *plugin = dlopen("libns-plugin.so", RTLD_NOW);
    if (plugin == NULL) {
        snprintf(text, sizeof text, "dep %d, plugin: %s", ns_dep_value(), dlerror());
        return text;
    }
    int (*value)(void) = (int (*)(void))dlsym(plugin, "ns_plugin_value");
    snprintf(text, sizeof text, "dep %d, plugin %d", ns_dep_value(), value != NULL ? value() : -1);
    return text;
}
